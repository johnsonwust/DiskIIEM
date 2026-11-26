#ifndef FLOPPY_EMULATOR_H
#define FLOPPY_EMULATOR_H

#include <atomic>
#include <cstdint>
#include <stdint.h>
#include <stdbool.h>
#include "hardware/gpio.h"
#include "pico/time.h"
#include "hardware/timer.h"
#include "hardware/pio.h"
#include "hardware/dma.h"
#include "floppy_bit_output.pio.h"
#include "FAT32.h"   // FAT32_File (handle of the loaded image file)
#include "WozImage.h" // WOZ1/WOZ2 parser

// Apple II Floppy Disk Constants
#define APPLE_II_TRACKS           35      // 0-34 tracks
#define APPLE_II_SECTORS_PER_TRACK 16      // Sectors per track
#define APPLE_II_BYTES_PER_SECTOR  256     // Bytes per sector (raw data)
#define APPLE_II_BIT_PERIOD_US     4       // 4 microseconds per bit period (standard Apple II Disk II: 1 bit every 4μs = 125 kbps)
#define APPLE_II_ROTATION_TIME_MS  200     // One full rotation = 200ms (300 RPM)
#define APPLE_II_ROTATION_TIME_US  (APPLE_II_ROTATION_TIME_MS * 1000)  // 200,000 microseconds
#define APPLE_II_DISK_SIZE         (APPLE_II_TRACKS * APPLE_II_SECTORS_PER_TRACK * APPLE_II_BYTES_PER_SECTOR)  // 143360 bytes (DSK format)
#define APPLE_II_NIC_DISK_SIZE     (APPLE_II_TRACKS * APPLE_II_SECTORS_PER_TRACK * 512)  // 286720 bytes (NIC format: 512 bytes per sector)
#define APPLE_II_MAX_DISK_SIZE     APPLE_II_NIC_DISK_SIZE  // Maximum size to support both DSK and NIC formats
#define APPLE_II_BYTES_PER_TRACK   (APPLE_II_SECTORS_PER_TRACK * APPLE_II_BYTES_PER_SECTOR)  // 4096 bytes (raw data for DSK)
#define APPLE_II_NIC_BYTES_PER_TRACK (APPLE_II_SECTORS_PER_TRACK * 512)  // 8192 bytes per track in NIC file (but only 416 bytes per sector are used)

// Apple II Disk II track format:
// Each track has 16 sectors, each sector consists of:
// - Address field: prologue (0xD5 0xAA 0x96) + volume/track/sector/checksum + epilogue (0xDE 0xAA 0xEB)
// - Data field: sync bytes (0xFF) + prologue + 256 bytes data + epilogue + gap
// Total raw bytes per track: 6656 bytes (including address fields, sync, gaps)
// Each sector: 416 bytes (not 512 - padding is not part of track format)
// Total: 16 sectors * 416 bytes = 6656 bytes per track
// After GCR encoding: 6656 bytes (already GCR encoded in NIC format)
#define APPLE_II_RAW_TRACK_BYTES   6656    // Raw bytes per track (416 bytes per sector * 16 sectors)
#define APPLE_II_GCR_BYTES_PER_TRACK  APPLE_II_RAW_TRACK_BYTES  // Already GCR encoded in NIC format

// Disk image file type
typedef enum {
    DISK_FILE_TYPE_DSK = 0,    // .dsk: 35 x 16 sectors of 256 bytes, DOS order
    DISK_FILE_TYPE_NIC = 1,    // .nic: 35 x 16 slots of 512 bytes holding 416 disk bytes each
    DISK_FILE_TYPE_WOZ = 2     // .woz: WOZ1 / WOZ2 bitstream image
} DiskFileType;

class SDCardManager;

class FloppyEmulator {
private:
    // RAW disk image storage (linear array)
    // For DSK files: stores raw sector data (143360 bytes)
    // For NIC files: stores GCR-encoded track data (286720 bytes, but only first 416 bytes per sector are used)
    uint8_t diskImage[APPLE_II_MAX_DISK_SIZE];
    volatile bool imageLoaded;  // True once a disk image has been loaded (set on core1, read on core0)
    
