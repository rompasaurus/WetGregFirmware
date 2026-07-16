/**
 * screens_anim.h — full-screen animations: the boot splash (STATE_SPLASH),
 * the "who is Greg" intro story (STATE_INTRO), and the sleep still
 * (STATE_SLEEP).
 *
 * Purpose: render the full-screen animations (splash, intro, sleep still)
 * and export their timing constants to the app.
 */
#ifndef SCREENS_ANIM_H
#define SCREENS_ANIM_H

#include <stdint.h>

/* Splash timing (the app's STATE_SPLASH loop drives these). */
#define SPLASH_TOTAL_MS   10000
#define SPLASH_FRAME_MS     450   /* per-frame input wait — stays above the panel drain */

/* Intro timing. */
#define INTRO_TOTAL_MS    38000   /* text done ~32.6 s + idle swim, then auto-exit */
#define INTRO_FRAME_MS      450   /* per-frame input wait — above the panel drain */

/* Re-scatter the rising bubbles (call at splash/intro/sleep entry). */
void splash_bubbles_init(void);

void render_splash(uint32_t elapsed, uint32_t frame_idx);
void render_intro(uint32_t elapsed, uint32_t frame_idx);
void render_sleep_screen(void);

#endif /* SCREENS_ANIM_H */
