#include "ST7567.h"

#ifdef USE_ST7567

#include "hardware/gpio.h"
#include "pico/time.h"
#include <string.h>
#include <stdlib.h>

// 5x7 font (ASCII 32-126), same as the other monochrome drivers
static const uint8_t font5x7[][5] = {
    {0x00, 0x00, 0x00, 0x00, 0x00}, // space
    {0x00, 0x00, 0x5F, 0x00, 0x00}, // !
    {0x00, 0x07, 0x00, 0x07, 0x00}, // "
    {0x14, 0x7F, 0x14, 0x7F, 0x14}, // #
    {0x24, 0x2A, 0x7F, 0x2A, 0x12}, // $
    {0x23, 0x13, 0x08, 0x64, 0x62}, // %
    {0x36, 0x49, 0x55, 0x22, 0x50}, // &
    {0x00, 0x00, 0x07, 0x00, 0x00}, // '
    {0x00, 0x1C, 0x22, 0x41, 0x00}, // (
    {0x00, 0x41, 0x22, 0x1C, 0x00}, // )
    {0x08, 0x2A, 0x1C, 0x2A, 0x08}, // *
    {0x08, 0x08, 0x3E, 0x08, 0x08}, // +
    {0x00, 0x50, 0x30, 0x00, 0x00}, // ,
    {0x08, 0x08, 0x08, 0x08, 0x08}, // -
    {0x00, 0x60, 0x60, 0x00, 0x00}, // .
    {0x20, 0x10, 0x08, 0x04, 0x02}, // /
    {0x3E, 0x51, 0x49, 0x45, 0x3E}, // 0
    {0x00, 0x42, 0x7F, 0x40, 0x00}, // 1
    {0x42, 0x61, 0x51, 0x49, 0x46}, // 2
    {0x21, 0x41, 0x45, 0x4B, 0x31}, // 3
    {0x18, 0x14, 0x12, 0x7F, 0x10}, // 4
    {0x27, 0x45, 0x45, 0x45, 0x39}, // 5
    {0x3C, 0x4A, 0x49, 0x49, 0x30}, // 6
    {0x01, 0x71, 0x09, 0x05, 0x03}, // 7
    {0x36, 0x49, 0x49, 0x49, 0x36}, // 8
    {0x06, 0x49, 0x49, 0x29, 0x1E}, // 9
    {0x00, 0x36, 0x36, 0x00, 0x00}, // :
    {0x00, 0x56, 0x36, 0x00, 0x00}, // ;
    {0x08, 0x14, 0x22, 0x41, 0x00}, // <
    {0x14, 0x14, 0x14, 0x14, 0x14}, // =
    {0x00, 0x41, 0x22, 0x14, 0x08}, // >
    {0x02, 0x01, 0x51, 0x09, 0x06}, // ?
    {0x32, 0x49, 0x59, 0x51, 0x3E}, // @
    {0x7C, 0x12, 0x11, 0x12, 0x7C}, // A
    {0x7F, 0x49, 0x49, 0x49, 0x36}, // B
    {0x3E, 0x41, 0x41, 0x41, 0x22}, // C
    {0x7F, 0x41, 0x41, 0x22, 0x1C}, // D
    {0x7F, 0x49, 0x49, 0x49, 0x41}, // E
    {0x7F, 0x09, 0x09, 0x09, 0x01}, // F
    {0x3E, 0x41, 0x49, 0x49, 0x7A}, // G
    {0x7F, 0x08, 0x08, 0x08, 0x7F}, // H
    {0x00, 0x41, 0x7F, 0x41, 0x00}, // I
    {0x20, 0x40, 0x41, 0x3F, 0x01}, // J
    {0x7F, 0x08, 0x14, 0x22, 0x41}, // K
    {0x7F, 0x40, 0x40, 0x40, 0x40}, // L
    {0x7F, 0x02, 0x0C, 0x02, 0x7F}, // M
    {0x7F, 0x04, 0x08, 0x10, 0x7F}, // N
    {0x3E, 0x41, 0x41, 0x41, 0x3E}, // O
    {0x7F, 0x09, 0x09, 0x09, 0x06}, // P
    {0x3E, 0x41, 0x51, 0x21, 0x5E}, // Q
    {0x7F, 0x09, 0x19, 0x29, 0x46}, // R
    {0x46, 0x49, 0x49, 0x49, 0x31}, // S
    {0x01, 0x01, 0x7F, 0x01, 0x01}, // T
    {0x3F, 0x40, 0x40, 0x40, 0x3F}, // U
    {0x1F, 0x20, 0x40, 0x20, 0x1F}, // V
    {0x3F, 0x40, 0x38, 0x40, 0x3F}, // W
    {0x63, 0x14, 0x08, 0x14, 0x63}, // X
    {0x07, 0x08, 0x70, 0x08, 0x07}, // Y
    {0x61, 0x51, 0x49, 0x45, 0x43}, // Z
    {0x00, 0x7F, 0x41, 0x41, 0x00}, // [
    {0x02, 0x04, 0x08, 0x10, 0x20}, // backslash
    {0x00, 0x41, 0x41, 0x7F, 0x00}, // ]
    {0x04, 0x02, 0x01, 0x02, 0x04}, // ^
    {0x40, 0x40, 0x40, 0x40, 0x40}, // _
    {0x00, 0x01, 0x02, 0x04, 0x00}, // `
    {0x20, 0x54, 0x54, 0x54, 0x78}, // a
    {0x7F, 0x48, 0x44, 0x44, 0x38}, // b
    {0x38, 0x44, 0x44, 0x44, 0x20}, // c
    {0x38, 0x44, 0x44, 0x48, 0x7F}, // d
    {0x38, 0x54, 0x54, 0x54, 0x18}, // e
    {0x08, 0x7E, 0x09, 0x01, 0x02}, // f
    {0x0C, 0x52, 0x52, 0x52, 0x3E}, // g
    {0x7F, 0x08, 0x04, 0x04, 0x78}, // h
    {0x00, 0x44, 0x7D, 0x40, 0x00}, // i
    {0x20, 0x40, 0x44, 0x3D, 0x00}, // j
    {0x7F, 0x10, 0x28, 0x44, 0x00}, // k
    {0x00, 0x41, 0x7F, 0x40, 0x00}, // l
    {0x7C, 0x04, 0x18, 0x04, 0x78}, // m
    {0x7C, 0x08, 0x04, 0x04, 0x78}, // n
    {0x38, 0x44, 0x44, 0x44, 0x38}, // o
    {0x7C, 0x14, 0x14, 0x14, 0x08}, // p
    {0x08, 0x14, 0x14, 0x18, 0x7C}, // q
    {0x7C, 0x08, 0x04, 0x04, 0x08}, // r
    {0x48, 0x54, 0x54, 0x54, 0x20}, // s
    {0x04, 0x3F, 0x44, 0x40, 0x20}, // t
    {0x3C, 0x40, 0x40, 0x20, 0x7C}, // u
    {0x1C, 0x20, 0x40, 0x20, 0x1C}, // v
    {0x3C, 0x40, 0x30, 0x40, 0x3C}, // w
    {0x44, 0x28, 0x10, 0x28, 0x44}, // x
    {0x0C, 0x50, 0x50, 0x50, 0x3C}, // y
    {0x44, 0x64, 0x54, 0x4C, 0x44}, // z
    {0x00, 0x08, 0x36, 0x41, 0x00}, // {
    {0x00, 0x00, 0x7F, 0x00, 0x00}, // |
    {0x00, 0x41, 0x36, 0x08, 0x00}, // }
    {0x08, 0x08, 0x2A, 0x1C, 0x08}, // ~
};

