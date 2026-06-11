# 08 — Function Reference

A guided tour of the important functions and where they live. Because Phase 0/1 still keeps
the logic in one `main.c`, this reference is organized by **subsystem within `main.c`**
plus the new RTOS files. After **Phase 2** splits the monolith into modules, each module
gets its own section here with full per-function detail.

For each entry: **what it does · which task/context calls it · blocking?**

---

## A. RTOS entry points and lifecycle (the new code)

### `main()` — `main.c`
The bare-metal entry, runs on the boot core **before** the scheduler. Does the init that
must precede the OS (stdio, software clock seed, `DEV_Module_Init` for the e-ink pins,
joystick, speaker, the **OTA hold-UP check**, startup chime, `EPD_Init`/`EPD_Clear`, RNG
seed), then `xTaskCreate(app_task, …, core 0)` + `vTaskStartScheduler()`. **Never returns.**
· *Context:* boot core, pre-scheduler · *Blocking:* yes, but nothing else exists yet.

### `app_task(void *param)` — `main.c`
The Phase-1 single application task: contains `cyw43_arch_init()` (must run under the
scheduler), `battery_init`, `mpu_init`, `saved_load`, and the entire state-machine
`while(true)`. Pinned to **core 0**. Phase 2 replaces this with the Input/UI/Display/
Housekeeping task set. · *Context:* its own FreeRTOS task · *Blocking:* loops forever; the
e-ink calls inside it block ~300 ms (the very thing Phase 2 moves to core 1).

### `vApplicationMallocFailedHook()` / `vApplicationStackOverflowHook()` — `freertos_hooks.c`
Kernel error callbacks: print and halt on out-of-memory or a task stack overrun. · *Context:*
called by the kernel from any task/ISR · *Blocking:* halts on purpose.

### `vApplicationGetIdleTaskMemory()` / `…PassiveIdleTaskMemory()` / `…TimerTaskMemory()` — `freertos_hooks.c`
Hand the kernel static RAM for its internal idle (one per core) and timer-daemon tasks,
because we use static allocation. Pure boilerplate. · *Context:* kernel startup.

---

## B. Display subsystem — `main.c` + `lib/e-Paper/` + `lib/Config/`

### `EPD_Init()`, `EPD_Clear()`, `EPD_Partial(buf)` — `lib/e-Paper/EPD_2in13_V4.c`
Initialize the panel, clear it, and do a partial refresh from a framebuffer. `EPD_Partial`
is the **~300 ms blocking call** at the heart of the responsiveness problem (it internally
calls `ReadBusy`). · *Context:* today `app_task`; Phase 2 → **Display task on core 1** ·
*Blocking:* ~300 ms.

### `EPD_2in13_V4_ReadBusy()` — `lib/e-Paper/EPD_2in13_V4.c`
Waits for the panel by polling the BUSY pin every 10 ms (`DEV_Delay_ms(10)`). **Phase 3
replaces this with a BUSY-pin interrupt + task notification** so the Display task truly
sleeps instead of polling. · *Blocking:* ~300 ms.

### `DEV_Delay_ms()`, `DEV_Digital_Read()`, SPI transfer — `lib/Config/DEV_Config.c`
The hardware-abstraction layer between the panel driver and the chip's pins. Under FreeRTOS,
`DEV_Delay_ms` yields to the scheduler (via the Pico time-interop in `FreeRTOSConfig.h`)
rather than busy-spinning. · *Context:* Display path.

