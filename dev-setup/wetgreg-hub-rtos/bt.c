/* WetGreg Hub — BLE peripheral: advertise as "WetGreg Hub", accept a connection
 * from a phone, pair/bond with a passkey shown on screen, and expose a small
 * custom GATT service (read live status + notify, write a command byte). */
#include "bt.h"
#include <stdio.h>
#include <string.h>
#include "btstack.h"
#include "pico/cyw43_arch.h"   /* cyw43_thread_enter/exit — serialise vs the BT run loop */
#include "wetgreg.h"   /* generated from wetgreg.gatt by pico_btstack_make_gatt_header */

/* Handle macros from the generated GATT header (custom 0xFFE0 service). */
#define H_STATUS_VALUE  ATT_CHARACTERISTIC_0000FFE1_0000_1000_8000_00805F9B34FB_01_VALUE_HANDLE
#define H_STATUS_CCC    ATT_CHARACTERISTIC_0000FFE1_0000_1000_8000_00805F9B34FB_01_CLIENT_CONFIGURATION_HANDLE
#define H_CMD_VALUE     ATT_CHARACTERISTIC_0000FFE2_0000_1000_8000_00805F9B34FB_01_VALUE_HANDLE

static bt_state_t g_state  = BT_OFF;
static bool       g_active = false;
/* The BTstack stack (l2cap/sm/att + the static handler nodes) is brought up
 * exactly ONCE, ever. Re-running l2cap_init()/sm_init()/att_server_init() or
 * re-adding an already-linked handler node corrupts BTstack's handler lists, so
 * enable/disable only toggles HCI power, not the whole stack. */
static bool       g_setup_done = false;
static char       g_peer[20] = "";
static uint32_t   g_passkey = 0;
static btstack_packet_callback_registration_t hci_reg, sm_reg;

static char            g_status[24] = "BOOTING 0";
/* volatile: written by the BTstack callback (cyw43 background task) and read+
 * cleared by the UI task (both core 0). A phone command landing in the
 * read-clear window can be dropped — rare, non-corrupting, acceptable. */
static volatile int    g_cmd        = -1;
static hci_con_handle_t g_con       = HCI_CON_HANDLE_INVALID;
static bool            g_notify     = false;
static btstack_context_callback_registration_t g_notify_cb;

/* Advertising payload: discoverable flags + name + the WetGreg social beacon
 * (manufacturer-specific AD). The id/target/flags bytes are patched at runtime,
 * so this is NOT const. Layout (byte offsets noted for the patch macros):
 *   [len][FLAGS][0x06]                          flags
 *   [len][NAME ]['WetGreg Hub']                  complete local name
 *   [0x0A][MFG ][compLo][compHi]['D']['L'][idLo][idHi][tgtLo][tgtHi][flags]
 * "company" 0xFFFF is the BLE "no company"/internal id — fine for a closed toy. */
#define WETGREG_COMPANY_LO 0xFF
#define WETGREG_COMPANY_HI 0xFF
#define WETGREG_HELLO_FLAG 0x01
static uint8_t adv_data[] = {
    0x02, BLUETOOTH_DATA_TYPE_FLAGS, 0x06,
    0x0B, BLUETOOTH_DATA_TYPE_COMPLETE_LOCAL_NAME,
    'D', 'i', 'l', 'd', 'e', 'r', ' ', 'H', 'u', 'b',
    0x0A, BLUETOOTH_DATA_TYPE_MANUFACTURER_SPECIFIC_DATA,
    WETGREG_COMPANY_LO, WETGREG_COMPANY_HI, 'D', 'L',
    0x00, 0x00,   /* our id      */
    0x00, 0x00,   /* hello target */
    0x00,         /* hello flags  */
};
#define ADV_OFF_ID   21   /* idLo */
#define ADV_OFF_TGT  23   /* tgtLo */
#define ADV_OFF_FLAG 25   /* flags */

/* ── Social state ──
 * THREADING RULE: the app task only writes the volatile "intent" flags below.
 * ALL BTstack calls (advert, scan, timers) happen in g_social_tick(), which runs
 * in the BT run-loop context. This avoids the cross-thread run-loop corruption
 * that previously wedged the stack after a couple of sends. */
static uint16_t g_self_id   = 0;
static volatile bool g_social = false;    /* desired: scan for WetGregs */
static bool     g_scanning  = false;      /* actual scan state (run-loop owned) */

