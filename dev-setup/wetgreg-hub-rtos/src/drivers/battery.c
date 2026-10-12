/**
 * battery.c — raw battery / USB-power sensing + the LiPo %-curve.
 *
 * Purpose: implement the (optionally cyw43-locked) ADC read with a trimmed
 * mean, USB detection with a CYW43-free fallback, the persisted additive
 * calibration offset, and the LiPo discharge curve. See battery.h for the
 * hardware analysis that drives the design.
 */
#include "battery.h"

#include <stdio.h>

#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "hardware/adc.h"
#include "hardware/gpio.h"

#include "storage.h"     /* battery_offset_store — persist the calibration */

uint16_t g_batt_offset_mv  = BATT_OFFSET_DEFAULT_MV;
char     g_batt_offset_src = 'D';

static bool battery_adc_ready = false;
static volatile bool g_cyw43_up = true;   /* the arch is up unless power.c says otherwise */

#ifdef BATT_DIRECT_ADC_GPIO
  #define BATT_ADC_GPIO  BATT_DIRECT_ADC_GPIO
#else
  #define BATT_ADC_GPIO  29                /* VSYS/3 (PICO_VSYS_PIN) */
#endif
#define BATT_ADC_INPUT (BATT_ADC_GPIO - 26)

void battery_init(void) {
    adc_init();
    adc_gpio_init(BATT_ADC_GPIO);        /* analog: digital input + pulls off */
    battery_adc_ready = true;
}

void battery_set_cyw43_up(bool up) {
    g_cyw43_up = up;
#ifndef BATT_DIRECT_ADC_GPIO
    if (!up) {
        /* With the radio powered down GPIO 25 (WL_CS) is no longer driven by the
         * cyw43 driver. On the Pico W / Pico 2 W that pin ALSO gates the VSYS/3
         * divider: "when high also enables GPIO29 ADC pin to read VSYS". Keep it
         * high ourselves so the gauge keeps working while the chip is dark. */
        gpio_init(25);
        gpio_set_dir(25, GPIO_OUT);
        gpio_put(25, 1);
    }
#endif
}

/* Uncalibrated reading — raw divider math only. On the VSYS path this is the
 * cell MINUS the D3 drop (battery) or the USB rail (on USB). */
float read_vsys_raw_volts(void) {
    if (!battery_adc_ready) battery_init();

#ifndef BATT_DIRECT_ADC_GPIO
    /* GPIO 29 doubles as the CYW43 SPI clock — lock the radio out while we read. */
    bool lock = g_cyw43_up;
    if (lock) cyw43_thread_enter();
    adc_gpio_init(BATT_ADC_GPIO);        /* reclaim the pin as an ADC input */
#endif
    adc_select_input(BATT_ADC_INPUT);
    /* The divider node needs to settle after the pin was toggling as a clock
     * (and, on the direct path, the 100 nF just needs the mux to settle). The
     * first conversions sample the transient and read LOW — discard them. */
    sleep_us(800);
    for (int i = 0; i < 8; i++) (void)adc_read();
    enum { N = 48 };
    uint16_t s[N];
    for (int i = 0; i < N; i++) s[i] = (uint16_t)adc_read();
#ifndef BATT_DIRECT_ADC_GPIO
    if (lock) cyw43_thread_exit();
#endif

    /* Trimmed mean: sort, drop the lowest/highest 25 %, average the middle —
     * shrugs off the switching noise coupled onto the shared pin. */
    for (int i = 1; i < N; i++) {
        uint16_t v = s[i]; int j = i - 1;
        while (j >= 0 && s[j] > v) { s[j + 1] = s[j]; j--; }
        s[j + 1] = v;
    }
    uint32_t acc = 0; int lo = N / 4, hi = N - N / 4, cnt = 0;
    for (int i = lo; i < hi; i++) { acc += s[i]; cnt++; }
    float raw = (float)acc / (float)cnt;

#ifdef BATT_DIRECT_ADC_GPIO
    return raw * 3.3f / 4095.0f * BATT_DIRECT_DIVIDER;
#else
    return raw * 3.3f / 4095.0f * 3.0f;  /* 3:1 divider, 3.3 V ADC ref */
#endif
}

bool is_usb_powered(void) {
    if (g_cyw43_up) {
        /* VBUS sense is WL_GPIO2 on the CYW43 — authoritative while the chip is up. */
        return cyw43_arch_gpio_get(CYW43_WL_GPIO_VBUS_PIN);
    }
#ifdef BATT_DIRECT_ADC_GPIO
    return false;                        /* the direct path can't see VBUS; power.c
                                          * re-checks once the radio is back */
#else
    return read_vsys_raw_volts() >= BATT_USB_VSYS_THRESHOLD_V;
#endif
}

float battery_cell_from_raw(float raw_v) {
#ifdef BATT_DIRECT_ADC_GPIO
    return raw_v;                        /* measured at the cell — nothing to add */
#else
    return raw_v + (float)g_batt_offset_mv / 1000.0f;
#endif
}

void battery_set_offset_mv(uint16_t mv, char src) {
    if (mv < BATT_OFFSET_MIN_MV) mv = BATT_OFFSET_MIN_MV;
    if (mv > BATT_OFFSET_MAX_MV) mv = BATT_OFFSET_MAX_MV;
    g_batt_offset_mv  = mv;
    g_batt_offset_src = src;
}

void battery_offset_persist(uint16_t mv, char src) {
    battery_set_offset_mv(mv, src);
    battery_offset_store(g_batt_offset_mv);
    printf("[BATT] offset now %u mV (%c)\n", (unsigned)g_batt_offset_mv, src);
}

void battery_anchor_to(float raw_peak_v, char src) {
    float off = BATT_ANCHOR_TARGET_V - raw_peak_v;
    int   mv  = (int)(off * 1000.0f + 0.5f);
    if (mv < BATT_OFFSET_MIN_MV || mv > BATT_OFFSET_MAX_MV) {
        printf("[BATT] anchor rejected: raw=%.3f V → offset %d mV out of range\n",
               (double)raw_peak_v, mv);
        return;
    }
    battery_set_offset_mv((uint16_t)mv, src);
    battery_offset_store(g_batt_offset_mv);
    printf("[BATT] anchor (%c): raw peak %.3f V → offset %u mV\n",
           src, (double)raw_peak_v, (unsigned)g_batt_offset_mv);
}

void battery_offset_reset(void) {
    battery_set_offset_mv(BATT_OFFSET_DEFAULT_MV, 'D');
    battery_offset_store(g_batt_offset_mv);
    printf("[BATT] offset reset to default %u mV\n", (unsigned)g_batt_offset_mv);
}

/* Single-cell LiPo RESTING voltage → % via a piecewise discharge curve. A
 * straight (V-3.0)/1.2 line badly misreports the flat mid-range (3.7 V is
 * ~45 % real, but linear claims ~58 %). */
int lipo_percent(float v) {
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
 * sub-percent voltage wobble. Always reaches the 0/100 rails. */
int lipo_percent_hyst(float v) {
    static int shown = -1;
    int raw = lipo_percent(v);
    if (shown < 0 || raw >= 100 || raw <= 0 || raw >= shown + 2 || raw <= shown - 2)
        shown = raw;
    return shown;
}
