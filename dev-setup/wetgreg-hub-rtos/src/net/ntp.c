/**
 * ntp.c — one-shot NTP client over lwIP UDP.
 *
 * Purpose: implement the one-shot NTP exchange (DNS lookup, UDP request, RTC
 * set) over lwIP.
 */
#include "ntp.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "lwip/dns.h"
#include "lwip/pbuf.h"
#include "lwip/udp.h"

#include "rtc_compat.h"
#include "wifi_config.h"   /* NTP_SERVER + TIMEZONE_OFFSET_SEC */

#define NTP_PORT 123
#define NTP_MSG_LEN 48
#define NTP_DELTA 2208988800ULL

bool ntp_synced = false;

static struct udp_pcb *ntp_pcb = NULL;
static ip_addr_t ntp_server_addr;
static volatile bool ntp_time_received = false;

static void ntp_recv_cb(void *arg, struct udp_pcb *pcb, struct pbuf *p,
                         const ip_addr_t *addr, u16_t port) {
    (void)arg; (void)pcb; (void)addr; (void)port;
    if (p->tot_len >= NTP_MSG_LEN) {
        uint8_t *buf = (uint8_t *)p->payload;
        uint32_t secs = (buf[40] << 24) | (buf[41] << 16) | (buf[42] << 8) | buf[43];
        time_t epoch = (time_t)(secs - NTP_DELTA) + TIMEZONE_OFFSET_SEC;
        struct tm *t = gmtime(&epoch);
        datetime_t dt = {
            .year  = (int16_t)(t->tm_year + 1900),
            .month = (int8_t)(t->tm_mon + 1),
            .day   = (int8_t)t->tm_mday,
            .dotw  = (int8_t)t->tm_wday,
            .hour  = (int8_t)t->tm_hour,
            .min   = (int8_t)t->tm_min,
            .sec   = (int8_t)t->tm_sec,
        };
        rtc_set_datetime(&dt);
        ntp_synced = true;
        ntp_time_received = true;
        printf("[NTP] Synced: %04d-%02d-%02d %02d:%02d:%02d\n",
               dt.year, dt.month, dt.day, dt.hour, dt.min, dt.sec);
    }
    pbuf_free(p);
}

static void ntp_dns_cb(const char *name, const ip_addr_t *addr, void *arg) {
    (void)name; (void)arg;
    if (addr) {
        ntp_server_addr = *addr;
        struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, NTP_MSG_LEN, PBUF_RAM);
        if (p) {
            memset(p->payload, 0, NTP_MSG_LEN);
            ((uint8_t *)p->payload)[0] = 0x1b;
            udp_sendto(ntp_pcb, p, &ntp_server_addr, NTP_PORT);
            pbuf_free(p);
            printf("[NTP] Request sent to %s\n", ipaddr_ntoa(addr));
        }
    }
}

void ntp_request(void) {
    if (!ntp_pcb) {
        ntp_pcb = udp_new();
        if (!ntp_pcb) return;
        udp_recv(ntp_pcb, ntp_recv_cb, NULL);
    }
    dns_gethostbyname(NTP_SERVER, &ntp_server_addr, ntp_dns_cb, NULL);
}
