/**
 * power.c — power policy.
 *
 * Purpose: implement the power policy: pseudo-off sleep entry/exit (radios,
 * panel, clocks), battery sampling + the filtered gauge, the auto-anchored
 * calibration, the time-left estimate, and the low-battery policy.
 *
 * ─── Where the milliamps go (and what this file does about each) ───────────
 * AWAKE   everything runs at 48 MHz off PLL_USB with PLL_SYS powered down
 *         (power_boot_begin) instead of 150 MHz. Both cores WFI whenever idle
 *         (freertos_hooks.c). The battery sampler talks to the radio chip once
 *         every 10 s instead of twice a second, and restarts Greg if the chip
 *         stops answering.
 * NAP     the LIGHT nap: radios + panel off, everything else as awake.
 *         The DEEP nap below is off by default until fully bench-verified
 *         (power.h). With -DDEEP_NAP=ON:
 *         on battery the RP2350's switched core is POWERED OFF via POWMAN
 *         (power_deep_nap_enter): CYW43 held off, accelerometer powered down,
 *         VSYS divider off, panel in deep sleep with the sleep still retained
 *         for free. The always-on POWMAN timer keeps the clock; CENTER (a
 *         level-low POWMAN wake) or the periodic USB peek powers it back up,
 *         and the firmware simply boots — the day's steps, the mood and the
 *         last-sync time come back from the POWMAN scratch registers.
 *         On USB the nap stays "light" (radios + panel off only) so serial,
 *         picotool and the charge timer keep working.
 * See power.h for the policy knobs and the reasoning behind the two depths.
 */
#include "power.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "hardware/clocks.h"    /* set_sys_clock_48mhz — the awake operating point */
#include "hardware/vreg.h"      /* vreg_set_voltage — keep the core rail at its default */
#include "hardware/sync.h"      /* save_and_disable_interrupts, __wfi */
#include "hardware/irq.h"       /* irq_set_enabled — silence every IRQ before the power-down */
#include "hardware/watchdog.h"  /* watchdog_reboot — if the power-down is refused */
#if !PICO_RP2040
#include "hardware/powman.h"    /* P-state power-down, wake sources, AON timer */
#endif

#include "FreeRTOS.h"
#include "task.h"

#include "accel.h"
#include "battery.h"
#include "bt.h"
#include "display.h"            /* display_park_cold — panel state for the nap */
#include "joystick.h"           /* JOY_CENTER (the wake pin), INPUT_* for KEY */
#include "motion.h"             /* the day's step / activity tallies */
#include "rtc_clock.h"          /* clock_epoch_s, clock_aon_save */
#include "rtos_tasks.h"
#include "storage.h"
#include "wifi.h"

bool g_power_sleep = false;
volatile bool g_power_deep_pending = false;
power_boot_t  g_boot_kind = BOOT_COLD;
nap_resume_t  g_nap_resume = { .valid = false, .day = -1, .mood = -1, .pct = -1 };

/* USB presence as the nap policy needs it: has the sampler looked yet, and
 * when was USB last seen (boot counts, so a fresh boot never deep-naps before
 * the anchor window has had its chance). */
static volatile bool     g_usb_known   = false;
static volatile uint32_t g_last_usb_ms = 0;
static volatile bool     g_usb_seen    = false;   /* USB present at any point this boot */

/* Bench controls (power_debug_cmd). */
static volatile bool     g_nap_request = false;
static volatile bool     g_force_deep  = false;
static bool              g_deep_refused = false;  /* this boot follows a refused power-down */
static volatile uint32_t g_peek_s      = DEEP_NAP_PEEK_S;

/* While the radios restart after a wake they hold the cyw43 lock for long
 * stretches; the battery task must NOT queue behind it (a lock-taking read
 * there used to stall accel sampling — field bug). Battery reads pause until
 * this deadline; cached values serve. */
static volatile uint32_t g_radio_settle_until = 0;

volatile int   g_batt_pct        = -1;
volatile float g_batt_v          = 0.0f;
volatile float g_batt_raw_v      = 0.0f;
volatile bool  g_on_usb          = false;
volatile float g_batt_hours_left = -1.0f;
volatile bool  g_batt_hours_est  = true;
volatile bool  g_batt_low        = false;
volatile bool  g_batt_critical   = false;

/* Lightest-load raw sample of the last closed window (0 = none yet) — what a
 * manual anchor should use, not whatever instantaneous read came last. */
static volatile float g_last_peak_raw = 0.0f;

/* Pre-sleep radio state, restored on wake. */
static bool g_slept_bt   = false;         /* HCI was on at sleep entry */
static bool g_slept_wifi = false;         /* WiFi was connected at sleep entry */
static char g_slept_ssid[33] = "";

/* ─── Time-left estimator ────────────────────────────────────────────────────
 * The resting-voltage curve is flat in the middle, so instantaneous %-rates
 * are noise. We only take a rate sample when the gauge has moved ≥ 4 points
 * AND ≥ 10 min have passed since the last anchor, then EMA the %/hour. Until
 * a measured rate exists, a nominal draw gives an "(EST)" figure. */
static int      rate_anchor_pct = -1;
static uint32_t rate_anchor_ms  = 0;
static float    rate_pct_per_h  = 0.0f;   /* 0 = unknown */

static void rate_reset(void) {
    rate_anchor_pct = -1; rate_anchor_ms = 0; rate_pct_per_h = 0.0f;
}

static void rate_update(int pct, uint32_t now) {
    if (rate_anchor_pct < 0) { rate_anchor_pct = pct; rate_anchor_ms = now; return; }
    int      dpct = rate_anchor_pct - pct;
    uint32_t dt   = now - rate_anchor_ms;
    if (dpct >= 4 && dt >= 10u * 60u * 1000u) {
        float r = (float)dpct * 3600000.0f / (float)dt;        /* %/hour */
        rate_pct_per_h = (rate_pct_per_h > 0.0f) ? rate_pct_per_h + 0.5f * (r - rate_pct_per_h) : r;
        rate_anchor_pct = pct; rate_anchor_ms = now;
        printf("[BATT] discharge rate %.1f %%/h\n", (double)rate_pct_per_h);
    } else if (dpct < 0) {
        rate_anchor_pct = pct; rate_anchor_ms = now;           /* gauge rose (relaxation) */
    }
}

static void hours_left_update(int pct) {
    if (pct < 0) { g_batt_hours_left = -1.0f; return; }
    if (rate_pct_per_h > 0.05f) {
        g_batt_hours_left = (float)pct / rate_pct_per_h;
        g_batt_hours_est  = false;
    } else {
        g_batt_hours_left = (float)BATT_CAPACITY_MAH * (float)pct / 100.0f / (float)BATT_NOMINAL_AVG_MA;
        g_batt_hours_est  = true;
    }
}

