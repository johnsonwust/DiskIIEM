#include "SDFormat.h"
#include "FAT32.h"
#include "pico/time.h"
#include <string.h>

// Static work buffer: formatting runs on core1 (UI), whose stack is small, so
// keep the 512-byte sector buffers off the stack. The whole format runs under
// the SD lock, so a single shared buffer is safe.
static uint8_t s_sector[512];

static bool writeSector(SDCardManager* sd, uint32_t sector, const uint8_t* data) {
    for (int retry = 0; retry < 3; retry++) {
        if (sd->writeBlock(sector, data)) return true;
    }
    return false;
}

// Zero a range of sectors. Uses CMD25 multi-block writes in chunks (a FAT on a
// 16-32GB card is 15-30k sectors; one CMD24 per sector took minutes and every
// single slow write was a chance to fail), falling back to single blocks.
static bool zeroRange(SDCardManager* sd, uint32_t start, uint32_t count) {
    const uint32_t CHUNK = 128;
    while (count > 0) {
        uint32_t n = (count < CHUNK) ? count : CHUNK;
        if (!sd->writeZeroBlocks(start, n)) {
            memset(s_sector, 0, sizeof(s_sector));
            for (uint32_t i = 0; i < n; i++) {
                if (!writeSector(sd, start + i, s_sector)) return false;
            }
        }
        start += n;
        count -= n;
    }
    return true;
}

static void put32(uint8_t* p, uint32_t v) {
    p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; p[2] = (v >> 16) & 0xFF; p[3] = (v >> 24) & 0xFF;
}

static bool formatSteps(SDCardManager* sd, int* step);

bool formatSDCard(SDCardManager* sd, int* failStep) {
    int step = 0;
    bool ok = formatSteps(sd, &step);
    if (failStep) *failStep = ok ? 0 : step;
    return ok;
}

