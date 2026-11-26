#include "SDCardManager.h"
#include "DebugOut.h"
#include "hardware/gpio.h"
#include "hardware/sync.h"
#include "pico/mutex.h"
#include <cstdint>
#include <stdio.h>
#include <string.h>

// Cross-core lock protecting the shared SPI bus (SD card on SPI0) and the
// FAT32 layer. The UI runs on core1 and lists/loads/saves files while core0
// saves tracks written by the Apple II, so serialize all SD access.
//
// Previously this was a spin lock taken only by readBlock(): writeBlock() was
// unprotected, multi-sector FAT32 operations were not atomic, and the lock was
// also taken from the stepper timer IRQ (could deadlock against core0 itself).
// A recursive mutex lets FAT32 hold the lock across a whole operation while
// still calling readBlock()/writeBlock() internally.
static recursive_mutex_t g_sdMutex;
static volatile bool g_sdMutexReady = false;

// Initialised from the SDCardManager constructor, which runs in main() on
// core0 before core1 is launched and before any SD access.
static void sdBusLockInit() {
    if (!g_sdMutexReady) {
        recursive_mutex_init(&g_sdMutex);
        g_sdMutexReady = true;
    }
}

void sdBusLock() {
    sdBusLockInit();
    recursive_mutex_enter_blocking(&g_sdMutex);
}

void sdBusUnlock() {
    recursive_mutex_exit(&g_sdMutex);
}

bool sdBusTryLock() {
    sdBusLockInit();
    return recursive_mutex_try_enter(&g_sdMutex, nullptr);
}

// Constructor
SDCardManager::SDCardManager(spi_inst_t* spi, uint8_t cs, uint8_t mosi, uint8_t miso, uint8_t sck, uint8_t detect) {
    sdBusLockInit();
    spiInstance = spi;
    csPin = cs;
    mosiPin = mosi;
    misoPin = miso;
    sckPin = sck;
    detectPin = detect;
    initialized = false;
    blockAddressing = true;
    ocrCCS = false;
    cmd58Ok = false;
    csdVersion = 0xFF;
    addressingCorrected = false;
    fat32 = nullptr;
    currentBaudrate = 0;
    lastFAT32Error = FAT32_OK;
    
    // Initialize card detect pin if provided
    if (detectPin != 0xFF) {
        gpio_init(detectPin);
        gpio_set_dir(detectPin, GPIO_IN);
        gpio_pull_up(detectPin);  // Pull-up resistor (active LOW means card present)
    }
}

