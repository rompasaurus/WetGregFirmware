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

static datetime_t _sw_rtc_base;
static uint64_t   _sw_rtc_base_us;

static inline void rtc_init(void) {
    /* nothing to do — timer is always running */
}

static inline void rtc_set_datetime(const datetime_t *t) {
    _sw_rtc_base    = *t;
    _sw_rtc_base_us = time_us_64();
}

static inline bool rtc_get_datetime(datetime_t *t) {
    /* Seconds elapsed since rtc_set_datetime was called */
    uint64_t elapsed_s = (time_us_64() - _sw_rtc_base_us) / 1000000ULL;

    /* Start from the base time and add elapsed seconds */
    *t = _sw_rtc_base;

    /* Roll up seconds -> minutes -> hours -> days (simplified) */
    int sec  = t->sec  + (int)(elapsed_s % 60);
    int carry = sec / 60;
    t->sec   = (int8_t)(sec % 60);

    int min  = t->min  + (int)((elapsed_s / 60) % 60) + carry;
    carry    = min / 60;
    t->min   = (int8_t)(min % 60);

    int hr   = t->hour + (int)((elapsed_s / 3600) % 24) + carry;
    carry    = hr / 24;
    t->hour  = (int8_t)(hr % 24);

    t->day   = (int8_t)(t->day + carry);  /* simplified — no month rollover */

    return true;
}

#endif /* PICO_RP2040 */

#endif /* RTC_COMPAT_H */
