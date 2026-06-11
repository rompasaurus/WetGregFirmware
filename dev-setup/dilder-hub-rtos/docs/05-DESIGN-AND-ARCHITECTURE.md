# 05 — Design and Architecture

This is the "why it's built this way" document. It describes the target task model, the
dual-core (SMP) decision, how a frame reaches the screen, and what is implemented now
versus planned.

---

## 1. The goal, restated as a requirement

> While the e-ink screen is mid-refresh (~300 ms of blocking), the firmware must still:
> register button presses within ~10 ms, keep the clock ticking, read the accelerometer,
> and service Wi-Fi/Bluetooth.

The original super-loop cannot meet this: one slow step stalls all the others. The fix is
concurrency, and we chose **dual-core SMP FreeRTOS**.

---

## 2. Why dual-core SMP (and the trade-off we accepted)

The RP2350 has two Cortex-M33 cores. We could have used single-core FreeRTOS (time-slicing
one core) — simpler and lower-risk. We chose **SMP** because it gives **true parallelism**:
the slow Display task runs on **core 1** while everything responsive runs on **core 0**, so
the 300 ms refresh has *zero* effect on input latency rather than merely being interrupted
every few milliseconds.

The trade-off we accept for that: SMP means two cores share memory simultaneously, so we
must be disciplined about synchronization (doc 06) and use multicore-safe flash writes
(doc 04 §8). The most error-prone combination on this chip is *cyw43 (Wi-Fi/BT) + SMP*, so
we **pin all networking to core 0** and keep core 1 doing nothing but the display. That
isolation keeps the fragile parts on one core and the parallelism where we need it.

(For the full pros/cons that led to this choice, see the project plan; this doc records the
decision, not the debate.)

---

## 3. The tasks

| Task | Core | Priority | Owns (only this task touches it) | Blocks on |
|---|---|---|---|---|
| **Input** | 0 | highest | the joystick GPIOs | a 5 ms `vTaskDelay` poll |
| **cyw43 / lwIP / BTstack** (SDK-created) | 0 | high | the Wi-Fi/BT chip, the network stack | internal events |
| **UI / Logic** | 0 | medium | the state machine, the logical canvas `frame[]`, canvas size/rotation, mood/quote/menu state | the input queue + a render-done signal |
| **Housekeeping** | 0 | low | the I²C bus + accelerometer, step/activity counters, the battery ADC, idle detection, the clock | a periodic `vTaskDelay` |
| **Display** | **1 (alone)** | high | the panel SPI bus + `display_buf[]` + the e-ink driver | a render request, then the BUSY pin |

The single most important line in that table: **Display is alone on core 1.** Everything
else is on core 0. That's the whole architecture in one sentence.

> One task **owning** a resource is our main concurrency tool. If only Housekeeping ever
> touches the I²C bus, there can be no I²C race — no lock needed. If only Display touches
> `display_buf`, no framebuffer race. We design ownership first and reach for locks only
> for the few things that genuinely cross tasks.

---

## 4. How a frame reaches the screen (the render hand-off)

This is the crux that lets the 300 ms cost live entirely on core 1.

```
  UI task (core 0)                         Display task (core 1)
  ----------------                         ---------------------
  1. decide what to show (state machine)
  2. draw into the logical canvas frame[]
  3. map frame[] → panel bytes via
     transpose_to_display() into the
     BACK buffer of a double buffer
  4. xQueueOverwrite(render_q, {buf, mode}) ───────►  5. xQueueReceive(render_q, …)  (was asleep)
  6. go back to waiting for input  ◄── (free!)        6. EPD push pixels from that buffer
                                                       7. wait on BUSY ~300 ms
                                                       8. notify UI "done" (frees the buffer)
```

- **Double buffer:** two `display_buf`s. UI fills the back one while Display shows the
  front one. They never touch the same buffer at the same time, so no lock on the pixels.
- **`xQueueOverwrite` (length-1 queue):** if UI produces frames faster than Display can
  show them, only the **newest** survives in the queue. That's exactly right — e-ink can't
  display intermediate frames anyway, so dropping them is free frame-coalescing.
