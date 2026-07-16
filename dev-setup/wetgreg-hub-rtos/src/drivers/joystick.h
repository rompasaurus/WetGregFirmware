/**
 * joystick.h — 5-way joystick: pin map, logical INPUT_* codes, and the
 * orientation-driven rotation remap.
 *
 * Purpose: define the joystick's public contract: pins, logical INPUT_*
 * codes, and the rotation hook the orientation logic drives.
 *
 * The joystick's physical up/down/left/right are remapped to logical
 * directions through a rotation, so the firmware can match how the device is
 * actually held. The accelerometer orientation logic drives this through
 * input_set_rotation(); every joystick read then auto-rotates.
 */
#ifndef JOYSTICK_H
#define JOYSTICK_H

#include <stdint.h>

/* ─── Joystick pins (WetGreg PCB / breadboard) ─── */
#define JOY_LEFT   2
#define JOY_DOWN   3
#define JOY_UP     4
#define JOY_RIGHT  5
#define JOY_CENTER 6

/* ─── Logical direction codes returned by read_joystick() ─── */
#define INPUT_NONE   0
#define INPUT_UP     1
#define INPUT_DOWN   2
#define INPUT_LEFT   3
#define INPUT_RIGHT  4
#define INPUT_CENTER 5

/* How many 90° CW steps the device is turned relative to the default hold. */
typedef enum { ROT_0 = 0, ROT_90 = 1, ROT_180 = 2, ROT_270 = 3 } input_rotation_t;

void    joystick_init(void);
uint8_t read_joystick(void);   /* read + rotate → INPUT_* (Input task is the sole runtime caller) */
void    input_set_rotation(input_rotation_t r);

#endif /* JOYSTICK_H */