// Initialize SD card
bool SDCardManager::init(uint32_t maxBaudrate, bool verbose) {
    SDLockGuard guard;
    sectorCount = 0;
    if (verbose) {
        DBG_PRINTF("SD Init: Starting initialization...\r\n");
    }
    
    // If already initialized, fully deinitialize first
    if (initialized) {
        if (verbose) {
            DBG_PRINTF("SD Init: Card already initialized, deinitializing first...\r\n");
        }
        deselectCard();
        sleep_ms(100);
        uint8_t dummyByte = 0xFF;
        for (int i = 0; i < 100; i++) {
            spi_write_blocking(spiInstance, &dummyByte, 1);
        }
        waitForReady();
        sleep_ms(100);
        initialized = false;
    }
    
    // Drop any filesystem object from a previous init (was leaked before)
    if (fat32) {
        delete fat32;
        fat32 = nullptr;
    }

    // Initialize SPI with low speed first (100-400kHz for initialization)
    spi_init(spiInstance, 400000);  // 400kHz for initialization (max allowed during init)
    gpio_set_function(mosiPin, GPIO_FUNC_SPI);
    gpio_set_function(misoPin, GPIO_FUNC_SPI);
    gpio_set_function(sckPin, GPIO_FUNC_SPI);
    // SD DO is tri-stated while the card is deselected/busy-releasing. The RP2350
    // pad default is pull-DOWN (and erratum E9 can latch it), which makes an idle
    // line read as 0x00 instead of 0xFF. SD-over-SPI requires a pull-up on DO.
    gpio_pull_up(misoPin);

    // Initialize CS pin
    gpio_init(csPin);
    gpio_set_dir(csPin, GPIO_OUT);
    gpio_put(csPin, 1);  // Deselect (high = inactive)
    sleep_ms(250);  // Wait longer for card to stabilize (especially for large capacity cards)
    
    if (verbose) {
        DBG_PRINTF("SD Init: Sending 80+ clock cycles...\r\n");
    }
    
    // Send 80+ clock cycles to initialize (card needs this to wake up)
    // SD spec requires at least 74 clock cycles with CS high
    // We send more for better compatibility with various cards
    deselectCard();
    uint8_t dummy = 0xFF;
    for (int i = 0; i < 100; i++) {  // 800 clock cycles for better compatibility
        spi_write_blocking(spiInstance, &dummy, 1);
    }
    sleep_ms(100);
    
    if (verbose) {
        DBG_PRINTF("SD Init: Sending CMD0 (reset)...\r\n");
    }
    
    // Send CMD0 to reset card (go to idle state) with retries
    // Some cards (especially SDXC) need multiple attempts
    uint8_t response = 0xFF;
    for (int retry = 0; retry < 10; retry++) {
        selectCard();
        response = sendCommand(SD_CMD0, 0);
        deselectCard();
        sleep_ms(50);
        
        if (response == 0x01) {  // IDLE state - success
            break;
        }
        
        // Send more clock cycles between retries
        for (int i = 0; i < 20; i++) {
            spi_write_blocking(spiInstance, &dummy, 1);
        }
        sleep_ms(50);
    }
    
    if (verbose) {
        DBG_PRINTF("SD Init: CMD0 response = 0x%02X\r\n", response);
    }
    
    // Check if card responded with idle state
    if (response != 0x01) {
        if (verbose) {
            DBG_PRINTF("SD Init: ERROR - CMD0 failed, response = 0x%02X\r\n", response);
        }
        return false;
    }
    
    // Try CMD8 (only for SDHC/SDXC cards, version 2.0+)
    // This command may fail for older cards, which is OK
    bool isSDHC = false;
    bool isVersion2 = false;
    
    if (verbose) {
        DBG_PRINTF("SD Init: Sending CMD8 (check voltage)...\r\n");
    }
    
    selectCard();
    response = sendCommand(SD_CMD8, 0x1AA);  // 0x1AA = voltage check pattern
    
    if (verbose) {
        DBG_PRINTF("SD Init: CMD8 response = 0x%02X\r\n", response);
    }
    
    if (response == SD_R1_IDLE) {
        // Card responded, read R7 response (4 bytes)
        uint8_t r7[4];
        for (int i = 0; i < 4; i++) {
            spi_read_blocking(spiInstance, 0xFF, &r7[i], 1);
        }
        
        if (verbose) {
            DBG_PRINTF("SD Init: CMD8 R7 = %02X %02X %02X %02X\r\n", r7[0], r7[1], r7[2], r7[3]);
        }
        
        // Check if voltage and check pattern match
        if (r7[2] == 0x01 && r7[3] == 0xAA) {
            isVersion2 = true;  // SD version 2.0+ (SDHC/SDXC capable)
            if (verbose) {
                DBG_PRINTF("SD Init: Detected SD version 2.0+ card (SDHC/SDXC capable)\r\n");
            }
        }
    } else {
        if (verbose) {
            DBG_PRINTF("SD Init: CMD8 not supported (SD version 1.x or MMC)\r\n");
        }
    }
    deselectCard();
    sleep_ms(50);
    
    // Check if CMD8 failed with illegal command - might be MMC card
    bool isMMC = (response == 0x05 || response == 0x09);  // Illegal command response to CMD8
    
    if (isMMC && verbose) {
        DBG_PRINTF("SD Init: Detected possible MMC card (CMD8 illegal)\r\n");
    }
    
    // Try ACMD41 to initialize card (with HCS bit for version 2.0+ cards)
    // HCS bit (Host Capacity Support) indicates we support SDHC/SDXC
    uint32_t acmd41Arg = isVersion2 ? 0x40000000 : 0x00000000;  // HCS bit for SDHC/SDXC
    
    if (verbose) {
        DBG_PRINTF("SD Init: Sending ACMD41 (initialize, arg=0x%08X)...\r\n", acmd41Arg);
    }
    
    response = 0xFF;
    // Try ACMD41 for SD cards
    // SDXC cards may need up to 1 second (1000ms) to initialize
    selectCard();
    for (int i = 0; i < 500; i++) {  // Increased retries for large capacity cards
        // First send CMD55 (app command prefix)
        uint8_t cmd55Response = sendCommand(SD_CMD55, 0);
        if (verbose && i == 0) {
            DBG_PRINTF("SD Init: CMD55 response = 0x%02X\r\n", cmd55Response);
        }
        
        if (cmd55Response != 0x01 && cmd55Response != 0x00) {
            // CMD55 failed - card might not support ACMD commands
            if (verbose) {
                DBG_PRINTF("SD Init: CMD55 failed, response = 0x%02X\r\n", cmd55Response);
            }
            deselectCard();
            break;
        }
        
        // Now send ACMD41 with HCS bit
        response = sendCommand(SD_ACMD41, acmd41Arg);
        if (response == 0) {
            if (verbose && i > 0) {
                DBG_PRINTF("SD Init: ACMD41 succeeded after %d attempts\r\n", i + 1);
            }
            break;  // Card is ready
        }
        if (i < 20) {
            sleep_ms(1);  // Shorter delay for first attempts
        } else if (i < 100) {
            sleep_ms(10);  // Medium delay
        } else {
            sleep_ms(20);  // Longer delay for stubborn cards
        }
        if (verbose && (i == 0 || i == 9 || i == 49 || i == 99 || i == 199 || i == 499)) {
            DBG_PRINTF("SD Init: ACMD41 attempt %d, response = 0x%02X\r\n", i + 1, response);
        }
    }
    deselectCard();
    
    if (response != 0) {
        if (verbose) {
            DBG_PRINTF("SD Init: ACMD41 with HCS failed, trying without HCS...\r\n");
        }
        // Try without HCS bit (for older cards)
        selectCard();
        for (int i = 0; i < 200; i++) {
            // Send CMD55 first
            uint8_t cmd55Response = sendCommand(SD_CMD55, 0);
            if (cmd55Response != 0x01 && cmd55Response != 0x00) {
                deselectCard();
                break;
            }
            
            response = sendCommand(SD_ACMD41, 0x00000000);
            if (response == 0) {
                if (verbose) {
                    DBG_PRINTF("SD Init: ACMD41 succeeded without HCS after %d attempts\r\n", i + 1);
                }
                isVersion2 = false;  // Card doesn't support SDHC/SDXC
                break;
            }
            sleep_ms(10);
        }
        deselectCard();
        
        // SD 1.x also rejects CMD8. Only try MMC after ACMD41 fails.
        if (response != 0 && isMMC) {
            selectCard();
            for (int i = 0; i < 100; ++i) {
                response = sendCommand(1, 0);
                if (response == 0 || (response & SD_R1_ILLEGAL_CMD)) break;
                sleep_ms(10);
            }
            deselectCard();
        }
        if (response != 0) {
            if (verbose) {
                DBG_PRINTF("SD Init: ERROR - ACMD41 failed, response = 0x%02X\r\n", response);
                DBG_PRINTF("SD Init: Card may be MMC or not properly powered\r\n");
            }
            return false;
        }
    }
    
    
    sleep_ms(50);
    
    // Send CMD58 to read OCR (required to determine card capacity type)
    if (verbose) {
        DBG_PRINTF("SD Init: Sending CMD58 (read OCR)...\r\n");
    }
    selectCard();
    cmd58Ok = false;
    response = sendCommand(SD_CMD58, 0);
    if (response == 0) {
        cmd58Ok = true;
        uint8_t ocr[4];
        for (int i = 0; i < 4; i++) {
            spi_read_blocking(spiInstance, 0xFF, &ocr[i], 1);
        }
        
        if (verbose) {
            DBG_PRINTF("SD Init: OCR = %02X %02X %02X %02X\r\n", ocr[0], ocr[1], ocr[2], ocr[3]);
        }
        
        // Check card power up status (bit 31)
        if (ocr[0] & 0x80) {
            if (verbose) {
                DBG_PRINTF("SD Init: Card power up status: OK\r\n");
            }
            
            // Check CCS bit (bit 30) - Card Capacity Status
            // CCS = 1 means SDHC/SDXC (block addressing)
            // CCS = 0 means SDSC (byte addressing)
            if (ocr[0] & 0x40) {
                isSDHC = true;  // Card is SDHC or SDXC
                if (verbose) {
                    DBG_PRINTF("SD Init: Card type: SDHC/SDXC (block addressing, 32GB+)\r\n");
                    DBG_PRINTF("SD Init: Card supports: V30/V60/V90 speed classes if labeled\r\n");
                }
            } else {
                if (verbose) {
                    DBG_PRINTF("SD Init: Card type: SDSC (byte addressing, up to 2GB)\r\n");
                }
            }
        } else {
            if (verbose) {
                DBG_PRINTF("SD Init: WARNING - Card power up not complete\r\n");
            }
        }
        
        // Check voltage support
        if (verbose) {
            if (ocr[1] & 0x80) DBG_PRINTF("SD Init: Supports 3.5-3.6V\r\n");
            if (ocr[1] & 0x40) DBG_PRINTF("SD Init: Supports 3.4-3.5V\r\n");
            if (ocr[1] & 0x20) DBG_PRINTF("SD Init: Supports 3.3-3.4V\r\n");
            if (ocr[1] & 0x10) DBG_PRINTF("SD Init: Supports 3.2-3.3V\r\n");
        }
    } else {
        if (verbose) {
            DBG_PRINTF("SD Init: CMD58 failed, response = 0x%02X\r\n", response);
        }
    }
    deselectCard();
    sleep_ms(50);
    
    // Set block size to 512 bytes (CMD16) - only for standard SD cards
    // SDHC/SDXC cards have fixed 512-byte blocks and use block addressing
    if (!isSDHC) {
        if (verbose) {
            DBG_PRINTF("SD Init: Sending CMD16 (set block size to 512 bytes)...\r\n");
        }
        selectCard();
        response = sendCommand(SD_CMD16, 512);
        deselectCard();
        if (response != 0) {
            if (verbose) {
                DBG_PRINTF("SD Init: ERROR - CMD16 failed, response = 0x%02X\r\n", response);
            }
            return false;
        }
        sleep_ms(50);
    } else {
        if (verbose) {
            DBG_PRINTF("SD Init: SDHC/SDXC card detected - using block addressing (512-byte blocks)\r\n");
        }
    }


    // SDSC cards use byte addresses for CMD17/CMD24; SDHC/SDXC use block numbers.
    // V1 always used block numbers, which works on every SDHC/SDXC card; the
    // mode is now taken from two registers so that a single misread bit cannot
    // switch an SDHC card to byte addresses (all reads and writes would then go
    // to sector * 512 - another place than a PC uses):
    //   CSD v2 (structure 1)  -> SDHC/SDXC -> block addresses
    //   CSD v1                -> SDSC      -> byte addresses, unless OCR says CCS
    //   CSD unreadable        -> OCR CCS; if both reads fail, use CMD8 support
    ocrCCS = isSDHC;
    initialized = true;

    // Set SPI speed to specified maximum baudrate
    currentBaudrate = isVersion2 ? maxBaudrate : (maxBaudrate < 4000000 ? maxBaudrate : 4000000);
    spi_set_baudrate(spiInstance, currentBaudrate);

    if (verbose) {
        DBG_PRINTF("SD Init: SPI speed set to %u Hz (%.2f MHz)\r\n",
               currentBaudrate, currentBaudrate / 1000000.0f);
    }

    {
        uint8_t csd[16];
        // CSD uses register addressing, so it can be read before choosing LBA.
        csdVersion = readCSDWithRetry(csd) ? (uint8_t)(csd[0] >> 6) : 0xFF;
        if (csdVersion <= 1) sectorCount = decodeSectorCount(csd);
    }
    if (csdVersion == 1) {
        blockAddressing = true;
    } else if (csdVersion == 0) {
        blockAddressing = ocrCCS;
    } else {
        blockAddressing = ocrCCS || (!cmd58Ok && isVersion2);
    }
    if (verbose) {
        DBG_PRINTF("SD Init: CCS=%d CMD58=%d CSD v%d -> %s addressing\r\n", ocrCCS, cmd58Ok,
               csdVersion == 0xFF ? -1 : csdVersion + 1, blockAddressing ? "block" : "byte");
    }

    // Initialize FAT32 filesystem
    return mountFat32();
}

