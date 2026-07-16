/**
 * joystick.c — 5-way joystick driver: GPIO init, debounce-free raw read
 * (debounce/edge/repeat live in the Input task), and the rotation remap.
 *
 * Purpose: own the joystick GPIOs: init, the raw read the Input task polls,
 * and the hold-rotation remap applied to every read.
 */
#include "joystick.h"

#include "pico/stdlib.h"
#include "hardware/gpio.h"

#include "rtos_tasks.h"   /* INPUT_USE_IRQ + rtos_input_isr_notify() */

/* ─── Input orientation remap ───────────────────────────────────────────
 * On the WetGreg board (held screen-left / joystick-right) the physical axes
 * read reversed, so we default to 180° (up<->down, left<->right).
 *
 * volatile: written by the Housekeeping task (orientation_update) and read by
 * the higher-priority Input task (read_joystick). A single aligned enum store
 * is atomic on the Cortex-M33, so no torn read — `volatile` just stops the
 * compiler caching it. INVARIANT: both Housekeeping and Input are pinned to
 * core 0; if a future phase moves either off core 0 this must become
 * snapshot-/mutex-guarded. */
static volatile input_rotation_t input_rotation = ROT_180;  /* default for the WetGreg hold */

/* Directions in clockwise order; the rotation is how many 90° CW steps the
 * device is turned, so a press maps forward around the ring by that many. */
static const uint8_t CW_RING[4] = { INPUT_UP, INPUT_RIGHT, INPUT_DOWN, INPUT_LEFT };

#if INPUT_USE_IRQ
/* GPIO interrupt callback (shared by all 5 joystick pins). Fires on every press
 * AND release edge; it does nothing but wake the Input task instantly — all the
 * debounce/edge/repeat logic lives in the task, off the interrupt.
 * Compiled only when INPUT_USE_IRQ=1 (bench-only — see rtos_tasks.h). */
static void joystick_irq_cb(uint gpio, uint32_t events) {
    (void)gpio; (void)events;
    rtos_input_isr_notify();
}
#endif

void joystick_init(void) {
    const uint pins[] = {JOY_UP, JOY_DOWN, JOY_LEFT, JOY_RIGHT, JOY_CENTER};
    for (int i = 0; i < 5; i++) {
        gpio_init(pins[i]);
        gpio_set_dir(pins[i], GPIO_IN);
        gpio_pull_up(pins[i]);
    }
#if INPUT_USE_IRQ
    /* Interrupt-driven input: wake the Input task the instant any line changes,
     * instead of polling. Sub-millisecond press latency, and the core idles when
     * nothing is pressed (better for battery). One callback is shared by all
     * pins; register it on the first, then enable the IRQ on the rest.
     * GATED OFF by default: this path soft-bricked the RP2350 under FreeRTOS SMP
     * and is not yet root-caused. Enable only on the bench with serial. */
    gpio_set_irq_enabled_with_callback(pins[0], GPIO_IRQ_EDGE_FALL | GPIO_IRQ_EDGE_RISE,
                                       true, joystick_irq_cb);
    for (int i = 1; i < 5; i++)
        gpio_set_irq_enabled(pins[i], GPIO_IRQ_EDGE_FALL | GPIO_IRQ_EDGE_RISE, true);
#endif
}

void input_set_rotation(input_rotation_t r) { input_rotation = r; }

static uint8_t rotate_input(uint8_t dir) {
    if (dir == INPUT_NONE || dir == INPUT_CENTER) return dir;  /* center is rotation-invariant */
    for (int i = 0; i < 4; i++) {
        if (CW_RING[i] == dir)
            return CW_RING[(i + (int)input_rotation) & 3];
    }
    return dir;
}

/* The Input task (rtos_tasks.c) is the sole runtime caller. */
uint8_t read_joystick(void) {
    uint8_t raw = INPUT_NONE;
    if      (!gpio_get(JOY_UP))     raw = INPUT_UP;
    else if (!gpio_get(JOY_DOWN))   raw = INPUT_DOWN;
    else if (!gpio_get(JOY_LEFT))   raw = INPUT_LEFT;
    else if (!gpio_get(JOY_RIGHT))  raw = INPUT_RIGHT;
    else if (!gpio_get(JOY_CENTER)) raw = INPUT_CENTER;
    return rotate_input(raw);
}
