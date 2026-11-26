#include "FloppyEmulator.h"
#include "DebugOut.h"
#include "SDCardManager.h"
#include "ConfigManager.h"
#include "DiskCodec.h"
#include "Beeper.h"
#include "hardware/gpio.h"
#include "hardware/sync.h"
#include "hardware/irq.h"
#include "hardware/timer.h"
#include "hardware/pio.h"
#include "hardware/pio_instructions.h"
#include "hardware/dma.h"
#include <cstdint>
#include <pico/time.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include "PinConfig.h"
#ifndef WPROT_OUTPUT_INVERTED
#error "WPROT_OUTPUT_INVERTED must be defined in PinConfig.h (1 = board with an inverting buffer, 0 = direct)"
#endif
#ifndef READ_OUTPUT_INVERTED
#error "READ_OUTPUT_INVERTED must be defined in PinConfig.h (1 = board with an inverting buffer, 0 = direct)"
#endif
#include "hardware/pwm.h"

// Host test hook (tools/hosttest): estimated CPU cycles of the stream refill,
// so the simulation can charge the time the interrupt takes. Empty here.
#ifndef HOSTTEST_CYCLES
#define HOSTTEST_CYCLES(n) ((void)0)
#endif

// Static pointer to FloppyEmulator instance for timer callback and DMA IRQ
static FloppyEmulator* g_floppyEmulatorInstance = nullptr;

static const uint WRITE_TIMER_SLICE = 7;
static uint slice_num;


// Constructor
FloppyEmulator::FloppyEmulator(
    uint8_t ph0, uint8_t ph1, uint8_t ph2, uint8_t ph3,
    uint8_t read, uint8_t write, uint8_t writeEnable, uint8_t driveSel
) {
    // Initialize stepper motor pins
    stepperPhasePins[0] = ph0;
    stepperPhasePins[1] = ph1;
    stepperPhasePins[2] = ph2;
    stepperPhasePins[3] = ph3;
    
    // Initialize read/write pins
    readPin = read;
    writePin = write;
    writeEnablePin = writeEnable;
    driveSelPin = driveSel;
    
    // Initialize state
    currentTrack = 0;
    physicalTrack = 0;  // Physical track (0-69), logical track = physicalTrack >> 1
    
    // Initialize write buffer state
    
    // Initialize PIO/DMA state
    pio = nullptr;
    pioSm = 0;
    pioOffset = 0;
    dmaChannel = -1;
    dmaChannelB = -1;
    pioDmaActive = false;
    streamFillBit = 0;
    streamHalfStartBit[0] = 0;
    streamHalfStartBit[1] = 0;
    streamStopBit = 0;
    streamStopTime = get_absolute_time();
    trackBits = gcrTrackCache;
    trackBitCount = APPLE_II_GCR_BYTES_PER_TRACK * 8;
    bitPeriodNs = APPLE_II_BIT_PERIOD_US * 1000;
    maxPhysicalTrack = APPLE_II_TRACKS * 4 - 1;
    memset(&woz, 0, sizeof(woz));
    lastWozResult = WOZ_OK;
    wozSlotMode = false;
    wozSlotBytes = 0;
    wozSlotCount = 0;
    wozSlotClock = 0;
    for (int i = 0; i < WOZ_MAX_SLOTS; i++) {
        wozSlots[i].key = -1;
        wozSlots[i].bits = 0;
        wozSlots[i].lastUse = 0;
    }
    fakeBitsEnabled = false;
    mcWindow = 0;
    fakePos = 0;
    {
        // Fixed random ring for MC3470 fake bits / empty tracks: ~30% ones
        uint32_t r = 0x9E3779B9u;
        for (int i = 0; i < 32; i++) {
            uint8_t b = 0;
            for (int k = 0; k < 8; k++) {
                r ^= r << 13; r ^= r >> 17; r ^= r << 5;
                b = (uint8_t)((b << 1) | (((r & 0xFF) < 77) ? 1 : 0));
            }
            fakeRing[i] = b;
        }
    }
    lastWriteTime = get_absolute_time();
    lastStepTimeUs = 0;
    lastFlushOk = true;
    unsavedLost = false;
    
    // Initialize GCR cache state
    gcrTrackCacheTrack = -1;  // Cache invalid initially
    gcrTrackCacheBits = 0;
    memset(trackVolume, 0xFE, sizeof(trackVolume));   // DOS default volume 254
    gcrTrackCacheDirty = false;  // Cache is clean initially
    
    // Initialize SD card and file management
    sdCardManager = nullptr;
    memset(currentFileName, 0, sizeof(currentFileName));
    memset(&imageFile, 0, sizeof(imageFile));
    pendingClearAll();
    wozCrcCleared = false;
    lastPendingRetry = get_absolute_time();
    ejectRequested = false;
    currentFileType = DISK_FILE_TYPE_DSK;  // Default to .dsk format
    
    // Clear disk image
    clearDiskImage();
    
}

    // Destructor
FloppyEmulator::~FloppyEmulator() {
    // Stop PIO/DMA if active
    stopPIO_DMA();
}

// Initialize hardware
void FloppyEmulator::init() {
    // Set static instance pointer for IRQ handlers
    g_floppyEmulatorInstance = this;

    // WRPROT output to the controller: "protected" until an image is loaded
    gpio_init(GPIO_WR_PROTECT);
    wprotOutput = false;
    updateWriteProtectOutput();
    gpio_set_dir(GPIO_WR_PROTECT, GPIO_OUT);
    
    // Configure stepper phase pins as INPUT (we read from controller)
    // Phases are active HIGH. Every input of the board has a 2.2k pull-up to
    // 3.3V (RN1/RN2) behind a series diode, so the internal pull must be a
    // pull-up too: the RP2350 reset default, kept by gpio_init(), is a
    // pull-down working against it. With the cable open all four phases read
    // HIGH, which rotorPosition() ignores.
    for (int i = 0; i < 4; i++) {
        gpio_init(stepperPhasePins[i]);
        gpio_set_dir(stepperPhasePins[i], GPIO_IN);
        gpio_pull_up(stepperPhasePins[i]);
    }
    
    // Configure read pin as OUTPUT (we send data to controller)
    gpio_init(readPin);
    gpio_put(readPin, READ_OUTPUT_INVERTED ? 1 : 0);    // idle level
    gpio_set_dir(readPin, GPIO_OUT);
    
    // Configure write pin as INPUT (we receive data from controller)
    gpio_init(writePin);
    gpio_set_dir(writePin, GPIO_IN);
    gpio_pull_up(writePin);     // same direction as the board's 2.2k (RN1)
    
    // Configure write enable pin as INPUT (from controller)
    // WRITE_ENABLE# / DRIVE_SEL#: active low; the pull-ups make an open line
    // read as "not writing" / "not selected"
    gpio_init(writeEnablePin);
    gpio_set_dir(writeEnablePin, GPIO_IN);
    gpio_pull_up(writeEnablePin);
    
    // Configure drive select pin as INPUT (from controller)
    gpio_init(driveSelPin);
    gpio_set_dir(driveSelPin, GPIO_IN);
    gpio_pull_up(driveSelPin);
    
    // Initialize position tracking
    currentTrack = 0;
    physicalTrack = 0;  // Physical track (0-69), logical track = physicalTrack >> 1
    
    // No image yet: the track cache holds an empty track
    updateGCRTrackCache();
    
    // Initialize PIO/DMA for continuous bit output (jitter-free hardware timing)
    initPIO_DMA();
    
    // Initialize PWM timer for write bit capture
    initWritePWMTimer();
    
    // The read stream is started by process() once an image is loaded and
    // the drive is selected. (It used to start here and played an all-zero
    // track 0 while the SD card was still being mounted: an Apple II booting
    // at power-on read that as valid sectors instead of waiting for the disk.)
}

// Head stepper. The four phase magnets pull the rotor to one of eight
// positions per cycle (one position = a quarter track):
//   one phase k energised              -> position 2k
//   two neighbouring phases k and k+1  -> the position between them, 2k+1
// Any other combination (none, opposite phases, three or four) gives no
// defined pull and is ignored. Returns -1 for those.
static inline int rotorPosition(uint32_t phases) {
    phases &= 0x0F;
    for (int k = 0; k < 4; k++) {
        if (phases == (1u << k)) return 2 * k;
        if (phases == ((1u << k) | (1u << ((k + 1) & 3)))) return (2 * k + 1) & 7;
    }
    return -1;
}

// Quarter tracks the head moves when the rotor is pulled from position `from`
// to `to` (each 0..7): the shorter way round, -3..+3. Half a cycle away the
// direction is undefined, so nothing moves.
static inline int rotorMove(int from, int to) {
    const int ahead = (to - from) & 7;
    if (ahead == 0 || ahead == 4) return 0;
    return ahead < 4 ? ahead : ahead - 8;
}

// Follow the phase lines (called every 500 us from the timer interrupt)
void FloppyEmulator::detectStepperPhaseChange() {
    // Read all 4 phase pins at once
    volatile uint32_t gpio_all = gpio_get_all();
    // A Disk II drive powers its stepper only while it is enabled (DRIVE_SEL#
    // low). The phase lines are shared by both drives of a controller: without
    // this check the head would follow the seeks meant for the other drive.
    if (gpio_all & (1u << driveSelPin)) {
        return;
    }
    const int target = rotorPosition(gpio_all >> GPIO_PH0);
    if (target < 0) {
        return;
    }
    // The rotor position is the head position modulo one phase cycle (8
    // quarter tracks)
    const int move = rotorMove(physicalTrack & 7, target);
    if (move == 0) {
        return;
    }

    beepStep();      // head-seek sound

    int pt = physicalTrack + move;
    lastStepTimeUs = time_us_32();

    // Mechanical limits: ignore steps beyond track 0 or max track
    // (4 quarter-track positions per track)
    if (pt < 0) {
        pt = 0;
    } else if (pt > maxPhysicalTrack) {
        pt = maxPhysicalTrack;   // 35 tracks, or 40 (TMAP) for WOZ
    }

    physicalTrack = pt;
    currentTrack = pt >> 2;
    // NOTE: the dirty track is NOT saved here any more. This function runs in
    // the 500us repeating-timer IRQ; saving here decoded GCR and wrote the SD
    // card from interrupt context (blocking the IRQ for tens of ms and racing /
    // dead-locking with SD accesses from core0 and core1). process() now saves
    // the cached track before it loads the new one.
}


// Process stepper motor (monitor controller signals)
void FloppyEmulator::processStepperMotor() {
    // Monitor phase changes from controller
    detectStepperPhaseChange();
    
    // Sounds (buzzer) start and end here
    beeperTick();
}

// Get current track
int FloppyEmulator::getCurrentTrack() const {
    return currentTrack;
}


// GCR bytes per sector in track cache (416 = 6656/16)
#define GCR_BYTES_PER_SECTOR_IN_CACHE  416


