#include "Beeper.h"
#include "ConfigManager.h"
#include "PinConfig.h"
#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "hardware/pwm.h"
#include <stdint.h>

#if BEEP_DUTY_PERCENT < 1 || BEEP_DUTY_PERCENT > 25
#error "BEEP_DUTY_PERCENT: 1..25 (a higher duty cycle overheats the buzzer driver)"
#endif
#if BEEP_FREQ_HZ < 100 || BEEP_FREQ_HZ > 10000
#error "BEEP_FREQ_HZ: 100..10000"
#endif

namespace {

const uint32_t PWM_TICK_HZ = 2000000;       // 0.5 us
const uint32_t PERIOD_TICKS = PWM_TICK_HZ / BEEP_FREQ_HZ;
const uint32_t HIGH_TICKS = PERIOD_TICKS * BEEP_DUTY_PERCENT / 100;

// Lengths in timer ticks (500 us)
const uint16_t KEY_TICKS = 60;              // 30 ms
const uint16_t STEP_TICKS = 16;             // 8 ms

volatile uint16_t s_request = 0;            // length of a requested sound
volatile bool s_on = false;                 // the pin is connected to the PWM
uint16_t s_ticksLeft = 0;

inline void connectPwm() {
    gpio_set_function(GPIO_BEEP, GPIO_FUNC_PWM);
    s_on = true;
}

inline void pinLow() {
    gpio_set_function(GPIO_BEEP, GPIO_FUNC_SIO);    // plain output, its level is always 0
    s_on = false;
}

}  // namespace

void beeperInit() {
    gpio_init(GPIO_BEEP);                   // plain pin, level 0
    gpio_put(GPIO_BEEP, 0);
    gpio_set_dir(GPIO_BEEP, GPIO_OUT);
    s_on = false;
    s_request = 0;

    const uint slice = pwm_gpio_to_slice_num(GPIO_BEEP);
    pwm_set_clkdiv(slice, (float)clock_get_hz(clk_sys) / (float)PWM_TICK_HZ);
    pwm_set_wrap(slice, (uint16_t)(PERIOD_TICKS - 1));
    pwm_set_chan_level(slice, pwm_gpio_to_channel(GPIO_BEEP), (uint16_t)HIGH_TICKS);
    pwm_set_enabled(slice, true);           // runs forever; the pin decides what is heard
}

void beepKey() {
    if (g_config.soundOn) s_request = KEY_TICKS;
}

void beepStep() {
    if (g_config.soundOn) s_request = STEP_TICKS;
}

void beeperTick() {
    const uint16_t wanted = s_request;
    if (wanted) {
        s_request = 0;
        if (s_on) {
            if (wanted > s_ticksLeft) s_ticksLeft = wanted;   // already sounding: prolong
        } else {
            s_ticksLeft = wanted;
            connectPwm();
        }
        return;
    }
    if (s_on) {
        if (s_ticksLeft > 0) s_ticksLeft--;
        if (s_ticksLeft == 0) pinLow();
    }
}

void beeperSilence() {
    if (s_on) pinLow();
    s_ticksLeft = 0;
    s_request = 0;
}
