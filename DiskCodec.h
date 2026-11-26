#ifndef DISK_CODEC_H
#define DISK_CODEC_H

#include <stdint.h>

// Encoding of 16-sector 5.25" disks ("6-and-2", as written by DOS 3.3 and
// ProDOS). Written for this project from the published description of the
// format (Beneath Apple DOS, chapter 3); nothing here depends on hardware.
//
// The 64 disk bytes of the code are not stored as a list of constants: they
// are derived at compile time from the rules that define them (bit 7 set, at
// least one pair of adjacent 1 bits below bit 7, at most one pair of adjacent
// 0 bits, not the reserved bytes AA and D5).
namespace diskcodec {

const int NIBBLES_PER_DATA_FIELD = 343;   // 342 six-bit values + checksum
const int SECTOR_BYTES = 416;             // one sector as stored in a .nic slot

// 256 data bytes <-> the 343 disk bytes of a data field.
// decode62() returns false for an invalid disk byte or a wrong checksum.
void encode62(const uint8_t* data, uint8_t* nibbles);
bool decode62(const uint8_t* nibbles, uint8_t* data);

// "4-and-4": one byte as two disk bytes (address fields)
void put44(uint8_t value, uint8_t* out);

// Sector number on the track (address field) -> sector position in a
// DOS-order image file (.dsk)
uint8_t dosOrderSector(uint8_t trackSector);

// A complete sector, byte aligned, SECTOR_BYTES long: sync, address field,
// sync, data field, sync. The first sync run is self-synchronising (10-bit
// FF bytes packed into the byte stream).
void buildSector(uint8_t* out, uint8_t volume, uint8_t track, uint8_t sector, const uint8_t* data);

}  // namespace diskcodec

#endif // DISK_CODEC_H