// Load disk image from external source
bool FloppyEmulator::loadDiskImage(const uint8_t* image, uint32_t size) {

    // The previous image must have been flushed with ejectImage() BEFORE the
    // new file was read into diskImage. Saving here (as before) decoded the old
    // track cache into the NEW image buffer and then wrote that mixed track
    // into the OLD file, corrupting both images.
    imageLoaded = false;
    gcrTrackCacheDirty = false;
    pendingClearAll();
    wozCrcCleared = false;

    // Default track source / geometry (DSK, NIC)
    trackBits = gcrTrackCache;
    trackBitCount = APPLE_II_GCR_BYTES_PER_TRACK * 8;
    bitPeriodNs = APPLE_II_BIT_PERIOD_US * 1000;
    maxPhysicalTrack = APPLE_II_TRACKS * 4 - 1;
    lastWozResult = WOZ_OK;
    wozSlotMode = false;
    wozSlotCount = 0;
    fakeBitsEnabled = weakBitsRequested.load(std::memory_order_relaxed);
    mcWindow = 0;

    if (currentFileType == DISK_FILE_TYPE_WOZ) {
        if (size > APPLE_II_MAX_DISK_SIZE) {
            lastWozResult = WOZ_ERR_TOO_BIG;
            imageFile.valid = false;
            return false;
        }
        if (image != diskImage) {
            memcpy(diskImage, image, size);
        }
        // Whole file in RAM (tracks played in place), or - for a WOZ2 larger
        // than the buffer - only its header: tracks then come from the SD
        // card into a slot cache (diskImage split into slots)
        uint32_t fileSize = (imageFile.valid && imageFile.size > size) ? imageFile.size : size;
        wozSlotMode = fileSize > size;
        lastWozResult = wozParse(diskImage, size, fileSize, &woz);
        if (lastWozResult != WOZ_OK || !wozFinishLoad(size, fileSize)) {
            wozSlotMode = false;
            imageFile.valid = false;
            return false;   // drive stays empty
        }
        bitPeriodNs = (uint32_t)woz.bitTiming * 125u;
        maxPhysicalTrack = WOZ_QUARTER_TRACKS - 1;
    } else {
        // A DSK is 35 tracks x 16 sectors x 256 bytes, a NIC 35 x 16 x 512.
        // A shorter file is not such an image (truncated or misnamed): playing
        // the missing part as zeros would only produce I/O errors on the Apple II.
        uint32_t need = (currentFileType == DISK_FILE_TYPE_NIC) ? APPLE_II_NIC_DISK_SIZE : APPLE_II_DISK_SIZE;
        if (size < need) {
            imageFile.valid = false;
            return false;   // drive stays empty
        }
        uint32_t copySize = size < APPLE_II_MAX_DISK_SIZE ? size : APPLE_II_MAX_DISK_SIZE;
        if (image != diskImage) {
            memcpy(diskImage, image, copySize);
        }
        // Clear the rest of the buffer: a short image (or a DSK loaded after a
        // NIC/WOZ) must never show data of the previously loaded image
        if (copySize < APPLE_II_MAX_DISK_SIZE) {
            memset(diskImage + copySize, 0, APPLE_II_MAX_DISK_SIZE - copySize);
        }
    }
    
    // The head stays where it is: changing the disk does not move the head of a
    // real drive, and the stepper is tracked all the time (see main.cpp).
    // (Forcing track 0 here left the emulator on the wrong track - and out of
    // step with the stepper phases - whenever a disk was changed while the
    // head was not on track 0, e.g. "insert disk 2" in a multi-disk program.)

    // Invalidate GCR track cache - force regeneration with new disk image data
    // Cache will be regenerated automatically in process() when track is accessed
    gcrTrackCacheTrack = -1;
    memset(trackVolume, 0xFE, sizeof(trackVolume));   // DOS default volume 254
    gcrTrackCacheDirty = false;  // New disk loaded - cache is clean
    // A fresh load already matches its backing file. Only removal starts recovery.
    recoveryOffset = imageFile.size;
    dbgLog('L', (int)currentFileType, (int32_t)size, (int32_t)streamTrackBits());
    imageLoaded = true;          // Now we can start monitoring Apple II signals
    
    // NO DBG_PRINTF() here - this may be called from core1 (UI handler) and DBG_PRINTF() can block!
    return true;
}

// Clear disk image
void FloppyEmulator::clearDiskImage() {
    for (uint32_t i = 0; i < APPLE_II_DISK_SIZE; i++) {
        diskImage[i] = 0;
    }
    imageLoaded = false;
}

// Get pointer to disk image
uint8_t* FloppyEmulator::getDiskImage() {
    return diskImage;
}

// Get disk image size
uint32_t FloppyEmulator::getDiskImageSize() const {
    // Return maximum size to support both DSK and NIC formats
    return APPLE_II_MAX_DISK_SIZE;
}

// Check if this drive is selected by controller
bool FloppyEmulator::isDriveSelected() const {
    return gpio_get(driveSelPin) == 0;     // DRIVE_SEL#, active low
}

// Check if write is enabled
bool FloppyEmulator::isWriteEnabled() const {
    return gpio_get(writeEnablePin) == 0;  // WRITE_ENABLE#, active low
}

// Set SD card manager for saving tracks
void FloppyEmulator::setSDCardManager(SDCardManager* sdCard) {
    sdCardManager = sdCard;
}

// Ask core0 to flush and unload the current image (see process()).
// Called from core1 (UI) before a new image is read into diskImage or the
// card is formatted.
bool FloppyEmulator::ejectImage(uint32_t timeoutMs) {
    // (ejectRequested may still be set from a call that timed out: core0 can
    //  be in the middle of flushing the old image even though imageLoaded is
    //  already false - wait for it instead of returning "done")
    if (!imageLoaded && !ejectRequested) {
        return true;
    }
    {
        SDLockGuard guard;
        if (hasPendingMediaRecovery()) return false;
        ejectRequested = true;
    }
    absolute_time_t deadline = make_timeout_time_ms(timeoutMs);
    while (ejectRequested) {
        if (time_reached(deadline)) {
            // core0 has not finished (long Apple II write or slow SD retries).
            // Leave the request pending -- core0 completes it later -- and tell
            // the caller NOT to touch diskImage or the card yet: core0 may still
            // be writing the old image from diskImage.
            return false;
        }
        sleep_ms(1);
    }
    return true;
}

void FloppyEmulator::setImagePath(const char* path) {
    snprintf(imagePath, sizeof(imagePath), "%s", path ? path : "");
}

bool FloppyEmulator::hasPendingMediaRecovery() {
    SDLockGuard guard;
    return imageLoaded && imageFile.size &&
           (!imageFile.valid || recoveryOffset < imageFile.size);
}

void FloppyEmulator::sdMediaRemoved() {
    // The handle describes the old volume. Keep its size for recovery.
    if (imageLoaded && imageFile.valid) {
        imageFile.valid = false;
        recoveryOffset = 0;
    }
}

bool FloppyEmulator::recoverImageOnSD() {
    if (!sdCardManager || !imageFile.size || !imagePath[0]) return false;
    FAT32* fat = sdCardManager->getFAT32();
    if (!fat) return false;

    if (!imageFile.valid) {
        char previousPath[256];
        fat->getCurrentDirectory(previousPath, sizeof(previousPath));
        fat->changeDirectory("/");
        const char* name = fat->enterPathDirectory(imagePath);
        if (!name && !wozSlotMode) {
            fat->changeDirectory("/");
            name = configBaseName(imagePath);
        }
        FAT32_File reopened;
        bool created = false;       // the file did not exist on this card
        bool ok = name && fat->openFile(name, 0, &reopened);
        if ((!ok || reopened.size != imageFile.size) && name && !wozSlotMode) {
            // The FAT writer creates short names. Use a free recovery name
            // if the original name cannot be created, or a name is taken.
            const char* ext = currentFileType == DISK_FILE_TYPE_WOZ ? "WOZ" :
                              currentFileType == DISK_FILE_TYPE_NIC ? "NIC" : "DSK";
            char fallback[13];
            if (fat->createImageFile(name, diskImage, imageFile.size)) {
                ok = fat->openFile(name, 0, &reopened);
                created = ok;
            } else {
                for (int i = 1; i <= 99; ++i) {
                    snprintf(fallback, sizeof(fallback), "RECOV%02d.%s", i, ext);
                    FAT32_File existing;
                    if (fat->openFile(fallback, 0, &existing)) continue;
                    if (fat->createImageFile(fallback, diskImage, imageFile.size)) {
                        ok = fat->openFile(fallback, 0, &reopened);
                        created = ok;
                        if (ok) {
                            char dir[256];
                            char path[384];
                            fat->getCurrentDirectory(dir, sizeof(dir));
                            snprintf(path, sizeof(path), "%s/%s", strcmp(dir, "/") ? dir : "", fallback);
                            setImagePath(path);
                        }
                    }
                    break;
                }
            }
        }
        fat->changeDirectory("/");
        if (strcmp(previousPath, "/")) {
            // enterPathDirectory treats the last component as a file name.
            char restore[258];
            snprintf(restore, sizeof(restore), "%s/.", previousPath);
            fat->enterPathDirectory(restore);
        }
        if (!ok || reopened.size != imageFile.size) return false;
        imageFile = reopened;
        // Write protection follows the image: a file created on this card
        // gets the read-only attribute the image had; an existing file
        // keeps its own attribute.
        if (created) {
            if (attrWriteProtected.load(std::memory_order_relaxed) &&
                fat->setReadOnly(imageFile, configBaseName(imagePath), true)) {
                imageFile.readOnly = true;
            }
        } else {
            attrWriteProtected.store(imageFile.readOnly, std::memory_order_relaxed);
        }
        recoveryOffset = wozSlotMode ? imageFile.size : 0;
        wozCrcCleared = false;
        if (!wozSlotMode && currentFileType == DISK_FILE_TYPE_WOZ) {
            memset(diskImage + 8, 0, 4);
            woz.crc = 0;
        }
    }

    if (recoveryOffset < imageFile.size) {
        uint32_t bytes = imageFile.size - recoveryOffset;
        if (bytes > 512) bytes = 512;
        if (!sdCardManager->writeFile(&imageFile, recoveryOffset, diskImage + recoveryOffset, bytes)) return false;
        recoveryOffset += bytes;
        if (recoveryOffset == imageFile.size && strlen(imagePath) < sizeof(g_config.lastImage)) {
            configSetLastImage(sdCardManager, imagePath);
        }
    }
    return true;
}