/* ─── Low-battery policy ─────────────────────────────────────────────────────
 * LOW: stop the BLE proximity scan (the biggest continuous radio load) and let
 * wifi.c's policy keep WiFi off; the user's social setting is restored once the
 * cell recovers or USB appears.
 * CRITICAL: the app forces STATE_SLEEP with a "CHARGE ME" still. The TP4056 has
 * no load cut-off and the Pico's buck-boost runs happily down to ~1.8 V, so the
 * firmware is the only thing keeping the cell out of deep-discharge territory. */
static void low_policy_update(int pct, float v, bool usb) {
    if (usb) {
        if (g_batt_low) {
            g_batt_low = false;
            if (g_saved.social_on && !g_power_sleep) {
                wetgreg_social_set_self(g_saved.wetgreg_id); wetgreg_social_enable(true);
            }
        }
        g_batt_critical = false;
        return;
    }
    if (!g_batt_low && pct >= 0 && pct <= BATT_LOW_PCT) {
        g_batt_low = true;
        printf("[BATT] LOW (%d%%): social scan suspended\n", pct);
        wetgreg_social_enable(false);
    } else if (g_batt_low && pct >= BATT_LOW_CLEAR_PCT) {
        g_batt_low = false;
        printf("[BATT] recovered (%d%%)\n", pct);
        if (g_saved.social_on && !g_power_sleep) {
            wetgreg_social_set_self(g_saved.wetgreg_id); wetgreg_social_enable(true);
        }
    }
    if (!g_batt_critical && v > 0.0f && v <= BATT_CRIT_V) {
        g_batt_critical = true;
        printf("[BATT] CRITICAL %.2f V: forcing sleep\n", (double)v);
    } else if (g_batt_critical && v >= BATT_CRIT_CLEAR_V) {
        g_batt_critical = false;
    }
}

/* ─── Battery + USB sampling — runs in its OWN task, NOT the accel loop ──────
 * read_vsys_raw_volts() grabs the cyw43 lock (GP29 is the shared CYW43 SPI
 * clock), so it lives on the Battery task
 * where a stall only delays the gauge, never auto-rotate. Values land in
 * g_batt_* (volatile, single-writer) that HK folds into the snapshot.
 *
 * ── PEAK-HOLD over a window, then EMA-smoothed ──
 * Load only ever sags VSYS below the true resting voltage (and makes the D3
 * drop bigger), so the MAX across a window of spaced samples ≈ the lightest-
 * load reading, i.e. the one the calibrated offset is defined for. Awake we
 * sample every 10 s and fold the 40 s window peak into an EMA; in the light
 * nap every 5 s with a 40 s window (nothing moves fast there, and each read
 * briefly holds the cyw43 lock and wakes the radio's bus). */
/* Minutes on USB needed before an unplug may anchor: charge from the level the
 * cell had when USB appeared (unknown → assume empty) to the CV phase at the
 * programmed current, +15 % for taper/efficiency, +1 h of CV tail. */
static uint32_t anchor_needed_min(int pct_at_plug) {
    int from = (pct_at_plug >= 0 && pct_at_plug <= 100) ? pct_at_plug : 0;
    int to   = 85;
    float mah = (float)BATT_CAPACITY_MAH * (float)(to > from ? to - from : 0) / 100.0f;
    float min = mah / (float)BATT_CHARGER_MA * 60.0f * 1.15f + 60.0f;
    if (min < (float)BATT_AUTO_ANCHOR_USB_MIN) min = (float)BATT_AUTO_ANCHOR_USB_MIN;
    return (uint32_t)min;
}

/* A cell that is being discharged does not gain charge, so on battery the
 * shown percentage may only FALL. The voltage does wander upward — load drops
 * (a radio burst ends, the panel stops refreshing) and the cell relaxes — and
 * on the flat middle of the LiPo curve 20 mV of that is 10 points, which is
 * what made the old gauge climb back up after dipping. A rise is accepted only
 * when it is large and has held for a long run of windows (a genuine
 * correction: the first loaded sample after boot, or a new anchor). `reseed`
 * restarts it (boot, USB unplug, new calibration). */
#define GAUGE_RISE_MIN_PCT   4
#define GAUGE_RISE_WINDOWS   8
static int gauge_monotonic(int pct, bool reseed) {
    static int shown = -1, rise_run = 0;
    static uint16_t offset_seen = 0;
    if (offset_seen != g_batt_offset_mv) {       /* new calibration: every reading moved */
        offset_seen = g_batt_offset_mv; reseed = true;
    }
    if (reseed || shown < 0 || pct < 0) { shown = pct; rise_run = 0; return shown; }
    if (pct <= shown) { shown = pct; rise_run = 0; }
    else if (pct >= shown + GAUGE_RISE_MIN_PCT) {
        if (++rise_run >= GAUGE_RISE_WINDOWS) { shown = pct; rise_run = 0; }
    } else rise_run = 0;
    return shown;
}

/* ─── Radio health ───────────────────────────────────────────────────────────
 * The CYW43 can stop answering (seen in the field after ~a day up). Every
 * battery sample starts with one ioctl to the chip (battery.c), so its result
 * doubles as the health check: RADIO_PROBE_FAILS failed samples in a row raise
 * a restart request that the UI task turns into a warm restart — state saved,
 * no splash — which re-initialises the chip from scratch. A stalled ioctl
 * busy-waits for a second, so a hung chip costs 1 s in 10 until the restart,
 * instead of the old 1 s in every 1.1 s for ever. */
#define RADIO_PROBE_FAILS  3
static volatile bool g_restart_request = false;

static void radio_health_check(void) {
    static int logged = 0;
    int fails = battery_radio_fail_count();
    if (fails == 0) { logged = 0; return; }
    if (fails != logged) {
        logged = fails;
        printf("[CYW43] radio did not answer (%d/%d)\n", fails, RADIO_PROBE_FAILS);
    }
    if (fails >= RADIO_PROBE_FAILS && !g_restart_request && !g_power_sleep && g_boot_kind != BOOT_WARM) {
        /* One automatic restart per cold boot: if the chip is still dead after
         * it, it is hardware, and a restart loop would only eat the battery. */
        printf("[CYW43] radio unresponsive — requesting a warm restart\n");
        g_restart_request = true;
    }
}

/* Every sample talks to the radio chip, and the driver then keeps its bus
 * awake ~2.5 s. At the old 2 Hz the chip therefore never slept at all; one
 * sample every 10 s leaves it asleep three quarters of the time. The cost is
 * that plugging / unplugging USB takes up to 10 s to show. */
