/**
 * power.c — power policy.
 *
 * Purpose: implement the power policy: pseudo-off sleep entry/exit (radios,
 * panel, clocks), the charging power-diet, and battery sampling.
 *
 * ─── Pseudo-off power rails (STATE_SLEEP) ───────────────────────────────────
 * "As off as we can get without losing the wake button":
 *   - BLE social scan + advertising stopped, HCI powered OFF (CYW43 BT core dark)
 *   - WiFi disassociated (the CYW43 chip itself must stay up: GPIO 29 / VSYS
 *     battery sensing rides its SPI CS line, see wifi_disconnect)
 *   - e-ink panel in deep sleep (the sleep still is retained at zero power)
 *   - clk_sys dropped to 48 MHz sourced from PLL_USB and PLL_SYS powered DOWN
 *     (set_sys_clock_48mhz — USB serial stays alive), core rail 1.10 → 1.00 V
 * FreeRTOS ticks stretch ~3x while underclocked (each core's SysTick reload was
 * computed at boot for the full clock) — every task just polls proportionally
 * slower, which is exactly what we want. The µs timebase (to_ms_since_boot) is
 * XOSC-derived and stays true, so real-time decisions remain correct.
 * NOT done (future work, see the Requirements doc): parking core 1, true
 * dormant/deep-sleep with SC7A20 INT1 wake — those need the display task and
 * the cyw43 arch to survive, which the current task split doesn't allow.
 */
#include "power.h"

#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "hardware/clocks.h"    /* set_sys_clock_48mhz/_khz — STATE_SLEEP underclock */
#include "hardware/vreg.h"      /* vreg_set_voltage — STATE_SLEEP core-rail drop */

#include "FreeRTOS.h"
#include "task.h"

#include "battery.h"
#include "bt.h"
#include "rtos_tasks.h"
#include "storage.h"
#include "wifi.h"

bool g_power_sleep = false;

/* While the radios restart after a wake they hold the cyw43 lock for long
 * stretches; Housekeeping must NOT queue behind it (its lock-taking battery
 * reads would stall the accel sampling and freeze auto-rotate for seconds —
 * field bug). Battery reads pause until this deadline; cached values serve. */
static volatile uint32_t g_radio_settle_until = 0;

volatile int   g_batt_pct = -1;
volatile float g_batt_v   = 0.0f;   /* filtered battery volts (calibrated) */

/* True while USB is connected (updated by batt_sample). Used for the "charging
 * power-diet": with no load-sharing on the board, the running radio/e-ink load
 * can out-draw the ~120 mA charger, so the cell never fills. While on USB we
 * suspend BLE scanning and slow the e-ink so the charge current wins. */
volatile bool g_on_usb = false;

/* Pre-sleep radio state, restored on wake. */
static bool g_slept_bt   = false;         /* HCI was on at sleep entry */
static bool g_slept_wifi = false;         /* WiFi was connected at sleep entry */
static char g_slept_ssid[33] = "";

/* ─── Battery + USB sampling — runs in its OWN task, NOT the accel loop ──────
 * read_vsys_volts() and is_usb_powered() both grab the cyw43 lock (GP29 is the
 * shared CYW43 SPI clock / VBUS sense line). This used to live inside hk_sample,
 * so every 250 ms the LOWEST-priority Housekeeping task blocked on that lock
 * behind the radio's background task — which stretched the whole HK loop and
 * dropped the 20 Hz accel sampling that makes auto-rotate feel instant. The
 * giveaway: after a wake auto-rotate was snappy for exactly the g_radio_settle
 * window (battery reads paused via the gate below), then went sluggish again the
 * instant the reads resumed. Splitting the battery read into batt_task lets HK
 * sample the accel at a rock-steady 20 Hz (pure I2C, no cyw43 lock) while the
 * battery block does its lock-taking on a task of its own — a stall there only
 * delays the gauge, never the rotation. Values land in g_batt_pct/g_batt_v/
 * g_on_usb (volatile, single-writer) that HK folds into the snapshot.
 *
 * ── Battery: PEAK-HOLD over a ~2 s window, then EMA-smoothed ──
 * One instantaneous burst every 2 s swung with whatever load was on the rail
 * that instant (e-ink full-refresh ~every 0.7 s, radio bursts — both pull VSYS
 * DOWN). Load only ever sags VSYS below the true resting voltage, so the MAX
 * across a window of lightly-spaced samples ≈ the open-circuit voltage the
 * discharge curve expects. We sample ~4x/s, keep the window peak, and every ~2 s
 * fold that peak into an EMA — steady, tracking charge instead of momentary load. */
