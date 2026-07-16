/**
 * wifi.c — Wi-Fi scanning, joining, and the post-wake background rejoin.
 *
 * Purpose: own all cyw43 WiFi calls: the scan callback/sorting, the polled
 * async join, disconnect, and the background rejoin machinery.
 */
#include "wifi.h"

#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"

#include "ntp.h"
#include "storage.h"       /* saved-network password lookup for the rejoin */
#include "wifi_config.h"   /* compile-time WIFI_SSID / WIFI_PASS */

/* ─── WiFi state ─── */
bool    wifi_enabled   = false;
bool    wifi_connected = false;
int32_t wifi_rssi      = 0;
char    wifi_ssid_display[33] = "---";
char    wifi_ip_str[20]       = "---";

/* ─── WiFi scan state ─── */
scan_entry_t scan_results[MAX_SCAN_RESULTS];
int  scan_count       = 0;
int  scan_sel         = 0;
bool scan_in_progress = false;
bool scan_complete    = false;

/* ─── Background-rejoin state ─── */
static const char *g_rejoin_pass = NULL;      /* saved-store key the rejoin is using */
static uint32_t g_wifi_rejoin_deadline = 0;   /* 0 = no background rejoin in flight */
static uint32_t g_rejoin_start_ms = 0;        /* deferred-join fire time; 0 = none */
static char     g_rejoin_pending[33] = "";    /* ssid waiting for the deferred join */
static char     g_rejoin_ssid[33] = "";       /* ssid of the join in flight */

/* ─── WiFi scan ─── */
static int wifi_scan_callback(void *env, const cyw43_ev_scan_result_t *result) {
    (void)env;
    if (!result || scan_count >= MAX_SCAN_RESULTS) return 0;
    if (result->ssid_len == 0) return 0;  /* skip hidden networks */

    /* Deduplicate — keep the one with better RSSI */
    for (int i = 0; i < scan_count; i++) {
        if (strncmp(scan_results[i].ssid, (const char *)result->ssid, result->ssid_len) == 0
            && strlen(scan_results[i].ssid) == result->ssid_len) {
            if (result->rssi > scan_results[i].rssi) {
                scan_results[i].rssi = (int8_t)result->rssi;
                scan_results[i].auth_mode = result->auth_mode;
            }
            return 0;
        }
    }

    memcpy(scan_results[scan_count].ssid, result->ssid, result->ssid_len);
    scan_results[scan_count].ssid[result->ssid_len] = '\0';
    scan_results[scan_count].rssi = (int8_t)result->rssi;
    scan_results[scan_count].auth_mode = result->auth_mode;
    scan_count++;
    return 0;
}

/* Sort discovered networks by signal strength, strongest first. RSSI is in
 * dBm (negative), so a larger value = stronger. Simple insertion sort — the
 * list is tiny (<= MAX_SCAN_RESULTS). */
static void wifi_sort_by_rssi(void) {
    for (int i = 1; i < scan_count; i++) {
        scan_entry_t key = scan_results[i];
        int j = i - 1;
        while (j >= 0 && scan_results[j].rssi < key.rssi) {
            scan_results[j + 1] = scan_results[j];
            j--;
        }
        scan_results[j + 1] = key;
    }
}

void wifi_start_scan(void) {
    scan_count = 0;
    scan_sel = 0;
    scan_in_progress = true;
    scan_complete = false;
    cyw43_arch_enable_sta_mode();
    cyw43_wifi_scan_options_t opts = {0};
    cyw43_wifi_scan(&cyw43_state, &opts, NULL, wifi_scan_callback);
    printf("[WiFi] Scan started\n");
}

/* Called from the scan-results screen loop: once the async scan has finished,
 * sort strongest-first and mark the results usable. */
void wifi_scan_poll(void) {
    if (scan_in_progress && !cyw43_wifi_scan_active(&cyw43_state)) {
        scan_in_progress = false;
        scan_complete = true;
        wifi_sort_by_rssi();   /* strongest signal first */
        scan_sel = 0;          /* highlight the strongest network */
        printf("[WiFi] Scan complete: %d networks (sorted by signal)\n",
               scan_count);
    }
}

