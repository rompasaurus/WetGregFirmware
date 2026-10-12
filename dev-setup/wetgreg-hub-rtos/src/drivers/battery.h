/**
 * battery.h — raw battery / USB-power sensing + the LiPo %-curve.
 *
 * Purpose: expose the raw cell-voltage estimate, USB detection, the
 * persisted per-board calibration, and the LiPo discharge curve. The
 * filtered gauge, the sampling policy and the time-left estimate live in
 * sys/power.c (batt_sample).
 *
 * ─── What the hardware actually lets us measure (Dilder PCB Rev 1) ───────────
 * From the KiCad netlist (Dilder Full PCB Rev 1.kicad_pcb):
 *
 *   cell (CN1) ─ SW1 slide switch ─┬─ TP4056 BAT pin (U4056.5), C1 10 µF
 *                                   └─ D3  BAT54 Schottky (A→K) ─ /VSYS ─ Pico pin 39
 *   USB-C VBUS ─┬─ Pico pin 40 (VBUS) ─ Pico's own Schottky ─ /VSYS
 *               └─ D1 SS14 ─ TP4056 VCC   (solar: CN2 ─ D2 SS14 ─ TP4056 VCC)
 *
 *   The Pico's 200k/100k divider feeds VSYS/3 to ADC3 (GPIO 29). GPIO 26/27/28
 *   and ADC_VREF are NOT connected on Rev 1, so there is no direct cell sense.
 *
 * Consequences:
 *   1. The ADC sees  V_cell − V_f(D3)  on battery. A BAT54's forward drop is
 *      load dependent: ~0.33 V at 1 mA, ~0.40 V at 10 mA, ~0.5 V at 30 mA,
 *      ~0.7 V at 100 mA (plus ~3 Ω of series resistance during radio bursts).
 *      It also varies part-to-part and with temperature. A fixed constant can
 *      only be right at one load, on one board.
 *   2. On USB the Pico is fed from VBUS (VSYS ≈ 4.6–4.8 V) and D3 is reverse-
 *      biased, so the cell is INVISIBLE while charging (and the Pico's load
 *      does NOT come out of the battery — the TP4056 charges it at its full
 *      programmed current; R3 = 12 kΩ → ~100 mA).
 *   3. VSYS ≥ 4.45 V can only happen on USB (a LiPo tops out at 4.20 V and the
 *      diode only lowers it), which gives a USB detector that needs no CYW43.
 *
 * How this driver copes:
 *   - peak-hold sampling in power.c picks the lightest-load instant, so what
 *     remains is the drop at the QUIET baseline current;
 *   - that remaining drop (plus the board's ADC gain/offset error) is folded
 *     into ONE additive, PERSISTED per-board offset (g_batt_offset_mv), which
 *     is auto-anchored to 4.19 V the moment USB is unplugged after a long
 *     charge (the cell sits at the charger's 4.20 V float in the CV phase), and
 *     can be anchored by hand from Device Info.
 *
 * Rev 2 / bodge: a 1 MΩ + 1 MΩ divider from the battery+ pad (post-switch) to
 * GPIO 26 (ADC0) with 100 nF to GND measures the cell directly and makes all
 * of the above moot — build with -DBATT_DIRECT_ADC_GPIO=26 to use it.
 */
#ifndef BATTERY_H
#define BATTERY_H

#include <stdbool.h>
#include <stdint.h>

/* ─── Sense path selection ─────────────────────────────────────────────────
 * Undefined (default): VSYS/3 on ADC3 behind D3 (Rev 1 as built).
 * -DBATT_DIRECT_ADC_GPIO=26 : direct cell divider on that ADC-capable GPIO
 *                             (26/27/28 → ADC0/1/2), ratio BATT_DIRECT_DIVIDER. */
#ifdef BATT_DIRECT_ADC_GPIO
  #ifndef BATT_DIRECT_DIVIDER
  #define BATT_DIRECT_DIVIDER 2.0f      /* 1 MΩ : 1 MΩ */
  #endif
#endif

/* Default additive offset (mV) added to the VSYS reading on battery: the D3
 * drop at the quiet baseline plus this board's ADC-path error. 530 mV is the
 * value MEASURED on the WetGreg PCB (a full 4.20 V cell read 3.67 V). The
 * idle-hook WFI lowers the baseline current and so the real drop a little,
 * which makes this slightly optimistic — the auto/manual anchor converges it
 * per board; it is only the starting point. (An estimate of 460 mV was tried
 * and read a low cell as 0 bars, so stay with the measured number.) */
#ifndef BATT_OFFSET_DEFAULT_MV
#define BATT_OFFSET_DEFAULT_MV 530
#endif
#define BATT_OFFSET_MIN_MV 150          /* sanity clamps for the stored offset */
#define BATT_OFFSET_MAX_MV 900

/* The cell voltage the auto/manual anchor targets. Straight off the TP4056 the
 * cell is pinned at the 4.20 V float; it relaxes ~10–30 mV in the first minutes. */
#define BATT_ANCHOR_TARGET_V 4.19f

/* VSYS at/above this can only be the USB path (cell ≤ 4.20 V minus a diode). */
#define BATT_USB_VSYS_THRESHOLD_V 4.45f

/* Persisted per-board offset, mV (loaded by saved_load → battery_set_offset_mv). */
extern uint16_t g_batt_offset_mv;
/* Where the current offset came from: 'D' default, 'A' auto-anchor, 'M' manual. */
extern char     g_batt_offset_src;

void  battery_init(void);

/* Tell the driver whether the CYW43 arch is up. When it is, GPIO 29 is the
 * radio's SPI clock and every read must take the cyw43 lock; when it is down
 * (deep-sleep power-down) the read is lock-free and USB presence is inferred
 * from VSYS instead of the CYW43's VBUS GPIO. Default: up. */
void  battery_set_cyw43_up(bool up);

float read_vsys_raw_volts(void);   /* what the ADC sees: VSYS (or the direct cell) — NO offset */
bool  is_usb_powered(void);        /* CYW43 VBUS pin when available, else VSYS ≥ 4.45 V */

/* Raw → estimated resting cell volts (adds the offset on the VSYS path). */
float battery_cell_from_raw(float raw_v);

/* Calibration. The offset is additive (the diode drop is additive; the ADC
 * gain error is < ±3 % and matters < 20 mV across the 3.4–4.2 V span). */
void  battery_set_offset_mv(uint16_t mv, char src);   /* clamp + apply (RAM only) */
void  battery_offset_persist(uint16_t mv, char src);  /* clamp + apply + write to flash */
void  battery_anchor_to(float raw_peak_v, char src);  /* offset := 4.19 V − raw (clamped), persisted */
void  battery_offset_reset(void);                     /* back to the default, persisted */

int   lipo_percent(float v);       /* resting-voltage discharge curve → 0..100 */
int   lipo_percent_hyst(float v);  /* same with a 2 % display deadband */

#endif /* BATTERY_H */
