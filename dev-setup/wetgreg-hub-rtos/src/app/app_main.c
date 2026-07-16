/**
 * app_main.c — firmware entry point + the UI state machine.
 *
 * Purpose: boots the hardware, starts the FreeRTOS scheduler, and runs the
 * application state machine (the UI task): it renders screens through the
 * ui/ modules, consumes input EVENTS from the Input task's queue, and hands
 * finished frames to the Display task on core 1 (rtos_tasks.c). All
 * hardware/service specifics live in the drivers/, sys/, net/, and ui/
 * modules — this file is only the app shell + per-state input handling.
 *
 * Sassy Octopus — Runtime-rendered e-ink animation.
 *
 * Wiring — WetGreg PCB / breadboard SPI0 (GP17-22, see DEV_Config.c):
 *   VCC  -> 3V3(OUT) pin 36    GND  -> GND      pin 38
 *   CS   -> GP17     pin 22    SCL  -> GP18     pin 24  (SPI0 SCK)
 *   SDA  -> GP19     pin 25    DC   -> GP20     pin 26  (SPI0 TX)
 *   RES  -> GP21     pin 27    BUSY -> GP22     pin 29
 */

#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "hardware/watchdog.h"  /* watchdog_reboot — clean restart after factory reset */

/* --- FreeRTOS: the real-time kernel that schedules our tasks across both cores.
 *     FreeRTOS.h MUST come first (it pulls in FreeRTOSConfig.h); task.h adds the
 *     task/scheduler API (xTaskCreate, vTaskStartScheduler, vTaskDelay, ...). --- */
#include "FreeRTOS.h"
#include "task.h"

#include "rtos_tasks.h"        /* the 4-task split interface */

#include "DEV_Config.h"
#include "bt.h"
#include "quotes.h"            /* the ONE translation unit that references quotes[] */
#include "rtc_compat.h"
#include "version.h"

#include "accel.h"
#include "battery.h"
#include "display.h"
#include "joystick.h"
#include "speaker.h"

#include "motion.h"
#include "power.h"
#include "rng.h"
#include "rtc_clock.h"
#include "storage.h"

#include "ntp.h"
#include "wifi.h"

#include "canvas.h"
#include "icons.h"
#include "octopus.h"
#include "text.h"

#include "screens_anim.h"
#include "screens_home.h"
#include "screens_menu.h"
#include "screens_network.h"
#include "screens_social.h"

#ifdef PICOWOTA_OTA
#include "picowota/reboot.h"
#endif

/* ─── App states ─── */
#define STATE_OCTOPUS     0   /* main screen — octopus + quote + clock */
#define STATE_MENU        1   /* menu overlay */
#define STATE_SOUND       2   /* sound test sub-screen */
#define STATE_INFO        3   /* device info sub-screen */
#define STATE_MOOD_SELECT 4   /* mood picker */
#define STATE_NETWORK     5   /* WiFi status (read-only) */
#define STATE_NET_MENU    6   /* Network submenu */
#define STATE_NET_SCAN    7   /* Scan results list */
#define STATE_NET_KEYBOARD 8  /* On-screen keyboard for WiFi password */
#define STATE_MOTION       9  /* Accelerometer / pedometer menu */
#define STATE_SET_TIME    10  /* Manual date/time setter */
#define STATE_SAVED_NETS  11  /* Saved WiFi networks (connect / forget) */
#define STATE_BLUETOOTH   12  /* Bluetooth pairing screen */
#define STATE_SOCIAL          13  /* Social menu (toggle, met list, set name) */
#define STATE_SOCIAL_PROMPT   14  /* "WetGreg found — say hi?" Y/N */
#define STATE_SOCIAL_RECV     15  /* "WetGreg says hello!" — say hi back? Y/N */
#define STATE_SOCIAL_NAME     16  /* reroll picker for your WetGreg name */
#define STATE_SOCIAL_MET      17  /* list of WetGregs we've met */
#define STATE_SOCIAL_NEARBY   18  /* live list of WetGregs in range now */
#define STATE_EMOTE_PICK      19  /* pick an emote to send to g_social_peer */
#define STATE_EMOTE_PLAY      20  /* octopus acts out g_play_emote, then g_play_next */
#define STATE_DISPLAY         21  /* Display settings: auto-rotate + orientation */
#define STATE_SPLASH          22  /* one-shot boot splash animation */
#define STATE_INTRO           23  /* "who is Greg" story animation (Animations menu) */
#define STATE_ANIM_MENU       24  /* Animations submenu (intro, future animations) */
#define STATE_SETTINGS        25  /* Settings submenu (network/bt/display/reset) */
#define STATE_RESET_CONFIRM   26  /* factory reset confirmation card */
#define STATE_SLEEP           27  /* pseudo-off: Greg asleep, radios off, underclocked */

/* Bluetooth is OFF by default to save power; toggled on in the Bluetooth screen. */
static bool bt_enabled = false;

/* ─── Pick a quote matching current_mood, or random if -1 ─── */
static int pick_quote(void) {
    if (current_mood < 0)
        return rng_next() % QUOTE_COUNT;

    /* Collect indices matching the mood */
    int matches[QUOTE_COUNT];
    int count = 0;
    for (int i = 0; i < QUOTE_COUNT; i++) {
        if (quotes[i].mood == (uint8_t)current_mood)
            matches[count++] = i;
    }
    if (count == 0)
        return rng_next() % QUOTE_COUNT;  /* fallback */
    return matches[rng_next() % count];
}

/* Push the current mood + step count to the BLE status characteristic so a
 * paired phone can read/subscribe. No-op (and no notify) when unchanged. */
static void bt_push_status(void) {
    if (!wetgreg_bt_active()) return;
    char s[24];
    snprintf(s, sizeof(s), "%s %lu",
             current_mood < 0 ? "ALL" : mood_names[current_mood],
             (unsigned long)steps_today);
    wetgreg_bt_set_status(s);
}

/* ─── FreeRTOS application task ─────────────────────────────────────────────
 * This is the UI task: it renders, runs the state machine, consumes input
 * EVENTS from the Input task's queue, and hands frames to the Display task
 * (core 1). It also spawns the Input/Display/Housekeeping tasks once it is
 * running (see rtos_tasks_start). Declared here, defined just below main(). */
static void app_task(void *param);
static TaskHandle_t g_app_task = NULL;

/* Stack is measured in WORDS (4 bytes each). The state machine's render path is
 * deep (nested draw_* + snprintf), so we reserve a generous 16 KB for now and
 * trim it in Phase 3 using uxTaskGetStackHighWaterMark(). */
#define APP_TASK_STACK_WORDS  4096
#define APP_TASK_PRIO         (tskIDLE_PRIORITY + 2)

