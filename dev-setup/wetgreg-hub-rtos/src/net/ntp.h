/**
 * ntp.h — one-shot NTP time sync (fires after every WiFi connect).
 *
 * Purpose: let the app request a time sync and see whether the clock is
 * network time.
 */
#ifndef NTP_H
#define NTP_H

#include <stdbool.h>

/* True once an NTP response has set the RTC. Cleared by wifi_disconnect()
 * and by a manual time set (the clock is then no longer network time). */
extern bool ntp_synced;

/* Resolve NTP_SERVER (wifi_config.h) and send one request; the RTC is set in
 * the receive callback. Safe to call repeatedly. */
void ntp_request(void);

#endif /* NTP_H */