    // Stepper motor control
    uint8_t stepperPhasePins[4];  // GPIO pins for PH0, PH1, PH2, PH3
    volatile int currentTrack;      // Current logical track (0-34); written by the stepper timer IRQ

    volatile int physicalTrack;     // Head position in QUARTER tracks (0-139, WOZ 0-159); written by the stepper timer IRQ
    volatile uint32_t lastStepTimeUs; // time_us_32() of the last head step (32 bit: atomic to read)
    
    
    // Read/Write control
    uint8_t readPin;                // GPIO pin for READ data output (to controller)
    uint8_t writePin;               // GPIO pin for WRITE data input (from controller)
    uint8_t writeEnablePin;         // GPIO pin for WRITE_ENABLE (from controller)
    uint8_t driveSelPin;            // GPIO pin for DRIVE_SEL (from controller - selects this drive)
    
    // Timing and synchronization
    
    
    
    // GCR track cache - pre-encoded GCR data for current track
    // This allows fast bit access in interrupt handler without expensive GCR encoding
    // (gcrTrackCache_tmp[] removed: unused, 6.5KB of RAM)

    // Track bitstream, NATURAL order: MSB first, a 1 bit = flux transition
    // (same layout as a WOZ track). It used to be stored inverted and 4-byte
    // swapped for the 32-bit DMA, which broke every reader/writer of the cache.
    uint8_t gcrTrackCache[APPLE_II_GCR_BYTES_PER_TRACK] __attribute__((aligned(4)));
    volatile int gcrTrackCacheTrack;         // Track number for which cache is valid (-1 = invalid) - set on core1
    volatile uint32_t gcrTrackCacheBits;     // Number of GCR bits in cache
    bool gcrTrackCacheDirty;        // True if GCR cache has been modified (needs to be saved before track change)
    
    
    // SD card and file management
    SDCardManager* sdCardManager;    // Pointer to SD card manager (for saving tracks to file)
    char currentFileName[64];        // Current disk image filename (for saving tracks)
    char imagePath[384] = {};
    uint32_t recoveryOffset = 0;
    bool recoverImageOnSD();
    FAT32_File imageFile;            // Handle of the loaded image (validated at load time)
    // Tracks changed in diskImage but not yet written to the SD card.
    // Index = DSK/NIC track number (0..34) or WOZ TRK entry (0..159).
    static const int PENDING_SLOTS = WOZ_QUARTER_TRACKS;
    uint32_t pendingMask[(PENDING_SLOTS + 31) / 32];
    void pendingSet(int i) { pendingMask[i >> 5] |= (1u << (i & 31)); }
    void pendingClear(int i) { pendingMask[i >> 5] &= ~(1u << (i & 31)); }
    bool pendingTest(int i) const { return (pendingMask[i >> 5] >> (i & 31)) & 1u; }
    bool pendingAny() const {
        for (uint32_t w : pendingMask) { if (w) return true; }
        return false;
    }
    void pendingClearAll() { for (uint32_t& w : pendingMask) w = 0; }
    bool wozCrcCleared;              // WOZ header CRC already set to 0 in the file
    bool lastFlushOk;                // last SD flush attempt succeeded (pacing of retries)
    volatile bool unsavedLost;       // an eject could not write everything to the card
    bool trackWritable();            // may the Apple II write to the track under the head?
    absolute_time_t lastPendingRetry;
    bool writeTrackToSD(int track);  // Write one track of diskImage into the image file
    bool retryPendingTracks(int maxTracks);   // false if a write failed
    volatile bool ejectRequested;    // Set by core1 (UI); core0 flushes + unloads in process()
    DiskFileType currentFileType;     // Type of loaded disk image file (.dsk or .nic)
    
