/*
 * rtc_compat.c — storage for the RP2350 software-RTC fallback.
 *
 * Purpose: hold the single shared copy of the software-RTC offset for
 * every module.
 *
 * rtc_compat.h implements rtc_set/get_datetime() as static inline functions;
 * the clock offset they share must live in exactly ONE translation unit
 * so every module sees the same clock.
 */
#include "rtc_compat.h"

#if !PICO_RP2040
volatile int64_t _sw_rtc_offset_us;
#endif
