/**
 * speaker.h — active-buzzer driver (GP14): tones, sound patterns, settings.
 *
 * Purpose: give the rest of the firmware one tiny API for beeps and jingles,
 * and keep every buzzer pin/timing detail private to the driver.
 *
 * Wire: buzzer (+) → pin 19 (GP14), buzzer (-) → GND (pin 18).
 *
 * The buzzer is a simple on/off active type, so "volume" is really beep
 * DURATION (three levels) and a pattern is a list of (on_ms, off_ms) pairs.
 */
#ifndef SPEAKER_H
#define SPEAKER_H

#include <stdbool.h>
#include <stdint.h>

#define SPEAKER_PATTERN_COUNT 6   /* BEEP / CHIRP / SOS / DOORBELL / ALERT / HAPPY */
#define SPEAKER_VOL_LEVELS    3   /* 0=short, 1=med, 2=long beep duration */

void speaker_init(void);
void speaker_tone(uint16_t freq_hz, uint16_t duration_ms);
void play_sound_pattern(int idx);

/* Sound settings (driven by the Sound menu; not persisted). */
bool        speaker_enabled(void);
void        speaker_set_enabled(bool on);
uint8_t     speaker_volume(void);                 /* 0..SPEAKER_VOL_LEVELS-1 */
void        speaker_set_volume(uint8_t vol);
int         speaker_pattern(void);                /* currently selected pattern */
void        speaker_set_pattern(int idx);
const char *speaker_pattern_name(int idx);

#endif /* SPEAKER_H */