/* ─── WiFi connect (accepts arbitrary SSID/password) ─── */
void wifi_connect_to(const char *ssid, const char *password) {
    printf("[WiFi] Connecting to \"%s\"...\n", ssid);
    strncpy(wifi_ssid_display, ssid, sizeof(wifi_ssid_display) - 1);
    wifi_ssid_display[sizeof(wifi_ssid_display) - 1] = '\0';
    wifi_enabled = true;

    cyw43_arch_enable_sta_mode();

    uint32_t auth = (password[0] != '\0') ? CYW43_AUTH_WPA2_AES_PSK : 0;

    /* Drive the join ourselves instead of cyw43_arch_wifi_connect_timeout_ms().
     *
     * WHY: under pico_cyw43_arch_lwip_sys_freertos (SMP), that SDK helper's wait
     * loop calls cyw43_arch_wait_for_work_until(<full deadline>) — and in this
     * config a link/DHCP-completion event does NOT reliably wake that wait. So it
     * sleeps the entire timeout, then its own time_reached() check trips and it
     * returns PICO_ERROR_TIMEOUT *even though association + DHCP had succeeded*.
     * Net effect: scanning worked but every connect "failed".
     *
     * FIX: start the join async, then poll cyw43_tcpip_link_status() ~4x/sec so we
     * see the LINK_UP transition (DHCP done) immediately. Link states:
     *   0=DOWN 1=JOIN 2=NOIP 3=UP ; negative: -1=FAIL -2=NONET -3=BADAUTH. */
    int err = cyw43_arch_wifi_connect_async(ssid, password, auth);
    if (err) {
        printf("[WiFi] connect_async start failed (err=%d)\n", err);
        wifi_connected = false;
        return;
    }

    int last_t = -999;
    absolute_time_t deadline = make_timeout_time_ms(20000);
    for (;;) {
        int ts = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);
        if (ts != last_t) {
            printf("[WiFi] link tcpip=%d\n", ts);
            last_t = ts;
        }
        if (ts == CYW43_LINK_UP) break;          /* associated + DHCP IP assigned */
        if (ts == CYW43_LINK_BADAUTH) {
            printf("[WiFi] bad auth — wrong password\n");
            wifi_connected = false;
            return;
        }
        if (ts == CYW43_LINK_FAIL) {
            printf("[WiFi] association failed\n");
            wifi_connected = false;
            return;
        }
        /* CYW43_LINK_NONET = AP not seen this round; re-issue the join (mirrors the
         * SDK) and keep polling until the deadline. */
        if (ts == CYW43_LINK_NONET)
            cyw43_arch_wifi_connect_async(ssid, password, auth);
        if (time_reached(deadline)) {
            printf("[WiFi] timeout (last tcpip=%d)\n", last_t);
            wifi_connected = false;
            return;
        }
        cyw43_arch_wait_for_work_until(make_timeout_time_ms(250));
    }

    wifi_connected = true;
    struct netif *netif = &cyw43_state.netif[CYW43_ITF_STA];
    snprintf(wifi_ip_str, sizeof(wifi_ip_str), "%s", ipaddr_ntoa(&netif->ip_addr));
    cyw43_wifi_get_rssi(&cyw43_state, &wifi_rssi);
    printf("[WiFi] Connected: %s  RSSI: %d\n", wifi_ip_str, (int)wifi_rssi);

    ntp_request();
}

void wifi_connect(void) {
    wifi_connect_to(WIFI_SSID, WIFI_PASS);
}

void wifi_disconnect(void) {
    printf("[WiFi] Disconnecting...\n");
    cyw43_wifi_leave(&cyw43_state, CYW43_ITF_STA);
    /* Keep CYW43 initialised — battery VSYS sense needs it on GPIO 29 */
    wifi_connected = false;
    wifi_enabled = false;
    ntp_synced = false;
    strncpy(wifi_ssid_display, "---", sizeof(wifi_ssid_display));
    strncpy(wifi_ip_str, "---", sizeof(wifi_ip_str));
}

