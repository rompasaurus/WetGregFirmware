/**
 * power.c — power policy.
 *
 * Purpose: implement the power policy: pseudo-off sleep entry/exit (radios,
 * panel, clocks), battery sampling + the filtered gauge, the auto-anchored
 * calibration, the time-left estimate, and the low-battery policy.
 *
 * ─── Pseudo-off power rails (STATE_SLEEP) ───────────────────────────────────
 * "As off as we can get without losing the wake button":
 *   - BLE social scan + advertising stopped, HCI powered OFF (CYW43 BT core dark)
 *   - WiFi disassociated; with SLEEP_CYW43_OFF the whole CYW43 is powered down
 *     (WL_REG_ON low) and GPIO 25 is held high so the VSYS divider stays readable
 *   - e-ink panel in deep sleep (the sleep still is retained at zero power)
 *   - clk_sys dropped to 48 MHz sourced from PLL_USB and PLL_SYS powered DOWN
 *     (set_sys_clock_48mhz — USB serial stays alive), core rail 1.10 → 1.00 V
 *   - both cores WFI whenever idle (freertos_hooks.c), so the tasks' periodic
 *     polling costs microseconds per wake instead of a spinning core
 * FreeRTOS ticks stretch ~3x while underclocked (each core's SysTick reload was
 * computed at boot for the full clock) — every task just polls proportionally
 * slower, which is exactly what we want. The µs timebase (to_ms_since_boot) is
 * XOSC-derived and stays true, so real-time decisions remain correct.
 */
#include "power.h"

#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "hardware/clocks.h"    /* set_sys_clock_48mhz/_khz — STATE_SLEEP underclock */
#include "hardware/vreg.h"      /* vreg_set_voltage — STATE_SLEEP core-rail drop */

#include "FreeRTOS.h"
#include "task.h"

#include "battery.h"
#include "bt.h"
#include "rtos_tasks.h"
#include "storage.h"
#include "wifi.h"

bool g_power_sleep = false;

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
 * read_vsys_raw_volts() and is_usb_powered() grab the cyw43 lock (GP29 is the
 * shared CYW43 SPI clock / VBUS sense line), so they live on the Battery task
 * where a stall only delays the gauge, never auto-rotate. Values land in
 * g_batt_* (volatile, single-writer) that HK folds into the snapshot.
 *
 * ── PEAK-HOLD over a window, then EMA-smoothed ──
 * Load only ever sags VSYS below the true resting voltage (and makes the D3
 * drop bigger), so the MAX across a window of spaced samples ≈ the lightest-
 * load reading, i.e. the one the calibrated offset is defined for. Awake we
 * sample every 500 ms and fold the 4 s window peak into an EMA; asleep every
 * 5 s with a 40 s window (nothing moves fast there, and each read briefly
 * holds the cyw43 lock). */
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

