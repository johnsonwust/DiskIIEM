#include "FAT32.h"
#include "SDCardManager.h"
#include <string.h>
#include <ctype.h>
#include <stdio.h>

// All directory and file I/O works sector by sector with 512-byte buffers.
// (Originally three 32KB static cluster buffers = 96KB of RAM; RAM is kept
// for track data.) Without a cluster buffer, 64KB clusters (128 sectors) are
// supported too.
#define FAT32_MAX_SECTORS_PER_CLUSTER 128

// Every FAT32 instance (= every mount) gets a new generation number
static uint32_t s_generationCounter = 0;

// Constructor
FAT32::FAT32(SDCardManager* sdCardManager) {
    sdCard = sdCardManager;
    partitionStartSector = 0;
    memset(&bootSector, 0, sizeof(bootSector));
    fatStartSector = 0;
    dataStartSector = 0;
    rootDirStartSector = 0;
    bytesPerCluster = 0;
    sectorsPerCluster = 0;
    allocHint = 2;
    generation = ++s_generationCounter;
    fatCacheSector = 0;
    fatCacheValid = false;
    fsInfoInvalidated = false;
    currentDirCluster = 0;
    memset(currentPath, 0, sizeof(currentPath));
    strcpy(currentPath, "/");
    lastError = FAT32_OK;
}

// Detect filesystem type from boot sector
// Returns: 0 = unknown, 1 = FAT12, 2 = FAT16, 3 = FAT32, 4 = exFAT, 5 = NTFS
static int detectFilesystemType(uint8_t* bootSectorBuffer) {
    // Check for exFAT signature at offset 3
    if (memcmp(bootSectorBuffer + 3, "EXFAT   ", 8) == 0) {
        return 4;  // exFAT
    }
    
    // Check for NTFS signature at offset 3
    if (memcmp(bootSectorBuffer + 3, "NTFS    ", 8) == 0) {
        return 5;  // NTFS
    }
    
    // Check for valid boot sector signature (0x55AA at offset 510)
    if (bootSectorBuffer[510] != 0x55 || bootSectorBuffer[511] != 0xAA) {
        return 0;  // Unknown or unformatted
    }
    
    // Check bytes per sector (offset 11-12, little endian)
    uint16_t bytesPerSector = bootSectorBuffer[11] | (bootSectorBuffer[12] << 8);
    if (bytesPerSector != 512 && bytesPerSector != 1024 && 
        bytesPerSector != 2048 && bytesPerSector != 4096) {
        return 0;  // Invalid sector size
    }
    
    // Check for FAT32 signature at offset 82
    if (memcmp(bootSectorBuffer + 82, "FAT32   ", 8) == 0) {
        return 3;  // FAT32
    }
    
    // Check for FAT16 signature at offset 54
    if (memcmp(bootSectorBuffer + 54, "FAT16   ", 8) == 0) {
        return 2;  // FAT16
    }
    
    // Check for FAT12 signature at offset 54
    if (memcmp(bootSectorBuffer + 54, "FAT12   ", 8) == 0) {
        return 1;  // FAT12
    }
    
    // Check for FAT signature at offset 54 (generic)
    if (memcmp(bootSectorBuffer + 54, "FAT     ", 8) == 0) {
        // Determine FAT type by checking sectors_per_fat_32 (offset 36-39)
        uint32_t sectorsPerFat32 = bootSectorBuffer[36] | (bootSectorBuffer[37] << 8) |
                                   (bootSectorBuffer[38] << 16) | (bootSectorBuffer[39] << 24);
        if (sectorsPerFat32 != 0) {
            return 3;  // FAT32 (has 32-bit sectors per FAT)
        }
        return 2;  // Assume FAT16
    }
    
    // Check sectors_per_fat_32 field (offset 36-39) for FAT32 without signature
    uint32_t sectorsPerFat32 = bootSectorBuffer[36] | (bootSectorBuffer[37] << 8) |
                               (bootSectorBuffer[38] << 16) | (bootSectorBuffer[39] << 24);
    uint16_t sectorsPerFat16 = bootSectorBuffer[22] | (bootSectorBuffer[23] << 8);
    
    if (sectorsPerFat32 != 0 && sectorsPerFat16 == 0) {
        return 3;  // FAT32
    }
    
    if (sectorsPerFat16 != 0) {
        return 2;  // FAT16 or FAT12
    }
    
    return 0;  // Unknown
}

// Parse MBR partition table and find first partition
// Returns the start sector of the first partition, or 0 if no partition table
static uint32_t findFirstPartition(uint8_t* mbrBuffer) {
    // Check for valid MBR signature (0x55AA at offset 510)
    if (mbrBuffer[510] != 0x55 || mbrBuffer[511] != 0xAA) {
        return 0;  // No valid MBR
    }
    
    // Check if this is an MBR (partition table at offset 446)
    // or a VBR (Volume Boot Record - actual filesystem)
    // MBR typically has 0x00 at offset 0, VBR has jump instruction (0xEB or 0xE9)
    
    // Check for filesystem signatures that indicate this is a VBR, not MBR
    if (memcmp(mbrBuffer + 3, "MSDOS", 5) == 0 ||
        memcmp(mbrBuffer + 3, "MSWIN", 5) == 0 ||
        memcmp(mbrBuffer + 3, "mkdosfs", 7) == 0 ||
        memcmp(mbrBuffer + 3, "EXFAT", 5) == 0 ||
        memcmp(mbrBuffer + 3, "NTFS", 4) == 0 ||
        memcmp(mbrBuffer + 54, "FAT", 3) == 0 ||
        memcmp(mbrBuffer + 82, "FAT32", 5) == 0) {
        return 0;  // This is a VBR (boot sector), not MBR
    }
    
    // Parse partition table (4 entries starting at offset 446)
    for (int i = 0; i < 4; i++) {
        uint8_t* entry = mbrBuffer + 446 + (i * 16);
        
        // Partition type at offset 4
        uint8_t partType = entry[4];
        
        // Skip empty partitions
        if (partType == 0x00) {
            continue;
        }
        
        // Start LBA at offset 8 (little endian)
        uint32_t startLBA = entry[8] | (entry[9] << 8) | 
                           (entry[10] << 16) | (entry[11] << 24);
        
        // Accept FAT32, FAT16, exFAT, NTFS partition types
        // 0x01 = FAT12
        // 0x04 = FAT16 <32MB
        // 0x06 = FAT16 >32MB
        // 0x07 = NTFS/exFAT/HPFS
        // 0x0B = FAT32 (CHS)
        // 0x0C = FAT32 (LBA)
        // 0x0E = FAT16 (LBA)
        // 0x0F = Extended (LBA)
        if (partType == 0x01 || partType == 0x04 || partType == 0x06 ||
            partType == 0x07 || partType == 0x0B || partType == 0x0C ||
            partType == 0x0E) {
            return startLBA;
        }
    }
    
    return 0;  // No suitable partition found
}