#define BATT_SAMPLE_MS_AWAKE 10000u
#define BATT_SAMPLE_MS_SLEEP 30000u      /* napping: nothing changes fast, let the radio chip sleep */
#define BATT_WINDOW_MS_AWAKE 40000u
#define BATT_WINDOW_MS_SLEEP 120000u

void batt_sample(void) {
    static uint32_t batt_win_start = 0, batt_last_ms = 0;
    static float    batt_win_peak  = 0.0f;
    static float    batt_v_ema     = 0.0f;
    static int      was_usb        = -1;
    static uint32_t usb_since_ms   = 0;      /* when USB was last seen appearing */
    static int      pct_at_plug    = -1;     /* on-battery % just before USB appeared */
    static bool     anchor_armed   = false;  /* unplug after a long charge → anchor next window */
    static float    last_bv        = 0.0f;   /* last on-battery reading, retained for diag */
    static int      last_bp        = -1;
    uint32_t now = to_ms_since_boot(get_absolute_time());

    if (g_power_deep_pending) return;         /* core is about to lose power: hands off */
    uint32_t period = !g_usb_known   ? 250u      /* first verdict fast: a peek boot is waiting on it */
                    : g_power_sleep  ? BATT_SAMPLE_MS_SLEEP : BATT_SAMPLE_MS_AWAKE;
    uint32_t window = g_power_sleep ? BATT_WINDOW_MS_SLEEP : BATT_WINDOW_MS_AWAKE;
    if (now - batt_last_ms < period || (int32_t)(now - g_radio_settle_until) < 0) return;
    batt_last_ms = now;

    /* ONE lock-taking ADC read per sample; USB comes from the same number. */
    BOOT_TRACE("batt: read");
    float raw = read_vsys_raw_volts();
    BOOT_TRACE("batt: read done");
    if (raw < BATT_RAW_VALID_MIN_V) {
        radio_health_check();
        /* Not a voltage this board can be running from: a failed read. Never
         * let one flip the USB state or trip the critical-battery nap. */
        static uint32_t bad_log_ms = 0;
        if (now - bad_log_ms >= 30000) {
            bad_log_ms = now;
            printf("[BATT] invalid VSYS read %.3f V — ignored\n", (double)raw);
        }
        return;
    }
    bool  usb = battery_usb_from_raw(raw);
    g_on_usb = usb;
    radio_health_check();
    if (usb) { g_last_usb_ms = now; g_usb_seen = true; }
    g_usb_known = true;

    if ((was_usb != 1) && usb) {
        usb_since_ms = now ? now : 1;
        /* Unknown on a boot straight into USB — unless this is the quiet boot
         * out of a deep-nap peek, where the level at lights-out came with us. */
        pct_at_plug  = (was_usb == 0) ? last_bp
                     : (g_boot_kind == BOOT_NAP_PEEK && g_nap_resume.valid) ? g_nap_resume.pct : -1;
        rate_reset();                         /* charging — discharge history is moot */
        batt_win_peak = 0.0f; batt_win_start = 0; batt_v_ema = 0.0f;
        printf("[BATT] USB in (cell was %d%%): anchor after >= %lu min on USB\n",
               pct_at_plug, (unsigned long)anchor_needed_min(pct_at_plug));
    } else if ((was_usb != 0) && !usb) {
        /* Unplug edge. If USB was present long enough for the TP4056 to be in
         * its 4.20 V CV phase, the first clean window after this reads the cell
         * at (almost exactly) the float voltage — a free per-board anchor. */
        uint32_t on_usb_min = usb_since_ms ? (now - usb_since_ms) / 60000u : 0;
        anchor_armed = (was_usb == 1) && on_usb_min >= anchor_needed_min(pct_at_plug);
        if (was_usb == 1)
            printf("[BATT] USB unplugged after %lu min%s\n", (unsigned long)on_usb_min,
                   anchor_armed ? " — auto-anchor armed" : " (too short to anchor)");
        batt_win_peak = 0.0f; batt_win_start = 0; batt_v_ema = 0.0f;
        rate_reset();
    }

    if (usb) {
        /* On USB the rail isn't the battery; show live rail volts, flag -1. */
        g_batt_raw_v = raw;
        g_batt_pct   = -1;
        g_batt_v     = raw;
        hours_left_update(-1);
        low_policy_update(-1, 0.0f, true);
        /* DIAGNOSTIC: serial needs USB, but USB hides the battery. Print the
         * LAST on-battery reading every ~10 s so it's easy to capture: run on
         * battery a few seconds, replug USB, read this line. */
        static uint32_t usb_log_ms = 0;
        if (now - usb_log_ms >= 10000) {
            usb_log_ms = now;
            printf("[BATT] USB rail=%.3f V | last on-battery=%.3f V %d%% | offset %u mV (%c)\n",
                   (double)raw, (double)last_bv, last_bp,
                   (unsigned)g_batt_offset_mv, g_batt_offset_src);
        }
    } else {
        g_batt_raw_v = raw;
        if (raw > batt_win_peak) batt_win_peak = raw;        /* lightest-load sample */
        if (batt_win_start == 0) batt_win_start = now ? now : 1;
        float v = battery_cell_from_raw(raw);
        if (batt_v_ema <= 0.0f) {                            /* seed immediately on unplug/boot */
            batt_v_ema = v; g_batt_v = v; g_batt_pct = gauge_monotonic(lipo_percent_hyst(v), true);
        }
        if (now - batt_win_start >= window) {
            float peak_raw = batt_win_peak;
            if (anchor_armed) {
                /* Slew-limited so one odd unplug can't throw the gauge far; a
                 * run of good charges converges within 2–3 cycles. */
                int   want = (int)((BATT_ANCHOR_TARGET_V - peak_raw) * 1000.0f + 0.5f);
                int   cur  = (int)g_batt_offset_mv;
                int   step = want - cur;
                if (step >  BATT_AUTO_ANCHOR_MAX_STEP_MV) step =  BATT_AUTO_ANCHOR_MAX_STEP_MV;
                if (step < -BATT_AUTO_ANCHOR_MAX_STEP_MV) step = -BATT_AUTO_ANCHOR_MAX_STEP_MV;
                printf("[BATT] auto-anchor: raw peak %.3f V wants %d mV (have %d, step %d)\n",
                       (double)peak_raw, want, cur, step);
                if (step != 0 && want >= BATT_OFFSET_MIN_MV && want <= BATT_OFFSET_MAX_MV)
                    battery_offset_persist((uint16_t)(cur + step), 'A');
                anchor_armed = false;
                batt_v_ema = 0.0f;                             /* re-seed with the new offset */
            }
            g_last_peak_raw = peak_raw;
            float peak = battery_cell_from_raw(peak_raw);
            if (batt_v_ema <= 0.0f) batt_v_ema = peak;
            batt_v_ema += 0.35f * (peak - batt_v_ema);          /* ~5-window settle */
            g_batt_v    = batt_v_ema;
            g_batt_pct  = gauge_monotonic(lipo_percent_hyst(batt_v_ema), false);
            batt_win_peak  = 0.0f;
            batt_win_start = 0;
            rate_update(g_batt_pct, now);
            hours_left_update(g_batt_pct);
            low_policy_update(g_batt_pct, batt_v_ema, false);
            static uint32_t log_ms = 0;
            static bool     log_first = true;
            if (log_first || now - log_ms >= 30000) {
                log_first = false;
                log_ms = now;
                printf("[BATT] raw %.3f V + %u mV → %.3f V = %d%%  left %.1f h%s%s%s\n",
                       (double)peak_raw, (unsigned)g_batt_offset_mv, (double)batt_v_ema,
                       g_batt_pct, (double)g_batt_hours_left, g_batt_hours_est ? " (est)" : "",
                       g_batt_low ? " LOW" : "", g_batt_critical ? " CRIT" : "");
            }
        }
        last_bv = g_batt_v; last_bp = g_batt_pct;
    }
    was_usb = usb ? 1 : 0;
}

