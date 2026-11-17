/**
 * power.h — power policy: the STATE_SLEEP pseudo-off rails, the filtered
 * battery gauge + time-left estimate, the low-battery policy, and the
 * Battery-task sampling hook (batt_sample).
 *
 * Purpose: expose the sleep-state policy knobs, the filtered battery state,
 * the low-battery flags, and the Battery-task sampling hook.
 */
#ifndef POWER_H
#define POWER_H

#include <stdbool.h>
#include <stdint.h>

/* ─── Boot tracing (bench builds) ────────────────────────────────────────────
 * -DBOOT_TRACE=1 prints + flushes a marker at each early-boot step so a hang
 * shows how far it got; -DBOOT_LOG_DELAY_MS=n holds main() that long after USB
 * comes up so a serial monitor can attach before anything interesting happens.
 * Both off in normal builds. */
#ifdef BOOT_TRACE
#define BOOT_TRACE_ON 1
#undef BOOT_TRACE
#else
#define BOOT_TRACE_ON 0
#endif
#ifndef BOOT_LOG_DELAY_MS
#define BOOT_LOG_DELAY_MS 0
#endif
#if BOOT_TRACE_ON
#include <stdio.h>
#include "pico/stdio.h"
#define BOOT_TRACE(msg) do { printf("[TRACE] %s\n", (msg)); stdio_flush(); } while (0)
#else
#define BOOT_TRACE(msg) do { } while (0)
#endif

/* ─── Awake clock ────────────────────────────────────────────────────────────
 * Greg is an e-ink UI: nothing he does needs 150 MHz. With AWAKE_CLK_48MHZ
 * (default) the whole firmware runs at 48 MHz straight off PLL_USB with
 * PLL_SYS powered down, all the time. The core rail stays at its 1.10 V
 * default (lowering it broke warm boots — see awake_clock_apply). The radio's
 * PIO SPI program must match: CMakeLists.txt selects the SDK's low-speed
 * variant, without which the CYW43 does not start at this clock.
 * Build with -DAWAKE_CLK_48MHZ=OFF to go back to the board default. */
#ifndef AWAKE_CLK_48MHZ
#define AWAKE_CLK_48MHZ 1
#endif

/* ─── Nap (STATE_SLEEP) — policy knobs ───────────────────────────────────────
 * Greg dozes off (sleep still + everything off, see power_sleep_enter) when
 * the device sits MOTIONLESS for SLEEP_IDLE_MS, or immediately when CENTER is
 * pressed SLEEP_TAP_N times inside SLEEP_TAP_WINDOW_MS. Only CENTER wakes him.
 *
 * Two depths:
 *   DEEP nap  (on battery, DEEP_NAP=1, the default): the RP2350's switched
 *             core is POWERED OFF through POWMAN (a "P-state"), the CYW43 is
 *             held off, the accelerometer is powered down. What is left is the
 *             always-on domain: the POWMAN timer (which keeps the clock) and
 *             the CENTER wake comparator. Raspberry Pi measure a bare Pico 2
 *             at ~0.2 mA in this state, against ~6 mA for the best clock-
 *             gated WFI sleep. Waking is a reboot: the day's steps, the mood
 *             and the last-sync time ride through in the POWMAN scratch
 *             registers and the boot goes straight to the octopus.
 *   LIGHT nap (on USB, or DEEP_NAP=0): radios off + panel asleep, everything
 *             else running, so USB serial / picotool keep working on the
 *             bench and the charger's time-on-USB can be tracked for the
 *             gauge's auto-anchor. Unplug a light-napping Greg and he drops to
 *             the deep nap ~90 s later.
 *
 * A deep-napping Greg cannot see USB arrive, so the POWMAN alarm wakes him
 * every DEEP_NAP_PEEK_S for a QUIET boot (no chime, panel untouched, radios
 * not started — about 2 s) to look at VSYS: battery → straight back down;
 * USB → stay in the light nap so the charge is tracked. It has to be a real
 * boot because VSYS is only readable with the radio chip up (battery.c). */
