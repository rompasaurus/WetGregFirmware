/**
 * Sassy Octopus — Runtime-rendered e-ink animation
 *
 * Instead of pre-baking every frame, this firmware renders each frame
 * on the fly: composites the octopus body, eyes, mouth expression,
 * chat bubble, and text at display time.  This means ALL quotes fit
 * in flash (~10KB of strings vs ~4MB of bitmaps).
 *
 * Wiring — WetGreg PCB / breadboard SPI0 (GP17-22, see DEV_Config.c):
 *   VCC  -> 3V3(OUT) pin 36    GND  -> GND      pin 38
 *   CS   -> GP17     pin 22    SCL  -> GP18     pin 24  (SPI0 SCK)
 *   SDA  -> GP19     pin 25    DC   -> GP20     pin 26  (SPI0 TX)
 *   RES  -> GP21     pin 27    BUSY -> GP22     pin 29
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "hardware/pwm.h"
#include "hardware/adc.h"
#include "hardware/i2c.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
/* --- FreeRTOS: the real-time kernel that schedules our tasks across both cores.
 *     FreeRTOS.h MUST come first (it pulls in FreeRTOSConfig.h); task.h adds the
 *     task/scheduler API (xTaskCreate, vTaskStartScheduler, vTaskDelay, ...). --- */
#include "FreeRTOS.h"
#include "task.h"
#include "pico/flash.h"        /* flash_safe_execute — multicore-safe flash writes (SMP) */
#include "rtos_tasks.h"        /* Phase 2: the 4-task split interface */
#include "lwip/dns.h"
#include "lwip/pbuf.h"
#include "lwip/udp.h"
#include "rtc_compat.h"
#include "DEV_Config.h"
#include "version.h"
#include "wifi_config.h"
#include "bt.h"
#ifdef PICOWOTA_OTA
#include "picowota/reboot.h"
#endif

/* ─── Joystick pins ─── */
#define JOY_LEFT   2
#define JOY_DOWN   3
#define JOY_UP     4
#define JOY_RIGHT  5
#define JOY_CENTER 6

/* ─── Speaker (active buzzer on GP14) ─── */
/* Wire: buzzer (+) → pin 19 (GP14), buzzer (-) → GND (pin 18). */
#define BUZZER_PIN 14  /* GP14 = pin 19 — active buzzer (+) */

static bool sound_enabled = true;
static uint8_t sound_vol = 2;  /* 0=short, 1=med, 2=long beep duration */
static const uint16_t vol_durations[] = {20, 50, 100};  /* ms per level */

static void speaker_init(void) {
    gpio_init(BUZZER_PIN);
    gpio_set_dir(BUZZER_PIN, GPIO_OUT);
    gpio_put(BUZZER_PIN, 0);
}

static void speaker_tone(uint16_t freq_hz, uint16_t duration_ms) {
    if (!sound_enabled || freq_hz == 0) { sleep_ms(duration_ms); return; }
    uint16_t actual = duration_ms < vol_durations[sound_vol] ? duration_ms : vol_durations[sound_vol];
    gpio_put(BUZZER_PIN, 1);
    sleep_ms(actual);
    gpio_put(BUZZER_PIN, 0);
    if (duration_ms > actual) sleep_ms(duration_ms - actual);
}

/* ─── Sound patterns (on_ms, off_ms pairs; 0 terminates) ─── */
static const uint16_t pat_beep[]     = {150, 0};
static const uint16_t pat_chirp[]    = {30,30, 30,30, 30,30, 30,30, 30, 0};
static const uint16_t pat_sos[]      = {60,60, 60,60, 60,180, 180,60, 180,60, 180,180, 60,60, 60,60, 60, 0};
static const uint16_t pat_doorbell[] = {200, 100, 300, 0};
static const uint16_t pat_alert[]    = {300,200, 300,200, 300, 0};
static const uint16_t pat_happy[]    = {40,60, 40,60, 40,60, 200,100, 50,50, 50,50, 300, 0};

#define SOUND_PATTERN_COUNT 6
static const char *pattern_names[] = {"BEEP", "CHIRP", "SOS", "DOORBELL", "ALERT", "HAPPY"};
static const uint16_t *patterns[]  = {pat_beep, pat_chirp, pat_sos, pat_doorbell, pat_alert, pat_happy};
static int current_pattern = 0;

static void play_sound_pattern(int idx) {
    if (!sound_enabled || idx < 0 || idx >= SOUND_PATTERN_COUNT) return;
    const uint16_t *p = patterns[idx];
    while (*p) {
        uint16_t on_ms = *p++;
        uint16_t actual = on_ms < vol_durations[sound_vol] ? on_ms : vol_durations[sound_vol];
        gpio_put(BUZZER_PIN, 1);
        sleep_ms(actual);
        gpio_put(BUZZER_PIN, 0);
        if (on_ms > actual) sleep_ms(on_ms - actual);
        if (*p) { sleep_ms(*p); p++; }
    }
}

/* ─── Joystick init ─── */
#if INPUT_USE_IRQ
/* GPIO interrupt callback (shared by all 5 joystick pins). Fires on every press
 * AND release edge; it does nothing but wake the Input task instantly — all the
 * debounce/edge/repeat logic lives in the task, off the interrupt.
 * Compiled only when INPUT_USE_IRQ=1 (bench-only — see rtos_tasks.h). */
static void joystick_irq_cb(uint gpio, uint32_t events) {
    (void)gpio; (void)events;
    rtos_input_isr_notify();
}
#endif

static void joystick_init(void) {
    const uint pins[] = {JOY_UP, JOY_DOWN, JOY_LEFT, JOY_RIGHT, JOY_CENTER};
    for (int i = 0; i < 5; i++) {
        gpio_init(pins[i]);
        gpio_set_dir(pins[i], GPIO_IN);
        gpio_pull_up(pins[i]);
    }
#if INPUT_USE_IRQ
    /* Interrupt-driven input: wake the Input task the instant any line changes,
     * instead of polling. Sub-millisecond press latency, and the core idles when
     * nothing is pressed (better for battery). One callback is shared by all
     * pins; register it on the first, then enable the IRQ on the rest.
     * GATED OFF by default: this path soft-bricked the RP2350 under FreeRTOS SMP
     * and is not yet root-caused. Enable only on the bench with serial. */
    gpio_set_irq_enabled_with_callback(pins[0], GPIO_IRQ_EDGE_FALL | GPIO_IRQ_EDGE_RISE,
                                       true, joystick_irq_cb);
    for (int i = 1; i < 5; i++)
        gpio_set_irq_enabled(pins[i], GPIO_IRQ_EDGE_FALL | GPIO_IRQ_EDGE_RISE, true);
#endif
}

/* ─── Joystick read (returns single direction or 0) ─── */
#define INPUT_NONE   0
#define INPUT_UP     1
#define INPUT_DOWN   2
#define INPUT_LEFT   3
#define INPUT_RIGHT  4
#define INPUT_CENTER 5

/* ─── Input orientation remap ───────────────────────────────────────────
 * The joystick's physical up/down/left/right are remapped to logical
 * directions through a rotation, so the firmware can match how the device is
 * actually held.  On the WetGreg board (held screen-left / joystick-right) the
 * physical axes read reversed, so we default to 180° (up<->down, left<->right).
 *
 * This is the single hook the accelerometer feature will drive: once the IMU
 * is installed, call input_set_rotation() from the orientation logic and every
 * joystick read auto-rotates — no other code changes needed.
 */
typedef enum { ROT_0 = 0, ROT_90 = 1, ROT_180 = 2, ROT_270 = 3 } input_rotation_t;
/* volatile: written by the Housekeeping task (orientation_update) and read by the
 * higher-priority Input task (read_joystick). A single aligned enum store is
 * atomic on the Cortex-M33, so no torn read — `volatile` just stops the compiler
 * caching it. INVARIANT: both Housekeeping and Input are pinned to core 0; if a
 * future phase moves either off core 0 this must become snapshot-/mutex-guarded. */
static volatile input_rotation_t input_rotation = ROT_180;  /* default for the WetGreg hold */

/* Display orientation state (declared early so the accel orientation_update()
 * can drive it). display_rotation is the panel-map angle (see transpose);
 * g_orientation is the 0..3 hold from the accelerometer. */
static int display_rotation = 90;
static int g_orientation = 0;

/* Set once the accel classifier delivers its first post-boot verdict (or the
 * manual hold is applied). volatile: written by Housekeeping, read by the UI
 * task — both pinned to core 0, same rule as input_rotation above. The boot
 * splash holds its first frame on this so it starts in the correct hold. */
static volatile bool g_orient_primed = false;

/* While calibrating: 1 = log accel/orientation to serial AND draw an on-screen
 * HUD (accel + orientation). Set to 0 once the orientation map is dialed in. */
#define ORIENT_DEBUG 0
#define FACE_DEBUG 0

void input_set_rotation(input_rotation_t r) { input_rotation = r; }

/* ── Orientation model: 3 valid holds (joystick ABOVE the screen is ignored) ──
 *   OR_LAND_R = landscape, joystick RIGHT  → WIDE side-by-side
 *   OR_LAND_L = landscape, joystick LEFT   → WIDE side-by-side (flipped)
 *   OR_TALL   = portrait,  joystick BOTTOM → TALL longways
 * Per-orientation display + joystick input rotation in one table — CALIBRATE
 * from the on-screen HUD, then adjust this table and the classifier. */
enum { OR_LAND_R = 0, OR_LAND_L = 1, OR_TALL = 2 };
typedef struct { bool tall; int disp_rot; input_rotation_t in_rot; } orient_cfg_t;
static const orient_cfg_t ORIENT_CFG[3] = {
    /* OR_LAND_R */ { false, 90,  ROT_180 },
    /* OR_LAND_L */ { false, 270, ROT_0   },
    /* OR_TALL   */ { true,  180, ROT_270 },   /* joystick-bottom: display flipped, input +180 */
};
static bool orientation_is_tall(void) { return ORIENT_CFG[g_orientation].tall; }

/* Display settings (runtime mirror of g_saved.auto_rotate / .manual_orient, which
 * are declared much later; loaded in saved_load). When auto_rotate is false,
 * orientation_update() locks g_orientation to g_manual_orient. */
static bool    g_auto_rotate   = true;
static uint8_t g_manual_orient = OR_TALL;

/* Directions in clockwise order; the rotation is how many 90° CW steps the
 * device is turned, so a press maps forward around the ring by that many. */
static const uint8_t CW_RING[4] = { INPUT_UP, INPUT_RIGHT, INPUT_DOWN, INPUT_LEFT };

static uint8_t rotate_input(uint8_t dir) {
    if (dir == INPUT_NONE || dir == INPUT_CENTER) return dir;  /* center is rotation-invariant */
    for (int i = 0; i < 4; i++) {
        if (CW_RING[i] == dir)
            return CW_RING[(i + (int)input_rotation) & 3];
    }
    return dir;
}

/* Non-static: the Input task (rtos_tasks.c) is the sole runtime caller. */
uint8_t read_joystick(void) {
    uint8_t raw = INPUT_NONE;
    if      (!gpio_get(JOY_UP))     raw = INPUT_UP;
    else if (!gpio_get(JOY_DOWN))   raw = INPUT_DOWN;
    else if (!gpio_get(JOY_LEFT))   raw = INPUT_LEFT;
    else if (!gpio_get(JOY_RIGHT))  raw = INPUT_RIGHT;
    else if (!gpio_get(JOY_CENTER)) raw = INPUT_CENTER;
    return rotate_input(raw);
}

/* ─── App states ─── */
#define STATE_OCTOPUS     0   /* main screen — octopus + quote + clock */
#define STATE_MENU        1   /* menu overlay */
#define STATE_SOUND       2   /* sound test sub-screen */
#define STATE_INFO        3   /* device info sub-screen */
#define STATE_MOOD_SELECT 4   /* mood picker */
#define STATE_NETWORK     5   /* WiFi status (read-only) */
#define STATE_NET_MENU    6   /* Network submenu */
#define STATE_NET_SCAN    7   /* Scan results list */
#define STATE_NET_KEYBOARD 8  /* On-screen keyboard for WiFi password */
#define STATE_MOTION       9  /* Accelerometer / pedometer menu */
#define STATE_SET_TIME    10  /* Manual date/time setter */
#define STATE_SAVED_NETS  11  /* Saved WiFi networks (connect / forget) */
#define STATE_BLUETOOTH   12  /* Bluetooth pairing screen */
#define STATE_SOCIAL          13  /* Social menu (toggle, met list, set name) */
#define STATE_SOCIAL_PROMPT   14  /* "WetGreg found — say hi?" Y/N */
#define STATE_SOCIAL_RECV     15  /* "WetGreg says hello!" — say hi back? Y/N */
#define STATE_SOCIAL_NAME     16  /* reroll picker for your WetGreg name */
#define STATE_SOCIAL_MET      17  /* list of WetGregs we've met */
#define STATE_SOCIAL_NEARBY   18  /* live list of WetGregs in range now */
#define STATE_EMOTE_PICK      19  /* pick an emote to send to g_social_peer */
#define STATE_EMOTE_PLAY      20  /* octopus acts out g_play_emote, then g_play_next */
#define STATE_DISPLAY         21  /* Display settings: auto-rotate + orientation */
#define STATE_SPLASH          22  /* one-shot boot splash animation */

/* ─── WiFi state ─── */
static bool wifi_enabled = false;
static bool wifi_connected = false;
static bool ntp_synced = false;

/* Bluetooth is OFF by default to save power; toggled on in the Bluetooth screen. */
static bool bt_enabled = false;

/* ── Social UI state (the WetGreg a prompt/recv screen is currently about) ── */
static uint16_t g_social_peer      = 0;
static int8_t   g_social_peer_rssi = 0;
static uint16_t g_name_seed        = 0;   /* candidate seed on the Set-Name reroll screen */

/* Live "scan nearby" buffer — distinct WetGreg ids seen while the screen is open. */
#define NEARBY_MAX 8
static uint16_t g_nearby_id[NEARBY_MAX];
static int8_t   g_nearby_rssi[NEARBY_MAX];
static int      g_nearby_count = 0;

/* Emote playback: which emote the octopus is acting out, who it's about, whether
 * we received it (vs sent), and which state to enter once the animation ends. */
static uint8_t  g_play_emote    = 0;
static bool     g_play_incoming = false;
static uint8_t  g_play_next     = 0;
static int      g_emote_sel     = 0;
static int32_t wifi_rssi = 0;
static char wifi_ssid_display[33] = "---";
static char wifi_ip_str[20] = "---";

/* ─── WiFi scan state ─── */
#define MAX_SCAN_RESULTS 16
typedef struct { char ssid[33]; int8_t rssi; uint8_t auth_mode; } scan_entry_t;
static scan_entry_t scan_results[MAX_SCAN_RESULTS];
static int  scan_count = 0;
static int  scan_sel = 0;
static bool scan_in_progress = false;
static bool scan_complete = false;

/* ─── On-screen keyboard state ─── */
#define PW_MAX_LEN 63
static char pw_buf[PW_MAX_LEN + 1];
static int  pw_len = 0;
static int  kb_row = 0;
static int  kb_col = 0;
static bool kb_shift = true;   /* start in CAPS mode */
static int  selected_network = -1;

static const char kb_grid[4][10] = {
    {'Q','W','E','R','T','Y','U','I','O','P'},
    {'A','S','D','F','G','H','J','K','L','.'},
    {'Z','X','C','V','B','N','M','-','!','?'},
    {'0','1','2','3','4','5','6','7','8','9'},
};
#define KB_CHAR_ROWS 4
#define KB_SPECIAL_ROW 4
#define KB_SP_SHIFT  0
#define KB_SP_SPACE  1
#define KB_SP_DEL    2
#define KB_SP_DONE   3
#define KB_SP_CANCEL 4
#define KB_SP_COUNT  5

static char kb_char_at(int row, int col, bool shift) {
    if (row >= KB_CHAR_ROWS) return '\0';
    char c = kb_grid[row][col];
    if (!shift && c >= 'A' && c <= 'Z') return c + 32;
    return c;
}

/* ─── SC7A20 accelerometer (I2C0 on GP0/GP1) ───────────────────────────────
 * The WetGreg PCB fits an SC7A20HTR — LIS2DH12 / LIS3DH register-compatible,
 * NOT an MPU-6050. PCB wiring (U1): SDO->GND => I2C addr 0x18, CS->3V3 => I2C
 * mode, INT1->GP15. Differences from the old MPU driver:
 *   - address 0x18 (not 0x68)
 *   - WHO_AM_I at 0x0F (SC7A20 reports 0x11; ST parts report 0x33)
 *   - accel data at OUT_X_L 0x28, LITTLE-endian (low byte first)
 *   - multi-byte reads REQUIRE the auto-increment bit (0x80) in the sub-address
 *   - no gyro, no usable temperature (those vars stay 0)
 * Variable/function names (accel_x.., mpu_ok, accel_g, tilt_*) are kept so the
 * existing UI/render code compiles unchanged.
 */
#define MPU_I2C    i2c0
static uint8_t mpu_addr = 0x18;  /* SC7A20 SDO=GND -> 0x18 (0x19 if SDO=VDD) */
#define MPU_SDA    0   /* GP0 = pin 1 */
#define MPU_SCL    1   /* GP1 = pin 2 */

/* LIS2DH / LIS3DH / SC7A20 registers */
#define SC_WHO_AM_I   0x0F
#define SC_CTRL_REG1  0x20
#define SC_CTRL_REG4  0x23
#define SC_OUT_X_L    0x28
#define SC_AUTO_INC   0x80   /* OR into the sub-address for multi-byte transfer */

static bool mpu_ok = false;

/* Raw sensor data (accel only — SC7A20 has no gyro/temp) */
static int16_t accel_x, accel_y, accel_z;
static int16_t gyro_x, gyro_y, gyro_z;   /* unused on SC7A20 — held at 0 */
static float   mpu_temp_c;               /* unused on SC7A20 — held at 0 */

/* Pedometer state */
static uint32_t step_count = 0;
static float    step_threshold = 1.3f;  /* g — adjustable */
static bool     step_above = false;     /* debounce: was last sample above threshold? */

/* ALWAYS use the *_timeout_us variants — never the plain _blocking ones — so a
 * missing / half-soldered / bus-held-low accelerometer can never hang a task
 * (especially the boot-time probe). A no-device bus then just returns an error
 * and mpu_ok stays false; the firmware runs fine without orientation/steps. */
#define MPU_I2C_TIMEOUT_US 3000

static bool mpu_write_reg(uint8_t reg, uint8_t val) {
    uint8_t buf[2] = {reg, val};
    return i2c_write_timeout_us(MPU_I2C, mpu_addr, buf, 2, false, MPU_I2C_TIMEOUT_US) == 2;
}

static int mpu_read_reg(uint8_t reg) {
    uint8_t val;
    if (i2c_write_timeout_us(MPU_I2C, mpu_addr, &reg, 1, true, MPU_I2C_TIMEOUT_US) < 0) return -1;
    if (i2c_read_timeout_us(MPU_I2C, mpu_addr, &val, 1, false, MPU_I2C_TIMEOUT_US) < 0) return -1;
    return val;
}

static bool mpu_read_burst(uint8_t reg, uint8_t *dst, uint8_t len) {
    reg |= SC_AUTO_INC;  /* SC7A20/LIS2DH need this for multi-byte reads */
    if (i2c_write_timeout_us(MPU_I2C, mpu_addr, &reg, 1, true, MPU_I2C_TIMEOUT_US) < 0) return false;
    return i2c_read_timeout_us(MPU_I2C, mpu_addr, dst, len, false, MPU_I2C_TIMEOUT_US) == (int)len;
}

static void mpu_init(void) {
    i2c_init(MPU_I2C, 400 * 1000);  /* 400 kHz */
    gpio_set_function(MPU_SDA, GPIO_FUNC_I2C);
    gpio_set_function(MPU_SCL, GPIO_FUNC_I2C);
    gpio_pull_up(MPU_SDA);
    gpio_pull_up(MPU_SCL);
    sleep_ms(20);  /* SC7A20 boot/turn-on time */

    /* SC7A20 SDO->GND is 0x18 on the WetGreg PCB; probe 0x18 then 0x19. */
    uint8_t try_addrs[] = {0x18, 0x19};
    bool found = false;
    for (int a = 0; a < 2; a++) {
        uint8_t addr = try_addrs[a];
        uint8_t reg = SC_WHO_AM_I;
        uint8_t who = 0;
        int w = i2c_write_timeout_us(MPU_I2C, addr, &reg, 1, true, MPU_I2C_TIMEOUT_US);
        int r = i2c_read_timeout_us(MPU_I2C, addr, &who, 1, false, MPU_I2C_TIMEOUT_US);
        printf("[ACCEL] Probe 0x%02X: w=%d r=%d who=0x%02X\n", addr, w, r, who);
        if (w >= 0 && r >= 0) {
            mpu_addr = addr;
            found = true;
            /* Known IDs: SC7A20=0x11, ST LIS2DH12/LIS3DH=0x33. Accept either;
               warn (but still try) if a clone reports something else yet ACKs. */
            if (who != 0x11 && who != 0x33)
                printf("[ACCEL] Unexpected WHO_AM_I 0x%02X (continuing anyway)\n", who);
            printf("[ACCEL] SC7A20 found at 0x%02X\n", addr);
            break;
        }
    }

    if (!found) {
        printf("[ACCEL] NOT DETECTED on I2C0 (tried 0x18/0x19)\n");
        mpu_ok = false;
        return;
    }

    /* CTRL_REG1 0x57: 100 Hz ODR, normal mode, X/Y/Z enabled. */
    mpu_write_reg(SC_CTRL_REG1, 0x57);
    /* CTRL_REG4 0x88: block-data-update, +/-2g full scale, high-resolution. */
    mpu_write_reg(SC_CTRL_REG4, 0x88);
    sleep_ms(10);
    mpu_ok = true;
    printf("[ACCEL] SC7A20 initialized OK at 0x%02X\n", mpu_addr);
}

static void mpu_read_all(void) {
    if (!mpu_ok) return;

    /* 6 bytes from OUT_X_L (auto-increment): XL,XH,YL,YH,ZL,ZH. Data is
       left-justified 16-bit (12 significant bits) and LITTLE-endian. */
    uint8_t buf[6];
    if (!mpu_read_burst(SC_OUT_X_L, buf, 6)) return;

    accel_x = (int16_t)((buf[1] << 8) | buf[0]);
    accel_y = (int16_t)((buf[3] << 8) | buf[2]);
    accel_z = (int16_t)((buf[5] << 8) | buf[4]);

    /* SC7A20 has no gyro and no usable temperature path here. */
    gyro_x = gyro_y = gyro_z = 0;
    mpu_temp_c = 0.0f;
}

/* Convert raw accel to g. +/-2g high-res ~= 16384 LSB/g (same scale as before). */
static float accel_g(int16_t raw) { return raw / 16384.0f; }

/* Convert raw gyro to deg/s (at +/-250, 131 LSB/(deg/s)) */
static float gyro_dps(int16_t raw) { return raw / 131.0f; }

/* Magnitude of acceleration vector in g */
static float accel_magnitude(void) {
    float ax = accel_g(accel_x);
    float ay = accel_g(accel_y);
    float az = accel_g(accel_z);
    return sqrtf(ax * ax + ay * ay + az * az);
}

/* Tilt angles in degrees */
static float tilt_x_deg(void) {
    return atan2f(accel_g(accel_x), sqrtf(accel_g(accel_y) * accel_g(accel_y) + accel_g(accel_z) * accel_g(accel_z))) * 57.2958f;
}
static float tilt_y_deg(void) {
    return atan2f(accel_g(accel_y), sqrtf(accel_g(accel_x) * accel_g(accel_x) + accel_g(accel_z) * accel_g(accel_z))) * 57.2958f;
}

/* Simple pedometer: detect step when magnitude crosses threshold going up then back down */
static void pedometer_update(void) {
    float mag = accel_magnitude();
    if (!step_above && mag > step_threshold) {
        step_above = true;
        step_count++;
    } else if (step_above && mag < (step_threshold - 0.3f)) {
        step_above = false;
    }
}

/* ─── Daily activity tracking (steps + active time) ───────────────────────
 * steps_today is the running pedometer delta; active_seconds_today accrues
 * whenever there's real movement. Both reset when the RTC date rolls over.
 * Sampled cheaply (pedometer at ~20 Hz on the main screen, activity at ~4 Hz).
 * LOW-POWER ROADMAP: the SC7A20 INT1 line is on GP15 — the next step is to arm
 * its activity/wake interrupt so the MCU can sleep and only sample on motion. */
static uint32_t steps_today = 0;
static uint32_t active_seconds_today = 0;
static int8_t   activity_day = -1;       /* RTC day the tally belongs to */
static uint32_t last_step_total = 0;     /* step_count snapshot for the delta */
static uint32_t active_accum_ms = 0;

static void activity_update(uint32_t dt_ms) {
    datetime_t t; rtc_get_datetime(&t);
    if (t.day != activity_day) {         /* new day → reset */
        activity_day = t.day;
        steps_today = 0;
        active_seconds_today = 0;
        last_step_total = step_count;
        active_accum_ms = 0;
    }
    if (step_count >= last_step_total)
        steps_today += step_count - last_step_total;
    last_step_total = step_count;

    /* Movement = accel magnitude deviating from 1 g (rest). */
    if (fabsf(accel_magnitude() - 1.0f) > 0.12f) {
        active_accum_ms += dt_ms;
        if (active_accum_ms >= 1000) {
            active_seconds_today += active_accum_ms / 1000;
            active_accum_ms %= 1000;
        }
    }
}

/* ─── Pocket / not-viewed detection → freeze e-ink redraws to save power ─────
 * There's no proximity sensor, so "being viewed" is inferred from the
 * accelerometer: the main screen stops refreshing (e-ink holds its last image
 * at ZERO power) after a stretch of stillness, or immediately when the device
 * is laid face-down. Any handling/jostle — or a button press — wakes it and
 * forces a fresh redraw. (Continuous in-pocket walking looks like handling
 * without a proximity/IR sensor; arming the SC7A20 INT1 line on GP15 for
 * hardware motion-wake is the future path to true MCU sleep.) */
#define VIEW_IDLE_MS 30000u            /* stillness before the screen freezes */
static bool     g_screen_idle  = false;
static uint32_t last_motion_ms = 0;

static void viewing_update(void) {
    static uint32_t last_ms = 0;
    static float    prev_mag = 1.0f;
    uint32_t now = to_ms_since_boot(get_absolute_time());
    if (!mpu_ok) { last_motion_ms = now; return; }   /* no accel → never sleep */
    if (now - last_ms < 200) return;                 /* ~5 Hz is plenty */
    last_ms = now;
    float mag = accel_magnitude();
    float az  = accel_g(accel_z);
    bool moving    = fabsf(mag - prev_mag) > 0.06f;  /* handling / jostle */
    bool face_down = az < -0.55f;                    /* screen-down on a surface */
    prev_mag = mag;
    if (moving && !face_down) last_motion_ms = now;
}

static bool screen_is_viewed(void) {   /* recent motion, not laid face-down */
    return (to_ms_since_boot(get_absolute_time()) - last_motion_ms) < VIEW_IDLE_MS;
}

static void wake_screen(void) {        /* user is present → unfreeze + redraw */
    g_screen_idle  = false;
    last_motion_ms = to_ms_since_boot(get_absolute_time());
}

/* ─── Orientation → display + input rotation (accelerometer auto-rotate) ───
 * Classifies the in-plane gravity vector into one of 4 holds and, with
 * hysteresis, sets g_orientation. The canvas setters then pick a compatible
 * display_rotation, and the joystick input rotation follows too.
 *
 * CALIBRATION: the accel-sign → orientation mapping and the input map below
 * are first-pass guesses — confirm each physical hold on the device and adjust.
 * Throttled to ~4 Hz; cheap enough to leave running. */
/* Classify the in-plane gravity vector into one of the 3 valid holds, or -1
 * when indeterminate (device flat, or joystick-above ignore zone). Pure math —
 * the hysteresis / prime policy lives in orientation_update(). */
static int orientation_classify(float ax, float ay) {
    /* Anchor angle per orientation — classify to the nearest. CALIBRATE: the
     * landscape two come from your readings; OR_TALL is a guess until you send
     * the joystick-bottom angle. */
    static const float OR_ANGLE[3] = {
        [OR_LAND_R] = 90.0f,    /* joystick right  */
        [OR_LAND_L] = 270.0f,   /* joystick left   (calibrated) */
        [OR_TALL]   = 0.0f,     /* joystick bottom (calibrated) */
    };
    /* Joystick-ABOVE (joystick at TOP ≈ 180°) is ignored: keep current. */
    const float IGNORE_CENTER = 180.0f, IGNORE_HALF = 55.0f;

    /* Need enough in-plane gravity to be meaningful (else the device is flat). */
    if (sqrtf(ax * ax + ay * ay) < 0.35f) return -1;

    float ang = atan2f(ay, ax) * 57.2958f;
    if (ang < 0) ang += 360.0f;

    float di = fabsf(ang - IGNORE_CENTER); if (di > 180) di = 360 - di;
    if (di < IGNORE_HALF) return -1;              /* joystick above → ignore */

    int o = 0; float best = 1e9f;
    for (int i = 0; i < 3; i++) {
        float d = fabsf(ang - OR_ANGLE[i]); if (d > 180) d = 360 - d;
        if (d < best) { best = d; o = i; }
    }
    return o;
}

