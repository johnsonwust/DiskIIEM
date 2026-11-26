#ifndef PIN_CONFIG_H
#define PIN_CONFIG_H

// ============================================================================
// Apple II Floppy Interface GPIO Pins
// ============================================================================

// Checked against the schematic "apple ii PICO" of 2026-11-25 (U4 = Pico 2).
//
// Disk II signals. Levels are those at the Pico pin; no signal is inverted on
// the board:
//   inputs   series diode (D1-D7, cathode at the connector) and a 2.2k pull-up
//            to 3.3V at the Pico (RN1/RN2): an open line reads HIGH
//   outputs  U2 = 74HC125, a NON-inverting buffer with 3-state outputs. Its
//            output enables for RD_DATA and WR_PROTECT (pins 10 and 13, active
//            low) are on DRIVE_SEL_3V (the same net as GP13; the name
//            DEVICE_ENABLE_3V in the 2026-11-25 export was merged into it), so
//            a drive that is not selected releases the lines it shares with
//            the other drive. The firmware has nothing to do with it.
//
//   pin   signal          dir  level at the Pico pin = level at the connector
//   GP2   PH0             in   active high
//   GP3   PH1             in   active high
//   GP4   PH2             in   active high
//   GP5   PH3             in   active high
//   GP10  RD_DATA         out  idle LOW, a 1 bit is a 1 us HIGH pulse
//   GP11  WR_DATA         in   only its edges (flux transitions) count
//   GP12  WRITE_ENABLE#   in   active low
//   GP13  DRIVE_SEL#      in   active low
//   GP14  WR_PROTECT      out  HIGH = write protected
//
// Behaviour: RD_DATA is output whenever DRIVE_SEL# = 0 (paused only while a
// write is being captured); WR_DATA is captured while DRIVE_SEL# = 0 and
// WRITE_ENABLE# = 0. Both inputs are read as levels, never as edges, so a
// drive already selected before the Pico starts is served at once.
//
// Earlier demo board (inverting buffers behind GP10 and GP14): set
// READ_OUTPUT_INVERTED and WPROT_OUTPUT_INVERTED to 1. Nothing else changes.

#define GPIO_PH0           2
#define GPIO_PH1           3
#define GPIO_PH2           4
#define GPIO_PH3           5

#define GPIO_READ          10
// RD_DATA: 0 = direct: idle LOW, 1 us HIGH pulse for a 1 bit (pad output
// override; bitstream, PIO program and DMA are identical). 1 = the board has an
// inverting buffer: idle HIGH, 1 us LOW pulse at the pin.
#define READ_OUTPUT_INVERTED 0

#define GPIO_WRITE         11
#define GPIO_WRITE_ENABLE  12   // WRITE_ENABLE#, active low
#define GPIO_DRIVE_SEL     13   // DRIVE_SEL#, active low

// Buzzer: HIGH = current through the coil (two gates of U2 in parallel, always
// enabled, coil to GND). The pin must never stay HIGH (the driver would
// overheat); see Beeper.h. Silent = LOW.
#define GPIO_BEEP          0
#define BEEP_DUTY_PERCENT  10     // provisional value
#define BEEP_FREQ_HZ       2000

#define GPIO_WR_PROTECT    14   // WR_PROTECT (output to the controller)
// At the Disk II connector HIGH = write protected. 0 = direct: pin HIGH =
// protected. 1 = the board has an inverting buffer: pin LOW = protected.
#define WPROT_OUTPUT_INVERTED 0

// ============================================================================
// SD Card SPI Interface GPIO Pins (SPI0 is dedicated to the SD card)
// ============================================================================

#define SD_SPI_INSTANCE    spi0
#define SD_CARD_DETECT     15  // SD_EJECT: card detect (active LOW)
#define SD_SPI_MISO        16
#define SD_SPI_CS          17
#define SD_SPI_SCK         18
#define SD_SPI_MOSI        19

// ============================================================================
// LCD Display: ST7567 (SPI, monochrome 128x64)
// ----------------------------------------------------------------------------
// SPI1 is dedicated to the LCD (SPI0 is the SD card).
//   SCK  = GP26  (SPI1 SCLK, verified valid on RP2350)
//   MOSI = GP27  (SPI1 TX,   verified valid on RP2350)
//   CS   = GP9,  A0/DC = GP20,  RST = GP22
//   BL   = GP21: LOW = backlight on, HIGH = off (plain output, no PWM)
// ============================================================================

#define USE_ST7567

#define LCD_SPI_INSTANCE   spi1
#define LCD_SPI_SCK        26
#define LCD_SPI_MOSI       27
#define LCD_SPI_CS         9
#define LCD_SPI_DC         20   // A0: data/command (LOW = command, HIGH = data)
#define LCD_SPI_RST        22
#define LCD_BL             21   // Backlight (0xFF = not connected)
#define LCD_BL_ON_LEVEL    0    // pin level that switches the backlight on

#define LCD_WIDTH          128
#define LCD_HEIGHT         64

// ============================================================================
// Keys (4 buttons, active low)
// ============================================================================

#define KEY_DW             6
#define KEY_UP             7
#define KEY_RET            8
#define KEY_ENTR           28

#endif // PIN_CONFIG_H