/* App-set intent for the directed emote (latest-wins; older sends are thrown out). */
static volatile uint16_t g_want_target = 0;
static volatile uint8_t  g_want_emote  = 0;
static volatile int      g_want_ttl    = 0;     /* social ticks left to broadcast it */
static volatile bool     g_adv_dirty   = true;  /* advert needs (re)applying */

static btstack_timer_source_t g_social_timer;
static bool g_social_timer_on = false;

/* one-slot latch: written by the BT run loop (advertising-report parse),
 * read+cleared by the app via wetgreg_social_poll(). */
static volatile uint16_t g_peer_id      = 0;
static volatile bool     g_peer_hello   = false;
static volatile uint8_t  g_peer_emote   = 0;
static volatile int8_t   g_peer_rssi    = 0;
static volatile bool     g_peer_pending = false;
static uint16_t g_last_seen_id = 0;     /* rate-limit repeats of the same id */
static uint32_t g_last_seen_ms = 0;

/* Periodic worker — runs ONLY in the BT run-loop context (re-arms itself). It
 * reconciles the scan state, applies a dirty advert once, and expires the
 * directed emote. ~250 ms cadence keeps HCI traffic and radio duty low. */
static void g_social_tick(btstack_timer_source_t *ts) {
    if (g_active) {
        /* reconcile scanning to the desired state */
        if (g_social && !g_scanning) {
            /* low duty: 30 ms window every 200 ms ≈ 15% — far less overhead/flood
             * than the old continuous (window==interval) scan. */
            gap_set_scan_parameters(0 /* passive */, 0x0140, 0x0030);
            gap_start_scan();
            g_scanning = true;
        } else if (!g_social && g_scanning) {
            gap_stop_scan();
            g_scanning = false;
        }
        /* expire the directed emote after its window */
        if (g_want_ttl > 0 && --g_want_ttl == 0) {
            g_want_target = 0; g_want_emote = 0; g_adv_dirty = true;
        }
        /* apply the advert at most once per change (coalesces rapid sends) */
        if (g_adv_dirty) {
            g_adv_dirty = false;
            adv_data[ADV_OFF_ID]      = (uint8_t)(g_self_id & 0xFF);
            adv_data[ADV_OFF_ID + 1]  = (uint8_t)(g_self_id >> 8);
            adv_data[ADV_OFF_TGT]     = (uint8_t)(g_want_target & 0xFF);
            adv_data[ADV_OFF_TGT + 1] = (uint8_t)(g_want_target >> 8);
            adv_data[ADV_OFF_FLAG]    = g_want_emote;
            gap_advertisements_set_data(sizeof(adv_data), adv_data);
        }
    } else {
        g_scanning = false;
    }
    btstack_run_loop_set_timer(ts, 250);
    btstack_run_loop_add_timer(ts);
}

static void start_advertising(void) {
    uint16_t adv_int_min = 0x0030, adv_int_max = 0x0030;
    bd_addr_t null_addr; memset(null_addr, 0, sizeof(null_addr));
    gap_advertisements_set_params(adv_int_min, adv_int_max, 0, 0, null_addr, 0x07, 0x00);
    gap_advertisements_set_data(sizeof(adv_data), (uint8_t *)adv_data);
    gap_advertisements_enable(1);
    g_state = BT_ADVERTISING;
}

/* ── Custom GATT service: read status, write command, notify on change ── */
static uint16_t att_read_cb(hci_con_handle_t con, uint16_t handle, uint16_t offset,
                            uint8_t *buf, uint16_t buf_size) {
    (void)con;
    if (handle == H_STATUS_VALUE)
        return att_read_callback_handle_blob((const uint8_t *)g_status,
                                             (uint16_t)strlen(g_status), offset, buf, buf_size);
    return 0;
}

static int att_write_cb(hci_con_handle_t con, uint16_t handle, uint16_t mode,
                        uint16_t offset, uint8_t *buf, uint16_t buf_size) {
    (void)con; (void)mode; (void)offset;
    if (handle == H_CMD_VALUE) {
        if (buf_size > 0) g_cmd = buf[0];
    } else if (handle == H_STATUS_CCC) {
        g_notify = (buf_size >= 2) && (little_endian_read_16(buf, 0) != 0);
    }
    return 0;
}

static void notify_now(void *ctx) {
    (void)ctx;
    if (g_con != HCI_CON_HANDLE_INVALID && g_notify)
        att_server_notify(g_con, H_STATUS_VALUE, (const uint8_t *)g_status, (uint16_t)strlen(g_status));
}