static void orientation_update(void) {
    /* Manual orientation: lock to the user's chosen hold regardless of the
     * accelerometer (so it works even on a unit with no accel). */
    if (!g_auto_rotate) {
        if (g_orientation != (int)g_manual_orient) {
            g_orientation = (int)g_manual_orient;
            input_set_rotation(ORIENT_CFG[g_orientation].in_rot);
        }
        g_orient_primed = true;
    }
    if (!mpu_ok) return;
    static uint32_t last_ms = 0;
    uint32_t now = to_ms_since_boot(get_absolute_time());
    if (now - last_ms < 250) return;
    last_ms = now;

    mpu_read_all();
    pedometer_update();          /* coarse step sampling at this 4 Hz cadence */
    activity_update(250);        /* daily steps + active-time accrual */
    if (!g_auto_rotate) return;   /* manual hold: steps counted, skip auto-rotate */
    /* In-plane gravity angle (degrees, 0..360). Measured anchors:
     *   joystick LEFT  → (X+0.9, Y0.0) → ~0°
     *   joystick RIGHT → (X0.1, Y+0.8) → ~83° (≈90°)
     *   joystick BOTTOM (tall) → TODO: read the HUD in that hold and set below. */
    float ax = accel_g(accel_x), ay = accel_g(accel_y), az = accel_g(accel_z);

#if ORIENT_DEBUG
    {
        float ang = atan2f(ay, ax) * 57.2958f;
        if (ang < 0) ang += 360.0f;
        printf("[ORIENT] ax=%.2f ay=%.2f az=%.2f  ang=%.0f  o=%d tall=%d rot=%d\n",
               (double)ax, (double)ay, (double)az, (double)ang,
               g_orientation, orientation_is_tall(), display_rotation);
    }
#else
    (void)az;
#endif

    int o = orientation_classify(ax, ay);
    if (o < 0) return;                       /* flat / ignore zone: keep current */

    /* First valid verdict after boot applies IMMEDIATELY — the boot splash
     * holds its first frame on g_orient_primed, and waiting out the hysteresis
     * (~1 s at this 4 Hz cadence) would draw it in the wrong hold. Every
     * later switch still goes through the stable-read filter below. */
    if (!g_orient_primed) {
        g_orient_primed = true;
        if (g_orientation != o) {
            g_orientation = o;
            input_set_rotation(ORIENT_CFG[o].in_rot);
        }
        return;
    }

    /* Hysteresis: require a few stable reads before switching. */
    static int cand = 0, stable = 0;
    if (o == cand) { if (stable < 3) stable++; }
    else { cand = o; stable = 0; }

    if (stable >= 3 && g_orientation != cand) {
        g_orientation = cand;
        input_set_rotation(ORIENT_CFG[g_orientation].in_rot);  /* joystick follows */
    }
}

/* ─── NTP ─── */
#define NTP_PORT 123
#define NTP_MSG_LEN 48
#define NTP_DELTA 2208988800ULL

static struct udp_pcb *ntp_pcb = NULL;
static ip_addr_t ntp_server_addr;
static volatile bool ntp_time_received = false;

/* ─── Mood names for the selector ─── */
static const char *mood_names[] = {
    "NORMAL", "WEIRD", "UNHINGED", "ANGRY",
    "SAD", "CHAOTIC", "HUNGRY", "TIRED",
    "SLAP HAPPY", "LAZY", "FAT", "CHILL",
    "CREEPY", "EXCITED", "NOSTALGIC", "HOMESICK",
};
#define MOOD_COUNT 16
static int current_mood = -1;  /* -1 = all moods (random) */

/* Display variant selection */
#if defined(DISPLAY_V2)
  #include "EPD_2in13_V2.h"
  #define DISP_W EPD_2in13_V2_WIDTH
  #define DISP_H EPD_2in13_V2_HEIGHT
  #define EPD_Init()     EPD_2in13_V2_Init()
  #define EPD_Clear()    EPD_2in13_V2_Clear()
  #define EPD_Display(b) EPD_2in13_V2_Display(b)
  #define EPD_Base(b)    EPD_2in13_V2_Display(b)
  #define EPD_Partial(b) EPD_2in13_V2_Display_Partial(b)
  #define EPD_Sleep()    EPD_2in13_V2_Sleep()
  #define DISPLAY_NAME   "V2"
#elif defined(DISPLAY_V3A)
  #include "EPD_2in13_V3a.h"
  #define DISP_W EPD_2in13_V3a_WIDTH
  #define DISP_H EPD_2in13_V3a_HEIGHT
  #define EPD_Init()     EPD_2in13_V3a_Init()
  #define EPD_Clear()    EPD_2in13_V3a_Clear()
  #define EPD_Display(b) EPD_2in13_V3a_Display(b)
  #define EPD_Base(b)    EPD_2in13_V3a_Display_Base(b)
  #define EPD_Partial(b) EPD_2in13_V3a_Display_Partial(b)
  #define EPD_Sleep()    EPD_2in13_V3a_Sleep()
  #define DISPLAY_NAME   "V3a"
#elif defined(DISPLAY_V4)
  #include "EPD_2in13_V4.h"
  #define DISP_W EPD_2in13_V4_WIDTH
  #define DISP_H EPD_2in13_V4_HEIGHT
  #define EPD_Init()     EPD_2in13_V4_Init()
  #define EPD_Clear()    EPD_2in13_V4_Clear()
  #define EPD_Display(b) EPD_2in13_V4_Display(b)
  #define EPD_Base(b)    EPD_2in13_V4_Display_Base(b)
  #define EPD_Partial(b) EPD_2in13_V4_Display_Partial(b)
  #define EPD_Sleep()    EPD_2in13_V4_Sleep()
  #define DISPLAY_NAME   "V4"
#else
  #include "EPD_2in13_V3.h"
  #define DISP_W EPD_2in13_V3_WIDTH
  #define DISP_H EPD_2in13_V3_HEIGHT
  #define EPD_Init()     EPD_2in13_V3_Init()
  #define EPD_Clear()    EPD_2in13_V3_Clear()
  #define EPD_Display(b) EPD_2in13_V3_Display(b)
  #define EPD_Base(b)    EPD_2in13_V3_Display_Base(b)
  #define EPD_Partial(b) EPD_2in13_V3_Display_Partial(b)
  #define EPD_Sleep()    EPD_2in13_V3_Sleep()
  #define DISPLAY_NAME   "V3"
#endif

/* Auto-generated quotes + tagline */
#include "quotes.h"

/* ─── Canvas constants ─── */
#define IMG_W         250
#define IMG_H         122
#define IMG_ROW_BYTES ((IMG_W + 7) / 8)  /* 32 */

/* Mood values (match quotes.h mood_map in devtool.py) */
#define MOOD_NORMAL   0
#define MOOD_WEIRD    1
#define MOOD_UNHINGED 2
#define MOOD_ANGRY    3
#define MOOD_SAD      4
#define MOOD_CHAOTIC  5
#define MOOD_HUNGRY   6
#define MOOD_TIRED    7
#define MOOD_SLAPHAPPY 8
#define MOOD_LAZY      9
#define MOOD_FAT       10
#define MOOD_CHILL     11
#define MOOD_CREEPY     12
#define MOOD_EXCITED   13
#define MOOD_NOSTALGIC 14
#define MOOD_HOMESICK  15

/* Mouth expressions */
#define EXPR_SMIRK    0
#define EXPR_OPEN     1
#define EXPR_SMILE    2
#define EXPR_WEIRD    3
#define EXPR_UNHINGED 4
#define EXPR_ANGRY    5
#define EXPR_SAD      6
#define EXPR_CHAOTIC  7
#define EXPR_HUNGRY   8
#define EXPR_TIRED    9
#define EXPR_SLAPHAPPY 10
#define EXPR_LAZY      11
#define EXPR_FAT       12
#define EXPR_CHILL     13
#define EXPR_CREEPY     14
#define EXPR_EXCITED   15
#define EXPR_NOSTALGIC 16
#define EXPR_HOMESICK  17

/* Landscape frame buffer (1 = black pixel, packed MSB-first) */
/* The logical drawing canvas. Two shapes share one buffer:
 *   WIDE  250x122 (row bytes 32) — side-by-side octopus screen + all menus
 *   TALL  122x250 (row bytes 16) — the "longways" stacked layout
 * transpose_to_display() rotates whichever canvas is active onto the fixed
 * 122x250 panel. Buffer is sized for the larger (tall: 16*250 = 4000). */
#define CANVAS_BYTES_MAX 4000
static uint8_t frame[CANVAS_BYTES_MAX];
static int canvas_w        = IMG_W;          /* default WIDE */
static int canvas_h        = IMG_H;
static int canvas_row_bytes = IMG_ROW_BYTES;

/* Orientation enum/config + orientation_is_tall() are declared earlier (next
 * to the orientation state) so the accel classifier can use them. */
static void set_canvas_wide(void) {
    canvas_w = 250; canvas_h = 122; canvas_row_bytes = 32;
    /* Wide screens must use a wide angle (90/270); a tall hold viewing a wide
     * menu falls back to 90 until the tall-menu rework lands. */
    display_rotation = orientation_is_tall() ? 90 : ORIENT_CFG[g_orientation].disp_rot;
}
static void set_canvas_tall(void) {
    canvas_w = 122; canvas_h = 250; canvas_row_bytes = 16;
    display_rotation = ORIENT_CFG[g_orientation].disp_rot;
}

/* ── Display hand-off buffers (Phase 2 / RTOS) ──────────────────────────────
 * ui_buf        : the UI task's PRIVATE transpose target. transpose_to_display()
 *                 writes the finished panel image here.
 * display_buf[2]: the two Display-task buffers. display_render() copies ui_buf
 *                 into a FREE one and hands its index to the Display task (core 1),
 *                 which does the ~300 ms e-ink refresh. Two buffers let the UI
 *                 prepare the next frame while core 1 is still pushing the last
 *                 one — with zero shared-buffer race (ownership passes by queue). */
#define DISPLAY_BUF_SIZE (((DISP_W + 7) / 8) * DISP_H)
static uint8_t ui_buf[DISPLAY_BUF_SIZE];
static uint8_t display_buf[2][DISPLAY_BUF_SIZE];

/* Layout origin — lets the octopus art be repositioned (e.g. bottom of the
 * tall canvas) without touching any of the px_set_off drawing routines. */
static int layout_ox = 0, layout_oy = 0;

/* Forward decls — status icons are defined later but used by the tall layout. */
static void draw_battery_icon(int x0, int y0);
static void draw_wifi_icon(int x0, int y0, bool connected);
static void draw_bt_icon(int x0, int y0);
static void draw_social_icon(int x0, int y0);

/* Vertical offset — pushes octopus + bubble down to make room for clock */
#define Y_OFF 12

/* ─── Body animation transform (set per-frame before rendering) ─── */
static int body_dx = 0;     /* global x shift */
static int body_dy = 0;     /* global y shift */
static int body_x_expand = 0; /* expand/shrink body spans (+ = wider) */
/* Per-row wobble amplitude and phase (for wavy effects) */
static float wobble_amp = 0;
static float wobble_freq = 0;
static float wobble_phase = 0;

static int row_wobble(int y) {
    if (wobble_amp == 0) return 0;
    return (int)(wobble_amp * sinf(y * wobble_freq + wobble_phase));
}

/* ─── Pixel helpers ─── */
static inline void px_set(int x, int y) {
    if (x >= 0 && x < canvas_w && y >= 0 && y < canvas_h)
        frame[y * canvas_row_bytes + x / 8] |= (0x80 >> (x & 7));
}
static inline void px_clr(int x, int y) {
    if (x >= 0 && x < canvas_w && y >= 0 && y < canvas_h)
        frame[y * canvas_row_bytes + x / 8] &= ~(0x80 >> (x & 7));
}
/* Offset versions — add layout origin + Y_OFF + body transform before drawing */
static inline void px_set_off(int x, int y) {
    px_set(x + body_dx + row_wobble(y) + layout_ox, y + Y_OFF + body_dy + layout_oy);
}
static inline void px_clr_off(int x, int y) {
    px_clr(x + body_dx + row_wobble(y) + layout_ox, y + Y_OFF + body_dy + layout_oy);
}

/* ─── Octopus body (RLE: y, num_spans, x0, x1, ...) terminated by 0xFF ─── */
static const uint8_t body_rle[] = {
    10,1, 22,48,  11,1, 18,52,  12,1, 16,54,  13,1, 14,56,
    14,1, 13,57,  15,1, 12,58,  16,1, 11,59,  17,1, 10,60,
    18,1, 10,60,  19,1,  9,61,  20,1,  9,61,  21,1,  9,61,
    22,1,  9,61,  23,1,  9,61,  24,1,  9,61,  25,1,  9,61,
    26,1,  9,61,  27,1,  9,61,  28,1, 10,60,  29,1, 10,60,
    30,1, 10,60,  31,1, 10,60,  32,1, 10,60,  33,1, 10,60,
    34,1, 10,60,  35,1, 10,60,  36,1, 10,60,  37,1, 10,60,
    38,1, 10,60,  39,1, 10,60,  40,1, 10,60,  41,1, 11,59,
    42,1, 11,59,  43,1, 12,58,  44,1, 13,57,  45,1, 14,56,
    46,1, 12,58,  47,1, 11,59,  48,1, 10,60,  49,1, 10,60,
    50,1, 11,59,  51,1, 12,58,  52,1, 13,57,  53,1, 14,56,
    54,1, 15,55,
    /* Tentacles */
    55,5, 10,17, 21,28, 32,39, 43,50, 54,61,
    56,5,  8,15, 19,26, 30,37, 45,52, 56,63,
    57,5,  7,14, 18,24, 29,35, 47,53, 58,64,
    58,5,  6,12, 19,25, 31,37, 46,52, 57,63,
    59,5,  7,13, 21,27, 33,39, 44,50, 55,61,
    60,5,  8,14, 20,26, 31,37, 43,49, 54,60,
    61,5,  9,14, 18,24, 30,36, 44,50, 56,62,
    62,5,  8,13, 17,22, 31,37, 46,52, 57,63,
    63,5,  7,12, 18,23, 33,38, 45,51, 55,61,
    64,5,  8,13, 20,25, 32,37, 43,48, 54,59,
    65,5,  9,14, 19,24, 30,35, 44,49, 55,60,
    66,5, 10,14, 17,22, 31,36, 46,51, 57,62,
    67,5,  9,13, 18,22, 33,37, 45,50, 56,61,
    68,5,  8,12, 19,23, 32,36, 43,48, 54,59,
    69,5,  9,13, 21,25, 30,34, 44,48, 55,59,
    70,5, 10,14, 20,24, 31,35, 46,50, 57,61,
    71,5, 11,14, 18,22, 33,37, 45,49, 56,60,
    72,5, 10,13, 19,22, 32,35, 43,47, 54,58,
    73,5,  9,12, 20,23, 30,33, 44,47, 55,58,
    74,5, 10,13, 21,24, 31,34, 46,49, 57,60,
    75,5, 11,14, 20,23, 33,36, 45,48, 56,59,
    76,5, 12,14, 19,22, 32,35, 43,46, 54,57,
    77,5, 11,13, 20,22, 30,33, 44,46, 55,57,
    78,5, 10,12, 21,23, 31,33, 45,47, 56,58,
    79,5, 11,13, 22,24, 32,34, 44,46, 55,57,
    80,5, 12,14, 21,23, 33,35, 43,45, 54,56,
    0xFF /* terminator */
};

/* ─── 5×7 bitmap font ─── */
/* Index: A=0..Z=25, 0=26..9=35, ' '=36, .=37, ,=38, !=39, ?=40,
   '=41, -=42, ~=43, /=44, :=45, (=46, )=47, %=48 */
static const uint8_t font5x7[][7] = {
    {0x0e,0x11,0x11,0x1f,0x11,0x11,0x11}, /* A */
    {0x1e,0x11,0x11,0x1e,0x11,0x11,0x1e}, /* B */
    {0x0e,0x11,0x10,0x10,0x10,0x11,0x0e}, /* C */
    {0x1e,0x11,0x11,0x11,0x11,0x11,0x1e}, /* D */
    {0x1f,0x10,0x10,0x1e,0x10,0x10,0x1f}, /* E */
    {0x1f,0x10,0x10,0x1e,0x10,0x10,0x10}, /* F */
    {0x0e,0x11,0x10,0x17,0x11,0x11,0x0e}, /* G */
    {0x11,0x11,0x11,0x1f,0x11,0x11,0x11}, /* H */
    {0x1f,0x04,0x04,0x04,0x04,0x04,0x1f}, /* I */
    {0x07,0x02,0x02,0x02,0x02,0x12,0x0c}, /* J */
    {0x11,0x12,0x14,0x18,0x14,0x12,0x11}, /* K */
    {0x10,0x10,0x10,0x10,0x10,0x10,0x1f}, /* L */
    {0x11,0x1b,0x15,0x15,0x11,0x11,0x11}, /* M */
    {0x11,0x11,0x19,0x15,0x13,0x11,0x11}, /* N */
    {0x0e,0x11,0x11,0x11,0x11,0x11,0x0e}, /* O */
    {0x1e,0x11,0x11,0x1e,0x10,0x10,0x10}, /* P */
    {0x0e,0x11,0x11,0x11,0x15,0x12,0x0d}, /* Q */
    {0x1e,0x11,0x11,0x1e,0x14,0x12,0x11}, /* R */
    {0x0e,0x11,0x10,0x0e,0x01,0x11,0x0e}, /* S */
    {0x1f,0x04,0x04,0x04,0x04,0x04,0x04}, /* T */
    {0x11,0x11,0x11,0x11,0x11,0x11,0x0e}, /* U */
    {0x11,0x11,0x11,0x11,0x0a,0x0a,0x04}, /* V */
    {0x11,0x11,0x11,0x15,0x15,0x15,0x0a}, /* W */
    {0x11,0x11,0x0a,0x04,0x0a,0x11,0x11}, /* X */
    {0x11,0x11,0x0a,0x04,0x04,0x04,0x04}, /* Y */
    {0x1f,0x01,0x02,0x04,0x08,0x10,0x1f}, /* Z */
    {0x0e,0x11,0x13,0x15,0x19,0x11,0x0e}, /* 0 */
    {0x04,0x0c,0x04,0x04,0x04,0x04,0x0e}, /* 1 */
    {0x0e,0x11,0x01,0x06,0x08,0x10,0x1f}, /* 2 */
    {0x0e,0x11,0x01,0x06,0x01,0x11,0x0e}, /* 3 */
    {0x02,0x06,0x0a,0x12,0x1f,0x02,0x02}, /* 4 */
    {0x1f,0x10,0x1e,0x01,0x01,0x11,0x0e}, /* 5 */
    {0x0e,0x11,0x10,0x1e,0x11,0x11,0x0e}, /* 6 */
    {0x1f,0x01,0x02,0x04,0x08,0x08,0x08}, /* 7 */
    {0x0e,0x11,0x11,0x0e,0x11,0x11,0x0e}, /* 8 */
    {0x0e,0x11,0x11,0x0f,0x01,0x11,0x0e}, /* 9 */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* ' ' */
    {0x00,0x00,0x00,0x00,0x00,0x0c,0x0c}, /* . */
    {0x00,0x00,0x00,0x00,0x04,0x04,0x08}, /* , */
    {0x04,0x04,0x04,0x04,0x04,0x00,0x04}, /* ! */
    {0x0e,0x11,0x01,0x06,0x04,0x00,0x04}, /* ? */
    {0x04,0x04,0x08,0x00,0x00,0x00,0x00}, /* ' */
    {0x00,0x00,0x00,0x1f,0x00,0x00,0x00}, /* - */
    {0x00,0x00,0x08,0x15,0x02,0x00,0x00}, /* ~ */
    {0x01,0x02,0x02,0x04,0x08,0x08,0x10}, /* / */
    {0x00,0x0c,0x0c,0x00,0x0c,0x0c,0x00}, /* : */
    {0x02,0x04,0x08,0x08,0x08,0x04,0x02}, /* ( */
    {0x08,0x04,0x02,0x02,0x02,0x04,0x08}, /* ) */
    {0x19,0x1a,0x02,0x04,0x08,0x0b,0x13}, /* % */
};

static const char font_chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 .,!?'-~/:()\%";

static int font_index(char c) {
    for (int i = 0; font_chars[i]; i++)
        if (font_chars[i] == c) return i;
    return 36; /* space fallback */
}

/* ─── Drawing primitives ─── */

static void fill_circle(int cx, int cy, int r_sq, int set) {
    int r = 5;
    for (int dy = -r; dy <= r; dy++)
        for (int dx = -r; dx <= r; dx++)
            if (dx * dx + dy * dy <= r_sq) {
                if (set) px_set_off(cx + dx, cy + dy);
                else     px_clr_off(cx + dx, cy + dy);
            }
}

static void draw_body(void) {
    const uint8_t *p = body_rle;
    while (*p != 0xFF) {
        int y = *p++;
        int n = *p++;
        for (int i = 0; i < n; i++) {
            int x0 = *p++;
            int x1 = *p++;
            for (int x = x0; x <= x1; x++)
                px_set_off(x, y);
        }
    }
}

/* Body with span expansion applied (uses body_x_expand global) */
static void draw_body_transformed(void) {
    const uint8_t *p = body_rle;
    while (*p != 0xFF) {
        int y = *p++;
        int n = *p++;
        for (int i = 0; i < n; i++) {
            int x0 = *p++;
            int x1 = *p++;
            int ax0 = x0 - body_x_expand;
            int ax1 = x1 + body_x_expand;
            if (ax0 < 0) ax0 = 0;
            if (ax1 >= IMG_W) ax1 = IMG_W - 1;
            for (int x = ax0; x <= ax1; x++)
                px_set_off(x, y);
        }
    }
}

static void draw_eyes(void) {
    /* White eye sockets: two circles r²=16 at (22,25) and (48,25) */
    fill_circle(22, 25, 16, 0);
    fill_circle(48, 25, 16, 0);
}

static void draw_pupils_normal(void) {
    /* Black pupils: r²=4 at (23,26) and (49,26) */
    fill_circle(23, 26, 4, 1);
    fill_circle(49, 26, 4, 1);
    /* White highlights: r²=1 at (20,23) and (46,23) */
    fill_circle(20, 23, 1, 0);
    fill_circle(46, 23, 1, 0);
}

static void draw_pupils_weird(void) {
    /* Misaligned: left up-left (21,24), right down-right (50,28) */
    fill_circle(21, 24, 4, 1);
    fill_circle(50, 28, 4, 1);
    fill_circle(20, 23, 1, 0);
    fill_circle(46, 23, 1, 0);
}

static void draw_pupils_unhinged(void) {
    /* Tiny pinprick pupils, no highlights */
    px_set_off(22, 25); px_set_off(23, 25); px_set_off(22, 26); px_set_off(23, 26);
    px_set_off(48, 25); px_set_off(49, 25); px_set_off(48, 26); px_set_off(49, 26);
}

static void draw_pupils_angry(void) {
    /* Pupils shifted inward and down — glaring toward the nose */
    fill_circle(25, 27, 4, 1);  /* left: shifted right+down */
    fill_circle(47, 27, 4, 1);  /* right: shifted left+down */
    fill_circle(23, 24, 1, 0);  /* highlights */
    fill_circle(45, 24, 1, 0);
}

static void draw_brows_angry(void) {
    /* Thick slanted half-circle arcs across top of eye sockets.
       Outer edges high, inner edges low — angry V shape.
       Must match Python _octo_angry_eyes(). */
    for (int i = 0; i < 18; i++) {
        float t = i / 17.0f;
        int x = 14 + (int)(t * 16);
        float arc = 2.5f * sinf(t * 3.14159f);
        int y = (int)(20 + t * 5 - arc);
        for (int dy = 0; dy < 3; dy++) px_set_off(x, y + dy);
        px_set_off(x + 1, y + 1);
    }
    for (int i = 0; i < 18; i++) {
        float t = i / 17.0f;
        int x = 40 + (int)(t * 16);
        float arc = 2.5f * sinf(t * 3.14159f);
        int y = (int)(25 - t * 5 - arc);
        for (int dy = 0; dy < 3; dy++) px_set_off(x, y + dy);
        px_set_off(x + 1, y + 1);
    }
}

static void draw_pupils_sad(void) {
    /* Pupils shifted downward — looking at the floor */
    fill_circle(23, 28, 4, 1);
    fill_circle(49, 28, 4, 1);
    fill_circle(21, 25, 1, 0);
    fill_circle(47, 25, 1, 0);
}

static void draw_brows_sad(void) {
    /* Droopy brows — outer edges low, inner edges high (inverse of angry) */
    for (int i = 0; i < 18; i++) {
        float t = i / 17.0f;
        int x = 14 + (int)(t * 16);
        float arc = 2.5f * sinf(t * 3.14159f);
        int y = (int)(25 - t * 5 - arc);  /* inner high, outer low */
        for (int dy = 0; dy < 3; dy++) px_set_off(x, y + dy);
    }
    for (int i = 0; i < 18; i++) {
        float t = i / 17.0f;
        int x = 40 + (int)(t * 16);
        float arc = 2.5f * sinf(t * 3.14159f);
        int y = (int)(20 + t * 5 - arc);
        for (int dy = 0; dy < 3; dy++) px_set_off(x, y + dy);
    }
}

static void draw_pupils_chaotic(void) {
    /* Spiral/ring eyes — concentric circles for dizzy look */
    for (int ecx_i = 0; ecx_i < 2; ecx_i++) {
        int ecx = (ecx_i == 0) ? 22 : 48;
        /* Outer ring */
        for (int dy = -3; dy <= 3; dy++)
            for (int dx = -3; dx <= 3; dx++) {
                int dist = dx * dx + dy * dy;
                if (dist >= 5 && dist <= 9)
                    px_set_off(ecx + dx, 25 + dy);
            }
        /* Center dot */
        px_set_off(ecx, 25);
    }
}

/* ─── Mouth expressions ─── */

static void draw_mouth_smirk(void) {
    for (int x = 28; x < 44; x++) {
        float t = (x - 28) / 15.0f;
        float tilt = -2.0f + t * 4.0f;
        float v = 2.0f * t - 1.0f;
        float arc = (fabsf(v) < 1.0f) ? 5.0f * sqrtf(1.0f - v * v) : 0.0f;
        int yc = (int)(39.0f + tilt + arc);
        px_clr_off(x, yc);
        px_set_off(x, yc - 1);
        px_set_off(x, yc + 1);
    }
}

static void draw_mouth_smile(void) {
    for (int x = 26; x < 45; x++) {
        int cy = 38 + ((x - 35) * (x - 35)) / 25;
        px_set_off(x, cy);
        px_set_off(x, cy + 1);
    }
}

static void draw_mouth_open(void) {
    int cx = 35, cy = 40, rx = 7, ry = 5;
    for (int dy = -4; dy <= 4; dy++)
        for (int dx = -6; dx <= 6; dx++)
            if (dx*dx*16 + dy*dy*36 <= 36*16)
                px_clr_off(cx + dx, cy + dy);
    for (int dy = -ry; dy <= ry; dy++)
        for (int dx = -rx; dx <= rx; dx++) {
            if (dx*dx*ry*ry + dy*dy*rx*rx > rx*rx*ry*ry) continue;
            for (int nd = 0; nd < 4; nd++) {
                int nx = dx + ((nd==0)?-1:(nd==1)?1:0);
                int ny = dy + ((nd==2)?-1:(nd==3)?1:0);
                if (nx*nx*ry*ry + ny*ny*rx*rx > rx*rx*ry*ry) {
                    px_set_off(cx + dx, cy + dy);
                    break;
                }
            }
        }
}

static void draw_mouth_weird(void) {
    for (int x = 24; x < 48; x++) {
        float t = (x - 24) / 23.0f;
        int yc = 39 + (int)(3.5f * sinf(t * 3.14159f * 3.0f));
        px_clr_off(x, yc);
        px_set_off(x, yc - 1);
        px_set_off(x, yc + 1);
    }
}

static void draw_mouth_unhinged(void) {
    int cx = 35, cy = 41, rx = 10, ry = 7;
    for (int dy = -6; dy <= 6; dy++)
        for (int dx = -9; dx <= 9; dx++)
            if (dx*dx*36 + dy*dy*81 <= 81*36)
                px_clr_off(cx + dx, cy + dy);
    for (int dy = -ry; dy <= ry; dy++)
        for (int dx = -rx; dx <= rx; dx++) {
            if (dx*dx*ry*ry + dy*dy*rx*rx > rx*rx*ry*ry) continue;
            for (int nd = 0; nd < 4; nd++) {
                int nx = dx + ((nd==0)?-1:(nd==1)?1:0);
                int ny = dy + ((nd==2)?-1:(nd==3)?1:0);
                if (nx*nx*ry*ry + ny*ny*rx*rx > rx*rx*ry*ry) {
                    px_set_off(cx + dx, cy + dy);
                    break;
                }
            }
        }
    for (int x = cx - 7; x <= cx + 7; x += 3) {
        px_set_off(x, cy - 5);
        px_set_off(x, cy - 4);
        px_set_off(x + 1, cy - 4);
    }
}

static void draw_mouth_angry(void) {
    /* Tight downward frown — inverted parabola */
    for (int x = 28; x < 43; x++) {
        int cy = 40 - ((x - 35) * (x - 35)) / 20;
        px_set_off(x, cy);
        px_set_off(x, cy + 1);
    }
}

static void draw_mouth_sad(void) {
    /* Gentle downward curve frown */
    for (int x = 26; x < 45; x++) {
        int cy = 42 - ((x - 35) * (x - 35)) / 30;
        px_set_off(x, cy);
        px_set_off(x, cy + 1);
    }
}

static void draw_mouth_chaotic(void) {
    /* Zigzag lightning-bolt mouth */
    for (int x = 24; x < 48; x++) {
        int phase = (x - 24) % 6;
        int y = (phase < 3) ? 38 + phase * 2 : 44 - phase * 2 + 6;
        px_set_off(x, y);
        px_set_off(x, y + 1);
    }
}

static void draw_pupils_hungry(void) {
    /* Pupils shifted upward — staring at imaginary food */
    fill_circle(23, 23, 4, 1);
    fill_circle(49, 23, 4, 1);
    fill_circle(21, 21, 1, 0);
    fill_circle(47, 21, 1, 0);
}