static bool formatSteps(SDCardManager* sd, int* step) {
    *step = 1;
    if (!sd || !sd->isInitialized()) return false;

    // The whole format is one SD operation: keep core0 (track saves) off the bus.
    SDLockGuard guard;

    uint32_t totalSectors = sd->getSectorCount();
    if (totalSectors == 0) return false;          // CSD could not be read
    *step = 2;

    const uint32_t partitionStart = 2048;         // 1MB aligned
    const uint32_t reserved = 32;
    const uint8_t numFats = 2;
    if (totalSectors <= partitionStart + 4096) return false;
    uint32_t partitionSectors = totalSectors - partitionStart;

    // Adaptive cluster size (sectors per cluster)
    uint8_t spc;
    if (totalSectors < 532480) spc = 1;           // < 260MB: 512B
    else if (totalSectors < 16777216) spc = 8;    // < 8GB: 4KB
    else if (totalSectors < 33554432) spc = 16;   // < 16GB: 8KB
    else if (totalSectors < 67108864) spc = 32;   // < 32GB: 16KB
    else spc = 64;                                 // >= 32GB: 32KB

    // Sectors per FAT: sized from an upper bound of the cluster count, so it is
    // always large enough (FAT32 uses 4 bytes per entry, entries 0/1 reserved).
    uint32_t clustersUpper = (partitionSectors - reserved) / spc;
    uint32_t spf = ((clustersUpper + 2) * 4 + 511) / 512;
    if (partitionSectors <= reserved + numFats * spf) return false;
    uint32_t dataSectors = partitionSectors - reserved - numFats * spf;
    uint32_t totalClusters = dataSectors / spc;

    // FAT32 needs at least 65525 clusters, otherwise every OS (and the FAT
    // type rules) treats the volume as FAT16 -> card unreadable. The old code
    // accepted 32MB cards which produced only ~62k clusters.
    if (totalClusters < 65525) return false;

    const uint32_t fatStart = partitionStart + reserved;
    const uint32_t rootSector = fatStart + numFats * spf;    // cluster 2
    const uint32_t volumeId = time_us_32() ^ (totalSectors * 2654435761u);

    // Order matters: first invalidate the old boot area, write the new metadata,
    // and write boot sector + MBR last, so an interrupted format never leaves a
    // valid-looking but half-erased file system behind.

    // 1. Clear the gap in front of the partition (sectors 1..2047: a leftover
    //    GPT header/table would otherwise still be found there by a PC) and the
    //    reserved area (old boot sector, FSInfo and their backups)
    *step = 3;
    if (!zeroRange(sd, 1, partitionStart - 1)) return false;
    *step = 4;
    if (!zeroRange(sd, partitionStart, reserved)) return false;
    *step = 5;

    // 2. FAT tables (all copies): zero, then the first sector with the
    //    media/reserved entries and the end-of-chain for the root cluster
    for (uint8_t f = 0; f < numFats; f++) {
        uint32_t fatBase = fatStart + f * spf;
        if (!zeroRange(sd, fatBase, spf)) return false;
        memset(s_sector, 0, sizeof(s_sector));
        put32(&s_sector[0], 0x0FFFFFF8);   // FAT[0] = media
        put32(&s_sector[4], 0x0FFFFFFF);   // FAT[1]
        put32(&s_sector[8], 0x0FFFFFFF);   // FAT[2] = EOC (root directory)
        if (!writeSector(sd, fatBase, s_sector)) return false;
    }

    // 3. Root directory cluster (cluster 2): zero it
    *step = 6;
    if (!zeroRange(sd, rootSector, spc)) return false;

    // 4. FSInfo (partition sector 1) + backup (sector 7)
    *step = 7;
    memset(s_sector, 0, sizeof(s_sector));
    put32(&s_sector[0], 0x41615252);       // "RRaA"
    put32(&s_sector[484], 0x61417272);     // "rrAa"
    put32(&s_sector[488], totalClusters - 1);  // free clusters (root uses one)
    put32(&s_sector[492], 3);              // next free cluster hint
    put32(&s_sector[508], 0xAA550000);     // trail signature 00 00 55 AA
    if (!writeSector(sd, partitionStart + 1, s_sector)) return false;
    if (!writeSector(sd, partitionStart + 7, s_sector)) return false;

    // 5. Boot sector backup (sector 6) and primary boot sector (sector 0)
    *step = 8;
    {
        FAT32_BootSector bs;
        memset(&bs, 0, sizeof(bs));
        bs.jump[0] = 0xEB; bs.jump[1] = 0x58; bs.jump[2] = 0x90;
        memcpy(bs.oem_name, "MSDOS5.0", 8);
        bs.bytes_per_sector = 512;
        bs.sectors_per_cluster = spc;
        bs.reserved_sectors = reserved;
        bs.num_fats = numFats;
        bs.root_entries = 0;
        bs.total_sectors_16 = 0;
        bs.media_type = 0xF8;
        bs.sectors_per_fat_16 = 0;
        bs.sectors_per_track = 63;
        bs.num_heads = 255;
        bs.hidden_sectors = partitionStart;
        bs.total_sectors_32 = partitionSectors;
        bs.sectors_per_fat_32 = spf;
        bs.flags = 0;
        bs.version = 0;
        bs.root_cluster = 2;
        bs.fs_info_sector = 1;
        bs.backup_boot_sector = 6;
        bs.drive_number = 0x80;
        bs.boot_signature = 0x29;
        bs.volume_id = volumeId;
        memcpy(bs.volume_label, "NO NAME    ", 11);
        memcpy(bs.fs_type, "FAT32   ", 8);
        memset(s_sector, 0, sizeof(s_sector));
        memcpy(s_sector, &bs, sizeof(bs));
        s_sector[510] = 0x55; s_sector[511] = 0xAA;
        if (!writeSector(sd, partitionStart + 6, s_sector)) return false;
        if (!writeSector(sd, partitionStart, s_sector)) return false;
    }

    // 6. MBR (sector 0) with one FAT32-LBA partition
    *step = 9;
    {
        memset(s_sector, 0, sizeof(s_sector));
        put32(&s_sector[440], volumeId);      // disk signature
        uint8_t* pe = &s_sector[446];
        pe[0] = 0x00;                         // not bootable
        pe[1] = 0xFE; pe[2] = 0xFF; pe[3] = 0xFF;   // CHS start: use LBA
        pe[4] = 0x0C;                         // FAT32 LBA partition type
        pe[5] = 0xFE; pe[6] = 0xFF; pe[7] = 0xFF;   // CHS end: use LBA
        put32(&pe[8], partitionStart);
        put32(&pe[12], partitionSectors);
        s_sector[510] = 0x55; s_sector[511] = 0xAA;
        if (!writeSector(sd, 0, s_sector)) return false;
    }
    // 7. Read back what a PC looks at first: the partition table and the boot
    //    sector. A card that accepts writes but does not store them (or stores
    //    them somewhere else) is reported instead of showing "FORMAT OK".
    *step = 10;
    {
        static uint8_t check[512];
        if (!sd->readBlock(0, check)) return false;
        if (check[510] != 0x55 || check[511] != 0xAA || check[446 + 4] != 0x0C) return false;
        uint32_t ps = (uint32_t)check[454] | ((uint32_t)check[455] << 8) |
                      ((uint32_t)check[456] << 16) | ((uint32_t)check[457] << 24);
        if (ps != partitionStart) return false;
        if (!sd->readBlock(partitionStart, check)) return false;
        uint32_t vid = (uint32_t)check[67] | ((uint32_t)check[68] << 8) |
                       ((uint32_t)check[69] << 16) | ((uint32_t)check[70] << 24);
        if (memcmp(&check[82], "FAT32   ", 8) != 0 || vid != volumeId) return false;
    }
    *step = 0;
    return true;
}
