# 11 — Troubleshooting (field diagnosis)

This chapter is the practical "a unit is misbehaving — what now?" guide. It was
written after a real round of field failures (three units showing "snow" and
refusing to boot) that turned out to be **e-ink BUSY-line solder faults**, not
dead silicon. The firmware now logs enough over serial to diagnose almost any
unit in one glance.

---

## 0. The master tool: read the serial log

Plug in USB and open the log (DevTool **Serial Monitor**, or `cat /dev/ttyACM1`).
Two lines tell you almost everything:

- **`[disp] refresh N ms`** — the e-ink health meter (printed every refresh when
  `EPD_TIMING` is on).
- **`[BATT] … rail=… | last on-battery=…`** — the power meter.

```
REFRESH-TIME METER                 BATTERY / CHARGE METER
~700 ms  → display HEALTHY          USB rail ~4.6 V            → VSYS read OK
~20 ms   → BUSY open / dead         last on-battery 3.9–4.2 V  → cell healthy
~5000 ms → BUSY stuck HIGH          last on-battery <3.3 V / 0 → drained/latched cell
(no [disp] lines at all → Display task hung = BUSY stuck, pre-timeout build)
```

> The `last on-battery` trick exists because serial needs USB, but USB hides the
> battery. The firmware retains the last on-battery reading and prints it while
> on USB — so: run on battery a few seconds, replug USB, read the line.

---

## 1. First question: is the board alive?

| Can you flash it (picotool / DevTool)? | Meaning |
|---|---|
| **Yes** | MCU + USB + flash programming are **fine** — the fault is a peripheral (display/battery), flash content, or the host. Go to §2. |
| **No `2e8a` in `lsusb`** | **Host USB wedge** (not the board) → replug / `sudo bash tools/devtool/fix_pico_usb.sh`, or hold **BOOTSEL** + replug. |

---

## 2. Symptom → test → cause → fix

| Symptom | Quick test (serial) | Likely cause | Fix |
|---|---|---|---|
| Snow, won't boot, flashes fine | `refresh ~20 ms` | e-ink **BUSY open/disconnected** (GP22) | **Reseat FPC**; reflow GP22 / J1 pin 6 joint. |
| Snow, frozen / "won't boot" | `refresh ~5000 ms` or **no `[disp]` lines** | e-ink **BUSY stuck HIGH** | **Reseat FPC**; clear a **solder bridge to 3V3** at the BUSY corner. |
| Boots, no auto-rotate / steps | `[ACCEL] NOT DETECTED` | accel missing / cold joint | Solder/reseat **SC7A20** — *cosmetic, not fatal*; firmware runs without it. |
| Battery stuck ~1%, never charges | `last on-battery <3.3 V`, won't rise | **charge starvation** (load on battery node) and/or **drained cell** | Charge with the Pico off; power-diet firmware; measure cell; Rev 2 power-path. |
| "Full" (STDBY) LED dim-blinking on USB | n/a (visual) | TP4056 **oscillating** — starvation or **latched/dead cell** | Charge with load removed → solid = starvation; still blinking = bad cell. |
| Won't boot, flashes fine, but `refresh ~700 ms` | display OK, app still wrong | **corrupted flash** (brownout during a write) | DevTool **Full Erase** → reflash. |
| Cell hot / puffy, LED never greens | n/a (visual) | over-discharged / damaged cell | ⚠️ Replace the cell; don't recharge a damaged LiPo. |

---

## 3. The e-ink BUSY repair loop

The e-ink is on **J1, a 2×4 header**. BUSY (pin 6) sits in a corner next to DC
(pin 4) and a power pin (pin 8) — the classic spot for a bridge or cold joint.

```
Reseat FPC → re-power → read [disp] refresh:
   ~700 ms  → FIXED (image should appear)
   ~5000 ms → BUSY still stuck HIGH  → clear bridge to 3V3 at the BUSY corner
   ~20 ms   → BUSY open / low        → reflow GP22↔J1 pin6; check flex seating
```

Continuity (power off): **BUSY ↔ GP22 pad** must beep (connected);
**BUSY ↔ GND** and **BUSY ↔ 3V3** must **not** beep (no bridge). The signal must
be continuous all the way **GP22 → J1 pin 6 → flex → panel BUSY**.

---

## 4. Battery / charge isolation test

```
1. Power the Pico OFF, charge JUST the TP4056 + cell:
     solid red → solid green  → charges fine alone → it was LOAD STARVATION
     still dim-blinking        → BAD / LATCHED cell
2. Multimeter on the cell:
     3.6–4.2 V → healthy (problem is the load/charge path)
     <3.0 V / 0 V / won't rise → over-discharged, protection latched → replace
3. Check the battery connector polarity/keying (reverse-insert can fry the TP4056).
```

---

## 5. Why the firmware no longer hard-freezes on bad hardware

Two boot-time waits used to be unbounded and could hang a task forever:

- **Accel I²C probe** — now uses `i2c_*_timeout_us` (see `mpu_init` /
  `mpu_read_*`). A missing or bus-stuck accelerometer can never hang boot; it
  just logs `NOT DETECTED` and `mpu_ok` stays false.
- **e-ink `EPD_2in13_V4_ReadBusy`** — now capped at ~3 s. A panel with BUSY stuck
  asserted can't freeze the Display task; it degrades to the slow refresh time
  (~5 s) that flags the fault, and the rest of the device stays alive.

Net effect: a dead sensor or a bad display **degrades to a diagnosable state**
instead of bricking the unit at boot — and the refresh-time/battery meters above
make the cause obvious.
