#include "Display.h"

void Display::drawStringScaled(int x, int y, const char* str, int scale, bool color) {
    for (; *str; str++, x += 6 * scale) {
        lcd->drawCharScaled(x, y, *str, scale, color);
    }
}
