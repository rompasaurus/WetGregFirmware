/* ============================================================================
 *  rtos_tasks.c  —  the 4-task split: orchestration
 * ============================================================================
 *
 *  This file owns the FreeRTOS plumbing — the queues, the mutex, the task
 *  bodies, and how a frame is handed from the UI task to the Display task. It is
 *  deliberately FREE of hardware/driver code: anything that touches the e-ink
 *  panel, the joystick GPIOs, or the sensors is a small function back in main.c
 *  (declared in rtos_tasks.h). That keeps the concurrency logic here readable in
 *  isolation. Read docs/06-SYNCHRONIZATION.md alongside this file.
 * ========================================================================== */

#include "rtos_tasks.h"
#include "pico/flash.h"   /* flash_safe_execute_core_init — core-1 side of safe flash writes */
#include "hardware/watchdog.h"   /* watchdog_reboot — hard-reboot escape hatch */
#include <string.h>
#include <stdio.h>

/* Set to 1 to log each e-ink refresh duration over USB serial (helps decide if
 * further display-speed work is worth it). Set to 0 to silence. */
#ifndef EPD_TIMING
#define EPD_TIMING 1
#endif

/* INPUT_* codes live in main.c; mirror the ones we need (kept in sync by hand).
 * They are tiny stable constants, not worth a shared header just for these. */
#ifndef INPUT_NONE
#define INPUT_NONE   0
#define INPUT_UP     1
#define INPUT_DOWN   2
#define INPUT_LEFT   3
#define INPUT_RIGHT  4
#define INPUT_CENTER 5
#endif

/* ----------------------------------------------------------------------------
 *  Shared IPC objects
 * ------------------------------------------------------------------------- */
QueueHandle_t     g_input_q   = NULL;   /* Input -> UI: discrete press codes */
SemaphoreHandle_t g_snap_mtx  = NULL;   /* guards g_snap */

/*  The published sensor snapshot. PRIVATE to this file: Housekeeping writes it
 *  via rtos_snapshot_publish(), the UI reads it via rtos_snapshot_get(), both
 *  under g_snap_mtx. Not volatile — the mutex provides the ordering guarantee,
 *  and memcpy of a volatile struct is ill-formed (see doc 06). */
static sensor_snapshot_t g_snap;

/*  Display hand-off: TWO framebuffers (the actual byte arrays live in main.c,
 *  indexed 0/1). We pass OWNERSHIP of a buffer index between the UI and Display
 *  tasks through two queues — pure message passing, so there is no shared index
 *  to race on (doc 06 §2):
 *    - g_free_q  : indices the UI is allowed to render into (seeded with 0 and 1)
 *    - g_render_q: indices that are filled and waiting to be shown
 *  Protocol:
 *    UI:       idx = recv(free_q);  transpose frame[] -> buf[idx];  send(render_q, idx)
 *    Display:  idx = recv(render_q); EPD_Partial(buf[idx]);          send(free_q, idx)
 *  With two buffers the UI can prepare the next frame while the Display task is
 *  still pushing the previous one; it only blocks if BOTH are in flight. */
static QueueHandle_t g_free_q   = NULL;
static QueueHandle_t g_render_q = NULL;

static TaskHandle_t g_input_task = NULL;
static TaskHandle_t g_disp_task  = NULL;
static TaskHandle_t g_hk_task    = NULL;

/* Given by the Display task (core 1) once it has registered with the flash
 * lockout; the UI task waits on it before its first flash write. */
static SemaphoreHandle_t g_flash_ready = NULL;

/* ----------------------------------------------------------------------------
 *  Snapshot publish / get  (tiny critical sections — copy only, never block
 *  while holding the mutex; doc 06 §3)
 * ------------------------------------------------------------------------- */
void rtos_snapshot_publish(const sensor_snapshot_t *in) {
    xSemaphoreTake(g_snap_mtx, portMAX_DELAY);
    g_snap = *in;                       /* struct copy under the lock */
    xSemaphoreGive(g_snap_mtx);
}

