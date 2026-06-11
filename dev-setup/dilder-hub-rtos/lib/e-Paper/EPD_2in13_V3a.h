/*****************************************************************************
* | File      	:   EPD_2in13_V3a.h
* | Author      :   Waveshare team (adapted for V3 rev.A variant)
* | Function    :   2.13inch e-paper V3 revision A
* | Info        :
*   The "A" revision of the 2.13" V3 display uses the same SSD1680
*   controller but requires different LUT waveform tables and a
*   modified initialization sequence. Board is marked with "A" on
*   the silkscreen.
*----------------
* |	This version:   V1.0
* | Date        :   2026-04-12
* | Info        :   Based on EPD_2in13_V3.h — same resolution and
*                   API, different init and LUT tables.
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
#ifndef __EPD_2in13_V3a_H_
#define __EPD_2in13_V3a_H_

#include "DEV_Config.h"

// Display resolution (same as V3)
#define EPD_2in13_V3a_WIDTH       122
#define EPD_2in13_V3a_HEIGHT      250

void EPD_2in13_V3a_Init(void);
void EPD_2in13_V3a_Clear(void);
void EPD_2in13_V3a_Display(UBYTE *Image);
void EPD_2in13_V3a_Display_Base(UBYTE *Image);
void EPD_2in13_V3a_Display_Partial(UBYTE *Image);
void EPD_2in13_V3a_Sleep(void);

#endif
