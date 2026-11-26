#ifndef WOZ_IMAGE_H
#define WOZ_IMAGE_H

// WOZ 1 / WOZ 2 disk image parser (5.25" only).
// Normally works on the whole file already loaded in RAM; tracks are then
// described by their byte offset into that buffer (no copy). For WOZ2 files
// larger than the buffer only the first 1536 bytes (header, INFO, TMAP, TRK
// list) are needed: every offset/length is validated against the FILE size,
// so a damaged or hostile file can never make the emulator access anything
// outside the file.
// Reference: applesaucefdc.com/woz/reference2/ (WOZ 2.1) and /woz/reference/.

#include <stdint.h>
#include <stdbool.h>

#define WOZ_QUARTER_TRACKS   160     // TMAP entries (5.25": quarter tracks 0.00 .. 39.75)
#define WOZ_NO_TRACK         0xFF    // TMAP value: no data -> emulator plays random bits
#define WOZ_EMPTY_TRACK_BITS 51200   // length used for empty tracks (spec)
#define WOZ2_HEADER_BYTES    1536    // header + INFO + TMAP + TRKS list (WOZ2 fixed layout)

typedef enum {
    WOZ_OK = 0,
    WOZ_ERR_HEADER,       // not "WOZ1"/"WOZ2" + FF 0A 0D 0A
    WOZ_ERR_CRC,          // header CRC32 does not match the file
    WOZ_ERR_CHUNKS,       // chunk list runs past the end of the file / INFO, TMAP or TRKS missing
    WOZ_ERR_NOT_525,      // 3.5" disk (not supported by a Disk II)
    WOZ_ERR_TRKS,         // a track lies outside the file or has an impossible length
    WOZ_ERR_TMAP,         // TMAP points to a missing/invalid track
    WOZ_ERR_NO_TRACKS,    // nothing to play
    WOZ_ERR_TOO_BIG,      // larger than the buffer and not a WOZ2 (WOZ1 must fit in RAM)
    WOZ_ERR_FLUX,         // FLUX chunk invalid
    WOZ_ERR_READ          // SD read error while loading
} WozResult;

typedef struct {
    uint32_t offset;      // byte offset of the bitstream (or flux data) in the file
    uint32_t bitCount;    // bits; for FLUX tracks: number of flux BYTES; 0 = unused entry
    uint32_t byteCount;   // bytes reserved for the track in the file
} WozTrack;

typedef struct {
    uint8_t  version;          // 1 = WOZ1, 2 = WOZ2
    uint8_t  infoVersion;      // INFO chunk version (1..3)
    bool     writeProtected;
    bool     synchronized;
    bool     cleaned;          // MC3470 fake bits removed by the imager
    bool     hasFlux;          // INFO says a FLUX chunk exists
    bool     fluxMapValid;     // fluxMap[] loaded and validated
    uint8_t  bitTiming;        // 125 ns units, 32 = 4 us
    uint8_t  trackCount;       // valid TRK entries
    uint16_t fluxBlock;        // block of the FLUX chunk (0 = none)
    uint32_t crc;              // CRC32 from the header (0 = none)
    uint8_t  tmap[WOZ_QUARTER_TRACKS];
    uint8_t  fluxMap[WOZ_QUARTER_TRACKS];   // 0xFF = no flux data for this quarter track
    WozTrack trk[WOZ_QUARTER_TRACKS];
    char     creator[33];
} WozInfo;

// Parse and validate.
//   data/avail : the first `avail` bytes of the file
//   fileSize   : real size of the file (avail == fileSize: whole file in RAM)
// With avail < fileSize only WOZ2 is accepted and the CRC is NOT checked here
// (the caller streams it with wozCrc32Begin/Update/End).
// On failure the image must not be used.
WozResult wozParse(const uint8_t* data, uint32_t avail, uint32_t fileSize, WozInfo* info);

// Parse the FLUX chunk (WOZ 2.1). `chunk` points at file offset fluxBlock*512,
// `avail` bytes are readable there. Fills info->fluxMap / fluxMapValid.
WozResult wozParseFluxMap(const uint8_t* chunk, uint32_t avail, WozInfo* info);

// Short text for the display (max 16 characters)
const char* wozResultText(WozResult r);

// Standard CRC32 (as used by WOZ, Appendix A)
uint32_t wozCrc32(const uint8_t* data, uint32_t len);
static inline uint32_t wozCrc32Begin() { return 0xFFFFFFFFu; }
uint32_t wozCrc32Update(uint32_t state, const uint8_t* data, uint32_t len);
static inline uint32_t wozCrc32End(uint32_t state) { return state ^ 0xFFFFFFFFu; }

#endif // WOZ_IMAGE_H
