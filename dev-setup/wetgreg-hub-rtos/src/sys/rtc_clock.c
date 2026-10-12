/**
 * rtc_clock.c — RTC bring-up from the compile-time date/time.
 *
 * Purpose: seed the RTC from __DATE__/__TIME__ so the clock is plausible
 * before NTP or a manual set, and take the real time from a USB serial push.
 */
#include "rtc_clock.h"

#include <stdio.h>
#include <stdlib.h>

#include "rtc_compat.h"

const char *const month_names[12] = {
    "JANUARY","FEBRUARY","MARCH","APRIL","MAY","JUNE",
    "JULY","AUGUST","SEPTEMBER","OCTOBER","NOVEMBER","DECEMBER"
};

/* ─── Parse compile-time date/time to seed the RTC ─── */
static int parse_month(const char *s) {
    static const char *m[] = {"Jan","Feb","Mar","Apr","May","Jun",
                              "Jul","Aug","Sep","Oct","Nov","Dec"};
    for (int i = 0; i < 12; i++)
        if (s[0] == m[i][0] && s[1] == m[i][1] && s[2] == m[i][2])
            return i + 1;
    return 1;
}

void init_rtc_from_compile_time(void) {
    /* __DATE__ = "Apr 12 2026", __TIME__ = "19:05:15" */
    const char *d = __DATE__;
    const char *t = __TIME__;

    datetime_t dt = {
        .year  = (int16_t)(atoi(d + 7)),
        .month = (int8_t)parse_month(d),
        .day   = (int8_t)atoi(d + 4),
        .dotw  = 0,  /* RTC doesn't need accurate day-of-week */
        .hour  = (int8_t)atoi(t),
        .min   = (int8_t)atoi(t + 3),
        .sec   = (int8_t)atoi(t + 6),
    };

    rtc_init();
    rtc_set_datetime(&dt);
    sleep_us(64);  /* wait for RTC to latch */
    printf("RTC set to %04d-%02d-%02d %02d:%02d:%02d\n",
           dt.year, dt.month, dt.day, dt.hour, dt.min, dt.sec);
}

/* ─── USB serial time push (sent by flash-and-sync tooling) ─── */
static bool parse_time_line(const char *s, datetime_t *dt) {
    int y, mo, d, h, mi, se;
    if (sscanf(s, "TIME %d-%d-%d %d:%d:%d", &y, &mo, &d, &h, &mi, &se) != 6)
        return false;
    if (y < 2020 || y > 2099 || mo < 1 || mo > 12 || d < 1 || d > 31
        || h < 0 || h > 23 || mi < 0 || mi > 59 || se < 0 || se > 59)
        return false;
    *dt = (datetime_t){ .year = (int16_t)y, .month = (int8_t)mo, .day = (int8_t)d,
                        .dotw = 0, .hour = (int8_t)h, .min = (int8_t)mi, .sec = (int8_t)se };
    return true;
}

void clock_serial_poll(void) {
    static char line[40];
    static int  len = 0;
    int c;
    while ((c = getchar_timeout_us(0)) != PICO_ERROR_TIMEOUT) {
        if (c == '\r' || c == '\n') {
            if (len == 0) continue;
            line[len] = '\0';
            len = 0;
            datetime_t dt;
            if (parse_time_line(line, &dt)) {
                rtc_set_datetime(&dt);
                printf("TIME OK %04d-%02d-%02d %02d:%02d:%02d\n",
                       dt.year, dt.month, dt.day, dt.hour, dt.min, dt.sec);
            } else if (line[0] == 'T' && line[1] == 'I') {
                printf("TIME ERR (want: TIME YYYY-MM-DD HH:MM:SS)\n");
            }
        } else if (len < (int)sizeof(line) - 1) {
            line[len++] = (char)c;
        } else {
            len = 0;   /* overlong garbage — drop it */
        }
    }
}