static void hci_handler(uint8_t type, uint16_t ch, uint8_t *packet, uint16_t size) {
    (void)ch; (void)size;
    if (type != HCI_EVENT_PACKET) return;
    switch (hci_event_packet_get_type(packet)) {
        case BTSTACK_EVENT_STATE:
            if (btstack_event_state_get_state(packet) == HCI_STATE_WORKING) {
                g_adv_dirty = true;                  /* bake our id/beacon into the advert */
                start_advertising();
                if (!g_social_timer_on) {            /* one periodic run-loop worker */
                    btstack_run_loop_set_timer_handler(&g_social_timer, g_social_tick);
                    btstack_run_loop_set_timer(&g_social_timer, 250);
                    btstack_run_loop_add_timer(&g_social_timer);
                    g_social_timer_on = true;
                }
            }
            break;
        case GAP_EVENT_ADVERTISING_REPORT: {
            if (!g_social) break;
            const uint8_t *data = gap_event_advertising_report_get_data(packet);
            uint8_t dlen = gap_event_advertising_report_get_data_length(packet);
            uint16_t found_id = 0, tgt = 0; uint8_t flags = 0; bool is_wetgreg = false;
            ad_context_t ctx;
            for (ad_iterator_init(&ctx, dlen, data); ad_iterator_has_more(&ctx); ad_iterator_next(&ctx)) {
                if (ad_iterator_get_data_type(&ctx) != BLUETOOTH_DATA_TYPE_MANUFACTURER_SPECIFIC_DATA)
                    continue;
                const uint8_t *d = ad_iterator_get_data(&ctx);
                if (ad_iterator_get_data_len(&ctx) >= 9 &&
                    d[0] == WETGREG_COMPANY_LO && d[1] == WETGREG_COMPANY_HI &&
                    d[2] == 'D' && d[3] == 'L') {
                    found_id = (uint16_t)(d[4] | (d[5] << 8));
                    tgt      = (uint16_t)(d[6] | (d[7] << 8));
                    flags    = d[8];
                    is_wetgreg = true;
                }
                break;
            }
            if (!is_wetgreg || found_id == 0 || found_id == g_self_id) break;
            /* `flags` carries the emote code (0 = none); it's directed at us when
             * the target id matches ours. */
            bool hello = (flags != 0) && (tgt == g_self_id);
            uint32_t now = btstack_run_loop_get_time_ms();
            /* Rate-limit repeats of the same id (~4 s) so a steady beacon doesn't
             * spam the app — but a directed emote always gets through. */
            if (!hello && found_id == g_last_seen_id && (uint32_t)(now - g_last_seen_ms) < 4000) break;
            g_last_seen_id = found_id; g_last_seen_ms = now;
            g_peer_id    = found_id;
            g_peer_hello = hello;
            g_peer_emote = hello ? flags : 0;
            g_peer_rssi  = (int8_t)gap_event_advertising_report_get_rssi(packet);
            g_peer_pending = true;
            break;
        }
        case HCI_EVENT_DISCONNECTION_COMPLETE:
            g_peer[0] = '\0';
            g_passkey = 0;
            g_con     = HCI_CON_HANDLE_INVALID;
            g_notify  = false;
            start_advertising();
            break;
        case HCI_EVENT_LE_META:
            if (hci_event_le_meta_get_subevent_code(packet) == HCI_SUBEVENT_LE_CONNECTION_COMPLETE) {
                bd_addr_t addr;
                hci_subevent_le_connection_complete_get_peer_address(packet, addr);
                snprintf(g_peer, sizeof(g_peer), "%s", bd_addr_to_str(addr));
                g_con   = hci_subevent_le_connection_complete_get_connection_handle(packet);
                g_state = BT_CONNECTED;
            }
            break;
        default:
            break;
    }
}

static void sm_handler(uint8_t type, uint16_t ch, uint8_t *packet, uint16_t size) {
    (void)ch; (void)size;
    if (type != HCI_EVENT_PACKET) return;
    switch (hci_event_packet_get_type(packet)) {
        case SM_EVENT_PASSKEY_DISPLAY_NUMBER:
            g_passkey = sm_event_passkey_display_number_get_passkey(packet);
            g_state   = BT_PAIRING;
            break;
        case SM_EVENT_PAIRING_COMPLETE:
            g_passkey = 0;
            if (sm_event_pairing_complete_get_status(packet) == ERROR_CODE_SUCCESS) g_state = BT_PAIRED;
            else g_state = BT_CONNECTED;
            break;
        case SM_EVENT_REENCRYPTION_COMPLETE:
            g_state = BT_PAIRED;
            break;
        default:
            break;
    }
}

