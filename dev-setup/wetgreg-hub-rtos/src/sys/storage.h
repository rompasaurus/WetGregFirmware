/**
 * storage.h — flash-backed settings store + WetGreg identity + social log.
 *
 * Purpose: define the persisted settings store (WiFi, identity, social log,
 * display prefs) and its mutation API.
 *
 * One 4 KB image in the last flash sector (survives reboot + OTA): saved WiFi
 * networks, the device's social id/name, the "WetGregs met" log, and the
 * display settings.
 */
#ifndef STORAGE_H
#define STORAGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MAX_SAVED     8
#define SOCIAL_MAX    16             /* other WetGregs remembered in the Social log */

typedef struct { char ssid[33]; char pass[64]; } saved_net_t;

/* One remembered peer WetGreg. The name is NOT stored — it is derived
 * deterministically from `id` (every WetGreg computes the same ridiculous name
 * for a given id), so the log only needs the id + when we last greeted + how. */
#define MET_HELLO_SENT  0x01
#define MET_HELLO_RECV  0x02
typedef struct { uint16_t id; uint32_t last_day; uint8_t flags; } wetgreg_met_t;

/* vsys_cal and the social/identity block are appended at the END; the magic bump
 * forces a clean re-seed so we never read stale layout into the new fields. */
typedef struct {
    uint32_t     magic, count;
    saved_net_t  nets[MAX_SAVED];
    float        vsys_cal;
    /* ── WetGreg identity + social ── */
    uint16_t     wetgreg_id;            /* our BLE social id (random, set once) */
    char         wetgreg_name[24];      /* custom name; "" → use the auto name */
    uint8_t      social_on;             /* persisted opt-in for proximity scanning */
    uint32_t     met_count;
    wetgreg_met_t met[SOCIAL_MAX];
    /* ── Display settings (appended AFTER the social block so old flash images
     * load fine — these read as 0xFF and get sanitised to defaults on load,
     * no SAVED_MAGIC bump needed). ── */
    uint8_t      auto_rotate;          /* 1 = accelerometer auto-rotate (default), 0 = manual */
    uint8_t      manual_orient;        /* OR_LAND_R/OR_LAND_L/OR_TALL when auto_rotate==0 */
    /* ── Battery calibration + WiFi policy (appended; 0xFF on old images →
     * sanitised to defaults on load, no SAVED_MAGIC bump). ── */
    uint16_t     batt_offset_mv;       /* additive VSYS→cell offset (battery.h); 0xFFFF = unset */
    uint8_t      batt_offset_src;      /* 'D' default / 'A' auto-anchor / 'M' manual */
    uint8_t      wifi_mode;            /* WIFI_MODE_SYNC_ONLY (default) / WIFI_MODE_ALWAYS */
    char         last_ssid[33];        /* last network joined — the daily time-sync target */
    /* ── Language (appended; 0xFF on old images → sanitised to English). ── */
    uint8_t      language;             /* LANG_EN (default) / LANG_AR (Egyptian Arabic) */
} saved_store_t;

#define WIFI_MODE_SYNC_ONLY 0          /* join → NTP → drop (and once a day in the background) */
#define WIFI_MODE_ALWAYS    1          /* stay associated (PM1 power-save) until told otherwise */

#define LANG_EN 0                      /* English (default) */
#define LANG_AR 1                      /* Egyptian Arabic: quotes, moods, home-screen labels */
#define LANG_COUNT 2

/* The in-RAM copy of the store. Mutate ONLY through the functions below (they
 * persist), except g_saved.auto_rotate/manual_orient which the Display-settings
 * handler updates before calling saved_write_flash() itself. */
extern saved_store_t g_saved;

void        saved_load(void);          /* load or first-boot seed (+ id assignment) */
void        saved_write_flash(void);   /* persist g_saved (multicore-safe) */
void        saved_seed_defaults(void); /* factory defaults into g_saved (no write) */

/* Saved WiFi networks */
const char *saved_find_pass(const char *ssid);
void        saved_add(const char *ssid, const char *pass);
void        saved_forget(int idx);

/* Social log (other WetGregs we've met) */
uint32_t    wetgreg_today(void);       /* calendar-day key for the re-greet cooldown */
void        met_record(uint16_t id, uint8_t flag, uint32_t day);
bool        met_should_greet(uint16_t id);

/* Battery calibration + WiFi policy (persist) */
void        battery_offset_store(uint16_t mv);   /* called by battery.c; stores g_batt_offset_src too */
void        wifi_mode_store(uint8_t mode);
void        wifi_last_ssid_store(const char *ssid); /* no-op (no flash wear) when unchanged */

/* Language (persist). lang_is_ar() is the one check every renderer uses. */
void        language_store(uint8_t lang);
static inline int lang_is_ar(void) { return g_saved.language == LANG_AR; }

/* Identity */
void        wetgreg_set_name(const char *name);
void        wetgreg_set_social(bool on);
void        wetgreg_auto_name(uint16_t id, char *buf, size_t n);
const char *wetgreg_display_name(void);

#endif /* STORAGE_H */