ST7567::ST7567(spi_inst_t* spiInstance, uint8_t cs, uint8_t dc, uint8_t rst, uint8_t bl) {
    spi = spiInstance;
    csPin = cs;
    dcPin = dc;
    rstPin = rst;
    blPin = bl;
    memset(buffer, 0, sizeof(buffer));
}

ST7567::~ST7567() {
}

void ST7567::setCS(bool state) {
    gpio_put(csPin, !state);  // CS is active LOW
}

void ST7567::setDC(bool state) {
    gpio_put(dcPin, state);   // HIGH = data, LOW = command
}

void ST7567::sendCommand(uint8_t cmd) {
    setCS(true);
    setDC(false);
    spi_write_blocking(spi, &cmd, 1);
    setCS(false);
}

void ST7567::sendData(uint8_t* data, uint32_t len) {
    setCS(true);
    setDC(true);
    spi_write_blocking(spi, data, len);
    setCS(false);
}

bool ST7567::init() {
    // Control GPIOs
    gpio_init(csPin);
    gpio_set_dir(csPin, GPIO_OUT);
    gpio_put(csPin, 1);  // CS inactive (HIGH)

    gpio_init(dcPin);
    gpio_set_dir(dcPin, GPIO_OUT);
    gpio_put(dcPin, 0);  // command mode

    gpio_init(rstPin);
    gpio_set_dir(rstPin, GPIO_OUT);
    gpio_put(rstPin, 1);  // reset inactive

    if (blPin != 0xFF) {
        gpio_init(blPin);
        gpio_put(blPin, LCD_BL_ON_LEVEL);              // backlight on
        gpio_set_dir(blPin, GPIO_OUT);
    }

    // Hardware reset (match ST7567.c: CS asserted during reset)
    // The LCD is now started right after power-on (boot logo), so the reset
    // pulse and the wait after it are generous
    gpio_put(csPin, 0);   // CS low (asserted)
    gpio_put(rstPin, 0);
    sleep_us(20);
    gpio_put(rstPin, 1);
    sleep_ms(2);
    gpio_put(csPin, 1);   // CS high (deasserted)

    configure();

    clear();
    display();

    return true;
}