// Initialize FAT32 filesystem
bool FAT32::init() {
    lastError = FAT32_OK;
    SDLockGuard guard;
    
    if (!sdCard) {
        lastError = FAT32_ERROR_NO_SDCARD;
        return false;
    }
    
    // Read sector 0 (could be MBR or VBR). The FAT cache sector doubles as
    // the work buffer here (it is not valid yet), so mounting needs neither
    // 1KB of stack nor extra static RAM.
    uint8_t* sectorBuffer = fatCache;
    fatCacheValid = false;
    memset(sectorBuffer, 0, 512);
    
    // Try multiple times to read sector 0
    bool readSuccess = false;
    for (int retry = 0; retry < 5; retry++) {
        if (sdCard->readBlock(0, sectorBuffer)) {
            readSuccess = true;
            break;
        }
        sleep_ms(100);
    }
    
    if (!readSuccess) {
        lastError = FAT32_ERROR_READ_FAILED;
        return false;
    }
    
    // Check if this is an MBR with partition table
    uint32_t partitionStart = findFirstPartition(sectorBuffer);
    uint32_t bootSectorLBA = 0;
    uint8_t* bootSectorBuffer = sectorBuffer;   // the MBR is not needed any more

    if (partitionStart > 0) {
        // This is an MBR, read the actual boot sector from partition
        bootSectorLBA = partitionStart;
        
        if (!sdCard->readBlock(partitionStart, bootSectorBuffer)) {
            lastError = FAT32_ERROR_READ_FAILED;
            return false;
        }
    } else {
        // No partition table, sector 0 is the boot sector (superfloppy format)
        bootSectorLBA = 0;
    }
    
    // Detect filesystem type
    int fsType = detectFilesystemType(bootSectorBuffer);
    
    // Handle non-FAT32 filesystems
    if (fsType == 4) {  // exFAT
        lastError = FAT32_ERROR_EXFAT;
        return false;
    }
    
    if (fsType == 5) {  // NTFS
        lastError = FAT32_ERROR_NTFS;
        return false;
    }
    
    if (fsType == 1) {  // FAT12
        lastError = FAT32_ERROR_FAT12;
        return false;
    }
    
    if (fsType == 2) {  // FAT16
        lastError = FAT32_ERROR_FAT16;
        return false;
    }
    
    if (fsType == 0) {  // Unknown
        lastError = FAT32_ERROR_UNKNOWN_FS;
        return false;
    }
    
    // At this point, fsType should be 3 (FAT32)
    memcpy(&bootSector, bootSectorBuffer, sizeof(FAT32_BootSector));
    
    // Store partition start sector for all subsequent reads
    partitionStartSector = bootSectorLBA;
    
    // Verify FAT32 parameters
    if (bootSector.bytes_per_sector != 512) {
        lastError = FAT32_ERROR_INVALID_PARAMS;
        return false;
    }
    
    // Sanity-check the geometry before anything is written with it: a bogus
    // boot sector must never lead to writes at computed (wrong) sectors.
    // Cluster size must be a power of two, at most 128 sectors (64KB).
    uint8_t spc = bootSector.sectors_per_cluster;
    if (spc == 0 || (spc & (spc - 1)) != 0 ||
        spc > FAT32_MAX_SECTORS_PER_CLUSTER ||
        bootSector.reserved_sectors == 0 ||
        bootSector.num_fats == 0 ||
        bootSector.sectors_per_fat_32 == 0 ||
        bootSector.root_cluster < 2) {
        lastError = FAT32_ERROR_INVALID_PARAMS;
        return false;
    }
    // FAT mirroring switched off with an active FAT other than the first one:
    // this driver reads FAT #1 and updates every copy, which would be wrong.
    if ((bootSector.flags & 0x0080) && (bootSector.flags & 0x000F) != 0) {
        lastError = FAT32_ERROR_INVALID_PARAMS;
        return false;
    }
    
    // Calculate FAT32 structure (all sectors are relative to partition start)
    sectorsPerCluster = bootSector.sectors_per_cluster;
    bytesPerCluster = bootSector.bytes_per_sector * sectorsPerCluster;
    
    // FAT start sector (relative to partition start)
    fatStartSector = bootSector.reserved_sectors;
    
    // Data start sector = reserved + (num_fats * sectors_per_fat)
    dataStartSector = bootSector.reserved_sectors + 
                     (bootSector.num_fats * bootSector.sectors_per_fat_32);
    
    // The root directory must lie inside the volume
    if (!isValidCluster(bootSector.root_cluster)) {
        lastError = FAT32_ERROR_INVALID_PARAMS;
        return false;
    }

    // Root directory starts at root_cluster
    rootDirStartSector = getClusterSector(bootSector.root_cluster);
    
    // Initialize current directory to root
    currentDirCluster = bootSector.root_cluster;
    strcpy(currentPath, "/");
    
    return true;
}

// Get sector number for a cluster (includes partition offset)
uint32_t FAT32::getClusterSector(uint32_t cluster) {
    // First data cluster is 2, so subtract 2
    // Add partition start sector for absolute addressing
    return partitionStartSector + dataStartSector + ((cluster - 2) * sectorsPerCluster);
}

// Read FAT entry for a cluster (FAT #1, through a one-sector cache)
uint32_t FAT32::readFATEntry(uint32_t cluster) {
    if (!sdCard || !isValidCluster(cluster)) {
        return FAT32_CLUSTER_EOF_MIN;  // invalid cluster ends any chain walk
    }
    SDLockGuard guard;

    uint32_t fatOffset = cluster * 4;
    uint32_t fatSector = partitionStartSector + fatStartSector + (fatOffset / 512);
    uint32_t fatEntryOffset = fatOffset % 512;

    if (!fatCacheValid || fatCacheSector != fatSector) {
        if (!sdCard->readBlock(fatSector, fatCache)) {
            fatCacheValid = false;
            return FAT32_CLUSTER_EOF_MIN;
        }
        fatCacheSector = fatSector;
        fatCacheValid = true;
    }

    uint32_t nextCluster = (uint32_t)fatCache[fatEntryOffset] |
                           ((uint32_t)fatCache[fatEntryOffset + 1] << 8) |
                           ((uint32_t)fatCache[fatEntryOffset + 2] << 16) |
                           ((uint32_t)fatCache[fatEntryOffset + 3] << 24);
    return nextCluster & 0x0FFFFFFF;
}

// A data cluster number that lies inside this volume
bool FAT32::isValidCluster(uint32_t cluster) const {
    return cluster >= FAT32_CLUSTER_RESERVED_MIN && cluster < clusterLimit();
}

// Guarded write into the FAT area (all FAT copies) of this volume
bool FAT32::writeFatSector(uint32_t sector, const uint8_t* buffer) {
    uint32_t lo = partitionStartSector + fatStartSector;
    uint32_t hi = partitionStartSector + dataStartSector;
    if (!sdCard || sector < lo || sector >= hi) {
        return false;
    }
    fatCacheValid = false;
    return sdCard->writeBlock(sector, buffer);
}