bool SDCardManager::isInitialized() const {
    return initialized;
}

void SDCardManager::deinit() {
    if (initialized) {
        // Clean up FAT32
        if (fat32) {
            delete fat32;
            fat32 = nullptr;
        }
        
        // Deselect card
        deselectCard();
        
        initialized = false;
    }
}

void SDCardManager::remount() {
    // Re-read the filesystem (used after formatting the card)
    SDLockGuard guard;
    if (!initialized) return;
    mountFat32();
}

// Create the FAT32 object and mount the volume. If no FAT32 volume is found
// with the addressing mode from the card registers, the other mode is tried
// (reads only). It is kept only if it finds a complete FAT32 boot sector whose
// "hidden sectors" field matches the partition table, i.e. exactly the layout
// a PC sees - so a card with a misreported capacity class still works and,
// more importantly, is never written at addresses different from a PC's.
bool SDCardManager::mountFat32() {
    SDLockGuard guard;
    if (fat32) {
        delete fat32;
        fat32 = nullptr;
    }
    addressingCorrected = false;
    fat32 = new FAT32(this);
    if (fat32->init()) {
        lastFAT32Error = FAT32_OK;
        return true;
    }
    FAT32_Error first = fat32->getLastError();
    if (first == FAT32_ERROR_UNKNOWN_FS || first == FAT32_ERROR_INVALID_PARAMS ||
        first == FAT32_ERROR_READ_FAILED) {
        blockAddressing = !blockAddressing;
        if (fat32->init() && fat32->getHiddenSectors() == fat32->getPartitionStartSector()) {
            addressingCorrected = true;
            lastFAT32Error = FAT32_OK;
            return true;
        }
        blockAddressing = !blockAddressing;
    }
    lastFAT32Error = first;
    delete fat32;
    fat32 = nullptr;
    return false;
}

