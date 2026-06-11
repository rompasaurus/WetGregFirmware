/*****************************************************************************
* | File      	:  	EPD_2in13_V2.c
* | Author      :   Waveshare team
* | Function    :   2.13inch e-paper V2
* | Info        :
*   V2 uses SSD1675B controller. Key differences from V3 (SSD1680):
*     - Different LUT format (70 bytes per group, 5 groups + timing)
*     - Uses 0xC4 for full refresh, 0x0C for partial
*     - Init sends LUT via command 0x32
*     - Partial refresh uses ping-pong RAM with different register
*----------------
* |	This version:   V1.0
* | Date        :   2026-04-12
* -----------------------------------------------------------------------------
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documnetation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to  whom the Software is
# furished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in
# all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS OR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
# THE SOFTWARE.
#
******************************************************************************/
#include "EPD_2in13_V2.h"
#include "Debug.h"

/* Full refresh LUT for SSD1675B */
static const UBYTE WF_Full_2IN13_V2[] = {
    0x80,0x60,0x40,0x00,0x00,0x00,0x00,  /* LUT0: BB */
    0x10,0x60,0x20,0x00,0x00,0x00,0x00,  /* LUT1: BW */
    0x80,0x60,0x40,0x00,0x00,0x00,0x00,  /* LUT2: WB */
    0x10,0x60,0x20,0x00,0x00,0x00,0x00,  /* LUT3: WW */
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,  /* LUT4 */
    0x03,0x03,0x00,0x00,0x02,             /* TP0A/B, SR, TP1A/B */
    0x09,0x09,0x00,0x00,0x02,
    0x03,0x03,0x00,0x00,0x02,
    0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,
};

/* Partial refresh LUT for SSD1675B */
static const UBYTE WF_Partial_2IN13_V2[] = {
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,  /* LUT0: BB — no change */
    0x80,0x00,0x00,0x00,0x00,0x00,0x00,  /* LUT1: BW */
    0x40,0x00,0x00,0x00,0x00,0x00,0x00,  /* LUT2: WB */
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,  /* LUT3: WW — no change */
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,  /* LUT4 */
    0x0A,0x00,0x00,0x00,0x00,             /* Timing */
    0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,
};

/******************************************************************************
function :	Software reset
******************************************************************************/
static void EPD_2in13_V2_Reset(void)
{
    DEV_Digital_Write(EPD_RST_PIN, 1);
    DEV_Delay_ms(20);
    DEV_Digital_Write(EPD_RST_PIN, 0);
    DEV_Delay_ms(2);
    DEV_Digital_Write(EPD_RST_PIN, 1);
    DEV_Delay_ms(20);
}

static void EPD_2in13_V2_SendCommand(UBYTE Reg)
{
    DEV_Digital_Write(EPD_DC_PIN, 0);
    DEV_Digital_Write(EPD_CS_PIN, 0);
    DEV_SPI_WriteByte(Reg);
    DEV_Digital_Write(EPD_CS_PIN, 1);
}

static void EPD_2in13_V2_SendData(UBYTE Data)
{
    DEV_Digital_Write(EPD_DC_PIN, 1);
    DEV_Digital_Write(EPD_CS_PIN, 0);
    DEV_SPI_WriteByte(Data);
    DEV_Digital_Write(EPD_CS_PIN, 1);
}

static void EPD_2in13_V2_ReadBusy(void)
{
    Debug("e-Paper busy\r\n");
    while(1) {
        if(DEV_Digital_Read(EPD_BUSY_PIN)==0)
            break;
        DEV_Delay_ms(10);
    }
    DEV_Delay_ms(10);
    Debug("e-Paper busy release\r\n");
}

static void EPD_2in13_V2_TurnOnDisplay(void)
{
    EPD_2in13_V2_SendCommand(0x22);
    EPD_2in13_V2_SendData(0xC7);
    EPD_2in13_V2_SendCommand(0x20);
    EPD_2in13_V2_ReadBusy();
}