// Guarded write into the data area (clusters) of this volume
bool FAT32::writeDataSector(uint32_t sector, const uint8_t* buffer) {
    uint32_t totalSectors = bootSector.total_sectors_32 ? bootSector.total_sectors_32 : bootSector.total_sectors_16;
    uint32_t lo = partitionStartSector + dataStartSector;
    uint32_t hi = partitionStartSector + totalSectors;
    if (!sdCard || sector < lo || sector >= hi) {
        return false;
    }
    return sdCard->writeBlock(sector, buffer);
}

static uint32_t le32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void put_le32(uint8_t* p, uint32_t v) {
    p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; p[2] = (v >> 16) & 0xFF; p[3] = (v >> 24) & 0xFF;
}

// Mark the FSInfo free-cluster count as unknown (once per mount). After we
// allocate clusters the stored count is wrong; "unknown" makes the PC recount
// instead of reporting a damaged volume. Only writes the validated FSInfo sector.
void FAT32::invalidateFsInfo(uint32_t nextFreeHint) {
    if (fsInfoInvalidated || !sdCard) return;
    fsInfoInvalidated = true;
    uint16_t fsInfo = bootSector.fs_info_sector;
    if (fsInfo == 0 || fsInfo == 0xFFFF || fsInfo >= bootSector.reserved_sectors) return;
    static uint8_t buf[512];
    uint32_t sector = partitionStartSector + fsInfo;
    if (!sdCard->readBlock(sector, buf)) return;
    if (le32(&buf[0]) != 0x41615252 || le32(&buf[484]) != 0x61417272) return;
    put_le32(&buf[488], 0xFFFFFFFF);
    put_le32(&buf[492], nextFreeHint);
    sdCard->writeBlock(sector, buf);
}

// Walk a directory sector by sector. Long file names are assembled while
// walking (their entries may span sector and cluster boundaries) and handed to
// the visitor together with the short entry. Deleted entries and volume labels
// are skipped. The visitor returns false to stop.
bool FAT32::forEachDirEntry(uint32_t dirCluster, DirVisitor visit, void* ctx,
                            uint32_t* sector, int* index, bool* hasLFN) {
    if (!sdCard || !visit) return false;
    SDLockGuard guard;

    // Static: only used with the SD lock held, and visitors do no SD I/O
    static uint8_t sec[512];
    static char lfn[256];
    // Byte offsets of the 13 UTF-16 characters inside an LFN entry
    static const uint8_t lfnCharOffsets[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};

    bool lfnOk = false;
    int lfnNext = 0;
    uint8_t lfnSum = 0;
    uint32_t lfnEnd = 0;
    uint32_t cl = dirCluster;
    uint32_t chainGuard = 0;

    while (isValidCluster(cl)) {
        uint32_t first = getClusterSector(cl);
        for (uint32_t s = 0; s < sectorsPerCluster; s++) {
            if (!sdCard->readBlock(first + s, sec)) return false;
            for (int i = 0; i < 512 / 32; i++) {
                const FAT32_DirEntry* e = (const FAT32_DirEntry*)(sec + i * 32);
                uint8_t c0 = e->name[0];
                if (c0 == 0x00) return true;            // end of directory
                if (c0 == 0xE5) { lfnOk = false; continue; }  // deleted

                if (e->attributes == FAT32_ATTR_LONG_NAME) {
                    const uint8_t* raw = (const uint8_t*)e;
                    int seq = raw[0] & 0x1F;
                    uint8_t sum = raw[13];
                    if (raw[0] & 0x40) {                 // first physical = last part
                        if (seq < 1 || seq > 20) { lfnOk = false; continue; }
                        lfnOk = true;
                        lfnNext = seq;
                        lfnSum = sum;
                        lfnEnd = (uint32_t)seq * 13;
                        if (lfnEnd > sizeof(lfn) - 1) lfnEnd = sizeof(lfn) - 1;
                        memset(lfn, 0, sizeof(lfn));
                    }
                    if (!lfnOk || seq != lfnNext || sum != lfnSum) { lfnOk = false; continue; }
                    for (int k = 0; k < 13; k++) {
                        uint16_t u = raw[lfnCharOffsets[k]] | (raw[lfnCharOffsets[k] + 1] << 8);
                        uint32_t pos = (uint32_t)(seq - 1) * 13 + k;
                        if (u == 0x0000) { if (pos < lfnEnd) lfnEnd = pos; break; }
                        if (u == 0xFFFF) break;
                        // Only ASCII can be shown/compared; anything else becomes '?'
                        if (pos < sizeof(lfn) - 1) lfn[pos] = (u >= 0x20 && u < 0x7F) ? (char)u : '?';
                    }
                    lfnNext--;
                    continue;
                }

                const char* longName = nullptr;
                if (lfnOk && lfnNext == 0 && calculateLFNChecksum(e->name) == lfnSum) {
                    lfn[lfnEnd] = 0;
                    longName = lfn;
                }
                lfnOk = false;
                if (e->attributes & FAT32_ATTR_VOLUME_ID) continue;
                if (!visit(ctx, e, longName)) {
                    if (sector) *sector = first + s;
                    if (index) *index = i;
                    if (hasLFN) *hasLFN = longName != nullptr;
                    return true;
                }
            }
        }
        cl = readFATEntry(cl);
        // A looping or garbage chain would otherwise be followed for up to
        // 65536 clusters - many minutes of reading with the UI frozen
        if (++chainGuard > maxDirClusters()) return false;
    }
    return true;
}

static bool equalsIgnoreCase(const char* a, const char* b) {
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
        a++; b++;
    }
    return *a == *b;
}

// True if `name` is a FAT 8.3 short name as it stands ("GAME.DSK",
// "PRINCE~1.DSK"): 1-8 characters, optionally a dot and 1-3 characters.
static bool isShortName(const char* name) {
    int base = 0;
    while (name[base] && name[base] != '.') {
        if (name[base] == ' ') return false;
        base++;
    }
    if (base < 1 || base > 8) return false;
    if (name[base] == 0) return true;
    const char* ext = name + base + 1;
    int n = 0;
    while (ext[n]) {
        if (ext[n] == '.' || ext[n] == ' ') return false;
        n++;
    }
    return n >= 1 && n <= 3;
}

struct FindCtx {
    const char* name;
    char name83[11];
    bool use83;        // compare short names only if `name` is one
    FAT32_DirEntry* out;
    bool found;
};

static bool findVisitor(void* p, const FAT32_DirEntry* e, const char* longName) {
    FindCtx* c = (FindCtx*)p;
    if ((c->use83 && memcmp(e->name, c->name83, 11) == 0) ||
        (longName && equalsIgnoreCase(longName, c->name))) {
        memcpy(c->out, e, sizeof(FAT32_DirEntry));
        c->found = true;
        return false;
    }
    return true;
}

// Find file in directory (supports both 8.3 and LFN)
bool FAT32::findFileInDirectory(uint32_t dirCluster, const char* filename, FAT32_DirEntry* entry) {
    if (!sdCard || !entry || !filename || !filename[0]) {
        return false;
    }
    FindCtx c;
    c.name = filename;
    format83Name(filename, c.name83);
    // A long name must not be cut to 8.3 for the comparison: looking for
    // "KARATEKA SIDE B.DSK" matched the file "KARATEKA.DSK" (same first 8
    // characters) when that came first in the directory - the wrong image
    // was loaded and later written to.
    c.use83 = isShortName(filename);
    c.out = entry;
    c.found = false;
    forEachDirEntry(dirCluster, findVisitor, &c);
    return c.found;
}


