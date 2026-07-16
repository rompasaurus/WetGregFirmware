/**
 * wifi.h — Wi-Fi: scanning, connect/disconnect, and the post-wake
 * background rejoin.
 *
 * Purpose: expose WiFi state, scanning, joining, and the deferred post-wake
 * rejoin to the app and screens.
 */
#ifndef WIFI_H
#define WIFI_H

#include <stdbool.h>
#include <stdint.h>

/* ─── Connection state (written by wifi.c, read by screens/app) ─── */
extern bool    wifi_enabled;
extern bool    wifi_connected;
extern int32_t wifi_rssi;
extern char    wifi_ssid_display[33];
extern char    wifi_ip_str[20];

/* ─── Scan state ─── */
#define MAX_SCAN_RESULTS 16
typedef struct { char ssid[33]; int8_t rssi; uint8_t auth_mode; } scan_entry_t;
extern scan_entry_t scan_results[MAX_SCAN_RESULTS];
extern int  scan_count;
extern int  scan_sel;           /* UI selection in the results list */
extern bool scan_in_progress;
extern bool scan_complete;

void wifi_start_scan(void);
void wifi_scan_poll(void);      /* finalize (sort + log) once the async scan ends */

/* Blocking join (~up to 20 s): association + DHCP, then an NTP request. */
void wifi_connect_to(const char *ssid, const char *password);
void wifi_connect(void);        /* compile-time WIFI_SSID/WIFI_PASS */
void wifi_disconnect(void);

/* ─── Post-wake background rejoin (see power_sleep_exit) ─────────────────────
 * An async join + cheap polling from the octopus loop, so waking never blocks
 * ~20 s on DHCP like wifi_connect_to. The join is DEFERRED by `delay_ms` so
 * the WiFi scan/join doesn't pile onto the BLE restart the moment the user
 * wakes the device (that combined radio burst starved Housekeeping's sensor
 * sampling — frozen auto-rotate, field bug). */
void wifi_schedule_rejoin(const char *ssid, uint32_t delay_ms);
void wifi_rejoin_cancel(void);  /* drop any deferred or in-flight rejoin */
bool wifi_rejoin_pending(void); /* deferred or in-flight rejoin exists */
void wifi_rejoin_tick(void);    /* octopus-loop hook: fire deferred join + poll it */

#endif /* WIFI_H */