// Write one track of diskImage into the image file through the handle taken
// at load time. Only data inside the existing file is overwritten (no FAT or
// directory change), and a stale handle (card remounted) is refused.
bool FloppyEmulator::writeTrackToSD(int track) {
    if (track < 0 || track >= PENDING_SLOTS) {
        return false;
    }
    if (!sdBusTryLock()) return false;
    SDLockGuard guard;
    sdBusUnlock();
    if (!sdCardManager || !imageFile.valid) return false;

    uint32_t offset;
    uint32_t bytes;
    const uint8_t* src = nullptr;
    if (currentFileType == DISK_FILE_TYPE_WOZ) {
        // Only the track's own bitstream bytes in the file (validated by
        // wozParse): WOZ2 blocks of the track, WOZ1 TRK record bitstream.
        // Lengths/bit counts are never changed.
        if (woz.trk[track].bitCount == 0) {
            pendingClear(track);
            return false;
        }
        offset = woz.trk[track].offset;
        bytes = (woz.trk[track].bitCount + 7) / 8;
        if (!wozSlotMode) {
            if (offset + bytes > APPLE_II_MAX_DISK_SIZE) {
                pendingClear(track);
                return false;
            }
            src = &diskImage[offset];
        } else {
            for (int i = 0; i < wozSlotCount; i++) {
                if (wozSlots[i].key == track) {
                    src = wozSlotPtr(i);
                    break;
                }
            }
            if (!src) {
                pendingClear(track);   // data no longer in RAM (should not happen)
                return false;
            }
        }

        // Before the first track write of this image: header CRC -> 0
        // ("not computed", spec). Otherwise the file would carry a CRC that no
        // longer matches its contents (also after a power loss mid-write).
        if (!wozCrcCleared) {
            if (woz.crc != 0) {
                static const uint8_t zeroCrc[4] = {0, 0, 0, 0};
                if (!sdCardManager->writeFile(&imageFile, 8, zeroCrc, 4)) {
                    pendingSet(track);
                    return false;
                }
                if (!wozSlotMode) {
                    memset(&diskImage[8], 0, 4);   // keep the RAM copy identical
                }
                woz.crc = 0;
            }
            wozCrcCleared = true;
        }
    } else {
        bytes = (currentFileType == DISK_FILE_TYPE_NIC) ? APPLE_II_NIC_BYTES_PER_TRACK
                                                        : APPLE_II_BYTES_PER_TRACK;
        offset = (uint32_t)track * bytes;
        if (offset + bytes > APPLE_II_MAX_DISK_SIZE) {
            pendingClear(track);
            return false;
        }
        src = &diskImage[offset];
    }

    if (offset + bytes > imageFile.size) {
        pendingClear(track);   // not inside this file
        return false;
    }
    if (sdCardManager->writeFile(&imageFile, offset, src, bytes)) {
        pendingClear(track);
        return true;
    }
    pendingSet(track);
    return false;
}

// May the Apple II write to the track under the head?
// WOZ: not if the image is write protected (INFO) or the quarter track has no
// data in the file (TMAP 0xFF): a WOZ file is never grown, so there is nowhere
// to store it. Writes to such tracks are dropped; the WPROT line only reports
// write protection of the whole image (see updateWriteProtectOutput).
bool FloppyEmulator::isImageWriteProtected() const {
    return attrWriteProtected.load(std::memory_order_relaxed) ||
           (currentFileType == DISK_FILE_TYPE_WOZ && woz.writeProtected);
}

bool FloppyEmulator::toggleImageWriteProtection() {
    SDLockGuard guard;
    if (!imageLoaded || !sdCardManager || !imagePath[0] || hasPendingMediaRecovery()) return false;
    if (currentFileType == DISK_FILE_TYPE_WOZ && woz.writeProtected) return false;
    FAT32* fat = sdCardManager->getFAT32();
    if (!fat) return false;

    // Write protection belongs to the image file: it is the file's FAT
    // read-only attribute (one byte in its directory entry). The file name is
    // never changed and has no meaning for write protection.
    const bool protect = !attrWriteProtected.load(std::memory_order_relaxed);
    if (!fat->setReadOnly(imageFile, configBaseName(imagePath), protect)) return false;
    imageFile.readOnly = protect;
    attrWriteProtected.store(protect, std::memory_order_relaxed);
    return true;
}

bool FloppyEmulator::trackWritable() {
    if (isImageWriteProtected()) return false;
    if (currentFileType == DISK_FILE_TYPE_WOZ) {
        int key = desiredTrackKey();
        return key != TRACK_KEY_EMPTY &&
               key >= 0 && key < WOZ_QUARTER_TRACKS && woz.trk[key].bitCount != 0;
    }
    return true;
}

// Retry up to maxTracks pending track writes; stops at the first failure
// (the card is probably busy or gone, no point hammering it)
bool FloppyEmulator::retryPendingTracks(int maxTracks) {
    for (int t = 0; t < PENDING_SLOTS && maxTracks > 0; t++) {
        if (pendingTest(t)) {
            maxTracks--;
            if (!writeTrackToSD(t)) {
                wstats.sdFail = wstats.sdFail + 1;
                dbgLog('F', t, 0);
                return false;
            }
            wstats.sdOk = wstats.sdOk + 1;
            dbgLog('F', t, 1);
        }
    }
    return true;
}

// Set current disk image filename
void FloppyEmulator::setCurrentFileName(const char* filename) {
    if (filename) {
        strncpy(currentFileName, filename, sizeof(currentFileName) - 1);
        currentFileName[sizeof(currentFileName) - 1] = 0;
        
        // Determine file type based on extension (.dsk or .nic, case-insensitive)
        const char* ext = strrchr(filename, '.');
        if (ext != nullptr && strlen(ext) >= 4) {
            // Case-insensitive comparison
            char ext_lower[5] = {0};
            for (int i = 0; i < 4 && ext[i+1] != '\0'; i++) {
                ext_lower[i] = tolower(ext[i+1]);
            }
            
            if (strcmp(ext_lower, "dsk") == 0) {
                currentFileType = DISK_FILE_TYPE_DSK;
            } else if (strcmp(ext_lower, "nic") == 0) {
                currentFileType = DISK_FILE_TYPE_NIC;
            } else if (strcmp(ext_lower, "woz") == 0) {
                currentFileType = DISK_FILE_TYPE_WOZ;
            } else {
                // Unknown extension, default to .dsk
                currentFileType = DISK_FILE_TYPE_DSK;
            }
        } else {
            // No extension or invalid extension, default to .dsk
            currentFileType = DISK_FILE_TYPE_DSK;
        }
    } else {
        currentFileName[0] = 0;
        currentFileType = DISK_FILE_TYPE_DSK;  // Reset to default
    }
}

const char* FloppyEmulator::getCurrentFileName() const {
    return currentFileName;
}

DiskFileType FloppyEmulator::getCurrentFileType() const {
    return currentFileType;
}


// ===================================================================== WOZ P5
// Track keys: 0..159 = bit track (TRK index), FLUX_KEY_BASE + n = FLUX track
// (TRK n holds flux data), TRACK_KEY_EMPTY = no data.

// Read bytes of the image file: RAM mode -> diskImage, slot mode -> SD card
bool FloppyEmulator::readImageBytes(uint32_t offset, uint8_t* buf, uint32_t n) {
    if (!wozSlotMode) {
        if (offset + n > APPLE_II_MAX_DISK_SIZE) return false;
        memcpy(buf, &diskImage[offset], n);
        return true;
    }
    if (!sdBusTryLock()) return false;
    SDLockGuard guard;
    sdBusUnlock();
    if (!sdCardManager || !imageFile.valid) return false;
    return sdCardManager->readFileAt(&imageFile, offset, buf, n);
}

// Convert a FLUX track (bytes = 125 ns tick counts since the previous flux
// transition, 255 = "keep adding") into a bitstream at the image's bit timing:
// an interval of n bit cells = (n-1) zero bits followed by a 1 bit.
// Returns the bit count, 0 if it does not fit `capBits` or on a read error.
uint32_t FloppyEmulator::fluxToBits(uint32_t offset, uint32_t bytes, uint8_t* out, uint32_t capBits) {
    static uint8_t chunk[512];
    memset(out, 0, (capBits + 7) / 8);
    const uint32_t timing = woz.bitTiming ? woz.bitTiming : 32;
    uint32_t bit = 0;
    uint32_t ticks = 0;
    for (uint32_t done = 0; done < bytes; ) {
        uint32_t n = bytes - done;
        if (n > sizeof(chunk)) n = sizeof(chunk);
        if (!readImageBytes(offset + done, chunk, n)) return 0;
        for (uint32_t i = 0; i < n; i++) {
            uint8_t v = chunk[i];
            ticks += v;
            if (v == 255) continue;
            uint32_t cells = (ticks + timing / 2) / timing;   // round to whole bit cells
            if (cells == 0) cells = 1;
            bit += cells - 1;                                  // zero bits are already 0
            if (bit >= capBits) return 0;
            out[bit >> 3] |= (uint8_t)(0x80u >> (bit & 7));
            bit++;
            ticks = 0;
        }
        done += n;
    }
    return (bit >= 8) ? bit : 0;
}

// Slot cache (large WOZ): find the slot holding `key`, or free one.
// Never evicts the playing track or a track whose write is still pending;
// if every candidate is pending, the oldest one is written to SD first.
int FloppyEmulator::wozSlotFor(int key, bool* loaded) {
    *loaded = false;
    for (int i = 0; i < wozSlotCount; i++) {
        if (wozSlots[i].key == key) {
            *loaded = true;
            return i;
        }
    }
    for (int i = 0; i < wozSlotCount; i++) {
        if (wozSlots[i].key < 0) return i;
    }
    int best = -1;
    int bestPending = -1;
    for (int i = 0; i < wozSlotCount; i++) {
        int k = wozSlots[i].key;
        if (k == gcrTrackCacheTrack) continue;                 // playing
        bool pending = (k >= 0 && k < WOZ_QUARTER_TRACKS) && pendingTest(k);
        if (!pending) {
            if (best < 0 || wozSlots[i].lastUse < wozSlots[best].lastUse) best = i;
        } else {
            if (bestPending < 0 || wozSlots[i].lastUse < wozSlots[bestPending].lastUse) bestPending = i;
        }
    }
    if (best < 0 && bestPending >= 0) {
        // All other slots hold unwritten data: flush the oldest synchronously
        if (!writeTrackToSD(wozSlots[bestPending].key)) return -1;
        best = bestPending;
    }
    if (best >= 0) wozSlots[best].key = -1;
    return best;
}

// Make `key` the playing track (trackBits/trackBitCount). false -> play empty.
bool FloppyEmulator::wozLoadTrack(int key) {
    bool flux = key >= FLUX_KEY_BASE;
    int idx = flux ? key - FLUX_KEY_BASE : key;
    if (idx < 0 || idx >= WOZ_QUARTER_TRACKS || woz.trk[idx].bitCount == 0) return false;
    const WozTrack& t = woz.trk[idx];

    if (!wozSlotMode) {
        if (!flux) {
            trackBits = &diskImage[t.offset];   // in place (validated by wozParse)
            trackBitCount = t.bitCount;
            return true;
        }
        uint32_t bits = fluxToBits(t.offset, t.bitCount, gcrTrackCache, APPLE_II_GCR_BYTES_PER_TRACK * 8);
        if (bits == 0) return false;
        trackBits = gcrTrackCache;
        trackBitCount = bits;
        return true;
    }

    bool loaded = false;
    int s = wozSlotFor(key, &loaded);
    if (s < 0) return false;
    uint8_t* p = wozSlotPtr(s);
    if (!loaded) {
        uint32_t bits = 0;
        if (flux) {
            bits = fluxToBits(t.offset, t.bitCount, p, wozSlotBytes * 8);
        } else if ((t.bitCount + 7) / 8 <= wozSlotBytes &&
                   readImageBytes(t.offset, p, (t.bitCount + 7) / 8)) {
            bits = t.bitCount;
        }
        if (bits == 0) {
            wozSlots[s].key = -1;
            return false;
        }
        wozSlots[s].key = (int16_t)key;
        wozSlots[s].bits = bits;
    }
    wozSlots[s].lastUse = ++wozSlotClock;
    trackBits = p;
    trackBitCount = wozSlots[s].bits;
    return true;
}

