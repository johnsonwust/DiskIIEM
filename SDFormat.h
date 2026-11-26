#ifndef SD_FORMAT_H
#define SD_FORMAT_H

#include <stdint.h>
#include <stdbool.h>
#include "SDCardManager.h"

// Format the SD card as FAT32 with an adaptive cluster size.
// (The project's FAT32 driver only supports FAT32, so all cards are
//  formatted as FAT32 regardless of size.)
// A 64 MB card uses 512-byte clusters to meet the FAT32 minimum of 65525.
// Holds the SD lock for the whole operation; eject the floppy image first.
// Returns true on success.
// Returns false on failure; *failStep (optional) says where it stopped:
//  1 card size (CSD)   2 card too small   3 gap before partition   4 reserved area
//  5 FAT tables        6 root directory   7 FSInfo                 8 boot sector
//  9 MBR              10 read-back check of MBR / boot sector failed
bool formatSDCard(SDCardManager* sd, int* failStep = nullptr);

#endif
