/**
 * power.h — power policy: the STATE_SLEEP pseudo-off rails, the charging
 * power-diet, and the Battery-task sampling hook (batt_sample).
 *
 * Purpose: expose the sleep-state policy knobs, the filtered battery gauge,
 * and the Battery-task sampling hook.
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

/* "We are in the low-power hold" — Housekeeping/batt_sample check it so a USB
 * unplug edge can't re-arm the BLE scan mid-sleep. */
extern bool g_power_sleep;

/* Filtered battery state, owned and updated by batt_sample (Battery task) and
 * read by the battery icon / info screen / Housekeeping snapshot — so the
 * render path never does an inline ADC hit, and everything shows ONE smoothed
 * value instead of a fresh, load-perturbed instantaneous read each frame.
 * -1 pct = running on USB. */
extern volatile int   g_batt_pct;
extern volatile float g_batt_v;    /* filtered battery volts (calibrated) */
extern volatile bool  g_on_usb;    /* true while USB is connected */

/* Battery-task sampling hook (also declared in rtos_tasks.h). */
void batt_sample(void);

void power_sleep_enter(void);
void power_sleep_exit(void);

#endif /* POWER_H */
