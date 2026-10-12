/**
 * rtc_clock.c — RTC bring-up, reset survival, and the USB time push/query.
 *
 * Purpose: seed the RTC so the clock is plausible before NTP or a manual set,
 * keep it across reboots, and take the real time from a USB serial push.
 *
 * There is no battery-backed RTC on the RP2350: the clock is an offset on the
 * microsecond timer, and that timer restarts from zero on EVERY reset — a
 * brown-out on a sagging battery, the watchdog, a crash, a picotool reflash.
 * Without help the clock would snap back to the firmware's build time each
 * time, and a shelf of Gregs would each show a different wrong time after a
 * night of random resets. So the housekeeping task checkpoints the current
 * time into RAM that the C runtime never zeroes (.uninitialized_data); at boot
 * a valid checkpoint wins over the build time. A true power-off leaves that
 * RAM as garbage, which the magic + checksum reject, and we fall back to the
 * build time as before.
 */
#include "rtc_clock.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rtc_compat.h"

#if !PICO_RP2040
#include "hardware/structs/powman.h"   /* powman_hw->chip_reset: why we booted */
#endif

const char *const month_names[12] = {
    "JANUARY","FEBRUARY","MARCH","APRIL","MAY","JUNE",
    "JULY","AUGUST","SEPTEMBER","OCTOBER","NOVEMBER","DECEMBER"
};

/* ─── Reset-surviving clock checkpoint (RP2350 software RTC only) ─── */
#if !PICO_RP2040
#define CKPT_MAGIC 0x57474331u   /* "WGC1" — bump if the layout changes */

typedef struct {
    uint32_t magic;
    uint32_t boots;      /* warm resets survived since the last cold power-up */
    int64_t  epoch_us;   /* local-time epoch µs at the last checkpoint */
    uint32_t sum;        /* checksum over the fields above */
} clock_ckpt_t;

/* Lives in .uninitialized_data: crt0 does not clear it, so it rides through
 * any reset that keeps SRAM powered. */
static clock_ckpt_t __uninitialized_ram(g_ckpt);

static uint32_t g_reset_flags = 0;   /* powman CHIP_RESET as read at boot */
static bool     g_restored    = false;

static uint32_t ckpt_sum(const clock_ckpt_t *c) {
    return ~(c->magic ^ (c->boots * 0x9E3779B9u)
             ^ (uint32_t)c->epoch_us ^ (uint32_t)((uint64_t)c->epoch_us >> 32));
}

static int64_t civil_to_epoch_us(const datetime_t *t) {
    int64_t days = _rtc_days_from_civil(t->year, t->month, t->day);
    return (days * 86400 + t->hour * 3600 + t->min * 60 + t->sec) * 1000000LL;
}

static int64_t now_epoch_us(void) {
    return (int64_t)time_us_64() + _rtc_read_offset();
}

void clock_checkpoint(void) {
    g_ckpt.epoch_us = now_epoch_us();
    g_ckpt.sum      = ckpt_sum(&g_ckpt);
}

/* Decode the HAD_* bits of powman CHIP_RESET into a short label list. */
static void reset_reason_str(uint32_t f, char *out, size_t n) {
    static const struct { uint32_t bit; const char *name; } tab[] = {
        { POWMAN_CHIP_RESET_HAD_POR_BITS,                    "POWER-ON" },
        { POWMAN_CHIP_RESET_HAD_BOR_BITS,                    "BROWN-OUT" },
        { POWMAN_CHIP_RESET_HAD_RUN_LOW_BITS,                "RUN-PIN" },
        { POWMAN_CHIP_RESET_HAD_DP_RESET_REQ_BITS,           "DEBUGGER" },
        { POWMAN_CHIP_RESET_HAD_RESCUE_BITS,                 "RESCUE" },
        { POWMAN_CHIP_RESET_HAD_WATCHDOG_RESET_SWCORE_BITS,  "WATCHDOG" },
        { POWMAN_CHIP_RESET_HAD_WATCHDOG_RESET_POWMAN_BITS,  "WATCHDOG-POWMAN" },
        { POWMAN_CHIP_RESET_HAD_WATCHDOG_RESET_RSM_BITS,     "WATCHDOG-RSM" },
        { POWMAN_CHIP_RESET_HAD_HZD_SYS_RESET_REQ_BITS,      "SYSRESETREQ" },
        { POWMAN_CHIP_RESET_HAD_GLITCH_DETECT_BITS,          "GLITCH" },
        { POWMAN_CHIP_RESET_HAD_SWCORE_PD_BITS,              "CORE-POWERDOWN" },
    };
    out[0] = '\0';
    for (size_t i = 0; i < sizeof(tab) / sizeof(tab[0]); i++) {
        if (!(f & tab[i].bit)) continue;
        if (out[0]) strncat(out, "+", n - strlen(out) - 1);
        strncat(out, tab[i].name, n - strlen(out) - 1);
    }
    if (!out[0]) snprintf(out, n, "UNKNOWN");
}
#endif /* !PICO_RP2040 */

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

