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

#include <stdint.h>

extern const char *const month_names[12];

/* Seed the RTC from __DATE__/__TIME__ so the clock is plausible before the
 * first NTP sync (or manual set). */
void init_rtc_from_compile_time(void);

/* Non-blocking: consume any USB-serial input and apply a complete
 *   TIME YYYY-MM-DD HH:MM:SS
 * line (local time). Replies "TIME OK ..." or "TIME ERR". "TIME?" answers
 * with the current clock, uptime, warm-boot count and last reset reason.
 * Call periodically from a task that may stall briefly on the stdio lock. */
void clock_serial_poll(void);

/* Save the current time into reset-surviving RAM so a brown-out / watchdog /
 * crash reboot resumes the clock instead of snapping back to the build time.
 * Cheap (a few stores); call it every housekeeping tick. */
#if PICO_RP2040
static inline void clock_checkpoint(void) { }   /* hardware RTC keeps itself */
static inline uint32_t clock_epoch_s(void) { return 0; }
static inline void clock_aon_save(void) { }
#else
void clock_checkpoint(void);

/* Local-time epoch seconds (the same timebase the RTC shows). */
uint32_t clock_epoch_s(void);

/* Deep-nap entry: load the wall clock into the always-on POWMAN timer, which
 * keeps counting while the switched core is powered off. The boot after the
 * nap reads it back (init_rtc_from_compile_time, when g_boot_kind says so). */
void clock_aon_save(void);
#endif

#endif /* RTC_CLOCK_H */