// Highest valid cluster number + 1 (data clusters are numbered from 2)
uint32_t FAT32::clusterLimit() const {
    uint32_t totalSectors = bootSector.total_sectors_32 ? bootSector.total_sectors_32 : bootSector.total_sectors_16;
    if (totalSectors <= dataStartSector || sectorsPerCluster == 0) return 2;
    uint32_t limit = (totalSectors - dataStartSector) / sectorsPerCluster + 2;
    // Never run past the end of the FAT itself
    uint32_t fatEntries = bootSector.sectors_per_fat_32 * (512 / 4);
    if (limit > fatEntries) limit = fatEntries;
    if (limit > FAT32_CLUSTER_RESERVED_MAX + 1) limit = FAT32_CLUSTER_RESERVED_MAX + 1;
    return limit;
}

// Write a FAT entry (4 bytes, FAT32) to EVERY FAT copy.
// (Only FAT #1 was updated before, leaving FAT #2 inconsistent: Windows/chkdsk
// reports the card as damaged and may "repair" it by dropping the file.)
bool FAT32::writeFATEntry(uint32_t cluster, uint32_t value) {
    if (!sdCard || !isValidCluster(cluster)) return false;
    SDLockGuard guard;
    uint32_t fatOffset = cluster * 4;
    uint32_t fatEntryOffset = fatOffset % 512;
    uint8_t numFats = bootSector.num_fats ? bootSector.num_fats : 1;
    static uint8_t fatBuffer[512];
    for (uint8_t f = 0; f < numFats; f++) {
        uint32_t fatSector = partitionStartSector + fatStartSector +
                             f * bootSector.sectors_per_fat_32 + (fatOffset / 512);
        if (!sdCard->readBlock(fatSector, fatBuffer)) return false;
        // Keep the 4 reserved high bits of the existing entry
        uint8_t hi = fatBuffer[fatEntryOffset + 3] & 0xF0;
        fatBuffer[fatEntryOffset] = value & 0xFF;
        fatBuffer[fatEntryOffset + 1] = (value >> 8) & 0xFF;
        fatBuffer[fatEntryOffset + 2] = (value >> 16) & 0xFF;
        fatBuffer[fatEntryOffset + 3] = hi | ((value >> 24) & 0x0F);
        if (!writeFatSector(fatSector, fatBuffer)) return false;
    }
    return true;
}

// Allocate a free cluster and mark it end-of-chain (returns 0 on failure).
// Scans the FAT one sector (128 entries) per read instead of one read per cluster.
uint32_t FAT32::allocateCluster() {
    if (!sdCard) return 0;
    SDLockGuard guard;
    uint32_t limit = clusterLimit();
    if (limit <= 2) return 0;
    if (allocHint < 2 || allocHint >= limit) allocHint = 2;

    static uint8_t fatBuffer[512];
    uint32_t bufferedSector = 0xFFFFFFFF;
    uint32_t c = allocHint;
    for (uint32_t n = 2; n < limit; n++) {
        uint32_t sec = (c * 4) / 512;
        if (sec != bufferedSector) {
            if (!sdCard->readBlock(partitionStartSector + fatStartSector + sec, fatBuffer)) return 0;
            bufferedSector = sec;
        }
        uint32_t off = (c * 4) % 512;
        uint32_t v = ((uint32_t)fatBuffer[off] | ((uint32_t)fatBuffer[off + 1] << 8) |
                      ((uint32_t)fatBuffer[off + 2] << 16) | ((uint32_t)fatBuffer[off + 3] << 24)) & 0x0FFFFFFF;
        if (v == FAT32_CLUSTER_FREE) {
            if (!writeFATEntry(c, FAT32_CLUSTER_EOF_MAX)) return 0;
            allocHint = c + 1;
            invalidateFsInfo(allocHint);
            return c;
        }
        c++;
        if (c >= limit) c = 2;  // wrap around
    }
    return 0;  // Disk full
}

// Fill a cluster with zeros (required for new directory clusters)
bool FAT32::zeroCluster(uint32_t cluster) {
    if (!sdCard) return false;
    SDLockGuard guard;
    static const uint8_t zeros[512] = {0};
    if (!isValidCluster(cluster)) return false;
    uint32_t first = getClusterSector(cluster);
    for (uint32_t i = 0; i < sectorsPerCluster; i++) {
        if (!writeDataSector(first + i, zeros)) return false;
    }
    return true;
}

// FAT date/time stamp for files we write (no RTC): 2026-01-01 12:00:00.
// A zero date (month 0 / day 0) is invalid and confuses some PC tools.
static const uint16_t FAT_STAMP_DATE = ((2026 - 1980) << 9) | (1 << 5) | 1;
static const uint16_t FAT_STAMP_TIME = (12 << 11);

