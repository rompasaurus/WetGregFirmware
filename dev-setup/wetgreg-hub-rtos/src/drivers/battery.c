/**
 * battery.c — VSYS battery / USB-power sensing on GPIO 29 (ADC3).
 *
 * Purpose: implement the cyw43-locked VSYS ADC read (trimmed mean), USB
 * detection, the LiPo curve, and the live calibration trim.
 *
 * GPIO 29 is shared with the CYW43 SPI clock: every read locks the radio out
 * (cyw43_thread_enter), reclaims the pin as an ADC input, waits out the
 * settling transient, then takes a trimmed mean over 48 samples.
 */
#include "battery.h"

#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "hardware/adc.h"

/* Board-level fixed correction (ADC-ref / divider tolerance). Compile-time.
 * On top of this sits g_vsys_cal — a runtime trim the user can nudge from the
 * Device Info screen (UP = "battery is full → treat this reading as 4.20 V"). */
#ifndef VSYS_CAL
#define VSYS_CAL 1.0f
#endif

float g_vsys_cal = 1.0f;        /* live runtime calibration trim (not persisted) */

static bool battery_adc_ready = false;

void battery_init(void) {
    adc_gpio_init(29);          /* disable digital pulls on ADC3 */
    battery_adc_ready = true;
}

/* Uncalibrated VSYS (battery) voltage — raw divider math + board VSYS_CAL only. */
float read_vsys_raw_volts(void) {
    if (!battery_adc_ready) battery_init();

    /* GPIO 29 is shared with the CYW43 SPI clock — lock the radio out while we read. */
    cyw43_thread_enter();
    adc_gpio_init(29);              /* reclaim pin as ADC input */
    adc_select_input(3);            /* ADC3 = GP29 = VSYS/3 */
    /* GP29 was just toggling as the SPI clock; the VSYS/3 divider node needs to
     * settle back to its analog level. Wait, then DISCARD the first conversions —
     * they sample the settling transient and read LOW, which was pegging the gauge
     * near 0% once the radio (BLE scan / WiFi) started running continuously. */
    sleep_us(800);
    for (int i = 0; i < 8; i++) (void)adc_read();
    uint16_t s[48];
    const int N = 48;
    for (int i = 0; i < N; i++) s[i] = (uint16_t)adc_read();
    cyw43_thread_exit();

    /* Trimmed mean: sort, drop the lowest/highest 25%, average the middle —
     * shrugs off the CYW43-SPI switching noise coupled onto the shared pin. */
    for (int i = 1; i < N; i++) {
        uint16_t v = s[i]; int j = i - 1;
        while (j >= 0 && s[j] > v) { s[j + 1] = s[j]; j--; }
        s[j + 1] = v;
    }
    uint32_t acc = 0; int lo = N / 4, hi = N - N / 4, cnt = 0;
    for (int i = lo; i < hi; i++) { acc += s[i]; cnt++; }
    float raw = (float)acc / (float)cnt;

    /* 3:1 divider, 3.3 V ADC ref. */
    return raw * 3.3f / 4095.0f * 3.0f * VSYS_CAL;
}

/* Calibrated VSYS (battery) voltage. */
float read_vsys_volts(void) {
    return read_vsys_raw_volts() * g_vsys_cal;
}

bool is_usb_powered(void) {
    /* CYW43 is always initialised at boot — VBUS sense is reliable */
    return cyw43_arch_gpio_get(CYW43_WL_GPIO_VBUS_PIN);
}

/* Single-cell LiPo voltage -> % via a piecewise discharge curve. A straight
 * (V-3.0)/1.2 line badly misreports the flat mid-range (e.g. 3.7 V is ~45%
 * real, but linear claims ~58%). Points are resting voltages; under load the
 * reading sags, so treat % as approximate. */
static int lipo_percent(float v) {
    static const float curve[][2] = {
        {4.20f,100},{4.15f,95},{4.11f,90},{4.08f,85},{4.02f,80},
        {3.98f,75},{3.95f,70},{3.91f,65},{3.87f,60},{3.85f,55},
        {3.84f,50},{3.82f,45},{3.80f,40},{3.79f,35},{3.77f,30},
        {3.75f,25},{3.73f,20},{3.71f,15},{3.69f,10},{3.61f,5},
        {3.50f,2},{3.30f,0},
    };
    const int N = (int)(sizeof(curve) / sizeof(curve[0]));
    if (v >= curve[0][0])   return 100;
    if (v <= curve[N-1][0]) return 0;
    for (int i = 0; i < N - 1; i++) {
        if (v <= curve[i][0] && v > curve[i+1][0]) {
            float span = curve[i][0] - curve[i+1][0];
            float frac = (v - curve[i+1][0]) / span;
            return (int)(curve[i+1][1] + frac * (curve[i][1] - curve[i+1][1]) + 0.5f);
        }
    }
    return 0;
}

/* lipo_percent() with a small deadband so the displayed % doesn't toggle on
 * sub-percent voltage wobble. Allowed to move freely once it must (and always
 * reaches the 0/100 rails). State is private to this mapper. */
int lipo_percent_hyst(float v) {
    static int shown = -1;
    int raw = lipo_percent(v);
    if (shown < 0 || raw >= 100 || raw <= 0 || raw >= shown + 2 || raw <= shown - 2)
        shown = raw;
    return shown;
}

/* Optional LIVE battery-trim nudge (Device-Info UP/DOWN). Not persisted — it
 * resets to the baked nominal estimate on the next boot. Kept only as a bench
 * sanity tool; normal use needs no calibration. */
void battery_cal_save(float cal) {
    if (cal < 0.5f) cal = 0.5f; else if (cal > 2.0f) cal = 2.0f;
    g_vsys_cal = cal;
}