- **Result:** after step 4, UI is *immediately* back to serving input. The screen catches
  up on the other core. Buttons never wait for pixels.

---

## 5. Synchronization at a glance (full detail in doc 06)

- **Input → UI:** a FreeRTOS **queue** of joystick codes. Replaces the old inline
  `read_joystick()` polling and the `POLL_INPUT` busy-loops.
- **UI → Display:** a length-1 **overwrite queue** carrying a tiny "show buffer N"
  descriptor, plus a **task notification** back to UI when the frame is done.
- **Housekeeping → readers:** a `sensor_snapshot_t` struct (orientation, steps, battery,
  idle flag) guarded by **one mutex**. Housekeeping writes it; UI/Display copy it.
- **Networking flags:** single `volatile` scalars (atomic); multi-byte network data is read
  under the cyw43 lock (`cyw43_arch_lwip_begin/end`).
- **Flash writes:** `flash_safe_execute()` (multicore-safe), not the old interrupt-disable.

---

## 6. Memory budget (rough)

RP2350 has 520 KB RAM. Approximate static consumers: FreeRTOS heap pool 128 KB, lwIP +
BTstack buffers, the framebuffers (~8 KB), task stacks (UI ~16 KB, others smaller), plus
the firmware's own globals. The Phase 0 build measured ~205 KB of `.bss` (zero-initialized
RAM) — comfortably within budget. Flash use (~600 KB of ~4 MB) is a non-issue.

---

## 7. The boot sequence

```
main() on the boot core, BEFORE any RTOS:
  stdio, clock-from-compile-time, hardware init (SPI/e-ink pins),
  joystick, speaker, [hold-UP → jump to the OTA bootloader], startup chime,
  EPD_Init/Clear, seed RNG
  → xTaskCreate(app_task, core 0); vTaskStartScheduler()   ← OS takes over here

app_task (Phase 1: the whole program; Phase 2: replaced by the task set):
  cyw43_arch_init()  ← must be here: the FreeRTOS Wi-Fi driver needs the scheduler running
  battery_init, mpu_init, load saved settings
  → the state-machine loop
```

The **OTA hold-UP check stays in `main()` before the scheduler** on purpose: jumping into
the bootloader must happen on bare metal, with no tasks alive.

---

## 8. Implemented vs planned

| Piece | Status |
|---|---|
| FreeRTOS SMP integrated, builds USB + OTA | ✅ Phase 0 |
| Whole program runs as one task under the scheduler (core 0) | ✅ Phase 1 |
| Split into Input / UI / Display / Housekeeping tasks | ✅ Phase 2 (`rtos_tasks.c`) |
| Display pinned to core 1 + 2-buffer render hand-off | ✅ Phase 2 |
| Input task + event queue (edge + auto-repeat) | ✅ Phase 2 |
| Housekeeping task + mutex-guarded sensor snapshot | ✅ Phase 2 |
| `flash_safe_execute()` for settings/calibration writes (+ core-1 lockout handshake) | ✅ Phase 2 |
| Verified by multi-agent adversarial concurrency review | ✅ Phase 2 |
| BUSY-pin interrupt → Display blocks instead of polling | 🔜 Phase 3 (marginal: the e-ink wait already yields core 1 cooperatively) |
| Stack/priority tuning via `uxTaskGetStackHighWaterMark` | 🔜 Phase 3 (needs on-device measurement) |

Phase 2 is implemented and builds clean (0 warnings) for USB + OTA. An adversarial review
(3 diverse-lens reviewers + synthesis) confirmed the concurrency core — the 2-buffer hand-off,
the snapshot mutex, the `POLL_INPUT` rewrite, and the `flash_safe_execute` mechanism — is
correct; the review's polish findings (flash handshake, step double-count, menu repeat fold,
volatiles) were fixed. The remaining Phase 3 items are hardening that is either marginal (the
display already yields core 1) or requires the physical board (stack high-water marks).
On-device validation of input latency, BLE pairing, and OTA is still owed by the hardware.