bool SDCardManager::isCardPresent() const {
    if (detectPin == 0xFF) {
        // No detect pin configured, assume card is always present
        return true;
    }
    return !gpio_get(detectPin);
}

// CMD9: read the 16-byte CSD register
bool SDCardManager::readCSD(uint8_t* csd) {
    SDLockGuard guard;
    selectCard();
    if (!waitReadyMs(500)) {
        deselectCard();
        return false;
    }
    bool ok = false;
    uint8_t response = sendCommand(SD_CMD9, 0);  // SEND_CSD
    if (response == 0) {
        uint8_t token = 0xFF;
        absolute_time_t deadline = make_timeout_time_ms(200);
        do {
            spi_read_blocking(spiInstance, 0xFF, &token, 1);
            if (token != 0xFF) break;
        } while (!time_reached(deadline));
        if (token == 0xFE) {
            spi_read_blocking(spiInstance, 0xFF, csd, 16);
            uint8_t crc[2];
            spi_read_blocking(spiInstance, 0xFF, crc, 2);
            ok = true;
        }
    }
    deselectCard();
    return ok;
}

bool SDCardManager::readCSDWithRetry(uint8_t* csd) {
    if (readCSD(csd) && decodeSectorCount(csd)) return true;
    spi_set_baudrate(spiInstance, 400000);
    bool ok = false;
    for (int i = 0; i < 3 && !ok; ++i) ok = readCSD(csd) && decodeSectorCount(csd);
    spi_set_baudrate(spiInstance, currentBaudrate);
    return ok;
}