void wetgreg_bt_init(void) {
    if (g_active) return;

    /* All BTstack calls below touch structures the BT run loop also walks on the
     * cyw43 background task. Hold the cyw43 lock so the two never race (an
     * unlocked call here is what bricked the device on disable). */
    cyw43_thread_enter();

    if (!g_setup_done) {
        l2cap_init();
        sm_init();
        /* DISPLAY_ONLY → device shows a passkey, phone enters it (Passkey Entry). */
        sm_set_io_capabilities(IO_CAPABILITY_DISPLAY_ONLY);
        sm_set_authentication_requirements(SM_AUTHREQ_MITM_PROTECTION | SM_AUTHREQ_BONDING | SM_AUTHREQ_SECURE_CONNECTION);

        att_server_init(profile_data, att_read_cb, att_write_cb);

        hci_reg.callback = &hci_handler;
        hci_add_event_handler(&hci_reg);
        att_server_register_packet_handler(hci_handler);
        sm_reg.callback = &sm_handler;
        sm_add_event_handler(&sm_reg);

        g_notify_cb.callback = &notify_now;
        g_setup_done = true;
    }

    g_active = true;
    g_state  = BT_STARTING;
    /* Power on; the BTSTACK_EVENT_STATE → HCI_STATE_WORKING handler advertises. */
    hci_power_control(HCI_POWER_ON);

    cyw43_thread_exit();
}

void wetgreg_bt_stop(void) {
    if (!g_active) return;

    cyw43_thread_enter();
    gap_advertisements_enable(0);
    hci_power_control(HCI_POWER_OFF);   /* tears down any active connection too */
    cyw43_thread_exit();

    g_active  = false;
    g_state   = BT_OFF;
    g_con     = HCI_CON_HANDLE_INVALID;
    g_notify  = false;
    g_passkey = 0;
    g_peer[0] = '\0';
}

void wetgreg_bt_set_status(const char *s) {
    if (!s) return;
    if (strncmp(g_status, s, sizeof(g_status)) == 0) return;   /* unchanged → no notify */
    cyw43_thread_enter();
    snprintf(g_status, sizeof(g_status), "%s", s);
    if (g_con != HCI_CON_HANDLE_INVALID && g_notify) {
        g_notify_cb.context = NULL;
        att_server_request_to_send_notification(&g_notify_cb, g_con);
    }
    cyw43_thread_exit();
}

int wetgreg_bt_take_command(void) {
    int c = g_cmd;
    g_cmd = -1;
    return c;
}

bt_state_t  wetgreg_bt_state(void)   { return g_state; }
const char *wetgreg_bt_peer(void)    { return g_peer; }
bool        wetgreg_bt_active(void)  { return g_active; }
uint32_t    wetgreg_bt_passkey(void) { return g_passkey; }

/* ── WetGreg-to-WetGreg social ── */

/* All of these only set intent flags — g_social_tick() (run-loop ctx) acts on
 * them. No BTstack calls here, so they're safe to call from the app task. */
void wetgreg_social_set_self(uint16_t id) {
    g_self_id = id;
    g_adv_dirty = true;
}

void wetgreg_social_enable(bool on) {
    g_social = on;             /* tick reconciles the actual scan within ~250 ms */
}

bool wetgreg_social_active(void) { return g_social && g_scanning; }

void wetgreg_social_send_emote(uint16_t target_id, uint8_t emote) {
    if (emote == 0) emote = WETGREG_HELLO_FLAG;   /* never broadcast a 0 "directed" code */
    g_want_target = target_id;                   /* latest-wins: throws out any older send */
    g_want_emote  = emote;
    g_want_ttl    = 60;                          /* ~60 × 250 ms ≈ 15 s broadcast window */
    g_adv_dirty   = true;
}

void wetgreg_social_say_hello(uint16_t target_id) {
    wetgreg_social_send_emote(target_id, WETGREG_HELLO_FLAG);
}

bool wetgreg_social_poll(wetgreg_peer_t *out) {
    if (!g_peer_pending) return false;
    out->id          = g_peer_id;
    out->hello_to_me = g_peer_hello;
    out->emote       = g_peer_emote;
    out->rssi        = g_peer_rssi;
    g_peer_pending   = false;
    return true;
}