// Second stage of loading a WOZ: FLUX map, slot cache, streamed CRC.
// `avail` bytes of the file are in diskImage; imageFile.size is the real size.
bool FloppyEmulator::wozFinishLoad(uint32_t avail, uint32_t fileSize) {
    // FLUX map (WOZ 2.1)
    if (woz.hasFlux) {
        uint32_t off = (uint32_t)woz.fluxBlock * 512u;
        static uint8_t fluxChunk[8 + WOZ_QUARTER_TRACKS];
        if (off + sizeof(fluxChunk) > fileSize) {
            lastWozResult = WOZ_ERR_FLUX;
            return false;
        }
        if (!wozSlotMode) {
            memcpy(fluxChunk, &diskImage[off], sizeof(fluxChunk));
        } else if (!readImageBytes(off, fluxChunk, sizeof(fluxChunk))) {
            lastWozResult = WOZ_ERR_READ;
            return false;
        }
        lastWozResult = wozParseFluxMap(fluxChunk, sizeof(fluxChunk), &woz);
        if (lastWozResult != WOZ_OK) return false;
    }

    if (!wozSlotMode) return true;

    // Slot size: largest bit track (rounded to 512), at least room for a
    // converted flux track (~51200 bits)
    bool isFlux[WOZ_QUARTER_TRACKS] = {false};
    if (woz.fluxMapValid) {
        for (int q = 0; q < WOZ_QUARTER_TRACKS; q++) {
            if (woz.fluxMap[q] != WOZ_NO_TRACK) isFlux[woz.fluxMap[q]] = true;
        }
    }
    uint32_t slotBytes = 7168;
    for (int i = 0; i < WOZ_QUARTER_TRACKS; i++) {
        if (woz.trk[i].bitCount == 0 || isFlux[i]) continue;
        uint32_t need = ((woz.trk[i].bitCount + 7) / 8 + 511) & ~511u;
        if (need > slotBytes) slotBytes = need;
    }
    int count = (int)(APPLE_II_MAX_DISK_SIZE / slotBytes);
    if (count > WOZ_MAX_SLOTS) count = WOZ_MAX_SLOTS;
    if (count < 2) {
        lastWozResult = WOZ_ERR_TOO_BIG;
        return false;
    }
    wozSlotBytes = slotBytes;
    wozSlotCount = count;
    for (int i = 0; i < WOZ_MAX_SLOTS; i++) {
        wozSlots[i].key = -1;
        wozSlots[i].bits = 0;
        wozSlots[i].lastUse = 0;
    }
    wozSlotClock = 0;

    // Header CRC over the whole file, streamed through the (still unused) buffer
    if (woz.crc != 0) {
        uint32_t crc = wozCrc32Begin();
        const uint32_t CHUNK = 16384;
        for (uint32_t off = 12; off < fileSize; ) {
            uint32_t n = fileSize - off;
            if (n > CHUNK) n = CHUNK;
            if (!readImageBytes(off, diskImage, n)) {
                lastWozResult = WOZ_ERR_READ;
                return false;
            }
            crc = wozCrc32Update(crc, diskImage, n);
            off += n;
        }
        if (wozCrc32End(crc) != woz.crc) {
            lastWozResult = WOZ_ERR_CRC;
            return false;
        }
    }
    (void)avail;
    return true;
}

// Cache key for the current head position: the logical track for DSK/NIC,
// the TMAP entry of the quarter track for WOZ (adjacent quarter tracks that
// map to the same data keep playing without a switch, as the spec requires).
int FloppyEmulator::desiredTrackKey() {
    if (currentFileType == DISK_FILE_TYPE_WOZ) {
        int q = physicalTrack;
        if (q < 0) q = 0;
        if (q >= WOZ_QUARTER_TRACKS) q = WOZ_QUARTER_TRACKS - 1;
        if (woz.fluxMapValid && woz.fluxMap[q] != WOZ_NO_TRACK) {
            return FLUX_KEY_BASE + woz.fluxMap[q];   // FLUX data wins over TMAP (spec)
        }
        uint8_t v = woz.tmap[q];
        return (v == WOZ_NO_TRACK) ? TRACK_KEY_EMPTY : (int)v;
    }
    return currentTrack;
}

// Empty WOZ track (TMAP 0xFF): random bits, about 30% ones, like the noise
// an MC3470 produces on an unformatted area. Uses gcrTrackCache (6400 bytes).
void FloppyEmulator::fillEmptyTrack() {
    static uint32_t rng = 0x2545F491u;
    for (uint32_t i = 0; i < WOZ_EMPTY_TRACK_BITS / 8; i++) {
        uint8_t b = 0;
        for (int k = 0; k < 8; k++) {
            rng ^= rng << 13;
            rng ^= rng >> 17;
            rng ^= rng << 5;
            b = (uint8_t)((b << 1) | (((rng & 0xFF) < 77) ? 1 : 0));
        }
        gcrTrackCache[i] = b;
    }
}

// Update GCR track cache for current track (Apple II NIC format)
void FloppyEmulator::updateGCRTrackCache() {
    // WOZ: no encoding, play the track bitstream in place (TMAP quarter track)
    if (currentFileType == DISK_FILE_TYPE_WOZ) {
        int key = desiredTrackKey();
        if (key == TRACK_KEY_EMPTY || !wozLoadTrack(key)) {
            fillEmptyTrack();
            trackBits = gcrTrackCache;
            trackBitCount = WOZ_EMPTY_TRACK_BITS;
            gcrTrackCacheBits = WOZ_EMPTY_TRACK_BITS;
        }
        gcrTrackCacheTrack = key;
        gcrTrackCacheDirty = false;
        return;
    }
    // DSK / NIC: the streamer plays the generated cache
    trackBits = gcrTrackCache;
    trackBitCount = APPLE_II_GCR_BYTES_PER_TRACK * 8;
    // ONE snapshot of the track for the whole function. currentTrack is changed
    // by the stepper timer IRQ; reading it again while encoding (as before)
    // could build a cache with the data of one track, address fields of
    // another and the label of the second: the Apple II then read (and wrote
    // back!) the wrong track without any error.
    const int track = currentTrack;
    // For NIC files, copy GCR data directly from diskImage buffer (already in GCR format)
    if (currentFileType == DISK_FILE_TYPE_NIC) {
        // NIC format: each track is 8192 bytes in file (16 sectors * 512 bytes)
        // But we only use first 416 bytes from each sector = 6656 bytes per track
        uint32_t trackOffsetInFile = track * APPLE_II_NIC_BYTES_PER_TRACK;  // Offset in NIC file (8192 bytes per track)
        
        // Copy 416 bytes from each of the 16 sectors directly to GCR cache
        for (int sector = 0; sector < 16; sector++) {
            uint32_t sectorOffsetInFile = trackOffsetInFile + (sector * 512);  // 512 bytes per sector in file
            uint32_t sectorOffsetInCache = sector * 416;  // 416 bytes per sector in cache
            
            // Copy first 416 bytes from file sector to GCR cache
            if (sectorOffsetInFile + 416 <= APPLE_II_NIC_DISK_SIZE && 
                sectorOffsetInCache + 416 <= APPLE_II_GCR_BYTES_PER_TRACK) {
                memcpy(gcrTrackCache + sectorOffsetInCache, diskImage + sectorOffsetInFile, 416);
            }
        }
        
        gcrTrackCacheTrack = track;
        gcrTrackCacheBits = APPLE_II_GCR_BYTES_PER_TRACK * 8;
        gcrTrackCacheDirty = false;  // Cache is clean after loading from buffer
        HOSTTEST_CYCLES(APPLE_II_GCR_BYTES_PER_TRACK * 2);   // 16 x 416-byte copies
        
        // Byte-swap each 4-byte group (DMA feeds little-endian words, PIO outputs MSB first)
        // AND invert for the inverted read output (hardware inverter).
    // (cache stays in natural bit order; the streamer inverts while filling the ring)
        return;
    }
    
    // DSK: the file holds only the 256 data bytes of every sector (in DOS
    // order); build the 16 sectors of the track from them.
    // Volume: 254 unless a format wrote another one on this track in this
    // session (a .dsk file cannot store it; see trackVolume)
    const uint8_t volume = (track >= 0 && track < APPLE_II_TRACKS) ? trackVolume[track] : 0xFE;
    static const uint8_t noData[APPLE_II_BYTES_PER_SECTOR] = {0};
    for (int sector = 0; sector < APPLE_II_SECTORS_PER_TRACK; sector++) {
        const uint32_t offset = (uint32_t)track * APPLE_II_BYTES_PER_TRACK +
                                (uint32_t)diskcodec::dosOrderSector((uint8_t)sector) * APPLE_II_BYTES_PER_SECTOR;
        const uint8_t* data = (track >= 0 && offset + APPLE_II_BYTES_PER_SECTOR <= APPLE_II_DISK_SIZE)
                                  ? &diskImage[offset] : noData;
        diskcodec::buildSector(&gcrTrackCache[sector * diskcodec::SECTOR_BYTES], volume, (uint8_t)track,
                               (uint8_t)sector, data);
    }
    const uint32_t gcrIndex = APPLE_II_SECTORS_PER_TRACK * diskcodec::SECTOR_BYTES;

    gcrTrackCacheTrack = track;
    gcrTrackCacheBits = gcrIndex * 8;  // Total GCR bits in cache
    gcrTrackCacheDirty = false;  // Cache is clean after loading
    HOSTTEST_CYCLES(APPLE_II_GCR_BYTES_PER_TRACK * 15);  // DSK: 6-and-2 encode of 16 sectors
    
    // Byte-swap each 4-byte group (DMA feeds little-endian words, PIO outputs MSB first)
    // AND invert for the inverted read output (hardware inverter).
    // (cache stays in natural bit order; the streamer inverts while filling the ring)
}


