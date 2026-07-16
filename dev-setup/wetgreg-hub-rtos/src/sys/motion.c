/**
 * motion.c — accelerometer-derived services (Housekeeping task).
 *
 * Purpose: run every accel-derived computation on the Housekeeping cadence
 * and publish the shared sensor snapshot.
 *
 * This module is the ONLY runtime caller of the I2C accelerometer — a fast,
 * cyw43-lock-FREE read — so it samples motion + orientation + steps every
 * Housekeeping pass at a steady 20 Hz and publishes a snapshot the UI copies.
 * Battery/USB values come from the Battery task (batt_sample, sys/power.c).
 */
#include "motion.h"

#include <math.h>
#include <stdio.h>

#include "pico/stdlib.h"

#include "accel.h"
#include "canvas.h"       /* display_rotation (ORIENT_DEBUG log only) */
#include "power.h"        /* cached g_batt_pct / g_batt_v / g_on_usb */
#include "rtc_compat.h"
#include "rtos_tasks.h"   /* sensor_snapshot_t + rtos_snapshot_publish */

/* ─── Orientation state ─── */
const orient_cfg_t ORIENT_CFG[3] = {
    /* OR_LAND_R */ { false, 90,  ROT_180 },
    /* OR_LAND_L */ { false, 270, ROT_0   },
    /* OR_TALL   */ { true,  180, ROT_270 },   /* joystick-bottom: display flipped, input +180 */
};

int           g_orientation  = 0;
volatile bool g_orient_primed = false;

bool    g_auto_rotate   = true;
uint8_t g_manual_orient = OR_TALL;

bool orientation_is_tall(void) { return ORIENT_CFG[g_orientation].tall; }

/* ─── Pedometer state ─── */
uint32_t step_count     = 0;
float    step_threshold = 1.3f;  /* g — adjustable */
static bool step_above  = false; /* debounce: was last sample above threshold? */

/* Each detected crossing counts as 2 steps. The detector is sampled at ~4 Hz
 * (orientation_update's 250 ms gate), so at a normal ~2 step/s walking cadence
 * roughly every other impact falls between samples — measured on-body, the raw
 * count landed at ~half of actual steps. */
#define STEP_CAL_FACTOR 2

/* Simple pedometer: detect step when magnitude crosses threshold going up then back down */
static void pedometer_update(void) {
    float mag = accel_magnitude();
    if (!step_above && mag > step_threshold) {
        step_above = true;
        step_count += STEP_CAL_FACTOR;
    } else if (step_above && mag < (step_threshold - 0.3f)) {
        step_above = false;
    }
}

/* ─── Daily activity tracking (steps + active time) ───────────────────────
 * steps_today is the running pedometer delta; active_seconds_today accrues
 * whenever there's real movement. Both reset when the RTC date rolls over.
 * Sampled cheaply (pedometer at ~20 Hz on the main screen, activity at ~4 Hz).
 * LOW-POWER ROADMAP: the SC7A20 INT1 line is on GP15 — the next step is to arm
 * its activity/wake interrupt so the MCU can sleep and only sample on motion. */
uint32_t steps_today          = 0;
uint32_t active_seconds_today = 0;
static int8_t   activity_day    = -1;   /* RTC day the tally belongs to */
static uint32_t last_step_total = 0;    /* step_count snapshot for the delta */
static uint32_t active_accum_ms = 0;

static void activity_update(uint32_t dt_ms) {
    datetime_t t; rtc_get_datetime(&t);
    if (t.day != activity_day) {         /* new day → reset */
        activity_day = t.day;
        steps_today = 0;
        active_seconds_today = 0;
        last_step_total = step_count;
        active_accum_ms = 0;
    }
    if (step_count >= last_step_total)
        steps_today += step_count - last_step_total;
    last_step_total = step_count;

    /* Movement = accel magnitude deviating from 1 g (rest). */
    if (fabsf(accel_magnitude() - 1.0f) > 0.12f) {
        active_accum_ms += dt_ms;
        if (active_accum_ms >= 1000) {
            active_seconds_today += active_accum_ms / 1000;
            active_accum_ms %= 1000;
        }
    }
}

/* ─── Pocket / not-viewed detection → freeze e-ink redraws to save power ─────
 * There's no proximity sensor, so "being viewed" is inferred from the
 * accelerometer: the main screen stops refreshing (e-ink holds its last image
 * at ZERO power) after a stretch of stillness, or immediately when the device
 * is laid face-down. Any handling/jostle — or a button press — wakes it and
 * forces a fresh redraw. (Continuous in-pocket walking looks like handling
 * without a proximity/IR sensor; arming the SC7A20 INT1 line on GP15 for
 * hardware motion-wake is the future path to true MCU sleep.) */
