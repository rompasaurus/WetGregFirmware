# 01 — Environment Setup (from a blank machine)

This is the complete, step-by-step recipe to install **every** tool, library, and
dependency needed to build, flash, and modify `wetgreg-hub-rtos`. It assumes you have
**never** built embedded C before. Follow it top to bottom once; after that, building is
two commands.

> **Mental model for a JS dev:** there is no `npm install` that fetches everything from
> one `package.json`. Embedded C needs three separate things installed by hand: (1) a
> **cross-compiler** (a compiler that runs on your PC but produces ARM code for the
> chip), (2) the **Pico SDK** (Raspberry Pi's hardware library), and (3) the **FreeRTOS
> kernel** (the operating system). We wire them together with **CMake**.

---

## 0. What you are installing, and why

| Tool | Role | JS analogy |
|---|---|---|
| **arm-none-eabi-gcc** | Cross-compiler: turns our C into RP2350 machine code | the thing that "runs" your code, but it can't run on your PC — only on the chip |
| **CMake** | Build configurator: reads `CMakeLists.txt`, generates build files | `vite`/`webpack` config that sets up the real build |
| **Ninja** | Build runner: actually invokes the compiler, fast & parallel | the task runner that executes the build |
| **Pico SDK** | Raspberry Pi's C library for the chip (GPIO, Wi-Fi, USB, …) | a big platform SDK / standard library |
| **FreeRTOS-Kernel** | The real-time operating system itself | (no JS analogy — there's no OS in a browser) |
| **picotool** | Talks to the Pico over USB (info, reboot, flash) | a CLI deploy tool |
| **Python 3** | Runs the DevTool GUI and generates `quotes.h` | Node, basically |
| **git** | Version control + fetches the submodules | git |

Versions this project is known to build with (what's on the reference machine):

- arm-none-eabi-gcc **14.2.0**
- CMake **4.3.3** (anything ≥ 3.13 is fine)
- Ninja **1.13.2**
- Pico SDK **2.2.0**
- FreeRTOS-Kernel **main** (commit `d877cd5`) + its RP2350 port submodule
- picotool **2.2.0**
- Python **3.14** (anything ≥ 3.9 is fine for the DevTool)

---

## 1. Install the host tools

### Linux (Arch / CachyOS — the reference machine)

```bash
sudo pacman -S --needed \
    arm-none-eabi-gcc arm-none-eabi-newlib \
    cmake ninja git python python-pip \
    picotool
```

### Linux (Debian / Ubuntu)

```bash
sudo apt update
sudo apt install -y \
    gcc-arm-none-eabi libnewlib-arm-none-eabi \
    cmake ninja-build git python3 python3-pip \
    build-essential
# picotool: build from source (below) or `sudo apt install picotool` on newer releases
```

### macOS (Homebrew)

```bash
brew install --cask gcc-arm-embedded
brew install cmake ninja git python picotool
```

Verify each is found:

```bash
arm-none-eabi-gcc --version   # → 14.x
cmake --version               # → ≥ 3.13
ninja --version
picotool version
python3 --version
```

---

## 2. Get the Raspberry Pi Pico SDK (with its submodules)

The SDK is a git repo and **must** be cloned `--recurse-submodules` (it pulls in cyw43,
lwIP, BTstack, TinyUSB — all of which we use). Pick a permanent home for it:

```bash
cd ~
mkdir -p pico && cd pico
git clone --branch 2.2.0 --recurse-submodules https://github.com/raspberrypi/pico-sdk.git
```

Then tell every shell where it lives by adding this to `~/.bashrc` / `~/.zshrc`:

```bash
export PICO_SDK_PATH="$HOME/pico/pico-sdk"
```

Open a new terminal (or `source ~/.bashrc`) so `$PICO_SDK_PATH` is set.

> **Why an env var?** The project's `pico_sdk_import.cmake` shim looks up
> `PICO_SDK_PATH` to find the SDK. No env var → the build can't locate the SDK.

---

## 3. Get the WetGreg repo (with **all** submodules, recursively)

The firmware lives in the WetGreg repo, which itself has submodules:
`picowota` (the Wi-Fi updater) and **`FreeRTOS-Kernel`**. Critically, **FreeRTOS-Kernel
has its OWN submodules** (the RP2350 CPU port lives in a nested
`Community-Supported-Ports` submodule). So you must clone **recursively**:

```bash
cd ~/COdingProjects        # or wherever you keep code
git clone --recurse-submodules <wetgreg-repo-url> WetGreg
cd WetGreg
```

If you already cloned without `--recurse-submodules`, fix it:

```bash
git submodule update --init --recursive
```

> **This is the #1 gotcha.** If FreeRTOS's nested port submodule is missing, CMake fails
> with *"does not contain a 'rp2350-arm-s' port"*. The fix is always
> `git submodule update --init --recursive`.

Confirm the pieces are present:

```bash
ls FreeRTOS-Kernel/tasks.c                                              # kernel source
ls FreeRTOS-Kernel/portable/ThirdParty/Community-Supported-Ports/GCC/RP2350_ARM_NTZ/   # the RP2350 port
ls picowota/CMakeLists.txt                                              # the OTA bootloader
```

---

## 4. (One-time) generate `quotes.h`

The octopus's quotes are generated into each firmware folder by the DevTool, so
`quotes.h` is **not** checked in (it's machine-generated, like a build artifact). Two
ways to create it:

**A. Via the DevTool GUI** (recommended — also how you'll deploy):

```bash
cd tools/devtool
python3 -m pip install -r requirements.txt   # if a requirements file exists
python3 devtool.py
# → Programs tab → select "WetGreg Hub RTOS" → it writes dev-setup/wetgreg-hub-rtos/quotes.h
```

**B. Quick shortcut:** the RTOS variant uses the *same* quotes as the original, so you
can simply copy the original's generated header if you have it:

```bash
cp dev-setup/wetgreg-hub/quotes.h dev-setup/wetgreg-hub-rtos/quotes.h
```

(If `dev-setup/wetgreg-hub/quotes.h` doesn't exist either, run the DevTool once for the
original too — it generates both.)

---

## 5. Build it (USB image)

```bash
cd dev-setup/wetgreg-hub-rtos
mkdir -p build && cd build
cmake -G Ninja -DPICO_BOARD=pico2_w -DDISPLAY_VARIANT=V4 ..
ninja
```

Success looks like `Linking CXX executable wetgreg_hub_rtos.elf` and a
`wetgreg_hub_rtos.uf2` in the `build/` folder.

> **What the flags mean:**
> - `-G Ninja` → use the Ninja build runner (fast).
> - `-DPICO_BOARD=pico2_w` → target the Pico 2 W board (RP2350 + Wi-Fi). **Required** —
>   this selects the dual-core chip and the Wi-Fi/BT hardware.
> - `-DDISPLAY_VARIANT=V4` → the WeAct 2.13" V4 e-ink panel the WetGreg uses.

The build finds FreeRTOS via `PICO_SDK_PATH` (for the SDK) and the project's
`set(FREERTOS_KERNEL_PATH …/../../FreeRTOS-Kernel)` line in `CMakeLists.txt` (for the
kernel). If you keep the kernel elsewhere, pass `-DFREERTOS_KERNEL_PATH=/path` or export
`FREERTOS_KERNEL_PATH`.

---

## 6. Flash it onto the device

**Option A — USB, hold the button:**
1. Unplug the Pico. Hold its **BOOTSEL** button and plug in USB. It mounts as a USB drive
   named `RP2350`.
2. Copy `build/wetgreg_hub_rtos.uf2` onto that drive. It reboots into the firmware.

**Option B — picotool, no button** (if the device is running firmware that supports it):
```bash
picotool load -x build/wetgreg_hub_rtos.uf2
```

**Option C — the DevTool**, Picotool or OTA tab, with `wetgreg-hub-rtos` selected. See
**07-BUILD-AND-DEPLOY.md** for the full OTA (over-Wi-Fi) flow.

---

## 7. Wi-Fi credentials (only if you want networking)

Networking is off until you connect from the menu. To bake in a default network, either
edit `wifi_config.h` (kept as placeholders in git for safety) **or** pass them to CMake:

```bash
cmake -G Ninja -DPICO_BOARD=pico2_w -DDISPLAY_VARIANT=V4 \
      -DWIFI_SSID="YourNetwork" -DWIFI_PASS="YourPassword" ..
```

> **Never commit a real password.** The build injects them as compile defines, so you
> don't have to put them in a tracked file.

---

## 8. Troubleshooting cheat-sheet

| Symptom | Cause | Fix |
|---|---|---|
| CMake: *"does not contain a 'rp2350-arm-s' port"* | FreeRTOS nested submodule missing | `git submodule update --init --recursive` |
| CMake: *"PICO_SDK_PATH ... not found"* | env var unset | `export PICO_SDK_PATH=$HOME/pico/pico-sdk` |
| `#error configENABLE_FPU must be defined` | wrong/edited `FreeRTOSConfig.h` | restore the ARMv8-M defines (see that file) |
| `fatal error: FreeRTOS.h: No such file` | kernel not linked | check `FREERTOS_KERNEL_PATH` and the `include(FreeRTOS_Kernel_import.cmake)` line |
| `quotes.h: No such file` | header not generated | run the DevTool Programs tab, or copy from `../wetgreg-hub` |
| No `RP2350` drive on BOOTSEL | held button too late / cable is power-only | re-plug holding BOOTSEL; use a data cable |
| Build links but Wi-Fi/BT dead on device | expected on a desktop "build only" check | it only runs on the real board; flash and test there |

Once this all works once, your everyday loop is just: edit code → `ninja` → flash.