static void draw_mouth_hungry(void) {
    /* Drooling open mouth — wide oval + drool drops */
    int cx = 35, cy = 40, rx = 8, ry = 5;
    /* White interior */
    for (int dy = -(ry-1); dy <= ry-1; dy++)
        for (int dx = -(rx-1); dx <= rx-1; dx++)
            if (dx*dx*(ry-1)*(ry-1) + dy*dy*(rx-1)*(rx-1) <= (rx-1)*(rx-1)*(ry-1)*(ry-1))
                px_clr_off(cx+dx, cy+dy);
    /* Black border */
    for (int dy = -ry; dy <= ry; dy++)
        for (int dx = -rx; dx <= rx; dx++) {
            if (dx*dx*ry*ry + dy*dy*rx*rx > rx*rx*ry*ry) continue;
            for (int nd = 0; nd < 4; nd++) {
                int nx = dx + ((nd==0)?-1:(nd==1)?1:0);
                int ny = dy + ((nd==2)?-1:(nd==3)?1:0);
                if (nx*nx*ry*ry + ny*ny*rx*rx > rx*rx*ry*ry) {
                    px_set_off(cx+dx, cy+dy);
                    break;
                }
            }
        }
    /* Drool drops */
    for (int dy = 1; dy < 6; dy++) px_set_off(33, cy+ry+dy);
    for (int dy = 1; dy < 4; dy++) px_set_off(37, cy+ry+dy+1);
}

static void draw_pupils_tired(void) {
    /* Tiny sleepy pupils low in half-closed eyes */
    for (int dx = -1; dx <= 1; dx++) {
        px_set_off(22+dx, 27); px_set_off(22+dx, 28);
        px_set_off(48+dx, 27); px_set_off(48+dx, 28);
    }
}

static void draw_lids_tired(void) {
    /* Half-closed eyelids: fill top half of eye sockets black */
    for (int ecx_i = 0; ecx_i < 2; ecx_i++) {
        int ecx = (ecx_i == 0) ? 22 : 48;
        for (int dy = -4; dy < -1; dy++)
            for (int dx = -4; dx <= 4; dx++)
                if (dx*dx + dy*dy <= 16)
                    px_set_off(ecx+dx, 25+dy);
    }
}

static void draw_mouth_tired(void) {
    /* Yawn mouth — tall oval, open wide vertically */
    int cx = 35, cy = 40, rx = 5, ry = 7;
    for (int dy = -(ry-1); dy <= ry-1; dy++)
        for (int dx = -(rx-1); dx <= rx-1; dx++)
            if (dx*dx*(ry-1)*(ry-1) + dy*dy*(rx-1)*(rx-1) <= (rx-1)*(rx-1)*(ry-1)*(ry-1))
                px_clr_off(cx+dx, cy+dy);
    for (int dy = -ry; dy <= ry; dy++)
        for (int dx = -rx; dx <= rx; dx++) {
            if (dx*dx*ry*ry + dy*dy*rx*rx > rx*rx*ry*ry) continue;
            for (int nd = 0; nd < 4; nd++) {
                int nx = dx + ((nd==0)?-1:(nd==1)?1:0);
                int ny = dy + ((nd==2)?-1:(nd==3)?1:0);
                if (nx*nx*ry*ry + ny*ny*rx*rx > rx*rx*ry*ry) {
                    px_set_off(cx+dx, cy+dy);
                    break;
                }
            }
        }
}

static void draw_eyes_slaphappy(void) {
    /* Left eye: squint shut (fill back to black, white slit) */
    for (int dy = -4; dy <= 4; dy++)
        for (int dx = -4; dx <= 4; dx++)
            if (dx*dx + dy*dy <= 16)
                px_set_off(22+dx, 25+dy);
    for (int dx = -3; dx <= 3; dx++)
        px_clr_off(22+dx, 25);
    /* Right eye: oversized pupil */
    fill_circle(49, 26, 9, 1);
}

static void draw_mouth_slaphappy(void) {
    /* Wide wobbly grin */
    for (int x = 22; x < 49; x++) {
        float t = (x - 22) / 26.0f;
        int base = 38 + ((x-35)*(x-35)) / 20;
        int wobble = (int)(1.5f * sinf(t * 3.14159f * 4.0f));
        int y = base + wobble;
        px_set_off(x, y);
        px_set_off(x, y+1);
    }
}

/* ─── Lazy: nearly-closed eyes, flat mouth ─── */

static void draw_lids_lazy(void) {
    /* Cover most of each eye socket — leave only bottom sliver open */
    for (int e = 0; e < 2; e++) {
        int ecx = e ? 48 : 22;
        for (int dy = -4; dy < 2; dy++)
            for (int dx = -4; dx <= 4; dx++)
                if (dx*dx + dy*dy <= 16)
                    px_set_off(ecx+dx, 25+dy);
    }
}

static void draw_pupils_lazy(void) {
    /* Barely visible dots low in the slit */
    for (int e = 0; e < 2; e++) {
        int ecx = e ? 48 : 22;
        px_set_off(ecx, 28);
        px_set_off(ecx+1, 28);
    }
}

static void draw_mouth_lazy(void) {
    /* Flat horizontal line — minimal effort */
    for (int x = 29; x < 42; x++) {
        px_set_off(x, 40);
        px_set_off(x, 41);
    }
}

/* ─── Fat: content wide pupils, smile with cheek puffs ─── */

static void draw_pupils_fat(void) {
    /* Wider pupils — happy and satisfied */
    for (int e = 0; e < 2; e++) {
        int ecx = e ? 49 : 23;
        for (int dy = -3; dy <= 3; dy++)
            for (int dx = -3; dx <= 3; dx++)
                if (dx*dx + dy*dy <= 9)
                    px_set_off(ecx+dx, 26+dy);
    }
}

static void draw_mouth_fat(void) {
    /* Wide satisfied smile + cheek puffs */
    for (int x = 24; x < 47; x++) {
        int cy = 38 + ((x-35)*(x-35)) / 18;
        px_set_off(x, cy);
        px_set_off(x, cy+1);
    }
    /* Cheek puffs */
    int cheeks[][2] = {{23,39},{47,39}};
    for (int c = 0; c < 2; c++)
        for (int dy = -2; dy <= 2; dy++)
            for (int dx = -2; dx <= 2; dx++)
                if (dx*dx + dy*dy <= 4)
                    px_set_off(cheeks[c][0]+dx, cheeks[c][1]+dy);
}

/* ─── Chill: side-glancing pupils, relaxed half-smile ─── */

static void draw_pupils_chill(void) {
    /* Pupils shifted right — looking to the side */
    int centers[][2] = {{25,26},{51,26}};
    for (int e = 0; e < 2; e++)
        for (int dy = -2; dy <= 2; dy++)
            for (int dx = -2; dx <= 2; dx++)
                if (dx*dx + dy*dy <= 4)
                    px_set_off(centers[e][0]+dx, centers[e][1]+dy);
}

static void draw_mouth_chill(void) {
    /* Slight asymmetric half-smile — relaxed */
    for (int x = 29; x < 44; x++) {
        float t = (x - 29) / 14.0f;
        int y = 40 + (int)(1.5f * t * t);
        px_set_off(x, y);
        px_set_off(x, y+1);
    }
}

/* ─── Creepy: heart-shaped pupils, tongue-out mouth ─── */

static void draw_pupils_creepy(void) {
    /* Heart-shaped pupils in each eye socket */
    for (int e = 0; e < 2; e++) {
        int ecx = e ? 48 : 22;
        /* Top bumps */
        static const int8_t top[][2] = {{-2,-1},{-1,-2},{0,-1},{1,-2},{2,-1}};
        for (int i = 0; i < 5; i++)
            px_set_off(ecx+top[i][0], 25+top[i][1]);
        /* Middle row */
        for (int dx = -2; dx <= 2; dx++)
            px_set_off(ecx+dx, 25);
        /* Lower taper */
        for (int dx = -1; dx <= 1; dx++)
            px_set_off(ecx+dx, 26);
        /* Bottom point */
        px_set_off(ecx, 27);
    }
}

static void draw_mouth_creepy(void) {
    /* Wide open smile with tongue hanging out */
    int cx = 35, cy = 39, rx = 8, ry = 5;
    for (int dy = 0; dy <= ry; dy++)
        for (int dx = -rx; dx <= rx; dx++) {
            int in = (dx*dx)*(ry*ry) + (dy*dy)*(rx*rx) <= (rx*rx)*(ry*ry);
            if (!in) continue;
            int edge = 0;
            if (dy == 0) edge = 1;
            else {
                int ndxs[] = {-1,1,0,0}, ndys[] = {0,0,-1,1};
                for (int n = 0; n < 4; n++) {
                    int nx = dx+ndxs[n], ny = dy+ndys[n];
                    if (ny < 0) continue;
                    if ((nx*nx)*(ry*ry)+(ny*ny)*(rx*rx) > (rx*rx)*(ry*ry))
                        { edge = 1; break; }
                }
            }
            if (edge) px_set_off(cx+dx, cy+dy);
            else      px_clr_off(cx+dx, cy+dy);
        }
    /* Tongue */
    for (int dy = 1; dy < 5; dy++)
        for (int dx = -2; dx <= 2; dx++)
            if (dx*dx + dy*dy <= 8)
                px_set_off(cx+dx, cy+ry+dy);
    /* Tongue interior */
    for (int dy = 2; dy < 4; dy++)
        for (int dx = -1; dx <= 1; dx++)
            px_clr_off(cx+dx, cy+ry+dy);
}

/* ─── Excited: star/sparkle pupils, wide open smile ─── */

static void draw_pupils_excited(void) {
    /* Star/sparkle cross-shaped pupils in each eye socket */
    for (int e = 0; e < 2; e++) {
        int ecx = e ? 48 : 22;
        /* Plus/cross shape */
        for (int d = -2; d <= 2; d++) {
            px_set_off(ecx + d, 25);   /* horizontal bar */
            px_set_off(ecx, 25 + d);   /* vertical bar */
        }
        /* Diagonal tips for sparkle */
        px_set_off(ecx - 1, 24); px_set_off(ecx + 1, 24);
        px_set_off(ecx - 1, 26); px_set_off(ecx + 1, 26);
    }
}

static void draw_mouth_excited(void) {
    /* Wide open smile — bigger upward curve than normal */
    for (int x = 22; x < 49; x++) {
        int cy = 37 + ((x - 35) * (x - 35)) / 12;
        px_set_off(x, cy);
        px_set_off(x, cy + 1);
    }
}

/* ─── Nostalgic: pupils looking up-right, gentle half-smile ─── */

static void draw_pupils_nostalgic(void) {
    /* Pupils shifted up and to the right — remembering */
    int centers[][2] = {{24, 23}, {50, 23}};
    for (int e = 0; e < 2; e++)
        for (int dy = -2; dy <= 2; dy++)
            for (int dx = -2; dx <= 2; dx++)
                if (dx*dx + dy*dy <= 4)
                    px_set_off(centers[e][0]+dx, centers[e][1]+dy);
}

static void draw_mouth_nostalgic(void) {
    /* Gentle closed half-smile — small, wistful */
    for (int x = 31; x < 40; x++) {
        float t = (x - 31) / 8.0f;
        float v = 2.0f * t - 1.0f;
        int y = 40 + (int)(1.5f * v * v);
        px_set_off(x, y);
        px_set_off(x, y + 1);
    }
}

/* ─── Homesick: watery eyes with tears, wobbly mouth ─── */

static void draw_pupils_homesick(void) {
    /* Normal-ish pupils, slightly lowered (sad-like) */
    for (int e = 0; e < 2; e++) {
        int ecx = e ? 49 : 23;
        for (int dy = -2; dy <= 2; dy++)
            for (int dx = -2; dx <= 2; dx++)
                if (dx*dx + dy*dy <= 4)
                    px_set_off(ecx+dx, 27+dy);
    }
}

static void draw_tears_homesick(void) {
    /* Tear drop pixels below each eye socket */
    for (int e = 0; e < 2; e++) {
        int ecx = e ? 48 : 22;
        px_set_off(ecx, 31);
        px_set_off(ecx, 32);
        px_set_off(ecx, 33);
        px_set_off(ecx - 1, 32);
        px_set_off(ecx + 1, 32);
    }
}

static void draw_mouth_homesick(void) {
    /* Wobbly trying-not-to-cry line — slightly wavy horizontal */
    for (int x = 28; x < 43; x++) {
        float t = (x - 28) / 14.0f;
        int y = 40 + (int)(1.5f * sinf(t * 3.14159f * 3.0f));
        px_set_off(x, y);
        px_set_off(x, y + 1);
    }
}

/* ─── Chat bubble ─── */

static void draw_bubble(void) {
    int bx = 75, by = 5 + Y_OFF, bw = 170, bh = 70;
    /* Top/bottom edges (double thick) */
    for (int x = bx + 3; x < bx + bw - 3; x++) {
        px_set(x, by); px_set(x, by + 1);
        px_set(x, by + bh - 1); px_set(x, by + bh - 2);
    }
    /* Left/right edges */
    for (int y = by + 3; y < by + bh - 3; y++) {
        px_set(bx, y); px_set(bx + 1, y);
        px_set(bx + bw - 1, y); px_set(bx + bw - 2, y);
    }
    /* Rounded corners */
    int corners[][2] = {{bx+2,by+2},{bx+bw-3,by+2},{bx+2,by+bh-3},{bx+bw-3,by+bh-3}};
    for (int c = 0; c < 4; c++)
        for (int dy = -1; dy <= 1; dy++)
            for (int dx = -1; dx <= 1; dx++)
                if (abs(dx) + abs(dy) <= 1)
                    px_set(corners[c][0]+dx, corners[c][1]+dy);
    /* Speech tail */
    int tb = 35 + Y_OFF;
    static const int8_t tail_dx[] = {0,-1,-2,-3,-4,-5,-6,-7,-6,-5,-4,-3,-2,-1,0};
    static const int8_t tail_dy[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 8, 8, 7, 6, 5, 4};
    for (int i = 0; i < 15; i++)
        px_set(bx + tail_dx[i], tb + tail_dy[i]);
}

/* ─── Text rendering ─── */

static void draw_char(int x0, int y0, int idx) {
    for (int row = 0; row < 7; row++) {
        uint8_t bits = font5x7[idx][row];
        for (int col = 0; col < 5; col++)
            if (bits & (0x10 >> col))
                px_set(x0 + col, y0 + row);
    }
}

static void draw_text(int x0, int y0, const char *text, int max_w) {
    int cx = x0, cy = y0;
    int char_w = 6; /* 5px + 1px gap */

    /* Simple word-wrap */
    const char *p = text;
    while (*p) {
        /* Measure next word */
        const char *word_start = p;
        int wlen = 0;
        while (p[wlen] && p[wlen] != ' ') wlen++;

        int word_px = wlen * char_w;

        /* Wrap if this word won't fit on current line */
        if (cx > x0 && (cx - x0) + word_px > max_w) {
            cx = x0;
            cy += 9; /* 7px + 2px line gap */
        }

        /* Render the word */
        for (int i = 0; i < wlen; i++) {
            char c = p[i];
            if (c >= 'a' && c <= 'z') c -= 32; /* uppercase */
            draw_char(cx, cy, font_index(c));
            cx += char_w;
        }

        p += wlen;
        /* Skip spaces */
        if (*p == ' ') {
            cx += char_w;
            p++;
        }
    }
}

/* Double-size text (each font pixel → 2x2 block); no wrap. Used for the BLE passkey. */
static void draw_text_2x(int x0, int y0, const char *text) {
    int cx = x0;
    for (const char *p = text; *p; p++) {
        char c = *p;
        if (c >= 'a' && c <= 'z') c -= 32;
        int idx = font_index(c);
        for (int row = 0; row < 7; row++) {
            uint8_t bits = font5x7[idx][row];
            for (int col = 0; col < 5; col++)
                if (bits & (0x10 >> col)) {
                    int px = cx + col * 2, py = y0 + row * 2;
                    px_set(px, py);     px_set(px + 1, py);
                    px_set(px, py + 1); px_set(px + 1, py + 1);
                }
        }
        cx += 16;   /* 5px*2 + 2px*2 gap */
    }
}

/* ─── Frame composition ─── */

/* ─── RTC clock helpers ─── */

static const char *month_names[] = {
    "JANUARY","FEBRUARY","MARCH","APRIL","MAY","JUNE",
    "JULY","AUGUST","SEPTEMBER","OCTOBER","NOVEMBER","DECEMBER"
};

static void draw_clock_header(void) {
    datetime_t t;
    rtc_get_datetime(&t);

    /* Format: "APRIL 12, 2026  3:47 PM" */
    char buf[48];
    int hr12 = t.hour % 12;
    if (hr12 == 0) hr12 = 12;
    const char *ampm = (t.hour < 12) ? "AM" : "PM";
    snprintf(buf, sizeof(buf), "%s %d, %d  %d:%02d %s",
             month_names[t.month - 1], t.day, t.year, hr12, t.min, ampm);

    /* Center the header (6px per char) */
    int len = (int)strlen(buf);
    int header_w = len * 6;
    int header_x = (IMG_W - header_w) / 2;
    if (header_x < 0) header_x = 0;

    /* draw_text uses raw px_set (no offset) — renders at y=1, top of screen */
    draw_text(header_x, 1, buf, IMG_W);
}

static void setup_body_transform(uint8_t mood, uint32_t f) {
    /* Reset */
    body_dx = 0; body_dy = 0; body_x_expand = 0;
    wobble_amp = 0; wobble_freq = 0; wobble_phase = 0;

    float pi = 3.14159f;
    switch (mood) {
        case MOOD_ANGRY:
            body_dy = -1; body_x_expand = 2;
            wobble_amp = 0.5f; wobble_freq = 0.3f; wobble_phase = f * pi;
            break;
        case MOOD_SAD:
            body_dy = 3; body_x_expand = -1;
            break;
        case MOOD_UNHINGED:
            body_dx = (int)(1.5f * sinf(f * 7.3f));
            body_dy = (int)(1.5f * sinf(f * 5.1f + 1));
            break;
        case MOOD_WEIRD:
            body_dx = (int)(3 * sinf(f * 0.8f));
            wobble_amp = 1.5f; wobble_freq = 0.15f; wobble_phase = (float)f;
            break;
        case MOOD_CHAOTIC:
            body_dx = (int)(2 * sinf(f * 2.1f));
            body_dy = (int)(2 * sinf(f * 1.7f));
            wobble_amp = 3; wobble_freq = 0.25f; wobble_phase = f * 2.0f;
            break;
        case MOOD_HUNGRY:
            body_dy = -2 + (int)sinf(f * 1.5f);
            break;
        case MOOD_TIRED:
            body_dy = 2 + (int)sinf(f * 0.5f); body_x_expand = -1;
            break;
        case MOOD_SLAPHAPPY:
            body_dx = (int)(3 * sinf(f * 1.2f));
            wobble_amp = 2; wobble_freq = 0.1f; wobble_phase = f * 1.2f;
            break;
        case MOOD_LAZY:
            body_dy = 3; body_x_expand = 3;
            break;
        case MOOD_FAT:
            body_x_expand = 3; body_dy = (int)sinf(f * 1.8f);
            break;
        case MOOD_CHILL:
            body_dx = (int)sinf(f * 0.4f); body_dy = 1;
            break;
        case MOOD_CREEPY:
            body_x_expand = (int)(2 * sinf(f * 2.0f));
            break;
        case MOOD_EXCITED:
            body_dy = (int)(3 * sinf(f * 3.0f));
            break;
        case MOOD_NOSTALGIC:
            body_dx = (int)(2 * sinf(f * 0.5f));
            body_dy = (int)sinf(f * 0.3f);
            break;
        case MOOD_HOMESICK:
            body_dy = 1; body_x_expand = -2;
            break;
        default: /* NORMAL: gentle breathing */
            body_dy = (int)sinf(f * 0.8f);
            break;
    }
}

/* Draw just the octopus (body + eyes + pupils + brows + mouth) at the current
 * layout origin. Shared by the wide and tall layouts. */
static void draw_octopus(const Quote *q, int expr, uint32_t frame_idx) {
    /* Set up body animation transform for this frame */
    setup_body_transform(q->mood, frame_idx);

    /* Body (with Y_OFF + body transform) */
    draw_body_transformed();

    /* Eyes (white sockets, with Y_OFF) */
    draw_eyes();

    /* Pupils (mood-specific, with Y_OFF) */
    switch (q->mood) {
        case MOOD_WEIRD:    draw_pupils_weird();    break;
        case MOOD_UNHINGED: draw_pupils_unhinged(); break;
        case MOOD_ANGRY:    draw_pupils_angry();    break;
        case MOOD_SAD:      draw_pupils_sad();      break;
        case MOOD_CHAOTIC:  draw_pupils_chaotic();  break;
        case MOOD_HUNGRY:   draw_pupils_hungry();   break;
        case MOOD_TIRED:    draw_pupils_tired();    break;
        case MOOD_LAZY:     draw_pupils_lazy();     break;
        case MOOD_FAT:      draw_pupils_fat();      break;
        case MOOD_CHILL:    draw_pupils_chill();    break;
        case MOOD_CREEPY:    draw_pupils_creepy();    break;
        case MOOD_EXCITED:  draw_pupils_excited();  break;
        case MOOD_NOSTALGIC: draw_pupils_nostalgic(); break;
        case MOOD_HOMESICK: draw_pupils_homesick(); break;
        default:            draw_pupils_normal();   break;
    }

    /* 3b. Eyebrows / eyelids / special eyes */
    if (q->mood == MOOD_ANGRY)     draw_brows_angry();
    if (q->mood == MOOD_SAD)       draw_brows_sad();
    if (q->mood == MOOD_TIRED)     draw_lids_tired();
    if (q->mood == MOOD_SLAPHAPPY) draw_eyes_slaphappy();
    if (q->mood == MOOD_LAZY)      draw_lids_lazy();
    if (q->mood == MOOD_HOMESICK)  draw_tears_homesick();

    /* 4. Mouth expression (with Y_OFF) */
    switch (expr) {
        case EXPR_OPEN:      draw_mouth_open();      break;
        case EXPR_SMILE:     draw_mouth_smile();     break;
        case EXPR_WEIRD:     draw_mouth_weird();     break;
        case EXPR_UNHINGED:  draw_mouth_unhinged();  break;
        case EXPR_ANGRY:     draw_mouth_angry();     break;
        case EXPR_SAD:       draw_mouth_sad();       break;
        case EXPR_CHAOTIC:   draw_mouth_chaotic();   break;
        case EXPR_HUNGRY:    draw_mouth_hungry();    break;
        case EXPR_TIRED:     draw_mouth_tired();     break;
        case EXPR_SLAPHAPPY: draw_mouth_slaphappy(); break;
        case EXPR_LAZY:      draw_mouth_lazy();      break;
        case EXPR_FAT:       draw_mouth_fat();       break;
        case EXPR_CHILL:     draw_mouth_chill();     break;
        case EXPR_CREEPY:     draw_mouth_creepy();     break;
        case EXPR_EXCITED:   draw_mouth_excited();   break;
        case EXPR_NOSTALGIC: draw_mouth_nostalgic(); break;
        case EXPR_HOMESICK:  draw_mouth_homesick();  break;
        default:             draw_mouth_smirk();     break;
    }

}

/* ── WIDE layout: octopus left, chat bubble + quote right (the classic view) ── */
static void render_frame(const Quote *q, int expr, uint32_t frame_idx) {
    set_canvas_wide();
    memset(frame, 0, sizeof(frame));
    layout_ox = 0; layout_oy = 0;

    draw_clock_header();
    draw_octopus(q, expr, frame_idx);

    /* Chat bubble + quote */
    draw_bubble();
    draw_text(81, 11 + Y_OFF, q->text, 158);

    /* Tagline — current mood/emotion name */
    int tag_y = 5 + 70 + 5 + Y_OFF;
    if (tag_y + 7 < IMG_H) {
        char mood_tag[40];
        snprintf(mood_tag, sizeof(mood_tag), "- %s -",
                 current_mood < 0 ? mood_names[q->mood] : mood_names[current_mood]);
        for (char *p = mood_tag; *p; p++)
            if (*p >= 'a' && *p <= 'z') *p -= 32;
        draw_text(81, tag_y, mood_tag, 170);
    }
}

/* ── TALL "longways" layout (122x250): 2-row status bar / quote / octopus
 * at the bottom. Pixel positions are first-pass and meant for on-device
 * tuning. ── */
/* Reserved bottom band of the tall canvas for the menu/status strip. The
 * interactive menu (STATE_MENU) renders here when in a tall hold. */
#define TALL_STRIP_Y 172

static void render_octopus_tall(const Quote *q, int expr, uint32_t frame_idx) {
    set_canvas_tall();          /* 122 x 250 */
    memset(frame, 0, sizeof(frame));

    /* ── Status bar (y0..24): wifi + bt + battery, then date/time ── */
    draw_wifi_icon(0, 1, wifi_connected);
    {
        int soc_x = 18;
        if (wetgreg_bt_state() == BT_PAIRED) { draw_bt_icon(18, 1); soc_x = 32; }
        if (wetgreg_social_active()) draw_social_icon(soc_x, 1);
    }
    draw_battery_icon(104, 1);
    {
        datetime_t t; rtc_get_datetime(&t);
        int hr12 = t.hour % 12; if (hr12 == 0) hr12 = 12;
        const char *ampm = (t.hour < 12) ? "AM" : "PM";
        char line[40];
        snprintf(line, sizeof(line), "%s %d  %d:%02d%s",
                 month_names[t.month - 1], t.day, hr12, t.min, ampm);
        int x = (122 - (int)strlen(line) * 6) / 2; if (x < 0) x = 0;
        draw_text(x, 14, line, 122);
    }
    for (int x = 4; x < 118; x++) px_set(x, 24);

    /* ── Quote bubble — dropped DOWN so its bottom sits just above the octopus
     *    head (~y125), with a speech-tail caret at the bottom-right so it reads
     *    like a quote. Text is CAPPED so it can't overrun the face. ── */
    {
        const int bx0 = 4, by0 = 48, bx1 = 117, by1 = 120;
        for (int x = bx0 + 2; x <= bx1 - 2; x++) { px_set(x, by0); px_set(x, by1); }
        for (int y = by0 + 2; y <= by1 - 2; y++) { px_set(bx0, y); px_set(bx1, y); }

        /* Speech-tail caret hanging off the bottom-right, pointing at the head. */
        int tipx = bx1 - 14, tipy = by1 + 9;
        for (int i = 0; i <= 12; i++) px_set(bx1 - 26 + i, by1 + (i * 9) / 12); /* left edge */
        for (int i = 0; i <= 9;  i++) px_set(bx1 - 8 - (i * 6) / 9, by1 + i);   /* right edge */
        px_set(tipx, tipy);
        for (int x = bx1 - 25; x < bx1 - 8; x++) px_clr(x, by1);                /* open the mouth */

        char qbuf[96];
        snprintf(qbuf, sizeof(qbuf), "%s", q->text);
        if (strlen(qbuf) > 90) { qbuf[88] = qbuf[89] = qbuf[90] = '.'; qbuf[91] = 0; }
        draw_text(bx0 + 4, by0 + 5, qbuf, (bx1 - bx0) - 8);
    }

    /* ── Octopus, moved down so the gaps above (bubble) and below (status
     *    block) are roughly even. Face lands ~y150. ── */
    const int OCT_OX = (122 - 65) / 2 - 5;
    const int OCT_OY = 113;
    layout_ox = OCT_OX;
    layout_oy = OCT_OY;
    draw_octopus(q, expr, frame_idx);
    layout_ox = 0; layout_oy = 0;

#if FACE_DEBUG
    /* TEMP: solid square at the computed left-eye position. If you SEE it on
     * the octopus face, the position is right and the eyes/mouth draw is the
     * bug; if you don't, the face is being clipped/placed off. */
    for (int yy = -3; yy <= 3; yy++)
        for (int xx = -3; xx <= 3; xx++)
            px_set(22 + OCT_OX + xx, 25 + Y_OFF + OCT_OY + yy);
#endif

    /* ── Bottom status block, pushed to the very bottom. Order (top→bottom):
     *    emotion state (centered), then STEPS (label left / count right). ── */
    {
        const int blk_div = 214, mood_y = 220, step_y = 234;
        for (int x = 4; x < 118; x++) px_set(x, blk_div);

        char mt[40];
        snprintf(mt, sizeof(mt), "- %s -",
                 current_mood < 0 ? mood_names[q->mood] : mood_names[current_mood]);
        for (char *p = mt; *p; p++) if (*p >= 'a' && *p <= 'z') *p -= 32;
        int mx = (122 - (int)strlen(mt) * 6) / 2; if (mx < 0) mx = 0;
        draw_text(mx, mood_y, mt, 122);          /* emotion, centered */

        char cnt[16];
        snprintf(cnt, sizeof(cnt), "%lu", (unsigned long)steps_today);
        draw_text(4, step_y, "STEPS", 122);                      /* label left */
        int cx = 118 - (int)strlen(cnt) * 6; if (cx < 40) cx = 40;
        draw_text(cx, step_y, cnt, 122);                         /* count right */
    }
}

/* On-screen orientation HUD for calibration (compact: accel ×10, 0..3 hold).
 * Drawn last so it sits on top; gated by ORIENT_DEBUG. */
static void draw_orient_hud(void) {
#if ORIENT_DEBUG
    char hud[28];
    snprintf(hud, sizeof(hud), "O%d X%d Y%d Z%d", g_orientation,
             (int)(accel_g(accel_x) * 10), (int)(accel_g(accel_y) * 10),
             (int)(accel_g(accel_z) * 10));
    /* In tall mode the top is the status bar — drop the HUD into the empty
     * band just below it so the date doesn't overlap it. */
    int hy = (canvas_w == 122) ? 40 : 2;
    draw_text(2, hy, hud, canvas_w);
#endif
}

/* ─── Transpose landscape → portrait for e-ink driver ─── */

/* Map the active canvas onto the fixed 122x250 panel at display_rotation.
 *   90 / 270  → WIDE canvas (250x122): legacy 90 = current view
 *   0  / 180  → TALL canvas (122x250): the longways layout
 * The orientation logic keeps content upright by choosing the rotation. */
