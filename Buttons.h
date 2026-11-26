#ifndef BUTTONS_H
#define BUTTONS_H

#include <stdint.h>
#include <stdbool.h>
#include "pico/time.h"

typedef enum {
    KEY_INDEX_DW = 0,   // down / next
    KEY_INDEX_UP = 1,   // up / previous
    KEY_INDEX_RET = 2,  // back
    KEY_INDEX_ENTR = 3  // enter / confirm
} KeyIndex;

// The four keys (active low, internal pull-ups), debounced.
// update() must be called often (every pass of the UI loop).
class Buttons {
public:
    Buttons(uint8_t dw, uint8_t up, uint8_t ret, uint8_t entr);

    void init();
    void update();

    bool anyPressed() const;          // a key is down now
    bool wasPressed(KeyIndex k);      // true once for every time the key went down
    uint32_t heldMs(KeyIndex k) const;// how long the key has been down (0 = it is up)
    void clearEvents();               // forget key presses not yet fetched with wasPressed()

private:
    static const int COUNT = 4;
    static const int64_t DEBOUNCE_US = 30000;   // a level must be stable this long

    uint8_t pin[COUNT];
    bool down[COUNT];                 // debounced state
    bool pressEvent[COUNT];
    absolute_time_t downSince[COUNT];
    absolute_time_t lastSame[COUNT];  // last time the pin agreed with the debounced state
};

#endif // BUTTONS_H