static void EPD_2in13_V2_TurnOnDisplay_Partial(void)
{
    EPD_2in13_V2_SendCommand(0x22);
    EPD_2in13_V2_SendData(0x0C);
    EPD_2in13_V2_SendCommand(0x20);
    EPD_2in13_V2_ReadBusy();
}

static void EPD_2in13_V2_SetLUT(const UBYTE *lut)
{
    EPD_2in13_V2_SendCommand(0x32);
    for(UBYTE i = 0; i < 70; i++)
        EPD_2in13_V2_SendData(lut[i]);
}

static void EPD_2in13_V2_SetWindows(UWORD Xstart, UWORD Ystart, UWORD Xend, UWORD Yend)
{
    EPD_2in13_V2_SendCommand(0x44);
    EPD_2in13_V2_SendData((Xstart>>3) & 0xFF);
    EPD_2in13_V2_SendData((Xend>>3) & 0xFF);

    EPD_2in13_V2_SendCommand(0x45);
    EPD_2in13_V2_SendData(Ystart & 0xFF);
    EPD_2in13_V2_SendData((Ystart >> 8) & 0xFF);
    EPD_2in13_V2_SendData(Yend & 0xFF);
    EPD_2in13_V2_SendData((Yend >> 8) & 0xFF);
}

static void EPD_2in13_V2_SetCursor(UWORD Xstart, UWORD Ystart)
{
    EPD_2in13_V2_SendCommand(0x4E);
    EPD_2in13_V2_SendData(Xstart & 0xFF);

    EPD_2in13_V2_SendCommand(0x4F);
    EPD_2in13_V2_SendData(Ystart & 0xFF);
    EPD_2in13_V2_SendData((Ystart >> 8) & 0xFF);
}

/******************************************************************************
function :	Initialize for SSD1675B
******************************************************************************/
void EPD_2in13_V2_Init(void)
{
    EPD_2in13_V2_Reset();
    DEV_Delay_ms(100);

    EPD_2in13_V2_ReadBusy();
    EPD_2in13_V2_SendCommand(0x12);  /* SWRESET */
    EPD_2in13_V2_ReadBusy();

    EPD_2in13_V2_SendCommand(0x01);  /* Driver output control */
    EPD_2in13_V2_SendData(0xf9);
    EPD_2in13_V2_SendData(0x00);
    EPD_2in13_V2_SendData(0x00);

    EPD_2in13_V2_SendCommand(0x11);  /* Data entry mode */
    EPD_2in13_V2_SendData(0x03);

    EPD_2in13_V2_SetWindows(0, 0, EPD_2in13_V2_WIDTH-1, EPD_2in13_V2_HEIGHT-1);

    EPD_2in13_V2_SendCommand(0x3C);  /* BorderWaveform */
    EPD_2in13_V2_SendData(0x05);

    EPD_2in13_V2_SendCommand(0x21);  /* Display update control */
    EPD_2in13_V2_SendData(0x00);
    EPD_2in13_V2_SendData(0x80);

    EPD_2in13_V2_SendCommand(0x18);  /* Temperature sensor */
    EPD_2in13_V2_SendData(0x80);

    EPD_2in13_V2_SetCursor(0, 0);
    EPD_2in13_V2_ReadBusy();

    EPD_2in13_V2_SetLUT(WF_Full_2IN13_V2);
}

/******************************************************************************
function :	Clear screen
******************************************************************************/
void EPD_2in13_V2_Clear(void)
{
    UWORD Width, Height;
    Width = (EPD_2in13_V2_WIDTH % 8 == 0)? (EPD_2in13_V2_WIDTH / 8 ): (EPD_2in13_V2_WIDTH / 8 + 1);
    Height = EPD_2in13_V2_HEIGHT;

    EPD_2in13_V2_SendCommand(0x24);
    for (UWORD j = 0; j < Height; j++)
        for (UWORD i = 0; i < Width; i++)
            EPD_2in13_V2_SendData(0xFF);

    EPD_2in13_V2_TurnOnDisplay();
}

