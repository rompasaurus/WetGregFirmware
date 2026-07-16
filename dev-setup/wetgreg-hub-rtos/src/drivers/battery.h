/**
 * battery.h — VSYS battery / USB-power sensing.
 *
 * Purpose: expose raw VSYS/USB sensing and the LiPo %-curve; the filtered
 * gauge and its sampling policy live in sys/power.c.
 *
 * On Pico W / Pico 2 W:
 *   - VSYS/3 sits on GPIO 29 / ADC3, but GPIO 29 is shared with the CYW43
 *     SPI bus, so reads lock the radio out and average many samples.
 *   - USB presence is detected via CYW43_WL_GPIO_VBUS_PIN (CYW43 GPIO 2).
 *
 * This module is the raw sensing + LiPo curve only; the filtered/published
 * battery state and the sampling policy live in sys/power.c (batt_sample).
 */
#ifndef BATTERY_H
#define BATTERY_H

#include <stdbool.h>

/* Battery-path voltage drop (ADDED BACK on battery only). The cell reaches the
 * Pico's VSYS through the board's series elements (D3 Schottky + slide switch),
 * so a full 4.20 V cell reads ~3.67 V at the ADC — about 0.53 V low. The USB
 * path doesn't have this drop, so the offset is applied ONLY to on-battery
 * samples (see batt_sample in power.c). */
#define VSYS_BATT_OFFSET 0.53f

/* Live (NOT persisted) calibration trim — resets to 1.0 every boot. Kept as a
 * bench sanity tool via the Device-Info UP/DOWN keys. */
extern float g_vsys_cal;

void  battery_init(void);
float read_vsys_raw_volts(void);   /* uncalibrated: divider math + board VSYS_CAL only */
float read_vsys_volts(void);       /* calibrated: raw * g_vsys_cal */
bool  is_usb_powered(void);
int   lipo_percent_hyst(float v);  /* LiPo discharge-curve % with display deadband */
void  battery_cal_save(float cal); /* clamp + apply the live trim */

#endif /* BATTERY_H */
