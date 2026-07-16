/**
 * rtc_clock.h — RTC bring-up (compile-time seed) + the month-name table
 * shared by every date-rendering screen.
 *
 * Purpose: give date-rendering screens the month-name table and boot code
 * the compile-time RTC seed.
 */
#ifndef RTC_CLOCK_H
#define RTC_CLOCK_H

extern const char *const month_names[12];

/* Seed the RTC from __DATE__/__TIME__ so the clock is plausible before the
 * first NTP sync (or manual set). */
void init_rtc_from_compile_time(void);

#endif /* RTC_CLOCK_H */
