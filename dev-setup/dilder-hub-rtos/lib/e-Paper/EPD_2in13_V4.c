/*****************************************************************************
* | File        :   EPD_2in13_V4.c
* | Author      :   Waveshare team + Dilder project
* | Function    :   2.13inch e-paper V4 (SSD1680)
* | Info        :
*   V1.5 — Flicker-free partial refresh using V3's custom LUT approach.
*
*   The V4's internal LUT has no proper partial waveform — its partial
*   mode still drives a full black-white-black cycle, causing visible
*   flashing every frame.
*
*   Fix: load the same custom partial waveform used by the V3 driver
*   (WF_PARTIAL). This waveform only drives changed pixels with minimal
*   voltage — no black flash, no full-screen redraw.
*
*   The SSD1680 chip is identical between V3 and V4. The only difference
*   is that V4 was intended to use the internal OTP LUT. We override it.
* -----------------------------------------------------------------------------
******************************************************************************/
#include "EPD_2in13_V4.h"
#include "Debug.h"
#include <string.h>

#define EPD_BUF_SIZE  ((((EPD_2in13_V4_WIDTH + 7) / 8)) * EPD_2in13_V4_HEIGHT)

static UBYTE base_seeded = 0;

/* Custom partial-refresh waveform — borrowed from V3 driver.
 * This waveform drives only changed pixels with short, weak pulses.
 * No black flash, no full-screen redraw.
 * 153 bytes LUT + 6 bytes config (gate voltage, source voltage, VCOM) */
static const UBYTE WF_PARTIAL[159] = {
    0x0,0x40,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,
    0x80,0x80,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,
    0x40,0x40,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,
    0x0,0x80,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,
    0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,
    0x14,0x0,0x0,0x0,0x0,0x0,0x0,
    0x1,0x0,0x0,0x0,0x0,0x0,0x0,
    0x1,0x0,0x0,0x0,0x0,0x0,0x0,
    0x0,0x0,0x0,0x0,0x0,0x0,0x0,
    0x0,0x0,0x0,0x0,0x0,0x0,0x0,
    0x0,0x0,0x0,0x0,0x0,0x0,0x0,
    0x0,0x0,0x0,0x0,0x0,0x0,0x0,
    0x0,0x0,0x0,0x0,0x0,0x0,0x0,
    0x0,0x0,0x0,0x0,0x0,0x0,0x0,
    0x0,0x0,0x0,0x0,0x0,0x0,0x0,
    0x0,0x0,0x0,0x0,0x0,0x0,0x0,
    0x0,0x0,0x0,0x0,0x0,0x0,0x0,
    0x22,0x22,0x22,0x22,0x22,0x22,0x0,0x0,0x0,
    0x22,0x17,0x41,0x00,0x32,0x36,
};

/* ─── Low-level helpers ─── */

static void EPD_2in13_V4_Reset(void)
{
    DEV_Digital_Write(EPD_RST_PIN, 1);
    DEV_Delay_ms(20);
    DEV_Digital_Write(EPD_RST_PIN, 0);
    DEV_Delay_ms(2);
    DEV_Digital_Write(EPD_RST_PIN, 1);
    DEV_Delay_ms(20);
}

static void EPD_2in13_V4_SendCommand(UBYTE Reg)
{
    DEV_Digital_Write(EPD_DC_PIN, 0);
    DEV_Digital_Write(EPD_CS_PIN, 0);
    DEV_SPI_WriteByte(Reg);
    DEV_Digital_Write(EPD_CS_PIN, 1);
}

static void EPD_2in13_V4_SendData(UBYTE Data)
{
    DEV_Digital_Write(EPD_DC_PIN, 1);
    DEV_Digital_Write(EPD_CS_PIN, 0);
    DEV_SPI_WriteByte(Data);
    DEV_Digital_Write(EPD_CS_PIN, 1);
}

static void EPD_2in13_V4_ReadBusy(void)
{
    /* Poll at 2 ms (was 10 ms) so we detect refresh-complete up to ~8 ms sooner
     * — shaves latency off every screen update. Under FreeRTOS DEV_Delay_ms
     * yields, so this does not busy-spin the core.
     *
     * BOUNDED: a real refresh finishes in well under 1 s. Cap the wait at ~3 s so
     * a panel whose BUSY line is stuck asserted (bad FPC / shorted-high GP22) can
     * NEVER hang the Display task and freeze the whole UI — it just gives up and
     * carries on (the panel won't show correctly, but the device stays alive and
     * the fast "refresh" time in the log flags the fault). */
    int waited = 0;
    while (DEV_Digital_Read(EPD_BUSY_PIN) == 1) {
        DEV_Delay_ms(2);
        if ((waited += 2) >= 3000) break;
    }
    DEV_Delay_ms(2);
}

static void EPD_2in13_V4_TurnOnDisplay(void)
{
    EPD_2in13_V4_SendCommand(0x22);
    EPD_2in13_V4_SendData(0xf7);
    EPD_2in13_V4_SendCommand(0x20);
    EPD_2in13_V4_ReadBusy();
}