### `render_frame()`, `render_octopus_tall()`, `draw_text()`, `draw_*` — `main.c`
Build the octopus + quote + status bar into the logical canvas `frame[]`. Many small drawing
helpers; the deepest call chain in the firmware (hence the UI task's large stack).
`transpose_to_display()` maps the canvas onto the panel's pixel layout in `display_buf`. ·
*Context:* today `app_task`; Phase 2 → **UI task** (then hands `display_buf` to Display).

---

## C. Input subsystem — `main.c`

### `read_joystick()` — `main.c`
Reads the 5 joystick GPIOs (active-low), applies orientation rotation, returns an
`INPUT_*` code. · *Context:* today inline in the loop; Phase 2 → **Input task**, which sends
the code into the input queue. · *Blocking:* no.

### `POLL_INPUT(ms)` / `POLL_END` macros — `main.c`
The current busy-poll loop used by sub-screens (sleep 5 ms, read joystick, repeat for `ms`).
Phase 2 retires these in favour of `xQueueReceive` with a timeout. · *Blocking:* spins for
up to `ms`.

---

## D. Sensors / housekeeping — `main.c`

### `mpu_init()`, `mpu_read_all()` — `main.c`
Set up and read the SC7A20 accelerometer over I²C (address 0x18). `mpu_read_all` fetches a
6-byte X/Y/Z burst. · *Context:* today the loop; Phase 2 → **Housekeeping task** (sole I²C
owner). · *Blocking:* sub-millisecond I²C.

### `orientation_update()` — `main.c`
Reads the accel, runs the pedometer + activity tally, and classifies the hold into one of
three orientations (auto-rotate). Throttled to ~4 Hz. · *Context:* Housekeeping (Phase 2).

### `pedometer_update()`, `activity_update()`, `viewing_update()` — `main.c`
Step detection, daily step/active-time accrual, and pocket/idle detection (freeze redraws
when not viewed). · *Context:* Housekeeping (Phase 2).

### `read_vsys_raw_volts()`, `read_vsys_volts()`, `read_battery_percent()` — `main.c`
Battery voltage via ADC on GPIO29 (shared with cyw43 SPI, so wrapped in
`cyw43_thread_enter/exit`), trimmed-mean filtered, scaled by the persisted calibration, and
mapped to a percentage. · *Context:* Housekeeping. · *Blocking:* brief ADC.

---

## E. Persistence (flash) — `main.c`

### `saved_load()`, `saved_write_flash()`, `battery_cal_save()` — `main.c`
Load/save Wi-Fi networks + battery calibration in the last flash sector. **`saved_write_flash`
currently uses `save_and_disable_interrupts` — Phase 3 switches it to `flash_safe_execute()`
for SMP safety** (doc 04 §8). · *Context:* UI/Housekeeping. · *Blocking:* ~tens of ms; stalls
the other core during the write.

---

## F. Networking — `main.c` + `bt.c`

### `cyw43_arch_init()` — SDK, called in `app_task`
Brings up the Wi-Fi/Bluetooth chip **and** (in the FreeRTOS variant) starts the networking
background task. Must run with the scheduler active. · *Blocking:* brief.

### `wifi_connect()`, scan, `ntp_request()` and callbacks — `main.c`
Join a network, scan, and sync the clock over NTP. Network callbacks run in the cyw43 task;
other tasks calling lwIP must hold the cyw43 lock. · *Context:* cyw43 task + UI triggers.

### `dilder_bt_init()`, `dilder_bt_*()`, GATT handlers — `bt.c`
The BLE peripheral: advertise, passkey-pair, and expose mood/steps to a phone. Runs on
BTstack inside the cyw43 task. · *Context:* cyw43 task.

---

## G. App state machine — `main.c`

The `switch (state)` over `STATE_OCTOPUS`, `STATE_MENU`, `STATE_INFO`, `STATE_NETWORK`,
`STATE_BLUETOOTH`, `STATE_SET_TIME`, etc. Each case renders a screen and handles its input.
In Phase 2 this becomes the **UI task**'s core: it consumes input from the queue, updates
state, and dispatches render requests to the Display task. · *Context:* today `app_task`;
Phase 2 → UI task.

---

## What changes after Phase 2

The functions don't disappear — they **move** into focused files:

- `src/rtos/` — `task_input.c`, `task_ui.c`, `task_display.c`, `task_housekeeping.c`,
  `sync.c/.h` (queues, the snapshot + mutex, task handles/priorities/affinity).
- `src/drivers/` — `display_epd.*`, `joystick.*`, `accel_sc7a20.*`, `battery.*`,
  `speaker.*`, `flash_store.*`.
- `src/app/` — `octopus_render.*`, `ui_screens.*`, `moods.*`, `clock_rtc.*`.
- `src/net/` — `wifi.*`, `ntp.*`, `ble.*`.

This reference will then expand to a full per-function entry for each module.
