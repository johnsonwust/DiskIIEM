#include "DiskCodec.h"
#include <string.h>

namespace diskcodec {

namespace {

struct CodeTables {
    uint8_t toDisk[64];
    uint8_t fromDisk[256];
};

// A byte may be written to the disk as a "6-and-2" code if the drive
// electronics can read it reliably and it cannot be mistaken for a marker:
constexpr bool isCodeByte(int b) {
    if (!(b & 0x80) || b == 0xAA || b == 0xD5) return false;
    int zeroPairs = 0;
    bool onePair = false;
    for (int k = 0; k < 7; k++) {
        const bool lo = (b >> k) & 1, hi = (b >> (k + 1)) & 1;
        if (!lo && !hi) zeroPairs++;
        if (lo && hi && k < 6) onePair = true;
    }
    return zeroPairs <= 1 && onePair;
}

constexpr CodeTables makeTables() {
    CodeTables t{};
    for (int b = 0; b < 256; b++) t.fromDisk[b] = 0xFF;
    int n = 0;
    for (int b = 0x80; b < 0x100 && n < 64; b++) {
        if (isCodeByte(b)) {
            t.toDisk[n] = (uint8_t)b;
            t.fromDisk[b] = (uint8_t)n;
            n++;
        }
    }
    return t;
}

constexpr CodeTables TABLES = makeTables();
static_assert(TABLES.toDisk[0] == 0x96 && TABLES.toDisk[63] == 0xFF, "6-and-2 code table");

// The two low bits of a data byte are stored with their order exchanged
inline uint8_t exchanged(uint8_t twoBits) {
    return (uint8_t)(((twoBits & 1) << 1) | ((twoBits >> 1) & 1));
}

// The 342 six-bit values of a sector: first 86 values made of the low bit
// pairs (of bytes i, i + 86 and i + 172), then the upper six bits of all 256
// bytes.
inline uint8_t sixBitValue(const uint8_t* data, int index) {
    if (index >= 86) {
        return (uint8_t)(data[index - 86] >> 2);
    }
    uint8_t v = (uint8_t)(exchanged(data[index] & 3) | (exchanged(data[index + 86] & 3) << 2));
    if (index + 172 < 256) {
        v |= (uint8_t)(exchanged(data[index + 172] & 3) << 4);
    }
    return v;
}

}  // namespace


void encode62(const uint8_t* data, uint8_t* nibbles) {
    // Each value is written XORed with the one before it; the last disk byte
    // is the checksum (the final value itself).
    uint8_t previous = 0;
    for (int i = 0; i < 342; i++) {
        const uint8_t v = sixBitValue(data, i);
        nibbles[i] = TABLES.toDisk[v ^ previous];
        previous = v;
    }
    nibbles[342] = TABLES.toDisk[previous];
}

bool decode62(const uint8_t* nibbles, uint8_t* data) {
    uint8_t low[86];
    uint8_t running = 0;
    for (int i = 0; i < NIBBLES_PER_DATA_FIELD; i++) {
        const uint8_t v = TABLES.fromDisk[nibbles[i]];
        if (v == 0xFF) {
            return false;                    // not a disk byte of this code
        }
        running ^= v;
        if (i < 86) {
            low[i] = running;
        } else if (i < 342) {
            data[i - 86] = (uint8_t)(running << 2);
        }
    }
    if (running != 0) {
        return false;                        // checksum
    }
    for (int i = 0; i < 86; i++) {
        data[i] |= exchanged(low[i] & 3);
        data[i + 86] |= exchanged((low[i] >> 2) & 3);
        if (i + 172 < 256) {
            data[i + 172] |= exchanged((low[i] >> 4) & 3);
        }
    }
    return true;
}

void put44(uint8_t value, uint8_t* out) {
    out[0] = (uint8_t)(0xAA | (value >> 1));     // odd bits
    out[1] = (uint8_t)(0xAA | value);            // even bits
}


uint8_t dosOrderSector(uint8_t trackSector) {
    // DOS 3.3 skew: 0 and 15 stay, the others advance by 7 (mod 15)
    trackSector &= 15;
    return trackSector == 15 ? 15 : (uint8_t)((trackSector * 7) % 15);
}

void buildSector(uint8_t* out, uint8_t volume, uint8_t track, uint8_t sector, const uint8_t* data) {
    int n = 0;
    // Gap: 22 FF bytes, then 96 bits of self-sync (FF followed by two 0 bits,
    // repeated). The 10-bit pattern starts 4 bits into a cell, so that the
    // run ends exactly on a byte boundary.
    memset(out, 0xFF, 22);
    n = 22;
    for (int byte = 0; byte < 12; byte++) {
        uint8_t v = 0;
        for (int bit = 0; bit < 8; bit++) {
            const int cell = (byte * 8 + bit + 4) % 10;
            v = (uint8_t)((v << 1) | (cell < 8 ? 1 : 0));
        }
        out[n++] = v;
    }
    // Address field
    out[n++] = 0xD5; out[n++] = 0xAA; out[n++] = 0x96;
    put44(volume, &out[n]); n += 2;
    put44(track, &out[n]); n += 2;
    put44(sector, &out[n]); n += 2;
    put44((uint8_t)(volume ^ track ^ sector), &out[n]); n += 2;
    out[n++] = 0xDE; out[n++] = 0xAA; out[n++] = 0xEB;
    // Gap, data field
    memset(&out[n], 0xFF, 5);
    n += 5;
    out[n++] = 0xD5; out[n++] = 0xAA; out[n++] = 0xAD;
    encode62(data, &out[n]);
    n += NIBBLES_PER_DATA_FIELD;
    out[n++] = 0xDE; out[n++] = 0xAA; out[n++] = 0xEB;
    // Gap up to the next sector
    memset(&out[n], 0xFF, (size_t)(SECTOR_BYTES - n));
}

}  // namespace diskcodec
