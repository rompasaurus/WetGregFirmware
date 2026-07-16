/**
 * motion.h — accelerometer-derived services:
 *   - orientation auto-rotate (hold classification + input/display rotation)
 *   - pedometer + daily activity tallies
 *   - viewed/idle detection (pocket freeze)
 *   - the Housekeeping sampling hook (hk_sample) that publishes the shared
 *     sensor snapshot (rtos_tasks.h)
 *
 * Purpose: publish the accelerometer-derived services (orientation, steps,
 * activity, idle detection) that the UI and canvas build on.
 */
#ifndef MOTION_H
#define MOTION_H

#include <stdbool.h>
#include <stdint.h>

#include "joystick.h"   /* input_rotation_t */

/* While calibrating: 1 = log accel/orientation to serial AND draw an
 * on-screen HUD (accel + orientation). Set to 0 once the map is dialed in. */
#define ORIENT_DEBUG 0

/* ── Orientation model: 3 valid holds (joystick ABOVE the screen is ignored) ──
 *   OR_LAND_R = landscape, joystick RIGHT  → WIDE side-by-side
 *   OR_LAND_L = landscape, joystick LEFT   → WIDE side-by-side (flipped)
 *   OR_TALL   = portrait,  joystick BOTTOM → TALL longways
 * Per-orientation display + joystick input rotation in one table — CALIBRATE
 * from the on-screen HUD, then adjust this table and the classifier. */
enum { OR_LAND_R = 0, OR_LAND_L = 1, OR_TALL = 2 };
typedef struct { bool tall; int disp_rot; input_rotation_t in_rot; } orient_cfg_t;
extern const orient_cfg_t ORIENT_CFG[3];

/* Shared orientation state — written by the Housekeeping task
 * (orientation_update), read by the UI task. Both are pinned to core 0;
 * see the invariant note on input_rotation in joystick.c. */
extern int           g_orientation;    /* 0..3 hold from the accelerometer */
extern volatile bool g_orient_primed;  /* first post-boot verdict has landed */

/* Display settings (runtime mirror of g_saved.auto_rotate / .manual_orient;
 * loaded in saved_load). When auto_rotate is false, orientation_update()
 * locks g_orientation to g_manual_orient. */
extern bool    g_auto_rotate;
extern uint8_t g_manual_orient;

bool orientation_is_tall(void);
void orientation_update(void);

/* ── Pedometer + daily activity ── */
extern uint32_t step_count;
extern float    step_threshold;         /* g — adjustable (Motion menu) */
extern uint32_t steps_today;
extern uint32_t active_seconds_today;

/* ── Pocket / not-viewed detection → freeze e-ink redraws to save power ── */
extern bool     g_screen_idle;          /* HK-owned redraw-freeze flag */
extern uint32_t last_motion_ms;         /* timestamp of last detected motion */

bool screen_is_viewed(void);
void wake_screen(void);

/* Housekeeping-task sampling hook (also declared in rtos_tasks.h). */
void hk_sample(void);

#endif /* MOTION_H */