int main(void) {
    stdio_init_all();
    sleep_ms(50);   /* brief settle; was 1000ms of pure boot delay for USB serial */
    printf("WETGREG HUB v%s (%s) | display: %s | %d quotes | built %s %s\n",
           WETGREG_VERSION, WETGREG_VERSION_DATE, DISPLAY_NAME, QUOTE_COUNT, __DATE__, __TIME__);

    init_rtc_from_compile_time();

    if (DEV_Module_Init() != 0) {
        printf("ERROR: Hardware init failed.\n");
        return 1;
    }

    joystick_init();
    speaker_init();

#ifdef PICOWOTA_OTA
    /* Hold the joystick UP at power-on to drop into the picowota WiFi
       bootloader for an over-the-air firmware update. Pull-ups are active
       (joystick is active-low), so give them a moment to settle first. */
    sleep_ms(20);
    if (!gpio_get(JOY_UP)) {
        speaker_tone(2000, 60);  /* audible "entering OTA" cue */
        picowota_reboot(true);
    }
#endif

    /* Startup chime */
    speaker_tone(1000, 80); sleep_ms(30);
    speaker_tone(1500, 80); sleep_ms(30);
    speaker_tone(2000, 120);

    /* NOTE: EPD_Init()/EPD_Clear() are NOT called here. The e-ink panel is
     * owned exclusively by the Display task (core 1), which initialises it in
     * its prologue (display_init_panel). DEV_Module_Init() above already brought
     * up the SPI bus on the boot core, which is fine (one-time, pre-scheduler). */
    rng_seed();

    /* ═══════════ Hand control to the FreeRTOS scheduler ═══════════
     * Everything above ran "bare metal": plain function calls on the boot core,
     * no kernel yet. Now we create our application task and start the scheduler.
     *
     * WHY create a task instead of just continuing? Because the FreeRTOS-flavour
     * cyw43/Wi-Fi/Bluetooth driver spins up its OWN background task, which only
     * works once the scheduler is running. So cyw43_arch_init() and the whole
     * app loop must happen *inside* a task, not here in plain main().
     *
     * vTaskCoreAffinitySet pins this task to core 0 (bit 0). vTaskStartScheduler
     * NEVER returns — from here on the kernel owns the CPUs and runs our tasks. */
    xTaskCreate(app_task, "app", APP_TASK_STACK_WORDS, NULL, APP_TASK_PRIO, &g_app_task);
    vTaskCoreAffinitySet(g_app_task, (1u << 0));   /* run on core 0 */
    vTaskStartScheduler();

    /* Unreachable: the scheduler only returns if it ran out of heap for the idle
     * task, which our hooks/heap size prevent. Hang loudly if it ever happens. */
    printf("FATAL: scheduler returned\n");
    for (;;) { }
}