/* ─── Background WiFi rejoin after wake ─── */

/* Start the async join for `ssid` using its saved-store key. */
static void wifi_rejoin_start(const char *ssid) {
    const char *pass = saved_find_pass(ssid);
    if (!pass) return;                        /* no stored key — Network menu it is */
    printf("[SLEEP] rejoining \"%s\" in the background\n", ssid);
    strncpy(wifi_ssid_display, ssid, sizeof(wifi_ssid_display) - 1);
    wifi_ssid_display[sizeof(wifi_ssid_display) - 1] = '\0';
    snprintf(g_rejoin_ssid, sizeof(g_rejoin_ssid), "%s", ssid);
    wifi_enabled = true;
    cyw43_arch_enable_sta_mode();
    if (cyw43_arch_wifi_connect_async(ssid, pass, pass[0] ? CYW43_AUTH_WPA2_AES_PSK : 0) == 0) {
        g_rejoin_pass = pass;                 /* points into g_saved — stable storage */
        g_wifi_rejoin_deadline = to_ms_since_boot(get_absolute_time()) + 30000;
    } else {
        wifi_enabled = false;
    }
}

static void wifi_rejoin_poll(void) {
    static uint32_t last_check = 0;
    uint32_t now = to_ms_since_boot(get_absolute_time());
    if (now - last_check < 2000) return;      /* ~0.5 Hz is plenty */
    last_check = now;
    int ts = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);
    bool expired = (int32_t)(now - g_wifi_rejoin_deadline) > 0;
    if (ts == CYW43_LINK_UP) {
        g_wifi_rejoin_deadline = 0;
        wifi_connected = true;
        struct netif *netif = &cyw43_state.netif[CYW43_ITF_STA];
        snprintf(wifi_ip_str, sizeof(wifi_ip_str), "%s", ipaddr_ntoa(&netif->ip_addr));
        cyw43_wifi_get_rssi(&cyw43_state, &wifi_rssi);
        printf("[SLEEP] wifi back: %s\n", wifi_ip_str);
        ntp_request();
    } else if (ts == CYW43_LINK_NONET && !expired && g_rejoin_pass) {
        /* AP not seen this scan round — re-issue the join and keep polling
         * until the deadline (mirrors wifi_connect_to's NONET handling; the
         * old code treated this transient as fatal and dropped the rejoin). */
        cyw43_arch_wifi_connect_async(g_rejoin_ssid, g_rejoin_pass,
                                      g_rejoin_pass[0] ? CYW43_AUTH_WPA2_AES_PSK : 0);
    } else if (ts == CYW43_LINK_BADAUTH || ts == CYW43_LINK_FAIL || expired) {
        g_wifi_rejoin_deadline = 0;           /* give up quietly; the menu can reconnect */
        wifi_enabled = false;
        strncpy(wifi_ssid_display, "---", sizeof(wifi_ssid_display));
    }
}

void wifi_schedule_rejoin(const char *ssid, uint32_t delay_ms) {
    snprintf(g_rejoin_pending, sizeof(g_rejoin_pending), "%s", ssid);
    g_rejoin_start_ms = to_ms_since_boot(get_absolute_time()) + delay_ms;
}

void wifi_rejoin_cancel(void) {
    g_wifi_rejoin_deadline = 0;               /* cancel any rejoin still in flight */
    g_rejoin_start_ms = 0;                    /* ... and any join still deferred */
}

bool wifi_rejoin_pending(void) {
    return g_wifi_rejoin_deadline != 0 || g_rejoin_start_ms != 0;
}

/* Octopus-loop hook: fire the DEFERRED post-wake join once its delay elapses,
 * then poll the join to completion (see wifi_schedule_rejoin's rationale). */
void wifi_rejoin_tick(void) {
    if (g_rejoin_start_ms) {
        if ((int32_t)(to_ms_since_boot(get_absolute_time()) - g_rejoin_start_ms) < 0) return;
        g_rejoin_start_ms = 0;
        wifi_rejoin_start(g_rejoin_pending);
    }
    if (g_wifi_rejoin_deadline) wifi_rejoin_poll();
}
