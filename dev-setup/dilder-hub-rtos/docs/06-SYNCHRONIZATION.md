# 06 — Synchronization: Queues, Mutexes, Notifications

When two tasks (especially on two cores) share data, you need rules so they don't corrupt
each other. This doc explains the three tools we use, the bugs they prevent, and the bugs
*they* can cause if misused. It pairs with doc 04 (memory) and doc 05 (architecture).

> **Status:** the patterns here are the Phase 2/3 design. Phase 0/1 is still a single task,
> so it needs none of this yet. This doc is the blueprint we implement against.

---

## 1. The bug we're preventing: the race condition

```c
// Sensor task (core 0, low prio)          // UI task (core 0/1)
steps_today = steps_today + 1;             int shown = steps_today;
```

`steps_today + 1` is really *read, add, write*. If the UI reads between the sensor task's
read and write — or worse, on the other core *simultaneously* — it can see a half-updated
value, or an update can be lost. With multi-byte data (a struct, a string) it's worse: the
reader can see a mix of old and new bytes. This class of bug is **non-deterministic**
(works 999 times, corrupts once), which makes it the hardest kind to debug. We design it
out rather than hunt it down.

---

## 2. Tool 1 — the Queue (for passing data between tasks)

A **queue** is a thread-safe FIFO. `xQueueSend` puts an item in; `xQueueReceive` takes one
out. Both are **atomic** and may **block**: a receiver can sleep until an item arrives; a
sender can sleep until there's room. Copies go in and out *by value*, so the sender and
receiver never share the same memory — no race possible.

**We use a queue for Input → UI:**

```c
QueueHandle_t input_q = xQueueCreate(8, sizeof(uint8_t)); // up to 8 button codes

// Input task:
uint8_t code = read_joystick();
if (code != INPUT_NONE) xQueueSend(input_q, &code, 0);   // 0 = don't block if full

// UI task:
uint8_t code;
if (xQueueReceive(input_q, &code, pdMS_TO_TICKS(3000))) { /* handle */ }
else { /* 3 s passed with no input → do the periodic redraw */ }
```

The receive **timeout** elegantly replaces the old `POLL_INPUT(3000)` busy-loop: "wake me
on a press, or after 3 s, whichever first." No CPU burned waiting.

> JS analogy: a queue is like an async generator / channel — `await queue.next()` sleeps
> until a value is pushed.

---

## 3. Tool 2 — the Mutex (for protecting shared state)

A **mutex** ("mutual exclusion") is a lock exactly one task can hold at a time. Take it,
touch the shared data, give it back. While you hold it, anyone else trying to take it
**blocks** until you release.

**We use a mutex for the sensor snapshot** — a struct the Housekeeping task writes and the
UI/Display tasks read:

```c
typedef struct {
    int16_t  ax, ay, az;
    uint32_t steps_today;
    int      orientation;
    bool     tall;
    float    battery_v;
    bool     screen_idle;
} sensor_snapshot_t;

static sensor_snapshot_t g_snap;
static SemaphoreHandle_t g_snap_mtx;   // created with xSemaphoreCreateMutex()

// Housekeeping (writer):
xSemaphoreTake(g_snap_mtx, portMAX_DELAY);
g_snap.steps_today = steps_today;  g_snap.orientation = o;  /* … */
xSemaphoreGive(g_snap_mtx);

// UI (reader): copy the WHOLE struct out under the lock, then use the copy freely
sensor_snapshot_t local;
xSemaphoreTake(g_snap_mtx, portMAX_DELAY);
local = g_snap;
xSemaphoreGive(g_snap_mtx);
```

Two rules we follow: **hold the lock for as short a time as possible** (copy in/out, do the
real work unlocked), and **never block (e.g. never call the e-ink driver) while holding a
lock**.

> Why not just mark the struct `volatile`? Because it's multi-byte — a reader could catch
> it half-written (doc 04 §6). `volatile` only saves single 32-bit values. A struct needs a
> mutex.

---

## 4. Tool 3 — the Task Notification (the lightest poke)

A **task notification** is a per-task flag/counter you can set to wake exactly one task. It
is faster and cheaper than a queue or semaphore and is perfect for "this specific task,
wake up." Crucially, there's a `...FromISR` form callable from an interrupt.

**We use it for the e-ink BUSY interrupt (Phase 3):**

```c
// ISR: BUSY pin went low (panel finished)
void busy_isr(uint gpio, uint32_t events) {
    BaseType_t woke = pdFALSE;
    vTaskNotifyGiveFromISR(g_display_task, &woke);
    portYIELD_FROM_ISR(woke);   // if a higher-prio task woke, switch to it now
}

// Display task: instead of polling BUSY for 300 ms, just sleep until poked
ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(500));  // wakes on the ISR (or 500 ms safety timeout)
```

That turns 300 ms of `while(busy) delay(10)` polling into a real sleep — core 1 idles
(saving power) until the hardware says "done."

We also use a notification for **Display → UI "frame done"** so UI knows a buffer is free
again.

---

## 5. The two classic mutex bugs (and how we avoid them)

### Deadlock
Two tasks each hold one lock and wait for the other's — both stuck forever. Avoidance: we
have essentially **one** data mutex (the snapshot), and we never take a second lock while
holding it. With a single lock, deadlock is impossible.

### Priority inversion
A high-priority task waits for a lock held by a low-priority task, which itself never gets
to run (a medium task hogs the CPU) — so the high task is stuck behind the low one.
FreeRTOS mutexes implement **priority inheritance** (the low holder is temporarily boosted
to the waiter's priority) which prevents the classic form. We keep critical sections tiny so
even bounded inversion is negligible.

---

## 6. The cyw43 / networking lock

The Wi-Fi/Bluetooth chip and the lwIP stack have their **own** lock, taken for us by the
SDK helpers `cyw43_arch_lwip_begin()` / `cyw43_arch_lwip_end()`. Any code outside the
network task that calls into lwIP (we have very little — just kicking off NTP or a scan)
must wrap the call in that pair. The battery ADC also uses the cyw43 lock
(`cyw43_thread_enter/exit`) because its pin (GPIO29) is physically shared with the Wi-Fi
chip's SPI — under SMP that lock is now genuinely required, not just polite.

---

## 7. Summary: which tool for what

| Need | Tool | Example here |
|---|---|---|
| Pass events/data between tasks | **Queue** | Input → UI button codes |
| Coalesce to "latest only" | **length-1 overwrite queue** | UI → Display render request |
| Protect a shared struct/array | **Mutex** | the sensor snapshot |
| Wake one specific task (incl. from an ISR) | **Task notification** | BUSY-pin → Display; Display → UI "done" |
| Share one 32-bit flag | **`volatile`** (no lock) | `wifi_connected` |
| Touch Wi-Fi/lwIP from another task | **cyw43 lock** | NTP/scan kickoff, battery ADC |
| Write flash safely on two cores | **`flash_safe_execute()`** | save settings/calibration |

Design principle threaded through all of it: **single ownership first, locks only where
data genuinely crosses tasks, and keep every critical section tiny.**
