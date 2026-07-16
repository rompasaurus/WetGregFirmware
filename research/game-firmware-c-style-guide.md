# WetGreg Firmware — Structure & Style Guide

The structure and coding principles of `dev-setup/wetgreg-hub-rtos` — the
FreeRTOS (SMP) firmware for the WetGreg: a Pico 2 W (RP2350) driving a WeAct
2.13" V4 e-ink panel, a 5-way joystick, an SC7A20 accelerometer, a buzzer, and
the CYW43 WiFi/BLE radio.

This guide replaced the original research survey after the July 2026 refactor
that split the 6,000-line `main.c` into the module tree below. The rules here
are the ones the codebase actually follows; the baseline standards they come
from are [BARR-C:2018](https://barrgroup.com/sites/default/files/barr_c_coding_standard_2018.pdf)
(module layout, header hygiene, naming) and NASA/JPL's
[Power of Ten](https://spinroot.com/gerard/pdf/P10.pdf) (function size, no
dynamic allocation, zero-warning builds). Where the codebase deliberately
deviates, the deviation and its reason are stated — a rule nobody follows is
worse than an honest exception.

---

## 1. Directory layout

```
wetgreg-hub-rtos/
├── src/
│   ├── app/                # the app shell — nothing else knows the states
│   │   └── app_main.c      #   main(), task creation, the UI state machine
│   ├── drivers/            # one .c/.h pair per hardware entity
│   │   ├── accel.c/.h      #   SC7A20 accelerometer (I2C0) register I/O
│   │   ├── battery.c/.h    #   VSYS/USB sensing + LiPo curve (raw, unfiltered)
│   │   ├── display.c/.h    #   e-ink glue: EPD variant, buffers, transpose
│   │   ├── joystick.c/.h   #   5-way joystick + rotation remap
│   │   └── speaker.c/.h    #   active buzzer: tones, patterns, settings
│   ├── sys/                # system services (no rendering, no app states)
│   │   ├── motion.c/.h     #   orientation / pedometer / idle + hk_sample()
│   │   ├── power.c/.h      #   sleep state, charging diet + batt_sample()
│   │   ├── rng.c/.h        #   xorshift PRNG, ADC-noise seeded
│   │   ├── rtc_clock.c/.h  #   compile-time RTC seed + month names
│   │   └── storage.c/.h    #   flash-sector settings store + social log
│   ├── net/
│   │   ├── ntp.c/.h        #   one-shot NTP sync over lwIP
│   │   └── wifi.c/.h       #   scan / join / disconnect / post-wake rejoin
│   └── ui/                 # everything that draws pixels
│       ├── canvas.c/.h     #   the shared 1-bpp canvas (wide/tall shapes)
│       ├── text.c/.h       #   5×7 font + text renderers
│       ├── icons.c/.h      #   status-bar glyphs
│       ├── octopus.c/.h    #   Greg: procedural art, moods, expressions
│       ├── screens_home.c/.h     # main screen (wide + tall layouts)
│       ├── screens_anim.c/.h     # splash / intro / sleep still
│       ├── screens_menu.c/.h     # menu system + Settings-family screens
│       ├── screens_network.c/.h  # WiFi + Bluetooth screens, keyboard
│       └── screens_social.c/.h   # social screens + emote playback
├── rtos_tasks.c/.h         # the 4-task split: Input/Display/HK/Battery + IPC
├── freertos_hooks.c        # kernel-required callbacks
├── bt.c/.h                 # BLE: GATT service + the social beacon protocol
├── quotes.h                # AUTO-GENERATED quote table (see §6) — don't edit
├── FreeRTOSConfig.h, lwipopts.h, btstack_config.h, wifi_config.h   # config
├── lib/                    # vendored code (EPD driver, DEV_Config, rtc_compat)
└── docs/                   # the numbered developer guides (00–11)
```

Every `.c` and `.h` file carries a top comment with an explicit
**`Purpose:`** line — one or two sentences saying what the module owns and
what stays private to it. Keep this rule for new files.

### The dependency rule (checkable by reading `#include` lines)

```
app  →  everything below
ui   →  ui, sys, drivers, rtos_tasks.h, bt.h        (screens read live state)
net  →  sys (storage), net, pico SDK
sys  →  drivers, sys, net (power→wifi), rtos_tasks.h, bt.h
drivers → pico SDK + rtos_tasks.h only              (one exception: display→canvas)
```

- **Nothing includes `app/`** — the app has no header; states, `POLL_INPUT`,
  and screen-selection indices live only in `app_main.c`.
- **Only `display.c` includes the EPD driver headers**; post-boot, only the
  Display task (core 1) executes them.
- **Only `app_main.c` may reference the `quotes[]` array** (see §6).
- Known exception: `drivers/display.c` includes `ui/canvas.h` (the transpose
  reads the canvas) and `ui/canvas.c` includes `sys/motion.h` (shape follows
  orientation). Both are one-way and documented at the include site.

---

## 2. Module rules (the BARR-C core)

- **One entity per `.c`/`.h` pair.** A module owns exactly one thing: one
  driver, one service, one screen family. If a file needs two Purpose
  sentences that don't connect, it's two modules.
- **Each `.c` includes its own header first** so the compiler checks every
  public prototype against its definition (BARR-C 4.3.c).
- **Section order inside a `.c`:** file comment (with `Purpose:`) → includes
  (own header, then libc, then SDK, then project) → macros/types → data →
  private helpers → public functions.
- **`static` on every module-private function and variable.** This is the
  encapsulation mechanism in C. Example: `octopus.c` exposes 6 functions and
  hides ~50 pupil/mouth/brow helpers; `text.c` hides the font tables behind
  `font_glyph()`.
- **Headers:** include guard, expose only what callers need, never define
  storage (only `extern` declarations), never include a private header.
- **Config in one place:** pins and tunables live at the top of the module
  that owns the hardware (e.g. `BUZZER_PIN` in speaker.c, `JOY_*` in
  joystick.h because main() needs one for the OTA check), not scattered.

### Shared state — the honest deviation

Strict BARR-C would wrap all cross-module state in accessor functions. This
codebase uses **documented `extern` state with a single named writer** where
accessors would only add noise (e.g. `wifi_connected`, `g_batt_pct`,
`g_orientation`, the keyboard state). The rules that make this safe:

1. Every shared variable is declared in exactly one header, defined in the
   owning `.c`, with a comment naming **who writes it and from which task**.
2. **One writer.** If two tasks would need to write it, it becomes a
   mutex-guarded snapshot instead (see §3).
3. Cross-task reads rely on the documented invariants: aligned ≤32-bit
   loads/stores are atomic on the Cortex-M33, and the writer/reader tasks are
   pinned to the same core where noted (`input_rotation`, `g_orientation`).
   If a future change unpins those tasks, the comments say exactly what must
   become a snapshot.
4. Where a small API is natural, prefer it — the speaker settings are
   accessors (`speaker_enabled()`, `speaker_set_pattern()`), not externs.

---

## 3. Task architecture (the RTOS rules)

The firmware is **four cooperating tasks** plus the cyw43 background task
(contract in `rtos_tasks.h`; rationale in `docs/05` and `docs/06`):

| Task | Core | Prio | Owns |
|---|---|---|---|
| UI / app | 0 | mid | the state machine, all rendering, `frame[]` |
| Input | 0 | top | the joystick — sole runtime reader; posts events |
| Display | 1 | mid | the e-ink panel — sole post-boot EPD caller |
| Housekeeping | 0 | low | accel sampling → `hk_sample()` (motion.c) |
| Battery | 0 | low | cyw43-locked ADC reads → `batt_sample()` (power.c) |

Principles the code holds to (keep them when adding features):

- **Block in one place per loop.** The UI blocks only on `ui_get_input()`;
  screens model waiting as a timeout on that call, not `vTaskDelay()`
  sprinkled through logic. The Housekeeping/Battery hooks run to completion
  and never block on the display.
- **Ownership passes by queue, not by sharing.** A finished frame is copied
  into a free display buffer and its *index* is queued to core 1 — the UI
  never touches a buffer that's in flight.
- **Never take the cyw43 lock on the Housekeeping path.** Battery reads take
  it, which is exactly why they live in their own task (`batt_sample`) — the
  field bug where auto-rotate froze for seconds is documented in power.c.
- **Cross-task data goes through the sensor snapshot** (`rtos_snapshot_get`),
  a plain struct copied under a mutex — except the deliberate single-writer
  reads listed in §2.
- **Flash writes park core 1.** Always `flash_safe_execute()` via
  `saved_write_flash()`; never write flash per-event in a hot loop (see the
  softlock note in the SCAN NEARBY handler).
- ISRs (the gated joystick IRQ path) do nothing but notify a task.

---

## 4. Rendering pipeline (e-ink specifics)

```
screen renderer → frame[] (canvas.h, wide 250×122 or tall 122×250)
               → transpose_to_display()  (display.c → panel-format ui_buf)
               → display_render()        (rtos_tasks — copy to a free buffer,
                                          queue to core 1, ~300 ms refresh)
```

- **All drawing goes through `px_set`/`px_clr`** (canvas.h) or the offset
  variants private to octopus.c. Nobody else pokes `frame[]` bytes except the
  documented row-clear helpers.
- **Renderers are pure draw functions**: `render_x(sel)` reads module state
  and draws; input handling stays in the app state machine. Screens that
  share editor state with their handler declare it in their header
  (`settime_*`, `kb_*`) with an ownership comment.
- **Respect the panel drain.** A screen that re-renders on a timer must poll
  slower than the ~0.7 s refresh or the Input task starves — the constants in
  the app (`POLL_INPUT(2000)` on STATE_MOTION, frame waits ≥450 ms on the
  animations) encode hard-won fixes; don't lower them casually.
- **The e-ink holds its image for free.** Prefer "render once, then sleep on
  input" (sleep still, social cards) over animation loops; the idle/pocket
  freeze (`g_screen_idle`) skips redraws entirely.

---

## 5. Memory rules

- **No `malloc`/`free` — ever.** Everything is statically allocated: the
  canvas, the display buffers, the scan tables, the FreeRTOS objects
  (`configSUPPORT_STATIC_ALLOCATION`; heap_4 exists only for the SDK/lwIP
  internals). Fixed-size pools with visible overflow policy (`NEARBY_MAX`,
  `SOCIAL_MAX` evicts-oldest) instead of growth.
- **Persistence is one 4 KB flash sector** (`storage.c`): magic-checked,
  loaded once at boot, sanitised field-by-field (unknown/0xFF values fall
  back to defaults), appended-to rather than re-laid-out so old images keep
  loading; bump `SAVED_MAGIC` only when a re-seed is truly required.
- **Const tables live in flash.** Quotes, font, body RLE art, name tables,
  discharge curve — `static const`, never copied to RAM.
- Timeouts on every external bus: I2C uses `*_timeout_us` variants only, so a
  missing/shorted part degrades a feature instead of hanging a task.

---

## 6. The `quotes.h` rule

`quotes.h` is generated by the DevTool and defines the entire table as
`static const Quote quotes[...]` **in the header**. That makes each includer
compile its own copy — so:

- **Only `app_main.c` may reference `quotes[]`.** Everyone else includes
  quotes.h solely for the `Quote` type and the `QUOTE_COUNT` macro (an
  unreferenced static const array costs nothing — the compiler drops it).
- Render functions take `const Quote *` parameters; they never index the
  table themselves.

---

## 7. Naming & style

- **Names:** `snake_case` functions and variables, `UPPER_CASE` macros and
  constants, module-prefixed public functions (`wifi_`, `speaker_`, `saved_`,
  `render_`, `draw_`), `g_` prefix on shared globals, `k_` on const tables.
  Legacy names that match the hardware story stay (`mpu_*` for the SC7A20 —
  the header explains why).
- **Functions ≤ ~60 lines** (Power of Ten rule 4). The state-machine `case`
  blocks in app_main.c are the standing exception; if one outgrows a screen,
  extract a handler.
- **Comments explain *why*, not *what*.** The codebase's most valuable
  comments record field bugs and their fixes (the cyw43-lock starvation, the
  double-press wake race, the 5-tap coalescing) — keep writing those. Delete
  a comment rather than let it go stale.
- **Zero warnings in our sources.** The vendored BTstack redefinition
  warnings are known noise; anything from `src/` gets fixed, not suppressed.
- Prefer designated initializers for config tables (`ORIENT_CFG`,
  `OR_ANGLE`) and terminated arrays (`0xFF`, `{…, 0}`) for variable-length
  const data.

---

## 8. Recipes

**Adding a screen**
1. Pick the screen family (`screens_menu` / `_network` / `_social`, or a new
   pair if it's a new family). Add `render_myscreen(...)` + any `*_ITEM_*`
   codes to the header.
2. Add a `STATE_MYSCREEN` in app_main.c and a `case` using the
   `render → transpose_to_display → display_render → POLL_INPUT` shape.
3. Editor state shared between renderer and handler goes in the screen
   module with an ownership comment — not in app_main.c.

**Adding a driver**
1. New `src/drivers/name.c/.h` pair: pins/registers private, a minimal API,
   `Purpose:` line, timeouts on all bus calls.
2. Decide which task calls it (a slow/locking read never runs on the
   Housekeeping path — see §3) and wire it via a `sys/` service if the raw
   readings need filtering or policy.
3. Add the `.c` to `CMakeLists.txt`.

**Adding a persisted setting**
1. Append the field to the END of `saved_store_t` (old flash images then read
   0xFF), sanitise it in `saved_load()`, default it in
   `saved_seed_defaults()`.
2. Mutate through a `storage.c` function that calls `saved_write_flash()` —
   never write flash from more than 3 lines away from the field change.

---

## 9. Build & verify

- Build via Docker (Linux containers): `cd dev-setup && docker compose run
  --rm build-wetgreg-hub-rtos` → `wetgreg-hub-rtos/build/wetgreg_hub_rtos.uf2`.
  Always `PICO_BOARD=pico2_w`, `DISPLAY_VARIANT=V4` (the compose file sets
  both).
- A change isn't done until the Docker build is clean and (for behavior
  changes) it has run on the device — the panel-drain and cyw43-lock classes
  of bug only show up on hardware.

## 10. Reference standards

- [BARR-C:2018 Embedded C Coding Standard](https://barrgroup.com/sites/default/files/barr_c_coding_standard_2018.pdf) — module layout (4.3), header hygiene (4.2), `static` for private symbols (1.8.a), naming
- [The Power of Ten — Holzmann, NASA/JPL](https://spinroot.com/gerard/pdf/P10.pdf) — function length, no dynamic allocation, zero warnings
- [FreeRTOS docs — static vs dynamic allocation](https://www.freertos.org/Documentation/02-Kernel/02-Kernel-features/09-Memory-management/03-Static-vs-Dynamic-memory-allocation)
- [Quantum Leaps — Active Objects for Embedded Systems](https://www.state-machine.com/doc/AN_Active_Objects_for_Embedded.pdf) — the "block in one place, run to completion" discipline §3 follows
- Project docs: `dev-setup/wetgreg-hub-rtos/docs/` 03 (RTOS primer), 05
  (design/architecture), 06 (synchronization) — the task split's full rationale