bool battery_anchor_full_now(void) {
    if (g_on_usb || g_batt_raw_v < 2.0f) return false;
    float raw = g_batt_raw_v;
    if (g_last_peak_raw > raw) raw = g_last_peak_raw;   /* lightest-load reading wins */
    battery_anchor_to(raw, 'M');
    return true;
}

/* ─── Awake clock ─────────────────────────────────────────────────────────── */
static void awake_clock_apply(void) {
    /* The core rail stays at the 1.10 V default — and is put BACK there first,
     * in case an earlier build left it low. The regulator setting lives in the
     * always-on domain and SURVIVES a warm reset (watchdog, picotool reboot,
     * reflash), while the C runtime and the boot ROM always start at 150 MHz.
     * A build that dropped the rail to 1.00 V for the 48 MHz clock therefore
     * ran its next warm boot at 150 MHz on 1.00 V: it froze in the splash and
     * could not even enumerate in BOOTSEL (bench, 2026-11-16). The ~15 % of
     * core power the lower rail would save is not worth that. */
    vreg_set_voltage(VREG_VOLTAGE_DEFAULT);
#if AWAKE_CLK_48MHZ
    /* Everything initialised after this (SPI, I2C, the cyw43 PIO bus,
     * FreeRTOS's SysTick reload) sizes itself from the clock it finds, which
     * is why this runs first thing in main(). */
    set_sys_clock_48mhz();                    /* clk_sys + clk_peri 48 MHz off PLL_USB, PLL_SYS off */
#endif
}

/* ─── Deep nap: POWMAN P-state power-down ─────────────────────────────────── */
#if DEEP_NAP && !PICO_RP2040
#define HAVE_DEEP_NAP 1
#else
#define HAVE_DEEP_NAP 0
#endif

#if HAVE_DEEP_NAP

#ifdef CYW43_DEFAULT_PIN_WL_REG_ON
#define NAP_PIN_WL_REG_ON CYW43_DEFAULT_PIN_WL_REG_ON
#else
#define NAP_PIN_WL_REG_ON 23u
#endif
#define NAP_PIN_VSYS_EN   25u               /* high = VSYS/3 divider connected (also WL_CS) */

/* POWMAN scratch layout (always-on domain: survives the power-down, cleared by
 * a real power-on). [6] and [7] belong to the SDK's pico_low_power, which is
 * linked for its boot-time fixes and reads them on every power-down wake. */
#define NAP_MAGIC  0x47524731u              /* "GRG1" — deep nap; bump if the layout changes */
#define REFUSED_MAGIC 0x47525231u           /* "GRR1" - power-down was refused: quiet boot into the light nap */
#define DOWN_MAGIC 0x47524431u              /* "GRD1" — reboot in progress: power down at boot */
#define WARM_MAGIC 0x47525731u              /* "GRW1" — warm restart (radio recovery), same layout */
enum { NAP_S_MAGIC = 0, NAP_S_STEPS, NAP_S_ACTIVE, NAP_S_PACKED, NAP_S_START, NAP_S_SYNC };
/* NAP_S_PACKED: [7:0] day, [15:8] mood+1, [23:16] pct+1, [31:24] USB-peek period in 10 s units. */

/* POWMAN_LAST_SWCORE_PWRUP is one bit per source. */
#define NAP_WHY_GPIO0 (1u << 1)             /* pwrup0 — the CENTER wake */
#define NAP_WHY_ALARM (1u << 6)             /* the peek alarm */

static void nap_state_save(int mood, uint32_t magic) {
    uint32_t now_s = clock_epoch_s();
    uint32_t age   = wifi_last_sync_age_s();
    int      pct   = g_batt_pct;            /* -1 while on USB */
    if (g_boot_kind == BOOT_NAP_PEEK && g_nap_resume.valid && !g_usb_seen) pct = g_nap_resume.pct;
    if (mood < -1 || mood > 126) mood = -1;
    if (pct  < -1 || pct  > 100) pct  = -1;
    powman_hw->scratch[NAP_S_STEPS]  = steps_today;
    powman_hw->scratch[NAP_S_ACTIVE] = active_seconds_today;
    powman_hw->scratch[NAP_S_PACKED] = (uint32_t)(uint8_t)motion_activity_day()
                                     | ((uint32_t)(uint8_t)(mood + 1) << 8)
                                     | ((uint32_t)(uint8_t)(pct + 1)  << 16)
                                     | ((uint32_t)(g_peek_s >= 2550u ? 255u : (g_peek_s + 9u) / 10u) << 24);
    /* Going back down after a peek is the SAME nap: keep when it began and
     * what the cell read then, so the wake log reports the whole stretch. */
    bool same_nap = g_boot_kind == BOOT_NAP_PEEK && g_nap_resume.valid && !g_usb_seen;
    if (same_nap && g_nap_resume.nap_start_s) now_s = g_nap_resume.nap_start_s;
    powman_hw->scratch[NAP_S_START]  = now_s;
    powman_hw->scratch[NAP_S_SYNC]   = (age == UINT32_MAX || age > now_s) ? 0u : now_s - age;
    powman_hw->scratch[NAP_S_MAGIC]  = magic;            /* last: the record is complete */
}