    // PIO/DMA for continuous bit output
    PIO pio;                        // PIO instance (pio0 or pio1)
    uint pioSm;                     // PIO state machine index
    uint pioOffset;                 // PIO program offset
    int dmaChannel;                 // DMA channel A (plays ring half 0, chains to B)
    int dmaChannelB;                // DMA channel B (plays ring half 1, chains to A)
    volatile bool pioDmaActive;     // PIO/DMA active flag
    dma_channel_config dmaConfig;   // Channel A config
    dma_channel_config dmaConfigB;  // Channel B config

    // Track streamer: bit-exact playback of gcrTrackCache through an 8-bit
    // ping-pong ring. The DMA IRQ refills the half that just finished from the
    // bitstream, wrapping at gcrTrackCacheBits (any bit count, needed for WOZ).
    static const uint32_t STREAM_HALF_BYTES = 64;     // 512 bits = 2 ms per half: a refill takes only ~10 us (see streamFillHalf)
    uint8_t streamRing[2][STREAM_HALF_BYTES] __attribute__((aligned(4)));
    volatile uint32_t streamFillBit;           // next track bit to be put into the ring
    volatile uint32_t streamHalfStartBit[2];   // track bit index of the first bit of each half
    uint32_t streamStopBit;                    // head position when streaming stopped
    absolute_time_t streamStopTime;            // ... and when (the disk keeps spinning)
    uint32_t streamTrackBits() const;          // valid bit count of the playing track

    // Track source played by the streamer. DSK/NIC: gcrTrackCache (generated).
    // WOZ: points straight into diskImage at the track's bitstream (zero copy),
    // or gcrTrackCache filled with random bits for an empty (0xFF) track.
    uint8_t* trackBits;
    volatile uint32_t trackBitCount;
    uint32_t bitPeriodNs;                      // bit cell, 4000 ns (WOZ: INFO bit timing)
    volatile int maxPhysicalTrack;             // quarter-track limit: 139 (35 tracks), WOZ 159
    WozInfo woz;                               // parsed WOZ image (valid when currentFileType == WOZ)
    WozResult lastWozResult;
    static const int TRACK_KEY_EMPTY = 1000;   // gcrTrackCacheTrack value for a WOZ empty track
    int desiredTrackKey();                     // what the cache should hold for the head position
    void fillEmptyTrack();

    // P5: FLUX tracks, large WOZ files, MC3470 fake bits
    static const int FLUX_KEY_BASE = 200;      // key = FLUX_KEY_BASE + TRK index for FLUX tracks
    bool wozSlotMode;                          // file larger than diskImage: tracks cached from SD
    uint32_t wozSlotBytes;                     // bytes per slot (diskImage is split into slots)
    int wozSlotCount;
    static const int WOZ_MAX_SLOTS = 64;
    struct WozSlot { int16_t key; uint32_t bits; uint32_t lastUse; };
    WozSlot wozSlots[WOZ_MAX_SLOTS];
    uint32_t wozSlotClock;
    uint8_t* wozSlotPtr(int i) { return &diskImage[(uint32_t)i * wozSlotBytes]; }
    bool readImageBytes(uint32_t offset, uint8_t* buf, uint32_t n);
    uint32_t fluxToBits(uint32_t offset, uint32_t bytes, uint8_t* out, uint32_t capBits);
    int wozSlotFor(int key, bool* loaded);
    bool wozLoadTrack(int key);
    bool wozFinishLoad(uint32_t avail, uint32_t fileSize);
    bool fakeBitsEnabled;                      // MC3470 model in the streamer (WOZ)
    std::atomic<bool> weakBitsRequested{true};
    std::atomic<bool> attrWriteProtected{false};   // FAT read-only attribute of the image file
    bool wprotOutput = false;          // WRPROT currently signalled as "protected"
    // Volume number per track for DSK: a .dsk file holds only the sector data,
    // so the volume a format wrote is kept here (for this session) and used
    // when the track is generated again. Default 254 (DOS 3.3).
    uint8_t trackVolume[APPLE_II_TRACKS];
    uint32_t commitGoodSectors = 0;    // counters of the running commit (wstats)
    uint32_t monLastPins = 0xFFFFFFFFu; // pin monitor: last levels seen
public:
    void monitorPins();                // update the pin monitor counters
    uint32_t monitorLevels() const { return monLastPins; }
private:
    uint32_t commitChangedSectors = 0;
    bool wprotInitialised = false;
    uint8_t mcWindow;                          // last 3 track bits seen by the model
    uint8_t fakeRing[32];                      // 256 random bits, ~30% ones (spec)
    uint32_t fakePos;                          // next byte of fakeRing (0..31)

