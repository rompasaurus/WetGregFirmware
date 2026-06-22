# WetGreg Firmware — Architecture, Organization & Development Guide

> **Audience:** a developer who is new to this project — and possibly new to C,
> microcontrollers, and real-time operating systems. This guide is the single
> "how the firmware is built, why it's built that way, how to work on it well,
> and how to test it before it ever touches hardware" reference.
>
> **Relationship to the other docs:** the firmware folder already ships a deep
> 12-part teaching series (`dev-setup/wetgreg-hub-rtos/docs/00`…`11`). This guide
> is the **map and the connective tissue** over all of it: it summarizes each
> area, adds the parts that weren't written down yet (most importantly a
> **testing & virtual-validation system**), and points you to the deep dive when
> you want more. Where a section says *"→ see doc NN"*, that is the authoritative
> long-form treatment.

---

## Table of contents

1. [How to use this guide](#1-how-to-use-this-guide)
2. [The project in one page (what it is, and its inspiration)](#2-the-project-in-one-page)
3. [Feature & functionality summary (what's in place today)](#3-feature--functionality-summary)
4. [Repository architecture & file-structure organization](#4-repository-architecture--file-structure-organization)
5. [Firmware architecture deep-dive](#5-firmware-architecture-deep-dive)
6. [Design guidelines & organization principles](#6-design-guidelines--organization-principles)
7. [The rules of C firmware development](#7-the-rules-of-c-firmware-development)
8. [Coding fundamentals & house conventions](#8-coding-fundamentals--house-conventions)
9. [A testing & validation system (virtual-first, then hardware)](#9-a-testing--validation-system)
10. [Development workflow & checklists](#10-development-workflow--checklists)
11. [Development resources & extended learning (curated, justified)](#11-development-resources--extended-learning)
12. [Glossary](#12-glossary)

---

## 1. How to use this guide

There are three honest ways to read this, depending on why you opened it:

| If you are… | Read… |
|---|---|
| **Brand new** and want to *understand* before touching anything | §2 → §3 → §4 → §5, then the firmware's `docs/00`–`06` in order |
| About to **make a change** | §6 (design rules) → §8 (conventions) → §10 (workflow) → §9 (test it) |
| Setting up **testing / CI** | §9 in full, then §11's "Testing & quality tooling" |
| Just **building/flashing** | `README.md` Quick start + `tools/devtool/DEVTOOL_GUIDE.md` |

**The single most important mental model in this whole codebase** (everything
else follows from it):

> The e-ink screen takes **~300 ms** to refresh and *blocks* while it does. In
> the original single-loop firmware, that 300 ms froze buttons, sensors, and
> the clock. This firmware runs an **RTOS** so the slow screen refresh happens on
> **its own CPU core**, in isolation, while everything responsive keeps running
> on the other core. Buttons never wait for pixels.

If you keep that one sentence in your head, the task split, the core pinning,
the double-buffering, and most of the design decisions will feel inevitable
rather than arbitrary.

---

## 2. The project in one page

**WetGregFirmware** is the firmware (and the tooling to build/flash/debug it) for
the **WetGreg hub** — a handheld gadget that draws an animated octopus showing
moods and quotes, counts your steps, talks to a phone over Bluetooth LE, says
hello to nearby WetGregs, and can update itself over Wi-Fi.

### The hardware target (fixed, on purpose)

| Setting | Value |
|---|---|
| MCU board | **Raspberry Pi Pico 2 W** |
| Chip | **RP2350** — dual **Cortex-M33** cores, **520 KB** SRAM, ~4 MB external flash (XIP) |
| Display | **WeAct 2.13″ B&W e-ink**, **SSD1680** controller, ~250×122, **~300 ms** refresh |
| Display wiring | WetGreg PCB — **SPI0, GP17–22** |
| Radios | CYW43439 — **Wi-Fi** + **Bluetooth LE**, sharing one SPI bus to the host |
| Sensors / IO | SC7A20 accelerometer (I²C), 5-way joystick (GPIO), battery ADC (VSYS), piezo speaker (PWM) |

This repo is a **deliberately narrow** extraction of a larger WetGreg project: one
firmware target, one display variant, one board. "One target, on purpose" is a
design principle, not a limitation — see §6.

### The inspiration & the engineering thesis

The project exists to answer a concrete, teachable engineering question:

> *"A blocking call in a super-loop starves everything else. How do you keep a
> UI responsive when one unavoidable operation (an e-ink refresh) takes 300 ms?"*

The answer this codebase demonstrates is the classic embedded one: **stop using
a super-loop; use a real-time operating system and true multicore parallelism.**
The original `wetgreg-hub` was a single `while(1)` loop on one core; this RTOS
rewrite splits the work into cooperating **tasks** scheduled across **both** RP2350
cores by **FreeRTOS** in **SMP** (symmetric multiprocessing) mode.

The intellectual lineage — and the references that justify every non-obvious
choice — is documented in `docs/09-SOURCES-AND-FURTHER-READING.md`. The
headliners: *Mastering the FreeRTOS Real-Time Kernel* (Richard Barry), the
FreeRTOS SMP documentation, the Raspberry Pi Pico C/C++ SDK book + RP2350
datasheet, and Elecia White's *Making Embedded Systems*. §11 of this guide
extends that list toward C fundamentals and testing.

---

## 3. Feature & functionality summary

What the firmware actually *does* today (✅ = implemented & building, 🔜 =
designed/planned). The phased status is tracked in `docs/00-START-HERE.md` and
`docs/05-DESIGN-AND-ARCHITECTURE.md §8`.

### Application features

| Feature | What it does | Where it lives |
|---|---|---|
| **Animated octopus** ✅ | A pixel-art octopus with many drawn moods/emotions (angry, chill, excited, creepy, tired, hungry, lazy, unhinged, …) and per-mood eyes/pupils/brows/mouths, with idle wobble animation | `main.c` (the `draw_*` family) |
| **Mood quotes** ✅ | Hundreds of mood-tagged quote strings shown with the octopus | `quotes.h` (⚙️ generated by the DevTool) |
| **Pedometer / steps** ✅ | Counts steps and "active seconds" from the accelerometer; daily tallies | `pedometer_update`, `activity_update` in `main.c` |
| **Orientation awareness** ✅ | Reads the SC7A20 accelerometer; switches between tall/wide canvas layouts; "face-down" detection | `orientation_update`, `ORIENT_CFG`, `accel_*` |
| **Presence / screen-wake** ✅ | Detects recent motion to decide the screen is being viewed (saves e-ink wear) | `viewing_update`, `screen_is_viewed`, `wake_screen` |
| **Battery gauge** ✅ | Reads VSYS via ADC, calibrated %; detects USB power | `main.c` battery code; published in the sensor snapshot |
| **Menus & on-screen keyboard** ✅ | Joystick-driven menu state machine + a `kb_char_at` text-entry keyboard | `main.c` UI state machine |
| **Sound** ✅ | Piezo tones / startup chime / patterns via PWM | `speaker_init`, `speaker_tone`, `play_sound_pattern` |
| **Bluetooth LE peripheral** ✅ | Advertises as "WetGreg Hub", passkey pairing, a small custom GATT service a phone can read/command | `bt.c`, `wetgreg.gatt`, BTstack |
| **Social proximity hellos + emotes** ✅ | WetGreg-to-WetGreg "hello" over **connectionless BLE beacons**, a Social menu, names, a met-log | `docs/10-SOCIAL-AND-EMOTES.md`, `bt.c` |
| **Wi-Fi + NTP clock** ✅ | Joins Wi-Fi (lwIP, OS mode), syncs time over NTP | `lwipopts.h`, `wifi_config.h`, `main.c` |
| **OTA self-update** ✅ (optional build) | Wi-Fi over-the-air firmware update via the **picowota** bootloader | `picowota/` submodule, `-DPICOWOTA_OTA=ON` |

### Platform / RTOS features

| Capability | Status | Note |
|---|---|---|
| FreeRTOS **SMP** integrated, builds USB + OTA | ✅ Phase 0/1 | dual Cortex-M33, `RP2350_ARM_NTZ` port |
| Whole program runs under the scheduler | ✅ Phase 1 | started as one `app_task` on core 0 |
| Split into **Input / UI / Display / Housekeeping** tasks | ✅ Phase 2 | `rtos_tasks.c` |
| Display pinned **alone to core 1** + 2-buffer render hand-off | ✅ Phase 2 | the responsiveness payoff |
| Input task + event **queue** (edge + auto-repeat) | ✅ Phase 2 | presses captured within a few ms during a refresh |
| Housekeeping task + **mutex-guarded** sensor snapshot | ✅ Phase 2 | no cross-task sensor race |
| Multicore-safe flash writes (`flash_safe_execute` + core-1 lockout handshake) | ✅ Phase 2 | settings/calibration persistence |
| Verified by a **multi-agent adversarial concurrency review** | ✅ Phase 2 | findings fixed |
| BUSY-pin **interrupt** → Display blocks instead of polling | 🔜 Phase 3 | marginal; the e-ink wait already yields core 1 |
| Stack/priority tuning via `uxTaskGetStackHighWaterMark` | 🔜 Phase 3 | needs on-device measurement |

> **Honest status note (carried from `docs/00`):** the builds are verified and a
> concurrency review passed, but **on-device validation** (true input latency
> during a refresh, BLE pairing round-trips, the OTA cycle) still requires the
> physical board. That gap is exactly what §9's testing system is designed to
> shrink — push as much validation as possible *off* the bench.

---

## 4. Repository architecture & file-structure organization

### 4.1 The top-level map

```
WetGregFirmware/
├── README.md                     ← project overview + quick start
├── FIRMWARE_DEVELOPMENT_GUIDE.md ← THIS FILE (the map over everything)
├── install-deps.sh               ← one-shot toolchain installer (Arch/Debian/Fedora)
├── .gitmodules                   ← submodule pins (FreeRTOS-Kernel, picowota)
│
├── dev-setup/                    ← BUILD CONTEXT: firmware + Docker build infra
│   ├── wetgreg-hub-rtos/          ← ★ the firmware itself (the heart of the repo)
│   ├── Dockerfile                ← Ubuntu + ARM toolchain + pico-sdk image
│   ├── docker-compose.yml        ← the single build service
│   └── version.h                 ← shared firmware version string
│
├── FreeRTOS-Kernel/              ← submodule: the RTOS scheduler (pinned commit)
├── picowota/                     ← submodule: optional Wi-Fi OTA bootloader
│
├── tools/devtool/                ← the WetGreg DevTool (build/flash/debug GUI + CLI)
│   ├── devtool.py                ← single-file tool (GUI tabs + CLI subcommands)
│   ├── DEVTOOL_GUIDE.md          ← full user guide (rendered in the Docs tab)
│   └── requirements.txt          ← pyserial (serial monitor only)
│
└── assets/emotion-previews/      ← reference PNGs of the octopus's moods (design ref)
```

**Why it's shaped this way** (the organizing ideas):

1. **The firmware is self-contained.** `dev-setup/wetgreg-hub-rtos/` keeps its
   *own* copy of the display library, its *own* SDK/RTOS import shims, and its
   *own* config headers. You can reason about the firmware without chasing files
   across the repo, and the e-ink driver can be patched for this board without
   disturbing anything upstream.
2. **The build context is a directory, not your whole machine.** Everything
   Docker needs to compile lives under `dev-setup/`. The build runs *inside*
   Docker so your host never needs an exact SDK/toolchain version.
3. **Third-party code is pinned submodules, never vendored-and-forgotten.**
   `FreeRTOS-Kernel` and `picowota` are git submodules at fixed commits, for
   reproducible builds.
4. **Tooling is one file, two faces.** `devtool.py` is *both* a Tkinter GUI and a
   headless CLI sharing one implementation — so automation and humans run the
   exact same code path.

### 4.2 Inside the firmware (`dev-setup/wetgreg-hub-rtos/`)

The full per-file breakdown — including which files are hand-written vs
generated vs build output — is `docs/02-FILE-MANIFEST.md`. The essentials:

```
wetgreg-hub-rtos/
├── CMakeLists.txt              ✍️ build recipe: sources, libs, display variant, OTA option
├── FreeRTOSConfig.h            ✍️ ★ the RTOS settings (SMP, heap, tick, priorities) — read this first
├── main.c                      ✍️ ~5000 lines: state machine + UI + drivers + sensors + rendering
├── rtos_tasks.c / rtos_tasks.h ✍️ ★ the 4-task split + IPC contract (queues/mutex/notifications)
├── freertos_hooks.c            ✍️ kernel-required callbacks (idle/timer memory, error hooks)
├── bt.c / bt.h                 📦 Bluetooth LE peripheral + social beacons
├── wetgreg.gatt                 📦 GATT database source (compiled to wetgreg.h at build time)
├── lwipopts.h                  ✍️ TCP/IP stack options (NO_SYS=0 → lwIP runs under the OS)
├── btstack_config.h            📦 Bluetooth stack feature/buffer config
├── wifi_config.h               ✍️ Wi-Fi/NTP/timezone placeholders (NEVER commit real secrets)
├── quotes.h                    ⚙️ generated by the DevTool — never hand-edit
├── *_import.cmake              shims that locate the Pico SDK / FreeRTOS kernel
├── lib/                        📦 vendored display stack
│   ├── Config/  DEV_Config.*   ← ★ the HAL: SPI/GPIO pins, delays, RTC shim (the test seam, §9)
│   ├── e-Paper/                ← SSD1680 panel drivers (V2/V3/V3a/V4 — we use V4)
│   ├── GUI/     GUI_Paint.*    ← Waveshare paint helpers (mostly unused; we draw our own)
│   └── Fonts/                  ← bitmap fonts (mostly legacy; we use a compact 5×7 font)
├── docs/  00…11                ✍️ the deep teaching series
└── build/ , build-ota/         🏗️ compiler output — git-ignored, never edit/commit
```

**Legend:** ✍️ hand-written · 📦 carried-over/third-party · ⚙️ tool-generated · 🏗️ build output.

### 4.3 The two files a newcomer should open first

- **`FreeRTOSConfig.h`** — FreeRTOS is configured entirely at *compile time*.
  Every behavioral knob (2 cores, core affinity, the 1 kHz tick, the 128 KB
  heap_4 pool, priorities, the Cortex-M33 port options) is a `#define` here. If
  the RTOS misbehaves, look here first. It's commented line-by-line for newcomers.
- **`rtos_tasks.h`** — the *contract* between the RTOS plumbing and the
  application logic. It declares the four tasks, the IPC objects, and the API the
  UI calls (`display_render`, `ui_get_input`, `rtos_snapshot_get`). Reading this
  header tells you how the whole system fits together without reading 5000 lines.

---

## 5. Firmware architecture deep-dive

> Authoritative long-form: `docs/03` (RTOS primer), `docs/04` (memory/pointers),
> `docs/05` (design/architecture), `docs/06` (synchronization). This is the
> condensed, connected version.

### 5.1 The task model

Five concurrent tasks (four of ours + the SDK's networking task), each *owning* a
set of resources so that ownership — not locking — is the primary concurrency tool.

| Task | Core | Priority | Owns (only it touches) | Blocks on |
|---|---|---|---|---|
| **Input** | 0 | highest (3) | the joystick GPIOs | a 5 ms poll delay (or a GPIO IRQ in Phase 3a) |
| **cyw43 / lwIP / BTstack** | 0 | high | Wi-Fi/BT chip, network stack | internal events |
| **UI / Logic** | 0 | medium (2) | the state machine, the logical canvas `frame[]`, mood/menu state | the input queue + a render-done signal |
| **Housekeeping** | 0 | low (1) | I²C bus + accelerometer, step/battery counters, the clock | a periodic delay |
| **Display** | **1 (alone)** | high (2) | the panel SPI bus + `display_buf[]` + the e-ink driver | a render request, then the BUSY pin |

**The whole architecture in one line:** *Display is alone on core 1; everything
else is on core 0.* The fragile combination on this chip is "cyw43 (Wi-Fi/BT) +
SMP", so all networking stays pinned to core 0 and core 1 does nothing but push
pixels. That isolation keeps the risky parts together and the parallelism exactly
where the latency problem is.

### 5.2 How a frame reaches the screen (the render hand-off)

This is the crux that lets the 300 ms cost live entirely on core 1:

```
  UI task (core 0)                          Display task (core 1)
  ----------------                          ---------------------
  1. decide what to show (state machine)
  2. draw into the logical canvas frame[]
  3. transpose frame[] → panel bytes into
     the BACK buffer of a double buffer
  4. xQueueOverwrite(render_q,{buf,mode}) ───────►  5. xQueueReceive(render_q)  (was asleep)
  6. immediately back to serving input ◄──(free!)   6. push pixels to the panel
                                                     7. wait on BUSY ~300 ms
                                                     8. notify UI "done" (frees the buffer)
```

Three mechanisms make it correct *and* fast:

- **Double buffer** — two `display_buf`s. UI fills the back one while Display
  shows the front one; they never touch the same buffer at once, so **no lock on
  the pixels**.
- **`xQueueOverwrite` on a length-1 queue** — if UI produces frames faster than
  Display can show them, only the *newest* survives. E-ink can't display
  intermediate frames anyway, so this is free, correct frame-coalescing.
- **Task notification back to UI** — the buffer is returned only when the refresh
  truly finishes. No timing guesses.

After step 4, the UI task is *immediately* back to serving input. The screen
catches up on the other core. **Buttons never wait for pixels.**

### 5.3 Synchronization at a glance

The ownership-first model means very few locks. The ones that exist (full detail
in `docs/06`):

- **Input → UI:** a FreeRTOS **queue** of joystick event codes (replaces the old
  busy-poll). The UI *sleeps* on `ui_get_input()` instead of spinning.
- **UI → Display:** the length-1 **overwrite queue** + a **task notification** back.
- **Housekeeping → readers:** a `sensor_snapshot_t` struct guarded by **one
  mutex** (`g_snap_mtx`). Housekeeping writes it; UI/Display copy it out. Mutexes
  on FreeRTOS carry **priority inheritance**, which prevents the priority
  inversion that bit Mars Pathfinder.
- **Networking flags:** single `volatile` scalars (naturally atomic on this MCU);
  multi-byte network data is read under the cyw43 lock
  (`cyw43_arch_lwip_begin/end`).
- **Flash writes:** `flash_safe_execute()` (multicore-safe), with an explicit
  core-1 lockout *handshake* (`rtos_wait_flash_ready()`) so the very first
  first-boot write can't race the other core.

### 5.4 Boot sequence (why ordering matters)

```
main() on the boot core, BEFORE any RTOS:
  stdio · clock-from-compile-time · hardware init (SPI/e-ink pins) · joystick · speaker
  · [hold-UP → jump to the OTA bootloader] · startup chime · EPD_Init/Clear · seed RNG
  → xTaskCreate(app_task, core 0); vTaskStartScheduler()    ← the OS takes over here

app_task:
  cyw43_arch_init()   ← MUST be here: the FreeRTOS Wi-Fi driver needs the scheduler running
  battery_init · mpu_init · load saved settings · rtos_tasks_start()
  → the UI state-machine loop
```

Two ordering rules you must not violate:

1. **`cyw43_arch_init()` runs *after* the scheduler starts** — the FreeRTOS-flavour
   Wi-Fi driver lives in a task, so the scheduler must already be alive.
2. **The OTA "hold-UP" check runs *before* the scheduler, on bare metal** — jumping
   into the bootloader must happen with no tasks alive.

### 5.5 Memory model

The RP2350 has **520 KB** SRAM and **no garbage collector** (`docs/04` is the full
pointers/stack/heap primer). Rough budget: FreeRTOS heap_4 pool **128 KB**, lwIP +
BTstack buffers, framebuffers (~8 KB), task stacks (UI ~16 KB, others smaller),
plus the firmware's globals. Phase 0 measured ~205 KB `.bss` — comfortably within
budget. Flash use (~600 KB of ~4 MB) is a non-issue.

Memory discipline (expanded in §7): allocate at startup, not in steady-state;
prefer static/stack allocation; the heap_4 allocator does *not* free in a way you
should rely on for churn; size task stacks from measured high-water marks.

---

## 6. Design guidelines & organization principles

These are the rules that keep the codebase coherent. Follow them and your change
will look like it belongs; ignore them and it will fight the architecture.

### 6.1 Architectural rules

1. **One task owns a resource.** Before reaching for a lock, ask "can a single
   task be the only thing that touches this?" If only Housekeeping reads I²C,
   there is no I²C race and no mutex needed. Design ownership first; lock only the
   few things that genuinely cross tasks.
2. **Keep the Display alone on core 1.** Do not add work to core 1. Do not move
   networking off core 0. The isolation *is* the design.
3. **Cross-task data moves through the documented IPC, not shared globals.**
   Input→UI via the queue; UI→Display via the overwrite queue + notification;
   sensors via the mutex-guarded snapshot. Adding a new global that two tasks poke
   is how you reintroduce the races the architecture was built to avoid.
4. **`rtos_tasks.c` stays free of hardware/driver code.** It orchestrates tasks
   and IPC and calls *back* into `main.c` through the hooks declared in
   `rtos_tasks.h` (`hk_sample`, `read_joystick`, `display_blit`, …). That seam is
   what keeps the RTOS logic testable and the hardware swappable.
5. **The HAL is `lib/Config/DEV_Config.*`.** All pin/SPI/GPIO/delay specifics go
   through it. This is both the portability boundary *and* the unit-test seam (§9).
6. **One target, on purpose.** Board = `pico2_w`, display = `V4`, wiring = WetGreg
   PCB. New boards/displays belong in the upstream WetGreg repo, not here. Don't
   add `#ifdef BOARD_X` ladders to this narrow extraction.

### 6.2 Organization rules

7. **Firmware source changes live under `dev-setup/wetgreg-hub-rtos/`.** New `.c`
   files get added to `add_executable(...)` in its `CMakeLists.txt`, and new
   libraries to `target_link_libraries(...)`.
8. **Keep the display library vendored and patched.** The e-ink driver in `lib/`
   is patched for this board's SPI0/GP17–22 wiring. Don't replace it with a clean
   upstream copy; if you must re-vendor, re-apply the wiring (the DevTool does this
   at build time via `apply_eink_wiring`).
9. **Pin dependencies; explain bumps.** When you move a submodule, commit the new
   pinned commit and say *why*. Reproducible builds depend on it.
10. **Extend the DevTool, don't fork it.** Add a feature as a module-level function
    first, then wire *both* a GUI button and a CLI subcommand to it, then document
    it in `DEVTOOL_GUIDE.md` (it renders live in the Docs tab).
11. **Never commit secrets.** `wifi_config.h` ships placeholders; real credentials
    go through CMake `-D` flags or a git-ignored local copy.
12. **`build/` and `build-ota/` are never committed.** They're git-ignored; don't
    force-add them.

---

## 7. The rules of C firmware development

C gives you nothing for free and protects you from nothing. These are the
field-tested rules that keep embedded C from biting you. They're grouped so you
can skim, and each ends with *why* — because a rule you don't understand is a rule
you'll break under pressure. (Sources for the deeper "why" are in §11; this is the
distilled checklist.)

### 7.1 Types & data

- **Use fixed-width integer types** (`uint8_t`, `int16_t`, `uint32_t` from
  `<stdint.h>`), never bare `int`/`long` for anything that touches hardware,
  protocols, or storage. *Why: `int` width is implementation-defined; a register
  or packet field has an exact width and you must match it.*
- **Be explicit about signedness.** Mixing signed and unsigned in comparisons is a
  classic bug (`for (unsigned i = n; i >= 0; i--)` never ends). *Why: integer
  promotion rules are subtle and the compiler won't always warn.*
- **`const`-qualify everything that doesn't change** — pointers-to-const for
  read-only params, `const` tables in flash. *Why: it documents intent, lets the
  compiler put data in flash instead of RAM, and catches accidental writes.*
- **Beware integer overflow and division.** Check ranges before multiplying;
  remember integer division truncates. *Why: there's no exception — it just wraps
  or truncates silently.*

### 7.2 Memory

- **Don't allocate on the heap in steady state.** Allocate what you need at
  startup; prefer static and stack allocation thereafter. *Why: fragmentation and
  allocation failure on a device that must run for weeks are unacceptable; the
  firmware's own `docs/04` makes this concrete.*
- **There is no garbage collector.** Every `malloc` has exactly one owner and one
  `free`. Better: avoid the question by not allocating dynamically.
- **Initialize before use; zero is not automatic for stack/locals.** *Why:
  reading an uninitialized local is undefined behavior and reads as a flaky bug.*
- **Size task stacks from measured high-water marks** (`uxTaskGetStackHighWater
  Mark`), don't guess. The firmware already declares stack sizes in `rtos_tasks.h`
  with a note that Phase 3 trims them from on-device measurement. *Why: too small
  = stack overflow (silent memory corruption); too large = wasted RAM.*
- **Keep the stack-overflow and malloc-failed hooks enabled** — they already exist
  in `freertos_hooks.c`. *Why: they turn silent corruption into an observable halt.*

### 7.3 Concurrency & interrupts (the RTOS rules)

- **`volatile` is for "the value can change behind the compiler's back"** —
  memory-mapped registers, variables shared with an ISR. It is **not** a
  synchronization primitive and does **not** make multi-byte access atomic. *Why:
  `volatile` only stops the compiler from caching/reordering a single access; it
  says nothing across cores. Use a mutex/queue for anything bigger than a word.*
- **ISRs must be short and must use the `…FromISR` API** (`xQueueSendFromISR`,
  `vTaskNotifyGiveFromISR`) with the `pxHigherPriorityTaskWoken` dance. *Why: the
  normal blocking API inside an ISR corrupts the scheduler.* (Note: the firmware's
  IRQ-driven input path is *off by default* precisely because a first `…FromISR`
  attempt soft-bricked the board — see the comment in `rtos_tasks.h`. Treat ISR
  work with respect.)
- **Never block, never `malloc`, never `printf` inside an ISR.** Defer the work to
  a task via a notification/queue.
- **Hold locks briefly and in a consistent order.** *Why: out-of-order acquisition
  is how you get deadlock; long holds cause priority inversion.*
- **Prefer message-passing (queues) over shared memory + locks.** *Why: it's the
  pattern the whole architecture is built on and it's far easier to reason about.*

### 7.4 Hardware & robustness

- **Read the datasheet, then read it again.** Register bits, timing, power-up
  sequences are exact. The e-ink ~300 ms BUSY wait and the SSD1680 command
  sequences are in the panel datasheet for a reason.
- **Validate every input and every return code.** Sensor reads fail, Wi-Fi drops,
  flash writes can be interrupted. *Why: on a device with no user looking at a
  stack trace, an unchecked failure becomes a freeze or a brick.*
- **Make flash writes power-loss-safe and multicore-safe** (`flash_safe_execute`,
  already used). *Why: a brownout mid-write corrupts settings.*
- **Consider the watchdog** for unattended recovery (`hardware_watchdog` is already
  linked for the hold-CENTER reboot). *Why: a hung field device should reset
  itself, not stay dead.*
- **Compile with `-Wall -Wextra` and treat warnings as bugs.** Phase 2 builds with
  **0 warnings** — keep it that way. *Why: most embedded C warnings are real
  defects in disguise.*

### 7.5 A note on MISRA C

Safety-critical embedded shops follow **MISRA C** — a ruleset that bans the
footgun corners of the language (implicit conversions, recursion, dynamic memory,
etc.). You don't need full MISRA compliance for a hobby hub, but the *spirit* of
it is exactly §7.1–7.4. FreeRTOS itself ships a `MISRA.md`; skim it to see what
"defensive C" looks like at the professional end. §9 shows how a static analyzer
can check a pragmatic subset for you automatically.

---

## 8. Coding fundamentals & house conventions

This codebase has a deliberate, consistent style. Match it.

- **Every source file opens with a banner comment** stating its purpose, which
  task uses it, and what it *owns*. Read the banner before the code; write one for
  any new file.
- **Comments are verbose on purpose.** This is a teaching codebase. Where C does
  something with no JavaScript/Python equivalent (pointers, casts, bit ops, manual
  memory, `volatile`), the comment spells it out. That's unusual for production C
  and intentional here — keep the altitude.
- **`WHY:` comments explain decisions, not mechanics.** `// WHY: pinned to core 1
  so the 300 ms refresh can't stall input` is worth ten comments that restate the
  code. Add `WHY:` whenever a choice is non-obvious.
- **Naming:** lower_snake_case for functions/variables, `UPPER_SNAKE` for macros
  and compile-time constants, `g_` prefix for the few shared globals
  (`g_input_q`, `g_snap_mtx`). Keep helpers `static` unless they're part of a
  module's public contract.
- **Headers are contracts.** `rtos_tasks.h` is the model: it documents the four
  tasks and the API in prose, then declares it. A new module's `.h` should read
  like a spec.
- **Small, single-purpose functions.** The big `main.c` is a Phase-0 monolith
  slated to split into `src/rtos`, `src/drivers`, `src/app`, `src/net` — new code
  should already be written as if it lives in one of those: cohesive and minimally
  coupled.
- **DevTool (Python) conventions:** 4-space indent, logging prefixes by subsystem
  (`[build]`, `[flash]`, `[picotool]`), one implementation shared by GUI and CLI.

---

## 9. A testing & validation system

> **This is the part the existing docs don't yet cover.** Today the firmware is
> validated by (a) a clean Docker build with 0 warnings, and (b) a one-time
> multi-agent adversarial concurrency review. There is **no automated test suite,
> no host unit tests, and no CI**. This section is a concrete, staged plan to add
> one — designed so you can **validate features virtually, before flashing**, and
> **harden the code with unit tests** as the project emphasizes.

### 9.1 Why testing firmware is different (and why "virtual-first" matters)

The thing that makes embedded testing hard is the same thing that makes it
valuable: the code is entangled with hardware you can't easily poke, and the
feedback loop ("flash, watch the screen, guess") is slow and lossy. The strategy
is to **separate the logic from the hardware** and test as much as possible *off*
the device, where the loop is milliseconds and a failure is a red assertion, not a
bricked board.

That separation already half-exists here:

- **The HAL seam** (`lib/Config/DEV_Config.*`) isolates pins/SPI/delays.
- **The RTOS seam** (`rtos_tasks.h` hooks) isolates orchestration from drivers.
- **Pure logic** is scattered through `main.c` and is the easiest, highest-value
  thing to unit-test today: `pedometer_update`, `activity_update`,
  `accel_magnitude`, `tilt_x_deg`/`tilt_y_deg`, `rotate_input`, `font_index`,
  the orientation mapping (`ORIENT_CFG`), the battery-%-from-volts curve, the
  input edge/auto-repeat state machine, and `transpose_to_display`.

### 9.2 The firmware testing pyramid (what runs where)

```
                  ▲ slower, higher-fidelity, fewer
   ┌──────────────────────────────────────────────────┐
   │ 5. HARDWARE-IN-THE-LOOP (real board, automated)   │  ← truth, but slow & owed to hardware
   ├──────────────────────────────────────────────────┤
   │ 4. EMULATION / SIM (Renode · Wokwi · QEMU)        │  ← "virtual board": boot, tasks, timing
   ├──────────────────────────────────────────────────┤
   │ 3. ON-HOST INTEGRATION (FreeRTOS POSIX port)      │  ← real scheduler, mocked drivers
   ├──────────────────────────────────────────────────┤
   │ 2. STATIC + DYNAMIC ANALYSIS (warnings·cppcheck·  │  ← cheap, catches whole bug classes
   │    clang-tidy·ASan/UBSan on host build)           │
   ├──────────────────────────────────────────────────┤
   │ 1. HOST UNIT TESTS (Unity/Ceedling, pure logic)   │  ← fast, run on every commit
   └──────────────────────────────────────────────────┘
                  ▼ faster, run constantly, many
```

Build from the bottom up. Layers 1–4 all run on a laptop or in CI **with no
hardware attached** — that's the "validate before deploying" the brief asks for.

### 9.3 Layer 1 — Host unit tests (start here)

**Goal:** compile the *pure logic* with your host's normal `gcc`/`clang` (not the
ARM cross-compiler) and assert on its outputs. No hardware, runs in milliseconds.

**Recommended framework:** **Unity + CMock + Ceedling** (ThrowTheSwitch) — the de
facto standard for C firmware unit testing. Unity is a tiny assertion library;
CMock auto-generates mock implementations from your headers (so you can fake the
HAL); Ceedling is the build/runner that ties them together. Alternatives: plain
Unity with a CMake runner, or **CppUTest** if you prefer.

**The pattern — extract, mock, assert.** Take a pure function and link it against a
*fake* HAL:

```c
/* test/test_pedometer.c — runs on the HOST, not the Pico */
#include "unity.h"
#include "pedometer.h"      /* extracted from main.c */

void test_single_step_is_counted(void) {
    pedometer_reset();
    /* feed a synthetic accel magnitude profile: rest → peak → rest */
    pedometer_feed(1.00f);   // standing
    pedometer_feed(1.45f);   // heel strike (above threshold)
    pedometer_feed(1.00f);   // back to rest
    TEST_ASSERT_EQUAL_UINT32(1, pedometer_steps());
}

void test_jitter_below_threshold_is_not_a_step(void) {
    pedometer_reset();
    for (int i = 0; i < 50; i++) pedometer_feed(1.02f);  // noise
    TEST_ASSERT_EQUAL_UINT32(0, pedometer_steps());
}
```

```c
/* test/mocks/mock_DEV_Config.c — the HAL, faked for the host.
   CMock can generate this automatically from DEV_Config.h. */
static uint8_t fake_pins[64];
void  DEV_Digital_Write(uint16_t pin, uint8_t v) { fake_pins[pin] = v; }
uint8_t DEV_Digital_Read(uint16_t pin)           { return fake_pins[pin]; }
void  DEV_Delay_ms(uint32_t ms)                  { (void)ms; /* no real delay in tests */ }
/* …SPI writes can record into a buffer you assert on. */
```

**What to test first (high value, low effort), and the bug each catch guards:**

| Target | Test it for | Guards against |
|---|---|---|
| `accel_magnitude`, `tilt_*` | known vectors → known g/degrees | float/scale math drift |
| `pedometer_update` | step / no-step / double-count | the exact double-count bug the concurrency review found |
| `rotate_input` + orientation map | each orientation → correct direction remap | "up scrolls down when held sideways" |
| battery-%-from-volts | curve endpoints + midpoints, USB case (−1) | a wrong gauge that erodes trust |
| input edge + auto-repeat FSM | tap = 1 move; hold = delay then repeat | dropped/duplicated presses |
| `font_index` / `kb_char_at` | every char maps correctly; out-of-range safe | off-by-one glyph/keyboard bugs |
| `transpose_to_display` | a known `frame[]` → exact panel bytes | rotated/mirrored screen output |

**The refactor this enables (and rewards):** to unit-test a function it must be
*reachable without the hardware*. Pulling `pedometer_*` out of `main.c` into
`pedometer.c/.h` that depend only on numbers (not on the accelerometer driver) is
exactly the `main.c` → `src/app/` split the docs already plan. **Testing and good
modularization are the same work.**

### 9.4 Layer 2 — Static & dynamic analysis (cheapest bug-per-dollar)

These catch entire *classes* of defect with near-zero per-test authoring effort.

**Static (no execution):**
- **Compiler as a linter:** build with `-Wall -Wextra -Wshadow -Wconversion
  -Wpointer-arith` and keep the **0-warning** bar. Consider `-Werror` in CI.
- **cppcheck** — a dedicated C static analyzer; finds uninitialized reads,
  buffer overruns, leaks, dead code. Has a MISRA add-on for a pragmatic subset of
  §7.5.
- **clang-tidy / clang static analyzer (`scan-build`)** — deeper data-flow
  analysis (null derefs, use-after-free, logic bugs).

**Dynamic (run the host build under instrumentation):** because Layer 1 compiles
logic for the host, you get the desktop sanitizers for *free*:
- **AddressSanitizer (`-fsanitize=address`)** — buffer overflows, use-after-free.
- **UndefinedBehaviorSanitizer (`-fsanitize=undefined`)** — signed overflow, bad
  shifts, misaligned access, the §7.1 footguns.
- Run the unit suite under both; many "works on my desk, flaky on device" bugs
  are UB that ASan/UBSan name precisely.

### 9.5 Layer 3 — On-host integration with the real scheduler

**Goal:** test *task interactions* — the queue hand-off, the snapshot mutex, the
render coalescing — without hardware, using the real FreeRTOS kernel.

FreeRTOS ships a **POSIX/Linux port** that runs the actual scheduler as host
threads. You compile `rtos_tasks.c` against it, stub the driver hooks
(`read_joystick`, `display_blit`, `hk_sample`) with fakes that feed scripted input
and record output, and assert on behavior:

- *"If UI posts 5 frames while Display is busy, Display shows only the newest"*
  (verifies the `xQueueOverwrite` coalescing).
- *"A press enqueued during a simulated 300 ms refresh is delivered to UI"*
  (verifies the latency claim at the logic level).
- *"Two tasks hammering the snapshot never read a torn struct"* (run under
  ThreadSanitizer for extra teeth).

This is the layer that lets you *regression-test the concurrency design* that was
previously only checked by a one-time human/agent review.

### 9.6 Layer 4 — Emulation / simulation ("the virtual board")

**Goal:** boot the *actual ARM firmware image* on a simulated RP2350 — no physical
board — and watch it come up, schedule tasks, and hit timing. This is the closest
"virtual validation before deploying" gets.

Realistic options (with honest caveats — RP2350 is newer than RP2040, so support
varies):

| Tool | Good for | Caveats |
|---|---|---|
| **Wokwi** | Browser/CI simulation of Pico (incl. Pico 2 / RP2350) running real `.uf2`; has FreeRTOS examples; scriptable for CI | Peripheral coverage is partial; the exact e-ink panel + cyw43 radio may not be modeled — test logic/boot/timing, not the radio |
| **Renode** | Whole-system emulation, deterministic, great for CI and fault injection; strong Cortex-M support | RP2350 platform may need a custom `.repl` machine description; budget setup time |
| **QEMU** | Generic Cortex-M; fast | No RP2350 board model out of the box; most useful for the host/POSIX path, not faithful peripherals |

**Practical use:** run the firmware in Wokwi/Renode in CI to assert "it boots,
creates its tasks, and reaches the UI loop without faulting" on every push — a
smoke test that would have caught the IRQ soft-brick *before* it reached a board.
Don't expect faithful e-ink or radio behavior; that's Layer 5.

### 9.7 Layer 5 — Hardware-in-the-loop (the truth, automated)

Some things are only true on silicon: real input latency during a refresh, BLE
pairing, the OTA round-trip, e-ink ghosting. Automate what you can:

- The **DevTool already gives you the primitives:** `devtool.py build`, `flash`,
  `info`, and the **115200 serial monitor**. `devtool.py deps` exits non-zero when
  the toolchain is incomplete — *it's already a CI gate.*
- **Golden serial-log assertions:** have the firmware `printf` structured markers
  (`[boot] tasks=4`, refresh-time and battery meters already exist per
  `docs/11`), flash on a bench board, and assert the expected lines appear within
  a timeout. This is a poor-man's HIL that needs only a USB cable.
- **Stack high-water marks:** print `uxTaskGetStackHighWaterMark` per task at
  runtime and fail the bench test if any task is within a safety margin — this is
  the Phase 3 "stack tuning needs the board" item turned into a check.
- A self-hosted runner with a board attached can run flash + serial-assert on
  every merge to `main`; until then, make it a documented manual gate.

### 9.8 Putting it together — a proposed layout & CI

A concrete starting structure (nothing here exists yet — it's the plan):

```
dev-setup/wetgreg-hub-rtos/
├── src/                      ← extract pure logic out of main.c as you test it
│   ├── app/ pedometer.c/.h  activity.c/.h  battery.c/.h  input_fsm.c/.h …
│   └── …
├── test/
│   ├── unit/                 ← Layer 1: Unity/Ceedling specs (test_pedometer.c …)
│   ├── mocks/                ← faked HAL (mock_DEV_Config.c) — CMock-generated
│   ├── integration/          ← Layer 3: FreeRTOS POSIX-port task tests
│   └── sim/                  ← Layer 4: Wokwi/Renode scripts + expected boot log
├── project.yml               ← Ceedling config (host toolchain, sanitizers on)
└── CMakeLists.txt            ← unchanged for the device build
```

```yaml
# .github/workflows/ci.yml  (illustrative)
jobs:
  unit:        # Layer 1+2 — seconds, every push
    steps: [ checkout, "ceedling test:all gcov", "cppcheck --enable=all src/",
             "build host with -fsanitize=address,undefined && run tests" ]
  build:       # device build must stay green & warning-free
    steps: [ "git submodule update --init --recursive",
             "python3 tools/devtool/devtool.py deps",      # the existing gate
             "cd dev-setup && docker compose run --rm build-wetgreg-hub-rtos" ]
  sim:         # Layer 4 — boot smoke test (optional, when the model is ready)
    steps: [ "wokwi-cli --timeout 30s --expect-text '[boot] tasks=4'" ]
```

### 9.9 Hardening checklist (turn tests into resilience)

Unit tests prove logic; these turn that into a device that survives the field:

- ☐ `configASSERT()` enabled and meaningful in debug builds (FreeRTOS catches
  misuse early). ☐ Stack-overflow + malloc-failed hooks stay enabled
  (`freertos_hooks.c` — they already are).
- ☐ Every driver/return code checked; sensor/Wi-Fi/flash failures degrade
  gracefully, never freeze.
- ☐ Watchdog arms in steady state so a hang self-recovers.
- ☐ Fuzz the parsers/decoders (BLE payloads, saved-settings blobs) on the host
  with libFuzzer — untrusted bytes are where firmware gets wedged.
- ☐ Track flash/RAM and per-task stack high-water marks over time; regressions
  are early warnings.
- ☐ Keep the **0-warning** build bar; run ASan/UBSan in CI on the host path.
- ☐ Coverage as a *guide* (gcov/lcov via Ceedling): chase coverage on the pure
  logic, don't fake it on glue code.

> **The one-sentence testing philosophy:** *make the logic testable off the
> device, prove it with fast host tests + sanitizers, smoke-test the boot in a
> simulator, and reserve the physical board for the few truths only silicon can
> tell.* Every step you push leftward (toward the laptop) is a step you stop
> paying for in slow flash-and-pray cycles.

---

## 10. Development workflow & checklists

### 10.1 The everyday loop

```bash
# one-time
git clone --recurse-submodules <repo-url> WetGregFirmware && cd WetGregFirmware
./install-deps.sh

# edit firmware under dev-setup/wetgreg-hub-rtos/, then:
python3 tools/devtool/devtool.py            # GUI → Flash → Clean Build & Flash
#   …or headless:
python3 tools/devtool/devtool.py build-flash
python3 tools/devtool/devtool.py info       # confirm what's actually on the board
#   watch it run: Debug tab → Serial monitor (115200)
```

- **Changed firmware → Clean Build & Flash** (deletes `build/` first — no
  stale-object ghosts).
- **Didn't change firmware → Flash (existing)** — skips the ~minute compile.
- **Won't build? → Debug → Run diagnostics** — points at the missing piece.
- **Bricked / snow → Full Erase, then Clean Build & Flash.**

### 10.2 Before you commit (the gate)

- ☐ Builds clean in Docker with **0 warnings**.
- ☐ New `.c` files added to `CMakeLists.txt`; new libs to `target_link_libraries`.
- ☐ Pure logic you added/changed has a host unit test (§9.3) — or you opened an
  issue saying why not.
- ☐ No secrets, no `build/` artifacts staged.
- ☐ New cross-task data uses the documented IPC, not a new shared global (§6).
- ☐ New file has a banner comment; non-obvious choices have `WHY:` comments.
- ☐ DevTool change touched a shared function + GUI button + CLI subcommand +
  `DEVTOOL_GUIDE.md`.
- ☐ Submodule bumps are pinned and explained.

---

## 11. Development resources & extended learning

Curated, and **justified** — each entry says *why it's here and what it unlocks*.
The firmware's own `docs/09-SOURCES-AND-FURTHER-READING.md` is the canonical list
for the RTOS/SDK design citations; this extends it toward C fundamentals and
testing.

### C language & embedded-C fundamentals

- ***The C Programming Language* — Kernighan & Ritchie ("K&R").** *Why:* the
  source. Short, exact; if you're new to C, the pointers and arrays chapters
  directly demystify `docs/04`.
- ***Modern C* — Jens Gustedt** (free PDF). *Why:* C as it's actually written in
  2020s, with the `<stdint.h>`/`const`/UB discipline §7 preaches.
- ***Effective C* — Robert Seacord (No Starch).** *Why:* correctness and security
  focus — overflow, undefined behavior, the traps §7.1 warns about, from the
  person who wrote the CERT C standard.
- **comp.lang.c FAQ** (c-faq.com). *Why:* the canonical answers to "why does this
  pointer/array/integer thing behave like that" — bookmark it.

### Embedded systems & firmware craft

- ***Making Embedded Systems* — Elecia White (O'Reilly).** *Why:* the practical
  bridge from "I know C" to "I write firmware" — interrupts, polling vs IRQ,
  state machines, exactly the §7.3/§7.4 material; already cited by `docs/09`.
- **Embedded.com / Jack Ganssle's archives.** *Why:* decades of hard-won articles
  on debouncing, watchdogs, stacks, and the failure modes this project hits.
- **MISRA C** (misra.org.uk) + FreeRTOS's `FreeRTOS-Kernel/MISRA.md`. *Why:* see
  what "defensive C" looks like at the safety-critical end (§7.5).

### RTOS & FreeRTOS (the core of this project)

- ***Mastering the FreeRTOS Real-Time Kernel* — Richard Barry** (free book).
  *Why:* THE book on tasks/queues/semaphores/mutexes/notifications; backs
  `docs/03` and `docs/06`. Read it alongside this firmware.
- **FreeRTOS SMP documentation.** *Why:* core affinity,
  `configNUMBER_OF_CORES`, `configRUN_MULTIPLE_PRIORITIES` — the exact mechanics
  of the dual-core decision in `docs/05`.
- **FreeRTOS API reference** (freertos.org/a00106.html). *Why:* precise semantics
  of every kernel call in `rtos_tasks.c`.
- **"Why use an RTOS?"** (freertos.org/about-RTOS.html). *Why:* the super-loop-vs-
  RTOS argument that *is* this project's thesis (§2).

### Raspberry Pi Pico / RP2350 platform

- **Raspberry Pi Pico-series C/C++ SDK book** + **RP2350 datasheet** (PDFs).
  *Why:* the `pico_cyw43_arch` variants (poll vs FreeRTOS), `pico_flash` /
  `flash_safe_execute`, the dual-M33 memory map — the silicon truth behind
  `docs/02/04/05`.
- **pico-examples — `pico_w/wifi/freertos`.** *Why:* the reference wiring for
  FreeRTOS + lwIP (`NO_SYS=0`) + cyw43 that `lwipopts.h` follows.
- **FreeRTOS-Kernel-Community-Supported-Ports (`RP2350_ARM_NTZ`).** *Why:* the
  ARMv8-M/Cortex-M33 port options (`configENABLE_FPU/MPU/TRUSTZONE`) set in
  `FreeRTOSConfig.h`.
- **lwIP "Common pitfalls" / multithreading.** *Why:* OS-mode networking and the
  `cyw43_arch_lwip_begin/end` core lock (`docs/06`).
- **Waveshare 2.13″ e-Paper (SSD1680) wiki + datasheet.** *Why:* the command
  sequences and the ~300 ms BUSY wait that motivate the entire architecture.

### Testing & quality tooling (for §9)

- **ThrowTheSwitch — Unity, CMock, Ceedling** (throwtheswitch.org +
  *Unity/Ceedling* docs). *Why:* the standard host-unit-test stack for C firmware;
  the framework for Layer 1.
- **James Grenning — *Test-Driven Development for Embedded C*.** *Why:* the
  definitive book on the extract-mock-assert workflow §9.3 describes; teaches the
  "separate logic from hardware to make it testable" discipline this project needs.
- **cppcheck**, **clang-tidy / scan-build**, **AddressSanitizer/UBSan** docs.
  *Why:* Layer 2 — whole bug classes for near-zero effort.
- **Renode** (renode.io) and **Wokwi** (docs.wokwi.com, incl. Pico/RP2350 + CI
  CLI). *Why:* Layer 4 — boot and run the real image on a virtual board in CI.
- **Interrupt blog (Memfault)** — practical firmware testing, CI, fault analysis,
  coredumps. *Why:* the best modern writing on shipping reliable firmware; maps
  directly onto §9's hardening checklist.

### Concurrency theory (the "why locks" background)

- **Priority inversion / Mars Pathfinder case study.** *Why:* the famous
  real-world bug that explains why FreeRTOS mutexes use priority inheritance
  (`docs/06`).
- **Race conditions, deadlock, atomicity** (any OS text; *Making Embedded
  Systems* ch. on concurrency). *Why:* the foundation under §5.3 and §7.3.

> **How to use this list:** when a doc or this guide makes a claim — "SMP needs
> core affinity", "flash writes must coordinate both cores", "a blocking call is
> fine inside a task" — find the matching resource and read the relevant section.
> The goal, as `docs/09` puts it, is that *nothing in this codebase is "because the
> author said so"*: every non-obvious choice traces to a reference plus the
> project's own measured results.

---

## 12. Glossary

| Term | Meaning |
|---|---|
| **RTOS** | Real-Time Operating System — a tiny library (here, FreeRTOS) compiled into the firmware that shares the CPU(s) among *tasks* |
| **Task** | An independent unit of work the scheduler runs; mentally, its own `while(1)` loop |
| **SMP** | Symmetric Multiprocessing — FreeRTOS scheduling tasks across *both* RP2350 cores for true parallelism |
| **Core affinity** | Pinning a task to a specific core (Display → core 1; everything else → core 0) |
| **Super-loop** | The single `while(1)` architecture the original firmware used and this RTOS rewrite replaces |
| **HAL** | Hardware Abstraction Layer — `lib/Config/DEV_Config.*`; the seam between logic and pins |
| **IPC** | Inter-Process (here, inter-task) Communication — queues, mutexes, task notifications |
| **Mutex** | A lock with priority inheritance; here, guards the sensor snapshot |
| **Queue** | A thread-safe FIFO; here, Input→UI events and the length-1 UI→Display render hand-off |
| **`volatile`** | "May change behind the compiler's back" — for registers/ISR-shared vars; **not** a lock |
| **BUSY pin** | The e-ink line that signals "refresh in progress (~300 ms)" |
| **OTA** | Over-The-Air firmware update (Wi-Fi), via the picowota bootloader |
| **GATT** | The Bluetooth LE service/characteristic database a phone reads (`wetgreg.gatt`) |
| **XIP** | eXecute-In-Place — running code directly from external flash |
| **HIL** | Hardware-In-the-Loop — automated tests driving a real attached board |
| **`.uf2`** | The drag-and-drop flashable image format for the BOOTSEL drive |

---

*Maintainers: keep this guide in sync with the firmware `docs/` set. When you add
a feature, update §3; when you add a test layer, update §9; when a Phase flips
from 🔜 to ✅, update both here and `docs/05 §8`. A guide that lies is worse than no
guide.*