bool FloppyEmulator::getGCRTrackCacheDirty() {
    // "Not yet on the SD card": uncommitted cache or committed-but-unflushed tracks
    return gcrTrackCacheDirty || pendingAny() || hasPendingMediaRecovery();
}
// Commit the (dirty) track cache into diskImage. RAM only: the SD card write
// is deferred (pendingTracks, flushed by process() while the drive is idle),
// because an SD write blocks core0 for ~10 ms and the controller may start
// writing the next track (e.g. during INIT) in that time.
bool FloppyEmulator::saveGCRCacheToDiskImage(bool mayAbort) {
    if (currentFileType == DISK_FILE_TYPE_WOZ) {
        // The written bits are already in the file image (trackBits points
        // into diskImage / a slot): only the SD write is left to do
        int key = gcrTrackCacheTrack;
        if (gcrTrackCacheDirty && key >= 0 && key < WOZ_QUARTER_TRACKS) {
            pendingSet(key);
        }
        gcrTrackCacheDirty = false;
        return true;
    }
    if (gcrTrackCacheTrack < 0 || gcrTrackCacheTrack >= APPLE_II_TRACKS || !gcrTrackCacheDirty) {
        return true;
    }
    int track = gcrTrackCacheTrack;
    bool completed = true;
    commitGoodSectors = 0;
    commitChangedSectors = 0;
    bool changed = commitTrackSectors(track, mayAbort, &completed);
    if (completed) {
        wstats.commits = wstats.commits + 1;
        wstats.lastGood = commitGoodSectors;
        wstats.lastChanged = commitChangedSectors;
    }
    dbgLog('C', track, (int32_t)commitGoodSectors, (int32_t)commitChangedSectors, completed ? 1 : 0);
    if (changed) {
        pendingSet(track);
    }
    if (completed) {
        gcrTrackCacheDirty = false;
    }
    return completed;
}

// NIC: the file holds one 512-byte slot per sector with 416 BYTE-ALIGNED disk
// nibbles. After a write the track cache is a bitstream that is usually no
// longer byte aligned, so it must not be copied back as it is: other tools
// (and other emulators) read the slots byte by byte. Like for DSK, the sector
// is decoded from the bitstream and its data field is re-encoded, byte
// aligned, into the slot. Address field and gaps of the slot stay untouched.
// Returns true if the slot changed.
bool FloppyEmulator::storeNicSector(int track, int sector, uint8_t volume, const uint8_t* data) {
    static uint8_t old[256];
    static uint8_t enc[343];
    uint32_t trackOffset = (uint32_t)track * APPLE_II_NIC_BYTES_PER_TRACK;
    if (trackOffset + APPLE_II_NIC_BYTES_PER_TRACK > APPLE_II_NIC_DISK_SIZE) {
        return false;
    }
    // Slot of this sector: normally slot == sector; verified by its address field
    int slot = -1;
    int dataPos = -1;
    int addrPos = -1;
    bool ownSlotForeign = false;
    for (int k = 0; k < 16 && slot < 0; k++) {
        int s = (sector + k) & 15;
        const uint8_t* p = &diskImage[trackOffset + (uint32_t)s * 512];
        for (int i = 0; i + 11 <= 416; i++) {
            if (p[i] == 0xD5 && p[i + 1] == 0xAA && p[i + 2] == 0x96) {
                uint8_t sec = (uint8_t)(((p[i + 7] << 1) | 1) & p[i + 8]);
                if (sec == sector) {
                    slot = s;
                    addrPos = i;
                    for (int j = i + 11; j + 3 + 343 + 3 <= 416; j++) {
                        if (p[j] == 0xD5 && p[j + 1] == 0xAA && p[j + 2] == 0xAD) {
                            dataPos = j + 3;
                            break;
                        }
                    }
                } else if (k == 0) {
                    ownSlotForeign = true;   // slot holds another sector: never overwrite it
                }
                break;   // one address field per slot
            }
        }
    }
    if (slot >= 0 && dataPos >= 0) {
        uint8_t* p = &diskImage[trackOffset + (uint32_t)slot * 512];
        // The volume number belongs to the format too (a reformat may change
        // it): keep the slot's address field in step with what was written
        bool addrChanged = false;
        uint8_t oldVol = (uint8_t)(((p[addrPos + 3] << 1) | 1) & p[addrPos + 4]);
        if (oldVol != volume) {
            uint8_t* a = &p[addrPos + 3];
            diskcodec::put44(volume, a);
            diskcodec::put44((uint8_t)track, a + 2);
            diskcodec::put44((uint8_t)sector, a + 4);
            diskcodec::put44((uint8_t)(volume ^ track ^ sector), a + 6);
            addrChanged = true;
        }
        if (diskcodec::decode62(&p[dataPos], old) && memcmp(old, data, 256) == 0) {
            return addrChanged;              // data unchanged
        }
        diskcodec::encode62(data, enc);
        memcpy(&p[dataPos], enc, 343);
        p[dataPos + 343] = 0xDE;
        p[dataPos + 344] = 0xAA;
        p[dataPos + 345] = 0xEB;
        return true;
    }
    if (slot < 0) {
        if (ownSlotForeign) {
            return false;                    // no place for this sector in the file
        }
        slot = sector;
    }
    // No usable (byte aligned) fields in the slot: rebuild it in the standard layout
    uint8_t* p = &diskImage[trackOffset + (uint32_t)slot * 512];
    diskcodec::buildSector(p, volume, (uint8_t)track, (uint8_t)sector, data);
    return true;
}

// DSK / NIC: scan the track bitstream like the Disk II logic state sequencer
// (a nibble is complete when its MSB is set; leading zero bits are timing
// bits and are dropped) for address fields D5 AA 96 + data fields D5 AA AD.
// Only sectors whose address checksum, track number and data checksum are all
// correct are written into diskImage (DSK: the 256 data bytes, NIC: the
// re-encoded nibbles, see storeNicSector); anything else keeps its old data
// (the image cannot hold it anyway, and writing garbage is worse).
// Scans one revolution plus ~1.2 sectors so a sector crossing the end of the
// bitstream is found too. Returns true if any sector changed.
bool FloppyEmulator::commitTrackSectors(int track, bool mayAbort, bool* completed) {
    *completed = true;
    const uint32_t bits = streamTrackBits();
    const uint32_t scanBits = bits + 4000;
    static uint8_t nib[343];
    static uint8_t sectorData[256];
    const bool nic = (currentFileType == DISK_FILE_TYPE_NIC);
    uint8_t volume = 0xFE;

    uint8_t reg = 0;
    uint32_t last3 = 0;
    int state = 0;        // 0 = look for D5 AA 96, 1 = address field, 2 = look for D5 AA AD, 3 = data field
    int count = 0;
    int window = 0;
    uint8_t addr[8];
    int sector = -1;
    bool changed = false;
    uint32_t p = 0;
    uint32_t dataStartP = 0;   // scan position right after the data prologue ...
    uint32_t dataStartI = 0;   // ... to resume from if it was not a data field
    for (uint32_t i = 0; i < scanBits; i++) {
        HOSTTEST_CYCLES(25);   // bit-wise nibble scanner
        // The scan takes a few ms. If the controller starts writing meanwhile
        // the capture loop must run at once: give up, the track stays dirty and
        // is committed later (sectors already copied are valid).
        if (mayAbort && (i & 0x3F) == 0 && isWriteEnabled() && isDriveSelected()) {
            *completed = false;
            return changed;
        }
        uint8_t bit = (gcrTrackCache[p >> 3] >> (7 - (p & 7))) & 1;
        if (++p >= bits) {
            p = 0;
        }
        if (reg & 0x80) {
            reg = 0;      // previous nibble was consumed
        }
        reg = (uint8_t)((reg << 1) | bit);
        if (!(reg & 0x80)) {
            continue;
        }
        uint8_t n = reg;  // complete nibble
        last3 = ((last3 << 8) | n) & 0xFFFFFF;

        switch (state) {
            case 0:
                if (last3 == 0xD5AA96) {
                    state = 1;
                    count = 0;
                }
                break;
            case 1:
                addr[count++] = n;
                if (count == 8) {
                    // 4-and-4 decode
                    uint8_t vol = (uint8_t)(((addr[0] << 1) | 1) & addr[1]);
                    uint8_t trk = (uint8_t)(((addr[2] << 1) | 1) & addr[3]);
                    uint8_t sec = (uint8_t)(((addr[4] << 1) | 1) & addr[5]);
                    uint8_t chk = (uint8_t)(((addr[6] << 1) | 1) & addr[7]);
                    if ((uint8_t)(vol ^ trk ^ sec) == chk && trk == track && sec < 16) {
                        sector = sec;
                        volume = vol;
                        if (track >= 0 && track < APPLE_II_TRACKS) {
                            trackVolume[track] = vol;   // DSK: regenerate with this volume
                        }
                        state = 2;
                        window = 0;
                    } else {
                        state = 0;
                    }
                }
                break;
            case 2:
                if (last3 == 0xD5AAAD) {
                    state = 3;
                    count = 0;
                    dataStartP = p;
                    dataStartI = i;
                } else if (last3 == 0xD5AA96) {
                    state = 1;    // next address field: this sector had no data field
                    count = 0;
                } else if (++window > 48) {
                    state = 0;    // data field must follow within a few dozen nibbles
                }
                break;
            case 3:
                nib[count++] = n;
                if (count == 343) {
                    if (diskcodec::decode62(nib, sectorData)) {
                        commitGoodSectors++;
                        if (nic) {
                            if (storeNicSector(track, sector, volume, sectorData)) {
                                changed = true;
                                commitChangedSectors++;
                            }
                        } else {
                            // Address field holds the physical sector; the image is
                            // in logical (DOS) order, as in updateGCRTrackCache()
                            uint32_t off = (uint32_t)track * APPLE_II_BYTES_PER_TRACK +
                                           (uint32_t)diskcodec::dosOrderSector(sector) * APPLE_II_BYTES_PER_SECTOR;
                            if (off + 256 <= APPLE_II_DISK_SIZE && memcmp(&diskImage[off], sectorData, 256) != 0) {
                                memcpy(&diskImage[off], sectorData, 256);
                                changed = true;
                                commitChangedSectors++;
                            }
                        }
                    } else {
                        // Not a data field after all. A whole-track write (INIT)
                        // that is shorter than the track leaves a piece of the
                        // old track behind it, possibly an old address field
                        // and data prologue directly in front of the new
                        // sector 0. The 343 nibbles just consumed then contain
                        // the next address field: look at them again (the
                        // Apple II finds that sector too, RWTS only searches
                        // for address fields). Each prologue is retried once.
                        p = dataStartP;
                        i = dataStartI;
                        reg = 0;
                    }
                    state = 0;
                    last3 = 0;
                }
                break;
        }
    }
    return changed;
}


// DMA IRQ handler for fast restart when transfer completes
// Fast handler that resets read address and restarts transfer
void dma_irq_handler() {
    if (g_floppyEmulatorInstance) {
        g_floppyEmulatorInstance->handleDMAIRQ();
    }
}