    // Write capture: captureWrite() stores the bits straight into trackBits
    absolute_time_t lastWriteTime;             // end of the last write (commit / SD flush wait for idle)
    // bitstream -> checked sectors in diskImage; with mayAbort it stops as soon
    // as the controller starts writing (*completed = false)
    // bitstream -> image: decode every good sector of the track (DSK and NIC)
    bool commitTrackSectors(int track, bool mayAbort, bool* completed);
    bool storeNicSector(int track, int sector, uint8_t volume, const uint8_t* data);
    void streamFillHalf(int half);
    uint32_t headBitPos();                     // current head position (bit index) on the track
    
    
    // Internal methods
    void detectStepperPhaseChange();  // Detect phase changes from controller
    void updateGCRTrackCache();  // Update GCR cache for current track (called when track changes)
    void initPIO_DMA();          // Initialize PIO and DMA for continuous bit output
    void startPIO_DMA();         // Start PIO/DMA streaming from cache buffer
    void stopPIO_DMA();          // Stop PIO/DMA streaming
    
public:
    // Commit the written track cache into the image (RAM). Returns false if
    // it was aborted (mayAbort) because the controller started writing.
    bool saveGCRCacheToDiskImage(bool mayAbort = false);
    void handleDMAIRQ();         // Handle DMA IRQ for fast restart (called from IRQ handler)
    // GPIO IRQ handler (public for IRQ handler access)
    //void handleWriteEnableIRQ(uint32_t events);  // Handle WRITE_EN GPIO IRQ (called from IRQ handler)
    //void handleWriteIRQ(uint32_t events);  // Handle WRITE pin GPIO IRQ (called from IRQ handler)
    // Timer interrupt methods (public for callback access)
    

    bool getGCRTrackCacheDirty(); // Get GCR track cache dirty flag

    void initWritePWMTimer();    // Free-running 25 ns timer for write capture
    // Capture one write (WRITE_EN active) into the track under the head.
    // Call from the core0 main loop; returns when the write has ended.
    void captureWrite();
    // True once after an eject that could not write all changes to the card
    bool takeUnsavedLost() { bool v = unsavedLost; unsavedLost = false; return v; }

    // Constructor
    FloppyEmulator(
        uint8_t ph0, uint8_t ph1, uint8_t ph2, uint8_t ph3,
        uint8_t read, uint8_t write, uint8_t writeEnable, uint8_t driveSel
    );
    
    // Destructor
    ~FloppyEmulator();
    
    // Initialization
    void init();
    
    // Stepper motor monitoring (reactive to controller signals)
    void processStepperMotor();     // Process stepper motor phase changes from controller
    int getCurrentTrack() const;
    
    // GCR encoding/decoding
    
    // Read/Write operations (reactive to controller)
    // Get GCR cache contents for a sector (416 bytes per sector in cache)
    // Uses current track from cache - each sector occupies 416 bytes
    
    // Disk image management
    // Returns false if the image is invalid (WOZ: see getLastWozResult()); the
    // drive then stays empty.
    bool loadDiskImage(const uint8_t* image, uint32_t size);
    bool isWoz() const { return currentFileType == DISK_FILE_TYPE_WOZ; }
    int getQuarterTrack() const { return physicalTrack; }   // head position in quarter tracks
    bool isWozFromSD() const { return currentFileType == DISK_FILE_TYPE_WOZ && wozSlotMode; }
    const WozInfo& getWozInfo() const { return woz; }
    WozResult getLastWozResult() const { return lastWozResult; }
    void clearDiskImage();
    uint8_t* getDiskImage();
    uint32_t getDiskImageSize() const;
    bool hasImageLoaded() const { return imageLoaded; }
    