/******************************************************************************
function :	Full refresh display
******************************************************************************/
void EPD_2in13_V2_Display(UBYTE *Image)
{
    UWORD Width, Height;
    Width = (EPD_2in13_V2_WIDTH % 8 == 0)? (EPD_2in13_V2_WIDTH / 8 ): (EPD_2in13_V2_WIDTH / 8 + 1);
    Height = EPD_2in13_V2_HEIGHT;

    EPD_2in13_V2_SendCommand(0x24);
    for (UWORD j = 0; j < Height; j++)
        for (UWORD i = 0; i < Width; i++)
            EPD_2in13_V2_SendData(Image[i + j * Width]);

    EPD_2in13_V2_TurnOnDisplay();
}

/******************************************************************************
function :	Base image (write to both RAM buffers)
******************************************************************************/
void EPD_2in13_V2_Display_Base(UBYTE *Image)
{
    UWORD Width, Height;
    Width = (EPD_2in13_V2_WIDTH % 8 == 0)? (EPD_2in13_V2_WIDTH / 8 ): (EPD_2in13_V2_WIDTH / 8 + 1);
    Height = EPD_2in13_V2_HEIGHT;

    EPD_2in13_V2_SendCommand(0x24);
    for (UWORD j = 0; j < Height; j++)
        for (UWORD i = 0; i < Width; i++)
            EPD_2in13_V2_SendData(Image[i + j * Width]);

    EPD_2in13_V2_SendCommand(0x26);
    for (UWORD j = 0; j < Height; j++)
        for (UWORD i = 0; i < Width; i++)
            EPD_2in13_V2_SendData(Image[i + j * Width]);

    EPD_2in13_V2_TurnOnDisplay();
}

/******************************************************************************
function :	Partial refresh
******************************************************************************/
void EPD_2in13_V2_Display_Partial(UBYTE *Image)
{
    UWORD Width, Height;
    Width = (EPD_2in13_V2_WIDTH % 8 == 0)? (EPD_2in13_V2_WIDTH / 8 ): (EPD_2in13_V2_WIDTH / 8 + 1);
    Height = EPD_2in13_V2_HEIGHT;

    /* Reset and switch to partial LUT */
    DEV_Digital_Write(EPD_RST_PIN, 0);
    DEV_Delay_ms(2);
    DEV_Digital_Write(EPD_RST_PIN, 1);
    DEV_Delay_ms(2);

    EPD_2in13_V2_SetLUT(WF_Partial_2IN13_V2);

    EPD_2in13_V2_SendCommand(0x37);  /* Write register for display option */
    EPD_2in13_V2_SendData(0x00);
    EPD_2in13_V2_SendData(0x00);
    EPD_2in13_V2_SendData(0x00);
    EPD_2in13_V2_SendData(0x00);
    EPD_2in13_V2_SendData(0x00);
    EPD_2in13_V2_SendData(0x40);     /* RAM Ping-Pong enable */
    EPD_2in13_V2_SendData(0x00);
    EPD_2in13_V2_SendData(0x00);
    EPD_2in13_V2_SendData(0x00);
    EPD_2in13_V2_SendData(0x00);

    EPD_2in13_V2_SendCommand(0x3C);  /* BorderWaveform */
    EPD_2in13_V2_SendData(0x80);

    EPD_2in13_V2_SendCommand(0x22);
    EPD_2in13_V2_SendData(0xC0);
    EPD_2in13_V2_SendCommand(0x20);
    EPD_2in13_V2_ReadBusy();

    EPD_2in13_V2_SetWindows(0, 0, EPD_2in13_V2_WIDTH-1, EPD_2in13_V2_HEIGHT-1);
    EPD_2in13_V2_SetCursor(0, 0);

    EPD_2in13_V2_SendCommand(0x24);
    for (UWORD j = 0; j < Height; j++)
        for (UWORD i = 0; i < Width; i++)
            EPD_2in13_V2_SendData(Image[i + j * Width]);

    EPD_2in13_V2_TurnOnDisplay_Partial();
}

/******************************************************************************
function :	Enter sleep mode
******************************************************************************/
void EPD_2in13_V2_Sleep(void)
{
    EPD_2in13_V2_SendCommand(0x10);
    EPD_2in13_V2_SendData(0x01);
    DEV_Delay_ms(100);
}