// Initialize PIO and DMA for continuous bit output
void FloppyEmulator::initPIO_DMA() {
    // Claim a PIO instance and state machine
    int sm = pio_claim_unused_sm(pio0, false);
    if (sm >= 0) {
        pio = pio0;
    } else {
        sm = pio_claim_unused_sm(pio1, false);
        if (sm < 0) {
            pio = nullptr;  // no state machine: startPIO_DMA() stays a no-op
            return;
        }
        pio = pio1;
    }
    pioSm = (uint)sm;

    // Load PIO program
    pioOffset = pio_add_program(pio, &floppy_bit_output_program);

    // PIO program (unchanged): per bit "out pins,1" (0.5us) + nop -> 1us at the
    // bit level, then "set pins,1" + rest -> 4us per bit. The ring holds the
    // INVERTED bitstream, so a 1 bit is a 1us LOW pulse at the PIO output; the
    // pad override below gives the level the board needs.
    // Autopull every 8 bits: the DMA writes one byte per transfer into the TX
    // FIFO (the bus replicates it to bits 31..24), which a left shift (MSB
    // first) outputs first.
    pio_sm_config c = floppy_bit_output_program_get_default_config(pioOffset);
    sm_config_set_out_pins(&c, readPin, 1);
    sm_config_set_set_pins(&c, readPin, 1);
    sm_config_set_out_shift(&c, false, true, 8);   // MSB first, autopull after 8 bits
    // No FIFO join: the 4-entry TX FIFO is part of the head-position
    // calculation (its fill level is read back in headBitPos())
    sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_NONE);

    // The PIO drives its idle level before the pin is handed over to it
    pio_sm_set_pins_with_mask(pio, pioSm, 1u << readPin, 1u << readPin);
    pio_gpio_init(pio, readPin);
#if READ_OUTPUT_INVERTED
    gpio_set_outover(readPin, GPIO_OVERRIDE_NORMAL);   // the board's buffer inverts
#else
    // No inverting buffer: flip the pad output so that the pin (and the
    // connector) is idle LOW with HIGH pulses. Must come after pio_gpio_init(),
    // which rewrites the pad control register.
    gpio_set_outover(readPin, GPIO_OVERRIDE_INVERT);
#endif
    pio_sm_set_consecutive_pindirs(pio, pioSm, readPin, 1, true);

    // 2MHz state-machine clock: 8 cycles per bit = 4us bit cell
    float div = (float)clock_get_hz(clk_sys) / 2000000.0f;
    sm_config_set_clkdiv(&c, div);

    pio_sm_init(pio, pioSm, pioOffset, &c);
    pio_sm_set_enabled(pio, pioSm, false);
    // Idle level (PIO side HIGH = no pulses; see the pad override above)
    pio_sm_set_pins_with_mask(pio, pioSm, 1u << readPin, 1u << readPin);

    // Two DMA channels in ping-pong: A plays half 0 and chains to B, B plays
    // half 1 and chains back to A. Each completion raises DMA_IRQ_0 where the
    // finished half is refilled and its read address reset.
    dmaChannel = dma_claim_unused_channel(false);
    dmaChannelB = dma_claim_unused_channel(false);
    if (dmaChannel < 0 || dmaChannelB < 0) {
        dmaChannel = -1;
        dmaChannelB = -1;
        return;
    }

    dmaConfig = dma_channel_get_default_config(dmaChannel);
    channel_config_set_transfer_data_size(&dmaConfig, DMA_SIZE_8);
    channel_config_set_dreq(&dmaConfig, pio_get_dreq(pio, pioSm, true));
    channel_config_set_read_increment(&dmaConfig, true);
    channel_config_set_write_increment(&dmaConfig, false);
    channel_config_set_chain_to(&dmaConfig, dmaChannelB);

    dmaConfigB = dma_channel_get_default_config(dmaChannelB);
    channel_config_set_transfer_data_size(&dmaConfigB, DMA_SIZE_8);
    channel_config_set_dreq(&dmaConfigB, pio_get_dreq(pio, pioSm, true));
    channel_config_set_read_increment(&dmaConfigB, true);
    channel_config_set_write_increment(&dmaConfigB, false);
    channel_config_set_chain_to(&dmaConfigB, dmaChannel);

    if (g_floppyEmulatorInstance == nullptr) {
        g_floppyEmulatorInstance = this;  // Set instance for IRQ handler
    }
    // Any channel can be routed to DMA_IRQ_0 on RP2350; install the handler once
    irq_set_exclusive_handler(DMA_IRQ_0, dma_irq_handler);

    pioDmaActive = false;
}

// Number of valid bits in the cached track (falls back to the full buffer)
uint32_t FloppyEmulator::streamTrackBits() const {
    uint32_t bits = trackBitCount;
    if (bits < 8) {
        bits = APPLE_II_GCR_BYTES_PER_TRACK * 8;
    }
    return bits;
}

// Next 8 track bits from bit position pos (any alignment), wrapping at the
// end of the track. Byte-wise except across the wrap (once per revolution).
static inline uint8_t fetchTrackByte(const uint8_t* src, uint32_t& pos, uint32_t bits) {
    uint8_t b;
    if (pos + 8 <= bits) {
        uint32_t i = pos >> 3;
        uint32_t sh = pos & 7;
        // sh > 0: the 8 bits end in byte i+1, which lies inside the track
        b = sh ? (uint8_t)((src[i] << sh) | (src[i + 1] >> (8 - sh))) : src[i];
        pos += 8;
        if (pos >= bits) {
            pos = 0;
        }
        HOSTTEST_CYCLES(12);
        return b;
    }
    b = 0;
    for (int k = 0; k < 8; k++) {
        b = (uint8_t)((b << 1) | ((src[pos >> 3] >> (7 - (pos & 7))) & 1));
        if (++pos >= bits) {
            pos = 0;
        }
    }
    HOSTTEST_CYCLES(8 * 10);
    return b;
}

// Fill one half of the ring with the next STREAM_HALF_BYTES * 8 track bits,
// wrapping bit-exactly at the track length; inverted for the PIO's
// low-active pulses. Runs in the DMA interrupt on core0 (and before streaming
// starts), so it must be short: a write capture that starts meanwhile only
// begins after it, and the first bits the controller writes (5 sync bytes,
// then the data prologue, ~200 us in all) are lost while it runs. So: byte at
// a time (~25 cycles/byte) and small halves (64 bytes: ~10 us per refill at
// 200 MHz, one refill every 2 ms; still fine if the real code is several
// times slower than estimated). The old bit-by-bit weak-bit loop over 1 KB
// halves took ~0.6 ms and damaged sectors whose write started during a refill.
void FloppyEmulator::streamFillHalf(int half) {
    uint8_t* out = streamRing[half];
    const uint8_t* src = trackBits;
    uint32_t bits = streamTrackBits();
    uint32_t pos = streamFillBit;
    if (pos >= bits) {
        pos = 0;
    }
    streamHalfStartBit[half] = pos;

    if (fakeBitsEnabled) {
        // MC3470 model (WOZ spec): the chip outputs each bit one bit time
        // late; when the last four bits read were all zero its AGC produces a
        // random (fake) bit instead. Per byte: h = last 3 track bits before
        // it, s = h:b (11 bits, oldest first). Output bit j = s bit j+1
        // (delay), replaced by a random bit where s bits j..j+3 are all zero.
        uint32_t h = mcWindow & 7;
        uint32_t fp = fakePos & 31;
        for (uint32_t i = 0; i < STREAM_HALF_BYTES; i++) {
            uint32_t b = fetchTrackByte(src, pos, bits);
            uint32_t s = (h << 8) | b;
            uint32_t z = ~s;
            uint32_t weak = z & (z >> 1) & (z >> 2) & (z >> 3) & 0xFF;
            uint32_t delayed = (s >> 1) & 0xFF;
            uint32_t o = (delayed & ~weak) | (fakeRing[fp] & weak);
            fp = (fp + 1) & 31;
            h = b & 7;
            out[i] = (uint8_t)~o;
            HOSTTEST_CYCLES(13);
        }
        mcWindow = (uint8_t)h;
        fakePos = fp;
        streamFillBit = pos;
        return;
    }

    for (uint32_t i = 0; i < STREAM_HALF_BYTES; i++) {
        out[i] = (uint8_t)~fetchTrackByte(src, pos, bits);
    }
    streamFillBit = pos;
}

// Current head position (bit index into the cached track).
// Streaming: start bit of the playing half + bytes the DMA already handed to
// the PIO - bytes still waiting in the TX FIFO - about half a byte in the OSR.
// Error <= 4 bits. Not streaming: the disk keeps turning at 4us per bit.
uint32_t FloppyEmulator::headBitPos() {
    uint32_t bits = streamTrackBits();
    if (!pioDmaActive || dmaChannel < 0 || pio == nullptr) {
        int64_t elapsed = absolute_time_diff_us(streamStopTime, get_absolute_time());
        uint64_t advance = (elapsed > 0) ? (uint64_t)elapsed * 1000u / bitPeriodNs : 0;
        return (uint32_t)((streamStopBit + advance) % bits);
    }

    // Interrupts off for the snapshot: if the DMA IRQ ran between the reads it
    // would refill a half and move its start bit (result off by two halves).
    uint32_t irq = save_and_disable_interrupts();
    int half = dma_channel_is_busy((uint)dmaChannel) ? 0 : 1;
    uint ch = (uint)(half ? dmaChannelB : dmaChannel);
    uint32_t remaining = dma_hw->ch[ch].transfer_count & 0x0FFFFFFFu;  // low 28 bits = count (RP2350)
    uint32_t inFifo = pio_sm_get_tx_fifo_level(pio, pioSm);
    uint32_t startBit = streamHalfStartBit[half];
    restore_interrupts(irq);
    if (remaining > STREAM_HALF_BYTES) {
        remaining = STREAM_HALF_BYTES;
    }
    uint32_t sent = STREAM_HALF_BYTES - remaining;

    // -4: on average half of the byte in the OSR is still to come.
    // The MC3470 model (WOZ) delays the output by one more bit.
    int64_t pos = (int64_t)startBit + (int64_t)sent * 8 - (int64_t)inFifo * 8 - 4 - (fakeBitsEnabled ? 1 : 0);
    pos %= (int64_t)bits;
    if (pos < 0) {
        pos += bits;
    }
    return (uint32_t)pos;
}