static void nap_state_load(void) {
    uint32_t p = powman_hw->scratch[NAP_S_PACKED];
    g_nap_resume.valid          = true;
    g_nap_resume.steps_today    = powman_hw->scratch[NAP_S_STEPS];
    g_nap_resume.active_seconds = powman_hw->scratch[NAP_S_ACTIVE];
    g_nap_resume.day            = (int8_t)(p & 0xFF);
    g_nap_resume.mood           = (int8_t)((int)((p >> 8) & 0xFF) - 1);
    g_nap_resume.pct            = (int8_t)((int)((p >> 16) & 0xFF) - 1);
    g_nap_resume.nap_start_s    = powman_hw->scratch[NAP_S_START];
    g_nap_resume.wifi_sync_s    = powman_hw->scratch[NAP_S_SYNC];
}

/* POWMAN registers only take a write that carries the password in the top
 * half (the SDK keeps its own helper for this private). */
static inline void nap_powman_write(volatile uint32_t *reg, uint32_t value) {
    *reg = POWMAN_PASSWORD_BITS | (value & 0xffffu);
}

/* Arm the wake sources and cut the switched core's power.
 *
 * MUST run bare-metal — single core, no scheduler, no USB, straight after
 * reset (nap_boot_classify calls it for a DOWN_MAGIC boot). POWMAN only powers
 * the core down once the processors are actually asleep in WFI, and WFI
 * returns at once for ANY enabled interrupt that is pending, masked or not.
 * Called from inside the running firmware (first attempt) there is always one
 * — SysTick, the timer alarm pool, USB, the other core's scheduler — so the
 * core never slept, the power never went, and Greg sat with interrupts off,
 * deaf to CENTER and to USB (seen on the bench). After a reset none of that
 * exists yet; this is the same context the SDK's pico_low_power is used in,
 * and the sequence mirrors its low_power_go_pstate().
 *
 * One more requirement, found the hard way: on the RP2350 a core that has come
 * up through a watchdog / ROM reboot does not enter sleep properly until it
 * has been through one dummy interrupt-and-WFI cycle. The SDK carries that
 * workaround as runtime_init_rp2350_sleep_fix inside pico_low_power — which is
 * why that library is linked (CMakeLists.txt) even though we call none of its
 * API: it runs the fix before main() on exactly the boot this path uses.
 * Without it the power-down request was accepted and then never happened.
 *
 * Does not return. If POWMAN explicitly refuses (e.g. CENTER is down right
 * now, so a wake is already pending) the request is withdrawn and Greg
 * reboots into an ordinary cold boot. */
static void __attribute__((noreturn)) deep_power_down(void) {
    (void)save_and_disable_interrupts();
    for (uint i = 0; i < NUM_IRQS; i++) irq_set_enabled(i, false);   /* nothing may end the WFI */

    /* A debugger's power-up request is deliberately NOT ignored: with a probe
     * on the SWD pads, attaching wakes a napping Greg — the only way back in
     * when no other wake source works (and harmless when no probe is fitted). */
    powman_set_debug_power_request_ignored(false);
    powman_timer_set_1khz_tick_source_lposc();        /* XOSC is about to stop; LPOSC (factory-cal) keeps time */
    if (!powman_timer_is_running()) powman_timer_start();
    hw_set_bits(&powman_hw->vreg_ctrl, POWMAN_PASSWORD_BITS | POWMAN_VREG_CTRL_UNLOCK_BITS);

    powman_disable_all_wakeups();
    powman_enable_gpio_wakeup(0, JOY_CENTER, false /* level */, false /* low = pressed */);
    uint32_t peek_s = (powman_hw->scratch[NAP_S_PACKED] >> 24) * 10u;
    if (peek_s == 0) peek_s = DEEP_NAP_PEEK_S;
    powman_enable_alarm_wakeup_at_ms(powman_timer_get_ms() + (uint64_t)peek_s * 1000u);

    powman_power_state off = POWMAN_POWER_STATE_NONE; /* no SRAM kept: state is in scratch + flash */
    if (powman_configure_wakeup_state(off, powman_get_power_state())) {
        powman_hw->boot[0] = 0; powman_hw->boot[1] = 0;   /* wake = a normal boot into main() */
        powman_hw->boot[2] = 0; powman_hw->boot[3] = 0;
        powman_hw->scratch[6] = powman_hw->timer;         /* readback of what was armed, for the */
        powman_hw->scratch[7] = powman_hw->pwrup[0];      /* wake snapshot (bench diagnostics) */

        /* Request the power-down ourselves rather than through the SDK's
         * powman_set_power_state(). That helper polls for the "waiting for
         * the processors to sleep" flag for only ~500 loop iterations — a few
         * microseconds — and the power manager's clock is far slower, so it
         * returned a TIMEOUT error while the request was in fact still
         * pending. The old code took that as a refusal, disarmed the wake
         * sources and cleared the nap record... and then the chip powered down
         * anyway, with nothing left that could wake it (bench, 2026-11-16:
         * no CENTER, no alarm, only a debugger brought it back). So: wait
         * properly, and only an explicit refusal counts as one. */
        powman_clear_bits(&powman_hw->state, POWMAN_STATE_REQ_IGNORED_BITS);
        nap_powman_write(&powman_hw->state, (~off << POWMAN_STATE_REQ_LSB) & POWMAN_STATE_REQ_BITS);
        bool refused = false;
        for (int i = 0; i < 2000; i++) {                  /* up to ~200 ms */
            uint32_t st = powman_hw->state;
            if (st & (POWMAN_STATE_REQ_IGNORED_BITS | POWMAN_STATE_BAD_SW_REQ_BITS)) { refused = true; break; }
            if (st & (POWMAN_STATE_WAITING_BITS | POWMAN_STATE_CHANGING_BITS)) break;   /* accepted */
            busy_wait_us(100);
        }
        if (!refused)
            for (;;) __wfi();                             /* power goes away in here */

        /* Refused (a wake condition is already true — CENTER held, or a
         * debugger asking for power): withdraw the request before anything
         * else, or it would still fire the next time the core idles. */
        nap_powman_write(&powman_hw->state, (~powman_get_power_state() << POWMAN_STATE_REQ_LSB) & POWMAN_STATE_REQ_BITS);
    }
    /* Come back up QUIETLY into the light nap and leave the deep nap alone for
     * one peek period. (This used to be a plain cold boot - and a Greg with a
     * debug probe still asking for power then chimed, redrew, dozed off, was
     * refused and rebooted again every few minutes, all night: 128 boots,
     * bench 2026-11-17.) */
    powman_disable_all_wakeups();
    powman_hw->scratch[NAP_S_MAGIC] = REFUSED_MAGIC;
    watchdog_reboot(0, 0, 0);
    for (;;) { }
}