void batt_sample(void) {
    static uint32_t batt_win_start = 0, batt_last_ms = 0;
    static float    batt_win_peak  = 0.0f;
    static float    batt_v_ema     = 0.0f;
    static int      was_usb  = -1;
    static float    last_bv  = 0.0f;     /* last on-battery reading, retained for diag */
    static int      last_bp  = -1;
    uint32_t now = to_ms_since_boot(get_absolute_time());

    /* Sample at most ~4x/sec (still ~8 peak samples across the 2 s window), and
     * not at all during the post-wake radio-settle window. */
    if (now - batt_last_ms < 250 || (int32_t)(now - g_radio_settle_until) < 0) return;
    batt_last_ms = now;

    bool usb = is_usb_powered();
    g_on_usb = usb;
    /* Charging power-diet: on a USB plug/unplug edge, suspend BLE scanning
     * while charging (it's the biggest continuous radio load), and restore
     * the user's setting when unplugged — so the cell can actually fill. */
    if ((was_usb != 1) && usb) {
        wetgreg_social_enable(false);
    } else if ((was_usb != 0) && !usb) {
        /* ... but never re-arm the scan while Greg is asleep (STATE_SLEEP
         * powered the whole HCI off; power_sleep_exit restores it). */
        if (g_saved.social_on && !g_power_sleep) { wetgreg_social_set_self(g_saved.wetgreg_id); wetgreg_social_enable(true); }
    }
    if (usb) {
        /* On USB the rail isn't the battery; show live rail volts, flag -1. */
        g_batt_pct = -1;
        g_batt_v   = read_vsys_volts();
        batt_win_peak = 0.0f; batt_win_start = now; batt_v_ema = 0.0f;
        (void)was_usb;
        /* DIAGNOSTIC: serial needs USB, but USB hides the battery. Print the
         * LAST on-battery reading every ~2 s so it's easy to capture: run on
         * battery a few seconds, replug USB, read this line. */
        static uint32_t usb_log_ms = 0;
        if (now - usb_log_ms >= 2000) {
            usb_log_ms = now;
            printf("[BATT] USB rail=%.3f V | last on-battery=%.3f V %d%%\n",
                   (double)g_batt_v, (double)last_bv, last_bp);
        }
    } else {
        /* Add back the battery-path drop so device-read 3.67 V (full) → 4.20 V. */
        float v = read_vsys_volts() + VSYS_BATT_OFFSET;
        if (v > batt_win_peak) batt_win_peak = v;    /* keep the lightest-load sample */
        if (batt_win_start == 0) batt_win_start = now;
        if (batt_v_ema <= 0.0f) {                    /* seed immediately on unplug/boot */
            batt_v_ema = v; g_batt_v = v; g_batt_pct = lipo_percent_hyst(v);
        }
        if (now - batt_win_start >= 2000) {
            float peak = batt_win_peak;
            batt_v_ema += 0.35f * (peak - batt_v_ema);   /* ~5-window settle */
            g_batt_v    = batt_v_ema;
            g_batt_pct  = lipo_percent_hyst(batt_v_ema);
            batt_win_peak = 0.0f;
            batt_win_start = now;
        }
        last_bv = g_batt_v; last_bp = g_batt_pct;
    }
    was_usb = usb ? 1 : 0;
}

void power_sleep_enter(void) {
    printf("[SLEEP] pseudo-off: radios down, panel asleep, 48 MHz\n");

    /* Radios first, while still at full clock. */
    g_slept_bt = wetgreg_bt_active();
    wetgreg_social_enable(false);
    if (g_slept_bt) wetgreg_bt_stop();        /* advertising off + HCI power off */
    /* An in-flight (or still-deferred) background rejoin counts as "wifi was
     * on" — otherwise a nap taken during the rejoin window would silently
     * forget the network (g_slept_ssid already holds the right name then). */
    g_slept_wifi = wifi_connected || wifi_rejoin_pending();
    if (wifi_connected)
        snprintf(g_slept_ssid, sizeof(g_slept_ssid), "%s", wifi_ssid_display);
    if (wifi_enabled || wifi_connected) wifi_disconnect();
    wifi_rejoin_cancel();     /* cancel any rejoin in flight or still deferred */

    /* Let the sleep still reach the glass, then deep-sleep the panel. The
     * render queue is FIFO, so the command lands strictly after the blit. */
    rtos_display_cmd(DISP_CMD_SLEEP);
    while (ui_display_busy()) vTaskDelay(pdMS_TO_TICKS(50));
    vTaskDelay(pdMS_TO_TICKS(150));           /* let the sleep cmd itself land */

    g_power_sleep = true;    /* HK: no BLE re-arm on a USB unplug edge mid-sleep */

    /* Underclock LAST. Hold the cyw43 lock so its background task is not
     * mid-SPI-transaction while clk_sys (the PIO SPI clock source) ramps. */
    cyw43_thread_enter();
    set_sys_clock_48mhz();                    /* clk_sys+clk_peri 48 MHz, PLL_SYS off */
    vreg_set_voltage(VREG_VOLTAGE_1_00);      /* core rail 1.10 → 1.00 V */
    cyw43_thread_exit();
}

void power_sleep_exit(void) {
    /* Clock back up FIRST — and raise the rail before the frequency. */
    cyw43_thread_enter();
    vreg_set_voltage(VREG_VOLTAGE_DEFAULT);
    busy_wait_us(300);                        /* rail settle (µs-timer, tick-safe) */
    set_sys_clock_khz(SYS_CLK_KHZ, true);     /* board default (150 MHz on RP2350) */
    cyw43_thread_exit();
    g_power_sleep = false;

    /* Panel back: hardware reset + init + full clear (clean base, no ghost of
     * the sleep still). Queued now, executes before the next rendered frame. */
    rtos_display_cmd(DISP_CMD_WAKE);

    /* Radios back the way they were — but STAGGERED: BT/social power on now,
     * the WiFi join fires ~4 s later from the octopus loop (wifi_rejoin_tick)
     * so the combined radio burst can't hog the cyw43 lock while the user is
     * interacting. Housekeeping's lock-taking battery reads also pause until
     * the settle deadline so accel sampling (auto-rotate) never stalls. */
    uint32_t now = to_ms_since_boot(get_absolute_time());
    g_radio_settle_until = now + 8000;
    if (g_slept_bt) wetgreg_bt_init();
    if (g_saved.social_on) {
        wetgreg_social_set_self(g_saved.wetgreg_id);
        wetgreg_social_enable(true);
    }
    if (g_slept_wifi)
        wifi_schedule_rejoin(g_slept_ssid, 4000);
    printf("[SLEEP] awake: clk_sys=%lu Hz clk_peri=%lu Hz, radios restoring\n",
           (unsigned long)clock_get_hz(clk_sys), (unsigned long)clock_get_hz(clk_peri));
}