#define VIEW_IDLE_MS 30000u            /* stillness before the screen freezes */
bool     g_screen_idle  = false;
uint32_t last_motion_ms = 0;

static void viewing_update(void) {
    static uint32_t last_ms = 0;
    static float    prev_mag = 1.0f;
    uint32_t now = to_ms_since_boot(get_absolute_time());
    if (!mpu_ok) { last_motion_ms = now; return; }   /* no accel → never sleep */
    if (now - last_ms < 200) return;                 /* ~5 Hz is plenty */
    last_ms = now;
    float mag = accel_magnitude();
    float az  = accel_g(accel_z);
    bool moving    = fabsf(mag - prev_mag) > 0.06f;  /* handling / jostle */
    bool face_down = az < -0.55f;                    /* screen-down on a surface */
    prev_mag = mag;
    if (moving && !face_down) last_motion_ms = now;
}

bool screen_is_viewed(void) {   /* recent motion, not laid face-down */
    return (to_ms_since_boot(get_absolute_time()) - last_motion_ms) < VIEW_IDLE_MS;
}

void wake_screen(void) {        /* user is present → unfreeze + redraw */
    g_screen_idle  = false;
    last_motion_ms = to_ms_since_boot(get_absolute_time());
}

/* ─── Orientation → display + input rotation (accelerometer auto-rotate) ───
 * Classifies the in-plane gravity vector into one of the valid holds and,
 * with hysteresis, sets g_orientation. The canvas setters then pick a
 * compatible display_rotation, and the joystick input rotation follows too.
 *
 * CALIBRATION: the accel-sign → orientation mapping and the input map are
 * first-pass guesses — confirm each physical hold on the device and adjust.
 * Classify to the nearest anchor angle, or -1 when indeterminate (device
 * flat, or joystick-above ignore zone). Pure math — the hysteresis / prime
 * policy lives in orientation_update(). */
static int orientation_classify(float ax, float ay) {
    /* Anchor angle per orientation — classify to the nearest. CALIBRATE: the
     * landscape two come from your readings; OR_TALL is a guess until you send
     * the joystick-bottom angle. */
    static const float OR_ANGLE[3] = {
        [OR_LAND_R] = 90.0f,    /* joystick right  */
        [OR_LAND_L] = 270.0f,   /* joystick left   (calibrated) */
        [OR_TALL]   = 0.0f,     /* joystick bottom (calibrated) */
    };
    /* Joystick-ABOVE (joystick at TOP ≈ 180°) is ignored: keep current. */
    const float IGNORE_CENTER = 180.0f, IGNORE_HALF = 55.0f;

    /* Need enough in-plane gravity to be meaningful (else the device is flat). */
    if (sqrtf(ax * ax + ay * ay) < 0.35f) return -1;

    float ang = atan2f(ay, ax) * 57.2958f;
    if (ang < 0) ang += 360.0f;

    float di = fabsf(ang - IGNORE_CENTER); if (di > 180) di = 360 - di;
    if (di < IGNORE_HALF) return -1;              /* joystick above → ignore */

    int o = 0; float best = 1e9f;
    for (int i = 0; i < 3; i++) {
        float d = fabsf(ang - OR_ANGLE[i]); if (d > 180) d = 360 - d;
        if (d < best) { best = d; o = i; }
    }
    return o;
}