/* A DOWN_MAGIC boot: power_deep_nap_enter() saved the nap record, loaded the
 * always-on timer and reset the chip so that we arrive here with nothing
 * running. The reset also returned every pad to its default, so put the few
 * that matter where they must sit for the nap, then pull the plug. */
static void __attribute__((noreturn)) nap_power_down_from_reset(void) {
    gpio_init(NAP_PIN_WL_REG_ON);             /* radio chip held off */
    gpio_set_dir(NAP_PIN_WL_REG_ON, GPIO_OUT);
    gpio_put(NAP_PIN_WL_REG_ON, 0);
    gpio_init(NAP_PIN_VSYS_EN);               /* VSYS/3 divider off: 300 kOhm across the cell otherwise */
    gpio_set_dir(NAP_PIN_VSYS_EN, GPIO_OUT);
    gpio_put(NAP_PIN_VSYS_EN, 0);
    display_park_cold();                      /* panel: image kept, controller in deep sleep */

    /* CENTER is a LEVEL wake: a finger still on the stick (the last tap of the
     * 5-tap) would wake Greg the instant he dozed off. Wait for a clean release. */
    gpio_init(JOY_CENTER);
    gpio_pull_up(JOY_CENTER);
    for (int i = 0, up = 0; i < 300 && up < 15; i++) {
        up = gpio_get(JOY_CENTER) ? up + 1 : 0;
        busy_wait_ms(10);
    }

    powman_hw->scratch[NAP_S_MAGIC] = NAP_MAGIC;      /* from here a wake is a nap wake */
    deep_power_down();
}

/* ─── Wake snapshot ──────────────────────────────────────────────────────────
 * Overrides pico_low_power's weak boot check (which runs before main() and
 * would wipe the evidence): on a power-down wake, copy the POWMAN state as the
 * wake left it, THEN do that check's housekeeping. power_boot_report() prints
 * it — this is how a wake that came from the wrong source (or a timer that
 * never ran) gets diagnosed. */
enum { SNAP_CHIP_RESET, SNAP_LAST_PWRUP, SNAP_STATE, SNAP_TIMER, SNAP_PWRUP0, SNAP_NOW_LO, SNAP_NOW_HI,
       SNAP_ALARM_LO, SNAP_ALARM_HI, SNAP_INTR, SNAP_VREG, SNAP_SEQ_CFG, SNAP_CUR_PWRUP,
       SNAP_PRE_TIMER, SNAP_PRE_PWRUP0, SNAP_N };
static uint32_t g_wake_snap[SNAP_N];
static bool     g_wake_snap_valid = false;

void runtime_init_low_power_reboot_check(void) {
    if (!(powman_hw->chip_reset & POWMAN_CHIP_RESET_HAD_SWCORE_PD_BITS)) return;
    uint64_t now = powman_timer_get_ms();
    g_wake_snap[SNAP_CHIP_RESET] = powman_hw->chip_reset;
    g_wake_snap[SNAP_LAST_PWRUP] = powman_hw->last_swcore_pwrup;
    g_wake_snap[SNAP_STATE]      = powman_hw->state;
    g_wake_snap[SNAP_TIMER]      = powman_hw->timer;
    g_wake_snap[SNAP_PWRUP0]     = powman_hw->pwrup[0];
    g_wake_snap[SNAP_NOW_LO]     = (uint32_t)now;
    g_wake_snap[SNAP_NOW_HI]     = (uint32_t)(now >> 32);
    g_wake_snap[SNAP_ALARM_LO]   = (powman_hw->alarm_time_31to16 << 16) | (powman_hw->alarm_time_15to0 & 0xffff);
    g_wake_snap[SNAP_ALARM_HI]   = (powman_hw->alarm_time_63to48 << 16) | (powman_hw->alarm_time_47to32 & 0xffff);
    g_wake_snap[SNAP_INTR]       = powman_hw->intr;
    g_wake_snap[SNAP_VREG]       = powman_hw->vreg;
    g_wake_snap[SNAP_SEQ_CFG]    = powman_hw->seq_cfg;
    g_wake_snap[SNAP_CUR_PWRUP]  = powman_hw->current_pwrup_req;
    g_wake_snap[SNAP_PRE_TIMER]  = powman_hw->scratch[6];   /* as armed, just before the power-down */
    g_wake_snap[SNAP_PRE_PWRUP0] = powman_hw->scratch[7];
    g_wake_snap_valid = true;

    powman_disable_all_wakeups();
    powman_hw->scratch[6] = 0;
    powman_hw->scratch[7] = 0;
    powman_timer_set_1khz_tick_source_xosc();
}

static void nap_boot_classify(void) {
    if (powman_hw->scratch[NAP_S_MAGIC] == DOWN_MAGIC) {  /* nap entry, second half */
        vreg_set_voltage(VREG_VOLTAGE_DEFAULT);           /* never nap with a lowered rail latched */
        nap_power_down_from_reset();                      /* never returns */
    }
    if (powman_hw->scratch[NAP_S_MAGIC] == WARM_MAGIC) {  /* our own warm restart */
        g_boot_kind = BOOT_WARM;
        nap_state_load();
        powman_hw->scratch[NAP_S_MAGIC] = 0;
        return;
    }
    if (powman_hw->scratch[NAP_S_MAGIC] == REFUSED_MAGIC) {   /* power-down refused: light nap instead */
        g_boot_kind    = BOOT_NAP_PEEK;                       /* quiet: no chime, panel keeps the sleep still */
        g_deep_refused = true;
        nap_state_load();
        powman_hw->scratch[NAP_S_MAGIC] = 0;
        powman_timer_set_1khz_tick_source_xosc();
        return;
    }
    bool mine = powman_hw->scratch[NAP_S_MAGIC] == NAP_MAGIC;
    bool pd   = (powman_hw->chip_reset & POWMAN_CHIP_RESET_HAD_SWCORE_PD_BITS) != 0;
    if (!mine) return;                                    /* ordinary boot */
    if (!pd) { powman_hw->scratch[NAP_S_MAGIC] = 0; return; }   /* stale record: power was lost */

    uint32_t why = powman_hw->last_swcore_pwrup;
    powman_disable_all_wakeups();
    /* The pad kept its pull-up latched through the power-down; re-own it (which
     * also releases the pad isolation) and see whether CENTER is still down. */
    gpio_init(JOY_CENTER);
    gpio_pull_up(JOY_CENTER);
    busy_wait_us(500);
    bool pressed = !gpio_get(JOY_CENTER);
    /* Only a clean "alarm, and nobody is touching the stick" is a peek —
     * anything doubtful is treated as the user asking Greg to wake up. */
    bool peek = (why & NAP_WHY_ALARM) && !(why & NAP_WHY_GPIO0) && !pressed;

    /* A peek cannot simply read VSYS here: with the radio chip powered down
     * (or merely asleep) its clock pad drags the divider node to ground, so
     * the ADC reads ~0 whatever is feeding the board (measured). The chip has
     * to be up, and under the FreeRTOS cyw43 arch that means the scheduler has
     * to be running — so a peek is a QUIET BOOT: no chime, panel untouched,
     * no radios started, straight into the light nap. The first battery sample
     * then decides: on battery → back down at once (power_nap_should_deepen);
     * on USB → stay in the light nap and track the charge. About 2 s awake. */
    g_boot_kind = peek ? BOOT_NAP_PEEK : BOOT_NAP_WAKE;
    nap_state_load();
    powman_hw->scratch[NAP_S_MAGIC] = 0;      /* consumed — a reset from here on is an ordinary boot */
    powman_timer_set_1khz_tick_source_xosc(); /* awake again: the crystal keeps better time */
}
#endif /* HAVE_DEEP_NAP */