// Start PIO/DMA streaming from cache buffer
void FloppyEmulator::startPIO_DMA() {
    if (pioDmaActive) {
        return;  // Already active
    }
    if (dmaChannel < 0 || dmaChannelB < 0 || pio == nullptr) {
        return;  // PIO/DMA not initialized
    }

    // Resume where the (still spinning) disk is now, then pre-fill both halves
    streamStopBit = headBitPos();
    streamFillBit = streamStopBit;
    streamFillHalf(0);
    streamFillHalf(1);

    // Restart the state machine cleanly (it may have stopped mid-bit)
    pio_sm_set_enabled(pio, pioSm, false);
    // 8 PIO cycles per bit cell: clkdiv = sys_clk * bitPeriod / 8
    pio_sm_set_clkdiv(pio, pioSm, (float)clock_get_hz(clk_sys) * (float)bitPeriodNs / 8.0e9f);
    pio_sm_clear_fifos(pio, pioSm);
    pio_sm_restart(pio, pioSm);
    pio_sm_exec(pio, pioSm, pio_encode_jmp(pioOffset));

    // 8-bit DMA writes to the TX FIFO register: the bus replicates the byte
    // into all four byte lanes, so it is in bits 31..24, which the left-shifting
    // OSR (MSB first, autopull 8) sends first. (Same as the SDK's PIO SPI example.)
    volatile uint8_t* txByte = (volatile uint8_t*)&pio->txf[pioSm];
    dma_channel_configure((uint)dmaChannel, &dmaConfig, (void*)txByte, streamRing[0], STREAM_HALF_BYTES, false);
    dma_channel_configure((uint)dmaChannelB, &dmaConfigB, (void*)txByte, streamRing[1], STREAM_HALF_BYTES, false);

    uint32_t mask = (1u << dmaChannel) | (1u << dmaChannelB);
    dma_hw->ints0 = mask;
    dma_channel_set_irq0_enabled((uint)dmaChannel, true);
    dma_channel_set_irq0_enabled((uint)dmaChannelB, true);
    irq_set_enabled(DMA_IRQ_0, true);

    HOSTTEST_CYCLES(1500);   // PIO/DMA set-up, position arithmetic
    pioDmaActive = true;
    pio_sm_set_enabled(pio, pioSm, true);
    dma_channel_start((uint)dmaChannel);
}

// Stop PIO/DMA streaming
void FloppyEmulator::stopPIO_DMA() {
    if (!pioDmaActive) {
        return;  // Already stopped
    }

    // Remember where the head is; startPIO_DMA() continues from here plus the
    // time that passed (the disk keeps spinning while we do not stream)
    streamStopBit = headBitPos();
    streamStopTime = get_absolute_time();
    pioDmaActive = false;

    if (dmaChannel >= 0 && dmaChannelB >= 0) {
        uint32_t mask = (1u << dmaChannel) | (1u << dmaChannelB);
        dma_channel_set_irq0_enabled((uint)dmaChannel, false);
        dma_channel_set_irq0_enabled((uint)dmaChannelB, false);

        // RP2350-E5: clear the enable bit of the aborted channel AND of the
        // channel it chains to before the abort, or the abort can re-trigger
        // the partner. startPIO_DMA() re-configures (and re-enables) both.
        hw_clear_bits(&dma_hw->ch[dmaChannel].al1_ctrl, DMA_CH0_CTRL_TRIG_EN_BITS);
        hw_clear_bits(&dma_hw->ch[dmaChannelB].al1_ctrl, DMA_CH0_CTRL_TRIG_EN_BITS);
        dma_hw->abort = mask;
        while ((dma_hw->ch[dmaChannel].ctrl_trig | dma_hw->ch[dmaChannelB].ctrl_trig) &
               DMA_CH0_CTRL_TRIG_BUSY_BITS) {
            tight_loop_contents();
        }
        dma_hw->ints0 = mask;   // a spurious completion IRQ may be latched
    }

    if (pio != nullptr) {
        pio_sm_set_enabled(pio, pioSm, false);
        pio_sm_clear_fifos(pio, pioSm);
        // Back to the idle level (it may have stopped in the middle of a pulse)
        pio_sm_set_pins_with_mask(pio, pioSm, 1u << readPin, 1u << readPin);
    }
}

// DMA IRQ: a ring half has been handed to the PIO. Reset that channel's read
// address (the chain retrigger reloads only the count) and refill the half
// with the next bits of the track while the other channel plays.
void FloppyEmulator::handleDMAIRQ() {
    if (dmaChannel < 0 || dmaChannelB < 0) {
        return;
    }
    uint32_t mask = (1u << dmaChannel) | (1u << dmaChannelB);
    uint32_t status = dma_hw->ints0 & mask;
    dma_hw->ints0 = status;  // clear first
    if (!pioDmaActive) {
        return;
    }
    if (status & (1u << dmaChannel)) {
        dma_channel_set_read_addr((uint)dmaChannel, streamRing[0], false);
        streamFillHalf(0);
    }
    if (status & (1u << dmaChannelB)) {
        dma_channel_set_read_addr((uint)dmaChannelB, streamRing[1], false);
        streamFillHalf(1);
    }
}

// Main processing loop - react to controller signals
// Drive WRPROT: write protected when no image is loaded (any write would be
// lost) or the image is protected (read-only file, WOZ INFO flag). The DOS
// then reports WRITE PROTECTED instead of believing a dropped write worked.
// Connector level: HIGH = protected; WPROT_OUTPUT_INVERTED flips the pin for
// a board with an inverting buffer.
void FloppyEmulator::updateWriteProtectOutput() {
    bool protect = !imageLoaded || isImageWriteProtected();
    if (protect != wprotOutput || !wprotInitialised) {
        wprotOutput = protect;
        wprotInitialised = true;
        gpio_put(GPIO_WR_PROTECT, (protect ? 1 : 0) ^ (WPROT_OUTPUT_INVERTED ? 1 : 0));
    }
}

// Pin monitor for hardware tests: counts falling edges of WRITE_EN (with and
// without this drive selected) and level changes of WRITE as seen by the main
// loop. If the Apple II writes but the capture never starts, these show
// whether WRITE_EN reaches its pin at all.
void FloppyEmulator::monitorPins() {
    uint32_t in = gpio_get_all();
    const uint32_t we = 1u << writeEnablePin, ds = 1u << driveSelPin, wr = 1u << writePin;
    // "active" bits: 1 where the (active low) signal is active
    const uint32_t inv = we | ds;
    if (monLastPins != 0xFFFFFFFFu) {
        uint32_t was = monLastPins ^ inv, now = in ^ inv;
        if (!(was & we) && (now & we)) {
            wstats.weFalls = wstats.weFalls + 1;          // write enable became active
            if (now & ds) wstats.weFallsSel = wstats.weFallsSel + 1;
        }
        if ((monLastPins ^ in) & wr) wstats.wrEdges = wstats.wrEdges + 1;
    }
    monLastPins = in;
}

void FloppyEmulator::process() {
    updateWriteProtectOutput();
    monitorPins();

    // Eject requested by the UI (core1): write back the last modified track of
    // the current image, stop the drive and unload. Done here on core0 so it
    // can never race with the write loop or with a track save.
    if (ejectRequested) {
        imageLoaded = false;          // stops the write loop
        if (pioDmaActive) {
            stopPIO_DMA();
        }
        saveGCRCacheToDiskImage(false);
        // Everything still pending must reach the card now: a few attempts
        for (int pass = 0; pass < 3 && pendingAny(); pass++) {
            if (pass > 0) {
                sleep_ms(100);
            }
            retryPendingTracks(PENDING_SLOTS);
        }
        if (pendingAny()) {
            unsavedLost = true;       // the UI reports it (takeUnsavedLost)
        }
        pendingClearAll();
        imageFile.valid = false;               // the handle must not be used again
        gcrTrackCacheTrack = -1;
        gcrTrackCacheDirty = false;
        ejectRequested = false;
        return;
    }
    
    if (imageLoaded && fakeBitsEnabled != weakBitsRequested.load(std::memory_order_relaxed)) {
        stopPIO_DMA();
        fakeBitsEnabled = weakBitsRequested.load(std::memory_order_relaxed);
        mcWindow = 0;
    }

    // RD_DATA is output whenever the drive is selected. (While write enable is
    // active too, the main loop is in captureWrite(), which pauses the stream
    // only for the duration of the write and resumes it afterwards.)
    if (imageLoaded && isDriveSelected()) {
        if (!pioDmaActive) {
            startPIO_DMA();
        }
    } else {
        if (pioDmaActive) {
            stopPIO_DMA();
        }
    }
    
    // The head moved to another track (the stepper is followed by the timer
    // interrupt). Only with an image loaded: while unloaded, core1 may be
    // reading a new file into diskImage and a cache built from it would be
    // garbage.
    if (imageLoaded && gcrTrackCacheTrack != desiredTrackKey()) {
        // Write back the track the head just left (if the Apple II wrote to
        // it) before the cache is overwritten with the new track.
        dbgLog('T', gcrTrackCacheTrack, desiredTrackKey(), physicalTrack);
        saveGCRCacheToDiskImage(false);
        // The ring still holds up to 2 x 32.8ms of the OLD track. DOS/ProDOS
        // check the track number in the address fields right after a seek, so
        // restart the streamer: it resumes at the same angular position (time
        // based) with the NEW track data.
        uint32_t oldBits = streamTrackBits();
        bool wasStreaming = pioDmaActive;
        if (wasStreaming) {
            stopPIO_DMA();
        }
        updateGCRTrackCache();
        // Tracks of different length (WOZ): keep the angular position,
        // new_position = position * new_length / old_length (WOZ spec)
        uint32_t newBits = streamTrackBits();
        if (newBits != oldBits && oldBits != 0) {
            streamStopBit = (uint32_t)((uint64_t)streamStopBit * newBits / oldBits);
        }
        if (wasStreaming) {
            startPIO_DMA();
        }
    }

    // ---- Persist what the Apple II wrote ---------------------------------
    // 1. Commit the written track into the image (RAM only, a few ms): as soon
    //    as the drive is deselected (motor off), or 1 s after the last write.
    //    The commit gives up at once if the controller starts writing again.
    absolute_time_t now = get_absolute_time();
    bool selected = isDriveSelected();
    if (gcrTrackCacheDirty &&
        (!selected || absolute_time_diff_us(lastWriteTime, now) > 1000000)) {
        saveGCRCacheToDiskImage(true);
    }

    // 2. Write committed tracks to the SD card only while the Apple II is not
    //    using the drive: an SD write blocks this loop for 10 ms or more, and a
    //    write from the controller that starts in that time would be lost.
    //    - Drive deselected (motor off): flush everything, one track per pass,
    //      so the card is up to date a fraction of a second after the drive
    //      light goes out.
    //    - Still selected: only if nothing was written or stepped for 2 s,
    //      then one track every 250 ms.
    //    A failed track stays pending and is retried every 500 ms.
    if (imageLoaded && sdCardManager && (!selected || pendingAny())) {
        int64_t sinceTry = absolute_time_diff_us(lastPendingRetry, now);
        bool quiet = absolute_time_diff_us(lastWriteTime, now) > 2000000 &&
                     (uint32_t)(time_us_32() - lastStepTimeUs) > 2000000u;
        bool due = !lastFlushOk ? (sinceTry > 500000)
                                : (!selected ? (pendingAny() || sinceTry > 1000)
                                             : sinceTry > 250000);
        if ((!selected || quiet) && due) {
            lastPendingRetry = now;
            // Never wait for the SD lock here: core1 (UI) may hold it for a while
            if (sdBusTryLock()) {
                bool recovering = imageFile.size && (!imageFile.valid || recoveryOffset < imageFile.size);
                if (recovering && !selected && sdCardManager->getFAT32()) {
                    lastFlushOk = recoverImageOnSD();
                } else if (!recovering && pendingAny()) {
                    lastFlushOk = retryPendingTracks(1);
                }
                if (recovering && !sdCardManager->getFAT32()) lastFlushOk = false;
                if (!recovering && !pendingAny() && !gcrTrackCacheDirty) lastFlushOk = true;
                sdBusUnlock();
            } else {
                lastFlushOk = false;
            }
        }
    }
}

