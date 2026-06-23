# Refresh-Rate Enhancement Research — Modos *Glider* / *Caster* vs. WetGreg Firmware

> **Goal of this document.** Glider (and its FPGA core *Caster*) drives e-paper at a continuous **~60 fps** with sub-frame latency. This report dissects *how* they do it — the algorithms, the gateware, the driving electronics — and maps each technique against our current stack (**RP2350 + WeAct 2.13″ / SSD1680 over SPI**) to determine what transfers, what doesn't, and what a realistic higher-refresh-rate path looks like for us.
>
> Prepared 2026-06-22. Sources are cited inline and listed in §11.

---

## Table of Contents

1. [Executive Summary (TL;DR)](#1-executive-summary-tldr)
2. [Our Current Stack — Baseline](#2-our-current-stack--baseline)
3. [How E-Paper Is Actually Driven (First Principles)](#3-how-e-paper-is-actually-driven-first-principles)
4. [The Glider Hardware](#4-the-glider-hardware)
5. [The Caster EPDC Core — Architecture & Algorithms](#5-the-caster-epdc-core--architecture--algorithms)
   - 5.1 [Top-level signals: it drives raw panel electrodes](#51-top-level-signals-it-drives-raw-panel-electrodes)
   - 5.2 [Per-pixel state machine (16-bit pixel state)](#52-per-pixel-state-machine-16-bit-pixel-state)
   - 5.3 [The waveform LUT system](#53-the-waveform-lut-system)
   - 5.4 [Update modes (A2 / fast-mono / grey / auto-LUT)](#54-update-modes-a2--fast-mono--grey--auto-lut)
   - 5.5 [Early cancellation & per-pixel regional update](#55-early-cancellation--per-pixel-regional-update)
   - 5.6 [The pixel-processing pipeline (dithering, de-gamma)](#56-the-pixel-processing-pipeline-dithering-de-gamma)
6. [Why "60 fps" Is Possible There But Not Here](#6-why-60-fps-is-possible-there-but-not-here)
7. [Side-by-Side Comparison Matrix](#7-side-by-side-comparison-matrix)
8. [What Transfers to RP2350 + SSD1680 (and What Doesn't)](#8-what-transfers-to-rp2350--ssd1680-and-what-doesnt)
9. [Options for Us — Ranked by Effort vs. Payoff](#9-options-for-us--ranked-by-effort-vs-payoff)
10. [Tooling Inventory (Glider/Caster vs. Ours)](#10-tooling-inventory-glidercaster-vs-ours)
11. [Sources](#11-sources)

---

## 1. Executive Summary (TL;DR)

- **Glider/Caster do not "speed up" an e-paper controller — they *replace* it.** They throw away the panel's integrated timing controller (TCON) entirely and drive the panel's **raw source- and gate-driver electrodes directly** from an FPGA, with an external high-voltage PMIC. ([Caster `caster.v`](https://github.com/Modos-Labs/Caster), [epdiy wiki](https://github.com/vroland/epdiy/wiki/How-pixels-are-driven-in-a-parallel-epaper-with-epdiy))
- **"60 fps" is the panel *scan* rate, not a per-image update rate.** The panel is scanned continuously at the video frame rate (DisplayPort/HDMI ~60 Hz). A full black↔white transition still takes multiple scan frames (~9 frames in Caster's fast-mono mode), but because the controller scans every frame and tracks each pixel independently, *latency to first motion* is one frame (~16 ms) instead of one full waveform (~300 ms).
- **The two enabling tricks are algorithmic and only possible with direct drive:**
  1. **Per-pixel state machine** — every pixel carries its own 16-bit state (mode + frame counter + target), so the controller can start, stop, or switch the waveform of any individual pixel on any frame.
  2. **Early cancellation** — if new pixel data arrives mid-transition, the controller recomputes the remaining drive time toward the *new* target instead of finishing the old one. This is what makes typing/cursor motion feel instant.
- **Our hardware fundamentally cannot do this as-is.** The WeAct 2.13″ panel has an **SSD1680 integrated controller** bonded to it. We talk to it over **4 MHz SPI**; it owns the source/gate drivers and the waveform engine internally. We hand it a framebuffer + a LUT, trigger, and wait ~**300 ms**. We have no access to the per-frame, per-pixel electrode timing that Caster relies on. Our ceiling on this panel is roughly **3–4 fps** (full custom-LUT partial) and maybe **8–15 fps** with an aggressive A2-style binary waveform + windowed updates.
- **What *does* transfer to us without new hardware:** (a) a faster, shorter **A2/DU-style binary waveform** LUT for monochrome animation; (b) **windowed/region partial updates** (only ship changed sub-rectangles to SSD1680 RAM); (c) **dithering** done our side for pseudo-grey; (d) double-buffering & input-coalescing (already done).
- **The only path to true Glider-class refresh on our side is a hardware change:** a **TCON-less ("raw"/"bare") e-paper panel** + an **EPD PMIC (e.g., TPS65185)** + **RP2350 PIO/DMA** generating the source/gate timing — i.e. porting the *epdiy/Caster* approach onto RP2350. The RP2350 (dual Cortex-M33 @ 150 MHz, PIO, DMA, HSTX) is actually a credible host for this on a small panel, but it is a board respin, not a firmware patch.

---

## 2. Our Current Stack — Baseline

Confirmed from the firmware (`dev-setup/wetgreg-hub-rtos/`) and `Dilder-PCB/`:

| Aspect | Value | Source |
|---|---|---|
| MCU | **RP2350** (Pico 2 W), dual Cortex-M33 @ 150 MHz, 520 KB SRAM | `Dilder-PCB/SCHEMATIC-OVERVIEW.md` |
| Panel | **WeAct 2.13″**, 122 × 250, B&W | `lib/e-Paper/EPD_2in13_V4.c` |
| Panel controller | **SSD1680** (integrated TCON, bonded to panel) | driver header |
| Interface | **SPI0, 4 MHz**, 4-wire + DC/RST/BUSY (GP17–22) | `lib/Config/DEV_Config.c:121` |
| Framebuffer | **3,844 B**, 1 bpp, MSB-first | driver |
| Refresh modes | Full (init/clear) + **custom-LUT partial** (flicker-free) | `EPD_2in13_V4_Display_Partial()` |
| Refresh time | **~300 ms** panel-side (BUSY-gated) | `EPD_2in13_V4_ReadBusy()` |
| Effective rate | **~3.3 fps** (display-limited, *not* MCU-limited) | timing instrumentation |
| RTOS | FreeRTOS SMP — Input/UI/HK on core 0, **Display on core 1** | `rtos_tasks.c` |
| Already-done opts | Custom V3 partial LUT (no black flash), 2 ms BUSY poll, double-buffer, display-paced input coalescing | `EPD_2in13_V4.c`, `rtos_tasks.c` |

**Key takeaway:** our bottleneck is 100% the SSD1680 waveform engine and the e-ink physics it commands — the RP2350 is idle most of the 300 ms. Throwing a faster MCU at it does nothing.

---

## 3. How E-Paper Is Actually Driven (First Principles)

To see why Caster is fast, you need the physical layer. An electrophoretic panel is a passive matrix of microcapsules over a TFT backplane with two driver families ([epdiy wiki](https://github.com/vroland/epdiy/wiki/How-pixels-are-driven-in-a-parallel-epaper-with-epdiy)):

- **Gate driver (rows):** a giant shift register. You shift in a single `1` then `0`s; the `1` walks down selecting one row at a time. Controlled by **SPV** (start pulse / vertical) and **CKV** (row clock), gated by **GMODE/OE**. Needs ±20–22 V to switch the TFT gates.
- **Source driver (columns):** applies the actual pixel drive voltage to every column of the selected row, latched per row. Each pixel gets **2 bits** per row-cycle:

  | bits | action |
  |---|---|
  | `00` / `11` | no drive (hold) |
  | `01` | push toward **black** (darken) |
  | `10` | push toward **white** (lighten) |

  Drive voltage is **±15 V** referenced to **VCOM** (~−1.5 to −2.5 V).
- **A "frame"** = one full top-to-bottom row scan applying one 2-bit instruction to every pixel. At a ~60–85 Hz scan rate, one frame is ~12–16 ms.
- **Grey levels and full transitions emerge from *repeating frames*.** "The more times we output black, the darker it gets." A complete black→white flip is ~9 frames; a grey target is a specific count of darken/lighten frames per the **waveform**. The waveform is the time-/temperature-indexed table that says, for a given (source state → target state), which 2-bit instruction to emit on each frame.

The integrated controllers (SSD1680, UC8151, IL0373…) bury *all* of the above inside the chip. You only get SPI: write image RAM, optionally write a LUT, send "go," poll BUSY. **You never see a frame or a row.** That abstraction is exactly what costs us the latency.

---

## 4. The Glider Hardware

Glider is the open-source e-ink *monitor*; the repo is the board + enclosure + STM32 firmware, while the gateware is the separate *Caster* core. ([Glider](https://github.com/Modos-Labs/Glider), [Crowd Supply](https://www.crowdsupply.com/modos-tech/modos-paper-monitor))

| Block | Part | Role |
|---|---|---|
| FPGA | **Xilinx Spartan-6 LX16** | runs Caster — generates all panel timing in real time |
| MCU | **STM32H750** | USB, firmware update, housekeeping (*not* in the pixel path) |
| Framebuffer | **DDR3-800** | holds pixel *state* (16 bit/pixel) + incoming video |
| Video in | USB-C **DisplayPort Alt-Mode** (PTN3460) or **DVI** via microHDMI (ADV7611) | the ~60 Hz pixel source |
| Power | ±15 V / +22 V / −20 V rails, up to 1 A peak | the high-voltage panel drive |
| Panels | TCON-less panels 4.3″–42″, mono + CFA (Kaleido/Triton/DES) | up to **280 MP/s** dithering off |

Two architectural facts matter for us:
1. The pixel datapath is **FPGA-only**. The MCU never touches pixels — it can't keep up, and doesn't need to. This is the inverse of our design, where the RP2350 *is* the controller.
2. There is a **dedicated high-voltage analog supply**. Direct drive is impossible without it.

---

## 5. The Caster EPDC Core — Architecture & Algorithms

Caster is ~64% Verilog, CERN-OHL-P licensed, top module `caster.v`. RTL inventory (from the repo tree):

```
rtl/caster.v               EPD controller top level
rtl/pixel_processing.v     combinational single-pixel waveform logic  ← the brain
rtl/wvfmlut.v              waveform lookup-table SRAM wrapper
rtl/csr.v                  SPI control/status registers
rtl/degamma.v              sRGB → linear
rtl/rgb2y.v                color → luma (Y) for CFA/mono
rtl/linear_4b_quantizer.v  16-level quantizer
rtl/bayer_dithering.v      ordered dithering
rtl/blue_noise_dithering.v blue-noise dithering (public-domain texture)
rtl/error_diffusion_*.v    Floyd–Steinberg-style diffusion
rtl/line_reverse.v         scan direction
rtl/spartan6/vin_*.v       video in: DVI / FPD-Link / DPI / internal
rtl/spartan6/{ddr3,memif,mig_wrapper}.v  DDR3 framebuffer
utils/caster_wvfm_asm/     C waveform "assembler" (CSV/INI → .mem)
utils/caster_extwvfm_asm/  external waveform converter (convert.py)
sim/                       C++ display simulator (dispsim, srcsim, vramsim)
```

### 5.1 Top-level signals: it drives raw panel electrodes

The `caster.v` port list is the proof that this is direct drive — these are *driver-chip* pins, not SPI:

```verilog
output wire        epd_gdoe;   // gate driver output enable (GMODE/OE)
output wire        epd_gdclk;  // gate clock (CKV) — advance row
output wire        epd_gdsp;   // gate start pulse (SPV) — top of frame
output wire        epd_sdclk;  // source driver clock
output wire        epd_sdle;   // source latch enable (commit a row)
output wire        epd_sdoe;   // source output enable
output wire [15:0] epd_sd;     // 16-bit parallel source data bus (the 2-bit codes, packed)
output wire        epd_sdce0;  // source driver chip enable
```

It also exposes the video input (`vin_pixel`, **4 pixels/clock, Y8**), a **64-bit framebuffer port** (`bi_pixel`/`bo_pixel` = 4 px × **16-bit state**), and an **SPI CSR** port for the host to set modes/registers. The clock is "4X/8X output clock rate" — the source bus is clocked many times faster than the row rate.

### 5.2 Per-pixel state machine (16-bit pixel state)

`pixel_processing.v` is the heart. **Every pixel stores 16 bits of state** in DDR3:

```
Bit 15-12: Mode
Bit 13-12: shared sub-field
... remaining bits: frame counter / target / previous value
```

Modes (localparams, verbatim):

```verilog
MODE_MANUAL_LUT_NO_DITHER   = 00xx
MODE_MANUAL_LUT_BLUE_NOISE  = 01xx
MODE_FAST_MONO_NO_DITHER    = 1000
MODE_FAST_MONO_BAYER        = 1001
MODE_FAST_MONO_BLUE_NOISE   = 1010
MODE_FAST_GREY              = 1011
MODE_AUTO_LUT_NO_DITHER     = 1100
MODE_AUTO_LUT_BLUE_NOISE    = 1101
```

Frame budgets (also verbatim) reveal the timing:

```verilog
FASTM_B2W_FRAMES      = 9;   // fast-mono black→white
FASTM_W2B_FRAMES      = 9;   // fast-mono white→black
FASTG_HOLDOFF_FRAMES  = 1;   // fast-grey
FASTG_B2G_FRAMES      = 2;
FASTG_W2G_FRAMES      = 2;
FASTG_SETTLE_FRAMES   = 5;
AUTOLUT_HOLDOFF_FRAMES= 60;
// grey→white frame count varies by previous level (7–9 frames)
```

So: a binary flip = 9 scan frames ≈ **~110–150 ms** to *fully settle*, but the pixel begins moving on **frame 1 (~16 ms)** and the controller is free to retarget it at any frame. The per-pixel frame counter (`op_framecnt`, `al_framecnt`) is what lets thousands of pixels each be at a different point in their own waveform simultaneously — the essence of "every pixel is its own update region."

### 5.3 The waveform LUT system

`wvfmlut.v` is a dual-read-port BRAM. It is written 8 bits at a time (12-bit write address) but **read 2 bits at a time** (14-bit read address; `addr[13:2]` picks the byte, `addr[1:0]` selects which of the four 2-bit fields):

```verilog
// 4 × 2-bit source codes packed per byte
douta = bram_douta[ {bsela*2 +: 2} ];   // -> 2-bit drive code (00/01/10/11)
```

The read address is composed from **(target level, current/source level, frame index)** — classic e-paper waveform indexing. The output 2-bit code is exactly the darken/lighten/hold instruction the source driver consumes (§3). The default table loads from `default_waveform.mem`, and the `utils/caster_wvfm_asm` C tool *assembles* human-readable CSV/INI waveforms into that `.mem` — Caster ships its own **waveform compiler toolchain**. Temperature compensation is handled by selecting different waveform sets (e-ink speed is strongly temperature-dependent).

### 5.4 Update modes (A2 / fast-mono / grey / auto-LUT)

- **Fast-mono (A2-like):** 1-bit, 9-frame binary waveform, optionally Bayer or blue-noise dithered for pseudo-grey. This is the low-latency, high-refresh path. No global flash.
- **Fast-grey:** a short 2-bit grey path (holdoff 1 + 2 + settle 5 frames) — quick coarse grey.
- **Manual-LUT:** host supplies a full waveform; pixel runs it to completion.
- **Auto-LUT (hybrid greyscale):** the headline trick. Update **binary first** for instant response, then after a 60-frame hold-off, **automatically re-render the same region in full grayscale** to recover contrast/detail once the image is static. User perceives instant motion *and* eventually-crisp grey. ([WebSearch summary](https://spectrum.ieee.org/e-paper-display-modos))

These modes are assignable **per pixel / per window**, on the fly, by writing the mode field — so a text cursor region can be A2 while a photo region is auto-LUT, in the same frame.

### 5.5 Early cancellation & per-pixel regional update

Because each pixel has independent state + frame counter and the panel is scanned every frame:

- **Regional update** = there is no "region." Each pixel is its own region; the host just writes new target values into DDR3 for whatever pixels changed, and the scan picks them up next frame. No "wait for previous update to finish."
- **Early cancellation** = if a pixel's target changes mid-waveform, `pixel_processing.v` recomputes drive frames toward the **new** target from the **current physical** state rather than completing the stale transition. This both cuts latency and prevents the "queue a full refresh per keypress" stutter. ([Crowd Supply](https://www.crowdsupply.com/modos-tech/modos-paper-monitor))

This is *only* expressible because Caster owns the per-frame electrode timing. An integrated controller gives you no frame to cancel on.

### 5.6 The pixel-processing pipeline (dithering, de-gamma)

Incoming video (Y8, 4 px/clk) flows: **`degamma` → `rgb2y` (for color/CFA) → `linear_4b_quantizer` → dithering (`bayer` / `blue_noise` / `error_diffusion`) → `pixel_processing` → waveform LUT → source bus.** Dithering is what lets a 1-bit-fast or 4-bit panel show photographic content; doing it in hardware is why it sustains 280 MP/s.

---

## 6. Why "60 fps" Is Possible There But Not Here

| Enabler in Caster | Why we can't replicate on SSD1680 |
|---|---|
| Drives raw gate/source electrodes; **scans the panel itself every ~16 ms** | SSD1680 owns the electrodes; we only get SPI "write RAM / go / BUSY". No frame to ride. |
| **Per-pixel 16-bit state in DDR3**, retargetable each frame | SSD1680 has 1-bit RAM (×2 for old/new); state machine is internal & fixed. |
| **Early cancellation** mid-waveform | No mid-waveform access — a triggered update runs to BUSY-clear (~300 ms). |
| **Hardware dithering @ 280 MP/s** in FPGA fabric | Possible in RP2350 software but irrelevant while the panel is the bottleneck. |
| **±15/±20/+22 V programmable PMIC** + parallel source bus | Our panel's HV supply & drivers are sealed inside the module. |
| 60 Hz video source (DP/DVI) as the clock | We have no continuous pixel stream; we push discrete frames. |

**Bottom line:** the 60 fps is a property of *direct, continuous, per-frame scanning of a TCON-less panel*. The SSD1680 architecture is categorically a "submit-and-wait" device. No amount of firmware turns one into the other.

---

## 7. Side-by-Side Comparison Matrix

| Dimension | **Glider / Caster** | **WetGreg (RP2350 + SSD1680)** |
|---|---|---|
| Controller location | FPGA gateware (Caster) | Inside the panel module (SSD1680) |
| Compute host | Spartan-6 LX16 + STM32H750 (housekeeping) | RP2350 dual M33 *is* the host |
| Panel | TCON-less, raw electrodes, 4.3″–42″ | Integrated-TCON WeAct 2.13″, 122×250 |
| Panel interface | Parallel gate/source drivers, ±15/±20 V | 4-wire SPI @ 4 MHz |
| Framebuffer | DDR3, **16 bit/pixel state** | 2× internal 1-bit RAM + our 3,844 B 1-bpp buffers |
| Refresh model | Continuous scan, per-pixel waveform | Submit full/partial buffer → BUSY ~300 ms |
| Per-pixel control | Yes (mode, frame counter, target) | No (window-level only) |
| Early cancellation | Yes | No |
| Grayscale | 2/4/16-level + hybrid auto-LUT + HW dither | 1-bpp (grey LUT exists but slow) |
| Effective rate | **~60 fps scan**, 1-frame latency to motion | **~3–4 fps** partial; ~8–15 fps theoretical A2 |
| Latency to first motion | ~16 ms | ~300 ms |
| Waveform | Programmable LUT + assembler toolchain | Custom 159-B partial LUT (borrowed from V3) |
| Power | Dedicated HV PMIC, ±rails, 1 A | Pico 3.3 V; HV generated inside panel module |

---

## 8. What Transfers to RP2350 + SSD1680 (and What Doesn't)

### ✅ Transfers (firmware-only, this hardware)
1. **Shorter A2/DU binary waveform.** Caster's fast-mono is a *short* binary waveform (≈9 frames, no flash). The SSD1680 *does* accept a custom LUT (we already load a 153-byte one). A more aggressive, fewer-phase monochrome waveform can cut partial-update time below 300 ms for pure B/W content — realistically into the **~70–150 ms** range (≈7–15 fps) at some cost in contrast/ghosting. **Highest-payoff firmware change.**
2. **Windowed / region partial updates.** Use SSD1680 RAM windowing (set X/Y window + cursor, registers 0x44/0x45/0x4E/0x4F) to write *only the changed sub-rectangle*. Less SPI data and, with a region-limited waveform, faster settle for small UI changes (cursor, counter, status line). This is our analog of "regional update" — at window granularity, not per-pixel.
3. **Host-side dithering** (Bayer / blue-noise, copy Caster's approach) to fake grey from 1-bpp — independent of refresh speed; improves perceived quality of fast frames.
4. **Pipeline hygiene we already have**: double buffering, display-paced input coalescing, 2 ms BUSY poll, Display pinned to core 1. Keep these; they're the same philosophy as Caster keeping the MCU out of the pixel path.
5. **A waveform toolchain.** Adopt Caster's idea of authoring waveforms in CSV/INI and assembling to a binary table, so we can iterate A2 waveforms quickly and add **temperature compensation** (pick LUT by measured temp — e-ink speed varies a lot).

### ❌ Does NOT transfer without new hardware
- Per-pixel state machine, early cancellation, true 60 fps continuous scan, hardware grayscale @ video rates — all require **owning the electrode timing**, which the SSD1680 forbids.

---

## 9. Options for Us — Ranked by Effort vs. Payoff

| # | Option | Effort | Expected result | Notes |
|---|---|---|---|---|
| **A** | **Custom A2/DU fast binary LUT** + tune frame count/voltages | Low (firmware) | ~3 fps → **~8–15 fps** for B/W animation; more ghosting | Best ROI. Periodic full refresh to clear ghosting. |
| **B** | **Windowed partial updates** (changed-rect only) | Low–Med (firmware) | Big latency win for small UI deltas | Combine with A. Track dirty rect in UI layer. |
| **C** | **Host-side dithering** for pseudo-grey | Low (firmware) | Better-looking fast frames, no speed cost | Port Bayer/blue-noise from Caster concept. |
| **D** | **Faster SPI** (4 MHz → 8–20 MHz) | Trivial | Shaves a few ms of transfer; **not** the bottleneck | SSD1680 SPI max ~20 MHz; only helps the ~1–2 ms shift-in, not the 300 ms waveform. Do it, but don't expect fps. |
| **E** | **Swap to a faster panel** still SPI/integrated (e.g. newer fast-refresh modules / E-Ink "Gallery"/A2-optimized parts) | Med (HW BOM) | Modest; still submit-and-wait | Stays in the SSD1680-class paradigm; no per-pixel control. |
| **F** | **Direct-drive respin: TCON-less panel + EPD PMIC (TPS65185) + RP2350 PIO/DMA** generating gate/source timing — port epdiy/Caster onto RP2350 | **High (board + firmware)** | **True Glider-class**: tens of fps, low latency, per-pixel possible | The only real route to 60 fps. RP2350 is a viable host (PIO state machines for source/gate timing, DMA from framebuffer, HSTX, dual M33). See §below. |

**On Option F (the serious one).** RP2350 is unusually well-suited to copy the *epdiy* trick (epdiy drives parallel e-paper from an ESP32's LCD/I2S peripheral). On RP2350 you would:
- Use a **TCON-less / "bare" e-paper panel** that exposes source+gate driver FPC lines.
- Add a **TPS65185** (the standard EPD PMIC: generates +15/−15/+22/−20 V and VCOM) — same family epdiy uses.
- Drive the **source data bus + SDCLK/LE/OE** with a **PIO** state machine fed by **DMA** from an SRAM framebuffer; drive **gate SPV/CKV/OE** with a second PIO SM.
- Implement the **per-pixel state + waveform LUT in software** on core 1 (we already dedicate core 1 to display). 520 KB SRAM holds a small-panel 2-bit framebuffer + waveform table comfortably; a 2.13″-class 122×250 panel is tiny (≈30 KB/plane).
- This gets you Caster's algorithms (fast-mono, dithering, early cancellation) without an FPGA, at small-panel scale. Latency to first motion ≈ one scan frame (~10–16 ms).

This is a hardware project (new FPC connector, PMIC, level shifting, a raw panel SKU) — but it is the documented, proven path, and RP2350 has the peripherals for it.

---

## 10. Tooling Inventory (Glider/Caster vs. Ours)

| Tool / artifact | Glider / Caster | Ours today |
|---|---|---|
| HDL / language | Verilog + SystemVerilog (`rtl/`) | C (Pico SDK) + FreeRTOS |
| Build / synthesis | Xilinx ISE (Spartan-6) | CMake + arm-none-eabi (`dev-setup/`) |
| Waveform authoring | `utils/caster_wvfm_asm` (CSV/INI → `.mem`), `caster_extwvfm_asm/convert.py` | Hand-coded LUT byte array in `EPD_2in13_V4.c` |
| Simulation | C++ display sim (`sim/dispsim`, `srcsim`, `vramsim`) | None for display |
| PMIC / HV | External ±15/±20/+22 V supply, 1 A | Internal to panel module |
| Video input | DP-Alt-Mode (PTN3460), DVI (ADV7611), FPD-Link | N/A — frames generated on-device |
| Framebuffer | DDR3-800, 16 bit/pixel | RP2350 SRAM, 1 bpp, double-buffered |
| Dithering | HW: Bayer, blue-noise, error-diffusion | None (opportunity — Option C) |
| Reference cousin | **epdiy** (ESP32 parallel direct-drive) — closest port target for RP2350 | — |

**Recommended adoptions for us:** the **waveform-assembler workflow** (Option A/E enabler), **dithering** (Option C), and — if Option F is ever greenlit — **epdiy** as the concrete reference implementation for RP2350-side direct drive.

---

## 11. Sources

- Modos *Glider* (open-source e-ink monitor): <https://github.com/Modos-Labs/Glider>
- Modos *Caster* (FPGA EPDC core): <https://github.com/Modos-Labs/Caster> — verbatim from `README.md`, `rtl/caster.v`, `rtl/wvfmlut.v`, `rtl/pixel_processing.v`, repo tree
- Modos Paper Monitor — Crowd Supply: <https://www.crowdsupply.com/modos-tech/modos-paper-monitor>
- IEEE Spectrum, "E-Paper Displays Get a Speed Boost With Modos' Open Source Kit": <https://spectrum.ieee.org/e-paper-display-modos>
- epdiy — "How pixels are driven in a parallel epaper": <https://github.com/vroland/epdiy/wiki/How-pixels-are-driven-in-a-parallel-epaper-with-epdiy>
- epdiy project: <https://github.com/vroland/epdiy>
- TI TPS65185 (EPD PMIC): <https://www.ti.com/product/TPS65185>
- Solomon Systech SSD1680 datasheet: <https://www.crystalfontz.com/controllers/SolomonSystech/SSD1680>
- Our firmware: `dev-setup/wetgreg-hub-rtos/lib/e-Paper/EPD_2in13_V4.c`, `lib/Config/DEV_Config.c`, `rtos_tasks.c`; `Dilder-PCB/SCHEMATIC-OVERVIEW.md`

---

*End of report.*
