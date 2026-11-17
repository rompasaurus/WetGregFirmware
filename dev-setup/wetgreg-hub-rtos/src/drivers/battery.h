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
 * drop at the quiet baseline plus this board's ADC-path error.
 *
 * 530 mV was MEASURED on the WetGreg PCB (a full 4.20 V cell read 3.67 V) when
 * the firmware idled at 150 MHz, roughly 25-30 mA. At the 48 MHz operating
 * point (power.h, AWAKE_CLK_48MHZ) the idle draw is around a third of that,
 * and a BAT54 drops ~70 mV less per decade of current plus ~3 ohm of series
 * resistance: (0.5 decade x 70 mV) + (18 mA x 3 ohm) = ~90 mV less. Hence 450.
 * This is an ESTIMATE until a board is put on a meter — it is only the
 * starting point; the auto/manual anchor converges the real value per board.
 * BATT_OFFSET_CLOCK_SHIFT_MV is the same step, applied once (storage.c) to an
 * offset that was anchored under the old 150 MHz firmware. */
#ifndef BATT_OFFSET_DEFAULT_MV
#define BATT_OFFSET_DEFAULT_MV 450
#endif
#define BATT_OFFSET_CLOCK_SHIFT_MV 80
#define BATT_OFFSET_MIN_MV 150          /* sanity clamps for the stored offset */
#define BATT_OFFSET_MAX_MV 900

/* The cell voltage the auto/manual anchor targets. Straight off the TP4056 the
 * cell is pinned at the 4.20 V float; it relaxes ~10–30 mV in the first minutes. */
#define BATT_ANCHOR_TARGET_V 4.19f

/* USB presence is read off VSYS itself — NOT off the CYW43's VBUS pin.
 *
 * On battery VSYS is the cell minus D3: a full 4.20 V cell reads ~3.7-3.9 V
 * and nothing can lift it higher. On USB it is VBUS minus the Pico's own
 * Schottky: 4.4-4.7 V. So VSYS >= 4.10 V means USB (4.00 V to drop back out).
 *
 * Asking the radio chip instead costs an ioctl per sample, which (a) wakes the
 * chip's bus twice a second so it never idles, and (b) is catastrophic when
 * the chip stops answering: the driver busy-waits a full SECOND per ioctl
 * with the radio lock held ("[CYW43] STALL ... timeout"), which turned a
 * 2 Hz battery sample into a core spinning flat out — seen in the field on a
 * unit up 20 h — while USB was reported as "battery" the whole time. */
#define BATT_USB_VSYS_ON_V   4.10f
#define BATT_USB_VSYS_OFF_V  4.00f

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

/* A VSYS reading below this is not a battery or a USB rail, it is a failed
 * read (radio asleep / pin contention): ignore it. The Pico's regulator cannot
 * even run from less. */
#define BATT_RAW_VALID_MIN_V 2.50f

float read_vsys_raw_volts(void);   /* what the ADC sees: VSYS (or the direct cell) — NO offset */
int   battery_radio_fail_count(void);   /* consecutive reads whose radio ioctl failed (0 = healthy) */
bool  battery_usb_from_raw(float raw_v);   /* USB verdict for a VSYS reading (with hysteresis) */
bool  is_usb_powered(void);        /* fresh read + verdict (direct-sense builds ask the CYW43) */

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
