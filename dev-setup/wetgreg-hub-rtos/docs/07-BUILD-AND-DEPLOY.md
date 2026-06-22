# 07 — Build and Deploy

How to compile the firmware and get it onto the device — by command line and via the
DevTool GUI, over both USB and Wi-Fi (OTA). Assumes you've done **01-ENVIRONMENT-SETUP.md**
once.

---

## 1. The two build flavours

| Flavour | Command flag | Produces | When to use |
|---|---|---|---|
| **USB** | (default) | `wetgreg_hub_rtos.uf2` | normal dev loop; flash by USB |
| **Wi-Fi OTA** | `-DPICOWOTA_OTA=ON` | `picowota_wetgreg_hub_rtos.uf2` (flash once) + `wetgreg_hub_rtos.elf` (push over Wi-Fi) | updating a device wirelessly |

Both target `-DPICO_BOARD=pico2_w` (the RP2350 + Wi-Fi board) and `-DDISPLAY_VARIANT=V4`.

---

## 2. USB build (command line)

```bash
cd dev-setup/wetgreg-hub-rtos
mkdir -p build && cd build
cmake -G Ninja -DPICO_BOARD=pico2_w -DDISPLAY_VARIANT=V4 ..
ninja
```

Output: `build/wetgreg_hub_rtos.uf2`.

Flash it:
- **Hold BOOTSEL**, plug in USB → a drive named `RP2350` appears → copy the `.uf2` onto it.
- Or, if current firmware allows: `picotool load -x build/wetgreg_hub_rtos.uf2`.

**Incremental builds:** after the first `cmake`, you only re-run `ninja`. Re-run `cmake`
only when you add files or change build options. To start clean: `rm -rf build`.

---

## 3. Wi-Fi OTA build (command line)

OTA lets you replace the firmware over Wi-Fi instead of touching USB. It works by flashing,
**once**, a small "bootloader + app" combined image; thereafter you push new app images
wirelessly.

```bash
cd dev-setup/wetgreg-hub-rtos
mkdir -p build-ota && cd build-ota
cmake -G Ninja -DPICO_BOARD=pico2_w -DPICOWOTA_OTA=ON -DDISPLAY_VARIANT=V4 \
      -DWIFI_SSID="YourNetwork" -DWIFI_PASS="YourPassword" ..
ninja
picotool uf2 convert picowota_wetgreg_hub_rtos.elf picowota_wetgreg_hub_rtos.uf2   # if a .uf2 isn't already present
```

First-time install (USB, once): flash `picowota_wetgreg_hub_rtos.uf2` via BOOTSEL.

Subsequent updates (wireless):
1. On the device, hold the **joystick UP at power-on** → it enters the OTA bootloader and
   joins Wi-Fi, showing its IP.
2. From your PC, push the standalone app:
   ```bash
   # using the picowota client bundled in tools/devtool, or serial-flash:
   serial-flash tcp:<device-ip>:4242 build-ota/wetgreg_hub_rtos.elf
   ```
3. It reboots into the new firmware.

> **Why two files?** `picowota_wetgreg_hub_rtos.uf2` is the bootloader+app you install once.
> `wetgreg_hub_rtos.elf` (from `build-ota/`, linked at offset `0x1005b000`) is the **payload**
> the bootloader accepts over Wi-Fi. A normal `build/` image is linked at `0x10000000` and
> **cannot** be OTA'd — always push the `build-ota/` ELF.

---

## 4. Using the DevTool GUI (easiest)

```bash
cd tools/devtool
python3 devtool.py
```

`wetgreg-hub-rtos` is registered as a selectable variant. Relevant tabs:

- **Programs** — select **WetGreg Hub RTOS**; it generates `quotes.h` into the folder.
- **Picotool** — clean-build + USB-flash the selected firmware (no BOOTSEL button needed if
  the running firmware supports `picotool reboot`).
- **Pico 2 W OTA** — pick `wetgreg-hub-rtos` in the firmware tree, enter Wi-Fi creds, click
  **Build Bootloader** (now builds the *selected* variant's combined image), flash it once
  over USB, then push updates over Wi-Fi.

The DevTool path constants assume firmware lives at `dev-setup/<name>/`, so
`wetgreg-hub-rtos` is auto-discovered; the build/flash/OTA helpers resolve
`build/wetgreg_hub_rtos.uf2` and `build-ota/…` automatically.

---

## 5. Build-time options reference

| CMake flag | Meaning | Default |
|---|---|---|
| `-DPICO_BOARD=pico2_w` | target board (RP2350 + Wi-Fi). **Required.** | — |
| `-DDISPLAY_VARIANT=V4` | e-ink panel revision (V2/V3/V3a/V4) | V4 |
| `-DPICOWOTA_OTA=ON` | build the Wi-Fi OTA combined image | OFF |
| `-DWIFI_SSID=…` / `-DWIFI_PASS=…` | bake in default Wi-Fi creds (compile define) | placeholders |
| `-DFREERTOS_KERNEL_PATH=…` | override the FreeRTOS kernel location | `../../FreeRTOS-Kernel` |

Environment: `PICO_SDK_PATH` must point at the SDK (see doc 01).

---

## 6. Verifying a build without hardware

You can confirm a build is sane on your PC even without a Pico:

```bash
# correct chip + secure (FreeRTOS-secure-only) image?
picotool info build/wetgreg_hub_rtos.elf | grep -E "target chip|image type"
# how big is it (text=flash code, bss=RAM)?
arm-none-eabi-size build/wetgreg_hub_rtos.elf
# did the RTOS actually link in?
arm-none-eabi-nm build/wetgreg_hub_rtos.elf | grep -E "vTaskStartScheduler|async_context_freertos"
```

Expect: `target chip: RP2350`, `image type: ARM Secure`, a few hundred KB of text, and the
FreeRTOS/cyw43 symbols present. What you **can't** verify off-device — input latency, BLE
pairing, the OTA round-trip, the idle-freeze — must be tested on the real board.

---

## 7. Common build issues

See the troubleshooting table in **01-ENVIRONMENT-SETUP.md §8** — the frequent ones are a
missing FreeRTOS nested submodule (`git submodule update --init --recursive`), an unset
`PICO_SDK_PATH`, and a missing generated `quotes.h` (run the DevTool Programs tab).
