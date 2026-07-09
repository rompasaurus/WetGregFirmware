/* ============================================================================
 *  rtos_tasks.h  —  the 4-task split: public interface
 * ============================================================================
 *
 *  Phase 2 turns the single app_task (which ran the whole firmware) into FOUR
 *  cooperating FreeRTOS tasks. This header is the CONTRACT between them and the
 *  state-machine code that still lives in main.c. See docs/05-DESIGN-AND-
 *  ARCHITECTURE.md and docs/06-SYNCHRONIZATION.md for the full picture.
 *
 *  THE FOUR TASKS
 *  --------------
 *    UI / Logic   (core 0, mid prio)  — the state machine in main.c. Renders into
 *                                       frame[], asks the Display task to show it,
 *                                       and reacts to input events from a queue.
 *    Input        (core 0, top prio)  — the ONLY runtime reader of the joystick.
 *                                       Debounces + auto-repeats, posts discrete
 *                                       press EVENTS to g_input_q. Top priority so
 *                                       a press is captured within a few ms even
 *                                       while the screen is mid-refresh.
 *    Display      (core 1, ALONE)     — the ONLY post-boot caller of the e-ink
 *                                       driver. Pulls a finished framebuffer off a
 *                                       queue and does the ~300 ms blocking refresh.
 *                                       Because it is alone on core 1, that block
 *                                       never stalls core 0.
 *    Housekeeping (core 0, low prio)  — samples accelerometer/steps/battery and
 *                                       publishes a mutex-guarded snapshot the UI
 *                                       reads. Lowest priority so it never starves
 *                                       input.
 *
 *  WHY THE SPLIT FIXES THE LAG: in the old super-loop, the 300 ms e-ink refresh
 *  blocked joystick sampling. Now the refresh runs on core 1 (Display task) while
 *  the high-priority Input task keeps sampling on core 0 — presses are never
 *  dropped. See doc 03 for the concepts.
 * ========================================================================== */

#ifndef RTOS_TASKS_H
#define RTOS_TASKS_H

#include <stdint.h>
#include <stdbool.h>

#include "FreeRTOS.h"
#include "queue.h"
#include "semphr.h"
#include "task.h"

/* ----------------------------------------------------------------------------
 *  Task tuning  —  priorities, stack sizes (in WORDS = 4 bytes), core pinning
 * ------------------------------------------------------------------------- */
/*  Priority rule (doc 03 §3): Input must outrank everyone so a press preempts
 *  whatever is running. Housekeeping is lowest so it never delays input. UI and
 *  Display are equal but live on different cores. tskIDLE_PRIORITY is 0. */
#define UI_TASK_PRIO       (tskIDLE_PRIORITY + 2)
#define INPUT_TASK_PRIO    (tskIDLE_PRIORITY + 3)   /* highest */
#define DISPLAY_TASK_PRIO  (tskIDLE_PRIORITY + 2)
#define HK_TASK_PRIO       (tskIDLE_PRIORITY + 1)   /* lowest */

/*  Stacks: starting estimates; trimmed in Phase 3 via uxTaskGetStackHighWater
 *  Mark(). UI is biggest (deep render/snprintf chain). */
#define INPUT_TASK_STACK    512
#define DISPLAY_TASK_STACK  2048   /* the EPD driver + SPI push call chain */
#define HK_TASK_STACK       2048   /* accel maths + battery ADC */

/* Input acquisition mode:
 *   0 = POLLING (Phase 2, proven) — the Input task samples the joystick every
 *       few ms. This is the DEFAULT.
 *   1 = INTERRUPT-driven (Phase 3a) — a GPIO edge ISR wakes the Input task.
 * Polling is the default because the interrupt path SOFT-BRICKED the RP2350 under
 * FreeRTOS SMP (the firmware's first-ever ...FromISR call) and is not yet root-
 * caused. Turn it on (=1) ONLY on the bench with USB serial attached, so a fault
 * is observable instead of bricking a deployed board. */
#ifndef INPUT_USE_IRQ
#define INPUT_USE_IRQ  0
#endif

/* Input feel — tune these for snappiness:
 *   REPEAT_DELAY_MS: how long a direction must be HELD before it starts auto-
 *                    repeating (longer than a normal tap, so one tap = one move).
 *   REPEAT_RATE_MS : the auto-repeat interval while held (smaller = faster scroll).
 * In INTERRUPT mode a press is captured instantly and these govern only the held-
 * key repeat; in POLLING mode they govern the same edge/repeat logic per sample. */
#define INPUT_REPEAT_DELAY_MS  400
#define INPUT_REPEAT_RATE_MS    90

/*  Core affinity bitmasks: bit 0 = core 0, bit 1 = core 1. Everything is on
 *  core 0 EXCEPT the Display task, which is alone on core 1. (The cyw43/Wi-Fi/BT
 *  background task also lives on core 0 — see main.c.) */
#define CORE0_AFFINITY     (1u << 0)
#define CORE1_AFFINITY     (1u << 1)

/* ----------------------------------------------------------------------------
 *  The sensor snapshot  —  Housekeeping publishes it, UI reads it
 * ------------------------------------------------------------------------- */
