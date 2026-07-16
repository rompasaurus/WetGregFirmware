/**
 * storage.c — flash-backed settings store + WetGreg identity + social log.
 *
 * Purpose: implement the flash-sector store: load/seed/persist, saved
 * networks, the met-log, and the deterministic name generator.
 *
 * The store lives in the LAST flash sector so it survives reboot + OTA. All
 * writes go through flash_safe_execute() — critical under SMP because the
 * Display task on core 1 executes from XIP flash and its next instruction
 * fetch during an erase would fault.
 */
#include "storage.h"

#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "pico/flash.h"        /* flash_safe_execute — multicore-safe flash writes (SMP) */
#include "hardware/flash.h"    /* low-level flash erase/program */

#include "FreeRTOS.h"
#include "task.h"

#include "battery.h"
#include "motion.h"            /* g_auto_rotate / g_manual_orient runtime mirrors */
#include "rng.h"
#include "rtc_compat.h"
#include "wifi_config.h"       /* WIFI_PASS — first-boot seeded networks */

#define SAVED_MAGIC   0x4D4F5033u    /* 'MOP3' — bumped: added social/identity block */
#define SAVED_FLASH_OFFSET  (PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE)

saved_store_t g_saved;

/* ─── Ridiculous WetGreg name generator ───────────────────────────────────────
 * A WetGreg's name is derived purely from its 16-bit id, so EVERY WetGreg computes
 * the same name for a given id without transmitting it. 32×32 = 1024 combos. */
static const char *k_name_adj[32] = {
    "Soggy","Feral","Greasy","Smug","Moist","Cursed","Spicy","Wobbly",
    "Crusty","Unhinged","Sneaky","Thicc","Forbidden","Haunted","Disco","Goth",
    "Sassy","Rancid","Deluxe","Bonkers","Gremlin","Chonky","Slippery","Vile",
    "Majestic","Sweaty","Eldritch","Bootleg","Feral","Yeeted","Mlem","Zoomie",
};
static const char *k_name_noun[32] = {
    "Noodle","Goblin","Trashpanda","Wizard","Gremlin","Nugget","Possum","Crumpet",
    "Walrus","Pickle","Goose","Yeti","Muppet","Cryptid","Dumpling","Snail",
    "Beans","Hamster","Goblin","Toad","Raccoon","Biscuit","Lizard","Moth",
    "Blobfish","Chinchilla","Wombat","Gourd","Sphinx","Frog","Capybara","Slug",
};

/* Fill `buf` with the deterministic auto-name for `id`. */
void wetgreg_auto_name(uint16_t id, char *buf, size_t n) {
    snprintf(buf, n, "%s %s", k_name_adj[id & 31], k_name_noun[(id >> 5) & 31]);
}

/* OUR display name: the custom one if set, else the auto-name for our id. */
const char *wetgreg_display_name(void) {
    static char nm[24];
    if (g_saved.wetgreg_name[0]) return g_saved.wetgreg_name;
    wetgreg_auto_name(g_saved.wetgreg_id, nm, sizeof(nm));
    return nm;
}

/* ─── The raw erase+program ───
 * Run ONLY via flash_safe_execute() (below) so the other core is parked first.
 * `param` points at the 4 KB image to program. */
static void saved_flash_op(void *param) {
    const uint8_t *buf = (const uint8_t *)param;
    flash_range_erase(SAVED_FLASH_OFFSET, FLASH_SECTOR_SIZE);
    flash_range_program(SAVED_FLASH_OFFSET, buf, FLASH_SECTOR_SIZE);
}