void orientation_update(void) {
    /* Manual orientation: lock to the user's chosen hold regardless of the
     * accelerometer (so it works even on a unit with no accel). */
    if (!g_auto_rotate) {
        if (g_orientation != (int)g_manual_orient) {
            g_orientation = (int)g_manual_orient;
            input_set_rotation(ORIENT_CFG[g_orientation].in_rot);
        }
        g_orient_primed = true;
    }
    if (!mpu_ok) return;

    /* Fresh accel EVERY Housekeeping pass (~20 Hz): the read is a 0.2 ms I2C
     * burst, and this fast cadence is what makes auto-rotate feel instant —
     * the old shared 250 ms gate made a rotation take ~1-2 s to land before
     * the panel even started refreshing. The step detector KEEPS the 250 ms
     * gate below: STEP_CAL_FACTOR was measured against that exact 4 Hz
     * cadence, so sampling it faster would wreck the calibration. */
    mpu_read_all();

    static uint32_t last_step_ms = 0;
    uint32_t now = to_ms_since_boot(get_absolute_time());
    if (now - last_step_ms >= 250) {
        last_step_ms = now;
        pedometer_update();      /* coarse step sampling at the 4 Hz cadence */
        activity_update(250);    /* daily steps + active-time accrual */
#if ORIENT_DEBUG
        {
            float dax = accel_g(accel_x), day_ = accel_g(accel_y);
            float ang = atan2f(day_, dax) * 57.2958f;
            if (ang < 0) ang += 360.0f;
            printf("[ORIENT] ax=%.2f ay=%.2f az=%.2f  ang=%.0f  o=%d tall=%d rot=%d\n",
                   (double)dax, (double)day_, (double)accel_g(accel_z), (double)ang,
                   g_orientation, orientation_is_tall(), display_rotation);
        }
#endif
    }
    if (!g_auto_rotate) return;   /* manual hold: steps counted, skip auto-rotate */
    /* In-plane gravity angle (degrees, 0..360). Measured anchors:
     *   joystick LEFT  → (X+0.9, Y0.0) → ~0°
     *   joystick RIGHT → (X0.1, Y+0.8) → ~83° (≈90°)
     *   joystick BOTTOM (tall) → TODO: read the HUD in that hold and set below. */
    float ax = accel_g(accel_x), ay = accel_g(accel_y);

    int o = orientation_classify(ax, ay);
    if (o < 0) return;                       /* flat / ignore zone: keep current */

    /* First valid verdict after boot applies IMMEDIATELY — the boot splash
     * holds its first frame on g_orient_primed, and waiting out the hysteresis
     * (~1 s at this 4 Hz cadence) would draw it in the wrong hold. Every
     * later switch still goes through the stable-read filter below. */
    if (!g_orient_primed) {
        g_orient_primed = true;
        if (g_orientation != o) {
            g_orientation = o;
            input_set_rotation(ORIENT_CFG[o].in_rot);
        }
        return;
    }

    /* Hysteresis: ~300 ms of consistent reads at the ~20 Hz cadence before
     * switching — the same flap protection the old 3-of-4Hz filter gave
     * (~750-1000 ms), at a quarter of the latency. */
    static int cand = 0, stable = 0;
    if (o == cand) { if (stable < 6) stable++; }
    else { cand = o; stable = 0; }

    if (stable >= 6 && g_orientation != cand) {
        g_orientation = cand;
        input_set_rotation(ORIENT_CFG[g_orientation].in_rot);  /* joystick follows */
    }
}

/* ─── Housekeeping sampling (Phase 2) ───────────────────────────────────────
 * Called repeatedly by the Housekeeping task (rtos_tasks.c). Samples motion +
 * orientation + steps every pass at a steady 20 Hz and publishes a snapshot
 * the UI copies. Battery/USB values come from batt_task (batt_sample), cached
 * in g_batt_pct/g_batt_v/g_on_usb so this loop never touches the cyw43 lock
 * and auto-rotate stays instant even under heavy radio traffic. */
void hk_sample(void) {
    orientation_update();                  /* accel -> g_orientation, steps, input rotation */
    viewing_update();                      /* updates last_motion_ms from real motion       */
    g_screen_idle = !screen_is_viewed();   /* pocket/idle freeze flag — HK owns it now       */

    sensor_snapshot_t s;
    s.orientation    = (uint8_t)g_orientation;
    s.is_tall        = orientation_is_tall();
    s.ax = accel_x; s.ay = accel_y; s.az = accel_z;
    s.mag            = accel_magnitude();
    s.steps_today    = steps_today;
    s.active_seconds = active_seconds_today;
    s.last_motion_ms = last_motion_ms;
    s.mpu_ok         = mpu_ok;
    s.batt_pct       = g_batt_pct;
    s.vsys           = g_batt_v;
    s.vsys_raw       = 0.0f;   /* the battery-cal screen takes its own fresh RAW read */
    /* Cached value from batt_task, NOT a fresh read: is_usb_powered()/read_vsys
     * take the cyw43 lock, and doing that in this loop parks the lowest-priority
     * HK task behind radio traffic — which starved the accel sampling and made
     * auto-rotate sluggish. batt_sample() refreshes g_on_usb/g_batt_* off-task. */
    s.usb            = g_on_usb;
    rtos_snapshot_publish(&s);
}