/*  Instead of the UI task touching the accelerometer/battery globals directly
 *  (a cross-task data race once Housekeeping owns them), Housekeeping fills this
 *  struct under a mutex and the UI copies it out under the same mutex. ONLY
 *  primitive fields here so the header needs no main.c types — and so a plain
 *  memcpy under the mutex is correct (NOT volatile; the mutex provides ordering,
 *  see doc 06 §3 and the critic's note on volatile+memcpy). */
typedef struct {
    uint8_t  orientation;     /* 0..2 accelerometer hold (OR_LAND_R/L/TALL) */
    bool     is_tall;         /* convenience: tall (portrait) layout? */
    int16_t  ax, ay, az;      /* raw accelerometer counts */
    float    mag;             /* accel magnitude in g */
    uint32_t steps_today;     /* daily step tally */
    uint32_t active_seconds;  /* daily active-time tally */
    uint32_t last_motion_ms;  /* timestamp of last detected motion (idle logic) */
    bool     mpu_ok;          /* accelerometer present/initialised */
    int      batt_pct;        /* 0..100, or -1 when USB-powered */
    float    vsys;            /* battery volts (calibrated) */
    float    vsys_raw;        /* battery volts UNcalibrated (for the cal screen) */
    bool     usb;             /* USB power present? */
} sensor_snapshot_t;

/* ----------------------------------------------------------------------------
 *  Shared IPC objects (defined in rtos_tasks.c)
 * ------------------------------------------------------------------------- */
extern QueueHandle_t     g_input_q;    /* Input task -> UI: discrete press codes */
extern SemaphoreHandle_t g_snap_mtx;   /* guards the sensor snapshot */

/* ----------------------------------------------------------------------------
 *  API used by the UI state machine (main.c)
 * ------------------------------------------------------------------------- */

/* Create + pin the Input, Display, and Housekeeping tasks. Call this from inside
 * the UI task (app_task) AFTER cyw43/battery/accel init, so the UI task's own
 * handle can be recorded as the display-completion target. */
void rtos_tasks_start(void);

/* Block until the Display task (core 1) has registered with the flash-lockout
 * machinery (flash_safe_execute_core_init). The UI task MUST call this before any
 * flash write (e.g. saved_load's first-boot seed) so that write is SMP-safe — an
 * explicit handshake, not a timing guess. Returns after the ready signal or a
 * safety timeout. */
void rtos_wait_flash_ready(void);

/* Copy the latest sensor snapshot out (mutex-guarded). Cheap; safe from the UI. */
void rtos_snapshot_get(sensor_snapshot_t *out);

/* Housekeeping calls this (from hk_sample in main.c) to publish a freshly-sampled
 * snapshot into the shared copy under the mutex. */
void rtos_snapshot_publish(const sensor_snapshot_t *in);

/* Render the CURRENT contents of frame[] to the panel: transpose into a free
 * back-buffer and hand it to the Display task. Returns immediately (the ~300 ms
 * refresh happens on core 1); only blocks if BOTH display buffers are still in
 * flight (rare — correct throttling, and input keeps flowing via the Input task). */
void display_render(void);

/* Display-task panel commands — sent through the SAME render queue as frames,
 * so they execute strictly in-order after any in-flight blit. Values sit far
 * above the real buffer indices (0/1) so the Display task can tell them apart. */
#define DISP_CMD_SLEEP  100   /* EPD deep sleep — image retained at ~zero power */
#define DISP_CMD_WAKE   101   /* EPD hardware reset + re-init + full clear */

/* Queue a DISP_CMD_* for the Display task (used by the STATE_SLEEP power path). */
void rtos_display_cmd(int cmd);

/* Block until the joystick produces an event, or `timeout_ms` elapses. Returns
 * true and sets *code (an INPUT_* value) on an event; false on timeout. This
 * replaces the old busy-poll: the UI sleeps efficiently instead of spinning. */
bool ui_get_input(uint8_t *code, uint32_t timeout_ms);

/* True while a frame is in flight to the panel (fewer than both back-buffers
 * free). The Input task paces presses with this; the STATE_SLEEP power path
 * uses it to wait for the final still before deep-sleeping the panel. */
bool ui_display_busy(void);

/* Called from the joystick GPIO interrupt (in main.c) to wake the Input task
 * the instant any joystick line changes. Safe before the task exists (no-op). */
void rtos_input_isr_notify(void);

/* ----------------------------------------------------------------------------
 *  Hooks the tasks call back into main.c (defined there, declared here so
 *  rtos_tasks.c stays free of hardware/driver details)
 * ------------------------------------------------------------------------- */
void    hk_sample(void);                 /* sample sensors + publish snapshot (Housekeeping) */
uint8_t read_joystick(void);             /* read+rotate joystick -> INPUT_* (Input task; already in main.c) */
void    display_init_panel(void);        /* EPD_Init + EPD_Clear (Display task prologue) */
void    display_blit(int buf_idx);       /* EPD_Partial(display_buf[idx]) (Display task) */
void    display_panel_cmd(int cmd);      /* EPD deep-sleep / re-init (Display task; DISP_CMD_*) */
void    display_grab_into(int buf_idx);  /* copy ui_buf -> display_buf[idx] (UI side) */

#endif /* RTOS_TASKS_H */
