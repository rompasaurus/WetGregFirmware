# WetGreg DevTool — User Guide

A focused build / flash / debug companion for the **wetgreg-hub-rtos** firmware.

This guide is rendered live inside the DevTool's **Docs** tab, and it also reads
fine as a normal Markdown file. Whenever you are unsure what a button does, this
is the place to look.

---

# 1. What this tool is

The WetGreg DevTool is the slimmed-down descendant of the original multi-board
WetGreg DevTool. The original supported many boards, displays, on-device
"programs", an image editor, an emulator, and a dozen tabs. **This one does only
what you need to ship the hub firmware**, organised into three tabs:

| Tab       | What it's for                                                            |
|-----------|--------------------------------------------------------------------------|
| **Flash** | Set up picotool, read device info, and deploy firmware (build + flash).   |
| **Debug** | A serial monitor for live `printf` output, plus environment diagnostics. |
| **Docs**  | This guide, with a clickable table of contents and search.               |

Everything is locked to **one** target so there is nothing to configure:

| Setting      | Value                                            |
|--------------|--------------------------------------------------|
| Firmware     | `wetgreg-hub-rtos`                                |
| Board        | `pico2_w` — Raspberry Pi Pico 2 W (RP2350)        |
| Display      | `V4` — WeAct 2.13" B&W e-ink (SSD1680 controller) |
| e-ink wiring | WetGreg PCB — SPI0, GP17–22                         |

> The WetGreg PCB carries a **soldered Pico 2 W module**, so to the toolchain it
> behaves exactly like a Pico 2 W (RP2350 silicon, `RP2350` BOOTSEL drive).

---

# 2. Quick start

If your machine already has Docker, the ARM toolchain, and picotool, the whole
loop is two clicks:

1. Plug the WetGreg (Pico 2 W) into USB.
2. Open the DevTool: `python3 tools/devtool/devtool.py`
3. On the **Flash** tab, click **Clean Build & Flash**.

That's it — Docker compiles the firmware from scratch, picotool reboots the
board into BOOTSEL, copies the `.uf2`, and the board reboots into the new
firmware. Watch the **Log** panel at the bottom for progress.

Already built once and just changed nothing on the firmware? Click
**Flash (existing build)** to skip the ~minute-long compile.

---

# 3. Installation

## 3.1 System dependencies

You need: Docker, the ARM cross-compiler, CMake, Ninja, Git, Tkinter, and
picotool. On Arch / CachyOS:

```bash
sudo pacman -S --needed docker docker-compose cmake ninja git \
                        arm-none-eabi-gcc arm-none-eabi-newlib \
                        tk python python-pyserial
# picotool is AUR-only on Arch/CachyOS (and building it from the SDK fails with
# GCC 15) — install it from the AUR, which carries the fix:
paru -S picotool      # or: yay -S picotool
```

On Debian / Ubuntu:

```bash
sudo apt install docker.io docker-compose cmake ninja-build git \
                 gcc-arm-none-eabi libnewlib-arm-none-eabi \
                 python3-tk python3-serial picotool
```

Or just run the bundled installer from the repo root:

```bash
./install-deps.sh
```

Make sure your user can talk to Docker and to the serial port:

```bash
sudo usermod -aG docker $USER     # then log out / back in
sudo usermod -aG uucp   $USER     # Arch serial group (Debian: dialout)
```

## 3.2 Submodules

The firmware depends on the **FreeRTOS-Kernel** (and, for optional OTA builds,
**picowota**). They are git submodules — after cloning:

```bash
git submodule update --init --recursive
```

The **Debug → Run diagnostics** button verifies all of the above in one click.

## 3.3 Python dependencies

The GUI itself needs nothing beyond the standard library. The serial monitor
uses `pyserial`:

```bash
pip install -r tools/devtool/requirements.txt
```

---

# 4. The Flash tab

This is where you spend 95% of your time. It has three sections.

## 4.1 picotool

**picotool** is Raspberry Pi's USB utility. It can reboot a running Pico into
BOOTSEL mode and flash a `.uf2` *without you touching the BOOTSEL button or
unplugging anything*. That is what makes the one-click flow possible.

- The status line shows whether picotool was found and where.
- **Install picotool (from SDK)** builds it from your Pico SDK if it's missing.
  (Installing it from your package manager is usually easier — see §3.1.)
- **Refresh** re-checks after you install it.

> If the board's firmware is hung and picotool can't reach it, do a manual
> BOOTSEL **once** (hold BOOTSEL, plug in, release), flash known-good firmware,
> and picotool works on its own from then on.

## 4.2 Device