// Create or update a directory entry in directory `dirCluster`.
// Walks the whole cluster chain of the directory (only the first cluster was
// searched before, so the entry was duplicated, or "directory full" was
// returned once the directory grew past one cluster) and extends the
// directory with a new cluster when there is no free slot.
bool FAT32::writeDirectoryEntry(uint32_t dirCluster, const char* filename, uint32_t cluster, uint32_t fileSize, bool replace) {
    if (!sdCard) return false;
    SDLockGuard guard;

    char name83[11];
    format83Name(filename, name83);

    static uint8_t sectorBuf[512];
    const int entriesPerSector = 512 / 32;
    uint32_t freeSector = 0;
    int freeIndex = -1;
    uint32_t lastCluster = dirCluster;
    uint32_t cur = dirCluster;
    bool endOfDir = false;
    int chainGuard = 0;

    while (!endOfDir && isValidCluster(cur)) {
        lastCluster = cur;
        uint32_t startSector = getClusterSector(cur);
        for (uint32_t s = 0; s < sectorsPerCluster && !endOfDir; s++) {
            if (!sdCard->readBlock(startSector + s, sectorBuf)) return false;
            FAT32_DirEntry* entries = (FAT32_DirEntry*)sectorBuf;
            for (int i = 0; i < entriesPerSector; i++) {
                uint8_t first = entries[i].name[0];
                if (first == 0x00) {
                    if (freeIndex < 0) { freeSector = startSector + s; freeIndex = i; }
                    endOfDir = true;
                    break;
                }
                if (first == 0xE5) {
                    if (freeIndex < 0) { freeSector = startSector + s; freeIndex = i; }
                    continue;
                }
                if (entries[i].attributes == FAT32_ATTR_LONG_NAME) continue;
                if (entries[i].attributes & FAT32_ATTR_VOLUME_ID) continue;
                if (memcmp(entries[i].name, name83, 11) == 0) {
                    if (!replace || (entries[i].attributes & FAT32_ATTR_DIRECTORY)) return false;
                    // Update the existing entry in place (keeps its LFN entries)
                    entries[i].cluster_low = cluster & 0xFFFF;
                    entries[i].cluster_high = (cluster >> 16) & 0xFFFF;
                    entries[i].file_size = fileSize;
                    entries[i].attributes |= FAT32_ATTR_ARCHIVE;
                    entries[i].modification_date = FAT_STAMP_DATE;
                    entries[i].modification_time = FAT_STAMP_TIME;
                    entries[i].last_access_date = FAT_STAMP_DATE;
                    return writeDataSector(startSector + s, sectorBuf);
                }
            }
        }
        if (endOfDir) break;
        uint32_t next = readFATEntry(cur);
        if (next < FAT32_CLUSTER_RESERVED_MIN || next >= FAT32_CLUSTER_EOF_MIN) break;
        cur = next;
        if (++chainGuard > (int)maxDirClusters()) return false;  // corrupt / looping chain
    }

    if (freeIndex < 0) {
        // Directory is full: append a zeroed cluster to its chain
        uint32_t newCluster = allocateCluster();
        if (newCluster == 0) return false;
        if (!zeroCluster(newCluster)) return false;
        if (!isValidCluster(lastCluster) || !writeFATEntry(lastCluster, newCluster)) return false;
        freeSector = getClusterSector(newCluster);
        freeIndex = 0;
    }

    if (!sdCard->readBlock(freeSector, sectorBuf)) return false;
    FAT32_DirEntry* entries = (FAT32_DirEntry*)sectorBuf;
    memset(&entries[freeIndex], 0, sizeof(FAT32_DirEntry));
    memcpy(entries[freeIndex].name, name83, 11);
    entries[freeIndex].attributes = FAT32_ATTR_ARCHIVE;
    entries[freeIndex].cluster_low = cluster & 0xFFFF;
    entries[freeIndex].cluster_high = (cluster >> 16) & 0xFFFF;
    entries[freeIndex].file_size = fileSize;
    entries[freeIndex].creation_date = FAT_STAMP_DATE;
    entries[freeIndex].creation_time = FAT_STAMP_TIME;
    entries[freeIndex].modification_date = FAT_STAMP_DATE;
    entries[freeIndex].modification_time = FAT_STAMP_TIME;
    entries[freeIndex].last_access_date = FAT_STAMP_DATE;
    return writeDataSector(freeSector, sectorBuf);
}

// Set or clear the read-only attribute of the file behind `file`. The entry is
// looked up by name in the file's directory and must describe the same file
// (first cluster and size); only its attribute byte is rewritten.
bool FAT32::setReadOnly(const FAT32_File& file, const char* name, bool readOnly) {
    SDLockGuard guard;
    if (!sdCard || !name || !name[0] || !file.valid || file.generation != generation) return false;
    FAT32_DirEntry entry;
    FindCtx c;
    c.name = name;
    format83Name(name, c.name83);
    c.use83 = isShortName(name);
    c.out = &entry;
    c.found = false;
    uint32_t sector = 0;
    int index = -1;
    if (!forEachDirEntry(file.dirCluster, findVisitor, &c, &sector, &index, nullptr) || !c.found || index < 0) return false;
    if ((entry.attributes & (FAT32_ATTR_DIRECTORY | FAT32_ATTR_VOLUME_ID)) ||
        entry.file_size != file.size ||
        (entry.cluster_low | ((uint32_t)entry.cluster_high << 16)) != file.firstCluster) return false;
    static uint8_t buffer[512];
    if (!sdCard->readBlock(sector, buffer)) return false;
    uint8_t* e = buffer + index * sizeof(FAT32_DirEntry);
    if (memcmp(e, entry.name, 11) != 0) return false;        // not the entry we found
    uint8_t attr = e[11];
    uint8_t want = readOnly ? (uint8_t)(attr | FAT32_ATTR_READ_ONLY) : (uint8_t)(attr & ~FAT32_ATTR_READ_ONLY);
    if (want == attr) return true;
    e[11] = want;
    return writeDataSector(sector, buffer);
}

// Create a new 8.3 file. Publish its directory entry after the data is written.
static void fillFromMemory(void* ctx, uint32_t offset, uint8_t* buf, uint32_t bytes) {
    memcpy(buf, static_cast<const uint8_t*>(ctx) + offset, bytes);
}

bool FAT32::createImageFile(const char* filename, const uint8_t* data, uint32_t size) {
    if (!data) return false;
    return createImageFile(filename, size, fillFromMemory, const_cast<uint8_t*>(data));
}

bool FAT32::createImageFile(const char* filename, uint32_t size, FileFill fill, void* ctx) {
    if (!sdCard || !filename || !isShortName(filename) || !fill || !size) return false;
    if (strpbrk(filename, R"(" *+,/:;<=>?[\]|)")) return false;
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(filename); *p; ++p) {
        if (*p < 0x21 || *p > 0x7E) return false;
    }
    SDLockGuard guard;
    FAT32_DirEntry existing;
    if (findFileInDirectory(currentDirCluster, filename, &existing)) return false;

    uint32_t first = 0;
    uint32_t last = 0;
    uint32_t offset = 0;
    static uint8_t tail[512];
    while (offset < size) {
        uint32_t cluster = allocateCluster();
        if (!cluster) break;
        if (last && !writeFATEntry(last, cluster)) {
            writeFATEntry(cluster, FAT32_CLUSTER_FREE);
            break;
        }
        if (!first) first = cluster;
        last = cluster;
        uint32_t sector = getClusterSector(cluster);
        bool ok = true;
        for (uint32_t i = 0; i < sectorsPerCluster && offset < size; ++i) {
            uint32_t bytes = size - offset;
            if (bytes > 512) bytes = 512;
            memset(tail, 0, sizeof(tail));
            fill(ctx, offset, tail, bytes);
            if (!(ok = writeDataSector(sector + i, tail))) break;
            offset += bytes;
        }
        if (!ok) break;
    }
    if (offset == size && writeDirectoryEntry(currentDirCluster, filename, first, size, false)) return true;

    // Best effort if the card was removed or returned an I/O error.
    while (isValidCluster(first)) {
        uint32_t next = readFATEntry(first);
        if (!writeFATEntry(first, FAT32_CLUSTER_FREE)) break;
        first = next;
    }
    return false;
}

// Create/overwrite a small file (<= 512 bytes, e.g. config.cfg) in the ROOT directory.
// Previously the file was written into whatever directory the user had browsed
// to (so CONFIG.CFG ended up in sub folders and was never read back at boot),
// and an existing empty file (start cluster 0) made it write to a bogus sector
// computed from cluster 0.
bool FAT32::writeSmallFile(const char* filename, const uint8_t* data, uint32_t size) {
    if (!sdCard || !filename || !data || size == 0) return false;
    if (size > 512) return false;
    SDLockGuard guard;

    uint32_t rootCluster = bootSector.root_cluster;
    uint32_t cluster = 0;
    FAT32_DirEntry entry;
    if (findFileInDirectory(rootCluster, filename, &entry) &&
        !(entry.attributes & FAT32_ATTR_DIRECTORY)) {
        cluster = entry.cluster_low | ((uint32_t)entry.cluster_high << 16);
    }
    if (!isValidCluster(cluster)) {
        // New file, or existing file without a data cluster
        cluster = allocateCluster();
        if (cluster == 0) return false;
    }

    static uint8_t sectorBuf[512];
    memset(sectorBuf, 0, 512);
    memcpy(sectorBuf, data, size);
    if (!writeDataSector(getClusterSector(cluster), sectorBuf)) return false;

    return writeDirectoryEntry(rootCluster, filename, cluster, size);
}