//================================================================================
// WRITE capture
//================================================================================
// Free-running 25 ns timer: the counter of PWM slice 0 (no GPIO is switched to
// the PWM function, only the counter is used).
//
// The Disk II controller writes one bit cell every 4 CPU cycles (3.92 us): the
// WRITE line toggles for a 1 bit and stays for a 0 bit. Each flux transition is
// time-stamped and the interval since the previous one is rounded to whole
// cells: n cells = (n-1) zero bits followed by a 1 bit. The decision point is
// therefore in the middle of a cell (+-1.96 us of margin).
// (The previous loop restarted a 4.0 us timer at every transition and counted
// its overflows; a transition and the overflow then arrive ~100 ns apart, which
// produced spurious or missing 0 bits - hence the old "skip bit 17" hack.)
#define WRITE_TIMER_HZ    40000000u
#define WRITE_CELL_TICKS  157u        // 3.92 us in 25 ns ticks
#define WRITE_CELL_NS     3920u

void FloppyEmulator::initWritePWMTimer() {
    // Any slice except the one of the buzzer pin (it is only a counter, no pin
    // is connected to it)
    static_assert(WRITE_TIMER_SLICE != ((GPIO_BEEP >> 1) & 7), "slice used by the buzzer");
    slice_num = WRITE_TIMER_SLICE;
    pwm_set_clkdiv(slice_num, (float)clock_get_hz(clk_sys) / (float)WRITE_TIMER_HZ);
    pwm_set_wrap(slice_num, 0xFFFF);
    pwm_set_enabled(slice_num, true);   // runs forever
}

// Capture a write into the track bitstream at the head position, exactly like
// a head writing on the disk: old bits are overwritten, the track length does
// not change, the position wraps at the end of the track. No byte framing and
// no sync detection, so sector writes, formats and WOZ all use the same path.
//
// A write is a SESSION of one or more bursts. DOS 3.3 writes a sector as one
// burst, but its format routine switches write mode off and on again between
// the address field and the data field of every sector (WRADR16 / WRITE16):
// 32 bursts per track, about 50 us apart, and the data burst starts with only
// 5 sync bytes (196 us). Leaving the capture after a burst and restarting the
// read stream (~100-200 us) lost the start of the next burst: sectors of a
// freshly formatted track were unreadable, INIT ended with I/O ERROR. So after
// a burst the capture waits WRITE_HOLD_TICKS for the next one and only then
// hands the head back to the read stream. During the pause the disk keeps
// turning: those bits are skipped (left as they are, as on a real disk).
//
// Called from the main loop on core0; runs with interrupts disabled until the
// session ends (WRITE_EN idle for the hold time, or drive deselected).
#define WRITE_HOLD_TICKS  12000u      // 300 us in 25 ns ticks

void FloppyEmulator::captureWrite() {
    // Head position now - and when "now" is: the set-up below takes a few
    // microseconds during which the controller is already writing.
    uint32_t headPos = headBitPos();
    uint32_t t0 = time_us_32();
    monitorPins();   // counts this write request too (WE falling edge)
    HOSTTEST_CYCLES(1500);   // set-up until the capture loop runs (stop DMA, checks)

    // The read stream must not run while the track is modified / replaced
    stopPIO_DMA();

    // The cache must hold the track under the head. Normally it does (the
    // controller reads an address field before it writes); if a head step is
    // still pending, switch now (committing is RAM only).
    if (imageLoaded && gcrTrackCacheTrack != desiredTrackKey()) {
        saveGCRCacheToDiskImage(false);
        updateGCRTrackCache();
    }

    const uint32_t bits = streamTrackBits();
    // Not writable (protected image, empty/FLUX track, no image): the bits are
    // received and the position advances, but nothing is stored.
    const bool store = imageLoaded && gcrTrackCacheTrack == desiredTrackKey() && trackWritable();
    uint8_t* const tb = trackBits;
    const uint32_t wrMask = 1u << writePin;
    const uint32_t dsMask = 1u << driveSelPin;
    // WRITE_ENABLE# and DRIVE_SEL# are active low: writing while both are 0
    const uint32_t endMask = (1u << writeEnablePin) | dsMask;

    uint32_t irq = save_and_disable_interrupts();
    beeperSilence();          // the timer interrupt cannot end a sound now

    // Bits the controller wrote during the set-up were missed: leave those
    // track bits as they are and continue where the head is by now.
    uint32_t skipped = (uint32_t)(((uint64_t)(time_us_32() - t0) * 1000u) / WRITE_CELL_NS);
    const uint32_t startPos = (headPos + skipped) % bits;
    uint32_t pos = startPos;
    uint32_t captured = 0;    // bits written, all bursts
    uint32_t span = 0;        // track bits passed since the first burst began (written + pauses)
    uint32_t glitches = 0;
    uint32_t bursts = 0;

    uint16_t last = pwm_get_counter(slice_num);
    uint32_t in = gpio_get_all();
    uint32_t mag = in & wrMask;
    absolute_time_t endTime = get_absolute_time();

    for (;;) {
        bursts++;

        // ---- one burst: until WRITE_EN is released or the drive deselected ----
        while (!(in & endMask)) {
            in = gpio_get_all();
            uint16_t now = pwm_get_counter(slice_num);
            uint32_t dt = (uint16_t)(now - last);

            if ((in ^ mag) & wrMask) {
                // A real flux transition keeps its new level for a whole bit cell
                // (3.92 us); a spike on the line does not: confirm the new level
                // with a second sample first.
                if (!((gpio_get_all() ^ mag) & wrMask)) {
                    glitches++;
                    continue;
                }
                // Flux transition: (cells - 1) zero bits, then a 1 bit
                mag = in & wrMask;
                uint32_t cells = (dt + WRITE_CELL_TICKS / 2) / WRITE_CELL_TICKS;
                captured += cells ? cells : 1;
                for (; cells > 1; cells--) {
                    if (store) tb[pos >> 3] &= (uint8_t)~(0x80u >> (pos & 7));
                    if (++pos >= bits) pos = 0;
                }
                if (store) tb[pos >> 3] |= (uint8_t)(0x80u >> (pos & 7));
                if (++pos >= bits) pos = 0;
                last = now;
            } else if (dt >= 8 * WRITE_CELL_TICKS) {
                // Long stretch without flux: book 4 zero cells and keep the phase
                // (also keeps the 16-bit time difference from wrapping)
                for (int k = 0; k < 4; k++) {
                    if (store) tb[pos >> 3] &= (uint8_t)~(0x80u >> (pos & 7));
                    if (++pos >= bits) pos = 0;
                }
                captured += 4;
                last = (uint16_t)(last + 4 * WRITE_CELL_TICKS);
            }
        }

        // Empty cells between the last transition and the end of the burst
        const uint16_t tOff = pwm_get_counter(slice_num);
        endTime = get_absolute_time();
        uint32_t trail = ((uint16_t)(tOff - last) + WRITE_CELL_TICKS / 2) / WRITE_CELL_TICKS;
        if (trail > 8) trail = 8;
        for (uint32_t k = 0; k < trail; k++) {
            if (store) tb[pos >> 3] &= (uint8_t)~(0x80u >> (pos & 7));
            if (++pos >= bits) pos = 0;
        }
        captured += trail;

        // ---- pause: does the controller switch write mode on again? ----
        bool again = false;
        while ((uint16_t)(pwm_get_counter(slice_num) - tOff) < WRITE_HOLD_TICKS) {
            in = gpio_get_all();
            if (in & dsMask) break;                     // drive deselected: session over
            if (!(in & endMask)) { again = true; break; }
        }
        if (!again) break;

        // The disk kept turning: skip the bits that passed since the last
        // transition (measured from there, so rounding does not add up over
        // the bursts), minus the trailing cells already written.
        const uint16_t tOn = pwm_get_counter(slice_num);
        uint32_t passed = ((uint16_t)(tOn - last) + WRITE_CELL_TICKS / 2) / WRITE_CELL_TICKS;
        uint32_t gap = passed > trail ? passed - trail : 0;
        pos += gap;                                      // gap <= 85 cells
        if (pos >= bits) pos -= bits;
        span += gap;
        last = tOn;
        mag = in & wrMask;
    }
    span += captured;

    restore_interrupts(irq);

    wstats.captures = wstats.captures + 1;
    if (store) wstats.stored = wstats.stored + 1;
    wstats.lastBits = captured;
    wstats.lastSkip = skipped;
    wstats.lastTrack = gcrTrackCacheTrack;
    wstats.glitches = wstats.glitches + glitches;
    dbgLog('W', gcrTrackCacheTrack, (int32_t)captured,
           (int32_t)(((span > 0xFFFFu ? 0xFFFFu : span) << 16) | (skipped > 0xFFFFu ? 0xFFFFu : skipped)),
           (int32_t)startPos,
           (int32_t)(((uint32_t)physicalTrack << 24) | (store ? 0x800000u : 0u) |
                     ((bursts > 0x7Fu ? 0x7Fu : bursts) << 16) | (glitches > 0xFFFFu ? 0xFFFFu : glitches)));

    // Whole-track write (a format: more than half a revolution in one session).
    // Our track can be longer than what the controller writes in one real
    // revolution (DSK/NIC: 53248 bits = 213 ms at 4 us; a real disk turns in
    // ~200 ms, and the controller writes ~2% faster than we play back), so a
    // formatter that fills one real revolution would leave up to several
    // thousand bits of the old track behind - often a complete old sector with
    // a valid address field. The track would then hold that sector number
    // twice. On a real drive the format covers the whole circumference, so the
    // part not reached is filled with sync bytes (1111111100) instead.
    if (store && span >= bits / 2 && span < bits) {
        uint32_t rest = bits - span;       // from the end of the write round to its start
        uint32_t p = pos;
        for (uint32_t k = 0; k < rest; k++) {
            if ((k % 10) < 8) tb[p >> 3] |= (uint8_t)(0x80u >> (p & 7));
            else              tb[p >> 3] &= (uint8_t)~(0x80u >> (p & 7));
            if (++p >= bits) p = 0;
        }
    }

    if (store && captured > 0) {
        gcrTrackCacheDirty = true;   // committed in process() when the drive is quiet
    }
    lastWriteTime = endTime;

    // Resume reading where the head is now: position at the end of the last
    // burst, plus the time that has passed since (startPIO_DMA works that out)
    streamStopBit = pos;
    streamStopTime = endTime;
    if (imageLoaded && isDriveSelected()) {
        startPIO_DMA();
    }
}