#define BATT_SAMPLE_MS_AWAKE   500u
#define BATT_SAMPLE_MS_SLEEP  5000u
#define BATT_WINDOW_MS_AWAKE  4000u
#define BATT_WINDOW_MS_SLEEP 40000u

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

    uint32_t period = g_power_sleep ? BATT_SAMPLE_MS_SLEEP : BATT_SAMPLE_MS_AWAKE;
    uint32_t window = g_power_sleep ? BATT_WINDOW_MS_SLEEP : BATT_WINDOW_MS_AWAKE;
    if (now - batt_last_ms < period || (int32_t)(now - g_radio_settle_until) < 0) return;
    batt_last_ms = now;

    bool usb = is_usb_powered();
    g_on_usb = usb;

    if ((was_usb != 1) && usb) {
        usb_since_ms = now ? now : 1;
        pct_at_plug  = (was_usb == 0) ? last_bp : -1;   /* unknown on a boot straight into USB */
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
        float raw = read_vsys_raw_volts();
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
        float raw = read_vsys_raw_volts();
        g_batt_raw_v = raw;
        if (raw > batt_win_peak) batt_win_peak = raw;        /* lightest-load sample */
        if (batt_win_start == 0) batt_win_start = now ? now : 1;
        float v = battery_cell_from_raw(raw);
        if (batt_v_ema <= 0.0f) {                            /* seed immediately on unplug/boot */
            batt_v_ema = v; g_batt_v = v; g_batt_pct = lipo_percent_hyst(v);
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
            g_batt_pct  = lipo_percent_hyst(batt_v_ema);
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

/* ─── Sleep entry / exit ──────────────────────────────────────────────────── */
void power_sleep_enter(void) {
    printf("[SLEEP] pseudo-off: radios down, panel asleep, 48 MHz%s\n",
           SLEEP_CYW43_OFF ? ", CYW43 off" : "");

    /* Radios first, while still at full clock. */
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

#if SLEEP_CYW43_OFF
    /* Whole radio chip dark. The BTstack instance is torn down with it, so the
     * BT module must redo its one-time setup on the next init. Keep the battery
     * task away from the cyw43 lock across the teardown, and switch it to the
     * lock-free read path BEFORE the context disappears. */
    g_radio_settle_until = to_ms_since_boot(get_absolute_time()) + 3000;
    battery_set_cyw43_up(false);              /* lock-free reads, GPIO 25 held high */
    vTaskDelay(pdMS_TO_TICKS(600));           /* HCI power-off settles; any in-flight read finishes */
    cyw43_arch_deinit();
    wetgreg_bt_arch_reset();
    set_sys_clock_48mhz();
    vreg_set_voltage(VREG_VOLTAGE_1_00);
#else
    /* Underclock LAST. Hold the cyw43 lock so its background task is not
     * mid-SPI-transaction while clk_sys (the PIO SPI clock source) ramps. */
    cyw43_thread_enter();
    set_sys_clock_48mhz();                    /* clk_sys+clk_peri 48 MHz, PLL_SYS off */
    vreg_set_voltage(VREG_VOLTAGE_1_00);      /* core rail 1.10 → 1.00 V */
    cyw43_thread_exit();
#endif
}

void power_sleep_exit(void) {
    /* Clock back up FIRST — and raise the rail before the frequency. */
#if SLEEP_CYW43_OFF
    g_radio_settle_until = to_ms_since_boot(get_absolute_time()) + 10000;  /* no reads during bring-up */
    vreg_set_voltage(VREG_VOLTAGE_DEFAULT);
    busy_wait_us(300);
    set_sys_clock_khz(SYS_CLK_KHZ, true);
    g_power_sleep = false;
    if (cyw43_arch_init()) {
        printf("[SLEEP] WARNING: cyw43_arch_init failed on wake — radio stays off\n");
    } else {
        battery_set_cyw43_up(true);
    }
#else
    cyw43_thread_enter();
    vreg_set_voltage(VREG_VOLTAGE_DEFAULT);
    busy_wait_us(300);                        /* rail settle (µs-timer, tick-safe) */
    set_sys_clock_khz(SYS_CLK_KHZ, true);     /* board default (150 MHz on RP2350) */
    cyw43_thread_exit();
    g_power_sleep = false;
#endif

    /* Panel back: hardware reset + init + full clear (clean base, no ghost of
     * the sleep still). Queued now, executes before the next rendered frame. */
    rtos_display_cmd(DISP_CMD_WAKE);

    /* Radios back the way they were — but STAGGERED: BT/social power on now,
     * the WiFi join fires ~4 s later from the octopus loop (wifi_rejoin_tick)
     * so the combined radio burst can't hog the cyw43 lock while the user is
     * interacting. The battery task's lock-taking reads also pause until the
     * settle deadline so accel sampling (auto-rotate) never stalls. */
    uint32_t now = to_ms_since_boot(get_absolute_time());
    g_radio_settle_until = now + 8000;
    if (g_slept_bt) wetgreg_bt_init();
    if (g_saved.social_on && !g_batt_low) {
        wetgreg_social_set_self(g_saved.wetgreg_id);
        wetgreg_social_enable(true);
    }
    if (g_slept_wifi && !g_batt_low)
        wifi_schedule_rejoin(g_slept_ssid, 4000);
    printf("[SLEEP] awake: clk_sys=%lu Hz clk_peri=%lu Hz, radios restoring\n",
           (unsigned long)clock_get_hz(clk_sys), (unsigned long)clock_get_hz(clk_peri));
}
