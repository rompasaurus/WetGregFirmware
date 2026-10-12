/**
 * rtc_clock.h — RTC bring-up (compile-time seed) + the month-name table
 * shared by every date-rendering screen.
 *
 * Purpose: give date-rendering screens the month-name table and boot code
 * the compile-time RTC seed, and accept the time pushed from a computer
 * over USB serial.
 */
#ifndef RTC_CLOCK_H
#define RTC_CLOCK_H


extern const char *const month_names[12];

/* Seed the RTC from __DATE__/__TIME__ so the clock is plausible before the
 * first NTP sync (or manual set). */
void init_rtc_from_compile_time(void);

/* Non-blocking: consume any USB-serial input and apply a complete
 *   TIME YYYY-MM-DD HH:MM:SS
 * line (local time). Replies "TIME OK ..." or "TIME ERR". Call periodically
 * from a task that may stall briefly on the stdio lock. */
void clock_serial_poll(void);

#endif /* RTC_CLOCK_H */
