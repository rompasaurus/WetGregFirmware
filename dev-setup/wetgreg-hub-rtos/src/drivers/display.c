/**
 * display.c — e-ink panel hand-off (Phase 2 / RTOS).
 *
 * Purpose: own the e-ink panel: EPD driver variant selection, the panel-
 * format buffers, the transpose, and the Display-task hooks.
 *
 * Owns the panel-format buffers and ALL access to the lib/e-Paper driver:
 *   ui_buf        : the UI task's PRIVATE transpose target. transpose_to_display()
 *                   writes the finished panel image here.
 *   display_buf[2]: the two Display-task buffers. display_render() (rtos_tasks.c)
 *                   copies ui_buf into a FREE one and hands its index to the
 *                   Display task (core 1), which does the ~300 ms e-ink refresh.
 *                   Two buffers let the UI prepare the next frame while core 1 is
 *                   still pushing the last one — with zero shared-buffer race
 *                   (ownership passes by queue).
 */
#include "display.h"

#include <string.h>

#include "canvas.h"       /* frame / canvas dims / display_rotation */
#include "rtos_tasks.h"   /* DISP_CMD_* + the hook prototypes */

/* ─── Display variant selection ─── */
#if defined(DISPLAY_V2)
  #include "EPD_2in13_V2.h"
  #define DISP_W EPD_2in13_V2_WIDTH
  #define DISP_H EPD_2in13_V2_HEIGHT
  #define EPD_Init()     EPD_2in13_V2_Init()
  #define EPD_Clear()    EPD_2in13_V2_Clear()
  #define EPD_Partial(b) EPD_2in13_V2_Display_Partial(b)
  #define EPD_Sleep()    EPD_2in13_V2_Sleep()
#elif defined(DISPLAY_V3A)
  #include "EPD_2in13_V3a.h"
  #define DISP_W EPD_2in13_V3a_WIDTH
  #define DISP_H EPD_2in13_V3a_HEIGHT
  #define EPD_Init()     EPD_2in13_V3a_Init()
  #define EPD_Clear()    EPD_2in13_V3a_Clear()
  #define EPD_Partial(b) EPD_2in13_V3a_Display_Partial(b)
  #define EPD_Sleep()    EPD_2in13_V3a_Sleep()
#elif defined(DISPLAY_V4)
  #include "EPD_2in13_V4.h"
  #define DISP_W EPD_2in13_V4_WIDTH
  #define DISP_H EPD_2in13_V4_HEIGHT
  #define EPD_Init()     EPD_2in13_V4_Init()
  #define EPD_Clear()    EPD_2in13_V4_Clear()
  #define EPD_Partial(b) EPD_2in13_V4_Display_Partial(b)
  #define EPD_Sleep()    EPD_2in13_V4_Sleep()
#else
  #include "EPD_2in13_V3.h"
  #define DISP_W EPD_2in13_V3_WIDTH
  #define DISP_H EPD_2in13_V3_HEIGHT
  #define EPD_Init()     EPD_2in13_V3_Init()
  #define EPD_Clear()    EPD_2in13_V3_Clear()
  #define EPD_Partial(b) EPD_2in13_V3_Display_Partial(b)
  #define EPD_Sleep()    EPD_2in13_V3_Sleep()
#endif

#define DISPLAY_BUF_SIZE (((DISP_W + 7) / 8) * DISP_H)
static uint8_t ui_buf[DISPLAY_BUF_SIZE];
static uint8_t display_buf[2][DISPLAY_BUF_SIZE];

/* ─── Transpose landscape → portrait for the e-ink driver ───
 * Map the active canvas onto the fixed 122x250 panel at display_rotation.
 *   90 / 270  → WIDE canvas (250x122): legacy 90 = current view
 *   0  / 180  → TALL canvas (122x250): the longways layout
 * The orientation logic keeps content upright by choosing the rotation. */
void transpose_to_display(void) {
    const int PW = DISP_W;   /* panel width  = 122 */
    const int PH = DISP_H;   /* panel height = 250 */
    uint16_t dst_row_bytes = (PW + 7) / 8;
    memset(ui_buf, 0xFF, sizeof(ui_buf));   /* UI's private buffer (Phase 2) */

    for (int y = 0; y < canvas_h; y++) {
        for (int x = 0; x < canvas_w; x++) {
            int src_byte = y * canvas_row_bytes + x / 8;
            if (!((frame[src_byte] >> (7 - (x & 7))) & 1)) continue;
            int dx, dy;
            switch (display_rotation) {
                case 0:   dx = x;          dy = y;          break;  /* tall */
                case 180: dx = PW - 1 - x; dy = PH - 1 - y; break;  /* tall flipped */
                case 270: dx = PW - 1 - y; dy = x;          break;  /* wide flipped */
                case 90:                                            /* wide (legacy) */
                default:  dx = y;          dy = PH - 1 - x; break;
            }
            if (dx < 0 || dx >= PW || dy < 0 || dy >= PH) continue;
            int dst_byte = dy * dst_row_bytes + dx / 8;
            ui_buf[dst_byte] &= ~(1 << (7 - (dx & 7)));
        }
    }
}

/* ── Display-task hooks (called from rtos_tasks.c) ──────────────────────────
 * display_grab_into : UI side — snapshot the just-transposed ui_buf into the
 *                     free Display buffer `idx` (a cheap 4000-byte copy).
 * display_blit      : Display task (core 1) — the actual ~300 ms panel refresh.
 * display_init_panel: Display task prologue — bring the panel up from core 1. */
void display_grab_into(int idx)   { memcpy(display_buf[idx], ui_buf, sizeof(ui_buf)); }
void display_blit(int idx)        { EPD_Partial(display_buf[idx]); }
void display_init_panel(void)     { EPD_Init(); EPD_Clear(); }
void display_panel_cmd(int cmd) {
    /* STATE_SLEEP power path. Deep sleep retains the image at ~zero panel
     * power; waking needs the full hardware-reset + init + clear (the same
     * clean-base sequence boot uses, so partials after it have no ghosting). */
    if (cmd == DISP_CMD_SLEEP) { EPD_Sleep(); }
    else                       { EPD_Init(); EPD_Clear(); }
}