/* ─── The application (UI) task ─── */
static void app_task(void *param) {
    (void)param;   /* we don't use the task argument; cast-to-void silences the warning */

    /* Init CYW43 early — even without WiFi, the chip's SPI CS shares
       GPIO 29 (ADC3/VSYS sense) and will hold it low if uninitialised. Under the
       FreeRTOS cyw43 arch this also starts the networking background task. */
    if (cyw43_arch_init()) {
        printf("WARNING: CYW43 init failed — battery reads may be 0\n");
    }
    /* WiFi is NOT auto-connected at boot — that blocked startup for up to ~15s.
       Credentials are cached (saved-networks store, seeded with Moop Ship +
       MoopsterCell), so connecting from the Network menu needs no password
       entry. The clock NTP-syncs whenever WiFi is connected. */

    battery_init();
    mpu_init();

    /* ═══ Phase 2: spin up the Input, Display, and Housekeeping tasks ═══
     * From here on THIS task is the UI task: it renders, runs the state machine,
     * gets input from the Input task's queue, and hands finished frames to the
     * Display task (core 1). cyw43_arch_init() above stays on this (core 0) task,
     * which is where the cyw43/Wi-Fi/BT background task must live. */
    rtos_tasks_start();

    /* Wait (explicit handshake, not a timing guess) for the Display task on core 1
     * to register with the flash lockout before saved_load() — which may write
     * flash on first boot — so that write is SMP-safe (core 1 parked, no XIP fault). */
    rtos_wait_flash_ready();
    saved_load();   /* load cached WiFi networks (seeds Moop Ship + MoopsterCell) */

    /* Social: bake our id into the beacon, and resume scanning if it was left on. */
    wetgreg_social_set_self(g_saved.wetgreg_id);
    if (g_saved.social_on) {
        if (!wetgreg_bt_active()) wetgreg_bt_init();
        bt_enabled = true;
        wetgreg_social_set_self(g_saved.wetgreg_id);   /* re-push now that BT is up */
        wetgreg_social_enable(true);
    }

    /* ─── State machine ─── */
    uint8_t state = STATE_SPLASH;      /* one-shot boot splash, then STATE_OCTOPUS */
    uint32_t splash_t0 = 0;            /* 0 = splash not started yet */
    uint32_t intro_t0  = 0;            /* 0 = intro not started (re-armed on exit) */
    uint32_t frame_idx = 0;
    int qi = pick_quote();
    int menu_sel = 0;
    int mood_sel = 0;  /* 0 = ALL, 1-16 = specific mood */

    int snd_sel = 0;
    int social_sel = 0;
    int disp_sel = 0;
    int anim_sel = 0;
    int set_sel = 0;
    int met_sel = 0;
    int nearby_sel = 0;

    uint32_t tap_t0 = 0;   /* CENTER-mash sleep trigger: window start ... */
    int      tap_n  = 0;   /* ... and presses seen inside it (see SLEEP_TAP_*) */

    /* Sub-screen input: block on the Input task's event queue for up to `ms` ms.
     * The do-while(0) PRESERVES the old `break;` semantics in every screen body —
     * a `break` leaves the poll, then the case's trailing `break` triggers the
     * redraw — but with NO busy-wait: the UI sleeps until a press arrives or the
     * timeout elapses (the Input task captures presses meanwhile). */
    #define POLL_INPUT(ms) \
        do { \
            uint8_t inp; \
            if (!ui_get_input(&inp, (ms))) break;
    #define POLL_END } while (0);

    while (true) {
        /* Default every screen to the WIDE canvas; the tall layouts override it
         * for themselves. NOTE (Phase 2): accelerometer sampling
         * (orientation_update) runs in the Housekeeping task, and Wi-Fi/BT are
         * serviced by the cyw43 FreeRTOS background task. */
        set_canvas_wide();

        switch (state) {

        /* ════════ BOOT SPLASH ════════ */
        case STATE_SPLASH: {
            if (splash_t0 == 0) {
                /* Hold the first frame until the accel classifier's first
                 * verdict lands (~300 ms; see orientation_update's boot
                 * prime) — otherwise the splash always STARTS in the default
                 * landscape hold and flips to portrait mid-entrance. Times
                 * out fast when no verdict can come (device flat, no accel);
                 * a press during the wait stays queued and skips on frame 1. */
                for (int w = 0; !g_orient_primed && w < 14; w++)
                    vTaskDelay(pdMS_TO_TICKS(50));
                uint32_t t = to_ms_since_boot(get_absolute_time());
                splash_t0 = t ? t : 1;
                splash_bubbles_init();
            }
            uint32_t elapsed = to_ms_since_boot(get_absolute_time()) - splash_t0;

            bool skip = false;
            if (elapsed < SPLASH_TOTAL_MS) {
                render_splash(elapsed, frame_idx);
                draw_orient_hud();
                transpose_to_display();
                display_render();
                frame_idx++;
                /* Sleep one frame on the input queue — any press skips. The wait
                 * keeps the submit cadence above the panel drain so the Input
                 * task is never starved (see the STATE_MOTION note). */
                uint8_t inp;
                skip = ui_get_input(&inp, SPLASH_FRAME_MS);
            }
            if (skip || elapsed >= SPLASH_TOTAL_MS) {
                if (skip) speaker_tone(1319, 80);
                qi = pick_quote();
                frame_idx = 0;
                wake_screen();
                state = STATE_OCTOPUS;
            }
            break;
        }

        /* ════════ OCTOPUS MAIN SCREEN ════════ */
        case STATE_OCTOPUS: {
            const Quote *q = &quotes[qi];
            const uint8_t *cycle = mood_cycle(q->mood);
            uint8_t expr = cycle[frame_idx % 4];

            if (expr == EXPR_OPEN && frame_idx > 0)
                qi = pick_quote();

            /* Skip the redraw entirely while pocketed/idle — the e-ink keeps
             * showing the last frame for free; this is the main power saver. */
            if (!g_screen_idle) {
                if (orientation_is_tall()) {
                    /* Longways layout draws its own 2-row status bar (wifi + batt,
                     * date + time), quote, and octopus at the bottom. */
                    render_octopus_tall(&quotes[qi], expr, frame_idx);
                } else {
                    render_frame(&quotes[qi], expr, frame_idx);
                    draw_wifi_icon(0, 1, wifi_connected);   /* top-left */
                    {
                        int soc_x = 18;
                        if (wetgreg_bt_state() == BT_PAIRED) { draw_bt_icon(18, 1); soc_x = 32; }
                        if (wetgreg_social_active()) draw_social_icon(soc_x, 1);
                    }
                    draw_battery_icon(234, 1);               /* top-right */
                    {
                        char sbuf[20];
                        snprintf(sbuf, sizeof(sbuf), "STEPS %lu",
                                 (unsigned long)steps_today);
                        draw_text(5, 113, sbuf, IMG_W);      /* left of the screen */
                    }
                    draw_text(175, 113, "DOWN:MENU", IMG_W);
                }
                draw_orient_hud();   /* calibration overlay (ORIENT_DEBUG) */
                transpose_to_display();

                display_render();
                bt_push_status();    /* keep a paired phone's mood/steps read fresh */
                frame_idx++;
            }

            /* UI tick loop (~3 s, then re-render so the mouth animates). The
             * accelerometer/idle are sampled by the Housekeeping task — we just
             * watch the snapshot's orientation + the g_screen_idle flag and drain
             * the Input task's event queue. The ~300 ms refresh runs on core 1, so
             * this loop keeps reacting to presses the whole time. */
            sensor_snapshot_t s; rtos_snapshot_get(&s);
            uint8_t o0    = s.orientation;
            bool    idle0 = g_screen_idle;
            /* While charging, animate ~10x less often (each tick is ~15 ms, so
             * 200=~3 s normally, 2000=~30 s on USB) to cut e-ink refresh load and
             * let the cell charge. Input still polled every tick, so it stays
             * responsive — only the idle animation cadence slows. */
            int octo_ticks = g_on_usb ? 2000 : 200;
            for (int i = 0; i < octo_ticks && state == STATE_OCTOPUS; i++) {
                uint8_t inp;
                bool got = ui_get_input(&inp, 15);     /* ~15 ms tick; sleeps efficiently */
                rtos_snapshot_get(&s);
                /* Idle flag is owned by Housekeeping (it freezes redraws when
                 * pocketed). Just re-enter the case on a change so the render
                 * gate (if !g_screen_idle) re-evaluates — do NOT wake_screen here
                 * or we'd fight HK and redraw forever while idle. */
                if (g_screen_idle != idle0) break;
                if (s.orientation != o0) { wake_screen(); break; }   /* rotated → user present, redraw */
                /* Pseudo-off: no significant motion (and no presses — those
                 * reset last_motion_ms via wake_screen) for 5 min → Greg dozes
                 * off. Guarded on mpu_ok twice over: without an accel,
                 * viewing_update pins last_motion_ms to now anyway.
                 *
                 * Read the last_motion_ms GLOBAL here, NOT the snapshot copy:
                 * wake_screen() (this task) refreshes the global the moment
                 * Greg wakes, but Housekeeping republishes the snapshot only
                 * every ~50 ms — after waking from an INACTIVITY nap the stale
                 * copy is still ≥5 min old, and the first tick would put Greg
                 * straight back to sleep (field bug: wake needed two presses).
                 * Aligned 32-bit read — atomic on the M33, benign race. */
                if (s.mpu_ok &&
                    to_ms_since_boot(get_absolute_time()) - last_motion_ms >= SLEEP_IDLE_MS) {
                    state = STATE_SLEEP;
                    break;
                }
                wifi_rejoin_tick();                    /* deferred post-wake wifi rejoin */
                if (wetgreg_bt_active() && wetgreg_bt_take_command() >= 0) {
                    qi = pick_quote();   /* phone poked us → fresh quote */
                    speaker_tone(1600, 60);
                    wake_screen(); break;
                }
                /* Another WetGreg in range? Interrupt the mood cycle. */
                if (g_saved.social_on) {
                    wetgreg_peer_t pr;
                    if (wetgreg_social_poll(&pr)) {
                        if (pr.hello_to_me) {
                            met_record(pr.id, MET_HELLO_RECV, wetgreg_today());
                            g_social_peer = pr.id; g_social_peer_rssi = pr.rssi;
                            g_play_emote = pr.emote ? pr.emote : EMOTE_WAVE;
                            g_play_incoming = true; g_play_next = STATE_SOCIAL_RECV;
                            speaker_tone(1800, 90);
                            wake_screen(); state = STATE_EMOTE_PLAY; break;
                        } else if (met_should_greet(pr.id)) {
                            g_social_peer = pr.id; g_social_peer_rssi = pr.rssi;
                            speaker_tone(1200, 100);
                            wake_screen(); state = STATE_SOCIAL_PROMPT; break;
                        }
                    }
                }
                if (got) {
                    wake_screen();                      /* any press = user present */
                    if (inp == INPUT_DOWN) {
                        state = STATE_MENU; menu_sel = 0; speaker_tone(800, 50);
                        break;                          /* re-render into the menu */
                    }
                    if (inp == INPUT_CENTER) {
                        /* 5 quick CENTER presses → pseudo-off. Do NOT break/re-
                         * render on a tap: a panel refresh here busies the display,
                         * and the Input task then COALESCES every following tap into
                         * one while it waits (fewer than both buffers free). That is
                         * why it used to take 10+ mashes — only ~1 tap per ~0.5 s
                         * refresh landed. Staying on the (already-drawn) octopus keeps
                         * the panel idle, so each human tap emits immediately and a
                         * normal 5-tap burst engages well inside the 5 s window. */
                        uint32_t now_ms = to_ms_since_boot(get_absolute_time());
                        if (tap_n == 0 || now_ms - tap_t0 > SLEEP_TAP_WINDOW_MS) {
                            tap_t0 = now_ms; tap_n = 1;
                        } else if (++tap_n >= SLEEP_TAP_N) {
                            tap_n = 0;
                            state = STATE_SLEEP;
                        }
                        speaker_tone(1319 + 90 * tap_n, 70);   /* per-tap blip, rising with the count */
                        if (state == STATE_SLEEP) break;   /* transition → run the SLEEP case */
                        continue;                          /* panel stays idle for the next tap */
                    }
                    break;                              /* any other press: re-render */
                }
            }
            break;
        }

        /* ════════ SLEEP / PSEUDO-OFF ════════ */
        case STATE_SLEEP: {
            if (!g_power_sleep) {
                /* Entry: one still — Greg in his nightcap — then power down.
                 * No animation on purpose: the e-ink holds the image free. */
                render_sleep_screen();
                draw_orient_hud();
                transpose_to_display();
                display_render();
                /* Descending "nighty night" chime while still at full clock. */
                speaker_tone(988, 70); speaker_tone(784, 70); speaker_tone(523, 110);
                power_sleep_enter();
                uint8_t drain;                       /* eat any leftover mashing so */
                while (ui_get_input(&drain, 0)) { }  /* a 6th tap can't insta-wake  */
            }
            /* Doze. ONLY CENTER wakes him — motion, other keys, phone pokes and
             * social sightings are all ignored (the radios are off anyway). */
            uint8_t inp;
            if (ui_get_input(&inp, 60000) && inp == INPUT_CENTER) {
                power_sleep_exit();
                speaker_tone(784, 60); speaker_tone(1047, 60); speaker_tone(1319, 100);
                qi = pick_quote();
                frame_idx = 0;
                wake_screen();
                state = STATE_OCTOPUS;
            }
            break;
        }

        /* ════════ MENU ════════ */
        case STATE_MENU: {
            /* The Input task already converts a held UP/DOWN into repeat events
             * and makes CENTER/LEFT strict one-shots, so this loop just DRAINS
             * the event queue and repaints. Because the ~300 ms refresh runs on
             * core 1, presses are never lost during a repaint. */
            bool was_tall  = orientation_is_tall();
            bool need_draw = true;
            for (;;) {
                if (need_draw) {
                    if (orientation_is_tall()) {
                        render_menu_tall(menu_sel);
                    } else {
                        const Quote *q = &quotes[qi];
                        render_frame(q, mood_cycle(q->mood)[frame_idx % 4], frame_idx);
                        render_menu(menu_sel);
                    }
                    transpose_to_display();
                    display_render();
                    need_draw = false;
                }

                uint8_t inp;
                bool got = ui_get_input(&inp, 200);   /* wake on a press, or re-check rotation */
                if (orientation_is_tall() != was_tall) {   /* rotated → redraw */
                    was_tall = orientation_is_tall(); need_draw = true; continue;
                }
                if (!got) continue;

                /* Fold a run of held UP/DOWN repeats into a SINGLE move so a fast
                 * scroll repaints once at the end (not once per ~120 ms repeat
                 * event, which would lag behind the ~300 ms e-ink refresh). Drains
                 * everything queued right now; a CENTER/LEFT ends the scroll and is
                 * then handled below. */
                if (inp == INPUT_UP || inp == INPUT_DOWN) {
                    int delta = 0;
                    do {
                        if      (inp == INPUT_UP)   delta -= 1;
                        else if (inp == INPUT_DOWN) delta += 1;
                        else break;                       /* CENTER/LEFT terminates the scroll */
                        inp = INPUT_NONE;
                    } while (ui_get_input(&inp, 0));       /* non-blocking drain */
                    if (delta) {
                        menu_sel = ((menu_sel + delta) % MENU_COUNT + MENU_COUNT) % MENU_COUNT;
                        speaker_tone(620, 12);
                        need_draw = true;
                    }
                    if (inp != INPUT_CENTER && inp != INPUT_LEFT) continue;  /* only moves were queued */
                    /* else fall through with inp = CENTER/LEFT */
                }

                if (inp == INPUT_CENTER) {
                    speaker_tone(1000, 40);
                    switch (menu_sel) {
                        case MENU_IDX_MOOD: state = STATE_MOOD_SELECT; mood_sel = current_mood + 1; break;
                        case MENU_IDX_ANIM: anim_sel = 0; state = STATE_ANIM_MENU; break;
                        case MENU_IDX_SOUND: state = STATE_SOUND; snd_sel = 0; break;
                        case MENU_IDX_MOTION: state = STATE_MOTION; break;
                        case MENU_IDX_INFO: state = STATE_INFO; break;
                        case MENU_IDX_SOCIAL: social_sel = 0; state = STATE_SOCIAL; break;
                        case MENU_IDX_SETTINGS: set_sel = 0; state = STATE_SETTINGS; break;
                        default: state = STATE_OCTOPUS; break;
                    }
                    break;
                }
                if (inp == INPUT_LEFT) {
                    speaker_tone(500, 40); state = STATE_OCTOPUS; break;
                }
            }
            break;
        }

        /* ════════ ANIMATIONS SUBMENU ════════ */
        case STATE_ANIM_MENU: {
            render_anim_menu(anim_sel);
            transpose_to_display();
            display_render();
            POLL_INPUT(4000)
                if (inp == INPUT_UP)   { anim_sel = (anim_sel - 1 + ANIM_MENU_COUNT) % ANIM_MENU_COUNT; speaker_tone(600, 30); break; }
                if (inp == INPUT_DOWN) { anim_sel = (anim_sel + 1) % ANIM_MENU_COUNT; speaker_tone(600, 30); break; }
                if (inp == INPUT_LEFT) { state = STATE_MENU; speaker_tone(500, 50); break; }
                if (inp == INPUT_CENTER) {
                    speaker_tone(1000, 40);
                    if (anim_sel == ANIM_ITEM_INTRO) {
                        intro_t0 = 0; frame_idx = 0;
                        state = STATE_INTRO;
                    } else {
                        state = STATE_MENU;
                    }
                    break;
                }
            POLL_END
            break;
        }

        /* ════════ GREG INTRO STORY ANIMATION ════════ */
        case STATE_INTRO: {
            if (intro_t0 == 0) {
                uint32_t t = to_ms_since_boot(get_absolute_time());
                intro_t0 = t ? t : 1;
                splash_bubbles_init();
            }
            uint32_t elapsed = to_ms_since_boot(get_absolute_time()) - intro_t0;

            bool leave = false;
            if (elapsed < INTRO_TOTAL_MS) {
                render_intro(elapsed, frame_idx);
                draw_orient_hud();
                transpose_to_display();
                display_render();
                frame_idx++;
                /* Sleep one frame on the input queue — any press exits, and the
                 * wait keeps the submit cadence above the panel drain so the
                 * Input task is never starved (same pattern as the splash). */
                uint8_t inp;
                leave = ui_get_input(&inp, INTRO_FRAME_MS);
            }
            if (leave || elapsed >= INTRO_TOTAL_MS) {
                if (leave) speaker_tone(1319, 80);
                intro_t0 = 0;              /* re-arm so the menu can replay it */
                qi = pick_quote();
                frame_idx = 0;
                wake_screen();
                state = STATE_OCTOPUS;
            }
            break;
        }

        /* ════════ SETTINGS SUBMENU ════════ */
        case STATE_SETTINGS: {
            render_settings_menu(set_sel);
            transpose_to_display();
            display_render();
            POLL_INPUT(4000)
                if (inp == INPUT_UP)   { set_sel = (set_sel - 1 + SET_MENU_COUNT) % SET_MENU_COUNT; speaker_tone(600, 30); break; }
                if (inp == INPUT_DOWN) { set_sel = (set_sel + 1) % SET_MENU_COUNT; speaker_tone(600, 30); break; }
                if (inp == INPUT_LEFT) { state = STATE_MENU; speaker_tone(500, 50); break; }
                if (inp == INPUT_CENTER) {
                    speaker_tone(1000, 40);
                    switch (set_sel) {
                        case SET_ITEM_NETWORK:   state = STATE_NET_MENU; break;
                        case SET_ITEM_BLUETOOTH: state = STATE_BLUETOOTH; break;
                        case SET_ITEM_DISPLAY:   disp_sel = 0; state = STATE_DISPLAY; break;
                        case SET_ITEM_SET_TIME:
                            rtc_get_datetime(&settime_dt); settime_field = 0;
                            settime_editing = false;
                            state = STATE_SET_TIME; break;
                        case SET_ITEM_RESET:     state = STATE_RESET_CONFIRM; break;
                        default:                 state = STATE_MENU; break;
                    }
                    break;
                }
            POLL_END
            break;
        }

        /* ════════ FACTORY RESET CONFIRMATION ════════ */
        case STATE_RESET_CONFIRM: {
            render_reset_confirm();
            transpose_to_display();
            display_render();
            POLL_INPUT(4000)
                if (inp == INPUT_LEFT) { state = STATE_SETTINGS; speaker_tone(500, 50); break; }
                if (inp == INPUT_CENTER) {
                    /* Re-seed the factory defaults (wetgreg_id 0 → a fresh
                     * identity is generated on the next boot) and persist, then
                     * reboot so every RAM-side setting (mood, steps, trims)
                     * starts over and the boot splash greets the "new" Greg. */
                    speaker_tone(400, 300);
                    saved_seed_defaults();
                    saved_write_flash();
                    vTaskDelay(pdMS_TO_TICKS(400));   /* let the tone land */
                    watchdog_reboot(0, 0, 0);
                    for (;;) vTaskDelay(pdMS_TO_TICKS(100));   /* await reboot */
                }
            POLL_END
            break;
        }

        /* ════════ DISPLAY SETTINGS (auto-rotate + orientation) ════════ */
        case STATE_DISPLAY: {
            render_display_menu(disp_sel);
            transpose_to_display();
            display_render();
            POLL_INPUT(4000)
                if (inp == INPUT_UP)   { disp_sel = (disp_sel - 1 + DISP_MENU_COUNT) % DISP_MENU_COUNT; speaker_tone(600, 30); break; }
                if (inp == INPUT_DOWN) { disp_sel = (disp_sel + 1) % DISP_MENU_COUNT; speaker_tone(600, 30); break; }
                if (inp == INPUT_LEFT) { state = STATE_SETTINGS; speaker_tone(500, 50); break; }
                if (inp == INPUT_CENTER) {
                    if (disp_sel == DISP_ITEM_AUTO) {
                        /* Toggle auto-rotate. Turning it OFF locks to the current hold. */
                        g_auto_rotate = !g_auto_rotate;
                        if (!g_auto_rotate) g_manual_orient = (uint8_t)g_orientation;
                        speaker_tone(1000, 50);
                    } else if (disp_sel == DISP_ITEM_ORIENT) {
                        /* Cycle WIDE → WIDE FLIP → TALL; picking one locks auto off. */
                        g_auto_rotate = false;
                        g_manual_orient = (uint8_t)(((int)g_manual_orient + 1) % 3);
                        /* Apply immediately so the menu redraws in the new hold. */
                        g_orientation = (int)g_manual_orient;
                        input_set_rotation(ORIENT_CFG[g_orientation].in_rot);
                        speaker_tone(1200, 50);
                    } else {
                        state = STATE_SETTINGS; speaker_tone(500, 50); break;
                    }
                    /* Persist the display settings. */
                    g_saved.auto_rotate = g_auto_rotate ? 1 : 0;
                    g_saved.manual_orient = g_manual_orient;
                    saved_write_flash();
                    break;
                }
            POLL_END
            break;
        }

        /* ════════ SET DATE / TIME ════════ */
        case STATE_SET_TIME: {
            render_set_time();
            transpose_to_display();
            display_render();

            POLL_INPUT(4000)
                if (settime_editing) {
                    /* EDIT: C/L is sel/back — confirm the value and drop to NAV. */
                    if (inp == INPUT_UP || inp == INPUT_DOWN) {
                        int d = (inp == INPUT_UP) ? 1 : -1;
                        switch (settime_field) {
                            case 0: settime_dt.year += d; break;
                            case 1: settime_dt.month = (settime_dt.month - 1 + 12 + d) % 12 + 1; break;
                            case 2: settime_dt.day   = (settime_dt.day   - 1 + 31 + d) % 31 + 1; break;
                            case 3: settime_dt.hour  = (settime_dt.hour  + 24 + d) % 24; break;
                            case 4: settime_dt.min   = (settime_dt.min   + 60 + d) % 60; break;
                        }
                        speaker_tone(700, 20); break;
                    }
                    if (inp == INPUT_CENTER || inp == INPUT_LEFT) {
                        settime_editing = false; speaker_tone(600, 30); break;
                    }
                } else {
                    /* NAV: U/D pick a field, C edits it, L commits and goes back. */
                    if (inp == INPUT_UP)   { settime_field = (settime_field + 4) % 5; speaker_tone(600, 30); break; }
                    if (inp == INPUT_DOWN) { settime_field = (settime_field + 1) % 5; speaker_tone(600, 30); break; }
                    if (inp == INPUT_CENTER) { settime_editing = true; speaker_tone(900, 40); break; }
                    if (inp == INPUT_LEFT) {
                        settime_dt.dotw = 0; settime_dt.sec = 0;
                        rtc_set_datetime(&settime_dt);
                        ntp_synced = false;        /* manually set */
                        speaker_tone(1000, 60);
                        state = STATE_SETTINGS; break;
                    }
                }
            POLL_END
            break;
        }

        /* ════════ MOOD SELECT ════════ */
        case STATE_MOOD_SELECT: {
            render_mood_select(mood_sel);
            transpose_to_display();
            display_render();

            POLL_INPUT(4000)
                if (inp == INPUT_UP) {
                    mood_sel = (mood_sel - 1 + MOOD_COUNT + 1) % (MOOD_COUNT + 1);
                    speaker_tone(600, 30); break;
                } else if (inp == INPUT_DOWN) {
                    mood_sel = (mood_sel + 1) % (MOOD_COUNT + 1);
                    speaker_tone(600, 30); break;
                } else if (inp == INPUT_CENTER) {
                    current_mood = mood_sel == 0 ? -1 : mood_sel - 1;
                    qi = pick_quote();
                    frame_idx = 0;  /* reset animation */
                    state = STATE_OCTOPUS;  /* return to octopus */
                    speaker_tone(1200, 80);
                    printf("[mood] Selected: %s → back to octopus\n",
                           current_mood < 0 ? "ALL" : mood_names[current_mood]);
                    break;
                } else if (inp == INPUT_LEFT) {
                    state = STATE_MENU;
                    speaker_tone(500, 50); break;
                }
            POLL_END
            break;
        }

        /* ════════ NETWORK STATUS (read-only) ════════ */
        case STATE_NETWORK: {
            render_network_screen();
            transpose_to_display();
            display_render();

            POLL_INPUT(4000)
                if (inp == INPUT_LEFT || inp == INPUT_CENTER) {
                    state = STATE_NET_MENU;
                    speaker_tone(500, 50); break;
                }
            POLL_END
            break;
        }

        /* ════════ SOUND SUBMENU ════════ */
        case STATE_SOUND: {
            render_sound_menu(snd_sel);
            transpose_to_display();
            display_render();

            POLL_INPUT(4000)
                if (inp == INPUT_UP) {
                    snd_sel = (snd_sel - 1 + SND_MENU_COUNT) % SND_MENU_COUNT;
                    speaker_tone(800, 50); break;
                }
                if (inp == INPUT_DOWN) {
                    snd_sel = (snd_sel + 1) % SND_MENU_COUNT;
                    speaker_tone(800, 50); break;
                }
                if (inp == INPUT_LEFT) {
                    if (snd_sel == SND_ITEM_PATTERN) {
                        speaker_set_pattern((speaker_pattern() - 1 + SPEAKER_PATTERN_COUNT) % SPEAKER_PATTERN_COUNT);
                        speaker_tone(800, 30); break;
                    }
                    state = STATE_MENU;
                    speaker_tone(500, 50); break;
                }
                if (inp == INPUT_RIGHT) {
                    if (snd_sel == SND_ITEM_PATTERN) {
                        speaker_set_pattern((speaker_pattern() + 1) % SPEAKER_PATTERN_COUNT);
                        speaker_tone(800, 30); break;
                    }
                }
                if (inp == INPUT_CENTER) {
                    switch (snd_sel) {
                        case SND_ITEM_PATTERN:
                            play_sound_pattern(speaker_pattern());
                            break;
                        case SND_ITEM_ONOFF:
                            speaker_set_enabled(!speaker_enabled());
                            if (speaker_enabled()) speaker_tone(1000, 50);
                            break;
                        case SND_ITEM_VOL:
                            speaker_set_volume((uint8_t)((speaker_volume() + 1) % SPEAKER_VOL_LEVELS));
                            speaker_tone(1000, 100);
                            break;
                        case SND_ITEM_BACK:
                            state = STATE_MENU;
                            speaker_tone(500, 50);
                            break;
                    }
                    break;
                }
            POLL_END
            break;
        }

        /* ════════ DEVICE INFO ════════ */
        case STATE_INFO: {
            render_info_screen();
            transpose_to_display();
            display_render();

            POLL_INPUT(4000)
                if (inp == INPUT_LEFT || inp == INPUT_CENTER) {
                    state = STATE_MENU;
                    speaker_tone(500, 50); break;
                }
                if (inp == INPUT_UP) {        /* calibrate: treat current reading as a full 4.20 V pack */
                    /* Trim against the SAME filtered value the icon shows (g_batt_v),
                     * so the cal target matches the displayed estimator. Off-USB
                     * only — on USB the rail isn't the resting pack voltage. */
                    if (g_batt_pct >= 0 && g_batt_v > 2.0f) {
                        battery_cal_save(g_vsys_cal * 4.20f / g_batt_v);
                        speaker_tone(1500, 80);
                    } else {
                        speaker_tone(300, 120);   /* refuse on USB / before first reading */
                    }
                    break;                    /* re-render with the new CAL/% */
                }
                if (inp == INPUT_DOWN) {      /* reset calibration */
                    battery_cal_save(1.0f); speaker_tone(700, 60);
                    break;
                }
            POLL_END
            break;
        }

        /* ════════ NETWORK SUBMENU ════════ */
        case STATE_NET_MENU: {
            static int net_menu_sel = 0;
            render_net_menu(net_menu_sel);
            transpose_to_display();
            display_render();

            POLL_INPUT(4000)
                if (inp == INPUT_UP) {
                    net_menu_sel = (net_menu_sel - 1 + NET_MENU_COUNT) % NET_MENU_COUNT;
                    speaker_tone(600, 30); break;
                }
                if (inp == INPUT_DOWN) {
                    net_menu_sel = (net_menu_sel + 1) % NET_MENU_COUNT;
                    speaker_tone(600, 30); break;
                }
                if (inp == INPUT_LEFT) {
                    state = STATE_SETTINGS;
                    speaker_tone(500, 50); break;
                }
                if (inp == INPUT_CENTER) {
                    speaker_tone(1000, 50);
                    switch (net_menu_sel) {
                        case NET_ITEM_ONOFF:
                            if (wifi_enabled) wifi_disconnect();
                            else wifi_connect();
                            break;
                        case NET_ITEM_SCAN:
                            wifi_start_scan();
                            state = STATE_NET_SCAN; break;
                        case NET_ITEM_SAVED:
                            saved_sel = 0; state = STATE_SAVED_NETS; break;
                        case NET_ITEM_STATUS:
                            state = STATE_NETWORK; break;
                        case NET_ITEM_BACK:
                            state = STATE_SETTINGS; break;
                    }
                    break;
                }
            POLL_END
            break;
        }

        /* ════════ SAVED NETWORKS ════════ */
        case STATE_SAVED_NETS: {
            render_saved_nets();
            transpose_to_display();
            display_render();
            POLL_INPUT(4000)
                if (g_saved.count > 0 && inp == INPUT_UP) {
                    saved_sel = (saved_sel - 1 + g_saved.count) % g_saved.count;
                    speaker_tone(600, 30); break;
                } else if (g_saved.count > 0 && inp == INPUT_DOWN) {
                    saved_sel = (saved_sel + 1) % g_saved.count;
                    speaker_tone(600, 30); break;
                } else if (g_saved.count > 0 && inp == INPUT_CENTER) {
                    speaker_tone(1000, 50);
                    show_connecting_screen(g_saved.nets[saved_sel].ssid);
                    wifi_connect_to(g_saved.nets[saved_sel].ssid,
                                    g_saved.nets[saved_sel].pass);
                    state = STATE_NETWORK; break;
                } else if (g_saved.count > 0 && inp == INPUT_RIGHT) {
                    saved_forget(saved_sel);     /* forget + persist */
                    speaker_tone(400, 60); break;
                } else if (inp == INPUT_LEFT) {
                    state = STATE_NET_MENU; speaker_tone(500, 50); break;
                }
            POLL_END
            break;
        }

        /* ════════ BLUETOOTH ════════ */
        case STATE_BLUETOOTH: {
            if (bt_enabled && !wetgreg_bt_active()) wetgreg_bt_init();    /* resume if left on */
            bt_state_t shown = (bt_state_t)255;
            int  was_tall  = orientation_is_tall();
            bool shown_en  = !bt_enabled;     /* mismatch forces the first render */
            for (;;) {
                if (wetgreg_bt_state() != shown || orientation_is_tall() != was_tall
                        || bt_enabled != shown_en) {
                    shown = wetgreg_bt_state();
                    was_tall = orientation_is_tall();
                    shown_en = bt_enabled;
                    render_bluetooth(bt_enabled);
                    transpose_to_display();
                    display_render();
                }
                /* Wake on a press, or every 250 ms to re-check the live BLE state
                 * (the cyw43 background task drives pairing/connection changes). */
                uint8_t inp;
                if (!ui_get_input(&inp, 250)) continue;   /* timeout → re-check state */
                if (inp == INPUT_LEFT) {
                    speaker_tone(500, 40); state = STATE_SETTINGS; break;
                }
                if (inp == INPUT_CENTER) {
                    bt_enabled = !bt_enabled;
                    if (bt_enabled) { wetgreg_bt_init(); speaker_tone(1200, 60); }
                    else {
                        /* radio OFF → save power; social shares the radio, so stop it too */
                        if (g_saved.social_on) wetgreg_set_social(false);
                        wetgreg_social_enable(false);
                        wetgreg_bt_stop(); speaker_tone(600, 60);
                    }
                }
            }
            break;
        }

        /* ════════ SOCIAL MENU ════════ */
        case STATE_SOCIAL: {
            render_social_menu(social_sel);
            transpose_to_display();
            display_render();
            POLL_INPUT(4000)
                if (inp == INPUT_UP)   { social_sel = (social_sel - 1 + SOCIAL_MENU_COUNT) % SOCIAL_MENU_COUNT; speaker_tone(600, 30); break; }
                if (inp == INPUT_DOWN) { social_sel = (social_sel + 1) % SOCIAL_MENU_COUNT; speaker_tone(600, 30); break; }
                if (inp == INPUT_LEFT) { state = STATE_MENU; speaker_tone(500, 50); break; }
                if (inp == INPUT_CENTER) {
                    switch (social_sel) {
                    case SOC_ITEM_SCAN: {                  /* toggle persistent scanning */
                        bool on = !g_saved.social_on;
                        wetgreg_set_social(on);
                        if (on) {
                            if (!wetgreg_bt_active()) wetgreg_bt_init();
                            bt_enabled = true;
                            wetgreg_social_set_self(g_saved.wetgreg_id);
                            wetgreg_social_enable(true);
                            speaker_tone(1200, 60);
                        } else {
                            wetgreg_social_enable(false);
                            speaker_tone(600, 60);
                        }
                        break;
                    }
                    case SOC_ITEM_NEARBY:                  /* live scan for in-range WetGregs */
                        nearby_sel = 0; g_nearby_count = 0;
                        state = STATE_SOCIAL_NEARBY; speaker_tone(1000, 40);
                        break;
                    case SOC_ITEM_MET:                     /* who we've met */
                        met_sel = 0;
                        state = STATE_SOCIAL_MET; speaker_tone(1000, 40);
                        break;
                    case SOC_ITEM_NAME:                    /* set name (reroll picker) */
                        g_name_seed = (uint16_t)rng_next();
                        state = STATE_SOCIAL_NAME; speaker_tone(1000, 40);
                        break;
                    default:                               /* BACK */
                        state = STATE_MENU; speaker_tone(500, 50);
                        break;
                    }
                    break;
                }
            POLL_END
            break;
        }

        /* ════════ SET NAME (reroll) ════════ */
        case STATE_SOCIAL_NAME: {
            render_social_name();
            transpose_to_display();
            display_render();
            POLL_INPUT(8000)
                if (inp == INPUT_UP || inp == INPUT_DOWN) {
                    g_name_seed = (uint16_t)rng_next(); speaker_tone(800, 30); break;
                }
                if (inp == INPUT_CENTER) {
                    char nm[24]; wetgreg_auto_name(g_name_seed, nm, sizeof(nm));
                    wetgreg_set_name(nm); speaker_tone(1400, 80);
                    state = STATE_SOCIAL; break;
                }
                if (inp == INPUT_LEFT) { state = STATE_SOCIAL; speaker_tone(500, 50); break; }
            POLL_END
            break;
        }

        /* ════════ WETGREGS MET (social log) ════════ */
        case STATE_SOCIAL_MET: {
            int n = (int)g_saved.met_count;
            uint16_t ids[SOCIAL_MAX]; uint8_t fl[SOCIAL_MAX];
            for (int i = 0; i < n && i < SOCIAL_MAX; i++) { ids[i] = g_saved.met[i].id; fl[i] = g_saved.met[i].flags; }
            render_wetgreg_list("WETGREGS MET", ids, NULL, fl, n, met_sel,
                               "NONE YET - TURN ON SCAN", "U/D  L:BACK");
            transpose_to_display();
            display_render();
            POLL_INPUT(6000)
                if (n > 0 && inp == INPUT_UP)   { met_sel = (met_sel - 1 + n) % n; speaker_tone(600, 30); break; }
                if (n > 0 && inp == INPUT_DOWN) { met_sel = (met_sel + 1) % n; speaker_tone(600, 30); break; }
                if (inp == INPUT_LEFT)          { state = STATE_SOCIAL; speaker_tone(500, 50); break; }
            POLL_END
            break;
        }

        /* ════════ SCAN NEARBY (live, in-range WetGregs) ════════ */
        case STATE_SOCIAL_NEARBY: {
            /* Make sure scanning is live for the duration of this screen. */
            if (!wetgreg_bt_active()) { wetgreg_bt_init(); bt_enabled = true; }
            wetgreg_social_set_self(g_saved.wetgreg_id);
            wetgreg_social_enable(true);

            int last_count = -1, last_sel = -1, last_tall = -1;
            for (;;) {
                /* Drain freshly-seen WetGregs into the nearby list (unique by id). */
                wetgreg_peer_t pr;
                while (wetgreg_social_poll(&pr)) {
                    int f = -1;
                    for (int i = 0; i < g_nearby_count; i++) if (g_nearby_id[i] == pr.id) { f = i; break; }
                    if (f >= 0) { g_nearby_rssi[f] = pr.rssi; }
                    else if (g_nearby_count < NEARBY_MAX) {
                        g_nearby_id[g_nearby_count] = pr.id;
                        g_nearby_rssi[g_nearby_count] = pr.rssi;
                        g_nearby_count++;
                    }
                    /* NOTE: do NOT met_record() here — that flash-writes (parks the
                     * display core) on every advert seen and would softlock. The log
                     * is updated only when a hello is actually sent/received. */
                }
                int tall_now = orientation_is_tall() ? 1 : 0;
                if (g_nearby_count != last_count || nearby_sel != last_sel || tall_now != last_tall) {
                    if (nearby_sel >= g_nearby_count) nearby_sel = g_nearby_count ? g_nearby_count - 1 : 0;
                    render_wetgreg_list("SCAN NEARBY", g_nearby_id, g_nearby_rssi, NULL,
                                       g_nearby_count, nearby_sel,
                                       "SCANNING... NONE YET", "C:EMOTE  L:BACK");
                    transpose_to_display();
                    display_render();
                    last_count = g_nearby_count; last_sel = nearby_sel; last_tall = tall_now;
                }
                uint8_t inp;
                if (!ui_get_input(&inp, 300)) continue;   /* keep scanning between presses */
                if (inp == INPUT_LEFT) {
                    if (!g_saved.social_on) wetgreg_social_enable(false);   /* stop scan if not opted-in */
                    state = STATE_SOCIAL; speaker_tone(500, 50); break;
                }
                if (g_nearby_count > 0 && inp == INPUT_UP)   { nearby_sel = (nearby_sel - 1 + g_nearby_count) % g_nearby_count; speaker_tone(600, 30); }
                if (g_nearby_count > 0 && inp == INPUT_DOWN) { nearby_sel = (nearby_sel + 1) % g_nearby_count; speaker_tone(600, 30); }
                if (g_nearby_count > 0 && inp == INPUT_CENTER) {
                    g_social_peer = g_nearby_id[nearby_sel];
                    g_social_peer_rssi = g_nearby_rssi[nearby_sel];
                    if (!g_saved.social_on) wetgreg_social_enable(false);  /* stop live scan; advertising stays */
                    g_emote_sel = 0; speaker_tone(1000, 50);
                    state = STATE_EMOTE_PICK; break;
                }
            }
            break;
        }

        /* ════════ "A WETGREG APPEARS — SAY HI?" ════════ */
        case STATE_SOCIAL_PROMPT: {
            render_social_card(false);
            transpose_to_display();
            display_render();
            /* Hold the card for ~2 min so there's real time to react. Poll input
             * AND an incoming hello each second; render only on a change (e-ink
             * holds the image). The peer may leave range mid-window — that's fine,
             * saying YES still logs them and best-effort broadcasts our reply. */
            uint32_t start = to_ms_since_boot(get_absolute_time());
            bool was_tall = orientation_is_tall();
            for (;;) {
                if (orientation_is_tall() != was_tall) {   /* rotated → redraw for new layout */
                    was_tall = !was_tall;
                    render_social_card(false); transpose_to_display(); display_render();
                }
                uint8_t inp;
                if (ui_get_input(&inp, 1000)) {
                    if (inp == INPUT_CENTER) {           /* YES — pick an emote (logs on send) */
                        g_emote_sel = 0; state = STATE_EMOTE_PICK; speaker_tone(1000, 50); break;
                    }
                    if (inp == INPUT_LEFT) {             /* NO — cooldown today */
                        met_record(g_social_peer, 0, wetgreg_today());
                        speaker_tone(500, 50); state = STATE_OCTOPUS; wake_screen(); break;
                    }
                }
                wetgreg_peer_t pr;                        /* did THEY greet us first? */
                if (wetgreg_social_poll(&pr) && pr.hello_to_me && pr.id == g_social_peer) {
                    met_record(pr.id, MET_HELLO_RECV, wetgreg_today());
                    g_social_peer_rssi = pr.rssi;
                    g_play_emote = pr.emote ? pr.emote : EMOTE_WAVE;
                    g_play_incoming = true; g_play_next = STATE_SOCIAL_RECV;
                    speaker_tone(1800, 90); state = STATE_EMOTE_PLAY; break;
                }
                if ((uint32_t)(to_ms_since_boot(get_absolute_time()) - start) >= 120000) {
                    met_record(g_social_peer, 0, wetgreg_today());   /* timed out — cooldown */
                    state = STATE_OCTOPUS; wake_screen(); break;
                }
            }
            break;
        }

        /* ════════ "A WETGREG SAYS HELLO!" — RESPOND? ════════ */
        case STATE_SOCIAL_RECV: {
            render_social_card(true);
            transpose_to_display();
            display_render();
            uint32_t start = to_ms_since_boot(get_absolute_time());
            bool was_tall = orientation_is_tall();
            for (;;) {
                if (orientation_is_tall() != was_tall) {
                    was_tall = !was_tall;
                    render_social_card(true); transpose_to_display(); display_render();
                }
                uint8_t inp;
                if (ui_get_input(&inp, 1000)) {
                    if (inp == INPUT_CENTER) {           /* respond with an emote */
                        g_emote_sel = 0; state = STATE_EMOTE_PICK; speaker_tone(1000, 50); break;
                    }
                    if (inp == INPUT_LEFT) {
                        speaker_tone(500, 50); state = STATE_OCTOPUS; wake_screen(); break;
                    }
                }
                if ((uint32_t)(to_ms_since_boot(get_absolute_time()) - start) >= 120000) {
                    state = STATE_OCTOPUS; wake_screen(); break;   /* ~2 min, then dismiss */
                }
            }
            break;
        }

        /* ════════ EMOTE PICKER (send to g_social_peer) ════════ */
        case STATE_EMOTE_PICK: {
            render_emote_pick(g_emote_sel);
            transpose_to_display();
            display_render();
            POLL_INPUT(15000)
                if (inp == INPUT_UP)   { g_emote_sel = (g_emote_sel - 1 + EMOTE_PICK_COUNT) % EMOTE_PICK_COUNT; speaker_tone(600, 30); break; }
                if (inp == INPUT_DOWN) { g_emote_sel = (g_emote_sel + 1) % EMOTE_PICK_COUNT; speaker_tone(600, 30); break; }
                if (inp == INPUT_LEFT) { state = STATE_OCTOPUS; speaker_tone(500, 50); wake_screen(); break; }
                if (inp == INPUT_CENTER) {
                    uint8_t code = (uint8_t)(g_emote_sel + 1);   /* skip EMOTE_NONE */
                    wetgreg_social_send_emote(g_social_peer, code);
                    met_record(g_social_peer, MET_HELLO_SENT, wetgreg_today());
                    g_play_emote = code; g_play_incoming = false; g_play_next = STATE_OCTOPUS;
                    speaker_tone(1600, 80);
                    state = STATE_EMOTE_PLAY; break;
                }
            POLL_END
            break;
        }

        /* ════════ EMOTE PLAYBACK (octopus acts it out) ════════ */
        case STATE_EMOTE_PLAY: {
            char nm[24]; wetgreg_auto_name(g_social_peer, nm, sizeof(nm));
            char cap[40];
            if (g_play_incoming) snprintf(cap, sizeof(cap), "%s SENT YOU", nm);
            else                 snprintf(cap, sizeof(cap), "TO %s", nm);
            for (uint32_t t = 0; t < 6; t++) {           /* ~6 animation frames */
                render_emote_octopus(g_play_emote, cap, t);
                transpose_to_display();
                display_render();
                uint8_t inp;
                if (ui_get_input(&inp, 350) && inp == INPUT_LEFT) break;   /* skip */
            }
            state = g_play_next ? g_play_next : STATE_OCTOPUS;
            if (state == STATE_OCTOPUS) wake_screen();
            break;
        }

        /* ════════ SCAN RESULTS ════════ */
        case STATE_NET_SCAN: {
            wifi_scan_poll();   /* finalize (sort + log) once the async scan ends */

            render_scan_results();
            transpose_to_display();
            display_render();

            POLL_INPUT(scan_in_progress ? 500 : 4000)
                if (inp == INPUT_LEFT) {
                    scan_in_progress = false;
                    state = STATE_NET_MENU;
                    speaker_tone(500, 50); break;
                }
                if (scan_complete && scan_count > 0) {
                    if (inp == INPUT_UP) {
                        scan_sel = (scan_sel - 1 + scan_count) % scan_count;
                        speaker_tone(600, 30); break;
                    }
                    if (inp == INPUT_DOWN) {
                        scan_sel = (scan_sel + 1) % scan_count;
                        speaker_tone(600, 30); break;
                    }
                    if (inp == INPUT_CENTER) {
                        speaker_tone(1000, 50);
                        selected_network = scan_sel;
                        const char *saved_pw = saved_find_pass(scan_results[scan_sel].ssid);
                        if (scan_results[scan_sel].auth_mode == 0) {
                            /* Open network — connect directly */
                            show_connecting_screen(scan_results[scan_sel].ssid);
                            wifi_connect_to(scan_results[scan_sel].ssid, "");
                            state = STATE_NETWORK;
                        } else if (saved_pw) {
                            /* Cached credentials — connect with no password entry */
                            show_connecting_screen(scan_results[scan_sel].ssid);
                            wifi_connect_to(scan_results[scan_sel].ssid, saved_pw);
                            state = STATE_NETWORK;
                        } else {
                            /* Needs password — keyboard */
                            pw_len = 0;
                            pw_buf[0] = '\0';
                            kb_row = 0; kb_col = 0; kb_shift = true;
                            state = STATE_NET_KEYBOARD;
                        }
                        break;
                    }
                }
            POLL_END
            break;
        }

        /* ════════ ON-SCREEN KEYBOARD ════════ */
        case STATE_NET_KEYBOARD: {
            render_keyboard();
            transpose_to_display();
            display_render();

            POLL_INPUT(4000)
                if (inp == INPUT_UP) {
                    if (kb_row > 0) kb_row--;
                    if (kb_row < KB_SPECIAL_ROW && kb_col >= 10) kb_col = 9;
                    speaker_tone(600, 20); break;
                }
                if (inp == INPUT_DOWN) {
                    if (kb_row < KB_SPECIAL_ROW) kb_row++;
                    if (kb_row == KB_SPECIAL_ROW && kb_col >= KB_SP_COUNT) kb_col = KB_SP_COUNT - 1;
                    speaker_tone(600, 20); break;
                }
                if (inp == INPUT_LEFT) {
                    if (kb_row < KB_SPECIAL_ROW)
                        kb_col = (kb_col - 1 + 10) % 10;
                    else
                        kb_col = (kb_col - 1 + KB_SP_COUNT) % KB_SP_COUNT;
                    speaker_tone(600, 20); break;
                }
                if (inp == INPUT_RIGHT) {
                    if (kb_row < KB_SPECIAL_ROW)
                        kb_col = (kb_col + 1) % 10;
                    else
                        kb_col = (kb_col + 1) % KB_SP_COUNT;
                    speaker_tone(600, 20); break;
                }
                if (inp == INPUT_CENTER) {
                    if (kb_row < KB_SPECIAL_ROW) {
                        /* Insert character */
                        if (pw_len < PW_MAX_LEN) {
                            pw_buf[pw_len++] = kb_char_at(kb_row, kb_col, kb_shift);
                            pw_buf[pw_len] = '\0';
                            speaker_tone(1000, 30);
                        }
                    } else {
                        /* Special key */
                        switch (kb_col) {
                            case KB_SP_SHIFT:
                                kb_shift = !kb_shift;
                                speaker_tone(800, 30);
                                break;
                            case KB_SP_SPACE:
                                if (pw_len < PW_MAX_LEN) {
                                    pw_buf[pw_len++] = ' ';
                                    pw_buf[pw_len] = '\0';
                                }
                                speaker_tone(1000, 30);
                                break;
                            case KB_SP_DEL:
                                if (pw_len > 0) pw_buf[--pw_len] = '\0';
                                speaker_tone(500, 30);
                                break;
                            case KB_SP_DONE:
                                speaker_tone(1200, 80);
                                show_connecting_screen(scan_results[selected_network].ssid);
                                wifi_connect_to(scan_results[selected_network].ssid, pw_buf);
                                if (wifi_connected)   /* remember it for next time */
                                    saved_add(scan_results[selected_network].ssid, pw_buf);
                                state = STATE_NETWORK;
                                break;
                            case KB_SP_CANCEL:
                                speaker_tone(500, 50);
                                state = STATE_NET_SCAN;
                                break;
                        }
                    }
                    break;
                }
            POLL_END
            break;
        }

        /* ════════ MOTION / ACCELEROMETER ════════ */
        case STATE_MOTION: {
            static int mot_sel = 0;
            render_motion_menu(mot_sel);
            transpose_to_display();
            display_render();

            /* Live-ish refresh for the accel/step readout. This MUST stay above the
             * ~0.7 s panel refresh: at 500 ms the menu resubmitted frames faster than
             * the display could drain, both buffers never went free at once, and the
             * Input task could never emit a press — so LEFT could never exit and the
             * device appeared locked. 2 s keeps the data fresh without saturating. */
            POLL_INPUT(2000)
                if (inp == INPUT_UP) {
                    mot_sel = (mot_sel - 1 + MOT_MENU_COUNT) % MOT_MENU_COUNT;
                    speaker_tone(600, 20); break;
                }
                if (inp == INPUT_DOWN) {
                    mot_sel = (mot_sel + 1) % MOT_MENU_COUNT;
                    speaker_tone(600, 20); break;
                }
                if (inp == INPUT_LEFT) {
                    state = STATE_MENU;
                    speaker_tone(500, 50); break;
                }
                if (inp == INPUT_CENTER) {
                    switch (mot_sel) {
                        case MOT_ITEM_RESET:
                            step_count = 0;
                            speaker_tone(1000, 50);
                            break;
                        case MOT_ITEM_THRESH:
                            step_threshold += 0.1f;
                            if (step_threshold > 2.5f) step_threshold = 0.8f;
                            speaker_tone(800, 30);
                            break;
                        case MOT_ITEM_I2CSCAN:
                            speaker_tone(1000, 50);
                            render_i2c_scan();
                            transpose_to_display();
                            display_render();
                            /* Wait for any button to go back */
                            POLL_INPUT(30000)
                                speaker_tone(500, 30);
                            POLL_END
                            break;
                        case MOT_ITEM_BACK:
                            state = STATE_MENU;
                            speaker_tone(500, 50);
                            break;
                        default:
                            break;
                    }
                    break;
                }
                if (inp == INPUT_RIGHT && mot_sel == MOT_ITEM_THRESH) {
                    step_threshold += 0.1f;
                    if (step_threshold > 2.5f) step_threshold = 0.8f;
                    speaker_tone(800, 30); break;
                }
            POLL_END
            break;
        }

        } /* end switch */
    }

    /* Unreachable (the while loop above never exits). A FreeRTOS task must NEVER
     * "return" off the end — if it somehow did, we delete it cleanly so the
     * kernel reclaims its memory instead of crashing. */
    vTaskDelete(NULL);
}
