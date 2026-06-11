#ifndef DILDER_BT_H
#define DILDER_BT_H
#include <stdbool.h>
#include <stdint.h>

/* BT_PAIRING = a 6-digit passkey is on screen, waiting for the phone to enter it. */
typedef enum { BT_OFF, BT_STARTING, BT_ADVERTISING, BT_CONNECTED, BT_PAIRING, BT_PAIRED } bt_state_t;

void        dilder_bt_init(void);    /* one-time: HCI/GATT/SM setup + power on + advertise */
void        dilder_bt_stop(void);    /* stop advertising + power off */
bt_state_t  dilder_bt_state(void);
const char *dilder_bt_peer(void);    /* connected peer address, "" if none */
bool        dilder_bt_active(void);  /* true once init'd — poll cyw43 while active */
uint32_t    dilder_bt_passkey(void); /* 6-digit pairing code to show, 0 if none */

/* Custom GATT service bridge to the app. */
void        dilder_bt_set_status(const char *s); /* phone-readable "MOOD STEPS"; notifies subscribers */
int         dilder_bt_take_command(void);        /* byte the phone last wrote, or -1; clears it */

/* ── Dilder-to-Dilder social (connectionless, advert-based) ──
 * Each Dilder bakes its 16-bit id into a manufacturer-specific AD in its advert.
 * "Saying hello" is just broadcasting that id in a directed field for a window;
 * the other Dilder sees it while scanning. No connection, no pairing. */
typedef struct {
    uint16_t id;           /* the other Dilder's 16-bit social id */
    bool     hello_to_me;  /* their beacon is directing an emote AT US */
    uint8_t  emote;        /* emote code they're sending (0 = none); see EMOTE_* */
    int8_t   rssi;
} dilder_peer_t;

void dilder_social_set_self(uint16_t id);          /* our id — baked into the beacon */
void dilder_social_enable(bool on);                /* start/stop scanning */
bool dilder_social_active(void);                   /* true while scanning */
/* Direct an emote (1..255) at target_id for ~15 s; 0 clears. */
void dilder_social_send_emote(uint16_t target_id, uint8_t emote);
void dilder_social_say_hello(uint16_t target_id);  /* convenience: send_emote(target, EMOTE_WAVE=1) */
bool dilder_social_poll(dilder_peer_t *out);       /* pop a fresh sighting → true, else false */

#endif
