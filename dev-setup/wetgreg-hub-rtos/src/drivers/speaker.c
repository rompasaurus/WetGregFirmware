/**
 * speaker.c — active-buzzer driver (GP14).
 *
 * Purpose: implement the buzzer driver: pin control, tone/pattern timing,
 * and the private sound settings behind the accessors in speaker.h.
 *
 * NOTE: speaker_tone()/play_sound_pattern() busy-sleep for the tone duration;
 * they are called only from the UI task, where a short blocking beep is fine.
 */
#include "speaker.h"

#include "pico/stdlib.h"
#include "hardware/gpio.h"

/* ─── Speaker (active buzzer on GP14) ─── */
#define BUZZER_PIN 14  /* GP14 = pin 19 — active buzzer (+) */

/* ─── Sound patterns (on_ms, off_ms pairs; 0 terminates) ─── */
static const uint16_t pat_beep[]     = {150, 0};
static const uint16_t pat_chirp[]    = {30,30, 30,30, 30,30, 30,30, 30, 0};
static const uint16_t pat_sos[]      = {60,60, 60,60, 60,180, 180,60, 180,60, 180,180, 60,60, 60,60, 60, 0};
static const uint16_t pat_doorbell[] = {200, 100, 300, 0};
static const uint16_t pat_alert[]    = {300,200, 300,200, 300, 0};
static const uint16_t pat_happy[]    = {40,60, 40,60, 40,60, 200,100, 50,50, 50,50, 300, 0};

static const char *pattern_names[] = {"BEEP", "CHIRP", "SOS", "DOORBELL", "ALERT", "HAPPY"};
static const uint16_t *patterns[]  = {pat_beep, pat_chirp, pat_sos, pat_doorbell, pat_alert, pat_happy};

static const uint16_t vol_durations[] = {20, 50, 100};  /* ms per level */

static bool    sound_enabled   = true;
static uint8_t sound_vol       = 2;   /* 0=short, 1=med, 2=long beep duration */
static int     current_pattern = 0;

void speaker_init(void) {
    gpio_init(BUZZER_PIN);
    gpio_set_dir(BUZZER_PIN, GPIO_OUT);
    gpio_put(BUZZER_PIN, 0);
}

void speaker_tone(uint16_t freq_hz, uint16_t duration_ms) {
    if (!sound_enabled || freq_hz == 0) { sleep_ms(duration_ms); return; }
    uint16_t actual = duration_ms < vol_durations[sound_vol] ? duration_ms : vol_durations[sound_vol];
    gpio_put(BUZZER_PIN, 1);
    sleep_ms(actual);
    gpio_put(BUZZER_PIN, 0);
    if (duration_ms > actual) sleep_ms(duration_ms - actual);
}

void play_sound_pattern(int idx) {
    if (!sound_enabled || idx < 0 || idx >= SPEAKER_PATTERN_COUNT) return;
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

bool speaker_enabled(void)            { return sound_enabled; }
void speaker_set_enabled(bool on)     { sound_enabled = on; }

uint8_t speaker_volume(void)          { return sound_vol; }
void speaker_set_volume(uint8_t vol)  { if (vol < SPEAKER_VOL_LEVELS) sound_vol = vol; }

int  speaker_pattern(void)            { return current_pattern; }
void speaker_set_pattern(int idx) {
    if (idx >= 0 && idx < SPEAKER_PATTERN_COUNT) current_pattern = idx;
}

const char *speaker_pattern_name(int idx) {
    if (idx < 0 || idx >= SPEAKER_PATTERN_COUNT) return "?";
    return pattern_names[idx];
}