uint32_t SDCardManager::getSectorCount() {
    SDLockGuard guard;
    if (!initialized) return 0;
    if (!sectorCount) {
        uint8_t csd[16];
        if (readCSDWithRetry(csd)) sectorCount = decodeSectorCount(csd);
    }
    return sectorCount;
}

uint32_t SDCardManager::decodeSectorCount(const uint8_t* csd) {
    uint32_t sectors = 0;
    uint8_t csdVer = (csd[0] >> 6) & 0x03;
    if (csdVer == 1) {
        // CSD v2.0 (SDHC/SDXC): C_SIZE is 22 bits
        uint32_t csize = ((uint32_t)(csd[7] & 0x3F) << 16) |
                         ((uint32_t)csd[8] << 8) | csd[9];
        sectors = (csize + 1) * 1024;
    } else if (csdVer == 0) {
        // CSD v1.0 (standard capacity SD)
        uint32_t csize = ((uint32_t)(csd[6] & 0x03) << 10) |
                         ((uint32_t)csd[7] << 2) | ((csd[8] >> 6) & 0x03);
        // MULT = 2^(C_SIZE_MULT + 2); capacity = (C_SIZE+1) * MULT * 2^READ_BL_LEN
        uint32_t cSizeMult = ((csd[9] & 0x03) << 1) | ((csd[10] >> 7) & 0x01);
        uint32_t mult = 1UL << (cSizeMult + 2);
        uint32_t readBlLen = csd[5] & 0x0F;
        if (readBlLen <= 11) {
            sectors = ((uint64_t)(csize + 1) * mult << readBlLen) / SD_BLOCK_SIZE;
        }
    }
    return sectors;
}