// Read a small file from the ROOT directory
bool FAT32::readSmallFile(const char* filename, uint8_t* buffer, uint32_t maxSize, uint32_t* bytesRead) {
    if (bytesRead) *bytesRead = 0;
    if (!sdCard || !filename || !buffer) return false;
    SDLockGuard guard;
    FAT32_DirEntry entry;
    if (!findFileInDirectory(bootSector.root_cluster, filename, &entry)) return false;
    if (entry.attributes & FAT32_ATTR_DIRECTORY) return false;
    return readFileFromEntry(&entry, buffer, maxSize, bytesRead);
}

// Enter the directory part of a path and return the file-name part
const char* FAT32::enterPathDirectory(const char* path) {
    if (!path) return nullptr;
    SDLockGuard guard;
    const char* p = path;
    if (*p == '/' || *p == '\\') {
        changeDirectory("/");
        while (*p == '/' || *p == '\\') p++;
    }
    char component[64];
    while (true) {
        const char* sep = p;
        while (*sep && *sep != '/' && *sep != '\\') sep++;
        if (*sep == 0) {
            return p;  // last component = file name
        }
        size_t len = (size_t)(sep - p);
        if (len > 0) {
            if (len >= sizeof(component)) return nullptr;
            memcpy(component, p, len);
            component[len] = 0;
            if (!changeDirectory(component)) return nullptr;
        }
        p = sep + 1;
    }
}

// Format filename to 8.3 format
void FAT32::format83Name(const char* filename, char* name83) {
    memset(name83, ' ', 11);
    
    const char* dot = strchr(filename, '.');
    const char* nameStart = filename;
    
    // Skip leading path separators
    while (*nameStart == '/' || *nameStart == '\\') {
        nameStart++;
    }
    
    // Extract base name (up to 8 characters)
    int nameLen = 0;
    if (dot) {
        nameLen = dot - nameStart;
    } else {
        nameLen = strlen(nameStart);
    }
    
    if (nameLen > 8) nameLen = 8;
    
    for (int i = 0; i < nameLen; i++) {
        name83[i] = toupper(nameStart[i]);
    }
    
    // Extract extension (3 characters)
    if (dot) {
        const char* ext = dot + 1;
        int extLen = strlen(ext);
        if (extLen > 3) extLen = 3;
        
        for (int i = 0; i < extLen; i++) {
            name83[8 + i] = toupper(ext[i]);
        }
    }
}


// Calculate LFN checksum from 8.3 name
uint8_t FAT32::calculateLFNChecksum(const uint8_t* shortName) {
    uint8_t sum = 0;
    for (int i = 0; i < 11; i++) {
        sum = ((sum & 1) ? 0x80 : 0) + (sum >> 1) + shortName[i];
    }
    return sum;
}


// Find file in current directory
bool FAT32::findFile(const char* filename, FAT32_DirEntry* entry) {
    SDLockGuard guard;
    return findFileInDirectory(currentDirCluster, filename, entry);
}


// ".." entry of a directory -> cluster of the parent (0 = root directory)
struct ParentCtx {
    uint32_t cluster;
    bool found;
};

static bool parentVisitor(void* p, const FAT32_DirEntry* e, const char* longName) {
    (void)longName;
    ParentCtx* c = (ParentCtx*)p;
    if (e->name[0] == '.' && e->name[1] == '.' && e->name[2] == ' ' &&
        (e->attributes & FAT32_ATTR_DIRECTORY)) {
        c->cluster = e->cluster_low | ((uint32_t)e->cluster_high << 16);
        c->found = true;
        return false;
    }
    return true;
}

// Change directory
bool FAT32::changeDirectory(const char* dirname) {
    if (!sdCard || !dirname) {
        return false;
    }
    SDLockGuard guard;
    const uint32_t rootCluster = bootSector.root_cluster;

    // Handle special cases
    if (strcmp(dirname, "/") == 0 || strcmp(dirname, "\\") == 0) {
        // Go to root
        currentDirCluster = rootCluster;
        strcpy(currentPath, "/");
        return true;
    }

    if (strcmp(dirname, "..") == 0) {
        // Go to the parent directory (it used to jump straight to the root)
        if (currentDirCluster == rootCluster) {
            return true;  // Already at root
        }
        ParentCtx pc;
        pc.cluster = 0;
        pc.found = false;
        forEachDirEntry(currentDirCluster, parentVisitor, &pc);
        uint32_t parent = (pc.found && pc.cluster != 0) ? pc.cluster : rootCluster;
        if (!isValidCluster(parent)) {
            parent = rootCluster;
        }
        currentDirCluster = parent;
        char* slash = strrchr(currentPath, '/');
        if (parent == rootCluster || slash == nullptr || slash == currentPath) {
            strcpy(currentPath, "/");
        } else {
            *slash = 0;   // drop the last path component
        }
        return true;
    }

    // Find directory entry
    FAT32_DirEntry entry;
    if (!findFileInDirectory(currentDirCluster, dirname, &entry)) {
        return false;
    }

    // Check if it's a directory
    if (!(entry.attributes & FAT32_ATTR_DIRECTORY)) {
        return false;  // Not a directory
    }

    // Get directory cluster
    uint32_t dirCluster = entry.cluster_low | ((uint32_t)entry.cluster_high << 16);
    if (dirCluster == 0) {
        dirCluster = rootCluster;
    }
    if (!isValidCluster(dirCluster)) {
        return false;  // damaged entry: do not walk into it
    }

    // The path must fit (it is stored in config.cfg as LAST IMAGE)
    const bool atRoot = (strcmp(currentPath, "/") == 0);
    if (strlen(currentPath) + (atRoot ? 0 : 1) + strlen(dirname) >= sizeof(currentPath)) {
        return false;
    }

    // Update current directory and path
    currentDirCluster = dirCluster;
    if (!atRoot) {
        strcat(currentPath, "/");
    }
    strcat(currentPath, dirname);

    return true;
}

// Get current directory path
bool FAT32::getCurrentDirectory(char* path, uint32_t maxSize) {
    if (!path || maxSize == 0) {
        return false;
    }
    
    strncpy(path, currentPath, maxSize - 1);
    path[maxSize - 1] = 0;
    return true;
}


// Check if file exists
bool FAT32::fileExists(const char* filename) {
    FAT32_DirEntry entry;
    return findFile(filename, &entry);
}


// Get volume label (returns static string)
const char* FAT32::getVolumeLabel() const {
    static char label[12];
    memcpy(label, bootSector.volume_label, 11);
    label[11] = '\0';
    // Trim trailing spaces
    for (int i = 10; i >= 0 && label[i] == ' '; i--) {
        label[i] = '\0';
    }
    return label;
}