static void EPD_2in13_V4_TurnOnDisplay_Partial(void)
{
    EPD_2in13_V4_SendCommand(0x22);
    EPD_2in13_V4_SendData(0x0f);   /* same as V3 — minimal drive */
    EPD_2in13_V4_SendCommand(0x20);
    EPD_2in13_V4_ReadBusy();
}

static void EPD_2in13_V4_SetWindows(UWORD Xstart, UWORD Ystart, UWORD Xend, UWORD Yend)
{
    EPD_2in13_V4_SendCommand(0x44);
    EPD_2in13_V4_SendData((Xstart>>3) & 0xFF);
    EPD_2in13_V4_SendData((Xend>>3) & 0xFF);
    EPD_2in13_V4_SendCommand(0x45);
    EPD_2in13_V4_SendData(Ystart & 0xFF);
    EPD_2in13_V4_SendData((Ystart >> 8) & 0xFF);
    EPD_2in13_V4_SendData(Yend & 0xFF);
    EPD_2in13_V4_SendData((Yend >> 8) & 0xFF);
}

static void EPD_2in13_V4_SetCursor(UWORD Xstart, UWORD Ystart)
{
    EPD_2in13_V4_SendCommand(0x4E);
    EPD_2in13_V4_SendData(Xstart & 0xFF);
    EPD_2in13_V4_SendCommand(0x4F);
    EPD_2in13_V4_SendData(Ystart & 0xFF);
    EPD_2in13_V4_SendData((Ystart >> 8) & 0xFF);
}

/* Load the custom partial waveform LUT + voltage config */
static void EPD_2in13_V4_LoadPartialLUT(void)
{
    /* LUT register 0x32 — 153 bytes */
    EPD_2in13_V4_SendCommand(0x32);
    for (int i = 0; i < 153; i++)
        EPD_2in13_V4_SendData(WF_PARTIAL[i]);
    EPD_2in13_V4_ReadBusy();

    /* Additional config from the LUT data */
    EPD_2in13_V4_SendCommand(0x3f);
    EPD_2in13_V4_SendData(WF_PARTIAL[153]);

    EPD_2in13_V4_SendCommand(0x03);   /* Gate voltage */
    EPD_2in13_V4_SendData(WF_PARTIAL[154]);

    EPD_2in13_V4_SendCommand(0x04);   /* Source voltage */
    EPD_2in13_V4_SendData(WF_PARTIAL[155]);  /* VSH */
    EPD_2in13_V4_SendData(WF_PARTIAL[156]);  /* VSH2 */
    EPD_2in13_V4_SendData(WF_PARTIAL[157]);  /* VSL */

    EPD_2in13_V4_SendCommand(0x2c);   /* VCOM */
    EPD_2in13_V4_SendData(WF_PARTIAL[158]);
}

/* ─── Public API ─── */

void EPD_2in13_V4_Init(void)
{
    EPD_2in13_V4_Reset();
    DEV_Delay_ms(100);

    EPD_2in13_V4_ReadBusy();
    EPD_2in13_V4_SendCommand(0x12);  /* SWRESET */
    EPD_2in13_V4_ReadBusy();

    EPD_2in13_V4_SendCommand(0x01);  /* Driver output control */
    EPD_2in13_V4_SendData(0xf9);
    EPD_2in13_V4_SendData(0x00);
    EPD_2in13_V4_SendData(0x00);

    EPD_2in13_V4_SendCommand(0x11);  /* Data entry mode */
    EPD_2in13_V4_SendData(0x03);

    EPD_2in13_V4_SetWindows(0, 0, EPD_2in13_V4_WIDTH-1, EPD_2in13_V4_HEIGHT-1);
    EPD_2in13_V4_SetCursor(0, 0);

    EPD_2in13_V4_SendCommand(0x3C);  /* BorderWaveform */
    EPD_2in13_V4_SendData(0x05);

    EPD_2in13_V4_SendCommand(0x21);  /* Display update control */
    EPD_2in13_V4_SendData(0x00);
    EPD_2in13_V4_SendData(0x80);

    EPD_2in13_V4_SendCommand(0x18);  /* Internal temperature sensor */
    EPD_2in13_V4_SendData(0x80);

    EPD_2in13_V4_ReadBusy();
    base_seeded = 0;
}

void EPD_2in13_V4_Init_Fast(void)
{
    EPD_2in13_V4_Init();
}

void EPD_2in13_V4_Clear(void)
{
    UWORD Width = (EPD_2in13_V4_WIDTH + 7) / 8;
    UWORD Height = EPD_2in13_V4_HEIGHT;

    EPD_2in13_V4_SendCommand(0x24);
    for (UWORD j = 0; j < Height; j++)
        for (UWORD i = 0; i < Width; i++)
            EPD_2in13_V4_SendData(0xFF);
    EPD_2in13_V4_SendCommand(0x26);
    for (UWORD j = 0; j < Height; j++)
        for (UWORD i = 0; i < Width; i++)
            EPD_2in13_V4_SendData(0xFF);

    EPD_2in13_V4_TurnOnDisplay();
    base_seeded = 1;
}