// Send the controller settings (no reset, the picture stays). main() repeats
// this once after the SD card has been read: should the controller have missed
// the first set of commands so soon after power-on, it is set up again at the
// point where the LCD used to be started.
void ST7567::configure() {
    // SPI format
    spi_set_format(spi, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);

    // ST7567 initialization sequence — exact match with reference ST7567.c
    sendCommand(0xA2);                       // 1/9 bias
    sendCommand(0xA0);                       // ADC select (normal)
    sendCommand(0x80);                       // (as in reference)
    sendCommand(0x27);                       // resistor ratio
    sendCommand(0x81);                       // electronic volume
    sendCommand(ST7567_CONTRAST);            // contrast (0x06 per reference)
    sendCommand(0xC8);                       // COM output reverse
    sendCommand(0xA6);                       // display normal (not inverted)
    sendCommand(0x2F);                       // power control: VC|VR|VF all on
    sendCommand(0xF8);                       // booster ratio
    sendCommand(0x00);                       // 4x boost
    sendCommand(0xAF);                       // display on
    sendCommand(0xA4);                       // all pixels normal
}

void ST7567::clear() {
    memset(buffer, 0, sizeof(buffer));
}

void ST7567::display() {
    // Write page-by-page (8 pages x 128 columns for 128x64)
    const int pages = LCD_HEIGHT / 8;
    for (int page = 0; page < pages; page++) {
        sendCommand(ST7567_SET_PAGE_ADDR | page);
        sendCommand(ST7567_SET_COLUMN_MSB | 0);  // column high = 0
        sendCommand(ST7567_SET_COLUMN_LSB | 0);  // column low = 0
        sendData(&buffer[page * LCD_WIDTH], LCD_WIDTH);
    }
}

void ST7567::setBacklight(bool on) {
    if (blPin != 0xFF) {
        gpio_put(blPin, on ? LCD_BL_ON_LEVEL : !LCD_BL_ON_LEVEL);
    }
}

void ST7567::setPixel(int x, int y, bool color) {
    if (x < 0 || x >= LCD_WIDTH || y < 0 || y >= LCD_HEIGHT) return;

    int page = y / 8;
    int bit = y % 8;

    if (color) {
        buffer[page * LCD_WIDTH + x] |= (1 << bit);
    } else {
        buffer[page * LCD_WIDTH + x] &= ~(1 << bit);
    }
}

void ST7567::drawChar(int x, int y, char c, bool color) {
    if (c < 32 || c > 126) c = 32;

    const uint8_t* fontData = font5x7[c - 32];

    for (int col = 0; col < 5; col++) {
        uint8_t colData = fontData[col];
        for (int row = 0; row < 7; row++) {
            if (colData & (1 << row)) {
                setPixel(x + col, y + row, color);
            }
        }
    }
}

void ST7567::drawCharScaled(int x, int y, char c, int scale, bool color) {
    if (c < 32 || c > 126) c = 32;
    if (scale < 1) scale = 1;
    const uint8_t* fontData = font5x7[c - 32];
    for (int col = 0; col < 5; col++) {
        uint8_t colData = fontData[col];
        for (int row = 0; row < 7; row++) {
            if (colData & (1 << row)) {
                for (int dx = 0; dx < scale; dx++) {
                    for (int dy = 0; dy < scale; dy++) {
                        setPixel(x + col * scale + dx, y + row * scale + dy, color);
                    }
                }
            }
        }
    }
}

void ST7567::drawString(int x, int y, const char* str, bool color) {
    int pos = x;
    while (*str && pos < LCD_WIDTH - 5) {
        drawChar(pos, y, *str++, color);
        pos += 6;  // 5 pixels + 1 space
    }
}

void ST7567::drawLine(int x0, int y0, int x1, int y1, bool color) {
    int dx = abs(x1 - x0);
    int dy = abs(y1 - y0);
    int sx = (x0 < x1) ? 1 : -1;
    int sy = (y0 < y1) ? 1 : -1;
    int err = dx - dy;

    int x = x0, y = y0;

    while (true) {
        setPixel(x, y, color);
        if (x == x1 && y == y1) break;
        int e2 = 2 * err;
        if (e2 > -dy) {
            err -= dy;
            x += sx;
        }
        if (e2 < dx) {
            err += dx;
            y += sy;
        }
    }
}

void ST7567::drawRect(int x, int y, int w, int h, bool color, bool filled) {
    if (filled) {
        fillRect(x, y, w, h, color);
    } else {
        drawLine(x, y, x + w - 1, y, color);
        drawLine(x + w - 1, y, x + w - 1, y + h - 1, color);
        drawLine(x + w - 1, y + h - 1, x, y + h - 1, color);
        drawLine(x, y + h - 1, x, y, color);
    }
}

void ST7567::fillRect(int x, int y, int w, int h, bool color) {
    for (int i = 0; i < w; i++) {
        for (int j = 0; j < h; j++) {
            setPixel(x + i, y + j, color);
        }
    }
}

void ST7567::drawBitmap(int x, int y, const uint16_t* bitmap, int w, int h, bool color) {
    for (int j = 0; j < h; j++) {
        uint16_t rowData = bitmap[j];
        for (int i = 0; i < w; i++) {
            int bitIndex = 15 - i;
            if (rowData & (1 << bitIndex)) {
                setPixel(x + i, y + j, color);
            } else {
                setPixel(x + i, y + j, !color);
            }
        }
    }
}

#endif // USE_ST7567
