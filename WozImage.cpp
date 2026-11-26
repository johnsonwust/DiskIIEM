#include "WozImage.h"
#include <string.h>

static uint16_t le16(const uint8_t* p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t le32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// Nibble-table CRC32 (reflected, poly 0xEDB88320): 64 bytes of table instead
// of 1KB, about 15 ms for a 236KB image at 200 MHz.
static const uint32_t s_crcNibble[16] = {
    0x00000000, 0x1DB71064, 0x3B6E20C8, 0x26D930AC, 0x76DC4190, 0x6B6B51F4, 0x4DB26158, 0x5005713C,
    0xEDB88320, 0xF00F9344, 0xD6D6A3E8, 0xCB61B38C, 0x9B64C2B0, 0x86D3D2D4, 0xA00AE278, 0xBDBDF21C
};

uint32_t wozCrc32Update(uint32_t crc, const uint8_t* data, uint32_t len) {
    for (uint32_t i = 0; i < len; i++) {
        crc ^= data[i];
        crc = (crc >> 4) ^ s_crcNibble[crc & 15];
        crc = (crc >> 4) ^ s_crcNibble[crc & 15];
    }
    return crc;
}

uint32_t wozCrc32(const uint8_t* data, uint32_t len) {
    return wozCrc32End(wozCrc32Update(wozCrc32Begin(), data, len));
}

const char* wozResultText(WozResult r) {
    switch (r) {
        case WOZ_OK:            return "OK";
        case WOZ_ERR_HEADER:    return "BAD WOZ HEADER";
        case WOZ_ERR_CRC:       return "WOZ CRC ERROR";
        case WOZ_ERR_CHUNKS:    return "BAD WOZ CHUNKS";
        case WOZ_ERR_NOT_525:   return "WOZ NOT 5.25\"";
        case WOZ_ERR_TRKS:      return "BAD WOZ TRACKS";
        case WOZ_ERR_TMAP:      return "BAD WOZ TMAP";
        case WOZ_ERR_NO_TRACKS: return "WOZ NO TRACKS";
        case WOZ_ERR_TOO_BIG:   return "WOZ TOO BIG";
        case WOZ_ERR_FLUX:      return "BAD WOZ FLUX";
        case WOZ_ERR_READ:      return "WOZ READ ERROR";
    }
    return "BAD WOZ";
}

// Chunk IDs (little-endian ASCII)
#define WOZ_ID_INFO 0x4F464E49u  // "INFO"
#define WOZ_ID_TMAP 0x50414D54u  // "TMAP"
#define WOZ_ID_TRKS 0x534B5254u  // "TRKS"
#define WOZ_ID_FLUX 0x58554C46u  // "FLUX"

#define WOZ1_TRK_SIZE        6656
#define WOZ1_BITSTREAM_SIZE  6646

WozResult wozParse(const uint8_t* data, uint32_t avail, uint32_t fileSize, WozInfo* info) {
    if (!data || !info) return WOZ_ERR_HEADER;
    memset(info, 0, sizeof(*info));
    memset(info->tmap, WOZ_NO_TRACK, sizeof(info->tmap));
    memset(info->fluxMap, WOZ_NO_TRACK, sizeof(info->fluxMap));
    if (avail > fileSize) avail = fileSize;
    const bool whole = (avail == fileSize);

    // ---- header ---------------------------------------------------------
    if (avail < 12 + 8) return WOZ_ERR_HEADER;
    if (data[0] != 'W' || data[1] != 'O' || data[2] != 'Z') return WOZ_ERR_HEADER;
    if (data[3] == '1') info->version = 1;
    else if (data[3] == '2') info->version = 2;
    else return WOZ_ERR_HEADER;
    if (data[4] != 0xFF || data[5] != 0x0A || data[6] != 0x0D || data[7] != 0x0A) return WOZ_ERR_HEADER;
    if (!whole && (info->version != 2 || avail < WOZ2_HEADER_BYTES)) {
        return WOZ_ERR_TOO_BIG;   // WOZ1 needs its TRK records in RAM
    }

    info->crc = le32(&data[8]);
    if (whole && info->crc != 0 && wozCrc32(&data[12], fileSize - 12) != info->crc) {
        return WOZ_ERR_CRC;
    }

    // ---- chunk walk (headers must be readable; lengths checked vs the file)
    const uint8_t* infoData = nullptr; uint32_t infoSize = 0;
    const uint8_t* tmapData = nullptr; uint32_t tmapSize = 0;
    uint32_t trksOffset = 0;           uint32_t trksSize = 0;
    uint32_t off = 12;
    for (int guard = 0; guard < 64 && off + 8 <= avail; guard++) {
        uint32_t id = le32(&data[off]);
        uint32_t len = le32(&data[off + 4]);
        uint32_t body = off + 8;
        if (len > fileSize - body) return WOZ_ERR_CHUNKS;   // chunk runs past the file end
        if (id == WOZ_ID_INFO && !infoData && len <= avail - body) { infoData = &data[body]; infoSize = len; }
        else if (id == WOZ_ID_TMAP && !tmapData && len <= avail - body) { tmapData = &data[body]; tmapSize = len; }
        else if (id == WOZ_ID_TRKS && !trksSize) { trksOffset = body; trksSize = len; }
        off = body + len;
    }
    if (!infoData || infoSize < 37 || !tmapData || tmapSize < WOZ_QUARTER_TRACKS || trksSize == 0) {
        return WOZ_ERR_CHUNKS;
    }

    // ---- INFO -----------------------------------------------------------
    info->infoVersion = infoData[0];
    if (infoData[1] != 1) return WOZ_ERR_NOT_525;           // disk type 1 = 5.25"
    info->writeProtected = infoData[2] == 1;
    info->synchronized = infoData[3] == 1;
    info->cleaned = infoData[4] == 1;
    memcpy(info->creator, &infoData[5], 32);
    info->creator[32] = 0;
    for (int i = 31; i >= 0 && (info->creator[i] == ' ' || info->creator[i] == 0); i--) {
        info->creator[i] = 0;
    }
    info->bitTiming = 32;
    if (info->infoVersion >= 2 && infoSize >= 40) {
        uint8_t t = infoData[39];                            // file offset 59
        if (t >= 24 && t <= 40) info->bitTiming = t;         // sane 5.25" range, else default 4 us
    }
    if (info->infoVersion >= 3 && infoSize >= 50) {
        uint16_t fluxBlock = le16(&infoData[46]);
        uint16_t largestFlux = le16(&infoData[48]);
        info->hasFlux = fluxBlock != 0 && largestFlux != 0;
        info->fluxBlock = info->hasFlux ? fluxBlock : 0;
    }

    // ---- TRKS -----------------------------------------------------------
    uint32_t trackEntries = 0;
    if (info->version == 2) {
        if (trksSize < WOZ_QUARTER_TRACKS * 8) return WOZ_ERR_TRKS;
        if (trksOffset + WOZ_QUARTER_TRACKS * 8 > avail) return WOZ_ERR_TRKS;
        trackEntries = WOZ_QUARTER_TRACKS;
        for (uint32_t i = 0; i < trackEntries; i++) {
            const uint8_t* e = &data[trksOffset + i * 8];
            uint32_t startBlock = le16(&e[0]);
            uint32_t blockCount = le16(&e[2]);
            uint32_t bitCount = le32(&e[4]);
            if (startBlock == 0 && blockCount == 0) continue;            // unused
            if (startBlock < 3 || blockCount == 0) return WOZ_ERR_TRKS;
            if ((startBlock + blockCount) * 512u > fileSize) return WOZ_ERR_TRKS;
            if (bitCount < 8 || bitCount > blockCount * 4096u) return WOZ_ERR_TRKS;
            info->trk[i].offset = startBlock * 512u;
            info->trk[i].bitCount = bitCount;
            info->trk[i].byteCount = blockCount * 512u;
            info->trackCount++;
        }
    } else {
        trackEntries = trksSize / WOZ1_TRK_SIZE;
        if (trackEntries > WOZ_QUARTER_TRACKS) trackEntries = WOZ_QUARTER_TRACKS;
        for (uint32_t i = 0; i < trackEntries; i++) {
            uint32_t rec = trksOffset + i * WOZ1_TRK_SIZE;               // whole file in RAM (checked above)
            uint32_t bytesUsed = le16(&data[rec + WOZ1_BITSTREAM_SIZE]);
            uint32_t bitCount = le16(&data[rec + WOZ1_BITSTREAM_SIZE + 2]);
            if (bitCount == 0) continue;
            if (bitCount < 8 || bytesUsed > WOZ1_BITSTREAM_SIZE || (bitCount + 7) / 8 > WOZ1_BITSTREAM_SIZE) {
                return WOZ_ERR_TRKS;
            }
            info->trk[i].offset = rec;
            info->trk[i].bitCount = bitCount;
            info->trk[i].byteCount = WOZ1_BITSTREAM_SIZE;
            info->trackCount++;
        }
    }

    // ---- TMAP -----------------------------------------------------------
    bool anyMapped = false;
    for (int q = 0; q < WOZ_QUARTER_TRACKS; q++) {
        uint8_t v = tmapData[q];
        if (v != WOZ_NO_TRACK) {
            if (v >= trackEntries || info->trk[v].bitCount == 0) return WOZ_ERR_TMAP;
            anyMapped = true;
        }
        info->tmap[q] = v;
    }
    // A pure-FLUX image may have an empty TMAP; it is checked again after the
    // FLUX map has been read (see wozParseFluxMap)
    if (!anyMapped && !info->hasFlux) return WOZ_ERR_NO_TRACKS;
    return WOZ_OK;
}

WozResult wozParseFluxMap(const uint8_t* chunk, uint32_t avail, WozInfo* info) {
    if (!info) return WOZ_ERR_FLUX;
    info->fluxMapValid = false;
    memset(info->fluxMap, WOZ_NO_TRACK, sizeof(info->fluxMap));
    if (!chunk || avail < 8 + WOZ_QUARTER_TRACKS) return WOZ_ERR_FLUX;
    if (le32(&chunk[0]) != WOZ_ID_FLUX || le32(&chunk[4]) < WOZ_QUARTER_TRACKS) return WOZ_ERR_FLUX;
    const uint8_t* map = &chunk[8];
    bool any = false;
    for (int q = 0; q < WOZ_QUARTER_TRACKS; q++) {
        uint8_t v = map[q];
        if (v != WOZ_NO_TRACK) {
            // For FLUX tracks bitCount holds the number of flux BYTES
            if (v >= WOZ_QUARTER_TRACKS || info->trk[v].bitCount == 0 ||
                info->trk[v].bitCount > info->trk[v].byteCount) {
                return WOZ_ERR_FLUX;
            }
            any = true;
        }
        info->fluxMap[q] = v;
    }
    info->fluxMapValid = any;
    if (!any) {
        // FLUX chunk without tracks: only TMAP counts
        for (int q = 0; q < WOZ_QUARTER_TRACKS; q++) {
            if (info->tmap[q] != WOZ_NO_TRACK) return WOZ_OK;
        }
        return WOZ_ERR_NO_TRACKS;
    }
    return WOZ_OK;
}