void power_boot_begin(void) {
#if HAVE_DEEP_NAP
    nap_boot_classify();
#endif
    awake_clock_apply();
}

void power_boot_report(void) {
    static const char *const kind[] = { "cold", "deep-nap wake (CENTER)", "deep-nap USB peek (quiet)",
                                        "warm restart (radio recovery)" };
    printf("[PWR] boot: %s | clk_sys %lu MHz | deep nap %s, peek every %lu s\n",
           kind[g_boot_kind], (unsigned long)(clock_get_hz(clk_sys) / 1000000u),
           HAVE_DEEP_NAP ? "on" : "off", (unsigned long)DEEP_NAP_PEEK_S);
#if HAVE_DEEP_NAP
    if (g_deep_refused)
        printf("[NAP] power-down REFUSED (debugger asking for power, or CENTER held): light nap, retry in %lu s\n",
               (unsigned long)DEEP_NAP_PEEK_S);
    if (g_wake_snap_valid && !g_deep_refused) {
        const uint32_t *w = g_wake_snap;
        printf("[NAP] wake snapshot: chip_reset=%08lx last_pwrup=%02lx cur_pwrup=%02lx state=%08lx timer=%08lx pwrup0=%08lx\n",
               (unsigned long)w[SNAP_CHIP_RESET], (unsigned long)w[SNAP_LAST_PWRUP], (unsigned long)w[SNAP_CUR_PWRUP],
               (unsigned long)w[SNAP_STATE], (unsigned long)w[SNAP_TIMER], (unsigned long)w[SNAP_PWRUP0]);
        printf("[NAP] wake snapshot: now=%08lx%08lx alarm=%08lx%08lx intr=%08lx vreg=%08lx seq_cfg=%08lx\n",
               (unsigned long)w[SNAP_NOW_HI], (unsigned long)w[SNAP_NOW_LO], (unsigned long)w[SNAP_ALARM_HI],
               (unsigned long)w[SNAP_ALARM_LO], (unsigned long)w[SNAP_INTR], (unsigned long)w[SNAP_VREG],
               (unsigned long)w[SNAP_SEQ_CFG]);
        printf("[NAP] armed before power-down: timer=%08lx pwrup0=%08lx\n",
               (unsigned long)w[SNAP_PRE_TIMER], (unsigned long)w[SNAP_PRE_PWRUP0]);
    }
#endif
    if (g_nap_resume.valid && g_boot_kind != BOOT_WARM) {
        uint32_t now_s = clock_epoch_s();
        uint32_t nap_s = (g_nap_resume.nap_start_s && now_s > g_nap_resume.nap_start_s)
                             ? now_s - g_nap_resume.nap_start_s : 0;
        printf("[NAP] slept %luh%02lum | cell was %d%% at lights-out | steps %lu\n",
               (unsigned long)(nap_s / 3600u), (unsigned long)((nap_s / 60u) % 60u),
               (int)g_nap_resume.pct, (unsigned long)g_nap_resume.steps_today);
    }
}

void power_deep_nap_enter(int mood) {
#if HAVE_DEEP_NAP
    printf("[NAP] deep: switched core off until CENTER (USB peek every %lu s)\n",
           (unsigned long)g_peek_s);
    /* Quiesce: the Battery and Housekeeping tasks see the flag and stop using
     * the ADC, the I2C bus and — crucially — the flash. Give any transaction
     * already in flight time to finish before the reset. */
    g_power_deep_pending = true;
    vTaskDelay(pdMS_TO_TICKS(400));
    mpu_power_down();                         /* accelerometer: ~20 uA down to ~0.5 uA (survives the reset) */

    /* The power-down itself cannot be done from in here (see deep_power_down):
     * leave the nap record and the wall clock in the always-on domain, then
     * reset. The first lines of the next boot finish the job with nothing
     * else running. The reset drops WL_REG_ON, so the radio chip goes off
     * with it. */
    nap_state_save(mood, DOWN_MAGIC);
    clock_aon_save();                         /* POWMAN timer := wall clock, running */
    stdio_flush();
    watchdog_reboot(0, 0, 0);
    for (;;) { }
#else
    (void)mood;
    watchdog_reboot(0, 0, 0);                 /* not reachable: no deep nap in this build */
    for (;;) { }
#endif
}

/* ─── Warm restart (radio recovery) ───────────────────────────────────────── */
bool power_take_restart_request(void) {
    if (!g_restart_request) return false;
    g_restart_request = false;
    return true;
}

void power_warm_restart(int mood) {
#if HAVE_DEEP_NAP
    /* Same carry-over record as the deep nap, different magic: the boot loads
     * the day's steps + mood and goes straight back to the octopus. The clock
     * survives on its own (RAM checkpoint — a watchdog reset keeps SRAM). */
    g_power_deep_pending = true;              /* no flash write mid-reset */
    vTaskDelay(pdMS_TO_TICKS(400));
    nap_state_save(mood, WARM_MAGIC);
#else
    (void)mood;
#endif
    stdio_flush();
    watchdog_reboot(0, 0, 0);
    for (;;) { }
}

