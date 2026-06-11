# WetGregFirmware

Firmware and tooling for the **Dilder hub** — a FreeRTOS application running on a
**Raspberry Pi Pico 2 W (RP2350)** soldered onto the **Dilder PCB**, driving a
**WeAct 2.13" B&W e-ink** display.

This repository is a deliberately **narrow** extraction of the larger Dilder
project. It carries exactly one firmware target and a single-purpose dev tool, so
there is nothing to configure and no unrelated hardware to reason about:

| Setting      | Value                                            |
|--------------|--------------------------------------------------|
| Firmware     | `dilder-hub-rtos` (FreeRTOS / SMP)               |
| Board        | `pico2_w` — Raspberry Pi Pico 2 W (RP2350)        |
| Display      | `V4` — WeAct 2.13" B&W e-ink (SSD1680 controller) |
| e-ink wiring | Dilder PCB — SPI0, GP17–22                         |

---

## Table of contents

1. [Quick start](#quick-start)
2. [What's in here (repository map)](#whats-in-here-repository-map)
3. [Directory-by-directory reference](#directory-by-directory-reference)
4. [The build & flash pipeline](#the-build--flash-pipeline)
5. [The DevTool](#the-devtool)
6. [Dependencies & submodules](#dependencies--submodules)
7. [Best practices — using the project](#best-practices--using-the-project)
8. [Best practices — adding to the project](#best-practices--adding-to-the-project)
9. [Conventions & invariants](#conventions--invariants)
10. [Troubleshooting](#troubleshooting)

---

## Quick start

```bash
# 1. Clone with submodules
git clone --recurse-submodules <repo-url> WetGregFirmware
cd WetGregFirmware
#    (already cloned without --recurse-submodules?)
git submodule update --init --recursive

# 2. Install the toolchain (Docker, ARM gcc, picotool, tk, pyserial, udev rules)
./install-deps.sh

# 3. Plug in the Dilder (Pico 2 W) over USB, then either:
python3 tools/devtool/devtool.py          # GUI → Flash tab → Clean Build & Flash
#    …or headless:
python3 tools/devtool/devtool.py build-flash
```

The first build is slow (Docker pulls Ubuntu and clones the pico-sdk). Every
build after that is cached and takes well under a minute.

---

## What's in here (repository map)

```
WetGregFirmware/
├── README.md                     ← you are here (project overview)
├── install-deps.sh               ← one-shot toolchain installer (Arch/Debian/Fedora)
├── .gitignore                    ← ignores build artifacts, caches, local secrets
├── .gitmodules                   ← submodule pins (FreeRTOS-Kernel, picowota)
│
├── dev-setup/                    ← BUILD CONTEXT: firmware + Docker build infra
│   ├── dilder-hub-rtos/          ← the firmware itself (the heart of the repo)
│   ├── Dockerfile                ← Ubuntu + ARM toolchain + pico-sdk image
│   ├── docker-compose.yml        ← the single `build-dilder-hub-rtos` service
│   └── version.h                 ← shared firmware version string
│
├── FreeRTOS-Kernel/              ← submodule: the RTOS scheduler (pinned commit)
├── picowota/                     ← submodule: optional Wi-Fi OTA bootloader
│
├── tools/
│   └── devtool/                  ← the WetGreg DevTool (build/flash/debug GUI + CLI)
│       ├── devtool.py            ← single-file tool (GUI tabs + CLI subcommands)
│       ├── DEVTOOL_GUIDE.md      ← full user guide (rendered in the Docs tab)
│       └── requirements.txt      ← pyserial (serial monitor only)
│
└── assets/
    └── emotion-previews/         ← reference PNGs of the hub's emotion states
```

---

## Directory-by-directory reference

### `dev-setup/` — the build context

Everything Docker needs to compile the firmware lives here. The directory is the
Docker **build context** and the working directory for `docker compose`.

- **`dev-setup/dilder-hub-rtos/`** — the firmware. Self-contained: it keeps its
  *own* copy of the display library (so we can patch the e-ink driver) and its
  own SDK/RTOS import shims. Key files:

  | File / dir              | Role                                                                 |
  |-------------------------|----------------------------------------------------------------------|
  | `CMakeLists.txt`        | The build recipe — sources, libraries, display variant, OTA option.  |
  | `main.c`                | Application entry + the bulk of the app logic.                        |
  | `rtos_tasks.c/.h`       | FreeRTOS task orchestration (Input / Display / Housekeeping).         |
  | `freertos_hooks.c`      | Kernel-required callbacks (idle/timer memory, stack-overflow hook).   |
  | `bt.c/.h`, `dilder.gatt`| Bluetooth LE (BTstack) + the GATT profile compiled into `dilder.h`.   |
  | `FreeRTOSConfig.h`      | RTOS tuning (heap model, priorities, SMP).                            |
  | `lwipopts.h`            | lwIP (TCP/IP) options for the FreeRTOS networking stack.              |
  | `btstack_config.h`      | BTstack feature configuration.                                        |
  | `wifi_config.h`         | Wi-Fi/NTP/timezone defaults (**placeholders — never commit secrets**).|
  | `pico_sdk_import.cmake` | Locates the Pico SDK (copied from the SDK at build time too).         |
  | `FreeRTOS_Kernel_import.cmake` | Locates the FreeRTOS kernel via `FREERTOS_KERNEL_PATH`.        |
  | `lib/`                  | Vendored display stack (see below).                                   |
  | `docs/`                 | The firmware's own deep-dive docs (RTOS primer, memory, build, etc.). |
  | `build/`                | Compiler output (`.uf2`/`.elf`) — **git-ignored**.                    |

  Inside `lib/`:

  | Subdir         | Contents                                                            |
  |----------------|---------------------------------------------------------------------|
  | `lib/e-Paper/` | Waveshare/WeAct e-ink panel drivers (`EPD_2in13_V4.c` is the WeAct). |
  | `lib/GUI/`     | `GUI_Paint` — the 1-bit drawing primitives (text, shapes, buffer).  |
  | `lib/Fonts/`   | Bitmap fonts (`font8`…`font24`).                                     |
  | `lib/Config/`  | `DEV_Config.c/.h` — the HAL: SPI/GPIO pins, delays, RTC shim.        |

- **`dev-setup/Dockerfile`** — builds an Ubuntu 24.04 image with
  `gcc-arm-none-eabi`, CMake, Ninja, and a fresh `pico-sdk` clone at
  `/opt/pico-sdk`. The default `CMD` configures and builds with CMake + Ninja.

- **`dev-setup/docker-compose.yml`** — defines the single `build-dilder-hub-rtos`
  service. It mounts the firmware at `/project`, the FreeRTOS kernel at
  `/FreeRTOS-Kernel` (via `FREERTOS_KERNEL_PATH`), and picowota at `/picowota`,
  and pins `PICO_BOARD=pico2_w` / `DISPLAY_VARIANT=V4`.

### `FreeRTOS-Kernel/` (submodule)

The FreeRTOS scheduler and kernel objects (tasks, queues, mutexes), plus its
own nested port submodules. The firmware links `FreeRTOS-Kernel` and
`FreeRTOS-Kernel-Heap4`. Pinned to a specific commit for reproducibility.

### `picowota/` (submodule)

[picowota](https://github.com/usedbytes/picowota) — a Wi-Fi OTA bootloader.
**Optional**: only used when the firmware is built with `-DPICOWOTA_OTA=ON`. The
default USB clean-build/flash flow does not touch it.

### `tools/devtool/`

The **WetGreg DevTool** — a single Python file that is both a Tkinter GUI (three
tabs: Flash / Debug / Docs) and a headless CLI. See [The DevTool](#the-devtool)
and `tools/devtool/DEVTOOL_GUIDE.md` for the full reference.

### `assets/emotion-previews/`

Reference PNG renders of the hub's emotion states (`angry`, `chill`, `excited`,
…) — both static and `-anim` variants. Design reference, not compiled into
firmware.

---

## The build & flash pipeline

The build runs **inside Docker** so your host doesn't need an exact SDK version.

```
        ┌────────────────────────── host ──────────────────────────┐
        │  python3 devtool.py build-flash                          │
        │        │                                                  │
        │        ▼                                                  │
        │  docker compose build  ─────────►  image: ubuntu + arm-gcc + pico-sdk
        │        │                                                  │
        │        ▼                                                  │
        │  docker compose run  ───────────►  cmake -G Ninja \       │
        │        │                              -DPICO_BOARD=pico2_w \
        │        │                              -DDISPLAY_VARIANT=V4 │
        │        │                            ninja                  │
        │        ▼                                                  │
        │  build/dilder_hub_rtos.uf2                                │
        │        │                                                  │
        │        ▼                                                  │
        │  picotool reboot -f -u  → BOOTSEL drive → copy .uf2 → eject → board reboots
        └──────────────────────────────────────────────────────────┘
```

Before each build the DevTool rewrites the vendored e-ink HAL onto the Dilder
PCB wiring (SPI0, GP17–22), so the firmware always matches the board.

To build manually without the DevTool:

```bash
cd dev-setup
docker compose run --rm -e DISPLAY_VARIANT=V4 -e PICO_BOARD=pico2_w build-dilder-hub-rtos
# → dev-setup/dilder-hub-rtos/build/dilder_hub_rtos.uf2
```

---

## The DevTool

`python3 tools/devtool/devtool.py [command]`

| Tab       | Capability                                                                 |
|-----------|----------------------------------------------------------------------------|
| **Flash** | picotool setup • Device Info / Reboot to BOOTSEL / Full Erase • Clean Build & Flash • Flash (existing) • Build Only |
| **Debug** | Environment diagnostics (one-click toolchain check) • Serial monitor (115200) |
| **Docs**  | Renders `DEVTOOL_GUIDE.md` with a clickable TOC and search                  |

CLI (headless / scriptable):

```bash
devtool.py info | flash [uf2] | build | build-flash | reboot | erase | deps | gui
```

`deps` exits non-zero if anything is missing — safe to gate CI on. Full details
in **[`tools/devtool/DEVTOOL_GUIDE.md`](tools/devtool/DEVTOOL_GUIDE.md)**.

---

## Dependencies & submodules

| Dependency        | How it's provided                          | Needed for                  |
|-------------------|--------------------------------------------|-----------------------------|
| Pico SDK          | Cloned **inside** the Docker image          | Every build                 |
| FreeRTOS-Kernel   | Git submodule (pinned)                       | Every build                 |
| picowota          | Git submodule (pinned)                       | Optional OTA builds only    |
| ARM toolchain     | Inside Docker (host copy optional)           | Every build                 |
| picotool          | Host (`install-deps.sh` / package manager)   | Flashing without BOOTSEL btn |
| Tkinter           | Host                                         | DevTool GUI                 |
| pyserial          | Host (`requirements.txt`)                    | DevTool serial monitor      |

Submodules must be initialised after cloning:

```bash
git submodule update --init --recursive
```

---

## Best practices — using the project

- **Change firmware → `Clean Build & Flash`.** It deletes `build/` first, so you
  never chase a stale-object ghost.
- **Didn't change firmware → `Flash (existing build)`.** Skips the ~minute compile.
- **"What's actually on the board?" → `Device Info`.** Confirms the program
  name/version baked into the running firmware.
- **"Why won't it build?" → Debug → `Run diagnostics`.** It points straight at the
  missing piece (daemon down, submodule not initialised, picotool absent…).
- **"What does it print?" → Debug → Serial monitor.** Live `printf` at 115200.
- **Bricked it (snow / won't boot after a brownout) → `Full Erase`, then
  `Clean Build & Flash`.**
- **Never commit real Wi-Fi credentials.** `wifi_config.h` ships placeholders;
  pass real values via CMake `-D` flags or a git-ignored local copy.
- **Keep `build/` out of git.** It's already ignored — don't force-add it.

## Best practices — adding to the project

- **Firmware source changes** live under `dev-setup/dilder-hub-rtos/`. Add new
  `.c` files to the `add_executable(...)` list in its `CMakeLists.txt` and add
  any new libraries to `target_link_libraries(...)`.
- **Keep the display library vendored.** The e-ink driver in `lib/` is patched
  for this board; do not replace it with an unmodified upstream copy. If you must
  re-vendor, re-apply the SPI0 / GP17–22 wiring (the DevTool does this
  automatically at build time via `apply_eink_wiring`).
- **Pin dependencies.** When bumping a submodule, commit the new pinned commit
  and note why. Reproducible builds depend on it.
- **Extend the DevTool, don't fork it.** `devtool.py` keeps all core logic in
  module-level functions (`docker_build`, `flash_uf2`, `clean_build_and_flash`,
  `device_info`, …) so the GUI and CLI share one implementation. Add a feature as
  a function first, then wire a button and a CLI subcommand to it.
- **Document in `DEVTOOL_GUIDE.md`.** It's rendered live in the Docs tab — a new
  button without a guide entry is half-finished. Hit **Reload** in the tab to see
  edits immediately.
- **Match the surrounding style.** The firmware is heavily commented for
  teaching; new code should keep that altitude. Python uses 4-space indent and
  the existing logging conventions (`[build]`, `[flash]`, `[picotool]` prefixes).

## Conventions & invariants

- **One target, on purpose.** Board = `pico2_w`, display = `V4`, wiring = Dilder
  PCB. If you need another board/display, that belongs in the upstream Dilder
  repo, not here.
- **The `.uf2` name is `dilder_hub_rtos.uf2`** (derived from the CMake project
  name). The DevTool and docs assume this.
- **BOOTSEL drive label is `RP2350`** (RP2040 boards mount as `RPI-RP2`; both are
  handled, but this board is RP2350).
- **Eject = logical unmount, never SCSI power-off.** The bootrom auto-reboots on
  a complete `.uf2`; a power-off eject can wedge the USB port.

---

## Troubleshooting

| Symptom                                  | Fix                                                                    |
|------------------------------------------|------------------------------------------------------------------------|
| `Docker is not running`                  | `sudo systemctl start docker`; add yourself to the `docker` group.     |
| Build fails on `FreeRTOS-Kernel`         | `git submodule update --init --recursive`.                             |
| `picotool not found`                     | `install-deps.sh`, your package manager, or the Flash tab's Install.   |
| Flash: "BOOTSEL drive didn't appear"     | Manual BOOTSEL once (hold button, plug in), flash, then picotool works.|
| Serial monitor: permission denied        | Add yourself to `uucp` (Arch) / `dialout` (Debian) and re-login.       |
| Board shows snow / won't boot            | **Full Erase**, then **Clean Build & Flash**.                          |
| `Tkinter is not installed`               | Install `tk` / `python3-tk`, or use the CLI commands.                  |

More detail (and a per-button reference) lives in
[`tools/devtool/DEVTOOL_GUIDE.md`](tools/devtool/DEVTOOL_GUIDE.md).
