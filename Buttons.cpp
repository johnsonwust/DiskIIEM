#include "Buttons.h"
#include "hardware/gpio.h"

Buttons::Buttons(uint8_t dw, uint8_t up, uint8_t ret, uint8_t entr) {
    pin[KEY_INDEX_DW] = dw;
    pin[KEY_INDEX_UP] = up;
    pin[KEY_INDEX_RET] = ret;
    pin[KEY_INDEX_ENTR] = entr;
    const absolute_time_t now = get_absolute_time();
    for (int i = 0; i < COUNT; i++) {
        down[i] = false;
        pressEvent[i] = false;
        downSince[i] = now;
        lastSame[i] = now;
    }
}

void Buttons::init() {
    for (int i = 0; i < COUNT; i++) {
        gpio_init(pin[i]);
        gpio_set_dir(pin[i], GPIO_IN);
        gpio_pull_up(pin[i]);
    }
}

void Buttons::update() {
    const absolute_time_t now = get_absolute_time();
    for (int i = 0; i < COUNT; i++) {
        const bool level = !gpio_get(pin[i]);          // low = pressed
        if (level == down[i]) {
            lastSame[i] = now;
            continue;
        }
        if (absolute_time_diff_us(lastSame[i], now) > DEBOUNCE_US) {
            down[i] = level;
            lastSame[i] = now;
            if (level) {
                downSince[i] = now;
                pressEvent[i] = true;
            }
        }
    }
}

bool Buttons::anyPressed() const {
    for (int i = 0; i < COUNT; i++) {
        if (down[i]) return true;
    }
    return false;
}

bool Buttons::wasPressed(KeyIndex k) {
    const bool event = pressEvent[k];
    pressEvent[k] = false;
    return event;
}

uint32_t Buttons::heldMs(KeyIndex k) const {
    if (!down[k]) return 0;
    const int64_t us = absolute_time_diff_us(downSince[k], get_absolute_time());
    return us < 1000 ? 1 : (uint32_t)(us / 1000);
}

void Buttons::clearEvents() {
    for (int i = 0; i < COUNT; i++) {
        pressEvent[i] = false;
    }
}