void rtos_snapshot_get(sensor_snapshot_t *out) {
    if (!g_snap_mtx) { memset(out, 0, sizeof(*out)); return; }  /* pre-start safety */
    xSemaphoreTake(g_snap_mtx, portMAX_DELAY);
    *out = g_snap;
    xSemaphoreGive(g_snap_mtx);
}

void rtos_wait_flash_ready(void) {
    /* Wait (up to 2 s) for the Display task to have run flash_safe_execute_core_
     * init() on core 1, so the next flash write is multicore-safe. */
    if (g_flash_ready) xSemaphoreTake(g_flash_ready, pdMS_TO_TICKS(2000));
}

/* ----------------------------------------------------------------------------
 *  UI-facing helpers
 * ------------------------------------------------------------------------- */
void display_render(void) {
    if (!g_free_q) return;              /* not started yet — nothing to do */
    int idx;
    /* Claim a free buffer (block if both are in flight — correct throttling: the
     * panel can't show frames faster than ~300 ms anyway, and the Input task
     * keeps capturing presses while we wait). */
    if (xQueueReceive(g_free_q, &idx, portMAX_DELAY) != pdTRUE) return;
    display_grab_into(idx);             /* main.c: ui_buf -> display_buf[idx] */
    xQueueSend(g_render_q, &idx, portMAX_DELAY);   /* Display task takes it from here */
}

