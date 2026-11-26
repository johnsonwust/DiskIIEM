#include <cstdint>
#include "DebugOut.h"
#include <hardware/sync.h>
#include <pico/time.h>
#include <stdint.h>
#include <hardware/gpio.h>
#include <stdio.h>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/spi.h"
#include "hardware/clocks.h"
#include "hardware/vreg.h"
#include "PinConfig.h"
#include "FloppyEmulator.h"
#include "SDCardManager.h"
#include "Display.h"
#include "Beeper.h"
#include "Buttons.h"
#include "UIHandler.h"
#include "ConfigManager.h"
#include "SDFormat.h"
#include "hardware/irq.h"
#include "hardware/timer.h"

// Core1 runs the UI and every FAT32 operation started from the menu (list,
// load, config save, format). Give it its own 8KB stack: the SDK default is
// only 2KB (PICO_CORE1_STACK_SIZE), which those call chains (several 256..512
// byte local buffers) could overflow.
static uint32_t core1Stack[8192 / sizeof(uint32_t)] __attribute__((aligned(8)));

// Global instances of UI and floppy handlers
static UIHandler* g_ui = nullptr;
static FloppyEmulator* g_floppy = nullptr;


// Core1 processing function
void core1_process() {
    while (true) {
        // UI always runs; Apple II signals only matter once an image is loaded
        if (g_ui) {
            g_ui->update();
        }

        // Small delay to prevent CPU spinning
        sleep_us(10);
    }
}

bool timer_callback(struct repeating_timer *t) {
    // Track the stepper all the time, also while no image is loaded: like a
    // real drive, the head must be where the Apple II put it when a disk is
    // inserted or changed.
    g_floppy->processStepperMotor();
    return true;   // keep the timer running
}