/* OFF BY DEFAULT until it has had a real run. Bench status (2026-11-16, with a
 * debug probe on the SWD pads): power-down, wake on the alarm (20 s and 10
 * min), the clock and nap record carried through, and the USB-present peek all
 * work. NOT yet verified: a physical CENTER press, and the on-battery peek
 * that goes straight back down. Build with -DDEEP_NAP=ON to use / test it.
 * (An earlier bug here — treating an SDK timeout as a refusal — left units
 * powered down with no wake source armed; see deep_power_down in power.c.) */
#ifndef DEEP_NAP
#define DEEP_NAP 0
#endif
#ifndef DEEP_NAP_PEEK_S
#define DEEP_NAP_PEEK_S 900u                     /* look for USB every 15 min */
#endif
#define NAP_DEEPEN_AFTER_UNPLUG_MS 90000u        /* light → deep, after the auto-anchor window */
#define SLEEP_IDLE_MS        (5u * 60u * 1000u)  /* 5 min of stillness */
#define SLEEP_TAP_N          5                   /* CENTER presses ... */
#define SLEEP_TAP_WINDOW_MS  5000u               /* ... within 5 s */

/* ─── Battery policy knobs ──────────────────────────────────────────────────── */
#define BATT_CAPACITY_MAH        1000   /* 503450 cell on the Rev 1 footprint (assumed) */
#define BATT_CHARGER_MA          100    /* TP4056 PROG R3 = 12 kΩ → 1200/12k = 100 mA */
#define BATT_NOMINAL_AVG_MA      10     /* fallback AWAKE draw for the "(EST)" time-left
                                         * (the deep nap costs ~0.2 mA, so time left is
                                         * really "hours of being awake") */
#define BATT_LOW_PCT             15     /* ≤ → radios go quiet (social scan off) */
#define BATT_LOW_CLEAR_PCT       22     /* ≥ → back to normal */
#define BATT_CRIT_V              3.40f  /* ≤ (resting est.) → forced sleep, CHARGE ME */
#define BATT_CRIT_CLEAR_V        3.55f
/* An unplug only auto-anchors the calibration when USB was present long enough
 * for the TP4056 to have reached its 4.20 V CV phase: the time to charge from
 * the last known on-battery level to ~85 % at BATT_CHARGER_MA, plus a 1 h CV
 * tail (from empty at 100 mA that is ~11 h; from 70 % ~3 h), never less than
 * this floor. */
#define BATT_AUTO_ANCHOR_USB_MIN (2u * 60u)     /* minutes */
#define BATT_AUTO_ANCHOR_MAX_STEP_MV 60  /* slew limit per anchor event */

/* "We are in the low-power hold" — Housekeeping/batt_sample check it so a USB
 * unplug edge can't re-arm the BLE scan mid-sleep. */
extern bool g_power_sleep;

/* Set a moment before the switched core is powered off: the Battery and
 * Housekeeping tasks stop touching the ADC / I2C / flash so nothing is mid-
 * transaction (above all, no half-written settings sector) when power goes. */
extern volatile bool g_power_deep_pending;

/* ─── How this boot came about ───────────────────────────────────────────── */
typedef enum {
    BOOT_COLD = 0,      /* power-on, reset, watchdog, reflash: splash + chime */
    BOOT_NAP_WAKE,      /* CENTER pressed during a deep nap: straight to the octopus */
    BOOT_NAP_PEEK,      /* the periodic deep-nap USB check: a quiet boot into the light nap */
    BOOT_WARM,          /* our own warm restart (radio recovery): silent, straight to the octopus */
} power_boot_t;
extern power_boot_t g_boot_kind;

