/*
 * rtc_compat.h — Cross-platform RTC compatibility for RP2040 / RP2350
 *
 * The RP2040 (Pico W) has a hardware RTC peripheral with rtc_init(),
 * rtc_set_datetime(), and rtc_get_datetime().
 *
 * The RP2350 (Pico 2 W) removed the hardware RTC and the datetime_t
 * type.  This header provides a software fallback that tracks time
 * using the microsecond timer (time_us_64()), available on both.
 *
 * Usage: replace  #include "hardware/rtc.h"  with  #include "rtc_compat.h"
 *        Everything else (rtc_init, rtc_set_datetime, rtc_get_datetime,
 *        datetime_t) works unchanged.
 */

#ifndef RTC_COMPAT_H
#define RTC_COMPAT_H

#include "pico/stdlib.h"

#if PICO_RP2040
/* ── RP2040: use the real hardware RTC + SDK datetime_t ── */
#include "pico/util/datetime.h"
#include "hardware/rtc.h"

#else
/* ── RP2350 (and others): software RTC using time_us_64() ── */

#include <stdint.h>
#include <stdbool.h>

/* datetime_t was removed from the RP2350 SDK — define it here */
typedef struct {
    int16_t year;
    int8_t  month;
    int8_t  day;
    int8_t  dotw;   /* day of the week (0 = Sunday, unused) */
    int8_t  hour;
    int8_t  min;
    int8_t  sec;
} datetime_t;

/* Shared software-RTC state — defined ONCE in rtc_compat.c so every module
 * sees the same clock. The whole clock is a single offset: local-time epoch
 * microseconds minus time_us_64(). Keeping it as one value means a reader can
 * never see half of an update, and the date is derived with real calendar
 * maths, so day/month/year roll over correctly however long it runs. */
extern volatile int64_t _sw_rtc_offset_us;

/* Days since 1970-01-01 for a proleptic Gregorian y/m/d (H. Hinnant). */
static inline int64_t _rtc_days_from_civil(int y, int m, int d) {
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;
    int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static inline void _rtc_civil_from_days(int64_t z, int *y, int *m, int *d) {
    z += 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    int64_t doe = z - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int64_t mp  = (5 * doy + 2) / 153;
    *d = (int)(doy - (153 * mp + 2) / 5 + 1);
    *m = (int)(mp < 10 ? mp + 3 : mp - 9);
    *y = (int)(yoe + era * 400 + (*m <= 2));
}

static inline int64_t _rtc_read_offset(void) {
    /* 64-bit loads aren't single-copy atomic on M33: re-read until stable. */
    int64_t a, b;
    do { a = _sw_rtc_offset_us; b = _sw_rtc_offset_us; } while (a != b);
    return a;
}

static inline void rtc_init(void) {
    /* nothing to do — timer is always running */
}

static inline void rtc_set_datetime(const datetime_t *t) {
    int64_t days = _rtc_days_from_civil(t->year, t->month, t->day);
    int64_t secs = days * 86400 + t->hour * 3600 + t->min * 60 + t->sec;
    _sw_rtc_offset_us = secs * 1000000LL - (int64_t)time_us_64();
}

static inline bool rtc_get_datetime(datetime_t *t) {
    int64_t secs = ((int64_t)time_us_64() + _rtc_read_offset()) / 1000000LL;
    int64_t days = secs / 86400;
    int64_t sod  = secs % 86400;
    int y, m, d;
    _rtc_civil_from_days(days, &y, &m, &d);
    t->year  = (int16_t)y;
    t->month = (int8_t)m;
    t->day   = (int8_t)d;
    t->dotw  = (int8_t)((days + 4) % 7);   /* 1970-01-01 was a Thursday; 0 = Sunday */
    t->hour  = (int8_t)(sod / 3600);
    t->min   = (int8_t)((sod / 60) % 60);
    t->sec   = (int8_t)(sod % 60);
    return true;
}

#endif /* PICO_RP2040 */

#endif /* RTC_COMPAT_H */