// Get total size in MB
uint32_t FAT32::getTotalSizeMB() const {
    uint32_t totalSectors = bootSector.total_sectors_32;
    if (totalSectors == 0) {
        totalSectors = bootSector.total_sectors_16;
    }
    // Calculate MB: sectors * 512 / (1024 * 1024) = sectors / 2048
    return totalSectors / 2048;
}

// Read file (current directory)
bool FAT32::readFile(const char* filename, uint8_t* buffer, uint32_t maxSize, uint32_t* bytesRead) {
    if (bytesRead) {
        *bytesRead = 0;
    }
    if (!sdCard || !buffer) {
        return false;
    }
    SDLockGuard guard;

    FAT32_DirEntry entry;
    if (!findFile(filename, &entry)) {
        return false;
    }
    return readFileFromEntry(&entry, buffer, maxSize, bytesRead);
}

// Read up to maxSize bytes of the file described by a directory entry
bool FAT32::readFileFromEntry(const FAT32_DirEntry* entry, uint8_t* buffer, uint32_t maxSize, uint32_t* bytesRead) {
    if (bytesRead) {
        *bytesRead = 0;
    }
    if (!sdCard || !entry || !buffer) {
        return false;
    }
    SDLockGuard guard;
    FAT32_File f;
    if (!openFileFromEntry(entry, 0, &f) || f.size == 0) {
        return false;
    }
    uint32_t n = (f.size < maxSize) ? f.size : maxSize;
    uint32_t done = 0;
    bool ok = fileIO(&f, 0, buffer, nullptr, n, &done);
    if (bytesRead) {
        *bytesRead = done;
    }
    return ok && done > 0;
}

// Build a handle from a directory entry; validates the whole cluster chain
bool FAT32::openFileFromEntry(const FAT32_DirEntry* entry, uint32_t dirCluster, FAT32_File* file) {
    if (!file) return false;
    memset(file, 0, sizeof(*file));
    if (!sdCard || !entry) return false;
    if (entry->attributes & (FAT32_ATTR_DIRECTORY | FAT32_ATTR_VOLUME_ID)) return false;
    SDLockGuard guard;

    uint32_t first = entry->cluster_low | ((uint32_t)entry->cluster_high << 16);
    file->firstCluster = first;
    file->size = entry->file_size;
    file->readOnly = (entry->attributes & FAT32_ATTR_READ_ONLY) != 0;
    file->dirCluster = dirCluster;
    file->generation = generation;
    file->contiguous = true;

    if (file->size == 0) {
        file->valid = true;
        return true;
    }
    if (!isValidCluster(first)) {
        return false;
    }
    // The chain must cover the whole file and stay inside the volume;
    // otherwise refuse the file instead of reading/writing wrong sectors.
    uint32_t needed = (file->size + bytesPerCluster - 1) / bytesPerCluster;
    uint32_t cl = first;
    for (uint32_t i = 1; i < needed; i++) {
        uint32_t next = readFATEntry(cl);
        if (!isValidCluster(next)) {
            return false;
        }
        if (next != cl + 1) {
            file->contiguous = false;
        }
        cl = next;
    }
    file->valid = true;
    return true;
}

bool FAT32::openFile(const char* filename, uint32_t dirCluster, FAT32_File* file) {
    if (!file) return false;
    file->valid = false;
    if (!sdCard || !filename) return false;
    SDLockGuard guard;
    uint32_t dir = resolveDir(dirCluster);
    FAT32_DirEntry entry;
    if (!findFileInDirectory(dir, filename, &entry)) {
        return false;
    }
    return openFileFromEntry(&entry, dir, file);
}

// Cluster number holding cluster #clusterIndex of the file
uint32_t FAT32::fileClusterAt(const FAT32_File* file, uint32_t clusterIndex) {
    if (file->contiguous) {
        return file->firstCluster + clusterIndex;
    }
    uint32_t cl = file->firstCluster;
    for (uint32_t i = 0; i < clusterIndex && isValidCluster(cl); i++) {
        cl = readFATEntry(cl);
    }
    return cl;
}

// Common read/write loop. Whole aligned sectors go straight to/from the
// caller's buffer; partial sectors are read-modify-written.
bool FAT32::fileIO(const FAT32_File* file, uint32_t offset, uint8_t* readBuf, const uint8_t* writeBuf,
                   uint32_t size, uint32_t* done) {
    if (done) *done = 0;
    if (!sdCard || !file || !file->valid || file->generation != generation) {
        return false;  // stale handle (card remounted/formatted) or never opened
    }
    if ((readBuf == nullptr) == (writeBuf == nullptr)) return false;
    if (size == 0) return true;
    if (offset >= file->size) return false;
    if (size > file->size - offset) {
        if (writeBuf) return false;  // writes never grow a file
        size = file->size - offset;
    }
    SDLockGuard guard;

    static uint8_t tmp[512];
    uint32_t cl = fileClusterAt(file, offset / bytesPerCluster);
    uint32_t pos = 0;
    while (pos < size) {
        if (!isValidCluster(cl)) return false;
        uint32_t inCluster = (offset + pos) % bytesPerCluster;
        uint32_t sector = getClusterSector(cl) + inCluster / 512;
        uint32_t inSector = inCluster % 512;
        uint32_t chunk = 512 - inSector;
        if (chunk > size - pos) chunk = size - pos;

        if (readBuf) {
            if (inSector == 0 && chunk == 512) {
                if (!sdCard->readBlock(sector, readBuf + pos)) return false;
            } else {
                if (!sdCard->readBlock(sector, tmp)) return false;
                memcpy(readBuf + pos, tmp + inSector, chunk);
            }
        } else {
            if (inSector == 0 && chunk == 512) {
                if (!writeDataSector(sector, writeBuf + pos)) return false;
            } else {
                if (!sdCard->readBlock(sector, tmp)) return false;
                memcpy(tmp + inSector, writeBuf + pos, chunk);
                if (!writeDataSector(sector, tmp)) return false;
            }
        }
        pos += chunk;
        if (done) *done = pos;
        if (pos < size && ((offset + pos) % bytesPerCluster) == 0) {
            cl = file->contiguous ? cl + 1 : readFATEntry(cl);
        }
    }
    return true;
}

bool FAT32::fileRead(const FAT32_File* file, uint32_t offset, uint8_t* buffer, uint32_t size, uint32_t* bytesRead) {
    uint32_t done = 0;
    bool ok = buffer && fileIO(file, offset, buffer, nullptr, size, &done);
    if (bytesRead) *bytesRead = done;
    return ok && done > 0;
}

bool FAT32::fileWrite(const FAT32_File* file, uint32_t offset, const uint8_t* buffer, uint32_t size) {
    return buffer && fileIO(file, offset, nullptr, buffer, size, nullptr);
}