static void transpose_to_display(void) {
    const int PW = DISP_W;   /* panel width  = 122 */
    const int PH = DISP_H;   /* panel height = 250 */
    uint16_t dst_row_bytes = (PW + 7) / 8;
    memset(ui_buf, 0xFF, sizeof(ui_buf));   /* UI's private buffer (Phase 2) */

    for (int y = 0; y < canvas_h; y++) {
        for (int x = 0; x < canvas_w; x++) {
            int src_byte = y * canvas_row_bytes + x / 8;
            if (!((frame[src_byte] >> (7 - (x & 7))) & 1)) continue;
            int dx, dy;
            switch (display_rotation) {
                case 0:   dx = x;          dy = y;          break;  /* tall */
                case 180: dx = PW - 1 - x; dy = PH - 1 - y; break;  /* tall flipped */
                case 270: dx = PW - 1 - y; dy = x;          break;  /* wide flipped */
                case 90:                                            /* wide (legacy) */
                default:  dx = y;          dy = PH - 1 - x; break;
            }
            if (dx < 0 || dx >= PW || dy < 0 || dy >= PH) continue;
            int dst_byte = dy * dst_row_bytes + dx / 8;
            ui_buf[dst_byte] &= ~(1 << (7 - (dx & 7)));
        }
    }
}

/* ── Display-task hooks (called from rtos_tasks.c) ──────────────────────────
 * These keep ALL e-ink/driver access in main.c; rtos_tasks.c only orchestrates.
 * display_grab_into : UI side — snapshot the just-transposed ui_buf into the
 *                     free Display buffer `idx` (a cheap 4000-byte copy).
 * display_blit      : Display task (core 1) — the actual ~300 ms panel refresh.
 * display_init_panel: Display task prologue — bring the panel up from core 1. */
void display_grab_into(int idx)   { memcpy(display_buf[idx], ui_buf, sizeof(ui_buf)); }
void display_blit(int idx)        { EPD_Partial(display_buf[idx]); }
void display_init_panel(void)     { EPD_Init(); EPD_Clear(); }

/* ─── Simple PRNG (seeded from ADC noise) ─── */

static uint32_t rng_state;

static void rng_seed(void) {
    adc_init();
    adc_gpio_init(26);
    adc_select_input(0);
    uint32_t seed = 0;
    for (int i = 0; i < 32; i++)
        seed = (seed << 1) | (adc_read() & 1);
    seed ^= time_us_32();
    rng_state = seed ? seed : 0xDEADBEEF;
}