/* What rode through the deep nap in the POWMAN scratch registers. */
typedef struct {
    bool     valid;
    uint32_t steps_today;
    uint32_t active_seconds;
    int8_t   day;             /* RTC day-of-month the tallies belong to */
    int8_t   mood;            /* current_mood (-1 = all) */
    int8_t   pct;             /* gauge reading when Greg dozed off (-1 unknown) */
    uint32_t nap_start_s;     /* epoch seconds at nap entry (0 unknown) */
    uint32_t wifi_sync_s;     /* epoch seconds of the last NTP sync (0 never) */
} nap_resume_t;
extern nap_resume_t g_nap_resume;

/* FIRST call in main(), before stdio: sets the awake clock, and classifies
 * the boot. A timed deep-nap peek that finds no USB never returns from here —
 * it powers straight back down. */
void power_boot_begin(void);
void power_boot_report(void);   /* once stdio + the RTC are up: log how we booted */

/* BOOT_NAP_PEEK only: mark the firmware as already being in the light nap
 * (radios never started, panel never initialised) so the normal CENTER-wake
 * path brings everything up. `social_on` = restart BLE on wake. */
void power_nap_adopt(bool social_on);

/* Light-nap poll: true when Greg should drop (back) into the deep nap — a
 * peek that found no USB, or ~90 s after being unplugged. power_nap_poll_ms()
 * is how long the nap loop may block before asking again. */
bool     power_nap_should_deepen(void);
uint32_t power_nap_poll_ms(void);

/* Power the switched core off until CENTER (or the peek alarm). Never returns:
 * the next thing that happens is a boot. `mood` is saved for the wake. */
void power_deep_nap_enter(int mood) __attribute__((noreturn));

/* Radio recovery: batt_sample's health probe raises a request when the CYW43
 * stops answering; the octopus loop takes it and calls power_warm_restart(),
 * which saves the day's state and reboots (BOOT_WARM) to re-init the chip. */
bool power_take_restart_request(void);
void power_warm_restart(int mood) __attribute__((noreturn));

/* Serial bench commands (rtc_clock.c forwards any non-TIME line here):
 *   PWR?            clocks, boot kind, USB, gauge
 *   KEY U|D|L|R|C   inject a joystick press
 *   NAP             doze off now (deep on battery, light on USB)
 *   NAP DEEP [s]    force the deep nap even on USB; optional peek period (s)
 *   RESTART         warm restart (what the radio health check does)
 * Returns true if the line was one of ours. */
bool power_debug_cmd(const char *line);
bool power_take_nap_request(void);   /* octopus loop: a NAP command is waiting */

/* Filtered battery state, owned and updated by batt_sample (Battery task) and
 * read by the battery icon / info screen / Housekeeping snapshot — so the
 * render path never does an inline ADC hit, and everything shows ONE smoothed
 * value instead of a fresh, load-perturbed instantaneous read each frame.
 * -1 pct = running on USB. */
extern volatile int   g_batt_pct;
extern volatile float g_batt_v;        /* filtered resting-cell estimate (calibrated) */
extern volatile float g_batt_raw_v;    /* last raw ADC volts (VSYS), for the info screen */
extern volatile bool  g_on_usb;        /* true while USB is connected */
extern volatile float g_batt_hours_left;   /* <0 = unknown */
extern volatile bool  g_batt_hours_est;    /* true = nominal-draw estimate, not measured */
extern volatile bool  g_batt_low;      /* ≤ BATT_LOW_PCT (with hysteresis) */
extern volatile bool  g_batt_critical; /* ≤ BATT_CRIT_V — the app forces sleep */

/* Battery-task sampling hook (also declared in rtos_tasks.h). */
void batt_sample(void);

/* Device-Info keys: anchor the offset so the CURRENT reading reads as a full
 * cell (do this right after a full charge), or reset it to the default. Both
 * persist. Returns false (refused) on USB or before the first reading. */
bool battery_anchor_full_now(void);

/* Nap entry: radios + panel down, then the deep nap (never returns) when on
 * battery, else the light nap (returns; power_sleep_exit undoes it). */
void power_sleep_enter(int mood);
void power_sleep_exit(void);

#endif /* POWER_H */
