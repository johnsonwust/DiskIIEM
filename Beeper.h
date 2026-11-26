#ifndef BEEPER_H
#define BEEPER_H

// Buzzer on GPIO_BEEP (PinConfig.h): HIGH = current through the buzzer coil.
// The pin must never stay HIGH, the driver (U2, 74HC125) would overheat.
//
// So the pin is never set HIGH by software. A PWM slice that is never stopped
// produces BEEP_DUTY_PERCENT pulses at BEEP_FREQ_HZ; to sound, the pin is
// connected to it, to be silent the pin is a plain output that is always LOW.
// Whatever happens to the program (interrupts off, a hang), the pin is either
// LOW or carries the PWM pulses.

// Pin LOW, PWM prepared. Call first in main().
void beeperInit();

// Request a sound (any core, returns at once; nothing if Sound is OFF)
void beepKey();     // key press
void beepStep();    // head step

// Start / end sounds. Called every 500 us from the timer interrupt of core0.
void beeperTick();

// Silence at once. Only from core0 with interrupts disabled: during a write
// the timer interrupt does not run and could not end a sound.
void beeperSilence();

#endif // BEEPER_H