void EPD_2in13_V4_Display(UBYTE *Image)
{
    UWORD Width = (EPD_2in13_V4_WIDTH + 7) / 8;
    UWORD Height = EPD_2in13_V4_HEIGHT;

    EPD_2in13_V4_SendCommand(0x24);
    for (UWORD j = 0; j < Height; j++)
        for (UWORD i = 0; i < Width; i++)
            EPD_2in13_V4_SendData(Image[i + j * Width]);

    EPD_2in13_V4_TurnOnDisplay();
}

void EPD_2in13_V4_Display_Base(UBYTE *Image)
{
    UWORD Width = (EPD_2in13_V4_WIDTH + 7) / 8;
    UWORD Height = EPD_2in13_V4_HEIGHT;

    /* Seed both RAM buffers with the same image */
    EPD_2in13_V4_SendCommand(0x24);
    for (UWORD j = 0; j < Height; j++)
        for (UWORD i = 0; i < Width; i++)
            EPD_2in13_V4_SendData(Image[i + j * Width]);
    EPD_2in13_V4_SendCommand(0x26);
    for (UWORD j = 0; j < Height; j++)
        for (UWORD i = 0; i < Width; i++)
            EPD_2in13_V4_SendData(Image[i + j * Width]);

    EPD_2in13_V4_TurnOnDisplay();
    base_seeded = 1;
}

/******************************************************************************
 * Partial refresh — V3-style with custom LUT.
 *
 * The V3 approach:
 *   1. Hardware reset (clears controller state)
 *   2. Load the custom partial waveform LUT
 *   3. Enable RAM ping-pong (controller auto-tracks old/new)
 *   4. Write ONLY the new image to 0x24
 *   5. Trigger with 0x0F (minimal partial drive)
 *
 * The controller diffs 0x24 (new) against 0x26 (previous — auto-copied
 * by ping-pong) and only drives pixels that actually changed. No black
 * flash, no full-screen redraw.
 ******************************************************************************/
void EPD_2in13_V4_Display_Partial(UBYTE *Image)
{
    UWORD Width = (EPD_2in13_V4_WIDTH + 7) / 8;
    UWORD Height = EPD_2in13_V4_HEIGHT;

    if (!base_seeded) {
        EPD_2in13_V4_Init();
        EPD_2in13_V4_Display_Base(Image);
        return;
    }

    /* Hardware reset — clears controller state for clean partial */
    DEV_Digital_Write(EPD_RST_PIN, 0);
    DEV_Delay_ms(2);
    DEV_Digital_Write(EPD_RST_PIN, 1);

    /* Load the custom partial waveform (no black pulse) */
    EPD_2in13_V4_LoadPartialLUT();

    /* Enable RAM ping-pong: after each update, the controller
     * auto-copies 0x24 → 0x26, so next time 0x26 has the "old" frame.
     * No need to manually write old data to 0x26 each frame. */
    EPD_2in13_V4_SendCommand(0x37);
    EPD_2in13_V4_SendData(0x00);
    EPD_2in13_V4_SendData(0x00);
    EPD_2in13_V4_SendData(0x00);
    EPD_2in13_V4_SendData(0x00);
    EPD_2in13_V4_SendData(0x00);
    EPD_2in13_V4_SendData(0x40);   /* bit 6 = ping-pong enable */
    EPD_2in13_V4_SendData(0x00);
    EPD_2in13_V4_SendData(0x00);
    EPD_2in13_V4_SendData(0x00);
    EPD_2in13_V4_SendData(0x00);

    /* Border waveform: follow LUT (no border flash) */
    EPD_2in13_V4_SendCommand(0x3C);
    EPD_2in13_V4_SendData(0x80);

    /* Enable clock + analog for partial mode setup */
    EPD_2in13_V4_SendCommand(0x22);
    EPD_2in13_V4_SendData(0xC0);
    EPD_2in13_V4_SendCommand(0x20);
    EPD_2in13_V4_ReadBusy();

    /* Set window and cursor */
    EPD_2in13_V4_SetWindows(0, 0, EPD_2in13_V4_WIDTH-1, EPD_2in13_V4_HEIGHT-1);
    EPD_2in13_V4_SetCursor(0, 0);

    /* Write ONLY the new image to 0x24.
     * 0x26 already has the previous frame (from ping-pong or base seed). */
    EPD_2in13_V4_SendCommand(0x24);
    for (UWORD j = 0; j < Height; j++)
        for (UWORD i = 0; i < Width; i++)
            EPD_2in13_V4_SendData(Image[i + j * Width]);

    /* Trigger partial update — 0x0F = minimal drive, only changed pixels */
    EPD_2in13_V4_TurnOnDisplay_Partial();
}

void EPD_2in13_V4_Sleep(void)
{
    EPD_2in13_V4_SendCommand(0x10);
    EPD_2in13_V4_SendData(0x01);
    DEV_Delay_ms(100);
}