/* ─── Nap entry / exit ────────────────────────────────────────────────────── */
void power_sleep_enter(int mood) {
    printf("[SLEEP] nap: radios down, panel asleep\n");

    g_slept_bt = wetgreg_bt_active();
    wetgreg_social_enable(false);
    if (g_slept_bt) wetgreg_bt_stop();        /* advertising off + HCI power off */
    /* An in-flight (or still-deferred) background rejoin counts as "wifi was
     * on" — otherwise a nap taken during the rejoin window would silently
     * forget the network (g_slept_ssid already holds the right name then). */
    g_slept_wifi = wifi_connected || wifi_rejoin_pending();
    if (wifi_connected)
        snprintf(g_slept_ssid, sizeof(g_slept_ssid), "%s", wifi_ssid_display);
    if (wifi_enabled || wifi_connected) wifi_disconnect();
    wifi_rejoin_cancel();     /* cancel any rejoin in flight or still deferred */

    /* Let the sleep still reach the glass, then deep-sleep the panel. The
     * render queue is FIFO, so the command lands strictly after the blit. */
    rtos_display_cmd(DISP_CMD_SLEEP);
    while (ui_display_busy()) vTaskDelay(pdMS_TO_TICKS(50));
    vTaskDelay(pdMS_TO_TICKS(150));           /* let the sleep cmd itself land */

    g_power_sleep = true;    /* HK: no BLE re-arm on a USB unplug edge mid-sleep */

#if HAVE_DEEP_NAP
    /* On battery → the deep nap, which never returns. Fresh off USB (< 45 s) we
     * take the light nap first so the gauge can close its auto-anchor window;
     * power_nap_should_deepen() sends Greg down ~90 s after the unplug. */
    uint32_t now = to_ms_since_boot(get_absolute_time());
    if (g_force_deep || (g_usb_known && !g_on_usb && now - g_last_usb_ms >= 45000u))
        power_deep_nap_enter(mood);
#else
    (void)mood;
#endif
}

void power_sleep_exit(void) {
    g_power_sleep = false;

    /* Panel back: hardware reset + init + full clear (clean base, no ghost of
     * the sleep still). Queued now, executes before the next rendered frame. */
    rtos_display_cmd(DISP_CMD_WAKE);

    /* Radios back the way they were — but STAGGERED: BT/social power on now,
     * the WiFi join fires ~4 s later from the octopus loop (wifi_rejoin_tick)
     * so the combined radio burst cannot hog the cyw43 lock while the user is
     * interacting. The battery task also pauses its lock-taking reads until
     * the settle deadline so accel sampling (auto-rotate) never stalls. */
    uint32_t now = to_ms_since_boot(get_absolute_time());
    g_radio_settle_until = now + 8000;
    if (g_slept_bt) wetgreg_bt_init();
    if (g_saved.social_on && !g_batt_low) {
        wetgreg_social_set_self(g_saved.wetgreg_id);
        wetgreg_social_enable(true);
    }
    if (g_slept_wifi && !g_batt_low)
        wifi_schedule_rejoin(g_slept_ssid, 4000);
    printf("[SLEEP] awake: radios restoring\n");
}

void power_nap_adopt(bool social_on) {
    /* Quiet boot out of a deep-nap peek: nothing radio-side was started and
     * the panel was never initialised (it still shows the sleep still), which
     * is exactly the state power_sleep_enter() leaves behind. */
    g_slept_bt    = social_on;
    g_slept_wifi  = false;
    g_power_sleep = true;
    printf("[NAP] peek: checking for USB\n");
}

bool power_nap_should_deepen(void) {
#if HAVE_DEEP_NAP
    if (g_force_deep) return true;
    if (!g_usb_known || g_on_usb) return false;
    uint32_t up_ms = to_ms_since_boot(get_absolute_time());
    if (g_deep_refused) return up_ms >= DEEP_NAP_PEEK_S * 1000u;   /* refused: try again one peek period on */
    /* A peek that found no USB: nothing to wait for, straight back down. */
    if (g_boot_kind == BOOT_NAP_PEEK && !g_usb_seen) return true;
    /* Unplugged during a light nap: give the gauge its post-unplug window. */
    uint32_t now = to_ms_since_boot(get_absolute_time());
    return now - g_last_usb_ms >= NAP_DEEPEN_AFTER_UNPLUG_MS;
#else
    return false;
#endif
}

uint32_t power_nap_poll_ms(void) {
    return g_usb_known ? 5000u : 100u;        /* a peek boot polls fast until the verdict is in */
}

/* ─── Serial bench commands ───────────────────────────────────────────────── */
bool power_take_nap_request(void) {
    if (!g_nap_request) return false;
    g_nap_request = false;
    return true;
}

bool power_debug_cmd(const char *line) {
    if (strcmp(line, "PWR?") == 0) {
        printf("PWR clk_sys=%lu clk_peri=%lu boot=%d nap=%d usb=%d(%s) batt=%d%% %.3fV raw=%.3fV "
               "off=%umV(%c) low=%d crit=%d left=%.1fh%s\n",
               (unsigned long)clock_get_hz(clk_sys), (unsigned long)clock_get_hz(clk_peri),
               (int)g_boot_kind, (int)g_power_sleep, (int)g_on_usb, g_usb_known ? "known" : "unsampled",
               g_batt_pct, (double)g_batt_v, (double)g_batt_raw_v,
               (unsigned)g_batt_offset_mv, g_batt_offset_src, (int)g_batt_low, (int)g_batt_critical,
               (double)g_batt_hours_left, g_batt_hours_est ? "(est)" : "");
        return true;
    }
    if (strncmp(line, "KEY ", 4) == 0) {
        uint8_t code = INPUT_NONE;
        switch (line[4]) {
            case 'U': code = INPUT_UP;     break;
            case 'D': code = INPUT_DOWN;   break;
            case 'L': code = INPUT_LEFT;   break;
            case 'R': code = INPUT_RIGHT;  break;
            case 'C': code = INPUT_CENTER; break;
            default: break;
        }
        if (code != INPUT_NONE) { rtos_inject_input(code); printf("KEY OK %c\n", line[4]); }
        else printf("KEY ERR (want: KEY U|D|L|R|C)\n");
        return true;
    }
    if (strncmp(line, "NAP DEEP", 8) == 0) {
        int s = atoi(line + 8);
        if (s >= 5) g_peek_s = (uint32_t)s;
        g_force_deep  = true;
        g_nap_request = true;
        printf("NAP OK deep (forced), peek in %lu s\n", (unsigned long)g_peek_s);
        return true;
    }
    if (strcmp(line, "RESTART") == 0) {
        g_restart_request = true;
        printf("RESTART OK (warm)\n");
        return true;
    }
    if (strcmp(line, "NAP") == 0) {
        g_nap_request = true;
        printf("NAP OK\n");
        return true;
    }
    return false;
}
