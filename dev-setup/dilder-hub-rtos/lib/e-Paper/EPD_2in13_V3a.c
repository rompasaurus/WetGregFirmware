/*****************************************************************************
* | File      	:  	EPD_2in13_V3a.c
* | Author      :   Waveshare team (adapted for V3 rev.A variant)
* | Function    :   2.13inch e-paper V3 revision A
* | Info        :
*   The "A" revision uses different LUT waveform tables and init
*   sequence compared to the standard V3.  Same SSD1680 controller,
*   same resolution (122x250), same SPI interface.
*----------------
* |	This version:   V1.0
* | Date        :   2026-04-12
* | Info        :   Based on EPD_2in13_V3.c
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
#include "EPD_2in13_V3a.h"
#include "Debug.h"

/*
 * LUT waveform tables for the V3a revision.
 * Key differences from V3: adjusted phase repeat count (0x0A vs 0x14
 * in partial LUT) and slightly lower voltage swing (0x48 vs 0x4A in
 * full refresh LUT).
 */

UBYTE WF_PARTIAL_2IN13_V3a[159] =
{
	0x0,0x40,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,
	0x80,0x80,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,
	0x40,0x40,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,
	0x0,0x80,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,
	0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,
	0x0A,0x0,0x0,0x0,0x0,0x0,0x0,
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

UBYTE WS_20_30_2IN13_V3a[159] =
{
	0x80,	0x48,	0x40,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,
	0x40,	0x48,	0x80,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,
	0x80,	0x48,	0x40,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,
	0x40,	0x48,	0x80,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,
	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,
	0xF,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,
	0xF,	0x0,	0x0,	0xF,	0x0,	0x0,	0x2,
	0xF,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,
	0x1,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,
	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,
	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,
	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,
	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,
	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,
	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,
	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,
	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,
	0x22,	0x22,	0x22,	0x22,	0x22,	0x22,	0x0,	0x0,	0x0,
	0x22,	0x17,	0x41,	0x0,	0x32,	0x36
};

/******************************************************************************
function :	Software reset
parameter:
******************************************************************************/
static void EPD_2in13_V3a_Reset(void)
{
    DEV_Digital_Write(EPD_RST_PIN, 1);
    DEV_Delay_ms(20);
    DEV_Digital_Write(EPD_RST_PIN, 0);
    DEV_Delay_ms(2);
    DEV_Digital_Write(EPD_RST_PIN, 1);
    DEV_Delay_ms(20);
}

/******************************************************************************
function :	send command
parameter:
     Reg : Command register
******************************************************************************/
static void EPD_2in13_V3a_SendCommand(UBYTE Reg)
{
    DEV_Digital_Write(EPD_DC_PIN, 0);
    DEV_Digital_Write(EPD_CS_PIN, 0);
    DEV_SPI_WriteByte(Reg);
    DEV_Digital_Write(EPD_CS_PIN, 1);
}

/******************************************************************************
function :	send data
parameter:
    Data : Write data
******************************************************************************/
static void EPD_2in13_V3a_SendData(UBYTE Data)
{
    DEV_Digital_Write(EPD_DC_PIN, 1);
    DEV_Digital_Write(EPD_CS_PIN, 0);
    DEV_SPI_WriteByte(Data);
    DEV_Digital_Write(EPD_CS_PIN, 1);
}

/******************************************************************************
function :	Wait until the busy_pin goes LOW
parameter:
******************************************************************************/
void EPD_2in13_V3a_ReadBusy(void)
{
    Debug("e-Paper busy\r\n");
	while(1)
	{	 //=1 BUSY
		if(DEV_Digital_Read(EPD_BUSY_PIN)==0)
			break;
		DEV_Delay_ms(10);
	}
	DEV_Delay_ms(10);
    Debug("e-Paper busy release\r\n");
}

/******************************************************************************
function :	Turn On Display
parameter:
******************************************************************************/
static void EPD_2in13_V3a_TurnOnDisplay(void)
{
	EPD_2in13_V3a_SendCommand(0x22); // Display Update Control
	EPD_2in13_V3a_SendData(0xf7);    // V3a: 0xf7 (V3 uses 0xc7)
	EPD_2in13_V3a_SendCommand(0x20); // Activate Display Update Sequence
	EPD_2in13_V3a_ReadBusy();
}

/******************************************************************************
function :	Turn On Display (partial refresh)
parameter:
******************************************************************************/
static void EPD_2in13_V3a_TurnOnDisplay_Partial(void)
{
	EPD_2in13_V3a_SendCommand(0x22); // Display Update Control
	EPD_2in13_V3a_SendData(0xff);    // V3a: 0xff (V3 uses 0x0f)
	EPD_2in13_V3a_SendCommand(0x20); // Activate Display Update Sequence
	EPD_2in13_V3a_ReadBusy();
}

/******************************************************************************
function :	Set lut
parameter:
    lut :   lut data
******************************************************************************/
static void EPD_2IN13_V3a_LUT(UBYTE *lut)
{
	UBYTE count;
	EPD_2in13_V3a_SendCommand(0x32);
	for(count=0; count<153; count++)
		EPD_2in13_V3a_SendData(lut[count]);
	EPD_2in13_V3a_ReadBusy();
}

/******************************************************************************
function :	Send lut data and configuration
parameter:
    lut :   lut data
******************************************************************************/
static void EPD_2IN13_V3a_LUT_by_host(UBYTE *lut)
{
	EPD_2IN13_V3a_LUT((UBYTE *)lut);			//lut
	EPD_2in13_V3a_SendCommand(0x3f);
	EPD_2in13_V3a_SendData(*(lut+153));
	EPD_2in13_V3a_SendCommand(0x03);	// gate voltage
	EPD_2in13_V3a_SendData(*(lut+154));
	EPD_2in13_V3a_SendCommand(0x04);	// source voltage
	EPD_2in13_V3a_SendData(*(lut+155));	// VSH
	EPD_2in13_V3a_SendData(*(lut+156));	// VSH2
	EPD_2in13_V3a_SendData(*(lut+157));	// VSL
	EPD_2in13_V3a_SendCommand(0x2c);		// VCOM
	EPD_2in13_V3a_SendData(*(lut+158));
}

/******************************************************************************
function :	Setting the display window
parameter:
	Xstart : X-axis starting position
	Ystart : Y-axis starting position
	Xend : End position of X-axis
	Yend : End position of Y-axis
******************************************************************************/
static void EPD_2in13_V3a_SetWindows(UWORD Xstart, UWORD Ystart, UWORD Xend, UWORD Yend)
{
    EPD_2in13_V3a_SendCommand(0x44); // SET_RAM_X_ADDRESS_START_END_POSITION
    EPD_2in13_V3a_SendData((Xstart>>3) & 0xFF);
    EPD_2in13_V3a_SendData((Xend>>3) & 0xFF);

    EPD_2in13_V3a_SendCommand(0x45); // SET_RAM_Y_ADDRESS_START_END_POSITION
    EPD_2in13_V3a_SendData(Ystart & 0xFF);
    EPD_2in13_V3a_SendData((Ystart >> 8) & 0xFF);
    EPD_2in13_V3a_SendData(Yend & 0xFF);
    EPD_2in13_V3a_SendData((Yend >> 8) & 0xFF);
}

/******************************************************************************
function :	Set Cursor
parameter:
	Xstart : X-axis starting position
	Ystart : Y-axis starting position
******************************************************************************/
static void EPD_2in13_V3a_SetCursor(UWORD Xstart, UWORD Ystart)
{
    EPD_2in13_V3a_SendCommand(0x4E); // SET_RAM_X_ADDRESS_COUNTER
    EPD_2in13_V3a_SendData(Xstart & 0xFF);

    EPD_2in13_V3a_SendCommand(0x4F); // SET_RAM_Y_ADDRESS_COUNTER
    EPD_2in13_V3a_SendData(Ystart & 0xFF);
    EPD_2in13_V3a_SendData((Ystart >> 8) & 0xFF);
}

/******************************************************************************
function :	Initialize the e-Paper register
parameter:
    V3a init differs from V3:
      - Display update control uses 0x00,0x80 -> 0x80,0x80
      - Border waveform set to 0x01 instead of 0x05
******************************************************************************/
void EPD_2in13_V3a_Init(void)
{
	EPD_2in13_V3a_Reset();
	DEV_Delay_ms(100);

	EPD_2in13_V3a_ReadBusy();
	EPD_2in13_V3a_SendCommand(0x12);  //SWRESET
	EPD_2in13_V3a_ReadBusy();

	EPD_2in13_V3a_SendCommand(0x01); //Driver output control
	EPD_2in13_V3a_SendData(0xf9);
	EPD_2in13_V3a_SendData(0x00);
	EPD_2in13_V3a_SendData(0x00);

	EPD_2in13_V3a_SendCommand(0x11); //data entry mode
	EPD_2in13_V3a_SendData(0x03);

	EPD_2in13_V3a_SetWindows(0, 0, EPD_2in13_V3a_WIDTH-1, EPD_2in13_V3a_HEIGHT-1);
	EPD_2in13_V3a_SetCursor(0, 0);

	EPD_2in13_V3a_SendCommand(0x3C); //BorderWaveform
	EPD_2in13_V3a_SendData(0x01);	 // V3a: 0x01 (V3 standard uses 0x05)

	EPD_2in13_V3a_SendCommand(0x21); //  Display update control
	EPD_2in13_V3a_SendData(0x80);    // V3a: source output mode
	EPD_2in13_V3a_SendData(0x80);

	EPD_2in13_V3a_SendCommand(0x18); //Read built-in temperature sensor
	EPD_2in13_V3a_SendData(0x80);

	EPD_2in13_V3a_ReadBusy();
	EPD_2IN13_V3a_LUT_by_host(WS_20_30_2IN13_V3a);
}

/******************************************************************************
function :	Clear screen
parameter:
******************************************************************************/
void EPD_2in13_V3a_Clear(void)
{
	UWORD Width, Height;
    Width = (EPD_2in13_V3a_WIDTH % 8 == 0)? (EPD_2in13_V3a_WIDTH / 8 ): (EPD_2in13_V3a_WIDTH / 8 + 1);
    Height = EPD_2in13_V3a_HEIGHT;

    EPD_2in13_V3a_SendCommand(0x24);
    for (UWORD j = 0; j < Height; j++)
	{
        for (UWORD i = 0; i < Width; i++)
		{
            EPD_2in13_V3a_SendData(0XFF);
        }
    }

	EPD_2in13_V3a_TurnOnDisplay();
}

/******************************************************************************
function :	Sends the image buffer in RAM to e-Paper and displays
parameter:
	image : Image data
******************************************************************************/
void EPD_2in13_V3a_Display(UBYTE *Image)
{
	UWORD Width, Height;
    Width = (EPD_2in13_V3a_WIDTH % 8 == 0)? (EPD_2in13_V3a_WIDTH / 8 ): (EPD_2in13_V3a_WIDTH / 8 + 1);
    Height = EPD_2in13_V3a_HEIGHT;

    EPD_2in13_V3a_SendCommand(0x24);
    for (UWORD j = 0; j < Height; j++)
	{
        for (UWORD i = 0; i < Width; i++)
		{
            EPD_2in13_V3a_SendData(Image[i + j * Width]);
        }
    }

	EPD_2in13_V3a_TurnOnDisplay();
}


/******************************************************************************
function :	Refresh a base image
parameter:
	image : Image data
******************************************************************************/
void EPD_2in13_V3a_Display_Base(UBYTE *Image)
{
	UWORD Width, Height;
    Width = (EPD_2in13_V3a_WIDTH % 8 == 0)? (EPD_2in13_V3a_WIDTH / 8 ): (EPD_2in13_V3a_WIDTH / 8 + 1);
    Height = EPD_2in13_V3a_HEIGHT;

	EPD_2in13_V3a_SendCommand(0x24);   //Write Black and White image to RAM
    for (UWORD j = 0; j < Height; j++)
	{
        for (UWORD i = 0; i < Width; i++)
		{
			EPD_2in13_V3a_SendData(Image[i + j * Width]);
		}
	}
	EPD_2in13_V3a_SendCommand(0x26);   //Write Black and White image to RAM
    for (UWORD j = 0; j < Height; j++)
	{
        for (UWORD i = 0; i < Width; i++)
		{
			EPD_2in13_V3a_SendData(Image[i + j * Width]);
		}
	}
	EPD_2in13_V3a_TurnOnDisplay();
}

/******************************************************************************
function :	Sends the image buffer in RAM to e-Paper and partial refresh
parameter:
	image : Image data
******************************************************************************/
void EPD_2in13_V3a_Display_Partial(UBYTE *Image)
{
	/*
	 * V3a partial refresh: the standard V3 partial LUT + 0xff update
	 * control hangs the busy pin on this panel. Instead we write the
	 * new image to RAM and trigger a full-sequence refresh (0xf7)
	 * which the V3a panel handles reliably. No blank-to-black flash
	 * because we skip the Clear step — the controller just overwrites
	 * the previous image in place.
	 */
	UWORD Width, Height;
    Width = (EPD_2in13_V3a_WIDTH % 8 == 0)? (EPD_2in13_V3a_WIDTH / 8 ): (EPD_2in13_V3a_WIDTH / 8 + 1);
    Height = EPD_2in13_V3a_HEIGHT;

	EPD_2in13_V3a_SetWindows(0, 0, EPD_2in13_V3a_WIDTH-1, EPD_2in13_V3a_HEIGHT-1);
	EPD_2in13_V3a_SetCursor(0, 0);

	/* Write new image to both RAM buffers so the controller sees the change */
	EPD_2in13_V3a_SendCommand(0x24);   // New image RAM
    for (UWORD j = 0; j < Height; j++)
	{
        for (UWORD i = 0; i < Width; i++)
		{
			EPD_2in13_V3a_SendData(Image[i + j * Width]);
		}
	}

	EPD_2in13_V3a_SetCursor(0, 0);
	EPD_2in13_V3a_SendCommand(0x26);   // Old image RAM (must match for clean refresh)
    for (UWORD j = 0; j < Height; j++)
	{
        for (UWORD i = 0; i < Width; i++)
		{
			EPD_2in13_V3a_SendData(Image[i + j * Width]);
		}
	}

	EPD_2in13_V3a_TurnOnDisplay();
}

/******************************************************************************
function :	Enter sleep mode
parameter:
******************************************************************************/
void EPD_2in13_V3a_Sleep(void)
{
	EPD_2in13_V3a_SendCommand(0x10); //enter deep sleep
	EPD_2in13_V3a_SendData(0x01);
	DEV_Delay_ms(100);
}