bool SDCardManager::probeCard() {
    SDLockGuard guard;
    // Try the lower clock before interpreting a communication error as removal.
    uint8_t csd[16];
    return initialized && readCSDWithRetry(csd);
}

// Select SD card (CS low)
void SDCardManager::selectCard() {
    gpio_put(csPin, 0);
    waitForReady();
}

// Deselect SD card (CS high)
void SDCardManager::deselectCard() {
    gpio_put(csPin, 1);
    waitForReady();
}

// Wait for card to be ready
void SDCardManager::waitForReady() {
    uint8_t dummy = 0xFF;
    uint8_t response = 0;
    for (int i = 0; i < 200; i++) {  // Increased timeout
        spi_read_blocking(spiInstance, dummy, &response, 1);
        if (response == 0xFF) {
            break;
        }
    }
}

// Wait (time based) until the card stops signalling busy. Must be called with
// CS asserted. A single-block write can keep the card busy for up to 250 ms
// (SDXC: 500 ms); the old fixed 200/1000-byte loops gave up after <1 ms, so
// the next command was sent while the card was still busy, its "response"
// read back as 0x00 (= OK) and the following write/read silently failed.
bool SDCardManager::waitReadyMs(uint32_t timeoutMs) {
    uint8_t response = 0;
    absolute_time_t deadline = make_timeout_time_ms(timeoutMs);
    do {
        spi_read_blocking(spiInstance, 0xFF, &response, 1);
        if (response == 0xFF) {
            return true;
        }
    } while (!time_reached(deadline));
    return false;
}

// Send command to SD card
uint8_t SDCardManager::sendCommand(uint8_t cmd, uint32_t arg) {
    uint8_t command[6];
    command[0] = 0x40 | cmd;
    command[1] = (arg >> 24) & 0xFF;
    command[2] = (arg >> 16) & 0xFF;
    command[3] = (arg >> 8) & 0xFF;
    command[4] = arg & 0xFF;
    
    // Calculate CRC for CMD0 and CMD8
    if (cmd == SD_CMD0) {
        command[5] = 0x95;  // CRC for CMD0
    } else if (cmd == SD_CMD8) {
        command[5] = 0x87;  // CRC for CMD8 (correct CRC for arg 0x1AA)
    } else {
        command[5] = 0xFF;  // No CRC for other commands (or dummy CRC)
    }
    
    // Send command
    spi_write_blocking(spiInstance, command, 6);
    
    // Wait for response (R1 format)
    // Response starts with 0 bit, so we wait until MSB is 0
    uint8_t response = 0xFF;
    for (int i = 0; i < 20; i++) {  // Increased timeout
        spi_read_blocking(spiInstance, 0xFF, &response, 1);
        if ((response & 0x80) == 0) {  // Response bit cleared (MSB = 0)
            break;
        }
    }
    
    return response;
}


// Read a single block (512 bytes)
bool SDCardManager::readBlock(uint32_t blockAddress, uint8_t* buffer) {
    if (!initialized || buffer == nullptr) {
        return false;
    }
    
    SDLockGuard guard;
    bool result = false;

    selectCard();

    // Make sure a previous write has really finished before sending a command
    if (!waitReadyMs(500)) {
        deselectCard();
        return false;
    }

    // Send CMD17 (read single block)
    uint8_t response = sendCommand(SD_CMD17, cardAddress(blockAddress));
    if (response == 0) {
        // Wait for data token (0xFE); spec allows up to 100 ms
        uint8_t token = 0xFF;
        absolute_time_t deadline = make_timeout_time_ms(200);
        do {
            spi_read_blocking(spiInstance, 0xFF, &token, 1);
            if (token == 0xFE) {
                break;
            }
            if (token != 0xFF && token != 0x00) {
                break;  // Data error token
            }
        } while (!time_reached(deadline));

        if (token == 0xFE) {
            // Read 512 bytes
            spi_read_blocking(spiInstance, 0xFF, buffer, SD_BLOCK_SIZE);

            // Read CRC (2 bytes) - ignore
            uint8_t crc[2];
            spi_read_blocking(spiInstance, 0xFF, crc, 2);
            result = true;
        }
    }

    deselectCard();
    return result;
}