    // Timing and synchronization
    
    // Main processing loop (call this in main loop to react to controller)
    void process();                 // Process all controller signals and respond
    
    // Status
    bool isDriveSelected() const;  // Check if this drive is selected by controller
    bool isWriteEnabled() const;
    
    // SD card and file management
    void setSDCardManager(SDCardManager* sdCard);  // Set SD card manager for saving tracks
    void setCurrentFileName(const char* filename);  // Set current disk image filename
    void setImagePath(const char* path);
    void setWeakBits(bool enabled) { weakBitsRequested.store(enabled, std::memory_order_relaxed); }
    bool isImageWriteProtected() const;
    void updateWriteProtectOutput();      // drive WRPROT (GPIO_WR_PROTECT) from the image state
    bool isWriteProtectSignalled() const { return wprotOutput; }

    // Write path counters (shown on the Status page; for hardware tests).
    // Each stage of a write: captured from the controller -> stored into the
    // track -> decoded into the image (commit) -> written to the SD card.
    struct WriteStats {
        uint32_t captures;      // write windows (WRITE_EN active)
        uint32_t stored;        // ... whose bits went into the track
        uint32_t lastBits;      // bits of the last write
        uint32_t lastSkip;      // bits missed before the capture loop started
        int32_t  lastTrack;     // track (cache key) of the last write
        uint32_t glitches;      // WRITE edges rejected as spikes
        uint32_t commits;       // DSK/NIC track commits
        uint32_t lastGood;      // sectors decoded in the last commit
        uint32_t lastChanged;   // ... that changed the image
        uint32_t sdOk;          // track writes to the card
        uint32_t sdFail;
        // Pin monitor (main loop, also when no write is captured):
        uint32_t weFalls;       // WRITE_EN went low (any drive select state)
        uint32_t weFallsSel;    // ... while DRIVE_SEL was low (this drive)
        uint32_t wrEdges;       // level changes seen on WRITE
    };
    volatile WriteStats wstats = {};

    // Event log for hardware tests (RAM ring, saved to the card from the
    // Config menu): every write capture, track switch, commit and card write.
    struct DbgEvent {
        uint32_t timeUs;
        char     type;        // W write, T track switch, C commit, F flush, L load
        int16_t  a;
        int32_t  b, c, d, e;
    };
    static const uint32_t DBG_EVENTS = 256;     // RAM is tight (the 280 KB disk buffer and the code live in RAM)
    DbgEvent dbgRing[DBG_EVENTS];
    volatile uint32_t dbgCount = 0;     // events logged so far (ring index = count % size)
    void dbgLog(char type, int a, int32_t b = 0, int32_t c = 0, int32_t d = 0, int32_t e = 0) {
        DbgEvent& ev = dbgRing[dbgCount % DBG_EVENTS];
        ev.timeUs = time_us_32(); ev.type = type; ev.a = (int16_t)a;
        ev.b = b; ev.c = c; ev.d = d; ev.e = e;
        dbgCount = dbgCount + 1;
    }
    const uint8_t* currentTrackBits() const { return trackBits; }
    uint32_t currentTrackBitCount() const { return streamTrackBits(); }
    int currentTrackKey() const { return gcrTrackCacheTrack; }
    bool toggleImageWriteProtection();
    // Called with the SD lock held before media is deinitialized.
    void sdMediaRemoved();
    bool hasPendingMediaRecovery();
    void setImageFile(const FAT32_File& file) {      // Handle from SDCardManager::loadDiskImage
        imageFile = file;
        attrWriteProtected.store(file.valid && file.readOnly, std::memory_order_relaxed);
    }
    // Called from core1 before loading another image / formatting: asks core0
    // to write back the dirty track of the current image, stop the drive and
    // unload it. Waits up to timeoutMs. Returns false on timeout.
    bool ejectImage(uint32_t timeoutMs = 5000);
    const char* getCurrentFileName() const;  // Get current disk image filename
    DiskFileType getCurrentFileType() const;  // Get current disk image file type
};

#endif // FLOPPY_EMULATOR_H

