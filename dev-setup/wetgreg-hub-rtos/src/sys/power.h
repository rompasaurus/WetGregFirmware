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

/* ─── Pseudo-off sleep state (STATE_SLEEP) — policy knobs ────────────────────
 * Greg dozes off (sleep still + radios off + underclock, see power_sleep_enter)
 * when the device sits MOTIONLESS for SLEEP_IDLE_MS, or immediately when CENTER
 * is pressed SLEEP_TAP_N times inside SLEEP_TAP_WINDOW_MS. Only CENTER wakes
 * him. */
#define SLEEP_IDLE_MS        (5u * 60u * 1000u)  /* 5 min of stillness */
#define SLEEP_TAP_N          5                   /* CENTER presses ... */
#define SLEEP_TAP_WINDOW_MS  5000u               /* ... within 5 s */

/* Power the CYW43 down completely (WL_REG_ON low) while asleep. Removes the
 * radio chip's idle draw but takes the whole cyw43 arch + BTstack through a
 * deinit/init cycle on every nap. BENCH-TEST WITH SERIAL ATTACHED before
 * shipping it (cmake -DSLEEP_CYW43_OFF=ON); the 5 s CENTER-hold watchdog reboot
 * remains the escape hatch. */
#ifndef SLEEP_CYW43_OFF
#define SLEEP_CYW43_OFF 0
#endif

/* ─── Battery policy knobs ──────────────────────────────────────────────────── */
#define BATT_CAPACITY_MAH        1000   /* 503450 cell on the Rev 1 footprint (assumed) */
#define BATT_CHARGER_MA          100    /* TP4056 PROG R3 = 12 kΩ → 1200/12k = 100 mA */
#define BATT_NOMINAL_AVG_MA      12     /* fallback draw for the "(EST)" time-left */
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

void power_sleep_enter(void);
void power_sleep_exit(void);

#endif /* POWER_H */