void saved_write_flash(void) {
    static uint8_t buf[FLASH_SECTOR_SIZE];
    memset(buf, 0xFF, sizeof(buf));
    memcpy(buf, &g_saved, sizeof(g_saved));

    /* flash_safe_execute uses the FreeRTOS-SMP multicore lockout to safely park
     * core 1 (the Display task, which runs the e-ink driver from XIP flash) for
     * the duration of the write. We ALWAYS go through it and retry on contention.
     * We deliberately do NOT fall back to a plain interrupts-off write: that only
     * stops THIS core and would let core 1 XIP-fault mid-erase. All callers run
     * under the scheduler after the flash-ready handshake, so this is safe. */
    for (int attempt = 0; attempt < 3; attempt++) {
        if (flash_safe_execute(saved_flash_op, buf, 3000 /* ms */) == PICO_OK) return;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    printf("[flash] WARNING: flash_safe_execute failed 3x — settings NOT saved this time\n");
}

void saved_seed_defaults(void) {
    memset(&g_saved, 0, sizeof(g_saved));
    g_saved.magic = SAVED_MAGIC;
    g_saved.count = 2;
    strncpy(g_saved.nets[0].ssid, "Moop Ship",    sizeof(g_saved.nets[0].ssid) - 1);
    strncpy(g_saved.nets[0].pass, WIFI_PASS,       sizeof(g_saved.nets[0].pass) - 1);
    strncpy(g_saved.nets[1].ssid, "MoopsterCell",  sizeof(g_saved.nets[1].ssid) - 1);
    strncpy(g_saved.nets[1].pass, WIFI_PASS,       sizeof(g_saved.nets[1].pass) - 1);
    g_saved.vsys_cal = 1.0f;
    g_saved.wetgreg_id = 0;            /* 0 → generated in saved_load() */
    g_saved.wetgreg_name[0] = '\0';
    g_saved.social_on = 0;
    g_saved.met_count = 0;
    g_saved.auto_rotate = 1;          /* auto-rotate ON by default */
    g_saved.manual_orient = OR_TALL;
}

void saved_load(void) {
    const saved_store_t *fl = (const saved_store_t *)(XIP_BASE + SAVED_FLASH_OFFSET);
    if (fl->magic == SAVED_MAGIC && fl->count <= MAX_SAVED) {
        memcpy(&g_saved, fl, sizeof(g_saved));
    } else {
        saved_seed_defaults();   /* first boot — persist the two defaults */
        saved_write_flash();
    }
    /* Battery trim is NOT loaded from flash anymore: the measurement uses the
     * baked nominal estimate (VSYS_CAL, the 3:1-divider + 3.3 V-ref math) every
     * boot, so the device needs no calibration ritual. The 4-bar icon doesn't
     * need per-board precision, and the peak-hold estimator removes the load
     * bias that used to make calibration seem necessary. The Device-Info UP/DOWN
     * keys remain as an OPTIONAL live trim (not persisted). For real per-board
     * accuracy, Rev 2 will auto-anchor to 4.2 V off the TP4056 STDBY pin. */
    battery_cal_save(1.0f);

    /* Assign a persistent social id on first ever boot (and defend against a
     * zeroed/sanitised field). rng is already seeded before the scheduler. */
    if (g_saved.wetgreg_id == 0) {
        g_saved.wetgreg_id = (uint16_t)(rng_next() & 0xFFFF);
        if (g_saved.wetgreg_id == 0) g_saved.wetgreg_id = 0x1D1E;   /* never 0 */
        if (g_saved.met_count > SOCIAL_MAX) g_saved.met_count = 0;
        saved_write_flash();
    }
    if (g_saved.met_count > SOCIAL_MAX) g_saved.met_count = 0;     /* sanity */
    /* Display settings appended after the social block — old images read 0xFF
     * here, so sanitise to defaults (auto-rotate ON, tall). */
    if (g_saved.auto_rotate > 1)   g_saved.auto_rotate = 1;
    if (g_saved.manual_orient > 2) g_saved.manual_orient = OR_TALL;
    g_auto_rotate   = g_saved.auto_rotate ? true : false;
    g_manual_orient = g_saved.manual_orient;
    printf("[social] this WetGreg: id=%04X name=\"%s\"\n",
           g_saved.wetgreg_id, wetgreg_display_name());
}

/* ─── Saved WiFi networks ─── */

const char *saved_find_pass(const char *ssid) {
    for (uint32_t i = 0; i < g_saved.count; i++)
        if (strcmp(g_saved.nets[i].ssid, ssid) == 0) return g_saved.nets[i].pass;
    return NULL;
}

void saved_add(const char *ssid, const char *pass) {
    for (uint32_t i = 0; i < g_saved.count; i++)
        if (strcmp(g_saved.nets[i].ssid, ssid) == 0) {       /* update existing */
            strncpy(g_saved.nets[i].pass, pass, sizeof(g_saved.nets[i].pass) - 1);
            saved_write_flash(); return;
        }
    if (g_saved.count < MAX_SAVED) {                          /* add new */
        strncpy(g_saved.nets[g_saved.count].ssid, ssid, sizeof(g_saved.nets[0].ssid) - 1);
        strncpy(g_saved.nets[g_saved.count].pass, pass, sizeof(g_saved.nets[0].pass) - 1);
        g_saved.count++;
        saved_write_flash();
    }
}

void saved_forget(int idx) {
    if (idx < 0 || idx >= (int)g_saved.count) return;
    for (uint32_t i = idx; i + 1 < g_saved.count; i++) g_saved.nets[i] = g_saved.nets[i + 1];
    g_saved.count--;
    saved_write_flash();
}

/* ─── Social log (other WetGregs we've met) ───────────────────────────────────
 * Persisted in g_saved.met[]. Names aren't stored (derived from id). */

/* Calendar-day key for the 24 h re-greet cooldown. RTC unset → 0 (still works:
 * a re-greet fires whenever the day key changes). */
uint32_t wetgreg_today(void) {
    datetime_t t;
    rtc_get_datetime(&t);
    if (t.year < 2020) return 0;
    return (uint32_t)t.year * 10000u + (uint32_t)t.month * 100u + (uint32_t)t.day;
}

static int met_find(uint16_t id) {
    for (uint32_t i = 0; i < g_saved.met_count && i < SOCIAL_MAX; i++)
        if (g_saved.met[i].id == id) return (int)i;
    return -1;
}

/* OR-in `flag`, stamp `day`, persisting. Evicts the oldest if the log is full. */
void met_record(uint16_t id, uint8_t flag, uint32_t day) {
    int idx = met_find(id);
    if (idx < 0) {
        if (g_saved.met_count < SOCIAL_MAX) {
            idx = (int)g_saved.met_count++;
        } else {
            /* full — evict the entry with the oldest last_day */
            idx = 0;
            for (uint32_t i = 1; i < SOCIAL_MAX; i++)
                if (g_saved.met[i].last_day < g_saved.met[idx].last_day) idx = (int)i;
        }
        g_saved.met[idx].id = id;
        g_saved.met[idx].flags = 0;
    }
    g_saved.met[idx].flags |= flag;
    g_saved.met[idx].last_day = day;
    saved_write_flash();
}

/* True if we should auto-prompt for this WetGreg today (24 h cooldown). */
bool met_should_greet(uint16_t id) {
    int idx = met_find(id);
    if (idx < 0) return true;                       /* never met */
    return g_saved.met[idx].last_day != wetgreg_today();
}

void wetgreg_set_name(const char *name) {
    snprintf(g_saved.wetgreg_name, sizeof(g_saved.wetgreg_name), "%s", name ? name : "");
    saved_write_flash();
}

void wetgreg_set_social(bool on) {
    g_saved.social_on = on ? 1 : 0;
    saved_write_flash();
}