Quick picotool actions against the connected board:

- **Device Info** — runs `picotool info -a` and prints the flash id, the program
  name/version baked into the firmware, the binary's memory map, and the device
  type. The fastest way to confirm *what is actually running* on the board.
- **Reboot to BOOTSEL** — puts the Pico into BOOTSEL (USB drive) mode and leaves
  it there, so you can flash by any method (drag-and-drop, `cp`, etc.).
- **Full Erase (recovery)** — wipes the **entire** flash (firmware, saved
  settings, any bootloader). Use this only to rescue a board that shows snow or
  won't boot after a brownout corrupted its flash. It leaves the board in
  BOOTSEL, ready to re-flash. **You must flash firmware again afterward.**

## 4.3 Deploy firmware

The three buttons that get firmware onto the board:

- **Clean Build & Flash** — the headline action. Steps:
  1. Delete `dev-setup/wetgreg-hub-rtos/build/` (a *clean* build, no stale state).
  2. Retarget the e-ink driver onto the WetGreg PCB wiring (SPI0, GP17–22).
  3. `docker compose build` the toolchain image (cached after the first run).
  4. `docker compose run` → CMake + Ninja compile `wetgreg_hub_rtos.uf2`.
  5. picotool reboots the board to BOOTSEL and copies the `.uf2`.
  Use this whenever you've changed firmware source.

- **Flash (existing build)** — skip the compile and flash the last
  `build/wetgreg_hub_rtos.uf2`. Instant; use it when the source hasn't changed
  (e.g. you reflashed by accident, or moved the board to another machine).

- **Build Only** — compile but don't flash. Handy for catching compile errors
  without a board attached, or for producing a `.uf2` to flash elsewhere.

- **Browse .uf2…** — pick any `.uf2` to flash instead of the default build
  (e.g. an OTA combined image, or a known-good backup).

The **progress bar** and **status line** track the active operation; full output
streams into the **Log** panel at the bottom of the window.

---

# 5. The Debug tab

## 5.1 Environment diagnostics

**Run diagnostics** checks everything the build/flash flow needs and shows a
green `OK` or red `FAIL` per item:

| Check                     | Why it matters                                             |
|---------------------------|-----------------------------------------------------------|
| cmake / ninja / arm-gcc   | The cross-compile toolchain (also lives inside Docker).    |
| docker + docker daemon    | The build runs in a container; the daemon must be running. |
| git                       | Needed to fetch the SDK and submodules.                    |
| picotool                  | Needed for flashing without the BOOTSEL button.            |
| FreeRTOS-Kernel submodule | The firmware won't link without it.                        |
| Pico serial port          | Confirms the board is enumerated as `/dev/ttyACM*`.        |

This is the first thing to run when something "won't build" — it usually points
straight at the missing piece.

## 5.2 Serial monitor

A live terminal onto the board's USB serial (`printf` output, 115200 baud):

- **Port** — auto-detects the Pico (Raspberry Pi USB VID `2E8A`). Use **Refresh**
  if you plugged in after opening the tool.
- **Connect / Disconnect** — open or close the serial link. The label turns
  green when connected.
- **Clear** — empty the output view.
- **Save Log…** — dump the captured output to a timestamped text file.
- The input box at the bottom sends a line (Enter or **Send**). **Ctrl+C** sends
  an interrupt byte; **Reset (Ctrl+D)** sends a soft-reset byte.

> The serial monitor needs `pyserial`. If it's missing you'll get a clear prompt
> to `pip install pyserial`; the rest of the tool is unaffected.

---

# 6. The Docs tab

You're reading what it renders. It loads `tools/devtool/DEVTOOL_GUIDE.md` and
shows:

- a **Contents** sidebar built from the headings — click any entry to jump to it;
- **Search** — type a term, press Find, and every match is highlighted; **Clear**
  removes the highlights;
- **Reload** — re-read the Markdown file if you've edited it.

To extend the docs, just edit `DEVTOOL_GUIDE.md` and hit **Reload**.

---

# 7. Command-line use (headless)

Everything the Flash tab does is also a CLI command, for scripting or flashing
over SSH where there's no display:

```bash
python3 tools/devtool/devtool.py info          # picotool device info
python3 tools/devtool/devtool.py flash         # flash the existing build
python3 tools/devtool/devtool.py flash fw.uf2  # flash a specific .uf2
python3 tools/devtool/devtool.py build         # clean Docker build only
python3 tools/devtool/devtool.py build-flash   # clean build, then flash
python3 tools/devtool/devtool.py reboot        # reboot the Pico into BOOTSEL
python3 tools/devtool/devtool.py erase         # full chip erase (recovery)
python3 tools/devtool/devtool.py deps          # check the toolchain
python3 tools/devtool/devtool.py gui           # launch the GUI (also the default)
```