int main()
{
    // The buzzer pin must be LOW from the start
    beeperInit();

    // Overclock Pico 2 (RP2350) to 200MHz for better performance
    // This provides more CPU power for GCR encoding and prevents track skipping
    // Set voltage regulator to 1.20V for stability at 200MHz (higher than default 1.10V)
    vreg_set_voltage(VREG_VOLTAGE_1_20);
    sleep_ms(10);  // Wait for voltage to stabilize
    
    // Set system clock to 200MHz (overclock from default 150MHz)
    // RP2350 can safely run at 200MHz with adequate voltage
    set_sys_clock_khz(200000, true);
    beeperInit();      // again: its frequency is derived from the system clock
    
    stdio_init_all();
    
    // Create floppy emulator instance (static to avoid 140KB stack allocation)
    static FloppyEmulator floppy(
        GPIO_PH0, GPIO_PH1, GPIO_PH2, GPIO_PH3,  // Stepper phases (INPUT - from controller)
        GPIO_READ,                                // Read data (OUTPUT - to controller)
        GPIO_WRITE,                               // Write data (INPUT - from controller)
        GPIO_WRITE_ENABLE,                        // Write enable (INPUT - from controller)
        GPIO_DRIVE_SEL                            // Drive select (INPUT - from controller)
    );
    g_floppy = &floppy;

    // Initialize floppy emulator hardware
    floppy.init();
    DBG_PRINTF("Floppy emulator initialized\r\n");
    DBG_PRINTF("Waiting for Apple II controller signals...\r\n");
    DBG_PRINTF("Stepper phases (PH0-PH3) are now INPUTS (monitoring controller)\r\n");
    
    // Initialize display
    DBG_PRINTF("Initializing ST7567 LCD display (SPI1)...\r\n");
    DBG_PRINTF("LCD SPI: SCK=GPIO%d, MOSI=GPIO%d, CS=GPIO%d, DC=GPIO%d, RST=GPIO%d\r\n",
           LCD_SPI_SCK, LCD_SPI_MOSI, LCD_SPI_CS, LCD_SPI_DC, LCD_SPI_RST);

    // Initialize SPI1 for the LCD (SPI0 is dedicated to the SD card)
    // Slow SPI + push-pull MOSI/SCK per ST7567.c reference
    spi_init(LCD_SPI_INSTANCE, 500000);  // 500 kHz
    gpio_set_function(LCD_SPI_SCK, GPIO_FUNC_SPI);
    gpio_set_function(LCD_SPI_MOSI, GPIO_FUNC_SPI);
    gpio_set_drive_strength(LCD_SPI_SCK, GPIO_DRIVE_STRENGTH_12MA);
    gpio_set_drive_strength(LCD_SPI_MOSI, GPIO_DRIVE_STRENGTH_12MA);
    gpio_disable_pulls(LCD_SPI_SCK);
    gpio_disable_pulls(LCD_SPI_MOSI);

    // Create ST7567 display instance (static to avoid stack allocation)
    static ST7567 lcdDisplay(LCD_SPI_INSTANCE, LCD_SPI_CS, LCD_SPI_DC, LCD_SPI_RST, LCD_BL);
    static Display display(&lcdDisplay);
    if (display.init()) {
        DBG_PRINTF("ST7567 LCD display initialized successfully\r\n");
    } else {
        DBG_PRINTF("ST7567 LCD display initialization failed\r\n");
    }
    
    // Boot logo: on the LCD while the SD card is read below (the UI on core1
    // animates it afterwards, so it costs no start-up time)
    UIHandler::drawBootLogo(&display, 0);
    display.update();

    // Initialize SD card with hotplug support (static to avoid large stack allocation)
    static SDCardManager sdCard(SD_SPI_INSTANCE, SD_SPI_CS, SD_SPI_MOSI, SD_SPI_MISO, SD_SPI_SCK, SD_CARD_DETECT);
    
    DBG_PRINTF("Initializing SD card with hotplug support...\r\n");
    DBG_PRINTF("SD Card SPI: CS=GPIO%d, MOSI=GPIO%d, MISO=GPIO%d, SCK=GPIO%d\r\n", 
           SD_SPI_CS, SD_SPI_MOSI, SD_SPI_MISO, SD_SPI_SCK);
    DBG_PRINTF("SD Card Detect: GPIO%d (active LOW)\r\n", SD_CARD_DETECT);
    
    // Initialize config (SOUND=ON default)
    configInit();
    
    // Check if card is present
    bool sdInitialized = false;
    bool autoLoaded = false;
    // The card-detect pin is not reliable on this board, so it only decides how
    // hard we try: three attempts if it reports a card, one if it does not.
    {
        const int attempts = sdCard.isCardPresent() ? 3 : 1;
        DBG_PRINTF("SD card: %d init attempt(s)...\r\n", attempts);

        // Try multiple times with delays
        for (int attempt = 0; attempt < attempts; attempt++) {
            if (attempt > 0) {
                DBG_PRINTF("Retrying SD card initialization (attempt %d/3)...\r\n", attempt + 1);
                sleep_ms(500);
            }
            
            // Fixed 10 MHz: fast and reliable for SDHC cards, avoids the
            // ~1s testMaxReadSpeed delay at boot.
            if (sdCard.init(10000000, true)) {
                DBG_PRINTF("SD card initialized successfully at 10 MHz\r\n");
                sdInitialized = true;
                configLoad(&sdCard);
                floppy.setWeakBits(g_config.weakBit);
                
                // Auto-load the last image recorded in config.cfg.
                // LAST IMAGE is a path ("/DIR/FILE.DSK"); old configs hold a
                // plain file name, which is looked up in the root directory.
                FAT32* fat32 = sdCard.getFAT32();
                if (g_config.bootLastImage && g_config.lastImage[0] != 0 && fat32) {
                    const char* baseName = fat32->enterPathDirectory(g_config.lastImage);
                    uint8_t* diskImage = floppy.getDiskImage();
                    uint32_t diskSize = floppy.getDiskImageSize();
                    uint32_t bytesRead = 0;
                    FAT32_File imageFile;
                    if (baseName && baseName[0] &&
                        sdCard.loadDiskImage(baseName, diskImage, diskSize, &bytesRead, &imageFile)) {
                        floppy.setSDCardManager(&sdCard);
                        floppy.setCurrentFileName(baseName);
                        floppy.setImageFile(imageFile);
                        floppy.setImagePath(g_config.lastImage);
                        autoLoaded = floppy.loadDiskImage(diskImage, bytesRead);
                        if (!autoLoaded) {
                            floppy.setCurrentFileName("");   // e.g. damaged WOZ: drive stays empty
                        }
                        DBG_PRINTF("Auto-load %s: %s (%u bytes)\r\n", autoLoaded ? "OK" : "rejected",
                               g_config.lastImage, (unsigned)bytesRead);
                    } else {
                        fat32->changeDirectory("/");
                        DBG_PRINTF("Auto-load failed: %s\r\n", g_config.lastImage);
                    }
                }
                break;
            }
        }
        
        if (!sdInitialized) {
            DBG_PRINTF("SD card initialization failed\r\n");
            DBG_PRINTF("Possible issues:\r\n");
            DBG_PRINTF("  - No card / wrong pin connections\r\n");
            DBG_PRINTF("  - Card may need formatting\r\n");
        }
    }
    

    // LCD settings once more (see ST7567::configure), then the logo again
    lcdDisplay.configure();
    display.update();

    // Create buttons instance (static to avoid stack allocation)
    DBG_PRINTF("Initializing buttons...\r\n");
    static Buttons keys(KEY_DW, KEY_UP, KEY_RET, KEY_ENTR);
    keys.init();
    DBG_PRINTF("Buttons initialized (DW: GPIO%d, UP: GPIO%d, RET: GPIO%d, ENTR: GPIO%d)\r\n", 
           KEY_DW, KEY_UP, KEY_RET, KEY_ENTR);
    
    // Create UI handler
    static UIHandler ui(&display, &keys);
    g_ui = &ui;
    g_ui->init();
    g_ui->setFloppyEmulator(&floppy);
    // Show the auto-loaded image in the status bar (only if it really loaded)
    g_ui->setLoadedFileName(autoLoaded ? configBaseName(g_config.lastImage) : "");
    // Always hand the SD manager to the UI. Before, it was only set when the
    // FAT32 mount succeeded, so on an exFAT/unformatted card Config -> FORMAT SD
    // silently did nothing, and a card inserted later could never be used.
    // All UI paths check isInitialized()/getFAT32() before touching the card.
    g_ui->setSDCardManager(&sdCard);
    if (sdInitialized) {
        g_ui->showMainMenu();
    } else if (sdCard.isInitialized()) {
        // The card answered but has no usable FAT32 volume - show why
        FAT32_Error err = sdCard.getLastFAT32Error();
        SDErrorType uiError = SD_ERROR_UNKNOWN_FS;
        switch (err) {
            case FAT32_ERROR_EXFAT: uiError = SD_ERROR_EXFAT; break;
            case FAT32_ERROR_NTFS: uiError = SD_ERROR_NTFS; break;
            case FAT32_ERROR_FAT12: uiError = SD_ERROR_FAT12; break;
            case FAT32_ERROR_FAT16: uiError = SD_ERROR_FAT16; break;
            case FAT32_ERROR_READ_FAILED: uiError = SD_ERROR_READ_FAILED; break;
            default: uiError = SD_ERROR_UNKNOWN_FS; break;
        }
        g_ui->showSDError(uiError);
    } else {
        // No SD card present
        g_ui->showNoSDCard();
    }
    DBG_PRINTF("UI handler initialized\r\n");
    
    // Launch core1 processing
    DBG_PRINTF("Launching core1 processing...\r\n");
    multicore_launch_core1_with_stack(core1_process, core1Stack, sizeof(core1Stack));
    DBG_PRINTF("Core1 processing started\r\n");
    
    // Main loop (runs on core0)
    
    DBG_PRINTF("Main loop starting... V0.3.0\r\n");


    static repeating_timer_t motorTimer;
    add_repeating_timer_us(-500, timer_callback, NULL, &motorTimer);
    // Highest priority for the IRQ of the alarm the repeating timer really uses
    // (the default alarm pool sits on hardware alarm 3, not on IRQ 0 as assumed
    // before), so the stepper is sampled on time even while the DMA IRQ refills
    // the read stream.
    irq_set_priority(hardware_alarm_get_irq_num(PICO_TIME_DEFAULT_ALARM_POOL_HARDWARE_ALARM_NUM), 0);

    while (true) {
      
        // The controller is writing: capture the whole write now. The loop inside
        // runs with interrupts disabled until WRITE_EN is released.
        if (floppy.hasImageLoaded() && floppy.isWriteEnabled() && floppy.isDriveSelected()) {
            floppy.captureWrite();
            continue;
        }

        g_floppy->process();
        
    }
}