static uint32_t rng_next(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

/* ─── Boot splash (STATE_SPLASH) ─────────────────────────────────────────────
 * One-shot power-on animation (Requirements/Feature_SplashBootAnimation):
 *   Phase A 0–2 s    : sassy Greg peeks in (left edge in landscape, bottom in
 *                      tall) and eases to the default position, bubbles rising.
 *   Phase B 2–3.5 s  : "WET GREG" title slides in (from the right in landscape,
 *                      from the top — with an escort bubble — in tall).
 *   Phase C    –10 s : hold — sassy idle sway, bubbles keep drifting.
 * Orientation is re-read every frame, so rotating mid-splash re-lays out. */
#define SPLASH_TOTAL_MS   10000
#define SPLASH_ENTER_MS    2000
#define SPLASH_TITLE_MS    1500   /* title slide duration, starts at SPLASH_ENTER_MS */
#define SPLASH_FRAME_MS     450   /* per-frame input wait — stays above the panel drain */

/* Sassy variant = side-eye pupils (CHILL) + smirk mouth. */
#define SPLASH_MOOD  MOOD_CHILL
#define SPLASH_EXPR  EXPR_SMIRK

/* Rising bubbles in 0..255 normalized coords, so an orientation change mid-
 * splash just rescales them onto the new canvas. */
#define SPLASH_NBUB 8
static struct { uint8_t x, y, r, spd; } splash_bub[SPLASH_NBUB];

static void splash_bubbles_init(void) {
    for (int i = 0; i < SPLASH_NBUB; i++) {
        splash_bub[i].x   = (uint8_t)(rng_next() & 0xFF);
        splash_bub[i].y   = (uint8_t)(rng_next() & 0xFF);
        splash_bub[i].r   = (uint8_t)(2 + rng_next() % 4);     /* 2..5 px */
        splash_bub[i].spd = (uint8_t)(15 + rng_next() % 26);   /* rise px/s */
    }
}

/* Bubble = circle outline with a 1 px glint (raw canvas coords, no layout offset). */
static void draw_splash_bubble(int cx, int cy, int r) {
    for (int dy = -r; dy <= r; dy++)
        for (int dx = -r; dx <= r; dx++) {
            int d2 = dx * dx + dy * dy;
            if (d2 <= r * r && d2 >= (r - 1) * (r - 1))
                px_set(cx + dx, cy + dy);
        }
    px_clr(cx - (r * 7) / 10, cy - (r * 7) / 10);
}

static void draw_splash_bubbles(uint32_t elapsed) {
    for (int i = 0; i < SPLASH_NBUB; i++) {
        int span = canvas_h + 12;                 /* wrap through a 6 px off-screen band */
        int y = (splash_bub[i].y * canvas_h) / 256 - (int)((elapsed * splash_bub[i].spd) / 1000);
        y = ((y % span) + span) % span - 6;
        int x = (splash_bub[i].x * canvas_w) / 256
              + (int)(2.5f * sinf(elapsed * 0.004f + i * 1.7f));
        draw_splash_bubble(x, y, splash_bub[i].r);
    }
}

/* Ease-out quadratic 0→1 over `dur` ms once `elapsed` passes `start`, clamped. */
static float splash_ease(uint32_t elapsed, uint32_t start, uint32_t dur) {
    if (elapsed <= start) return 0.0f;
    float p = (float)(elapsed - start) / (float)dur;
    if (p > 1.0f) p = 1.0f;
    return 1.0f - (1.0f - p) * (1.0f - p);
}

static void render_splash(uint32_t elapsed, uint32_t frame_idx) {
    Quote sq; sq.text = ""; sq.mood = SPLASH_MOOD;
    float in = splash_ease(elapsed, 0, SPLASH_ENTER_MS);
    float ti = splash_ease(elapsed, SPLASH_ENTER_MS, SPLASH_TITLE_MS);
    /* Small vertical bob while sliding — rides on the layout origin so it can't
     * fight the mood body-transform inside draw_octopus(). */
    int bob = (in < 1.0f) ? (int)(2.0f * sinf(elapsed * 0.012f)) : 0;

    if (orientation_is_tall()) {
        set_canvas_tall();                       /* 122 x 250 */
        memset(frame, 0, sizeof(frame));
        draw_splash_bubbles(elapsed);

        /* Greg rises from below the bottom edge to the tall layout's spot. */
        const int OX = (122 - 65) / 2 - 5, OY_END = 113, OY_START = 250;
        layout_ox = OX;
        layout_oy = OY_START - (int)((OY_START - OY_END) * in) + bob;
        draw_octopus(&sq, SPLASH_EXPR, frame_idx);
        layout_ox = 0; layout_oy = 0;

        /* Title drops in from the top, escorted by a bubble. */
        if (elapsed >= SPLASH_ENTER_MS) {
            int wy = -40 + (int)(70.0f * ti);    /* "WET" settles at y=30 */
            draw_text_2x((122 - 46) / 2, wy, "WET");
            draw_text_2x((122 - 62) / 2, wy + 18, "GREG");
            draw_splash_bubble(104, wy + 8, 4);
        }
    } else {
        set_canvas_wide();                       /* 250 x 122 */
        memset(frame, 0, sizeof(frame));
        draw_splash_bubbles(elapsed);

        /* Greg peeks in from the left edge and eases to the speech position. */
        layout_ox = -70 + (int)(70.0f * in);
        layout_oy = bob;
        draw_octopus(&sq, SPLASH_EXPR, frame_idx);
        layout_ox = 0; layout_oy = 0;

        /* Title slides in from the right once Greg is in position. */
        if (elapsed >= SPLASH_ENTER_MS)
            draw_text_2x(250 - (int)((250 - 98) * ti), 40, "WET GREG");
    }
}

/* ─── Expression cycles per mood ─── */

static const uint8_t cycle_normal[]   = {EXPR_SMIRK, EXPR_OPEN, EXPR_SMILE, EXPR_OPEN};
static const uint8_t cycle_weird[]    = {EXPR_WEIRD, EXPR_OPEN, EXPR_WEIRD, EXPR_SMILE};
static const uint8_t cycle_unhinged[] = {EXPR_UNHINGED, EXPR_OPEN, EXPR_UNHINGED, EXPR_OPEN};
static const uint8_t cycle_angry[]     = {EXPR_ANGRY, EXPR_OPEN, EXPR_ANGRY, EXPR_ANGRY};
static const uint8_t cycle_sad[]       = {EXPR_SAD, EXPR_OPEN, EXPR_SAD, EXPR_SMILE};
static const uint8_t cycle_chaotic[]   = {EXPR_CHAOTIC, EXPR_OPEN, EXPR_UNHINGED, EXPR_WEIRD};
static const uint8_t cycle_hungry[]    = {EXPR_HUNGRY, EXPR_OPEN, EXPR_HUNGRY, EXPR_SMILE};
static const uint8_t cycle_tired[]     = {EXPR_TIRED, EXPR_OPEN, EXPR_TIRED, EXPR_TIRED};
static const uint8_t cycle_slaphappy[] = {EXPR_SLAPHAPPY, EXPR_OPEN, EXPR_SLAPHAPPY, EXPR_SMILE};
static const uint8_t cycle_lazy[]      = {EXPR_LAZY, EXPR_LAZY, EXPR_LAZY, EXPR_OPEN};
static const uint8_t cycle_fat[]       = {EXPR_FAT, EXPR_OPEN, EXPR_FAT, EXPR_SMILE};
static const uint8_t cycle_chill[]     = {EXPR_CHILL, EXPR_OPEN, EXPR_CHILL, EXPR_SMILE};
static const uint8_t cycle_creepy[]     = {EXPR_CREEPY, EXPR_OPEN, EXPR_CREEPY, EXPR_SMILE};
static const uint8_t cycle_excited[]   = {EXPR_EXCITED, EXPR_OPEN, EXPR_EXCITED, EXPR_SMILE};
static const uint8_t cycle_nostalgic[] = {EXPR_NOSTALGIC, EXPR_OPEN, EXPR_NOSTALGIC, EXPR_SMILE};
static const uint8_t cycle_homesick[]  = {EXPR_HOMESICK, EXPR_OPEN, EXPR_HOMESICK, EXPR_HOMESICK};

static const uint8_t *mood_cycle(uint8_t mood) {
    switch (mood) {
        case MOOD_WEIRD:     return cycle_weird;
        case MOOD_UNHINGED:  return cycle_unhinged;
        case MOOD_ANGRY:     return cycle_angry;
        case MOOD_SAD:       return cycle_sad;
        case MOOD_CHAOTIC:   return cycle_chaotic;
        case MOOD_HUNGRY:    return cycle_hungry;
        case MOOD_TIRED:     return cycle_tired;
        case MOOD_SLAPHAPPY: return cycle_slaphappy;
        case MOOD_LAZY:      return cycle_lazy;
        case MOOD_FAT:       return cycle_fat;
        case MOOD_CHILL:     return cycle_chill;
        case MOOD_CREEPY:     return cycle_creepy;
        case MOOD_EXCITED:   return cycle_excited;
        case MOOD_NOSTALGIC: return cycle_nostalgic;
        case MOOD_HOMESICK:  return cycle_homesick;
        default:             return cycle_normal;
    }
}

/* ─── Main ─── */

/* ─── Parse compile-time date/time to seed the RTC ─── */

static int parse_month(const char *s) {
    static const char *m[] = {"Jan","Feb","Mar","Apr","May","Jun",
                              "Jul","Aug","Sep","Oct","Nov","Dec"};
    for (int i = 0; i < 12; i++)
        if (s[0] == m[i][0] && s[1] == m[i][1] && s[2] == m[i][2])
            return i + 1;
    return 1;
}

static void init_rtc_from_compile_time(void) {
    /* __DATE__ = "Apr 12 2026", __TIME__ = "19:05:15" */
    const char *d = __DATE__;
    const char *t = __TIME__;

    datetime_t dt = {
        .year  = (int16_t)(atoi(d + 7)),
        .month = (int8_t)parse_month(d),
        .day   = (int8_t)atoi(d + 4),
        .dotw  = 0,  /* RTC doesn't need accurate day-of-week */
        .hour  = (int8_t)atoi(t),
        .min   = (int8_t)atoi(t + 3),
        .sec   = (int8_t)atoi(t + 6),
    };

    rtc_init();
    rtc_set_datetime(&dt);
    sleep_us(64);  /* wait for RTC to latch */
    printf("RTC set to %04d-%02d-%02d %02d:%02d:%02d\n",
           dt.year, dt.month, dt.day, dt.hour, dt.min, dt.sec);
}

/* ─── Battery / power monitoring ─── */
/* On Pico W / Pico 2 W:
 *   - VSYS/3 sits on GPIO 29 / ADC3, but GPIO 29 is shared with the CYW43
 *     SPI bus, so we average many samples to fight the SPI-induced noise.
 *   - USB presence is detected via CYW43_WL_GPIO_VBUS_PIN (CYW43 GPIO 2)
 *     when the wireless stack has been brought up; otherwise we fall back
 *     to a VSYS threshold. */
static bool battery_adc_ready = false;

/* Board-level fixed correction (ADC-ref / divider tolerance). Compile-time.
 * On top of this sits g_vsys_cal — a flash-persisted runtime trim the user
 * calibrates ON-DEVICE from the Device Info screen (UP = "battery is full →
 * treat this reading as 4.20 V"). One-point calibration corrects the common
 * Pico W systematic low VSYS read that pegs a full pack at ~2 bars. */
#ifndef VSYS_CAL
#define VSYS_CAL 1.0f
#endif
static float g_vsys_cal = 1.0f;        /* persisted runtime calibration trim */

/* Battery-path voltage drop (ADDED BACK on battery only). The cell reaches the
 * Pico's VSYS through the board's series elements (D3 Schottky + slide switch),
 * so a full 4.20 V cell reads ~3.67 V at the ADC — about 0.53 V low. The USB
 * path doesn't have this drop (USB rail reads correctly at ~4.57 V), so the
 * offset is applied ONLY to on-battery samples. Measured on hardware: full pack
 * tops out at ~3.67 V device-read → +0.53 V puts it back at 4.20 V = 100%. */
#define VSYS_BATT_OFFSET 0.53f

/* True while USB is connected (updated by hk_sample). Used for the "charging
 * power-diet": with no load-sharing on the board, the running radio/e-ink load
 * can out-draw the ~120 mA charger, so the cell never fills. While on USB we
 * suspend BLE scanning and slow the e-ink so the charge current wins. */
static volatile bool g_on_usb = false;

/* Filtered battery state, owned and updated by the Housekeeping task (hk_sample)
 * and read by the battery icon / info screen — so the render path never does an
 * inline ADC hit, and everything shows ONE smoothed value instead of a fresh,
 * load-perturbed instantaneous read each frame. -1 = running on USB. */
static volatile int   g_batt_pct = -1;
static volatile float g_batt_v   = 0.0f;   /* filtered battery volts (calibrated) */

static void battery_init(void) {
    adc_gpio_init(29);          /* disable digital pulls on ADC3 */
    battery_adc_ready = true;
}

/* Uncalibrated VSYS (battery) voltage — raw divider math + board VSYS_CAL only. */
static float read_vsys_raw_volts(void) {
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
static float read_vsys_volts(void) {
    return read_vsys_raw_volts() * g_vsys_cal;
}

static bool is_usb_powered(void) {
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
static int lipo_percent_hyst(float v) {
    static int shown = -1;
    int raw = lipo_percent(v);
    if (shown < 0 || raw >= 100 || raw <= 0 || raw >= shown + 2 || raw <= shown - 2)
        shown = raw;
    return shown;
}

/* ─── Battery icon (16x10 pixels) ─── */
static void draw_battery_icon(int x0, int y0) {
    int pct = g_batt_pct;   /* filtered value from hk_sample — no inline ADC read */

    /* Battery outline: 14x8 rectangle + 2x4 terminal nub */
    for (int x = x0; x < x0 + 14; x++) { px_set(x, y0); px_set(x, y0 + 7); }
    for (int y = y0; y < y0 + 8; y++) { px_set(x0, y); px_set(x0 + 13, y); }
    /* Terminal nub on right */
    for (int y = y0 + 2; y < y0 + 6; y++) { px_set(x0 + 14, y); px_set(x0 + 15, y); }

    if (pct < 0) {
        /* USB powered — lightning bolt inside battery */
        px_set(x0 + 8, y0 + 1); px_set(x0 + 7, y0 + 2);
        px_set(x0 + 6, y0 + 2); px_set(x0 + 6, y0 + 3);
        px_set(x0 + 5, y0 + 3); px_set(x0 + 4, y0 + 3);
        px_set(x0 + 5, y0 + 3); px_set(x0 + 9, y0 + 3);
        px_set(x0 + 8, y0 + 3); px_set(x0 + 7, y0 + 3);
        px_set(x0 + 8, y0 + 4); px_set(x0 + 9, y0 + 4);
        px_set(x0 + 7, y0 + 5); px_set(x0 + 8, y0 + 5);
        px_set(x0 + 6, y0 + 6); px_set(x0 + 5, y0 + 6);
    } else {
        /* Fill bars based on percentage (4 bars max) */
        int bars = (pct + 12) / 25;  /* 0-4 bars */
        for (int b = 0; b < bars && b < 4; b++) {
            int bx = x0 + 2 + b * 3;
            for (int y = y0 + 2; y < y0 + 6; y++)
                for (int x = bx; x < bx + 2; x++)
                    px_set(x, y);
        }
    }
}

/* ─── WiFi icon (16x12 pixels) ─── */
static void draw_wifi_icon(int x0, int y0, bool connected) {
    for (int i = -6; i <= 6; i++) {
        int ay = y0 + 1;
        if (i >= -5 && i <= 5) ay = y0;
        if (i >= -3 && i <= 3) ay = y0 - 1;
        px_set(x0 + 8 + i, ay);
    }
    for (int i = -4; i <= 4; i++) {
        int ay = y0 + 4;
        if (i >= -3 && i <= 3) ay = y0 + 3;
        if (i >= -1 && i <= 1) ay = y0 + 2;
        px_set(x0 + 8 + i, ay);
    }
    for (int i = -2; i <= 2; i++) {
        int ay = y0 + 6;
        if (i >= -1 && i <= 1) ay = y0 + 5;
        px_set(x0 + 8 + i, ay);
    }
    px_set(x0 + 7, y0 + 8); px_set(x0 + 8, y0 + 8);
    px_set(x0 + 9, y0 + 8); px_set(x0 + 8, y0 + 9);
    if (!connected) {
        for (int i = 0; i < 11; i++) {
            px_set(x0 + 2 + i, y0 + i);
            px_set(x0 + 3 + i, y0 + i);
        }
    }
}

/* Short Bresenham line in canvas space (for the BT glyph). */
static void icon_line(int x0, int y0, int x1, int y1) {
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        px_set(x0, y0);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

/* Bluetooth rune (~7px wide, 9px tall), stem at x0+3. Drawn only when paired. */
static void draw_bt_icon(int x0, int y0) {
    int cx = x0 + 3, w = 3;
    int top = y0, bot = y0 + 8, u = y0 + 2, l = y0 + 6;
    icon_line(cx, top, cx, bot);     /* vertical stem            */
    icon_line(cx, top, cx + w, u);   /* top apex  → upper knee   */
    icon_line(cx + w, u, cx - w, l); /* upper knee → lower-left  */
    icon_line(cx, bot, cx + w, l);   /* bottom apex → lower knee */
    icon_line(cx + w, l, cx - w, u); /* lower knee → upper-left  */
}

/* Social/scanning icon — concentric "broadcast" rings (distinct from the wifi
 * corner-arcs). Shown when proximity scanning is live. ~9px box. */
static void draw_social_icon(int x0, int y0) {
    int cx = x0 + 4, cy = y0 + 4;
    px_set(cx, cy);                                                   /* center dot */
    px_set(cx, cy-2); px_set(cx, cy+2); px_set(cx-2, cy); px_set(cx+2, cy);      /* inner ring */
    px_set(cx-1, cy-1); px_set(cx+1, cy-1); px_set(cx-1, cy+1); px_set(cx+1, cy+1);
    px_set(cx, cy-4); px_set(cx, cy+4); px_set(cx-4, cy); px_set(cx+4, cy);      /* outer ring */
    px_set(cx-3, cy-2); px_set(cx+3, cy-2); px_set(cx-3, cy+2); px_set(cx+3, cy+2);
    px_set(cx-2, cy-3); px_set(cx+2, cy-3); px_set(cx-2, cy+3); px_set(cx+2, cy+3);
}

/* ─── Menu items ─── */
static const char *menu_items[] = {
    "MOOD SELECT",
    "NETWORK",
    "BLUETOOTH",
    "SOUND",
    "MOTION",
    "DEVICE INFO",
    "SET TIME",
    "SOCIAL",
    "DISPLAY",
    "BACK",
};
#define MENU_COUNT 10
#define MENU_IDX_BLUETOOTH 2
#define MENU_IDX_SET_TIME  6
#define MENU_IDX_SOCIAL    7
#define MENU_IDX_DISPLAY   8
#define MENU_IDX_BACK      9

/* ─── Helper: draw inverted text (white on black bar) ─── */
static void draw_inverted_line(int y, const char *text) {
    int hx_end = canvas_w - 6;          /* canvas-aware: 244 wide, 116 tall */
    for (int hy = y - 1; hy < y + 8; hy++)
        for (int hx = 6; hx < hx_end; hx++)
            px_set(hx, hy);
    int cx = 10;
    for (const char *c = text; *c; c++) {
        char up = *c;
        if (up >= 'a' && up <= 'z') up -= 32;
        const char *pos = strchr(font_chars, up);
        if (pos) {
            int idx = (int)(pos - font_chars);
            for (int row = 0; row < 7; row++) {
                uint8_t bits = font5x7[idx][row];
                for (int col = 0; col < 5; col++)
                    if (bits & (0x10 >> col))
                        px_clr(cx + col, y + row);
            }
        }
        cx += 6;
    }
}

/* ─── Draw menu overlay ─── */
static void render_menu(int selected) {
    /* Clear bottom half of screen for menu overlay */
    for (int y = 72; y < IMG_H; y++)
        for (int x = 0; x < IMG_ROW_BYTES; x++)
            frame[y * IMG_ROW_BYTES + x] = 0;

    for (int x = 5; x < 245; x++) px_set(x, 73);
    draw_text(8, 75, "MENU", IMG_W);

    /* 6 items — scroll if needed */
    int m_vis = 5;
    int m_start = 0;
    if (selected > m_vis - 2) m_start = selected - (m_vis - 2);
    if (m_start + m_vis > MENU_COUNT) m_start = MENU_COUNT - m_vis;
    if (m_start < 0) m_start = 0;
    for (int i = 0; i < m_vis && (m_start + i) < MENU_COUNT; i++) {
        int idx = m_start + i;
        int y = 84 + i * 8;
        char line[40];
        if (idx == selected) {
            snprintf(line, sizeof(line), "> %s", menu_items[idx]);
            draw_inverted_line(y, line);
        } else {
            snprintf(line, sizeof(line), "  %s", menu_items[idx]);
            draw_text(10, y, line, IMG_W);
        }
    }
}

/* ─── Menu redrawn + scaled for the TALL (122x250) orientation ───
 * A full vertical list (all items fit, no scrolling needed). */
static void render_menu_tall(int selected) {
    set_canvas_tall();
    memset(frame, 0, sizeof(frame));

    draw_text(44, 8, "MENU", 122);
    for (int x = 4; x < 118; x++) px_set(x, 20);

    for (int i = 0; i < MENU_COUNT; i++) {
        int y = 34 + i * 18;          /* roomy spacing in the tall view */
        char line[40];
        if (i == selected) {
            snprintf(line, sizeof(line), "> %s", menu_items[i]);
            draw_inverted_line(y, line);
        } else {
            snprintf(line, sizeof(line), "  %s", menu_items[i]);
            draw_text(8, y, line, 122);
        }
    }

    for (int x = 4; x < 118; x++) px_set(x, 236);
    draw_text(6, 240, "C:SELECT L:BACK", 122);
}

/* ─── Generic TALL (122x250) renderers, so every menu/list is readable and
 *     navigable when the device is held joystick-at-bottom. Input is already
 *     orientation-agnostic, so screens only need a tall render variant. ─── */
static void render_list_tall(const char *title, const char *const *items,
                             int count, int sel, const char *footer) {
    set_canvas_tall();
    memset(frame, 0, sizeof(frame));
    draw_text(6, 8, title, 122);
    for (int x = 4; x < 118; x++) px_set(x, 20);

    int vis = 9;                          /* ~9 rows fit (250px / 20) */
    int start = 0;
    if (sel > vis - 2) start = sel - (vis - 2);
    if (start + vis > count) start = count - vis;
    if (start < 0) start = 0;
    for (int i = 0; i < vis && (start + i) < count; i++) {
        int idx = start + i, y = 30 + i * 20;
        char line[44];
        if (idx == sel) {
            snprintf(line, sizeof(line), "> %s", items[idx]);
            draw_inverted_line(y, line);
        } else {
            snprintf(line, sizeof(line), "  %s", items[idx]);
            draw_text(8, y, line, 122);
        }
    }
    for (int x = 4; x < 118; x++) px_set(x, 232);
    draw_text(6, 236, footer ? footer : "C:SEL L:BACK", 122);
}

static void render_text_tall(const char *title, const char *const *lines, int n) {
    set_canvas_tall();
    memset(frame, 0, sizeof(frame));
    draw_text(6, 8, title, 122);
    for (int x = 4; x < 118; x++) px_set(x, 20);
    for (int i = 0; i < n; i++) draw_text(6, 30 + i * 16, lines[i], 122);
    draw_text(6, 236, "L:BACK", 122);
}

/* ─── Saved WiFi networks store — data (flash-backed; functions defined later
 * near the WiFi connect code). Last flash sector survives reboot + OTA. ─── */
#define SAVED_MAGIC   0x4D4F5033u    /* 'MOP3' — bumped: added social/identity block */
#define MAX_SAVED     8
#define SOCIAL_MAX    16             /* other WetGregs remembered in the Social log */
#define SAVED_FLASH_OFFSET  (PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE)
typedef struct { char ssid[33]; char pass[64]; } saved_net_t;

/* One remembered peer WetGreg. The name is NOT stored — it is derived
 * deterministically from `id` (every WetGreg computes the same ridiculous name
 * for a given id), so the log only needs the id + when we last greeted + how. */
#define MET_HELLO_SENT  0x01
#define MET_HELLO_RECV  0x02
typedef struct { uint16_t id; uint32_t last_day; uint8_t flags; } wetgreg_met_t;

/* vsys_cal and the social/identity block are appended at the END; the magic bump
 * forces a clean re-seed so we never read stale layout into the new fields. */
typedef struct {
    uint32_t     magic, count;
    saved_net_t  nets[MAX_SAVED];
    float        vsys_cal;
    /* ── WetGreg identity + social ── */
    uint16_t     wetgreg_id;            /* our BLE social id (random, set once) */
    char         wetgreg_name[24];      /* custom name; "" → use the auto name */
    uint8_t      social_on;            /* persisted opt-in for proximity scanning */
    uint32_t     met_count;
    wetgreg_met_t met[SOCIAL_MAX];
    /* ── Display settings (appended AFTER the social block so old flash images
     * load fine — these read as 0xFF and get sanitised to defaults on load,
     * no SAVED_MAGIC bump needed). ── */
    uint8_t      auto_rotate;          /* 1 = accelerometer auto-rotate (default), 0 = manual */
    uint8_t      manual_orient;        /* OR_LAND_R/OR_LAND_L/OR_TALL when auto_rotate==0 */
} saved_store_t;
static saved_store_t g_saved;
static const char *saved_find_pass(const char *ssid);   /* fwd (used in handlers) */

/* ─── Ridiculous WetGreg name generator ───────────────────────────────────────
 * A WetGreg's name is derived purely from its 16-bit id, so EVERY WetGreg computes
 * the same name for a given id without transmitting it. 32×32 = 1024 combos. */
static const char *k_name_adj[32] = {
    "Soggy","Feral","Greasy","Smug","Moist","Cursed","Spicy","Wobbly",
    "Crusty","Unhinged","Sneaky","Thicc","Forbidden","Haunted","Disco","Goth",
    "Sassy","Rancid","Deluxe","Bonkers","Gremlin","Chonky","Slippery","Vile",
    "Majestic","Sweaty","Eldritch","Bootleg","Feral","Yeeted","Mlem","Zoomie",
};
static const char *k_name_noun[32] = {
    "Noodle","Goblin","Trashpanda","Wizard","Gremlin","Nugget","Possum","Crumpet",
    "Walrus","Pickle","Goose","Yeti","Muppet","Cryptid","Dumpling","Snail",
    "Beans","Hamster","Goblin","Toad","Raccoon","Biscuit","Lizard","Moth",
    "Blobfish","Chinchilla","Wombat","Gourd","Sphinx","Frog","Capybara","Slug",
};
/* Fill `buf` with the deterministic auto-name for `id`. */
static void wetgreg_auto_name(uint16_t id, char *buf, size_t n) {
    snprintf(buf, n, "%s %s", k_name_adj[id & 31], k_name_noun[(id >> 5) & 31]);
}
/* OUR display name: the custom one if set, else the auto-name for our id. */
static const char *wetgreg_display_name(void) {
    static char nm[24];
    if (g_saved.wetgreg_name[0]) return g_saved.wetgreg_name;
    wetgreg_auto_name(g_saved.wetgreg_id, nm, sizeof(nm));
    return nm;
}

/* ─── Saved networks screen (connect / forget) ─── */
static int saved_sel = 0;

static void render_saved_nets(void) {
    if (g_saved.count == 0) saved_sel = 0;
    else if (saved_sel >= (int)g_saved.count) saved_sel = g_saved.count - 1;

    if (orientation_is_tall()) {
        static const char *items[MAX_SAVED];
        for (uint32_t i = 0; i < g_saved.count; i++) items[i] = g_saved.nets[i].ssid;
        if (g_saved.count == 0) {
            set_canvas_tall(); memset(frame, 0, sizeof(frame));
            draw_text(6, 8, "SAVED NETWORKS", 122);
            draw_text(8, 60, "NONE SAVED", 122);
            draw_text(6, 236, "L:BACK", 122);
            return;
        }
        render_list_tall("SAVED NETWORKS", items, g_saved.count, saved_sel,
                         "C:JOIN R:FORGET L:BACK");
        return;
    }

    memset(frame, 0, sizeof(frame));
    draw_text(30, 3, "SAVED NETWORKS", IMG_W);
    for (int x = 10; x < 240; x++) px_set(x, 14);
    if (g_saved.count == 0) {
        draw_text(50, 55, "NONE SAVED", IMG_W);
    } else {
        char line[40];
        for (uint32_t i = 0; i < g_saved.count && i < 7; i++) {
            int y = 22 + i * 12;
            if ((int)i == saved_sel) {
                snprintf(line, sizeof(line), "> %s", g_saved.nets[i].ssid);
                draw_inverted_line(y, line);
            } else {
                snprintf(line, sizeof(line), "  %s", g_saved.nets[i].ssid);
                draw_text(10, y, line, IMG_W);
            }
        }
    }
    draw_text(8, 110, "C:JOIN  R:FORGET  LEFT:BACK", IMG_W);
}

/* ─── Bluetooth pairing screen ─── */
static void render_bluetooth(void) {
    bool tall = orientation_is_tall();
    if (tall) set_canvas_tall(); else set_canvas_wide();
    memset(frame, 0, sizeof(frame));
    draw_text(tall ? 6 : 30, tall ? 8 : 3, "BLUETOOTH", canvas_w);
    for (int x = 4; x < canvas_w - 4; x++) px_set(x, tall ? 20 : 14);

    char line[40];

    /* ── Pairing: show the 6-digit passkey big; phone enters it. ── */
    if (wetgreg_bt_state() == BT_PAIRING) {
        int y = tall ? 44 : 26, dy = tall ? 20 : 14;
        draw_text(8, y, "ENTER THIS CODE ON", canvas_w); y += dy;
        draw_text(8, y, "YOUR PHONE:", canvas_w); y += dy + dy;
        snprintf(line, sizeof(line), "%06lu", (unsigned long)wetgreg_bt_passkey());
        /* double-size centred passkey */
        int tw = (int)strlen(line) * 16;
        draw_text_2x((canvas_w - tw) / 2, y, line);
        draw_text(tall ? 6 : 8, tall ? 236 : 110, "LEFT:CANCEL", canvas_w);
        return;
    }

    int y = tall ? 36 : 26, dy = tall ? 18 : 13;
    draw_text(8, y, "NAME: WetGreg Hub", canvas_w); y += dy;

    /* ── Disabled: radio is off to save power. ── */
    if (!bt_enabled) {
        draw_text(8, y, "STATUS: OFF (DISABLED)", canvas_w); y += dy + dy;
        draw_text(8, y, "BLUETOOTH IS OFF TO", canvas_w); y += dy;
        draw_text(8, y, "SAVE POWER.", canvas_w);
        draw_text(tall ? 6 : 8, tall ? 236 : 110, "C:ENABLE & PAIR  LEFT:BACK", canvas_w);
        return;
    }

    const char *st;
    switch (wetgreg_bt_state()) {
        case BT_STARTING:    st = "STARTING..."; break;
        case BT_ADVERTISING: st = "DISCOVERABLE"; break;
        case BT_CONNECTED:   st = "CONNECTED"; break;
        case BT_PAIRED:      st = "PAIRED"; break;
        default:             st = "ON"; break;
    }

    snprintf(line, sizeof(line), "STATUS: %s", st);
    draw_text(8, y, line, canvas_w); y += dy;
    if (wetgreg_bt_peer()[0]) {
        snprintf(line, sizeof(line), "PEER: %s", wetgreg_bt_peer());
        draw_text(8, y, line, canvas_w); y += dy;
    }
    y += dy;
    if (wetgreg_bt_state() == BT_PAIRED) {
        draw_text(8, y, "PAIRED! PHONE CAN NOW", canvas_w); y += dy;
        draw_text(8, y, "READ MOOD & STEPS.", canvas_w);
    } else {
        draw_text(8, y, "FIND \"WetGreg Hub\" ON", canvas_w); y += dy;
        draw_text(8, y, "YOUR PHONE & PAIR.", canvas_w);
    }

    draw_text(tall ? 6 : 8, tall ? 236 : 110, "C:DISABLE  LEFT:BACK", canvas_w);
}

/* ─── Emotes (WetGreg-to-WetGreg expressions) ─────────────────────────────────
 * An emote = a 1-byte code carried in the beacon. Each maps the octopus to a
 * fitting mood/face PLUS an animated overlay glyph, so the octopus stays on
 * screen and "acts out" the emote. Code 0 = none. */
#define EMOTE_NONE   0
#define EMOTE_WAVE   1
#define EMOTE_LOVE   2
#define EMOTE_LAUGH  3
#define EMOTE_PARTY  4
#define EMOTE_SLEEPY 5
#define EMOTE_WHOA   6
#define EMOTE_COUNT  7
typedef struct { const char *name; uint8_t mood; uint8_t expr; } emote_def_t;
static const emote_def_t emote_defs[EMOTE_COUNT] = {
    /* NONE  */ { "-",      MOOD_NORMAL,    EXPR_SMIRK     },
    /* WAVE  */ { "WAVE",   MOOD_CHILL,     EXPR_SMILE     },
    /* LOVE  */ { "LOVE",   MOOD_EXCITED,   EXPR_EXCITED   },
    /* LAUGH */ { "LAUGH",  MOOD_SLAPHAPPY, EXPR_SLAPHAPPY },
    /* PARTY */ { "PARTY",  MOOD_EXCITED,   EXPR_EXCITED   },
    /* SLEEPY*/ { "SLEEPY", MOOD_TIRED,     EXPR_TIRED     },
    /* WHOA  */ { "WHOA",   MOOD_EXCITED,   EXPR_OPEN      },
};

/* ── small procedural glyphs (wide-canvas coords) ── */
static void draw_heart(int cx, int cy) {
    px_set(cx-2,cy); px_set(cx-1,cy); px_set(cx+1,cy); px_set(cx+2,cy);
    for (int dx=-3; dx<=3; dx++) { px_set(cx+dx,cy+1); px_set(cx+dx,cy+2); }
    for (int dx=-2; dx<=2; dx++) px_set(cx+dx,cy+3);
    px_set(cx-1,cy+4); px_set(cx,cy+4); px_set(cx+1,cy+4);
    px_set(cx,cy+5);
}
static void draw_note(int x, int y) {
    for (int a=0;a<3;a++) for (int b=0;b<3;b++) px_set(x+a, y+5+b);   /* head */
    for (int b=0;b<8;b++) px_set(x+3, y+b);                          /* stem */
    px_set(x+4,y); px_set(x+5,y+1);                                  /* flag */
}
/* A waving hand that tilts with `t` (the wave motion). */
static void draw_wave_hand(int x, int y, int t) {
    int s = (t & 1) ? 1 : -1;
    for (int a=0;a<6;a++) for (int b=0;b<5;b++) px_set(x+a+(b<2?s:0), y+b+3);  /* palm */
    for (int f=0; f<4; f++) { int fx = x + f*1 + 1; px_set(fx+s, y+1); px_set(fx+s, y+2); }
}

/* Render the octopus acting out `emote`, with `caption` to the right. `tick`
 * drives the overlay animation. Keeps the octopus on screen at all times. */
/* Draw the animated emote glyph relative to a base origin (bx,by) so the same
 * code serves both orientations — only the base differs. */
static void draw_emote_overlay(uint8_t emote, int bx, int by, uint32_t tick) {
    switch (emote) {
        case EMOTE_WAVE:  draw_wave_hand(bx + 2, by + 4, (int)tick); break;
        case EMOTE_LOVE:
            for (int i = 0; i < 3; i++) draw_heart(bx + 6 + i*13, by + 32 - (int)((tick*3 + i*7) % 32));
            break;
        case EMOTE_LAUGH: draw_text(bx, by + 2, (tick & 1) ? "HA HA" : " HAHA", IMG_W); break;
        case EMOTE_PARTY:
            draw_note(bx + 2 + ((tick&1)?0:3), by + 2);
            draw_note(bx + 24, by + 10 + ((tick&1)?2:0));
            draw_note(bx + 44, by + ((tick&1)?0:3));
            break;
        case EMOTE_SLEEPY: {
            int n = (int)(tick % 3) + 1;
            for (int i = 0; i < n; i++) draw_text(bx + 6 + i*12, by + 18 - i*9, "Z", IMG_W);
            break;
        }
        case EMOTE_WHOA:  draw_text(bx + 2, by + 2, (tick & 1) ? "! !" : "!!!", IMG_W); break;
        default: break;
    }
}

/* Octopus acting out an emote — works in BOTH orientations (wide: octopus left,
 * caption right; tall: caption top, octopus at the bottom like the main screen). */
static void render_emote_octopus(uint8_t emote, const char *caption, uint32_t tick) {
    if (emote >= EMOTE_COUNT) emote = EMOTE_WAVE;
    const emote_def_t *e = &emote_defs[emote];
    Quote tq; tq.text = ""; tq.mood = e->mood;
    char nm[20]; snprintf(nm, sizeof(nm), "* %s *", e->name);

    if (orientation_is_tall()) {
        set_canvas_tall();
        memset(frame, 0, sizeof(frame));
        draw_text(6, 12, caption, 122);
        draw_text(6, 30, nm, 122);
        for (int x = 4; x < 118; x++) px_set(x, 44);
        layout_ox = (122 - 65) / 2 - 5; layout_oy = 113;   /* same spot as the main tall octopus */
        draw_octopus(&tq, e->expr, tick);
        draw_emote_overlay(emote, 36, 70, tick);            /* just above the head */
    } else {
        set_canvas_wide();
        memset(frame, 0, sizeof(frame));
        layout_ox = 0; layout_oy = 0;
        draw_clock_header();
        draw_octopus(&tq, e->expr, tick);
        draw_emote_overlay(emote, 58, 12, tick);
        draw_text(140, 34, caption, 108);
        draw_text(140, 52, nm, 108);
    }
}

/* ─── Social screens (WetGreg-to-WetGreg) ─── */
#define SOCIAL_MENU_COUNT 5
#define SOC_ITEM_SCAN   0
#define SOC_ITEM_NEARBY 1
#define SOC_ITEM_MET    2
#define SOC_ITEM_NAME   3
#define SOC_ITEM_BACK   4
static void render_social_menu(int sel) {
    bool tall = orientation_is_tall();
    if (tall) set_canvas_tall(); else set_canvas_wide();
    memset(frame, 0, sizeof(frame));
    draw_text(tall ? 8 : 40, tall ? 10 : 3, "SOCIAL", canvas_w);
    for (int x = 4; x < canvas_w - 4; x++) px_set(x, tall ? 22 : 14);

    char line[40];
    int y = tall ? 28 : 17, dy = tall ? 17 : 12;
    snprintf(line, sizeof(line), "ME: %s", wetgreg_display_name());
    draw_text(8, y, line, canvas_w); y += dy;
    snprintf(line, sizeof(line), "ID #%04X  MET %lu",
             g_saved.wetgreg_id, (unsigned long)g_saved.met_count);
    draw_text(8, y, line, canvas_w); y += dy + 2;

    const char *items[SOCIAL_MENU_COUNT];
    static char it0[24];
    snprintf(it0, sizeof(it0), "SCAN: %s", g_saved.social_on ? "ON" : "OFF");
    items[SOC_ITEM_SCAN]   = it0;
    items[SOC_ITEM_NEARBY] = "SCAN NEARBY";
    items[SOC_ITEM_MET]    = "WETGREGS MET";
    items[SOC_ITEM_NAME]   = "SET NAME";
    items[SOC_ITEM_BACK]   = "BACK";
    for (int i = 0; i < SOCIAL_MENU_COUNT; i++) {
        int yy = y + i * dy; char l[28];
        if (i == sel) { snprintf(l, sizeof(l), "> %s", items[i]); draw_inverted_line(yy, l); }
        else          { snprintf(l, sizeof(l), "  %s", items[i]); draw_text(8, yy, l, canvas_w); }
    }
    draw_text(4, tall ? 232 : 108, "U/D  C:SEL  L:BACK", canvas_w);
}

/* Scrollable list helper shared by "WETGREGS MET" and "SCAN NEARBY". `ids`/`rssi`
 * hold `count` entries; rssi==NULL hides the dBm column (met list). */
static void render_wetgreg_list(const char *title, const uint16_t *ids, const int8_t *rssi,
                               const uint8_t *flags, int count, int sel, const char *empty,
                               const char *foot) {
    bool tall = orientation_is_tall();
    if (tall) set_canvas_tall(); else set_canvas_wide();
    memset(frame, 0, sizeof(frame));
    draw_text(tall ? 8 : 24, tall ? 10 : 3, title, canvas_w);
    for (int x = 4; x < canvas_w - 4; x++) px_set(x, tall ? 22 : 14);
    if (count <= 0) {
        draw_text(8, tall ? 60 : 40, empty, canvas_w);
        draw_text(4, tall ? 232 : 108, "L:BACK", canvas_w);
        return;
    }
    int rows = tall ? 9 : 6, top = (sel >= rows) ? sel - rows + 1 : 0;
    int y0 = tall ? 30 : 20, dy = tall ? 20 : 13;
    for (int i = 0; i < rows && (top + i) < count; i++) {
        int idx = top + i;
        char nm[24]; wetgreg_auto_name(ids[idx], nm, sizeof(nm));
        char line[40];
        if (rssi) {
            snprintf(line, sizeof(line), "%s %ddBm", nm, (int)rssi[idx]);
        } else {
            char mk[4]; int k = 0;
            if (flags && (flags[idx] & MET_HELLO_SENT)) mk[k++] = '>';
            if (flags && (flags[idx] & MET_HELLO_RECV)) mk[k++] = '<';
            mk[k] = '\0';
            snprintf(line, sizeof(line), "%s %s", nm, mk);
        }
        int y = y0 + i * dy;
        if (idx == sel) { char l[44]; snprintf(l, sizeof(l), "> %s", line); draw_inverted_line(y, l); }
        else            { draw_text(8, y, line, canvas_w); }
    }
    draw_text(4, tall ? 232 : 108, foot, canvas_w);
}

/* "say hi?" / "say hi back?" prompts share a layout; `incoming` flips the wording. */
static void render_social_card(bool incoming) {
    bool tall = orientation_is_tall();
    if (tall) set_canvas_tall(); else set_canvas_wide();
    memset(frame, 0, sizeof(frame));
    char nm[24]; wetgreg_auto_name(g_social_peer, nm, sizeof(nm));
    char line[40];
    int y = tall ? 24 : 6, dy = tall ? 18 : 12;
    if (incoming) {
        draw_text(8, y, "ANOTHER WETGREG", canvas_w); y += dy;
        draw_text(8, y, "SAYS HELLO!", canvas_w); y += dy + 3;
    } else {
        draw_text(8, y, "A WETGREG APPEARS", canvas_w); y += dy;
        draw_text(8, y, "IN THE WILD!", canvas_w); y += dy + 3;
    }
    draw_text(8, y, nm, canvas_w); y += dy;
    snprintf(line, sizeof(line), "#%04X  %d dBm", g_social_peer, (int)g_social_peer_rssi);
    draw_text(8, y, line, canvas_w); y += dy + 3;
    draw_text(8, y, incoming ? "RESPOND?" : "SAY HI?", canvas_w);
    draw_text(4, tall ? 232 : 108, incoming ? "C:EMOTE  L:NO" : "C:YES  L:NO", canvas_w);
}

static void render_social_name(void) {
    bool tall = orientation_is_tall();
    if (tall) set_canvas_tall(); else set_canvas_wide();
    memset(frame, 0, sizeof(frame));
    char nm[24]; wetgreg_auto_name(g_name_seed, nm, sizeof(nm));
    int y = tall ? 24 : 8, dy = tall ? 22 : 18;
    draw_text(8, y, "PICK A NAME:", canvas_w); y += dy + 4;
    draw_text(8, y, nm, canvas_w);
    draw_text(4, tall ? 232 : 108, "U/D:REROLL C:SAVE L:NO", canvas_w);
}

#define EMOTE_PICK_COUNT (EMOTE_COUNT - 1)   /* skip EMOTE_NONE */
static void render_emote_pick(int sel) {
    bool tall = orientation_is_tall();
    if (tall) set_canvas_tall(); else set_canvas_wide();
    memset(frame, 0, sizeof(frame));
    draw_text(tall ? 8 : 24, tall ? 10 : 3, "SEND EMOTE", canvas_w);
    for (int x = 4; x < canvas_w - 4; x++) px_set(x, tall ? 22 : 14);
    char nm[24]; wetgreg_auto_name(g_social_peer, nm, sizeof(nm));
    char line[40]; snprintf(line, sizeof(line), "TO: %s", nm);
    int y = tall ? 28 : 18, dy = tall ? 17 : 13;
    draw_text(8, y, line, canvas_w); y += dy + 2;
    for (int i = 0; i < EMOTE_PICK_COUNT; i++) {
        int yy = y + i * dy; char l[28];
        const char *n = emote_defs[i + 1].name;
        if (i == sel) { snprintf(l, sizeof(l), "> %s", n); draw_inverted_line(yy, l); }
        else          { snprintf(l, sizeof(l), "  %s", n); draw_text(8, yy, l, canvas_w); }
    }
    draw_text(4, tall ? 232 : 108, "U/D C:SEND L:BACK", canvas_w);
}

/* ─── Display settings (auto-rotate + orientation) ─── */
static const char *orient_names[3] = {
    /* OR_LAND_R */ "WIDE",
    /* OR_LAND_L */ "WIDE FLIP",
    /* OR_TALL   */ "TALL",
};
#define DISP_MENU_COUNT 3
#define DISP_ITEM_AUTO   0
#define DISP_ITEM_ORIENT 1
#define DISP_ITEM_BACK   2
static void render_display_menu(int sel) {
    bool tall = orientation_is_tall();
    if (tall) set_canvas_tall(); else set_canvas_wide();
    memset(frame, 0, sizeof(frame));
    draw_text(tall ? 8 : 36, tall ? 10 : 3, "DISPLAY", canvas_w);
    for (int x = 4; x < canvas_w - 4; x++) px_set(x, tall ? 22 : 14);

    const char *items[DISP_MENU_COUNT];
    static char it_auto[24], it_orient[28];
    snprintf(it_auto, sizeof(it_auto), "AUTO-ROTATE: %s", g_auto_rotate ? "ON" : "OFF");
    /* Show the orientation that's actually active (auto follows the accel;
     * manual shows the locked choice). */
    int show_o = g_auto_rotate ? g_orientation : (int)g_manual_orient;
    if (show_o < 0 || show_o > 2) show_o = OR_TALL;
    snprintf(it_orient, sizeof(it_orient), "ORIENT: %s", orient_names[show_o]);
    items[DISP_ITEM_AUTO]   = it_auto;
    items[DISP_ITEM_ORIENT] = it_orient;
    items[DISP_ITEM_BACK]   = "BACK";

    int y = tall ? 34 : 24, dy = tall ? 22 : 14;
    for (int i = 0; i < DISP_MENU_COUNT; i++) {
        int yy = y + i * dy; char l[36];
        if (i == sel) { snprintf(l, sizeof(l), "> %s", items[i]); draw_inverted_line(yy, l); }
        else          { snprintf(l, sizeof(l), "  %s", items[i]); draw_text(8, yy, l, canvas_w); }
    }
    /* Hint: ORIENT cycles WIDE → WIDE FLIP → TALL (and locks auto-rotate off). */
    draw_text(8, tall ? 232 : 108,
              sel == DISP_ITEM_ORIENT ? "C:CYCLE  L:BACK" : "C:TOGGLE  L:BACK", canvas_w);
}

/* ─── Manual date/time setter ─── */
static datetime_t settime_dt;
static int  settime_field   = 0;     /* 0=year 1=month 2=day 3=hour 4=min */
static bool settime_editing = false; /* false = pick a field, true = adjust its value */

static void render_set_time(void) {
    bool tall = orientation_is_tall();
    if (tall) set_canvas_tall(); else set_canvas_wide();
    memset(frame, 0, sizeof(frame));

    draw_text(tall ? 8 : 30, tall ? 10 : 3, "SET DATE / TIME", canvas_w);
    for (int x = 4; x < canvas_w - 4; x++) px_set(x, tall ? 22 : 14);

    char f[5][14];
    snprintf(f[0], 14, "YEAR  %d", settime_dt.year);
    snprintf(f[1], 14, "MONTH %02d", settime_dt.month);
    snprintf(f[2], 14, "DAY   %02d", settime_dt.day);
    snprintf(f[3], 14, "HOUR  %02d", settime_dt.hour);
    snprintf(f[4], 14, "MIN   %02d", settime_dt.min);

    int y0 = tall ? 44 : 26, dy = tall ? 22 : 13;
    for (int i = 0; i < 5; i++) {
        int y = y0 + i * dy;
        bool selected = (i == settime_field);
        /* Selected field is a steady highlight either way. EDIT mode adds a second
         * '>' arrow so it's clear U/D now adjusts the value (vs. just picking it). */
        const char *prefix = selected ? (settime_editing ? ">>" : "> ") : "  ";
        char line[20];
        snprintf(line, sizeof(line), "%s%s", prefix, f[i]);
        if (selected) draw_inverted_line(y, line);
        else          draw_text(8, y, line, canvas_w);
    }
    draw_text(4, tall ? 232 : 108,
              settime_editing ? "U/D ADJUST  C OK  L BACK"
                              : "U/D FIELD  C EDIT  L BACK", canvas_w);
}

/* ─── Draw sound submenu ─── */
#define SND_ITEM_PATTERN 0
#define SND_ITEM_ONOFF   1
#define SND_ITEM_VOL     2
#define SND_ITEM_BACK    3
#define SND_MENU_COUNT   4

static const char *vol_labels[] = {"LOW", "MED", "HIGH"};

static void render_sound_menu(int sel) {
    memset(frame, 0, sizeof(frame));
    draw_text(30, 3, "SOUND", IMG_W);
    for (int x = 10; x < 240; x++) px_set(x, 14);

    char buf[40];
    const char *items[SND_MENU_COUNT];
    static char pattern_buf[30];
    snprintf(pattern_buf, sizeof(pattern_buf), "TONE: %s", pattern_names[current_pattern]);
    items[SND_ITEM_PATTERN] = pattern_buf;
    static char onoff_buf[20];
    snprintf(onoff_buf, sizeof(onoff_buf), "SOUND: %s", sound_enabled ? "ON" : "OFF");
    items[SND_ITEM_ONOFF] = onoff_buf;
    static char vol_buf[20];
    snprintf(vol_buf, sizeof(vol_buf), "VOLUME: %s", vol_labels[sound_vol]);
    items[SND_ITEM_VOL] = vol_buf;
    items[SND_ITEM_BACK] = "BACK";

    if (orientation_is_tall()) {
        render_list_tall("SOUND", items, SND_MENU_COUNT, sel, "C:PLAY L:BACK");
        return;
    }

    for (int i = 0; i < SND_MENU_COUNT; i++) {
        int y = 22 + i * 12;
        if (i == sel) {
            snprintf(buf, sizeof(buf), "> %s", items[i]);
            draw_inverted_line(y, buf);
        } else {
            snprintf(buf, sizeof(buf), "  %s", items[i]);
            draw_text(10, y, buf, IMG_W);
        }
    }

    draw_text(10, 96, "C:PLAY  L/R:CHANGE TONE", IMG_W);
    draw_text(155, 108, "LEFT:BACK", IMG_W);
}

/* ─── Draw device info screen ─── */
static void render_info_screen(void) {
    int   pct  = g_batt_pct;   /* filtered (hk_sample) — consistent with the icon */
    float vsys = g_batt_v;

    if (orientation_is_tall()) {
        static char L[9][40]; static const char *lp[9]; int n = 0;
        datetime_t t; rtc_get_datetime(&t);
        int h = t.hour % 12; if (h == 0) h = 12;
        snprintf(L[n], 40, "FW V%s", WETGREG_VERSION); lp[n] = L[n]; n++;
        snprintf(L[n], 40, "DISP %s", DISPLAY_NAME); lp[n] = L[n]; n++;
        snprintf(L[n], 40, "%s %d %d:%02d%s", month_names[t.month - 1], t.day,
                 h, t.min, t.hour < 12 ? "A" : "P"); lp[n] = L[n]; n++;
        snprintf(L[n], 40, "MOOD %s", current_mood < 0 ? "ALL" : mood_names[current_mood]); lp[n] = L[n]; n++;
        snprintf(L[n], 40, "WIFI %s", wifi_connected ? wifi_ip_str : "OFF"); lp[n] = L[n]; n++;
        if (pct < 0) snprintf(L[n], 40, "USB %.2fV", (double)vsys);
        else         snprintf(L[n], 40, "BATT %d%% %.2fV", pct, (double)vsys);
        lp[n] = L[n]; n++;
        snprintf(L[n], 40, "CAL x%.3f", (double)g_vsys_cal); lp[n] = L[n]; n++;
        snprintf(L[n], 40, "UP:CAL FULL=4.2"); lp[n] = L[n]; n++;
        render_text_tall("DEVICE INFO", lp, n);
        return;
    }
    memset(frame, 0, sizeof(frame));
    draw_text(30, 3, "DEVICE INFO", IMG_W);
    for (int x = 10; x < 240; x++) px_set(x, 14);
    char buf[48];
    int y = 20;

    snprintf(buf, sizeof(buf), "FW: V%s  %s", WETGREG_VERSION, DISPLAY_NAME);
    draw_text(10, y, buf, IMG_W); y += 11;

    snprintf(buf, sizeof(buf), "BUILT: %s %s", __DATE__, __TIME__);
    draw_text(10, y, buf, IMG_W); y += 11;

    datetime_t t; rtc_get_datetime(&t);
    int hr12 = t.hour % 12; if (hr12 == 0) hr12 = 12;
    snprintf(buf, sizeof(buf), "%s %d, %d  %d:%02d %s",
             month_names[t.month - 1], t.day, t.year, hr12, t.min,
             t.hour < 12 ? "AM" : "PM");
    draw_text(10, y, buf, IMG_W); y += 11;

    snprintf(buf, sizeof(buf), "MOOD: %s  QUOTES: %d",
             current_mood < 0 ? "ALL" : mood_names[current_mood], QUOTE_COUNT);
    draw_text(10, y, buf, IMG_W); y += 11;

    snprintf(buf, sizeof(buf), "WIFI: %s", wifi_connected ? wifi_ip_str : "OFF");
    draw_text(10, y, buf, IMG_W); y += 11;

    /* Battery / power status — show the live voltage to 2 decimals + cal trim. */
    if (pct < 0) {
        snprintf(buf, sizeof(buf), "POWER: USB  %.2fV  (CAL x%.3f)",
                 (double)vsys, (double)g_vsys_cal);
    } else {
        snprintf(buf, sizeof(buf), "BATTERY: %d%%  %.2fV  (CAL x%.3f)",
                 pct, (double)vsys, (double)g_vsys_cal);
    }
    draw_text(10, y, buf, IMG_W); y += 11;

    draw_text(10, 100, "UP:CAL FULL=4.2V  DN:RESET CAL", IMG_W);
    draw_text(10, 112, "PICO 2 W  RP2350        LEFT:BACK", IMG_W);
}

/* ─── Draw mood select screen ─── */
static void render_mood_select(int selected) {
    if (orientation_is_tall()) {
        static const char *items[MOOD_COUNT + 1];
        items[0] = "ALL MOODS";
        for (int i = 0; i < MOOD_COUNT; i++) items[i + 1] = mood_names[i];
        render_list_tall("SELECT MOOD", items, MOOD_COUNT + 1, selected, "C:SET L:BACK");
        return;
    }
    memset(frame, 0, sizeof(frame));
    draw_text(30, 3, "SELECT MOOD", IMG_W);
    for (int x = 10; x < 240; x++) px_set(x, 14);

    /* Show 8 moods at a time (scrolling window) */
    int start = 0;
    /* +1 for "ALL (RANDOM)" option */
    int total = MOOD_COUNT + 1;
    if (selected > 6) start = selected - 6;
    if (start + 8 > total) start = total - 8;
    if (start < 0) start = 0;

    for (int i = 0; i < 8 && (start + i) < total; i++) {
        int idx = start + i;
        int y = 20 + i * 11;
        char line[40];
        if (idx == 0) {
            snprintf(line, sizeof(line), idx == selected ? "> ALL MOODS (RANDOM)" : "  ALL MOODS (RANDOM)");
        } else {
            int mood_idx = idx - 1;
            snprintf(line, sizeof(line), idx == selected ? "> %s" : "  %s", mood_names[mood_idx]);
        }
        if (idx == selected)
            draw_inverted_line(y, line);
        else
            draw_text(10, y, line, IMG_W);
    }

    char hint[40];
    snprintf(hint, sizeof(hint), "CURRENT: %s",
             current_mood < 0 ? "ALL" : mood_names[current_mood]);
    draw_text(10, 110, hint, IMG_W);
    draw_text(175, 110, "LEFT:BACK", IMG_W);
}

/* ─── Draw network status screen (read-only) ─── */
static void render_network_screen(void) {
    if (orientation_is_tall()) {
        static char L[6][40]; static const char *lp[6]; int n = 0;
        snprintf(L[n], 40, "WIFI: %s", wifi_enabled ? "ON" : "OFF"); lp[n] = L[n]; n++;
        snprintf(L[n], 40, "SSID: %s", wifi_ssid_display); lp[n] = L[n]; n++;
        snprintf(L[n], 40, "STATE: %s", !wifi_enabled ? "OFF" :
                 wifi_connected ? "CONNECTED" : "DISCONN"); lp[n] = L[n]; n++;
        snprintf(L[n], 40, "IP: %s", wifi_connected ? wifi_ip_str : "---"); lp[n] = L[n]; n++;
        snprintf(L[n], 40, "SIGNAL: %d dBm", wifi_connected ? (int)wifi_rssi : 0); lp[n] = L[n]; n++;
        snprintf(L[n], 40, "NTP: %s", ntp_synced ? "SYNCED" : "NOT SYNCED"); lp[n] = L[n]; n++;
        render_text_tall("WIFI STATUS", lp, n);
        return;
    }
    memset(frame, 0, sizeof(frame));
    draw_text(30, 3, "WIFI STATUS", IMG_W);
    for (int x = 10; x < 240; x++) px_set(x, 14);
    char buf[48];

    snprintf(buf, sizeof(buf), "WIFI: %s", wifi_enabled ? "ON" : "OFF");
    draw_text(10, 22, buf, IMG_W);

    snprintf(buf, sizeof(buf), "SSID: %s", wifi_ssid_display);
    draw_text(10, 35, buf, IMG_W);

    snprintf(buf, sizeof(buf), "STATUS: %s",
             !wifi_enabled ? "DISABLED" :
             wifi_connected ? "CONNECTED" : "DISCONNECTED");
    draw_text(10, 48, buf, IMG_W);

    snprintf(buf, sizeof(buf), "IP: %s", wifi_connected ? wifi_ip_str : "---");
    draw_text(10, 61, buf, IMG_W);

    if (wifi_connected) {
        snprintf(buf, sizeof(buf), "SIGNAL: %d DBM", (int)wifi_rssi);
        draw_text(10, 74, buf, IMG_W);
    }

    snprintf(buf, sizeof(buf), "NTP: %s", ntp_synced ? "SYNCED" : "NOT SYNCED");
    draw_text(10, 87, buf, IMG_W);

    draw_text(175, 110, "LEFT:BACK", IMG_W);
}

/* ─── Draw network submenu ─── */
#define NET_ITEM_ONOFF      0
#define NET_ITEM_SCAN       1
#define NET_ITEM_SAVED      2
#define NET_ITEM_STATUS     3
#define NET_ITEM_BACK       4
#define NET_MENU_COUNT      5

static void render_net_menu(int sel) {
    memset(frame, 0, sizeof(frame));
    draw_text(30, 3, "NETWORK", IMG_W);
    for (int x = 10; x < 240; x++) px_set(x, 14);

    char buf[40];
    const char *items[NET_MENU_COUNT];
    static char onoff_buf[20];
    snprintf(onoff_buf, sizeof(onoff_buf), "WIFI: %s", wifi_enabled ? "ON" : "OFF");
    items[NET_ITEM_ONOFF] = onoff_buf;
    items[NET_ITEM_SCAN] = "SCAN NETWORKS";
    items[NET_ITEM_SAVED] = "SAVED NETWORKS";
    items[NET_ITEM_STATUS] = "STATUS";
    items[NET_ITEM_BACK] = "BACK";

    if (orientation_is_tall()) {
        render_list_tall("NETWORK", items, NET_MENU_COUNT, sel, "C:SEL L:BACK");
        return;
    }

    for (int i = 0; i < NET_MENU_COUNT; i++) {
        int y = 22 + i * 12;
        if (i == sel) {
            snprintf(buf, sizeof(buf), "> %s", items[i]);
            draw_inverted_line(y, buf);
        } else {
            snprintf(buf, sizeof(buf), "  %s", items[i]);
            draw_text(10, y, buf, IMG_W);
        }
    }

    snprintf(buf, sizeof(buf), "%s", wifi_connected ? "CONNECTED" : "DISCONNECTED");
    draw_text(10, 100, buf, IMG_W);
    draw_text(175, 110, "LEFT:BACK", IMG_W);
}

/* ─── Draw scan results ─── */
static void render_scan_results(void) {
    if (orientation_is_tall()) {
        if (scan_in_progress || scan_count == 0) {
            set_canvas_tall(); memset(frame, 0, sizeof(frame));
            draw_text(6, 8, "WIFI NETWORKS", 122);
            for (int x = 4; x < 118; x++) px_set(x, 20);
            draw_text(8, 60, scan_in_progress ? "SCANNING..." : "NO NETWORKS", 122);
            draw_text(6, 236, scan_in_progress ? "L:CANCEL" : "L:BACK", 122);
            return;
        }
        static char rows[MAX_SCAN_RESULTS][26];
        static const char *items[MAX_SCAN_RESULTS];
        for (int i = 0; i < scan_count; i++) {
            char lock = (scan_results[i].auth_mode != 0) ? '~' : ' ';
            snprintf(rows[i], sizeof(rows[i]), "%c%-11.11s%ddB",
                     lock, scan_results[i].ssid, (int)scan_results[i].rssi);
            items[i] = rows[i];
        }
        render_list_tall("WIFI NETWORKS", items, scan_count, scan_sel, "C:JOIN L:BACK");
        return;
    }
    memset(frame, 0, sizeof(frame));
    draw_text(30, 3, "WIFI NETWORKS", IMG_W);
    for (int x = 10; x < 240; x++) px_set(x, 14);

    if (scan_in_progress) {
        draw_text(60, 50, "SCANNING...", IMG_W);
        char buf[20];
        snprintf(buf, sizeof(buf), "FOUND: %d", scan_count);
        draw_text(80, 65, buf, IMG_W);
        draw_text(175, 110, "LEFT:CANCEL", IMG_W);
        return;
    }

    if (scan_count == 0) {
        draw_text(50, 55, "NO NETWORKS FOUND", IMG_W);
        draw_text(175, 110, "LEFT:BACK", IMG_W);
        return;
    }

    /* Scrolling list — 7 items visible */
    int start = 0;
    if (scan_sel > 5) start = scan_sel - 5;
    if (start + 7 > scan_count) start = scan_count - 7;
    if (start < 0) start = 0;

    char buf[42];
    for (int i = 0; i < 7 && (start + i) < scan_count; i++) {
        int idx = start + i;
        int y = 20 + i * 12;
        char lock = (scan_results[idx].auth_mode != 0) ? '~' : ' ';
        /* SSID truncated to a fixed field so the signal (dBm) lines up; list
           is sorted strongest-first. */
        if (idx == scan_sel) {
            snprintf(buf, sizeof(buf), "> %c%-22.22s %ddBm", lock,
                     scan_results[idx].ssid, (int)scan_results[idx].rssi);
            draw_inverted_line(y, buf);
        } else {
            snprintf(buf, sizeof(buf), "  %c%-22.22s %ddBm", lock,
                     scan_results[idx].ssid, (int)scan_results[idx].rssi);
            draw_text(10, y, buf, IMG_W);
        }
    }

    snprintf(buf, sizeof(buf), "%d FOUND  C:CONNECT", scan_count);
    draw_text(10, 110, buf, IMG_W);
    draw_text(175, 110, "LEFT:BACK", IMG_W);
}

/* ─── Draw on-screen keyboard ─── */
static void render_keyboard(void) {
    memset(frame, 0, sizeof(frame));

    /* Header: SSID */
    char hdr[42];
    snprintf(hdr, sizeof(hdr), "CONNECT: %s",
             selected_network >= 0 ? scan_results[selected_network].ssid : "?");
    draw_text(5, 0, hdr, IMG_W);

    /* Password field + shift indicator */
    char pw_show[35];
    int vis_start = pw_len > 28 ? pw_len - 28 : 0;
    for (int i = 0; i < pw_len - vis_start; i++) {
        char c = pw_buf[vis_start + i];
        pw_show[i] = (c >= 'a' && c <= 'z') ? c - 32 : c;
    }
    pw_show[pw_len - vis_start] = '\0';
    char pw_line[42];
    snprintf(pw_line, sizeof(pw_line), "PW:%s %s", pw_show, kb_shift ? "(CAPS)" : "(LOW)");
    draw_text(5, 10, pw_line, IMG_W);

    /* Separator */
    for (int x = 5; x < 245; x++) px_set(x, 20);

    /* Character grid: 4 rows x 10 cols, cells 24px wide x 12px tall */
    for (int r = 0; r < KB_CHAR_ROWS; r++) {
        for (int c = 0; c < 10; c++) {
            int cx = 5 + c * 24;
            int cy = 24 + r * 12;
            char ch = kb_grid[r][c];

            if (kb_row == r && kb_col == c) {
                /* Selected: inverted cell */
                for (int iy = cy; iy < cy + 10; iy++)
                    for (int ix = cx; ix < cx + 22; ix++)
                        px_set(ix, iy);
                /* Draw character white-on-black */
                const char *pos = strchr(font_chars, ch);
                if (pos) {
                    int fi = (int)(pos - font_chars);
                    for (int row2 = 0; row2 < 7; row2++) {
                        uint8_t bits = font5x7[fi][row2];
                        for (int col2 = 0; col2 < 5; col2++)
                            if (bits & (0x10 >> col2))
                                px_clr(cx + 8 + col2, cy + 1 + row2);
                    }
                }
            } else {
                /* Normal cell */
                const char *pos = strchr(font_chars, ch);
                if (pos) draw_char(cx + 8, cy + 1, (int)(pos - font_chars));
            }
        }
    }

    /* Special keys row at y=76 */
    static const char *sp_labels[] = {"SHIFT", "SPC", "DEL", "DONE", "CANCEL"};
    int sp_x[] = {5, 50, 90, 135, 190};
    int sp_w[] = {40, 35, 40, 50, 55};
    for (int i = 0; i < KB_SP_COUNT; i++) {
        int sx = sp_x[i];
        int sy = 76;
        if (kb_row == KB_SPECIAL_ROW && kb_col == i) {
            for (int iy = sy; iy < sy + 10; iy++)
                for (int ix = sx; ix < sx + sp_w[i]; ix++)
                    px_set(ix, iy);
            /* Draw label white-on-black */
            int tx = sx + 3;
            for (const char *cp = sp_labels[i]; *cp; cp++) {
                char up = *cp;
                const char *pos = strchr(font_chars, up);
                if (pos) {
                    int fi = (int)(pos - font_chars);
                    for (int row2 = 0; row2 < 7; row2++) {
                        uint8_t bits = font5x7[fi][row2];
                        for (int col2 = 0; col2 < 5; col2++)
                            if (bits & (0x10 >> col2))
                                px_clr(tx + col2, sy + 1 + row2);
                    }
                }
                tx += 6;
            }
        } else {
            draw_text(sx + 3, sy + 1, sp_labels[i], sp_w[i]);
        }
    }

    /* Help text */
    draw_text(5, 108, "U/D/L/R:MOVE  C:SELECT", IMG_W);
}

/* ─── Draw motion / accelerometer menu ─── */
#define MOT_ITEM_LIVE      0
#define MOT_ITEM_PEDOMETER 1
#define MOT_ITEM_TILT      2
#define MOT_ITEM_RESET     3
#define MOT_ITEM_THRESH    4
#define MOT_ITEM_I2CSCAN   5
#define MOT_ITEM_BACK      6
#define MOT_MENU_COUNT     7

/* Tall layout: the 122px width can't fit "ACCEL: x y zG" on one line, so the
 * three read-out items put their value on its OWN line under the label. */
static void render_motion_tall(int sel) {
    set_canvas_tall();
    memset(frame, 0, sizeof(frame));
    draw_text(8, 10, "MOTION", canvas_w);
    for (int x = 4; x < 118; x++) px_set(x, 22);

    static char vacc[24], vped[24], vtil[24], vthr[20];
    snprintf(vacc, sizeof(vacc), " %.1f %.1f %.1fG",
             (double)accel_g(accel_x), (double)accel_g(accel_y), (double)accel_g(accel_z));
    snprintf(vped, sizeof(vped), " %lu ST %ld MIN",
             (unsigned long)steps_today, (long)(active_seconds_today / 60));
    snprintf(vtil, sizeof(vtil), " X%.0f Y%.0f %.1fC",
             (double)tilt_x_deg(), (double)tilt_y_deg(), (double)mpu_temp_c);
    snprintf(vthr, sizeof(vthr), "THRESHOLD: %.1fG", (double)step_threshold);

    const char *labels[MOT_MENU_COUNT] = {
        "ACCEL:", "TODAY:", "TILT:", "RESET PEDOMETER", vthr, "I2C BUS SCAN", "BACK",
    };
    const char *vals[MOT_MENU_COUNT] = { vacc, vped, vtil, NULL, NULL, NULL, NULL };

    int y = 30, dy = 16;
    for (int i = 0; i < MOT_MENU_COUNT; i++) {
        char line[28];
        if (i == sel) { snprintf(line, sizeof(line), "> %s", labels[i]); draw_inverted_line(y, line); }
        else          { snprintf(line, sizeof(line), "  %s", labels[i]); draw_text(8, y, line, canvas_w); }
        y += dy;
        if (vals[i]) { draw_text(8, y, vals[i], canvas_w); y += dy; }
    }
    draw_text(6, 236, "C:SEL  L:BACK", canvas_w);
}

static void render_motion_menu(int sel) {
    /* Phase 2: the Housekeeping task already samples the accelerometer (~20 Hz)
     * and runs the pedometer, so we read the shared accel/step globals it keeps
     * fresh. Calling mpu_read_all()/pedometer_update() here too would DOUBLE-COUNT
     * steps (HK + UI both crossing the threshold) — so it's removed. */
    memset(frame, 0, sizeof(frame));
    draw_text(30, 3, "MOTION", IMG_W);
    for (int x = 10; x < 240; x++) px_set(x, 14);

    char buf[42];
    const char *items[MOT_MENU_COUNT];

    static char live_buf[42];
    snprintf(live_buf, sizeof(live_buf), "ACCEL: %.1f  %.1f  %.1fG",
             (double)accel_g(accel_x), (double)accel_g(accel_y), (double)accel_g(accel_z));
    items[MOT_ITEM_LIVE] = live_buf;

    static char ped_buf[42];
    snprintf(ped_buf, sizeof(ped_buf), "TODAY: %lu ST  %ld MIN",
             (unsigned long)steps_today, (long)(active_seconds_today / 60));
    items[MOT_ITEM_PEDOMETER] = ped_buf;

    static char tilt_buf[30];
    snprintf(tilt_buf, sizeof(tilt_buf), "TILT: X%.0f  Y%.0f  %.1fC",
             (double)tilt_x_deg(), (double)tilt_y_deg(), (double)mpu_temp_c);
    items[MOT_ITEM_TILT] = tilt_buf;

    items[MOT_ITEM_RESET] = "RESET PEDOMETER";

    static char thresh_buf[25];
    snprintf(thresh_buf, sizeof(thresh_buf), "THRESHOLD: %.1fG", (double)step_threshold);
    items[MOT_ITEM_THRESH] = thresh_buf;

    items[MOT_ITEM_I2CSCAN] = "I2C BUS SCAN";
    items[MOT_ITEM_BACK] = "BACK";

    if (orientation_is_tall()) {
        render_motion_tall(sel);
        return;
    }

    /* Scrolling window — 5 items visible */
    int vis = 5;
    int start = 0;
    if (sel > vis - 2) start = sel - (vis - 2);
    if (start + vis > MOT_MENU_COUNT) start = MOT_MENU_COUNT - vis;
    if (start < 0) start = 0;

    for (int i = 0; i < vis && (start + i) < MOT_MENU_COUNT; i++) {
        int idx = start + i;
        int y = 18 + i * 11;
        if (idx == sel) {
            snprintf(buf, sizeof(buf), "> %s", items[idx]);
            draw_inverted_line(y, buf);
        } else {
            snprintf(buf, sizeof(buf), "  %s", items[idx]);
            draw_text(10, y, buf, IMG_W);
        }
    }

    /* Scroll indicators */
    if (start > 0) draw_text(235, 18, "/", IMG_W);
    if (start + vis < MOT_MENU_COUNT) draw_text(235, 18 + (vis - 1) * 11, "/", IMG_W);

    /* Gyro readout at bottom */
    static char gyro_buf[42];
    snprintf(gyro_buf, sizeof(gyro_buf), "GYRO: %.0f  %.0f  %.0f D/S",
             (double)gyro_dps(gyro_x), (double)gyro_dps(gyro_y), (double)gyro_dps(gyro_z));
    draw_text(10, 86, gyro_buf, IMG_W);

    /* Magnitude bar */
    float mag = accel_magnitude();
    snprintf(buf, sizeof(buf), "MAG: %.2fG", (double)mag);
    draw_text(10, 98, buf, IMG_W);

    if (!mpu_ok) draw_text(130, 98, "MPU:NO", IMG_W);
    else draw_text(130, 98, "MPU:OK", IMG_W);
    draw_text(175, 110, "LEFT:BACK", IMG_W);
}

/* ─── I2C bus scan screen ─── */
static void render_i2c_scan(void) {
    memset(frame, 0, sizeof(frame));
    draw_text(30, 3, "I2C BUS SCAN", IMG_W);
    for (int x = 10; x < 240; x++) px_set(x, 14);

    draw_text(10, 20, "SCANNING I2C0 (GP0/GP1)...", IMG_W);
    transpose_to_display();
    display_render();

    /* Scan all valid 7-bit addresses */
    uint8_t found[16];
    int found_count = 0;
    printf("[I2C] Scanning I2C0 bus...\n");

    for (int addr = 0x08; addr < 0x78; addr++) {
        uint8_t dummy;
        int ret = i2c_read_timeout_us(MPU_I2C, addr, &dummy, 1, false, MPU_I2C_TIMEOUT_US);
        if (ret >= 0 && found_count < 16) {
            found[found_count++] = (uint8_t)addr;
            printf("[I2C]   Device at 0x%02X\n", addr);
        }
    }
    printf("[I2C] Scan complete: %d device(s)\n", found_count);

    /* Show results */
    memset(frame, 0, sizeof(frame));
    draw_text(30, 3, "I2C BUS SCAN", IMG_W);
    for (int x = 10; x < 240; x++) px_set(x, 14);

    char buf[42];
    snprintf(buf, sizeof(buf), "FOUND %d DEVICE(S):", found_count);
    draw_text(10, 20, buf, IMG_W);

    for (int i = 0; i < found_count && i < 8; i++) {
        const char *desc = "";
        if (found[i] == 0x68) desc = " (MPU-6050)";
        else if (found[i] == 0x69) desc = " (MPU-6050 AD0:H)";
        else if (found[i] == 0x76) desc = " (BME280/BMP280)";
        else if (found[i] == 0x77) desc = " (BME280/BMP280)";
        else if (found[i] == 0x3C) desc = " (SSD1306 OLED)";
        else if (found[i] == 0x50) desc = " (EEPROM)";
        snprintf(buf, sizeof(buf), "  0X%02X%s", found[i], desc);
        draw_text(10, 32 + i * 10, buf, IMG_W);
    }

    if (found_count == 0) {
        draw_text(10, 40, "NO DEVICES FOUND", IMG_W);
        draw_text(10, 55, "CHECK WIRING:", IMG_W);
        draw_text(10, 66, "SDA:GP0(PIN1) SCL:GP1(PIN2)", IMG_W);
        draw_text(10, 77, "VCC:3V3 GND:GND AD0:GND", IMG_W);
    }

    draw_text(155, 110, "C:BACK", IMG_W);
}

/* ─── Show connecting screen (blocks during wifi_connect_to) ─── */
static void show_connecting_screen(const char *ssid) {
    memset(frame, 0, sizeof(frame));
    draw_text(40, 40, "CONNECTING TO", IMG_W);
    draw_text(40, 55, ssid, IMG_W);
    draw_text(40, 75, "PLEASE WAIT...", IMG_W);
    transpose_to_display();
    display_render();
}

/* ─── WiFi / NTP functions ─── */

static void ntp_recv_cb(void *arg, struct udp_pcb *pcb, struct pbuf *p,
                         const ip_addr_t *addr, u16_t port) {
    (void)arg; (void)pcb; (void)addr; (void)port;
    if (p->tot_len >= NTP_MSG_LEN) {
        uint8_t *buf = (uint8_t *)p->payload;
        uint32_t secs = (buf[40] << 24) | (buf[41] << 16) | (buf[42] << 8) | buf[43];
        time_t epoch = (time_t)(secs - NTP_DELTA) + TIMEZONE_OFFSET_SEC;
        struct tm *t = gmtime(&epoch);
        datetime_t dt = {
            .year  = (int16_t)(t->tm_year + 1900),
            .month = (int8_t)(t->tm_mon + 1),
            .day   = (int8_t)t->tm_mday,
            .dotw  = (int8_t)t->tm_wday,
            .hour  = (int8_t)t->tm_hour,
            .min   = (int8_t)t->tm_min,
            .sec   = (int8_t)t->tm_sec,
        };
        rtc_set_datetime(&dt);
        ntp_synced = true;
        ntp_time_received = true;
        printf("[NTP] Synced: %04d-%02d-%02d %02d:%02d:%02d\n",
               dt.year, dt.month, dt.day, dt.hour, dt.min, dt.sec);
    }
    pbuf_free(p);
}

static void ntp_dns_cb(const char *name, const ip_addr_t *addr, void *arg) {
    (void)arg;
    if (addr) {
        ntp_server_addr = *addr;
        struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, NTP_MSG_LEN, PBUF_RAM);
        if (p) {
            memset(p->payload, 0, NTP_MSG_LEN);
            ((uint8_t *)p->payload)[0] = 0x1b;
            udp_sendto(ntp_pcb, p, &ntp_server_addr, NTP_PORT);
            pbuf_free(p);
            printf("[NTP] Request sent to %s\n", ipaddr_ntoa(addr));
        }
    }
}

static void ntp_request(void) {
    if (!ntp_pcb) {
        ntp_pcb = udp_new();
        if (!ntp_pcb) return;
        udp_recv(ntp_pcb, ntp_recv_cb, NULL);
    }
    dns_gethostbyname(NTP_SERVER, &ntp_server_addr, ntp_dns_cb, NULL);
}

/* ─── WiFi scan ─── */
static int wifi_scan_callback(void *env, const cyw43_ev_scan_result_t *result) {
    (void)env;
    if (!result || scan_count >= MAX_SCAN_RESULTS) return 0;
    if (result->ssid_len == 0) return 0;  /* skip hidden networks */

    /* Deduplicate — keep the one with better RSSI */
    for (int i = 0; i < scan_count; i++) {
        if (strncmp(scan_results[i].ssid, (const char *)result->ssid, result->ssid_len) == 0
            && strlen(scan_results[i].ssid) == result->ssid_len) {
            if (result->rssi > scan_results[i].rssi) {
                scan_results[i].rssi = (int8_t)result->rssi;
                scan_results[i].auth_mode = result->auth_mode;
            }
            return 0;
        }
    }

    memcpy(scan_results[scan_count].ssid, result->ssid, result->ssid_len);
    scan_results[scan_count].ssid[result->ssid_len] = '\0';
    scan_results[scan_count].rssi = (int8_t)result->rssi;
    scan_results[scan_count].auth_mode = result->auth_mode;
    scan_count++;
    return 0;
}

/* Sort discovered networks by signal strength, strongest first. RSSI is in
 * dBm (negative), so a larger value = stronger. Simple insertion sort — the
 * list is tiny (<= MAX_SCAN_RESULTS). */
static void wifi_sort_by_rssi(void) {
    for (int i = 1; i < scan_count; i++) {
        scan_entry_t key = scan_results[i];
        int j = i - 1;
        while (j >= 0 && scan_results[j].rssi < key.rssi) {
            scan_results[j + 1] = scan_results[j];
            j--;
        }
        scan_results[j + 1] = key;
    }
}

static void wifi_start_scan(void) {
    scan_count = 0;
    scan_sel = 0;
    scan_in_progress = true;
    scan_complete = false;
    cyw43_arch_enable_sta_mode();
    cyw43_wifi_scan_options_t opts = {0};
    cyw43_wifi_scan(&cyw43_state, &opts, NULL, wifi_scan_callback);
    printf("[WiFi] Scan started\n");
}

/* ─── Saved WiFi networks: flash-backed store (data declared earlier) ─── */
/* The raw erase+program. Run ONLY via flash_safe_execute() (below) so the other
 * core is parked first — critical under SMP because the Display task on core 1
 * executes from XIP flash and its next instruction fetch during an erase would
 * fault. `param` points at the 4 KB image to program. */
static void saved_flash_op(void *param) {
    const uint8_t *buf = (const uint8_t *)param;
    flash_range_erase(SAVED_FLASH_OFFSET, FLASH_SECTOR_SIZE);
    flash_range_program(SAVED_FLASH_OFFSET, buf, FLASH_SECTOR_SIZE);
}

static void saved_write_flash(void) {
    static uint8_t buf[FLASH_SECTOR_SIZE];
    memset(buf, 0xFF, sizeof(buf));
    memcpy(buf, &g_saved, sizeof(g_saved));

    /* flash_safe_execute uses the FreeRTOS-SMP multicore lockout to safely park
     * core 1 (the Display task, which runs the e-ink driver from XIP flash) for
     * the duration of the write. We ALWAYS go through it and retry on contention.
     * We deliberately do NOT fall back to a plain interrupts-off write: that only
     * stops THIS core and would let core 1 XIP-fault mid-erase. All callers run
     * under the scheduler after the flash-ready handshake, so this is safe. */
    for (int attempt = 0; attempt < 3; attempt++) {
        if (flash_safe_execute(saved_flash_op, buf, 3000 /* ms */) == PICO_OK) return;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    printf("[flash] WARNING: flash_safe_execute failed 3x — settings NOT saved this time\n");
}

static void saved_seed_defaults(void) {
    memset(&g_saved, 0, sizeof(g_saved));
    g_saved.magic = SAVED_MAGIC;
    g_saved.count = 2;
    strncpy(g_saved.nets[0].ssid, "Moop Ship",    sizeof(g_saved.nets[0].ssid) - 1);
    strncpy(g_saved.nets[0].pass, WIFI_PASS,       sizeof(g_saved.nets[0].pass) - 1);
    strncpy(g_saved.nets[1].ssid, "MoopsterCell",  sizeof(g_saved.nets[1].ssid) - 1);
    strncpy(g_saved.nets[1].pass, WIFI_PASS,       sizeof(g_saved.nets[1].pass) - 1);
    g_saved.vsys_cal = 1.0f;
    g_saved.wetgreg_id = 0;            /* 0 → generated in saved_load() */
    g_saved.wetgreg_name[0] = '\0';
    g_saved.social_on = 0;
    g_saved.met_count = 0;
    g_saved.auto_rotate = 1;          /* auto-rotate ON by default */
    g_saved.manual_orient = OR_TALL;
}

static void saved_load(void) {
    const saved_store_t *fl = (const saved_store_t *)(XIP_BASE + SAVED_FLASH_OFFSET);
    if (fl->magic == SAVED_MAGIC && fl->count <= MAX_SAVED) {
        memcpy(&g_saved, fl, sizeof(g_saved));
    } else {
        saved_seed_defaults();   /* first boot — persist the two defaults */
        saved_write_flash();
    }
    /* Battery trim is NOT loaded from flash anymore: the measurement uses the
     * baked nominal estimate (VSYS_CAL, the 3:1-divider + 3.3 V-ref math) every
     * boot, so the device needs no calibration ritual. The 4-bar icon doesn't
     * need per-board precision, and the peak-hold estimator removes the load
     * bias that used to make calibration seem necessary. The Device-Info UP/DOWN
     * keys remain as an OPTIONAL live trim (not persisted). For real per-board
     * accuracy, Rev 2 will auto-anchor to 4.2 V off the TP4056 STDBY pin. */
    g_vsys_cal = 1.0f;

    /* Assign a persistent social id on first ever boot (and defend against a
     * zeroed/sanitised field). rng is already seeded before the scheduler. */
    if (g_saved.wetgreg_id == 0) {
        g_saved.wetgreg_id = (uint16_t)(rng_next() & 0xFFFF);
        if (g_saved.wetgreg_id == 0) g_saved.wetgreg_id = 0x1D1E;   /* never 0 */
        if (g_saved.met_count > SOCIAL_MAX) g_saved.met_count = 0;
        saved_write_flash();
    }
    if (g_saved.met_count > SOCIAL_MAX) g_saved.met_count = 0;     /* sanity */
    /* Display settings appended after the social block — old images read 0xFF
     * here, so sanitise to defaults (auto-rotate ON, tall). */
    if (g_saved.auto_rotate > 1)   g_saved.auto_rotate = 1;
    if (g_saved.manual_orient > 2) g_saved.manual_orient = OR_TALL;
    g_auto_rotate   = g_saved.auto_rotate ? true : false;
    g_manual_orient = g_saved.manual_orient;
    printf("[social] this WetGreg: id=%04X name=\"%s\"\n",
           g_saved.wetgreg_id, wetgreg_display_name());
}

/* Optional LIVE battery-trim nudge (Device-Info UP/DOWN). Not persisted — it
 * resets to the baked nominal estimate on the next boot. Kept only as a bench
 * sanity tool; normal use needs no calibration. */
static void battery_cal_save(float cal) {
    if (cal < 0.5f) cal = 0.5f; else if (cal > 2.0f) cal = 2.0f;
    g_vsys_cal = cal;
}

/* ─── Social log (other WetGregs we've met) ───────────────────────────────────
 * Persisted in g_saved.met[]. Names aren't stored (derived from id). */

/* Calendar-day key for the 24 h re-greet cooldown. RTC unset → 0 (still works:
 * a re-greet fires whenever the day key changes). */
static uint32_t wetgreg_today(void) {
    datetime_t t;
    rtc_get_datetime(&t);
    if (t.year < 2020) return 0;
    return (uint32_t)t.year * 10000u + (uint32_t)t.month * 100u + (uint32_t)t.day;
}
static int met_find(uint16_t id) {
    for (uint32_t i = 0; i < g_saved.met_count && i < SOCIAL_MAX; i++)
        if (g_saved.met[i].id == id) return (int)i;
    return -1;
}
/* OR-in `flag`, stamp `day`, persisting. Evicts the oldest if the log is full. */
static void met_record(uint16_t id, uint8_t flag, uint32_t day) {
    int idx = met_find(id);
    if (idx < 0) {
        if (g_saved.met_count < SOCIAL_MAX) {
            idx = (int)g_saved.met_count++;
        } else {
            /* full — evict the entry with the oldest last_day */
            idx = 0;
            for (uint32_t i = 1; i < SOCIAL_MAX; i++)
                if (g_saved.met[i].last_day < g_saved.met[idx].last_day) idx = (int)i;
        }
        g_saved.met[idx].id = id;
        g_saved.met[idx].flags = 0;
    }
    g_saved.met[idx].flags |= flag;
    g_saved.met[idx].last_day = day;
    saved_write_flash();
}
/* True if we should auto-prompt for this WetGreg today (24 h cooldown). */
static bool met_should_greet(uint16_t id) {
    int idx = met_find(id);
    if (idx < 0) return true;                       /* never met */
    return g_saved.met[idx].last_day != wetgreg_today();
}
static void wetgreg_set_name(const char *name) {
    snprintf(g_saved.wetgreg_name, sizeof(g_saved.wetgreg_name), "%s", name ? name : "");
    saved_write_flash();
}
static void wetgreg_set_social(bool on) {
    g_saved.social_on = on ? 1 : 0;
    saved_write_flash();
}

static const char *saved_find_pass(const char *ssid) {
    for (uint32_t i = 0; i < g_saved.count; i++)
        if (strcmp(g_saved.nets[i].ssid, ssid) == 0) return g_saved.nets[i].pass;
    return NULL;
}

static void saved_add(const char *ssid, const char *pass) {
    for (uint32_t i = 0; i < g_saved.count; i++)
        if (strcmp(g_saved.nets[i].ssid, ssid) == 0) {       /* update existing */
            strncpy(g_saved.nets[i].pass, pass, sizeof(g_saved.nets[i].pass) - 1);
            saved_write_flash(); return;
        }
    if (g_saved.count < MAX_SAVED) {                          /* add new */
        strncpy(g_saved.nets[g_saved.count].ssid, ssid, sizeof(g_saved.nets[0].ssid) - 1);
        strncpy(g_saved.nets[g_saved.count].pass, pass, sizeof(g_saved.nets[0].pass) - 1);
        g_saved.count++;
        saved_write_flash();
    }
}

static void saved_forget(int idx) {
    if (idx < 0 || idx >= (int)g_saved.count) return;
    for (uint32_t i = idx; i + 1 < g_saved.count; i++) g_saved.nets[i] = g_saved.nets[i + 1];
    g_saved.count--;
    saved_write_flash();
}

/* ─── WiFi connect (accepts arbitrary SSID/password) ─── */
static void wifi_connect_to(const char *ssid, const char *password) {
    printf("[WiFi] Connecting to \"%s\"...\n", ssid);
    strncpy(wifi_ssid_display, ssid, sizeof(wifi_ssid_display) - 1);
    wifi_ssid_display[sizeof(wifi_ssid_display) - 1] = '\0';
    wifi_enabled = true;

    cyw43_arch_enable_sta_mode();

    uint32_t auth = (password[0] != '\0') ? CYW43_AUTH_WPA2_AES_PSK : 0;

    /* Drive the join ourselves instead of cyw43_arch_wifi_connect_timeout_ms().
     *
     * WHY: under pico_cyw43_arch_lwip_sys_freertos (SMP), that SDK helper's wait
     * loop calls cyw43_arch_wait_for_work_until(<full deadline>) — and in this
     * config a link/DHCP-completion event does NOT reliably wake that wait. So it
     * sleeps the entire timeout, then its own time_reached() check trips and it
     * returns PICO_ERROR_TIMEOUT *even though association + DHCP had succeeded*.
     * Net effect: scanning worked but every connect "failed".
     *
     * FIX: start the join async, then poll cyw43_tcpip_link_status() ~4x/sec so we
     * see the LINK_UP transition (DHCP done) immediately. Link states:
     *   0=DOWN 1=JOIN 2=NOIP 3=UP ; negative: -1=FAIL -2=NONET -3=BADAUTH. */
    int err = cyw43_arch_wifi_connect_async(ssid, password, auth);
    if (err) {
        printf("[WiFi] connect_async start failed (err=%d)\n", err);
        wifi_connected = false;
        return;
    }

    int last_t = -999;
    absolute_time_t deadline = make_timeout_time_ms(20000);
    for (;;) {
        int ts = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);
        if (ts != last_t) {
            printf("[WiFi] link tcpip=%d\n", ts);
            last_t = ts;
        }
        if (ts == CYW43_LINK_UP) break;          /* associated + DHCP IP assigned */
        if (ts == CYW43_LINK_BADAUTH) {
            printf("[WiFi] bad auth — wrong password\n");
            wifi_connected = false;
            return;
        }
        if (ts == CYW43_LINK_FAIL) {
            printf("[WiFi] association failed\n");
            wifi_connected = false;
            return;
        }
        /* CYW43_LINK_NONET = AP not seen this round; re-issue the join (mirrors the
         * SDK) and keep polling until the deadline. */
        if (ts == CYW43_LINK_NONET)
            cyw43_arch_wifi_connect_async(ssid, password, auth);
        if (time_reached(deadline)) {
            printf("[WiFi] timeout (last tcpip=%d)\n", last_t);
            wifi_connected = false;
            return;
        }
        cyw43_arch_wait_for_work_until(make_timeout_time_ms(250));
    }

    wifi_connected = true;
    struct netif *netif = &cyw43_state.netif[CYW43_ITF_STA];
    snprintf(wifi_ip_str, sizeof(wifi_ip_str), "%s", ipaddr_ntoa(&netif->ip_addr));
    cyw43_wifi_get_rssi(&cyw43_state, &wifi_rssi);
    printf("[WiFi] Connected: %s  RSSI: %d\n", wifi_ip_str, (int)wifi_rssi);

    ntp_request();
}

static void wifi_connect(void) {
    wifi_connect_to(WIFI_SSID, WIFI_PASS);
}

static void wifi_disconnect(void) {
    printf("[WiFi] Disconnecting...\n");
    cyw43_wifi_leave(&cyw43_state, CYW43_ITF_STA);
    /* Keep CYW43 initialised — battery VSYS sense needs it on GPIO 29 */
    wifi_connected = false;
    wifi_enabled = false;
    ntp_synced = false;
    strncpy(wifi_ssid_display, "---", sizeof(wifi_ssid_display));
    strncpy(wifi_ip_str, "---", sizeof(wifi_ip_str));
}

/* ─── Pick a quote matching current_mood, or random if -1 ─── */
static int pick_quote(void) {
    if (current_mood < 0)
        return rng_next() % QUOTE_COUNT;

    /* Collect indices matching the mood */
    int matches[QUOTE_COUNT];
    int count = 0;
    for (int i = 0; i < QUOTE_COUNT; i++) {
        if (quotes[i].mood == (uint8_t)current_mood)
            matches[count++] = i;
    }
    if (count == 0)
        return rng_next() % QUOTE_COUNT;  /* fallback */
    return matches[rng_next() % count];
}

/* Push the current mood + step count to the BLE status characteristic so a
 * paired phone can read/subscribe. No-op (and no notify) when unchanged. */
static void bt_push_status(void) {
    if (!wetgreg_bt_active()) return;
    char s[24];
    snprintf(s, sizeof(s), "%s %lu",
             current_mood < 0 ? "ALL" : mood_names[current_mood],
             (unsigned long)steps_today);
    wetgreg_bt_set_status(s);
}

/* ─── Housekeeping sampling (Phase 2) ───────────────────────────────────────
 * Called repeatedly by the Housekeeping task (rtos_tasks.c). This is now the
 * ONLY runtime caller of the I2C accelerometer and the battery ADC, so those
 * blocking reads leave the UI/render path entirely. It samples motion +
 * orientation + steps every pass, the battery every ~2 s, then publishes a
 * snapshot the UI copies. Battery values are cached in g_batt_pct/g_batt_v
 * (declared in the battery section) so the icon / info screen read them with no
 * inline ADC hit. */
void hk_sample(void) {
    orientation_update();                  /* accel -> g_orientation, steps, input rotation */
    viewing_update();                      /* updates last_motion_ms from real motion       */
    g_screen_idle = !screen_is_viewed();   /* pocket/idle freeze flag — HK owns it now       */

    /* ── Battery: PEAK-HOLD over a ~2 s window, then EMA-smoothed ──
     * The old code took one instantaneous trimmed-mean burst every 2 s and showed
     * it raw — so the reading swung with whatever load happened to be on the rail
     * that instant (the e-ink full-refresh runs ~every 0.7 s and the radio bursts,
     * both of which pull VSYS DOWN). Load only ever sags VSYS below the true
     * resting voltage, never above it, so the MAX reading across a window of
     * lightly-spaced samples ≈ the open-circuit voltage the discharge curve
     * expects. We sample once per HK tick (~50 ms), keep the window peak, and
     * every ~2 s fold that peak into an EMA. Result: a steady reading that tracks
     * real charge instead of momentary load. */
    static uint32_t batt_win_start = 0, batt_last_ms = 0;
    static float    batt_win_peak  = 0.0f;
    static float    batt_v_ema     = 0.0f;
    uint32_t now = to_ms_since_boot(get_absolute_time());

    /* Sample at most ~4x/sec. read_vsys_volts() grabs the cyw43 lock (GP29 is
     * shared with the CYW43 SPI), so hammering it at 20 Hz contends with the BLE
     * radio; 250 ms still gives ~8 peak samples across a 2 s window. */
    static int   was_usb  = -1;
    static float last_bv  = 0.0f;     /* last on-battery reading, retained for diag */
    static int   last_bp  = -1;
    if (now - batt_last_ms >= 250) {
        batt_last_ms = now;
        bool usb = is_usb_powered();
        g_on_usb = usb;
        /* Charging power-diet: on a USB plug/unplug edge, suspend BLE scanning
         * while charging (it's the biggest continuous radio load), and restore
         * the user's setting when unplugged — so the cell can actually fill. */
        if ((was_usb != 1) && usb) {
            wetgreg_social_enable(false);
        } else if ((was_usb != 0) && !usb) {
            if (g_saved.social_on) { wetgreg_social_set_self(g_saved.wetgreg_id); wetgreg_social_enable(true); }
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

    sensor_snapshot_t s;
    s.orientation    = (uint8_t)g_orientation;
    s.is_tall        = orientation_is_tall();
    s.ax = accel_x; s.ay = accel_y; s.az = accel_z;
    s.mag            = accel_magnitude();
    s.steps_today    = steps_today;
    s.active_seconds = active_seconds_today;
    s.last_motion_ms = last_motion_ms;
    s.mpu_ok         = mpu_ok;
    s.batt_pct       = g_batt_pct;
    s.vsys           = g_batt_v;
    s.vsys_raw       = 0.0f;   /* the battery-cal screen takes its own fresh RAW read */
    s.usb            = is_usb_powered();
    rtos_snapshot_publish(&s);
}

/* ─── Main ─── */
/* ─── FreeRTOS application task ─────────────────────────────────────────────
 * This is the UI task: it renders, runs the state machine, consumes input
 * EVENTS from the Input task's queue, and hands frames to the Display task
 * (core 1). It also spawns the Input/Display/Housekeeping tasks once it is
 * running (see rtos_tasks_start). Declared here, defined just below main(). */
static void app_task(void *param);
static TaskHandle_t g_app_task = NULL;

/* Stack is measured in WORDS (4 bytes each). The state machine's render path is
 * deep (nested draw_* + snprintf), so we reserve a generous 16 KB for now and
 * trim it in Phase 3 using uxTaskGetStackHighWaterMark(). */
#define APP_TASK_STACK_WORDS  4096
#define APP_TASK_PRIO         (tskIDLE_PRIORITY + 2)

int main(void) {
    stdio_init_all();
    sleep_ms(50);   /* brief settle; was 1000ms of pure boot delay for USB serial */
    printf("WETGREG HUB v%s (%s) | display: %s | %d quotes | built %s %s\n",
           WETGREG_VERSION, WETGREG_VERSION_DATE, DISPLAY_NAME, QUOTE_COUNT, __DATE__, __TIME__);

    init_rtc_from_compile_time();

    if (DEV_Module_Init() != 0) {
        printf("ERROR: Hardware init failed.\n");
        return 1;
    }

    joystick_init();
    speaker_init();

#ifdef PICOWOTA_OTA
    /* Hold the joystick UP at power-on to drop into the picowota WiFi
       bootloader for an over-the-air firmware update. Pull-ups are active
       (joystick is active-low), so give them a moment to settle first. */
    sleep_ms(20);
    if (!gpio_get(JOY_UP)) {
        speaker_tone(2000, 60);  /* audible "entering OTA" cue */
        picowota_reboot(true);
    }
#endif

    /* Startup chime */
    speaker_tone(1000, 80); sleep_ms(30);
    speaker_tone(1500, 80); sleep_ms(30);
    speaker_tone(2000, 120);

    /* NOTE: EPD_Init()/EPD_Clear() are NO LONGER called here. The e-ink panel is
     * now owned exclusively by the Display task (core 1), which initialises it in
     * its prologue (display_init_panel). DEV_Module_Init() above already brought
     * up the SPI bus on the boot core, which is fine (one-time, pre-scheduler). */
    rng_seed();

    /* ═══════════ Hand control to the FreeRTOS scheduler ═══════════
     * Everything above ran "bare metal": plain function calls on the boot core,
     * no kernel yet. Now we create our application task and start the scheduler.
     *
     * WHY create a task instead of just continuing? Because the FreeRTOS-flavour
     * cyw43/Wi-Fi/Bluetooth driver spins up its OWN background task, which only
     * works once the scheduler is running. So cyw43_arch_init() and the whole
     * app loop must happen *inside* a task, not here in plain main().
     *
     * vTaskCoreAffinitySet pins this task to core 0 (bit 0). vTaskStartScheduler
     * NEVER returns — from here on the kernel owns the CPUs and runs our tasks. */
    xTaskCreate(app_task, "app", APP_TASK_STACK_WORDS, NULL, APP_TASK_PRIO, &g_app_task);
    vTaskCoreAffinitySet(g_app_task, (1u << 0));   /* run on core 0 */
    vTaskStartScheduler();

    /* Unreachable: the scheduler only returns if it ran out of heap for the idle
     * task, which our hooks/heap size prevent. Hang loudly if it ever happens. */
    printf("FATAL: scheduler returned\n");
    for (;;) { }
}

/* ─── The single application task (Phase 1) ───
 * Identical work to the original firmware, just now running as a FreeRTOS task.
 * Phase 2 carves the body below into separate Input/UI/Display/Housekeeping
 * tasks; for now it stays monolithic so we can verify the RTOS plumbing alone. */
static void app_task(void *param) {
    (void)param;   /* we don't use the task argument; cast-to-void silences the warning */

    /* Init CYW43 early — even without WiFi, the chip's SPI CS shares
       GPIO 29 (ADC3/VSYS sense) and will hold it low if uninitialised. Under the
       FreeRTOS cyw43 arch this also starts the networking background task. */
    if (cyw43_arch_init()) {
        printf("WARNING: CYW43 init failed — battery reads may be 0\n");
    }
    /* WiFi is NOT auto-connected at boot — that blocked startup for up to ~15s.
       Credentials are cached (saved-networks store, seeded with Moop Ship +
       MoopsterCell), so connecting from the Network menu needs no password
       entry. The clock NTP-syncs whenever WiFi is connected. */

    battery_init();
    mpu_init();

    /* ═══ Phase 2: spin up the Input, Display, and Housekeeping tasks ═══
     * From here on THIS task is the UI task: it renders, runs the state machine,
     * gets input from the Input task's queue, and hands finished frames to the
     * Display task (core 1). cyw43_arch_init() above stays on this (core 0) task,
     * which is where the cyw43/Wi-Fi/BT background task must live. */
    rtos_tasks_start();

    /* Wait (explicit handshake, not a timing guess) for the Display task on core 1
     * to register with the flash lockout before saved_load() — which may write
     * flash on first boot — so that write is SMP-safe (core 1 parked, no XIP fault). */
    rtos_wait_flash_ready();
    saved_load();   /* load cached WiFi networks (seeds Moop Ship + MoopsterCell) */

    /* Social: bake our id into the beacon, and resume scanning if it was left on. */
    wetgreg_social_set_self(g_saved.wetgreg_id);
    if (g_saved.social_on) {
        if (!wetgreg_bt_active()) wetgreg_bt_init();
        bt_enabled = true;
        wetgreg_social_set_self(g_saved.wetgreg_id);   /* re-push now that BT is up */
        wetgreg_social_enable(true);
    }

    /* ─── State machine ─── */
    uint8_t state = STATE_SPLASH;      /* one-shot boot splash, then STATE_OCTOPUS */
    uint32_t splash_t0 = 0;            /* 0 = splash not started yet */
    uint32_t frame_idx = 0;
    int qi = pick_quote();
    int menu_sel = 0;
    int mood_sel = 0;  /* 0 = ALL, 1-16 = specific mood */

    int snd_sel = 0;
    int social_sel = 0;
    int disp_sel = 0;
    int met_sel = 0;
    int nearby_sel = 0;

    /* Sub-screen input: block on the Input task's event queue for up to `ms` ms.
     * The do-while(0) PRESERVES the old `break;` semantics in every screen body —
     * a `break` leaves the poll, then the case's trailing `break` triggers the
     * redraw — but with NO busy-wait: the UI sleeps until a press arrives or the
     * timeout elapses (the Input task captures presses meanwhile). */
    #define POLL_INPUT(ms) \
        do { \
            uint8_t inp; \
            if (!ui_get_input(&inp, (ms))) break;
    #define POLL_END } while (0);

    while (true) {
        /* Default every screen to the WIDE canvas; the tall layouts override it
         * for themselves. NOTE (Phase 2): accelerometer sampling
         * (orientation_update) now runs in the Housekeeping task, and Wi-Fi/BT are
         * serviced by the cyw43 FreeRTOS background task — so the per-iteration
         * orientation_update()/cyw43_arch_poll() that used to sit here are gone. */
        set_canvas_wide();

        switch (state) {

        /* ════════ BOOT SPLASH ════════ */
        case STATE_SPLASH: {
            if (splash_t0 == 0) {
                /* Hold the first frame until the accel classifier's first
                 * verdict lands (~300 ms; see orientation_update's boot
                 * prime) — otherwise the splash always STARTS in the default
                 * landscape hold and flips to portrait mid-entrance. Times
                 * out fast when no verdict can come (device flat, no accel);
                 * a press during the wait stays queued and skips on frame 1. */
                for (int w = 0; !g_orient_primed && w < 14; w++)
                    vTaskDelay(pdMS_TO_TICKS(50));
                uint32_t t = to_ms_since_boot(get_absolute_time());
                splash_t0 = t ? t : 1;
                splash_bubbles_init();
            }
            uint32_t elapsed = to_ms_since_boot(get_absolute_time()) - splash_t0;

            bool skip = false;
            if (elapsed < SPLASH_TOTAL_MS) {
                render_splash(elapsed, frame_idx);
                draw_orient_hud();
                transpose_to_display();
                display_render();
                frame_idx++;
                /* Sleep one frame on the input queue — any press skips. The wait
                 * keeps the submit cadence above the panel drain so the Input
                 * task is never starved (see the STATE_MOTION note). */
                uint8_t inp;
                skip = ui_get_input(&inp, SPLASH_FRAME_MS);
            }
            if (skip || elapsed >= SPLASH_TOTAL_MS) {
                if (skip) speaker_tone(1319, 80);
                qi = pick_quote();
                frame_idx = 0;
                wake_screen();
                state = STATE_OCTOPUS;
            }
            break;
        }

        /* ════════ OCTOPUS MAIN SCREEN ════════ */
        case STATE_OCTOPUS: {
            const Quote *q = &quotes[qi];
            const uint8_t *cycle = mood_cycle(q->mood);
            uint8_t expr = cycle[frame_idx % 4];

            if (expr == EXPR_OPEN && frame_idx > 0)
                qi = pick_quote();

            /* Skip the redraw entirely while pocketed/idle — the e-ink keeps
             * showing the last frame for free; this is the main power saver. */
            if (!g_screen_idle) {
                if (orientation_is_tall()) {
                    /* Longways layout draws its own 2-row status bar (wifi + batt,
                     * date + time), quote, and octopus at the bottom. */
                    render_octopus_tall(&quotes[qi], expr, frame_idx);
                } else {
                    render_frame(&quotes[qi], expr, frame_idx);
                    draw_wifi_icon(0, 1, wifi_connected);   /* top-left */
                    {
                        int soc_x = 18;
                        if (wetgreg_bt_state() == BT_PAIRED) { draw_bt_icon(18, 1); soc_x = 32; }
                        if (wetgreg_social_active()) draw_social_icon(soc_x, 1);
                    }
                    draw_battery_icon(234, 1);               /* top-right */
                    {
                        char sbuf[20];
                        snprintf(sbuf, sizeof(sbuf), "STEPS %lu",
                                 (unsigned long)steps_today);
                        draw_text(5, 113, sbuf, IMG_W);      /* left of the screen */
                    }
                    draw_text(175, 113, "DOWN:MENU", IMG_W);
                }
                draw_orient_hud();   /* calibration overlay (ORIENT_DEBUG) */
                transpose_to_display();

                display_render();
                bt_push_status();    /* keep a paired phone's mood/steps read fresh */
                frame_idx++;
            }

            /* UI tick loop (~3 s, then re-render so the mouth animates). The
             * accelerometer/idle are sampled by the Housekeeping task — we just
             * watch the snapshot's orientation + the g_screen_idle flag and drain
             * the Input task's event queue. The ~300 ms refresh runs on core 1, so
             * this loop keeps reacting to presses the whole time. */
            sensor_snapshot_t s; rtos_snapshot_get(&s);
            uint8_t o0    = s.orientation;
            bool    idle0 = g_screen_idle;
            /* While charging, animate ~10x less often (each tick is ~15 ms, so
             * 200=~3 s normally, 2000=~30 s on USB) to cut e-ink refresh load and
             * let the cell charge. Input still polled every tick, so it stays
             * responsive — only the idle animation cadence slows. */
            int octo_ticks = g_on_usb ? 2000 : 200;
            for (int i = 0; i < octo_ticks && state == STATE_OCTOPUS; i++) {
                uint8_t inp;
                bool got = ui_get_input(&inp, 15);     /* ~15 ms tick; sleeps efficiently */
                rtos_snapshot_get(&s);
                /* Idle flag is owned by Housekeeping (it freezes redraws when
                 * pocketed). Just re-enter the case on a change so the render
                 * gate (if !g_screen_idle) re-evaluates — do NOT wake_screen here
                 * or we'd fight HK and redraw forever while idle. */
                if (g_screen_idle != idle0) break;
                if (s.orientation != o0) { wake_screen(); break; }   /* rotated → user present, redraw */
                if (wetgreg_bt_active() && wetgreg_bt_take_command() >= 0) {
                    qi = pick_quote();   /* phone poked us → fresh quote */
                    speaker_tone(1600, 60);
                    wake_screen(); break;
                }
                /* Another WetGreg in range? Interrupt the mood cycle. */
                if (g_saved.social_on) {
                    wetgreg_peer_t pr;
                    if (wetgreg_social_poll(&pr)) {
                        if (pr.hello_to_me) {
                            met_record(pr.id, MET_HELLO_RECV, wetgreg_today());
                            g_social_peer = pr.id; g_social_peer_rssi = pr.rssi;
                            g_play_emote = pr.emote ? pr.emote : EMOTE_WAVE;
                            g_play_incoming = true; g_play_next = STATE_SOCIAL_RECV;
                            speaker_tone(1800, 90);
                            wake_screen(); state = STATE_EMOTE_PLAY; break;
                        } else if (met_should_greet(pr.id)) {
                            g_social_peer = pr.id; g_social_peer_rssi = pr.rssi;
                            speaker_tone(1200, 100);
                            wake_screen(); state = STATE_SOCIAL_PROMPT; break;
                        }
                    }
                }
                if (got) {
                    wake_screen();                      /* any press = user present */
                    if (inp == INPUT_DOWN)        { state = STATE_MENU; menu_sel = 0; speaker_tone(800, 50); }
                    else if (inp == INPUT_CENTER) { speaker_tone(1319, 100); }
                    break;                              /* re-render after a press */
                }
            }
            break;
        }

        /* ════════ MENU ════════ */
        case STATE_MENU: {
            /* Phase 2: the Input task already converts a held UP/DOWN into repeat
             * events and makes CENTER/LEFT strict one-shots, so this loop just
             * DRAINS the event queue and repaints. Because the ~300 ms refresh now
             * runs on core 1, presses are never lost during a repaint. */
            bool was_tall  = orientation_is_tall();
            bool need_draw = true;
            for (;;) {
                if (need_draw) {
                    if (orientation_is_tall()) {
                        render_menu_tall(menu_sel);
                    } else {
                        const Quote *q = &quotes[qi];
                        render_frame(q, mood_cycle(q->mood)[frame_idx % 4], frame_idx);
                        render_menu(menu_sel);
                    }
                    transpose_to_display();
                    display_render();
                    need_draw = false;
                }

                uint8_t inp;
                bool got = ui_get_input(&inp, 200);   /* wake on a press, or re-check rotation */

                if (orientation_is_tall() != was_tall) {   /* rotated → redraw */
                    was_tall = orientation_is_tall(); need_draw = true; continue;
                }
                if (!got) continue;

                /* Fold a run of held UP/DOWN repeats into a SINGLE move so a fast
                 * scroll repaints once at the end (not once per ~120 ms repeat
                 * event, which would lag behind the ~300 ms e-ink refresh). Drains
                 * everything queued right now; a CENTER/LEFT ends the scroll and is
                 * then handled below. */
                if (inp == INPUT_UP || inp == INPUT_DOWN) {
                    int delta = 0;
                    do {
                        if      (inp == INPUT_UP)   delta -= 1;
                        else if (inp == INPUT_DOWN) delta += 1;
                        else break;                       /* CENTER/LEFT terminates the scroll */
                        inp = INPUT_NONE;
                    } while (ui_get_input(&inp, 0));       /* non-blocking drain */
                    if (delta) {
                        menu_sel = ((menu_sel + delta) % MENU_COUNT + MENU_COUNT) % MENU_COUNT;
                        speaker_tone(620, 12);
                        need_draw = true;
                    }
                    if (inp != INPUT_CENTER && inp != INPUT_LEFT) continue;  /* only moves were queued */
                    /* else fall through with inp = CENTER/LEFT */
                }

                if (inp == INPUT_CENTER) {
                    speaker_tone(1000, 40);
                    switch (menu_sel) {
                        case 0: state = STATE_MOOD_SELECT; mood_sel = current_mood + 1; break;
                        case 1: state = STATE_NET_MENU; break;
                        case MENU_IDX_BLUETOOTH: state = STATE_BLUETOOTH; break;
                        case 3: state = STATE_SOUND; snd_sel = 0; break;
                        case 4: state = STATE_MOTION; break;
                        case 5: state = STATE_INFO; break;
                        case MENU_IDX_SET_TIME:
                            rtc_get_datetime(&settime_dt); settime_field = 0;
                            settime_editing = false;
                            state = STATE_SET_TIME; break;
                        case MENU_IDX_SOCIAL: social_sel = 0; state = STATE_SOCIAL; break;
                        case MENU_IDX_DISPLAY: disp_sel = 0; state = STATE_DISPLAY; break;
                        default: state = STATE_OCTOPUS; break;
                    }
                    break;
                }
                if (inp == INPUT_LEFT) {
                    speaker_tone(500, 40); state = STATE_OCTOPUS; break;
                }
            }
            break;
        }

        /* ════════ SET DATE / TIME ════════ */
        /* ════════ DISPLAY SETTINGS (auto-rotate + orientation) ════════ */
        case STATE_DISPLAY: {
            render_display_menu(disp_sel);
            transpose_to_display();
            display_render();
            POLL_INPUT(4000)
                if (inp == INPUT_UP)   { disp_sel = (disp_sel - 1 + DISP_MENU_COUNT) % DISP_MENU_COUNT; speaker_tone(600, 30); break; }
                if (inp == INPUT_DOWN) { disp_sel = (disp_sel + 1) % DISP_MENU_COUNT; speaker_tone(600, 30); break; }
                if (inp == INPUT_LEFT) { state = STATE_MENU; speaker_tone(500, 50); break; }
                if (inp == INPUT_CENTER) {
                    if (disp_sel == DISP_ITEM_AUTO) {
                        /* Toggle auto-rotate. Turning it OFF locks to the current hold. */
                        g_auto_rotate = !g_auto_rotate;
                        if (!g_auto_rotate) g_manual_orient = (uint8_t)g_orientation;
                        speaker_tone(1000, 50);
                    } else if (disp_sel == DISP_ITEM_ORIENT) {
                        /* Cycle WIDE → WIDE FLIP → TALL; picking one locks auto off. */
                        g_auto_rotate = false;
                        g_manual_orient = (uint8_t)(((int)g_manual_orient + 1) % 3);
                        /* Apply immediately so the menu redraws in the new hold. */
                        g_orientation = (int)g_manual_orient;
                        input_set_rotation(ORIENT_CFG[g_orientation].in_rot);
                        speaker_tone(1200, 50);
                    } else {
                        state = STATE_MENU; speaker_tone(500, 50); break;
                    }
                    /* Persist the display settings. */
                    g_saved.auto_rotate = g_auto_rotate ? 1 : 0;
                    g_saved.manual_orient = g_manual_orient;
                    saved_write_flash();
                    break;
                }
            POLL_END
            break;
        }

        case STATE_SET_TIME: {
            render_set_time();
            transpose_to_display();
            display_render();

            POLL_INPUT(4000)
                if (settime_editing) {
                    /* EDIT: C/L is sel/back — confirm the value and drop to NAV. */
                    if (inp == INPUT_UP || inp == INPUT_DOWN) {
                        int d = (inp == INPUT_UP) ? 1 : -1;
                        switch (settime_field) {
                            case 0: settime_dt.year += d; break;
                            case 1: settime_dt.month = (settime_dt.month - 1 + 12 + d) % 12 + 1; break;
                            case 2: settime_dt.day   = (settime_dt.day   - 1 + 31 + d) % 31 + 1; break;
                            case 3: settime_dt.hour  = (settime_dt.hour  + 24 + d) % 24; break;
                            case 4: settime_dt.min   = (settime_dt.min   + 60 + d) % 60; break;
                        }
                        speaker_tone(700, 20); break;
                    }
                    if (inp == INPUT_CENTER || inp == INPUT_LEFT) {
                        settime_editing = false; speaker_tone(600, 30); break;
                    }
                } else {
                    /* NAV: U/D pick a field, C edits it, L commits and goes back. */
                    if (inp == INPUT_UP)   { settime_field = (settime_field + 4) % 5; speaker_tone(600, 30); break; }
                    if (inp == INPUT_DOWN) { settime_field = (settime_field + 1) % 5; speaker_tone(600, 30); break; }
                    if (inp == INPUT_CENTER) { settime_editing = true; speaker_tone(900, 40); break; }
                    if (inp == INPUT_LEFT) {
                        settime_dt.dotw = 0; settime_dt.sec = 0;
                        rtc_set_datetime(&settime_dt);
                        ntp_synced = false;        /* manually set */
                        speaker_tone(1000, 60);
                        state = STATE_MENU; break;
                    }
                }
            POLL_END
            break;
        }

        /* ════════ MOOD SELECT ════════ */
        case STATE_MOOD_SELECT: {
            render_mood_select(mood_sel);
            transpose_to_display();
            display_render();

            POLL_INPUT(4000)
                if (inp == INPUT_UP) {
                    mood_sel = (mood_sel - 1 + MOOD_COUNT + 1) % (MOOD_COUNT + 1);
                    speaker_tone(600, 30); break;
                } else if (inp == INPUT_DOWN) {
                    mood_sel = (mood_sel + 1) % (MOOD_COUNT + 1);
                    speaker_tone(600, 30); break;
                } else if (inp == INPUT_CENTER) {
                    current_mood = mood_sel == 0 ? -1 : mood_sel - 1;
                    qi = pick_quote();
                    frame_idx = 0;  /* reset animation */
                    state = STATE_OCTOPUS;  /* return to octopus */
                    speaker_tone(1200, 80);
                    printf("[mood] Selected: %s → back to octopus\n",
                           current_mood < 0 ? "ALL" : mood_names[current_mood]);
                    break;
                } else if (inp == INPUT_LEFT) {
                    state = STATE_MENU; 
                    speaker_tone(500, 50); break;
                }
            POLL_END
            break;
        }

        /* ════════ NETWORK STATUS (read-only) ════════ */
        case STATE_NETWORK: {
            render_network_screen();
            transpose_to_display();
            display_render();

            POLL_INPUT(4000)
                if (inp == INPUT_LEFT || inp == INPUT_CENTER) {
                    state = STATE_NET_MENU;
                    speaker_tone(500, 50); break;
                }
            POLL_END
            break;
        }

        /* ════════ SOUND SUBMENU ════════ */
        case STATE_SOUND: {
            render_sound_menu(snd_sel);
            transpose_to_display();
            display_render();

            POLL_INPUT(4000)
                if (inp == INPUT_UP) {
                    snd_sel = (snd_sel - 1 + SND_MENU_COUNT) % SND_MENU_COUNT;
                    speaker_tone(800, 50); break;
                }
                if (inp == INPUT_DOWN) {
                    snd_sel = (snd_sel + 1) % SND_MENU_COUNT;
                    speaker_tone(800, 50); break;
                }
                if (inp == INPUT_LEFT) {
                    if (snd_sel == SND_ITEM_PATTERN) {
                        current_pattern = (current_pattern - 1 + SOUND_PATTERN_COUNT) % SOUND_PATTERN_COUNT;
                        speaker_tone(800, 30); break;
                    }
                    state = STATE_MENU;
                    speaker_tone(500, 50); break;
                }
                if (inp == INPUT_RIGHT) {
                    if (snd_sel == SND_ITEM_PATTERN) {
                        current_pattern = (current_pattern + 1) % SOUND_PATTERN_COUNT;
                        speaker_tone(800, 30); break;
                    }
                }
                if (inp == INPUT_CENTER) {
                    switch (snd_sel) {
                        case SND_ITEM_PATTERN:
                            play_sound_pattern(current_pattern);
                            break;
                        case SND_ITEM_ONOFF:
                            sound_enabled = !sound_enabled;
                            if (sound_enabled) speaker_tone(1000, 50);
                            break;
                        case SND_ITEM_VOL:
                            sound_vol = (sound_vol + 1) % 3;
                            speaker_tone(1000, 100);
                            break;
                        case SND_ITEM_BACK:
                            state = STATE_MENU;
                            speaker_tone(500, 50);
                            break;
                    }
                    break;
                }
            POLL_END
            break;
        }

        /* ════════ DEVICE INFO ════════ */
        case STATE_INFO: {
            render_info_screen();
            transpose_to_display();
            display_render();

            POLL_INPUT(4000)
                if (inp == INPUT_LEFT || inp == INPUT_CENTER) {
                    state = STATE_MENU;
                    speaker_tone(500, 50); break;
                }
                if (inp == INPUT_UP) {        /* calibrate: treat current reading as a full 4.20 V pack */
                    /* Trim against the SAME filtered value the icon shows (g_batt_v),
                     * so the cal target matches the displayed estimator. Off-USB
                     * only — on USB the rail isn't the resting pack voltage. */
                    if (g_batt_pct >= 0 && g_batt_v > 2.0f) {
                        battery_cal_save(g_vsys_cal * 4.20f / g_batt_v);
                        speaker_tone(1500, 80);
                    } else {
                        speaker_tone(300, 120);   /* refuse on USB / before first reading */
                    }
                    break;                    /* re-render with the new CAL/% */
                }
                if (inp == INPUT_DOWN) {      /* reset calibration */
                    battery_cal_save(1.0f); speaker_tone(700, 60);
                    break;
                }
            POLL_END
            break;
        }

        /* ════════ NETWORK SUBMENU ════════ */
        case STATE_NET_MENU: {
            static int net_menu_sel = 0;
            render_net_menu(net_menu_sel);
            transpose_to_display();
            display_render();

            POLL_INPUT(4000)
                if (inp == INPUT_UP) {
                    net_menu_sel = (net_menu_sel - 1 + NET_MENU_COUNT) % NET_MENU_COUNT;
                    speaker_tone(600, 30); break;
                }
                if (inp == INPUT_DOWN) {
                    net_menu_sel = (net_menu_sel + 1) % NET_MENU_COUNT;
                    speaker_tone(600, 30); break;
                }
                if (inp == INPUT_LEFT) {
                    state = STATE_MENU;
                    speaker_tone(500, 50); break;
                }
                if (inp == INPUT_CENTER) {
                    speaker_tone(1000, 50);
                    switch (net_menu_sel) {
                        case NET_ITEM_ONOFF:
                            if (wifi_enabled) wifi_disconnect();
                            else wifi_connect();
                            break;
                        case NET_ITEM_SCAN:
                            wifi_start_scan();
                            state = STATE_NET_SCAN; break;
                        case NET_ITEM_SAVED:
                            saved_sel = 0; state = STATE_SAVED_NETS; break;
                        case NET_ITEM_STATUS:
                            state = STATE_NETWORK; break;
                        case NET_ITEM_BACK:
                            state = STATE_MENU; break;
                    }
                    break;
                }
            POLL_END
            break;
        }

        /* ════════ SAVED NETWORKS ════════ */
        case STATE_SAVED_NETS: {
            render_saved_nets();
            transpose_to_display();
            display_render();
            POLL_INPUT(4000)
                if (g_saved.count > 0 && inp == INPUT_UP) {
                    saved_sel = (saved_sel - 1 + g_saved.count) % g_saved.count;
                    speaker_tone(600, 30); break;
                } else if (g_saved.count > 0 && inp == INPUT_DOWN) {
                    saved_sel = (saved_sel + 1) % g_saved.count;
                    speaker_tone(600, 30); break;
                } else if (g_saved.count > 0 && inp == INPUT_CENTER) {
                    speaker_tone(1000, 50);
                    show_connecting_screen(g_saved.nets[saved_sel].ssid);
                    wifi_connect_to(g_saved.nets[saved_sel].ssid,
                                    g_saved.nets[saved_sel].pass);
                    state = STATE_NETWORK; break;
                } else if (g_saved.count > 0 && inp == INPUT_RIGHT) {
                    saved_forget(saved_sel);     /* forget + persist */
                    speaker_tone(400, 60); break;
                } else if (inp == INPUT_LEFT) {
                    state = STATE_NET_MENU; speaker_tone(500, 50); break;
                }
            POLL_END
            break;
        }

        /* ════════ BLUETOOTH ════════ */
        case STATE_BLUETOOTH: {
            if (bt_enabled && !wetgreg_bt_active()) wetgreg_bt_init();    /* resume if left on */
            bt_state_t shown = (bt_state_t)255;
            int  was_tall  = orientation_is_tall();
            bool shown_en  = !bt_enabled;     /* mismatch forces the first render */
            for (;;) {
                if (wetgreg_bt_state() != shown || orientation_is_tall() != was_tall
                        || bt_enabled != shown_en) {
                    shown = wetgreg_bt_state();
                    was_tall = orientation_is_tall();
                    shown_en = bt_enabled;
                    render_bluetooth();
                    transpose_to_display();
                    display_render();
                }
                /* Wake on a press, or every 250 ms to re-check the live BLE state
                 * (the cyw43 background task drives pairing/connection changes). */
                uint8_t inp;
                if (!ui_get_input(&inp, 250)) continue;   /* timeout → re-check state */
                if (inp == INPUT_LEFT) {
                    speaker_tone(500, 40); state = STATE_MENU; break;
                }
                if (inp == INPUT_CENTER) {
                    bt_enabled = !bt_enabled;
                    if (bt_enabled) { wetgreg_bt_init(); speaker_tone(1200, 60); }
                    else {
                        /* radio OFF → save power; social shares the radio, so stop it too */
                        if (g_saved.social_on) wetgreg_set_social(false);
                        wetgreg_social_enable(false);
                        wetgreg_bt_stop(); speaker_tone(600, 60);
                    }
                }
            }
            break;
        }

        /* ════════ SOCIAL MENU ════════ */
        case STATE_SOCIAL: {
            render_social_menu(social_sel);
            transpose_to_display();
            display_render();
            POLL_INPUT(4000)
                if (inp == INPUT_UP)   { social_sel = (social_sel - 1 + SOCIAL_MENU_COUNT) % SOCIAL_MENU_COUNT; speaker_tone(600, 30); break; }
                if (inp == INPUT_DOWN) { social_sel = (social_sel + 1) % SOCIAL_MENU_COUNT; speaker_tone(600, 30); break; }
                if (inp == INPUT_LEFT) { state = STATE_MENU; speaker_tone(500, 50); break; }
                if (inp == INPUT_CENTER) {
                    switch (social_sel) {
                    case SOC_ITEM_SCAN: {                  /* toggle persistent scanning */
                        bool on = !g_saved.social_on;
                        wetgreg_set_social(on);
                        if (on) {
                            if (!wetgreg_bt_active()) wetgreg_bt_init();
                            bt_enabled = true;
                            wetgreg_social_set_self(g_saved.wetgreg_id);
                            wetgreg_social_enable(true);
                            speaker_tone(1200, 60);
                        } else {
                            wetgreg_social_enable(false);
                            speaker_tone(600, 60);
                        }
                        break;
                    }
                    case SOC_ITEM_NEARBY:                  /* live scan for in-range WetGregs */
                        nearby_sel = 0; g_nearby_count = 0;
                        state = STATE_SOCIAL_NEARBY; speaker_tone(1000, 40);
                        break;
                    case SOC_ITEM_MET:                     /* who we've met */
                        met_sel = 0;
                        state = STATE_SOCIAL_MET; speaker_tone(1000, 40);
                        break;
                    case SOC_ITEM_NAME:                    /* set name (reroll picker) */
                        g_name_seed = (uint16_t)rng_next();
                        state = STATE_SOCIAL_NAME; speaker_tone(1000, 40);
                        break;
                    default:                               /* BACK */
                        state = STATE_MENU; speaker_tone(500, 50);
                        break;
                    }
                    break;
                }
            POLL_END
            break;
        }

        /* ════════ SET NAME (reroll) ════════ */
        case STATE_SOCIAL_NAME: {
            render_social_name();
            transpose_to_display();
            display_render();
            POLL_INPUT(8000)
                if (inp == INPUT_UP || inp == INPUT_DOWN) {
                    g_name_seed = (uint16_t)rng_next(); speaker_tone(800, 30); break;
                }
                if (inp == INPUT_CENTER) {
                    char nm[24]; wetgreg_auto_name(g_name_seed, nm, sizeof(nm));
                    wetgreg_set_name(nm); speaker_tone(1400, 80);
                    state = STATE_SOCIAL; break;
                }
                if (inp == INPUT_LEFT) { state = STATE_SOCIAL; speaker_tone(500, 50); break; }
            POLL_END
            break;
        }

        /* ════════ WETGREGS MET (social log) ════════ */
        case STATE_SOCIAL_MET: {
            int n = (int)g_saved.met_count;
            uint16_t ids[SOCIAL_MAX]; uint8_t fl[SOCIAL_MAX];
            for (int i = 0; i < n && i < SOCIAL_MAX; i++) { ids[i] = g_saved.met[i].id; fl[i] = g_saved.met[i].flags; }
            render_wetgreg_list("WETGREGS MET", ids, NULL, fl, n, met_sel,
                               "NONE YET - TURN ON SCAN", "U/D  L:BACK");
            transpose_to_display();
            display_render();
            POLL_INPUT(6000)
                if (n > 0 && inp == INPUT_UP)   { met_sel = (met_sel - 1 + n) % n; speaker_tone(600, 30); break; }
                if (n > 0 && inp == INPUT_DOWN) { met_sel = (met_sel + 1) % n; speaker_tone(600, 30); break; }
                if (inp == INPUT_LEFT)          { state = STATE_SOCIAL; speaker_tone(500, 50); break; }
            POLL_END
            break;
        }

        /* ════════ SCAN NEARBY (live, in-range WetGregs) ════════ */
        case STATE_SOCIAL_NEARBY: {
            /* Make sure scanning is live for the duration of this screen. */
            if (!wetgreg_bt_active()) { wetgreg_bt_init(); bt_enabled = true; }
            wetgreg_social_set_self(g_saved.wetgreg_id);
            wetgreg_social_enable(true);

            int last_count = -1, last_sel = -1, last_tall = -1;
            for (;;) {
                /* Drain freshly-seen WetGregs into the nearby list (unique by id). */
                wetgreg_peer_t pr;
                while (wetgreg_social_poll(&pr)) {
                    int f = -1;
                    for (int i = 0; i < g_nearby_count; i++) if (g_nearby_id[i] == pr.id) { f = i; break; }
                    if (f >= 0) { g_nearby_rssi[f] = pr.rssi; }
                    else if (g_nearby_count < NEARBY_MAX) {
                        g_nearby_id[g_nearby_count] = pr.id;
                        g_nearby_rssi[g_nearby_count] = pr.rssi;
                        g_nearby_count++;
                    }
                    /* NOTE: do NOT met_record() here — that flash-writes (parks the
                     * display core) on every advert seen and would softlock. The log
                     * is updated only when a hello is actually sent/received. */
                }
                int tall_now = orientation_is_tall() ? 1 : 0;
                if (g_nearby_count != last_count || nearby_sel != last_sel || tall_now != last_tall) {
                    if (nearby_sel >= g_nearby_count) nearby_sel = g_nearby_count ? g_nearby_count - 1 : 0;
                    render_wetgreg_list("SCAN NEARBY", g_nearby_id, g_nearby_rssi, NULL,
                                       g_nearby_count, nearby_sel,
                                       "SCANNING... NONE YET", "C:EMOTE  L:BACK");
                    transpose_to_display();
                    display_render();
                    last_count = g_nearby_count; last_sel = nearby_sel; last_tall = tall_now;
                }
                uint8_t inp;
                if (!ui_get_input(&inp, 300)) continue;   /* keep scanning between presses */
                if (inp == INPUT_LEFT) {
                    if (!g_saved.social_on) wetgreg_social_enable(false);   /* stop scan if not opted-in */
                    state = STATE_SOCIAL; speaker_tone(500, 50); break;
                }
                if (g_nearby_count > 0 && inp == INPUT_UP)   { nearby_sel = (nearby_sel - 1 + g_nearby_count) % g_nearby_count; speaker_tone(600, 30); }
                if (g_nearby_count > 0 && inp == INPUT_DOWN) { nearby_sel = (nearby_sel + 1) % g_nearby_count; speaker_tone(600, 30); }
                if (g_nearby_count > 0 && inp == INPUT_CENTER) {
                    g_social_peer = g_nearby_id[nearby_sel];
                    g_social_peer_rssi = g_nearby_rssi[nearby_sel];
                    if (!g_saved.social_on) wetgreg_social_enable(false);  /* stop live scan; advertising stays */
                    g_emote_sel = 0; speaker_tone(1000, 50);
                    state = STATE_EMOTE_PICK; break;
                }
            }
            break;
        }

        /* ════════ "A WETGREG APPEARS — SAY HI?" ════════ */
        case STATE_SOCIAL_PROMPT: {
            render_social_card(false);
            transpose_to_display();
            display_render();
            /* Hold the card for ~2 min so there's real time to react. Poll input
             * AND an incoming hello each second; render only on a change (e-ink
             * holds the image). The peer may leave range mid-window — that's fine,
             * saying YES still logs them and best-effort broadcasts our reply. */
            uint32_t start = to_ms_since_boot(get_absolute_time());
            bool was_tall = orientation_is_tall();
            for (;;) {
                if (orientation_is_tall() != was_tall) {   /* rotated → redraw for new layout */
                    was_tall = !was_tall;
                    render_social_card(false); transpose_to_display(); display_render();
                }
                uint8_t inp;
                if (ui_get_input(&inp, 1000)) {
                    if (inp == INPUT_CENTER) {           /* YES — pick an emote (logs on send) */
                        g_emote_sel = 0; state = STATE_EMOTE_PICK; speaker_tone(1000, 50); break;
                    }
                    if (inp == INPUT_LEFT) {             /* NO — cooldown today */
                        met_record(g_social_peer, 0, wetgreg_today());
                        speaker_tone(500, 50); state = STATE_OCTOPUS; wake_screen(); break;
                    }
                }
                wetgreg_peer_t pr;                        /* did THEY greet us first? */
                if (wetgreg_social_poll(&pr) && pr.hello_to_me && pr.id == g_social_peer) {
                    met_record(pr.id, MET_HELLO_RECV, wetgreg_today());
                    g_social_peer_rssi = pr.rssi;
                    g_play_emote = pr.emote ? pr.emote : EMOTE_WAVE;
                    g_play_incoming = true; g_play_next = STATE_SOCIAL_RECV;
                    speaker_tone(1800, 90); state = STATE_EMOTE_PLAY; break;
                }
                if ((uint32_t)(to_ms_since_boot(get_absolute_time()) - start) >= 120000) {
                    met_record(g_social_peer, 0, wetgreg_today());   /* timed out — cooldown */
                    state = STATE_OCTOPUS; wake_screen(); break;
                }
            }
            break;
        }

        /* ════════ "A WETGREG SAYS HELLO!" — RESPOND? ════════ */
        case STATE_SOCIAL_RECV: {
            render_social_card(true);
            transpose_to_display();
            display_render();
            uint32_t start = to_ms_since_boot(get_absolute_time());
            bool was_tall = orientation_is_tall();
            for (;;) {
                if (orientation_is_tall() != was_tall) {
                    was_tall = !was_tall;
                    render_social_card(true); transpose_to_display(); display_render();
                }
                uint8_t inp;
                if (ui_get_input(&inp, 1000)) {
                    if (inp == INPUT_CENTER) {           /* respond with an emote */
                        g_emote_sel = 0; state = STATE_EMOTE_PICK; speaker_tone(1000, 50); break;
                    }
                    if (inp == INPUT_LEFT) {
                        speaker_tone(500, 50); state = STATE_OCTOPUS; wake_screen(); break;
                    }
                }
                if ((uint32_t)(to_ms_since_boot(get_absolute_time()) - start) >= 120000) {
                    state = STATE_OCTOPUS; wake_screen(); break;   /* ~2 min, then dismiss */
                }
            }
            break;
        }

        /* ════════ EMOTE PICKER (send to g_social_peer) ════════ */
        case STATE_EMOTE_PICK: {
            render_emote_pick(g_emote_sel);
            transpose_to_display();
            display_render();
            POLL_INPUT(15000)
                if (inp == INPUT_UP)   { g_emote_sel = (g_emote_sel - 1 + EMOTE_PICK_COUNT) % EMOTE_PICK_COUNT; speaker_tone(600, 30); break; }
                if (inp == INPUT_DOWN) { g_emote_sel = (g_emote_sel + 1) % EMOTE_PICK_COUNT; speaker_tone(600, 30); break; }
                if (inp == INPUT_LEFT) { state = STATE_OCTOPUS; speaker_tone(500, 50); wake_screen(); break; }
                if (inp == INPUT_CENTER) {
                    uint8_t code = (uint8_t)(g_emote_sel + 1);   /* skip EMOTE_NONE */
                    wetgreg_social_send_emote(g_social_peer, code);
                    met_record(g_social_peer, MET_HELLO_SENT, wetgreg_today());
                    g_play_emote = code; g_play_incoming = false; g_play_next = STATE_OCTOPUS;
                    speaker_tone(1600, 80);
                    state = STATE_EMOTE_PLAY; break;
                }
            POLL_END
            break;
        }

        /* ════════ EMOTE PLAYBACK (octopus acts it out) ════════ */
        case STATE_EMOTE_PLAY: {
            char nm[24]; wetgreg_auto_name(g_social_peer, nm, sizeof(nm));
            char cap[40];
            if (g_play_incoming) snprintf(cap, sizeof(cap), "%s SENT YOU", nm);
            else                 snprintf(cap, sizeof(cap), "TO %s", nm);
            for (uint32_t t = 0; t < 6; t++) {           /* ~6 animation frames */
                render_emote_octopus(g_play_emote, cap, t);
                transpose_to_display();
                display_render();
                uint8_t inp;
                if (ui_get_input(&inp, 350) && inp == INPUT_LEFT) break;   /* skip */
            }
            state = g_play_next ? g_play_next : STATE_OCTOPUS;
            if (state == STATE_OCTOPUS) wake_screen();
            break;
        }

        /* ════════ SCAN RESULTS ════════ */
        case STATE_NET_SCAN: {
            /* Check if scan finished */
            if (scan_in_progress && !cyw43_wifi_scan_active(&cyw43_state)) {
                scan_in_progress = false;
                scan_complete = true;
                wifi_sort_by_rssi();   /* strongest signal first */
                scan_sel = 0;          /* highlight the strongest network */
                printf("[WiFi] Scan complete: %d networks (sorted by signal)\n",
                       scan_count);
            }

            render_scan_results();
            transpose_to_display();
            display_render();

            POLL_INPUT(scan_in_progress ? 500 : 4000)
                if (inp == INPUT_LEFT) {
                    scan_in_progress = false;
                    state = STATE_NET_MENU;
                    speaker_tone(500, 50); break;
                }
                if (scan_complete && scan_count > 0) {
                    if (inp == INPUT_UP) {
                        scan_sel = (scan_sel - 1 + scan_count) % scan_count;
                        speaker_tone(600, 30); break;
                    }
                    if (inp == INPUT_DOWN) {
                        scan_sel = (scan_sel + 1) % scan_count;
                        speaker_tone(600, 30); break;
                    }
                    if (inp == INPUT_CENTER) {
                        speaker_tone(1000, 50);
                        selected_network = scan_sel;
                        const char *saved_pw = saved_find_pass(scan_results[scan_sel].ssid);
                        if (scan_results[scan_sel].auth_mode == 0) {
                            /* Open network — connect directly */
                            show_connecting_screen(scan_results[scan_sel].ssid);
                            wifi_connect_to(scan_results[scan_sel].ssid, "");
                            state = STATE_NETWORK;
                        } else if (saved_pw) {
                            /* Cached credentials — connect with no password entry */
                            show_connecting_screen(scan_results[scan_sel].ssid);
                            wifi_connect_to(scan_results[scan_sel].ssid, saved_pw);
                            state = STATE_NETWORK;
                        } else {
                            /* Needs password — keyboard */
                            pw_len = 0;
                            pw_buf[0] = '\0';
                            kb_row = 0; kb_col = 0; kb_shift = true;
                            state = STATE_NET_KEYBOARD;
                        }
                        break;
                    }
                }
            POLL_END
            break;
        }

        /* ════════ ON-SCREEN KEYBOARD ════════ */
        case STATE_NET_KEYBOARD: {
            render_keyboard();
            transpose_to_display();
            display_render();

            POLL_INPUT(4000)
                if (inp == INPUT_UP) {
                    if (kb_row > 0) kb_row--;
                    if (kb_row < KB_SPECIAL_ROW && kb_col >= 10) kb_col = 9;
                    speaker_tone(600, 20); break;
                }
                if (inp == INPUT_DOWN) {
                    if (kb_row < KB_SPECIAL_ROW) kb_row++;
                    if (kb_row == KB_SPECIAL_ROW && kb_col >= KB_SP_COUNT) kb_col = KB_SP_COUNT - 1;
                    speaker_tone(600, 20); break;
                }
                if (inp == INPUT_LEFT) {
                    if (kb_row < KB_SPECIAL_ROW)
                        kb_col = (kb_col - 1 + 10) % 10;
                    else
                        kb_col = (kb_col - 1 + KB_SP_COUNT) % KB_SP_COUNT;
                    speaker_tone(600, 20); break;
                }
                if (inp == INPUT_RIGHT) {
                    if (kb_row < KB_SPECIAL_ROW)
                        kb_col = (kb_col + 1) % 10;
                    else
                        kb_col = (kb_col + 1) % KB_SP_COUNT;
                    speaker_tone(600, 20); break;
                }
                if (inp == INPUT_CENTER) {
                    if (kb_row < KB_SPECIAL_ROW) {
                        /* Insert character */
                        if (pw_len < PW_MAX_LEN) {
                            pw_buf[pw_len++] = kb_char_at(kb_row, kb_col, kb_shift);
                            pw_buf[pw_len] = '\0';
                            speaker_tone(1000, 30);
                        }
                    } else {
                        /* Special key */
                        switch (kb_col) {
                            case KB_SP_SHIFT:
                                kb_shift = !kb_shift;
                                speaker_tone(800, 30);
                                break;
                            case KB_SP_SPACE:
                                if (pw_len < PW_MAX_LEN) {
                                    pw_buf[pw_len++] = ' ';
                                    pw_buf[pw_len] = '\0';
                                }
                                speaker_tone(1000, 30);
                                break;
                            case KB_SP_DEL:
                                if (pw_len > 0) pw_buf[--pw_len] = '\0';
                                speaker_tone(500, 30);
                                break;
                            case KB_SP_DONE:
                                speaker_tone(1200, 80);
                                show_connecting_screen(scan_results[selected_network].ssid);
                                wifi_connect_to(scan_results[selected_network].ssid, pw_buf);
                                if (wifi_connected)   /* remember it for next time */
                                    saved_add(scan_results[selected_network].ssid, pw_buf);
                                state = STATE_NETWORK;
                                break;
                            case KB_SP_CANCEL:
                                speaker_tone(500, 50);
                                state = STATE_NET_SCAN;
                                break;
                        }
                    }
                    break;
                }
            POLL_END
            break;
        }

        /* ════════ MOTION / ACCELEROMETER ════════ */
        case STATE_MOTION: {
            static int mot_sel = 0;
            render_motion_menu(mot_sel);
            transpose_to_display();
            display_render();

            /* Live-ish refresh for the accel/step readout. This MUST stay above the
             * ~0.7 s panel refresh: at 500 ms the menu resubmitted frames faster than
             * the display could drain, both buffers never went free at once, and the
             * Input task could never emit a press — so LEFT could never exit and the
             * device appeared locked. 2 s keeps the data fresh without saturating. */
            POLL_INPUT(2000)
                if (inp == INPUT_UP) {
                    mot_sel = (mot_sel - 1 + MOT_MENU_COUNT) % MOT_MENU_COUNT;
                    speaker_tone(600, 20); break;
                }
                if (inp == INPUT_DOWN) {
                    mot_sel = (mot_sel + 1) % MOT_MENU_COUNT;
                    speaker_tone(600, 20); break;
                }
                if (inp == INPUT_LEFT) {
                    state = STATE_MENU;
                    speaker_tone(500, 50); break;
                }
                if (inp == INPUT_CENTER) {
                    switch (mot_sel) {
                        case MOT_ITEM_RESET:
                            step_count = 0;
                            speaker_tone(1000, 50);
                            break;
                        case MOT_ITEM_THRESH:
                            step_threshold += 0.1f;
                            if (step_threshold > 2.5f) step_threshold = 0.8f;
                            speaker_tone(800, 30);
                            break;
                        case MOT_ITEM_I2CSCAN:
                            speaker_tone(1000, 50);
                            render_i2c_scan();
                            transpose_to_display();
                            display_render();
                            /* Wait for any button to go back */
                            POLL_INPUT(30000)
                                speaker_tone(500, 30);
                            POLL_END
                            break;
                        case MOT_ITEM_BACK:
                            state = STATE_MENU;
                            speaker_tone(500, 50);
                            break;
                        default:
                            break;
                    }
                    break;
                }
                if (inp == INPUT_RIGHT && mot_sel == MOT_ITEM_THRESH) {
                    step_threshold += 0.1f;
                    if (step_threshold > 2.5f) step_threshold = 0.8f;
                    speaker_tone(800, 30); break;
                }
            POLL_END
            break;
        }

        } /* end switch */
    }

    /* Unreachable (the while loop above never exits). A FreeRTOS task must NEVER
     * "return" off the end — if it somehow did, we delete it cleanly so the
     * kernel reclaims its memory instead of crashing. */
    vTaskDelete(NULL);
}
