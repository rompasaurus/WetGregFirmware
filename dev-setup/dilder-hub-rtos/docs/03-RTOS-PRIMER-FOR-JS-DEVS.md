# 03 — RTOS Primer for JavaScript Developers

This doc teaches the operating-system concepts behind the firmware from zero, using
JavaScript as the reference point. No prior C or embedded experience assumed. Read it
before the code; the comments in the code assume you've met these ideas.

---

## 1. The problem an RTOS solves

A microcontroller has one (or here, two) CPU cores and **no operating system by default**.
The simplest firmware is a "super-loop":

```c
while (true) {
    read_buttons();
    update_screen();   // <-- takes 300 ms on e-ink!
    read_sensors();
    service_wifi();
}
```

The trouble: `update_screen()` blocks for ~300 ms. During those 300 ms, `read_buttons()`
cannot run, so a press is missed. In JavaScript you'd never write a 300 ms **synchronous**
call on the main thread — it would freeze the page. The browser's event loop saves you by
making slow things (`fetch`, timers) asynchronous. A bare microcontroller has no event
loop. **The RTOS gives us one** (and more).

---

## 2. Tasks = independent loops the OS runs for you

A **task** is a C function that looks like its own little program with its own
`while(true)`:

```c
void input_task(void *arg) {
    while (true) {
        uint8_t button = read_joystick();
        if (button) xQueueSend(button_queue, &button, 0);
        vTaskDelay(pdMS_TO_TICKS(5));   // sleep 5 ms, letting others run
    }
}
```

We write several such tasks and hand them to FreeRTOS. The **scheduler** then runs them,
switching between them so fast it looks simultaneous (and on two cores, some genuinely
*are* simultaneous).

| JavaScript | FreeRTOS |
|---|---|
| You write `async` functions; the event loop interleaves them | You write tasks; the scheduler interleaves them |
| `await something` lets other callbacks run | `vTaskDelay(...)` or waiting on a queue lets other tasks run |
| Single thread — never truly parallel | Up to 2 tasks truly parallel (one per core) |

**Key difference:** in JS, code only yields at `await`. In a *preemptive* RTOS (ours), the
scheduler can **interrupt** a task at any moment to run a more important one. You don't
have to cooperate — the OS forces the switch. That's what makes buttons feel instant: the
high-priority input task **preempts** whatever was running.

---

## 3. Priorities — who gets the CPU

Every task has a **priority** number (higher = more important). The scheduler's rule is
simple: *always run the highest-priority task that is ready to run.* If a high-priority
task wakes up, it preempts a lower one immediately.

Our priorities (highest to lowest):

1. **Input** — must feel instant, so it's top. It runs for microseconds then sleeps.
2. **cyw43/networking** — Wi-Fi/Bluetooth servicing.
3. **UI/logic** — decides what to show.
4. **Housekeeping** — sensors, clock, battery; can wait a few ms.

There is no JS analogy for priorities — the event loop is first-come-first-served. Here
we explicitly rank importance.

> **Gotcha:** a high-priority task that never sleeps would **starve** everything below it
> (it never gives up the CPU). So high-priority tasks must do a little work and then block
> (sleep, or wait on a queue). Our Input task polls for ~microseconds then `vTaskDelay`s.

---

## 4. "Blocking" is good here (unlike JS)

In JS, "blocking" is a dirty word — a blocking call freezes everything. In an RTOS,
**blocking a task is the normal, efficient thing**: when a task calls `vTaskDelay()` or
waits on a queue, it tells the scheduler "I have nothing to do — run someone else." The
core is handed to another task (or sleeps to save power). The task is *blocked*, but the
*system* keeps going.

So `xQueueReceive(queue, &msg, portMAX_DELAY)` means "sleep until a message arrives." It's
like `await queue.next()` — it doesn't burn CPU while waiting.

---

## 5. Two cores: SMP