`deps` exits non-zero if anything is missing, so it's safe to use in CI.

---

# 8. How the build works

```
WetGregFirmware/
├── dev-setup/
│   ├── wetgreg-hub-rtos/        ← the firmware (main.c, rtos_tasks.c, bt.c, lib/…)
│   │   ├── CMakeLists.txt       ← the build recipe
│   │   └── build/               ← output (.uf2/.elf) — git-ignored
│   ├── Dockerfile               ← Ubuntu + ARM toolchain + pico-sdk
│   └── docker-compose.yml       ← the single build-wetgreg-hub-rtos service
├── FreeRTOS-Kernel/             ← submodule (the RTOS scheduler)
├── picowota/                    ← submodule (optional Wi-Fi OTA bootloader)
└── tools/devtool/               ← this tool
```

The build runs **inside Docker** so you don't have to install the exact SDK
version on your host:

1. The `Dockerfile` builds an Ubuntu image with `gcc-arm-none-eabi`, CMake,
   Ninja, and a fresh clone of the **pico-sdk** at `/opt/pico-sdk`.
2. `docker-compose.yml` mounts the firmware at `/project` and the
   **FreeRTOS-Kernel** submodule at `/FreeRTOS-Kernel` (pointed at via
   `FREERTOS_KERNEL_PATH`), and **picowota** at `/picowota`.
3. The container runs `cmake -G Ninja -DPICO_BOARD=pico2_w -DDISPLAY_VARIANT=V4`
   then `ninja`, producing `build/wetgreg_hub_rtos.uf2`.

Because the kernel and SDK are pinned (submodule commit + the SDK clone), builds
are reproducible across machines.

## 8.1 e-ink wiring

The WetGreg PCB routes the WeAct 2.13" panel to **SPI0, GP17–22**:

| Signal | GPIO |
|--------|------|
| CS     | 17   |
| CLK    | 18   |
| MOSI   | 19   |
| DC     | 20   |
| RST    | 21   |
| BUSY   | 22   |

Before every build the tool rewrites the vendored `lib/Config/DEV_Config.c` to
these pins (idempotent), so the firmware always matches the board.

## 8.2 Wi-Fi credentials (optional)

Networking features read `dev-setup/wetgreg-hub-rtos/wifi_config.h`. The checked-in
file has placeholders. To use real credentials without committing them, pass
them at build time via CMake `-D` flags, or keep a local copy — never commit real
passwords.

---

# 9. Troubleshooting

| Symptom                                   | Fix                                                                                 |
|-------------------------------------------|-------------------------------------------------------------------------------------|
| `Docker is not running`                   | Start it: `sudo systemctl start docker`. Add yourself to the `docker` group.        |
| Build fails on `FreeRTOS-Kernel`          | `git submodule update --init --recursive`.                                           |
| `picotool not found`                      | Click **Install picotool** (uses the AUR on Arch), or install per §3.1.              |
| picotool install fails on `elf2uf2.cpp` / `bintool.cpp` | GCC 15 can't compile picotool from source. On Arch use the AUR package: `paru -S picotool`. The DevTool's Install button now does this for you. |
| Flash: "BOOTSEL drive didn't appear"      | The reboot didn't take. Do a manual BOOTSEL once, flash, then picotool works again.  |
| `reboot failed — is the Pico running?`    | The firmware is hung. Manual BOOTSEL (hold button, plug in), then Flash.             |
| Serial monitor: no ports                  | Click **Refresh**. Check the board isn't in BOOTSEL mode, and the cable is data-capable. |
| Serial monitor: permission denied         | Add yourself to `uucp` (Arch) / `dialout` (Debian) and re-login.                    |
| Board shows snow / won't boot (brownout)  | **Full Erase (recovery)**, then **Clean Build & Flash**.                             |
| `pyserial is not installed`               | `pip install pyserial` (only the serial monitor needs it).                          |
| `Tkinter is not installed`                | Install `tk` (Arch) / `python3-tk` (Debian), or use the CLI commands (§7).           |

---

# 10. At a glance

- **Change firmware → Clean Build & Flash.**
- **Didn't change firmware → Flash (existing build).**
- **"What's on the board?" → Device Info.**
- **"Why won't it build?" → Debug → Run diagnostics.**
- **"What does it print?" → Debug → Serial monitor.**
- **Bricked it → Full Erase, then Clean Build & Flash.**
