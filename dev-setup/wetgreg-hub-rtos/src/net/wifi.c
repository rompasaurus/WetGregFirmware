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
#include "power.h"         /* g_batt_low / g_power_sleep — no background joins then */
#include "storage.h"       /* saved-network password lookup for the rejoin */
#include "wifi_config.h"   /* compile-time WIFI_SSID / WIFI_PASS */

/* ─── Policy state ─── */
#define WIFI_SYNC_LINGER_AUTO_MS    (20u * 1000u)          /* background join: drop 20 s after NTP */
#define WIFI_SYNC_LINGER_MANUAL_MS  (90u * 1000u)          /* menu join: give the status screen a look */
#define WIFI_SYNC_GIVEUP_MS         (120u * 1000u)         /* no NTP reply in 2 min → drop, retry later */
#define WIFI_SYNC_PERIOD_MS         (24u * 3600u * 1000u)  /* daily */
#define WIFI_SYNC_RETRY_MS          (2u * 3600u * 1000u)   /* after a failed attempt */
#define WIFI_SYNC_BOOT_DELAY_MS     (20u * 1000u)          /* let the splash finish first */

static uint32_t g_last_sync_ms      = 0;   /* 0 = never (ms since boot, forced non-zero) */
static uint32_t g_connected_since   = 0;   /* 0 = not connected */
static uint32_t g_linger_ms         = WIFI_SYNC_LINGER_AUTO_MS;
static uint32_t g_next_autosync_ms  = 0;   /* 0 = none scheduled */
static uint32_t g_ntp_retry_ms      = 0;
static bool     g_pm_applied        = false;

static uint32_t now_ms(void) { return to_ms_since_boot(get_absolute_time()); }

/* Both join paths land here once DHCP is up: remember the network for the
 * daily sync, stamp the session, and ask for the time. */
static void wifi_on_link_up(const char *ssid, bool manual) {
    wifi_connected = true;
    struct netif *netif = &cyw43_state.netif[CYW43_ITF_STA];
    snprintf(wifi_ip_str, sizeof(wifi_ip_str), "%s", ipaddr_ntoa(&netif->ip_addr));
    cyw43_wifi_get_rssi(&cyw43_state, &wifi_rssi);
    g_connected_since = now_ms() ? now_ms() : 1;
    g_linger_ms       = manual ? WIFI_SYNC_LINGER_MANUAL_MS : WIFI_SYNC_LINGER_AUTO_MS;
    g_ntp_retry_ms    = g_connected_since;
    g_pm_applied      = false;
    wifi_last_ssid_store(ssid);           /* no flash wear when unchanged */
    ntp_request();
}

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

    wifi_on_link_up(ssid, true);
    printf("[WiFi] Connected: %s  RSSI: %d\n", wifi_ip_str, (int)wifi_rssi);
}

void wifi_connect(void) {
    wifi_connect_to(WIFI_SSID, WIFI_PASS);
}

void wifi_disconnect(void) {
    printf("[WiFi] Disconnecting...\n");
    cyw43_wifi_leave(&cyw43_state, CYW43_ITF_STA);
    /* The CYW43 stays initialised here (GPIO 25 gates the VSYS divider and
     * VBUS sense lives on the chip); only the STATE_SLEEP path powers it down. */
    wifi_connected = false;
    wifi_enabled = false;
    g_connected_since = 0;
    g_pm_applied = false;
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
        wifi_on_link_up(g_rejoin_ssid, false);
        printf("[WiFi] background join up: %s\n", wifi_ip_str);
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

/* ─── WiFi power policy ─── */

void wifi_policy_boot(void) {
    if (!g_saved.last_ssid[0]) return;           /* never joined anything — nothing to sync against */
    uint32_t now = now_ms();
    if (g_saved.wifi_mode == WIFI_MODE_ALWAYS) {
        /* The user asked for the link to stay up: bring it up in the
         * background once the splash is done (never blocks boot). */
        wifi_schedule_rejoin(g_saved.last_ssid, WIFI_SYNC_BOOT_DELAY_MS);
    } else {
        g_next_autosync_ms = (now + WIFI_SYNC_BOOT_DELAY_MS) ? (now + WIFI_SYNC_BOOT_DELAY_MS) : 1;
    }
}

uint32_t wifi_last_sync_age_s(void) {
    if (!g_last_sync_ms) return UINT32_MAX;
    return (now_ms() - g_last_sync_ms) / 1000u;
}

void wifi_policy_tick(void) {
    static uint32_t last_tick = 0;
    static bool     prev_synced = false;
    uint32_t now = now_ms();
    if (now - last_tick < 1000) return;          /* 1 Hz is plenty */
    last_tick = now;

    if (ntp_synced && !prev_synced) {            /* NTP reply just landed */
        g_last_sync_ms = now ? now : 1;
        g_next_autosync_ms = now + WIFI_SYNC_PERIOD_MS;
        if (!g_next_autosync_ms) g_next_autosync_ms = 1;
        printf("[WiFi] clock synced; next background sync in 24 h\n");
    }
    prev_synced = ntp_synced;

    if (wifi_connected) {
        if (g_saved.wifi_mode == WIFI_MODE_SYNC_ONLY) {
            if (ntp_synced) {
                if (now - g_last_sync_ms >= g_linger_ms) {
                    printf("[WiFi] sync done — dropping the link to save power\n");
                    wifi_disconnect();
                }
            } else if (g_connected_since && now - g_connected_since >= WIFI_SYNC_GIVEUP_MS) {
                printf("[WiFi] no NTP reply in 2 min — dropping the link, retry in 2 h\n");
                wifi_disconnect();
                g_next_autosync_ms = now + WIFI_SYNC_RETRY_MS;
            } else if (now - g_ntp_retry_ms >= 20000) {
                g_ntp_retry_ms = now;
                ntp_request();                   /* UDP is lossy; ask again */
            }
        } else if (!g_pm_applied && ntp_synced) {
            /* ALWAYS: once the time is in, sit in the deepest WiFi power-save.
             * PM1 trades throughput we never use for the lowest idle draw. */
            cyw43_wifi_pm(&cyw43_state, CYW43_AGGRESSIVE_PM);
            g_pm_applied = true;
            printf("[WiFi] link kept up in PM1 power-save\n");
        } else if (g_pm_applied && g_last_sync_ms && now - g_last_sync_ms >= WIFI_SYNC_PERIOD_MS
                   && now - g_ntp_retry_ms >= 60000) {
            g_ntp_retry_ms = now;             /* ALWAYS: trim the software clock's drift daily */
            ntp_synced = false;               /* so the reply registers as a fresh sync */
            ntp_request();
        }
    } else if (g_saved.wifi_mode == WIFI_MODE_SYNC_ONLY && g_next_autosync_ms
               && (int32_t)(now - g_next_autosync_ms) >= 0
               && !wifi_enabled && !wifi_rejoin_pending()
               && g_saved.last_ssid[0] && !g_batt_low && !g_power_sleep) {
        printf("[WiFi] background time sync: joining \"%s\"\n", g_saved.last_ssid);
        wifi_schedule_rejoin(g_saved.last_ssid, 0);
        g_next_autosync_ms = now + WIFI_SYNC_RETRY_MS;   /* success re-arms the daily period */
        if (!g_next_autosync_ms) g_next_autosync_ms = 1;
    }
}