// Write a single block (512 bytes)
bool SDCardManager::writeBlock(uint32_t blockAddress, const uint8_t* buffer) {
    if (!initialized || buffer == nullptr) {
        return false;
    }

    SDLockGuard guard;
    selectCard();

    if (!waitReadyMs(500)) {
        deselectCard();
        return false;
    }

    // Send CMD24 (write single block)
    uint8_t response = sendCommand(SD_CMD24, cardAddress(blockAddress));
    if (response != 0) {
        deselectCard();
        return false;
    }

    // One gap byte (Nwr) + data token
    uint8_t hdr[2] = {0xFF, 0xFE};
    spi_write_blocking(spiInstance, hdr, 2);

    // Write 512 bytes
    spi_write_blocking(spiInstance, buffer, SD_BLOCK_SIZE);

    // Send dummy CRC
    uint8_t crc[2] = {0xFF, 0xFF};
    spi_write_blocking(spiInstance, crc, 2);

    // Data response token is the first non-0xFF byte: xxx0sss1, 0x05 = accepted
    uint8_t writeResponse = 0xFF;
    for (int i = 0; i < 64; i++) {
        spi_read_blocking(spiInstance, 0xFF, &writeResponse, 1);
        if (writeResponse != 0xFF) {
            break;
        }
    }
    bool accepted = ((writeResponse & 0x1F) == 0x05);

    // Wait for the internal programming to finish (busy = MISO held low)
    bool ready = waitReadyMs(500);

    deselectCard();
    return accepted && ready;
}

// Zero-fill consecutive blocks with one CMD25 (multi-block write).
// Formatting writes tens of thousands of FAT sectors; doing that one CMD24 at a
// time takes minutes on large cards.
bool SDCardManager::writeZeroBlocks(uint32_t startBlock, uint32_t count) {
    if (!initialized) {
        return false;
    }
    if (count == 0) {
        return true;
    }

    static const uint8_t zeroBlock[SD_BLOCK_SIZE] = {0};

    SDLockGuard guard;
    selectCard();

    if (!waitReadyMs(500)) {
        deselectCard();
        return false;
    }

    uint8_t response = sendCommand(SD_CMD25, cardAddress(startBlock));
    if (response != 0) {
        deselectCard();
        return false;
    }

    bool ok = true;
    for (uint32_t i = 0; i < count; i++) {
        uint8_t hdr[2] = {0xFF, 0xFC};  // gap byte + "start block" token for CMD25
        spi_write_blocking(spiInstance, hdr, 2);
        spi_write_blocking(spiInstance, zeroBlock, SD_BLOCK_SIZE);
        uint8_t crc[2] = {0xFF, 0xFF};
        spi_write_blocking(spiInstance, crc, 2);

        uint8_t dataResp = 0xFF;
        for (int r = 0; r < 64; r++) {
            spi_read_blocking(spiInstance, 0xFF, &dataResp, 1);
            if (dataResp != 0xFF) {
                break;
            }
        }
        if ((dataResp & 0x1F) != 0x05 || !waitReadyMs(500)) {
            ok = false;
            break;
        }
    }

    // "Stop transmission" token, then wait for the card to finish programming
    uint8_t stop[2] = {0xFD, 0xFF};
    spi_write_blocking(spiInstance, stop, 2);
    if (!waitReadyMs(1000)) {
        ok = false;
    }

    deselectCard();
    return ok;
}

