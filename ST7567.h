#ifndef ST7567_H
#define ST7567_H

#include <stdint.h>
#include <stdbool.h>
#include "PinConfig.h"
#include "hardware/spi.h"

#ifdef USE_ST7567

// ST7567 command set (Sitronix)
#define ST7567_DISPLAY_OFF        0xAE
#define ST7567_DISPLAY_ON         0xAF
#define ST7567_SET_START_LINE     0x40  // 0x40..0x7F
#define ST7567_SET_PAGE_ADDR      0xB0  // 0xB0..0xB7
#define ST7567_SET_COLUMN_MSB     0x10  // 0x10..0x1F
#define ST7567_SET_COLUMN_LSB     0x00  // 0x00..0x0F
#define ST7567_SEG_DIR_NORMAL     0xA0
#define ST7567_SEG_DIR_REVERSE    0xA1
#define ST7567_COM_DIR_NORMAL     0xC0
#define ST7567_COM_DIR_REVERSE    0xC8
#define ST7567_BIAS_1_9           0xA2
#define ST7567_BIAS_1_7           0xA3
#define ST7567_POWER_CONTROL      0x28  // + VC(0x04) | VR(0x02) | VF(0x01)
#define ST7567_RESISTOR_RATIO     0x20  // 0x20..0x27
#define ST7567_VOLUME_MODE        0x81  // electronic volume (contrast)
#define ST7567_BOOSTER_RATIO      0xF8  // followed by 0x00 (4x) / 0x01 (5x)
#define ST7567_DISPLAY_NORMAL     0xA6
#define ST7567_DISPLAY_REVERSE    0xA7
#define ST7567_ALL_PIXELS_ON      0xA5
#define ST7567_ALL_PIXELS_NORMAL  0xA4
#define ST7567_SOFT_RESET         0xE2
#define ST7567_NOP               0xE3

// Electronic volume (contrast), 0x00..0x3F.
// 0x06 = white bg but text too faint; 0x0C = text visible but bg over-driven dark.
#define ST7567_CONTRAST           0x08

class ST7567 {
private:
    spi_inst_t* spi;
    uint8_t csPin;
    uint8_t dcPin;
    uint8_t rstPin;
    uint8_t blPin;
    uint8_t buffer[LCD_WIDTH * LCD_HEIGHT / 8];  // 128x64 -> 1024 bytes

    void sendCommand(uint8_t cmd);
    void sendData(uint8_t* data, uint32_t len);
    void setCS(bool state);   // true = asserted
    void setDC(bool state);   // false = command, true = data

public:
    ST7567(spi_inst_t* spiInstance, uint8_t cs, uint8_t dc, uint8_t rst, uint8_t bl);
    ~ST7567();

    bool init();
    void configure();            // controller settings only (no reset, picture kept)
    void clear();
    void display();
    void setBacklight(bool on);  // pin level for "on": LCD_BL_ON_LEVEL
    void setPixel(int x, int y, bool color);
    void drawChar(int x, int y, char c, bool color = true);
    // Same glyph, every font pixel drawn as a scale x scale block
    void drawCharScaled(int x, int y, char c, int scale, bool color = true);
    void drawString(int x, int y, const char* str, bool color = true);
    void drawLine(int x0, int y0, int x1, int y1, bool color = true);
    void drawRect(int x, int y, int w, int h, bool color = true, bool filled = false);
    void fillRect(int x, int y, int w, int h, bool color = true);
    void drawBitmap(int x, int y, const uint16_t* bitmap, int w, int h, bool color = true);
};

#endif // USE_ST7567
#endif // ST7567_H