The RP2350 has **two** CPU cores. We run FreeRTOS in **SMP** mode (Symmetric
Multi-Processing): one scheduler manages both cores and can run **two tasks at the exact
same instant**, one per core.

We use this deliberately: the slow **Display** task is **pinned to core 1** (it's allowed
to run *only* there). So when it blocks for 300 ms waiting on the e-ink panel, **core 0 is
completely free** to run Input, UI, sensors, and Wi-Fi. True parallelism, not just fast
switching.

```
        core 0                         core 1
   ┌───────────────┐              ┌───────────────┐
   │ Input         │              │ Display       │
   │ UI/logic      │   (parallel) │  (e-ink:      │
   │ Housekeeping  │              │   ~300 ms     │
   │ Wi-Fi/BT      │              │   blocking)   │
   └───────────────┘              └───────────────┘
```

JavaScript only got *real* parallelism with Web Workers (separate threads that don't share
memory). SMP is more intimate: both cores **share the same memory**, which is powerful but
introduces a hazard JS never has — see the next point.

---

## 6. The price of two cores: race conditions

Because both cores share memory, two tasks can touch the **same variable at the same
time**. Example: the sensor task writes `steps_today` while the UI task reads it. If the
write is halfway done when the read happens, the reader can see garbage. This is a **race
condition**, and it cannot happen in single-threaded JS.

We prevent it with synchronization tools (full detail in **doc 06**):

- **Queue** — a thread-safe FIFO. The only way data crosses from Input → UI. Sending and
  receiving are atomic; no corruption possible.
- **Mutex** ("mutual exclusion") — a lock. A task `takes` it before touching shared data
  and `gives` it after. While held, no other task can take it, so only one toucher at a
  time.
- **Task notification** — the lightest possible "poke" to wake a specific task (used by
  the screen-busy interrupt to wake the Display task).

> **Design rule we follow to minimize locks:** give each piece of shared state a single
> **owner** task. Only that task writes it; others get a copy through a queue or a
> mutex-guarded snapshot. Fewer shared variables = fewer races.

---

## 7. The tick: the OS heartbeat

FreeRTOS keeps time with a periodic timer interrupt called the **tick**. We set it to
**1000 Hz** (one tick per millisecond) in `FreeRTOSConfig.h`. Every tick the scheduler
wakes any task whose `vTaskDelay()` has expired and decides who should run. `vTaskDelay
(pdMS_TO_TICKS(5))` = "wake me in 5 ticks = 5 ms."

---

## 8. Interrupts vs tasks

One more concept C/firmware has that JS doesn't: **hardware interrupts (ISRs)**. When
hardware needs attention (a pin changes, a timer fires), the CPU **immediately** jumps to a
tiny **Interrupt Service Routine**, no matter what task was running. ISRs must be *fast*
and may only call special `...FromISR` kernel functions.

We use this in Phase 3: the e-ink BUSY pin going low triggers an ISR that does one thing —
`vTaskNotifyGiveFromISR(display_task)` — which wakes the Display task. That replaces the
300 ms of polling with a true "sleep until the hardware says it's done."

---

## 9. Putting it together: the life of a button press

1. You press **DOWN**. The **Input task** (highest priority) is sleeping; within ≤5 ms it
   wakes, reads the joystick, and `xQueueSend`s the code `INPUT_DOWN` to the input queue.
2. The **UI task** was blocked on `xQueueReceive(input_queue, …)`. The send wakes it; the
   scheduler runs it (on core 0). It updates the menu selection.
3. UI renders the new frame into a buffer and `xQueueOverwrite`s a "please show this" note
   to the **Display task** on **core 1**.
4. UI goes back to waiting for input — **immediately responsive again**.
5. Meanwhile the Display task, on the *other* core, spends ~300 ms pushing pixels and
   waiting on BUSY. It does not affect steps 1–4 at all.

That choreography — impossible in the old super-loop — is the entire point of this
firmware. The concrete code is in **doc 05** (architecture) and **doc 06**
(synchronization).
