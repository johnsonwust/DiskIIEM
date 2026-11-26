#ifndef SD_CARD_MANAGER_H
#define SD_CARD_MANAGER_H

#include "pico/stdlib.h"
#include "hardware/spi.h"
#include "PinConfig.h"
#include "FAT32.h"
#include "FloppyEmulator.h"
#include <stdint.h>
#include <stdbool.h>

// SD Card commands
#define SD_CMD0            0
#define SD_CMD8            8
#define SD_CMD9            9   // SEND_CSD
#define SD_CMD16           16
#define SD_CMD17           17  // Read single block
#define SD_CMD24           24  // Write single block
#define SD_CMD25           25  // Write multiple blocks
#define SD_CMD55           55
#define SD_CMD58           58
#define SD_ACMD41          41

// SD Card response types
#define SD_R1              1
#define SD_R1_IDLE         0x01
#define SD_R1_ILLEGAL_CMD  0x04

// SD Card block size
#define SD_BLOCK_SIZE      512

// Cross-core lock for the SD card / FAT32 layer (recursive, NOT usable from IRQ).
// Core0 (track saves) and core1 (UI: list/load/config/format) both use the card;
// FAT32 also has shared state (static buffers, current directory), so every
// multi-step SD/FAT32 operation must hold this lock.
void sdBusLock();
void sdBusUnlock();
// Take the lock only if it is free right now (or already ours). Used by the
// core0 flush so the drive emulation never waits behind a UI operation.
bool sdBusTryLock();

class SDLockGuard {
public:
    SDLockGuard() { sdBusLock(); }
    ~SDLockGuard() { sdBusUnlock(); }
    SDLockGuard(const SDLockGuard&) = delete;
    SDLockGuard& operator=(const SDLockGuard&) = delete;
};

class SDCardManager {
private:
    spi_inst_t* spiInstance;
    uint8_t csPin;
    uint8_t mosiPin;
    uint8_t misoPin;
    uint8_t sckPin;
    uint8_t detectPin;  // Card detect pin (optional, 0xFF if not used)
    bool initialized;
    uint32_t sectorCount = 0;
    bool blockAddressing;  // true = SDHC/SDXC (LBA), false = SDSC (byte address)
    bool ocrCCS;           // OCR "card capacity status" bit (CMD58)
    bool cmd58Ok;          // CMD58 answered
    uint8_t csdVersion;    // CSD_STRUCTURE: 0 = SDSC, 1 = SDHC/SDXC, 0xFF = not read
    bool addressingCorrected;  // the file system was only found with the other addressing mode
    uint32_t currentBaudrate;  // Current SPI baudrate
    
    // FAT32 filesystem
    FAT32* fat32;
    FAT32_Error lastFAT32Error;  // Store last FAT32 error (preserved even if FAT32 deleted)
    
    // Internal methods
    void selectCard();
    void deselectCard();
    uint8_t sendCommand(uint8_t cmd, uint32_t arg);
    void waitForReady();
    bool waitReadyMs(uint32_t timeoutMs);   // Wait until card releases busy (MISO=0xFF)
    uint32_t cardAddress(uint32_t block) const { return blockAddressing ? block : block * SD_BLOCK_SIZE; }
    bool waitForResponse(uint8_t* response, uint32_t timeout);
    bool readCSD(uint8_t* csd);              // CMD9, 16 bytes
    bool readCSDWithRetry(uint8_t* csd);
    static uint32_t decodeSectorCount(const uint8_t* csd);
    bool mountFat32();                       // (re)create fat32 and mount the volume
    
public:
    // Constructor
    SDCardManager(spi_inst_t* spi, uint8_t cs, uint8_t mosi, uint8_t miso, uint8_t sck, uint8_t detect = 0xFF);
    
    // Initialization
    bool init(uint32_t maxBaudrate = 20000000, bool verbose = false);  // maxBaudrate in Hz (default 20MHz)
    void deinit();
    void remount();  // Re-read the filesystem after a format (no SD re-init)
    bool isInitialized() const;
    
    // Card detection
    bool isCardPresent() const;
    bool probeCard();  // Check whether the initialized card still answers CMD9
    
    // Read/Write operations
    bool readBlock(uint32_t blockAddress, uint8_t* buffer);
    bool writeBlock(uint32_t blockAddress, const uint8_t* buffer);
    // Fill `count` consecutive blocks with zeros (CMD25 multi-block write, used by format)
    bool writeZeroBlocks(uint32_t startBlock, uint32_t count);

    // File operations (FAT32)
    bool readFile(const char* filename, uint8_t* buffer, uint32_t maxSize, uint32_t* bytesRead);
    bool fileExists(const char* filename);
    bool listFiles(char* fileList, uint32_t maxSize, uint32_t* fileCount);
    
    // Disk image loading
    // fileOut (optional) receives a handle for later track writes
    bool loadDiskImage(const char* filename, uint8_t* buffer, uint32_t bufferSize, uint32_t* bytesRead, FAT32_File* fileOut = nullptr);
    // Read exactly `size` bytes from an opened file
    bool readFileAt(const FAT32_File* file, uint32_t offset, uint8_t* data, uint32_t size);
    // Overwrite data inside an opened file (never grows it or touches FAT/dir)
    bool writeFile(const FAT32_File* file, uint32_t offset, const uint8_t* data, uint32_t size);
    // dirCluster: directory that holds the image (0 = FAT32 current directory)
    
    // FAT32 access
    FAT32* getFAT32() const;
    FAT32_Error getLastFAT32Error() const { return lastFAT32Error; }
    
    // Speed testing
    
    // Get current SPI speed in Hz
    uint32_t getCurrentBaudrate() const { return currentBaudrate; }
    
    // Total number of 512-byte blocks on the card (reads CSD)
    uint32_t getSectorCount();

    // Card information for the status screen
    bool isHighCapacity() const { return csdVersion == 1 || ocrCCS; }
    bool usesBlockAddressing() const { return blockAddressing; }
    bool addressingWasCorrected() const { return addressingCorrected; }
};

#endif // SD_CARD_MANAGER_H

