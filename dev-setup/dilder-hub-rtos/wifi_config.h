#ifndef WIFI_CONFIG_H
#define WIFI_CONFIG_H

/*
 * WiFi credentials — edit these to match your network.
 *
 * For security, you can also pass them via cmake:
 *   cmake -DWIFI_SSID="YourNetwork" -DWIFI_PASS="YourPassword" ..
 */

#ifndef WIFI_SSID
#define WIFI_SSID  "YOUR_WIFI_SSID"
#endif

#ifndef WIFI_PASS
#define WIFI_PASS  "YOUR_WIFI_PASSWORD"
#endif

/* NTP server — pool.ntp.org is the standard public NTP pool */
#define NTP_SERVER "pool.ntp.org"

/* Your timezone offset from UTC in seconds.
 * Examples:
 *   UTC+0  (London winter)  =  0
 *   UTC+1  (Berlin winter)  =  3600
 *   UTC+2  (Berlin summer)  =  7200
 *   UTC-5  (New York winter)= -18000
 *   UTC-4  (New York summer)= -14400
 */
#define TIMEZONE_OFFSET_SEC  7200   /* UTC+2 (CEST — Central European Summer Time) */

#endif