bool ui_get_input(uint8_t *code, uint32_t timeout_ms) {
    if (!g_input_q) { vTaskDelay(pdMS_TO_TICKS(timeout_ms)); return false; }
    return xQueueReceive(g_input_q, code, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

/* The e-ink is "busy" whenever a frame is in flight — i.e. fewer than both
 * double-buffers sit free. (display_render() pulls a buffer out to fill it; the
 * Display task returns it after the ~0.7 s waveform completes.) The Input task
 * uses this to PACE presses to the panel instead of a fixed timer. */
#define NUM_DISP_BUFFERS 2
bool ui_display_busy(void) {
    return g_free_q && uxQueueMessagesWaiting(g_free_q) < NUM_DISP_BUFFERS;
}

/* ----------------------------------------------------------------------------
 *  Display task (core 1, alone) — the ONLY post-boot caller of the e-ink driver
 * ------------------------------------------------------------------------- */
static void display_task(void *arg) {
    (void)arg;
    /* Register THIS core (core 1) with the flash-lockout machinery so that when
     * core 0 calls flash_safe_execute() to save settings, this core is parked
     * safely instead of XIP-faulting mid-erase. Must run before any flash write
     * on core 0 can land (app_task delays saved_load until after this). */
    flash_safe_execute_core_init();
    if (g_flash_ready) xSemaphoreGive(g_flash_ready);   /* tell the UI: flash writes are now SMP-safe */

    /* Bring the panel up from THIS task so the SPI/e-ink hardware is touched from
     * exactly one core for the whole program (the bare-metal DEV_Module_Init in
     * main() set up the bus; we do the panel init/clear here, strictly before the
     * first blit). */
    display_init_panel();               /* main.c: EPD_Init + EPD_Clear */

    for (;;) {
        int idx;
        /* Sleep here (no CPU burned) until the UI hands us a filled buffer. */
        if (xQueueReceive(g_render_q, &idx, portMAX_DELAY) != pdTRUE) continue;
#if EPD_TIMING
        TickType_t t0 = xTaskGetTickCount();
        display_blit(idx);              /* main.c: EPD_Partial(display_buf[idx]) — the e-ink waveform */
        printf("[disp] refresh %lu ms\n",
               (unsigned long)((xTaskGetTickCount() - t0) * portTICK_PERIOD_MS));
#else
        display_blit(idx);
#endif
        xQueueSend(g_free_q, &idx, portMAX_DELAY);   /* return the buffer to the UI */
    }
}

/* ----------------------------------------------------------------------------
 *  Input task (core 0, highest prio) — sole runtime joystick reader
 * ------------------------------------------------------------------------- */
/*  Turns the raw, level-based joystick state into discrete press EVENTS:
 *    - UP / DOWN  : fire on press, then AUTO-REPEAT after a >400 ms hold every
 *                   ~120 ms (so a single tap moves one step, a hold scrolls).
 *    - LEFT / RIGHT / CENTER : strict one-shot — fire once per press edge only.
 *  This is the exact edge/repeat logic that used to live inside the menu loop;
 *  centralizing it here means every screen gets clean, identical events from one
 *  place. (doc 06 §4 / the plan's step 5.) */
/* Woken by the joystick GPIO interrupt (main.c's callback calls this). Pure
 * notification — no work in the ISR beyond unblocking the Input task. The body is
 * compiled out unless INPUT_USE_IRQ=1; in the default polling build the function
 * still exists (harmless no-op) but nothing wires the ISR that would call it. */
void rtos_input_isr_notify(void) {
#if INPUT_USE_IRQ
    if (!g_input_task) return;                   /* task not created yet → ignore the edge */
    BaseType_t woke = pdFALSE;
    vTaskNotifyGiveFromISR(g_input_task, &woke);
    portYIELD_FROM_ISR(woke);                    /* switch to the Input task immediately if it's higher prio */
#endif
}

static void input_task(void *arg) {
    (void)arg;
    /* Seed `prev` with the CURRENT stick state, not INPUT_NONE: a key already
     * down when this task starts (the CENTER hold that just triggered the 10 s
     * watchdog reboot, or a finger on the stick at power-on) is stale state,
     * not a press — treating it as an edge injected a phantom event that e.g.
     * skipped the boot splash. It must be released and re-pressed to count. */
    uint8_t    prev     = read_joystick();
    uint8_t    pending  = INPUT_NONE;             /* press waiting for the panel to settle */
    uint32_t   key_down = 0;                      /* ms when the current hold began; 0 = the
                                                   * hold predates this task (no repeats) */
    uint32_t   last_rep = 0;                      /* ms of the last emitted (auto-)repeat */
#if INPUT_USE_IRQ
    TickType_t wait     = portMAX_DELAY;          /* idle: sleep until an edge interrupt */
#endif

    for (;;) {
#if INPUT_USE_IRQ
        /* Interrupt mode: sleep with ZERO CPU until a joystick edge wakes us
         * (instant press), or — while a key is held — until the repeat interval
         * elapses. */
        ulTaskNotifyTake(pdTRUE, wait);
        vTaskDelay(pdMS_TO_TICKS(2));             /* tiny settle so a contact bounce reads as one level */
#else
        /* Polling mode (default): ~125 Hz sampling; vTaskDelay yields the core
         * each pass, so this is not a busy-spin. Proven path (Phase 2). */
        vTaskDelay(pdMS_TO_TICKS(8));
#endif

        uint8_t  j = read_joystick();             /* main.c: rotated current direction */
        uint32_t t = xTaskGetTickCount() * portTICK_PERIOD_MS;

        /* HARD-REBOOT ESCAPE HATCH: hold CENTER for ~10 s and the device reboots,
         * no matter what — this task always runs even if the UI is soft-locked,
         * and watchdog_reboot() restarts the firmware unconditionally. */
        static uint32_t center_since = 0;
        if (j == INPUT_CENTER) {
            if (center_since == 0) center_since = t;
            else if ((uint32_t)(t - center_since) >= 10000) {
                printf("[INPUT] CENTER held 10s -> reboot\n");
                watchdog_reboot(0, 0, 0);          /* immediate, unconditional restart */
            }
        } else {
            center_since = 0;
        }

        /* Turn the raw level into a "pending intent" rather than emitting straight
         * away. UP/DOWN auto-repeat while held; the others are one-shot edges. */
        if (j == INPUT_UP || j == INPUT_DOWN) {
            int edge   = (prev != j);
            int repeat = !edge && key_down != 0
                                && (t - key_down) > INPUT_REPEAT_DELAY_MS
                                && (t - last_rep) > INPUT_REPEAT_RATE_MS;
            if (edge)        { key_down = t; pending = j; }
            else if (repeat) { pending = j; }     /* last_rep is set when we actually emit */
        } else if (j != INPUT_NONE) {             /* LEFT / RIGHT / CENTER */
            if (prev != j) pending = j;           /* one-shot on the press edge */
        }
        prev = j;

        /* DISPLAY-PACED EMIT — the clever bit. Only release a press once the panel
         * has caught up (both buffers free). This makes the FIRST press instant
         * (panel idle → emit this tick), COALESCES a mash during a ~0.7 s refresh
         * into a single move (pending just gets overwritten), and THROTTLES a held
         * key to the panel's real refresh rate. No fixed timeout, no queue pile-up,
         * no ghost presses — response time self-tunes to the display. A press made
         * mid-refresh is remembered (survives release) so deliberate taps still
         * count; only repeats/mashes are thrown out. */
        if (pending != INPUT_NONE && !ui_display_busy()) {
            xQueueSend(g_input_q, &pending, 0);
            last_rep = t;
            pending = INPUT_NONE;
        }

#if INPUT_USE_IRQ
        /* Held → wake again in one repeat interval; released/idle → sleep until
         * the next edge interrupt (no polling, no wasted power). */
        wait = (j != INPUT_NONE || pending != INPUT_NONE)
                   ? pdMS_TO_TICKS(INPUT_REPEAT_RATE_MS) : portMAX_DELAY;
#endif
    }
}

/* ----------------------------------------------------------------------------
 *  Housekeeping task (core 0, lowest prio) — samples sensors, publishes snapshot
 * ------------------------------------------------------------------------- */
static void hk_task(void *arg) {
    (void)arg;
    for (;;) {
        hk_sample();                    /* main.c: read accel/steps/battery, publish snapshot */
        vTaskDelay(pdMS_TO_TICKS(50));  /* ~20 Hz; battery is sub-sampled inside hk_sample */
    }
}

/* ----------------------------------------------------------------------------
 *  Start everything — called from the UI task (app_task) once it is running
 * ------------------------------------------------------------------------- */
void rtos_tasks_start(void) {
    /* IPC objects first, before any task that uses them can run. */
    g_input_q     = xQueueCreate(8, sizeof(uint8_t));
    g_snap_mtx    = xSemaphoreCreateMutex();
    g_free_q      = xQueueCreate(2, sizeof(int));
    g_render_q    = xQueueCreate(2, sizeof(int));
    g_flash_ready = xSemaphoreCreateBinary();

    /* Seed the free list with both buffer indices (0 and 1). */
    int b0 = 0, b1 = 1;
    xQueueSend(g_free_q, &b0, 0);
    xQueueSend(g_free_q, &b1, 0);

    /* Display on core 1 (alone); Input + Housekeeping on core 0 with the UI. */
    xTaskCreate(display_task, "disp",  DISPLAY_TASK_STACK, NULL, DISPLAY_TASK_PRIO, &g_disp_task);
    vTaskCoreAffinitySet(g_disp_task, CORE1_AFFINITY);

    xTaskCreate(input_task,   "input", INPUT_TASK_STACK,   NULL, INPUT_TASK_PRIO,   &g_input_task);
    vTaskCoreAffinitySet(g_input_task, CORE0_AFFINITY);

    xTaskCreate(hk_task,      "hk",    HK_TASK_STACK,      NULL, HK_TASK_PRIO,      &g_hk_task);
    vTaskCoreAffinitySet(g_hk_task, CORE0_AFFINITY);
}