#if !PICO_RP2040
    g_reset_flags = powman_hw->chip_reset;

    /* A checkpoint is trusted only if it is intact AND later than the build
     * time (a clock that was never set is worth nothing) AND not absurdly far
     * in the future (garbage that happens to pass the checksum). */
    const int64_t build_us = civil_to_epoch_us(&dt);
    const int64_t far_us   = build_us + 20LL * 366 * 86400 * 1000000LL;
    if (g_ckpt.magic == CKPT_MAGIC && g_ckpt.sum == ckpt_sum(&g_ckpt)
        && g_ckpt.epoch_us > build_us && g_ckpt.epoch_us < far_us) {
        _sw_rtc_offset_us = g_ckpt.epoch_us - (int64_t)time_us_64();
        g_ckpt.boots++;
        g_restored = true;
    } else {
        g_ckpt.magic = CKPT_MAGIC;
        g_ckpt.boots = 0;
    }
    clock_checkpoint();

    char why[96];
    reset_reason_str(g_reset_flags, why, sizeof(why));
    rtc_get_datetime(&dt);
    printf("RTC %s %04d-%02d-%02d %02d:%02d:%02d | reset: %s (0x%08lx) | warm boots: %lu\n",
           g_restored ? "restored from checkpoint:" : "set to build time:",
           dt.year, dt.month, dt.day, dt.hour, dt.min, dt.sec,
           why, (unsigned long)g_reset_flags, (unsigned long)g_ckpt.boots);
#else
    printf("RTC set to %04d-%02d-%02d %02d:%02d:%02d\n",
           dt.year, dt.month, dt.day, dt.hour, dt.min, dt.sec);
#endif
}

/* ─── USB serial time push / query (sent by flash-and-sync tooling) ─── */
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

static void print_time_now(void) {
    datetime_t dt;
    rtc_get_datetime(&dt);
    uint64_t up_s = time_us_64() / 1000000ULL;
#if !PICO_RP2040
    char why[96];
    reset_reason_str(g_reset_flags, why, sizeof(why));
    printf("TIME NOW %04d-%02d-%02d %02d:%02d:%02d UP %lluh%02llum%02llus BOOTS %lu RESET %s SRC %s\n",
           dt.year, dt.month, dt.day, dt.hour, dt.min, dt.sec,
           (unsigned long long)(up_s / 3600), (unsigned long long)((up_s / 60) % 60),
           (unsigned long long)(up_s % 60), (unsigned long)g_ckpt.boots, why,
           g_restored ? "CHECKPOINT" : "BUILD-TIME");
#else
    printf("TIME NOW %04d-%02d-%02d %02d:%02d:%02d UP %lluh%02llum%02llus\n",
           dt.year, dt.month, dt.day, dt.hour, dt.min, dt.sec,
           (unsigned long long)(up_s / 3600), (unsigned long long)((up_s / 60) % 60),
           (unsigned long long)(up_s % 60));
#endif
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
            if (strcmp(line, "TIME?") == 0) {
                print_time_now();
            } else if (parse_time_line(line, &dt)) {
                rtc_set_datetime(&dt);
#if !PICO_RP2040
                clock_checkpoint();
#endif
                printf("TIME OK %04d-%02d-%02d %02d:%02d:%02d\n",
                       dt.year, dt.month, dt.day, dt.hour, dt.min, dt.sec);
            } else if (line[0] == 'T' && line[1] == 'I') {
                printf("TIME ERR (want: TIME YYYY-MM-DD HH:MM:SS, or TIME?)\n");
            }
        } else if (len < (int)sizeof(line) - 1) {
            line[len++] = (char)c;
        } else {
            len = 0;   /* overlong garbage — drop it */
        }
    }
}