// Case-insensitive string comparison for sorting
static int strcasecmp_sort(const char* s1, const char* s2) {
    while (*s1 && *s2) {
        char c1 = (*s1 >= 'A' && *s1 <= 'Z') ? (*s1 + 32) : *s1;
        char c2 = (*s2 >= 'A' && *s2 <= 'Z') ? (*s2 + 32) : *s2;
        if (c1 != c2) {
            return c1 - c2;
        }
        s1++;
        s2++;
    }
    return *s1 - *s2;
}

// Helper function to check if file has allowed extension (case-insensitive)
static bool hasAllowedExtension(const char* filename, const char* ext1, const char* ext2) {
    if (!filename) return false;
    const char* lastDot = strrchr(filename, '.');
    if (!lastDot) return false;
    const char* ext = lastDot + 1;
    return equalsIgnoreCase(ext, ext1) || equalsIgnoreCase(ext, ext2);
}

// Build the list entry for a directory entry; false = not listed
static bool makeListEntry(const FAT32_DirEntry* e, const char* longName, FAT32_ListEntry* out) {
    // "." self entry (".." is kept: it is the way back to the parent)
    if (e->name[0] == '.' && e->name[1] == ' ') return false;
    // Hidden/system entries: "System Volume Information", macOS "._NAME.DSK"
    // resource forks (which look like disk images but are not), etc.
    if (e->attributes & (FAT32_ATTR_HIDDEN | FAT32_ATTR_SYSTEM)) return false;
    if (longName && longName[0] == '.' && longName[1] == '_') return false;

    // Long names with non-ASCII characters (shown as '?') or longer than the
    // list entry cannot be found again by name -> list the 8.3 alias instead
    if (longName && !strchr(longName, '?') && strlen(longName) < sizeof(out->name)) {
        strcpy(out->name, longName);
    } else {
        int n = 0;
        for (int j = 0; j < 8; j++) {
            if (e->name[j] != ' ') out->name[n++] = (j == 0 && e->name[0] == 0x05) ? (char)0xE5 : (char)e->name[j];
        }
        if (e->name[8] != ' ') {
            out->name[n++] = '.';
            for (int j = 8; j < 11; j++) {
                if (e->name[j] != ' ') out->name[n++] = (char)e->name[j];
            }
        }
        out->name[n] = 0;
    }

    out->isDirectory = (e->attributes & FAT32_ATTR_DIRECTORY) != 0;
    out->readOnly = !out->isDirectory && (e->attributes & FAT32_ATTR_READ_ONLY) != 0;
    // Directories are always shown, files are filtered by extension (.dsk .nic .woz)
    return out->isDirectory || hasAllowedExtension(out->name, "dsk", "nic") ||
           hasAllowedExtension(out->name, "woz", "woz");
}

// Listing order: directories before files, then case-insensitive name (FAT
// names are unique case-insensitively; strcmp only makes the order total)
static int compareListEntry(const FAT32_ListEntry* a, const FAT32_ListEntry* b) {
    if (a->isDirectory != b->isDirectory) {
        return a->isDirectory ? -1 : 1;
    }
    int r = strcasecmp_sort(a->name, b->name);
    return r ? r : strcmp(a->name, b->name);
}

struct PageCtx {
    const FAT32_ListEntry* from;   // nullptr: no lower/upper bound
    bool backwards;
    FAT32_ListEntry* out;          // kept sorted ascending
    int max;
    int count;
    uint32_t total;                // counting mode (out == nullptr)
    uint32_t before;               // ... entries that sort before 'from'
    FAT32_ListEntry cand;
};

static bool pageVisitor(void* p, const FAT32_DirEntry* e, const char* longName) {
    PageCtx* c = (PageCtx*)p;
    if (!makeListEntry(e, longName, &c->cand)) return true;
    if (!c->out) {
        c->total++;
        if (c->from && compareListEntry(&c->cand, c->from) < 0) c->before++;
        return true;
    }
    if (c->from) {
        int r = compareListEntry(&c->cand, c->from);
        if (c->backwards ? r >= 0 : r <= 0) return true;    // not on the wanted side of 'from'
    }
    // Forward: keep the 'max' smallest candidates; backwards: the 'max' largest
    int pos = c->count;
    while (pos > 0 && compareListEntry(&c->cand, &c->out[pos - 1]) < 0) pos--;
    if (!c->backwards) {
        if (pos >= c->max) return true;                     // larger than all kept ones
        int last = (c->count < c->max) ? c->count : c->max - 1;
        for (int i = last; i > pos; i--) c->out[i] = c->out[i - 1];
        c->out[pos] = c->cand;
        if (c->count < c->max) c->count++;
    } else {
        if (c->count < c->max) {
            for (int i = c->count; i > pos; i--) c->out[i] = c->out[i - 1];
            c->out[pos] = c->cand;
            c->count++;
        } else {
            if (pos == 0) return true;                      // smaller than all kept ones
            for (int i = 0; i < pos - 1; i++) c->out[i] = c->out[i + 1];   // drop the smallest
            c->out[pos - 1] = c->cand;
        }
    }
    return true;
}

uint32_t FAT32::listCount(const FAT32_ListEntry* ref, uint32_t* before) {
    if (before) *before = 0;
    if (!sdCard) {
        return 0;
    }
    SDLockGuard guard;
    PageCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.from = ref;
    forEachDirEntry(currentDirCluster, pageVisitor, &ctx);
    if (before) *before = ctx.before;
    return ctx.total;
}

int FAT32::listPage(const FAT32_ListEntry* from, bool backwards, FAT32_ListEntry* out, int max) {
    if (!sdCard || !out || max <= 0) {
        return 0;
    }
    SDLockGuard guard;
    PageCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.from = from;
    ctx.backwards = backwards;
    ctx.out = out;
    ctx.max = max;
    forEachDirEntry(currentDirCluster, pageVisitor, &ctx);
    return ctx.count;
}

// Whole (sorted) listing as text, "NAME\r\n" / "NAME <DIR>\r\n" per entry, as
// far as it fits (kept for the SDCardManager API; the UI pages with listPage)
bool FAT32::listFiles(char* fileList, uint32_t maxSize, uint32_t* fileCount) {
    if (fileCount) {
        *fileCount = 0;
    }
    if (!sdCard || !fileList || maxSize == 0) {
        return false;
    }
    SDLockGuard guard;
    uint32_t listPos = 0;
    uint32_t listed = 0;
    FAT32_ListEntry page[8];
    FAT32_ListEntry last;
    bool haveLast = false;
    fileList[0] = 0;
    while (true) {
        int n = listPage(haveLast ? &last : nullptr, false, page, 8);
        for (int i = 0; i < n; i++) {
            int len = snprintf(fileList + listPos, maxSize - listPos, "%s%s\r\n", page[i].name,
                               page[i].isDirectory ? " <DIR>" : "");
            if (len <= 0 || listPos + (uint32_t)len >= maxSize) {
                fileList[listPos] = 0;
                if (fileCount) *fileCount = listed;
                return true;
            }
            listPos += len;
            listed++;
        }
        if (n < 8) break;
        last = page[n - 1];
        haveLast = true;
    }
    fileList[listPos] = 0;
    if (fileCount) {
        *fileCount = listed;
    }
    return true;
}
