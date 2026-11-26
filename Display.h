#ifndef DISPLAY_H
#define DISPLAY_H

#include <stdint.h>
#include <stdbool.h>
#include "PinConfig.h"
#include "ST7567.h"

#define DISPLAY_WIDTH  LCD_WIDTH
#define DISPLAY_HEIGHT LCD_HEIGHT

// What the user interface draws with. One panel is supported (ST7567,
// 128x64, monochrome): this class only forwards to its driver, so the UI code
// does not depend on the driver's class.
// color: true = pixel on (dark on this LCD), false = pixel off.
class Display {
public:
    explicit Display(ST7567* panel) : lcd(panel) {}

    bool init()                    { return lcd->init(); }
    void clear()                   { lcd->clear(); }
    void update()                  { lcd->display(); }       // send the frame buffer to the panel
    void setBacklight(bool on)     { lcd->setBacklight(on); }

    void setPixel(int x, int y, bool color)                                  { lcd->setPixel(x, y, color); }
    void drawString(int x, int y, const char* str, bool color = true)        { lcd->drawString(x, y, str, color); }
    // Text with every font pixel drawn as a scale x scale block (6*scale pixels per character)
    void drawStringScaled(int x, int y, const char* str, int scale, bool color = true);
    void drawLine(int x0, int y0, int x1, int y1, bool color = true)         { lcd->drawLine(x0, y0, x1, y1, color); }
    void drawRect(int x, int y, int w, int h, bool color = true, bool filled = false) { lcd->drawRect(x, y, w, h, color, filled); }
    void fillRect(int x, int y, int w, int h, bool color = true)             { lcd->fillRect(x, y, w, h, color); }
    // bitmap: one uint16_t per row, bit 15 = leftmost pixel; set bits are drawn
    // in `color`, clear bits in the opposite colour
    void drawBitmap(int x, int y, const uint16_t* bitmap, int w, int h, bool color = true) { lcd->drawBitmap(x, y, bitmap, w, h, color); }

private:
    ST7567* lcd;
};

#endif // DISPLAY_H
