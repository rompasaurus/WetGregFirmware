/*
 * rtc_compat.c — storage for the RP2350 software-RTC fallback.
 *
 * Purpose: hold the single shared copy of the software-RTC base time for
 * every module.
 *
 * rtc_compat.h implements rtc_set/get_datetime() as static inline functions;
 * the base-time state they share must live in exactly ONE translation unit
 * so every module sees the same clock.
 */
#include "rtc_compat.h"

#if !PICO_RP2040
datetime_t _sw_rtc_base;
uint64_t   _sw_rtc_base_us;
#endif