// Read file using FAT32
bool SDCardManager::readFile(const char* filename, uint8_t* buffer, uint32_t maxSize, uint32_t* bytesRead) {
    if (!initialized || !buffer) {
        return false;
    }
    SDLockGuard guard;
    
    // Use FAT32 if available
    if (fat32) {
        return fat32->readFile(filename, buffer, maxSize, bytesRead);
    }
    
    // Fallback: simplified read from block 0
    if (bytesRead) {
        *bytesRead = 0;
    }
    
    if (readBlock(0, buffer)) {
        if (bytesRead) {
            *bytesRead = SD_BLOCK_SIZE < maxSize ? SD_BLOCK_SIZE : maxSize;
        }
        return true;
    }
    
    return false;
}

bool SDCardManager::fileExists(const char* filename) {
    if (!initialized) {
        return false;
    }
    SDLockGuard guard;
    
    // Use FAT32 if available
    if (fat32) {
        return fat32->fileExists(filename);
    }
    
    // Fallback: always return false
    return false;
}

// List files using FAT32
bool SDCardManager::listFiles(char* fileList, uint32_t maxSize, uint32_t* fileCount) {
    if (!initialized || !fileList) {
        return false;
    }
    SDLockGuard guard;
    
    if (fat32) {
        return fat32->listFiles(fileList, maxSize, fileCount);
    }
    
    return false;
}

// Get FAT32 instance
FAT32* SDCardManager::getFAT32() const {
    return fat32;
}

// Load disk image from SD card using FAT32.
// The file is opened as a handle (whole cluster chain validated once) and read
// sector by sector straight into `buffer`. The handle is returned in fileOut so
// later track writes go to exactly this file without any directory search.
bool SDCardManager::loadDiskImage(const char* filename, uint8_t* buffer, uint32_t bufferSize, uint32_t* bytesRead, FAT32_File* fileOut) {
    if (bytesRead) *bytesRead = 0;
    if (fileOut) fileOut->valid = false;
    if (!initialized || buffer == nullptr || !filename) {
        return false;
    }
    SDLockGuard guard;
    if (!fat32) {
        return false;
    }
    
    FAT32_File file;
    if (!fat32->openFile(filename, 0, &file) || file.size == 0) {
        return false;
    }
    
    // Images larger than the buffer are refused, never truncated: a cut WOZ
    // or DSK would be played (and later written back) with missing tracks.
    // Exception: a large .woz -> only its header is loaded; the emulator then
    // reads tracks on demand through the returned handle (WOZ2 only, checked
    // by the WOZ parser).
    if (file.size > bufferSize) {
        const char* ext = strrchr(filename, '.');
        bool isWoz = ext && (ext[1] == 'w' || ext[1] == 'W') && (ext[2] == 'o' || ext[2] == 'O') &&
                     (ext[3] == 'z' || ext[3] == 'Z') && ext[4] == 0;
        if (!isWoz || bufferSize < WOZ2_HEADER_BYTES) {
            return false;
        }
        uint32_t done = 0;
        if (!fat32->fileRead(&file, 0, buffer, WOZ2_HEADER_BYTES, &done) || done != WOZ2_HEADER_BYTES) {
            return false;
        }
        if (bytesRead) *bytesRead = done;
        if (fileOut) *fileOut = file;
        return true;
    }
    
    uint32_t toRead = (file.size < bufferSize) ? file.size : bufferSize;
    uint32_t done = 0;
    if (!fat32->fileRead(&file, 0, buffer, toRead, &done) || done != toRead) {
        return false;
    }
    if (bytesRead) *bytesRead = done;
    if (fileOut) *fileOut = file;
    return true;
}

// Read from an opened file (exactly `size` bytes or false)
bool SDCardManager::readFileAt(const FAT32_File* file, uint32_t offset, uint8_t* data, uint32_t size) {
    if (!initialized || !file || !data) {
        return false;
    }
    SDLockGuard guard;
    if (!fat32) {
        return false;
    }
    uint32_t done = 0;
    return fat32->fileRead(file, offset, data, size, &done) && done == size;
}

// Write into an opened file (never changes its size / FAT / directory)
bool SDCardManager::writeFile(const FAT32_File* file, uint32_t offset, const uint8_t* data, uint32_t size) {
    if (!initialized || !file || !data) {
        return false;
    }
    SDLockGuard guard;
    if (!fat32) {
        return false;
    }
    return fat32->fileWrite(file, offset, data, size);
}


