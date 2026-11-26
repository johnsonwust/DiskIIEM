#ifndef FAT32_H
#define FAT32_H

#include <stdint.h>
#include <stdbool.h>

// FAT32 initialization error codes
typedef enum {
    FAT32_OK = 0,
    FAT32_ERROR_NO_SDCARD = 1,
    FAT32_ERROR_READ_FAILED = 2,
    FAT32_ERROR_EXFAT = 3,
    FAT32_ERROR_NTFS = 4,
    FAT32_ERROR_FAT12 = 5,
    FAT32_ERROR_FAT16 = 6,
    FAT32_ERROR_UNKNOWN_FS = 7,
    FAT32_ERROR_INVALID_PARAMS = 8
} FAT32_Error;

// FAT32 Boot Sector structure
typedef struct {
    uint8_t jump[3];
    uint8_t oem_name[8];
    uint16_t bytes_per_sector;
    uint8_t sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t num_fats;
    uint16_t root_entries;        // 0 for FAT32
    uint16_t total_sectors_16;    // 0 for FAT32
    uint8_t media_type;
    uint16_t sectors_per_fat_16;  // 0 for FAT32
    uint16_t sectors_per_track;
    uint16_t num_heads;
    uint32_t hidden_sectors;
    uint32_t total_sectors_32;
    uint32_t sectors_per_fat_32;
    uint16_t flags;
    uint16_t version;
    uint32_t root_cluster;
    uint16_t fs_info_sector;
    uint16_t backup_boot_sector;
    uint8_t reserved[12];
    uint8_t drive_number;
    uint8_t reserved1;
    uint8_t boot_signature;
    uint32_t volume_id;
    uint8_t volume_label[11];
    uint8_t fs_type[8];
} __attribute__((packed)) FAT32_BootSector;

// FAT32 Directory Entry structure
typedef struct {
    uint8_t name[11];             // 8.3 format
    uint8_t attributes;
    uint8_t reserved;
    uint8_t creation_time_tenths;
    uint16_t creation_time;
    uint16_t creation_date;
    uint16_t last_access_date;
    uint16_t cluster_high;
    uint16_t modification_time;
    uint16_t modification_date;
    uint16_t cluster_low;
    uint32_t file_size;
} __attribute__((packed)) FAT32_DirEntry;

// FAT32 Long File Name Entry structure
typedef struct {
    uint8_t sequence;            // Sequence number (bit 6 = last entry)
    uint16_t name1[5];           // First 5 characters (UTF-16)
    uint8_t attributes;          // Always 0x0F for LFN
    uint8_t type;                // Always 0x00
    uint8_t checksum;            // Checksum of short name
    uint16_t name2[6];           // Next 6 characters (UTF-16)
    uint16_t first_cluster;      // Always 0x0000
    uint16_t name3[2];           // Last 2 characters (UTF-16)
} __attribute__((packed)) FAT32_LFNEntry;

// FAT32 File attributes
#define FAT32_ATTR_READ_ONLY  0x01
#define FAT32_ATTR_HIDDEN     0x02
#define FAT32_ATTR_SYSTEM     0x04
#define FAT32_ATTR_VOLUME_ID  0x08
#define FAT32_ATTR_DIRECTORY  0x10
#define FAT32_ATTR_ARCHIVE    0x20
#define FAT32_ATTR_LONG_NAME  0x0F

// FAT32 special cluster values
#define FAT32_CLUSTER_FREE    0x0000000
#define FAT32_CLUSTER_RESERVED_MIN 0x0000002
#define FAT32_CLUSTER_RESERVED_MAX 0x0FFFFFEF
#define FAT32_CLUSTER_BAD     0x0FFFFFF7
#define FAT32_CLUSTER_EOF_MIN 0x0FFFFFF8
#define FAT32_CLUSTER_EOF_MAX 0x0FFFFFFF

// Handle of an opened file (see FAT32::openFile).
// Holds only cluster numbers plus the mount "generation": a handle obtained
// before a remount/format is refused, so stale cluster numbers can never be
// written into a different file system.
typedef struct {
    uint32_t firstCluster;   // first data cluster (0 for an empty file)
    uint32_t size;           // file size in bytes (never changed by fileWrite)
    uint32_t dirCluster;     // directory that holds the file
    uint32_t generation;     // FAT32 mount that produced the handle
    bool contiguous;         // clusters are consecutive -> no FAT lookups needed
    bool valid;
    bool readOnly;           // FAT read-only attribute of the directory entry
} FAT32_File;

// Forward declaration
class SDCardManager;

// One entry of a directory listing: the name shown to the user and passed to
// openFile()/changeDirectory() (long name, or the 8.3 alias when the long
// name does not fit or is not plain ASCII)
struct FAT32_ListEntry {
    char name[64];
    bool isDirectory;
    bool readOnly;      // file with the read-only attribute = write protected image
};

class FAT32 {
private:
    // Pointer to SDCardManager for reading blocks
    SDCardManager* sdCard;
    
    // Partition offset (for MBR partitioned cards)
    uint32_t partitionStartSector;
    
    // FAT32 structure
    FAT32_BootSector bootSector;
    uint32_t fatStartSector;
    uint32_t dataStartSector;
    uint32_t rootDirStartSector;
    uint32_t bytesPerCluster;
    uint32_t sectorsPerCluster;
    
    // Next-free-cluster search hint for allocateCluster()
    uint32_t allocHint;
    
    // Current directory tracking
    uint32_t currentDirCluster;
    char currentPath[256];
    
    // Last error code
    FAT32_Error lastError;
    
    // Mount generation (unique per FAT32 instance, see FAT32_File)
    uint32_t generation;
    
    // One-sector cache of FAT #1 (cluster chain walks hit the same sector
    // 128 times in a row). Invalidated by every FAT write.
    uint8_t fatCache[512];
    uint32_t fatCacheSector;
    bool fatCacheValid;
    
    // FSInfo free-cluster count is marked "unknown" once per mount, on the
    // first cluster allocation, so a PC recounts instead of trusting a stale value
    bool fsInfoInvalidated;
    
    // Internal methods
    bool readBootSector();
    uint32_t getClusterSector(uint32_t cluster);
    uint32_t readFATEntry(uint32_t cluster);
    bool findFileInDirectory(uint32_t dirCluster, const char* filename, FAT32_DirEntry* entry);
    void format83Name(const char* filename, char* name83);
    
    // File creation helpers
    uint32_t clusterLimit() const;   // highest valid cluster number + 1
    bool writeFATEntry(uint32_t cluster, uint32_t value);   // updates every FAT copy
    uint32_t allocateCluster();
    bool zeroCluster(uint32_t cluster);
    bool writeCluster(uint32_t cluster, const uint8_t* buffer);
    bool writeDirectoryEntry(uint32_t dirCluster, const char* filename, uint32_t cluster, uint32_t fileSize, bool replace = true);
    bool readFileFromEntry(const FAT32_DirEntry* entry, uint8_t* buffer, uint32_t maxSize, uint32_t* bytesRead);
    
    // Safety: every write of this driver goes through one of these. They
    // refuse any sector outside the FAT area / data area of this volume, so a
    // corrupt cluster number can never overwrite the MBR, boot sector or
    // another partition.
    bool isValidCluster(uint32_t cluster) const;
    bool writeFatSector(uint32_t sector, const uint8_t* buffer);
    bool writeDataSector(uint32_t sector, const uint8_t* buffer);
    void invalidateFsInfo(uint32_t nextFreeHint);
    
    // Sector-by-sector directory walk with streaming long-file-name assembly
    // (needs only a 512-byte buffer; handles names that span sectors/clusters)
    typedef bool (*DirVisitor)(void* ctx, const FAT32_DirEntry* entry, const char* longName);
    bool forEachDirEntry(uint32_t dirCluster, DirVisitor visit, void* ctx,
                         uint32_t* sector = nullptr, int* index = nullptr, bool* hasLFN = nullptr);
    
    // File handle internals
    bool openFileFromEntry(const FAT32_DirEntry* entry, uint32_t dirCluster, FAT32_File* file);
    uint32_t fileClusterAt(const FAT32_File* file, uint32_t clusterIndex);
    bool fileIO(const FAT32_File* file, uint32_t offset, uint8_t* readBuf, const uint8_t* writeBuf,
                uint32_t size, uint32_t* done);
    uint32_t resolveDir(uint32_t dirCluster) const { return dirCluster ? dirCluster : currentDirCluster; }
    
public:
    // Constructor
    FAT32(SDCardManager* sdCardManager);
    
    // Initialization
    bool init();
    
    // File operations
    bool fileExists(const char* filename);
    bool readFile(const char* filename, uint8_t* buffer, uint32_t maxSize, uint32_t* bytesRead);
    // dirCluster: directory to search (0 = current directory)
    bool listFiles(char* fileList, uint32_t maxSize, uint32_t* fileCount);

    // Sorted directory listing that does not hold the directory in RAM:
    // directories first (".." first of all), then .dsk/.nic/.woz files, each
    // alphabetically (case-insensitive). Every call scans the directory once.
    //   listCount(): number of entries in the current directory
    //   listPage():  up to 'max' entries that follow 'from' in that order
    //                (from = nullptr: from the start), or that precede it when
    //                backwards = true; returned in ascending order either way.
    // Number of entries; with `ref` also how many of them sort before it
    uint32_t listCount(const FAT32_ListEntry* ref = nullptr, uint32_t* before = nullptr);
    int listPage(const FAT32_ListEntry* from, bool backwards, FAT32_ListEntry* out, int max);
    
    // Handle based access (used for disk images). openFile validates the whole
    // cluster chain once; fileRead/fileWrite then work sector by sector with no
    // directory search. fileWrite only overwrites existing data: it never
    // changes the file size, the FAT or the directory.
    bool openFile(const char* filename, uint32_t dirCluster, FAT32_File* file);  // dirCluster 0 = current dir
    bool fileRead(const FAT32_File* file, uint32_t offset, uint8_t* buffer, uint32_t size, uint32_t* bytesRead);
    bool fileWrite(const FAT32_File* file, uint32_t offset, const uint8_t* buffer, uint32_t size);
    uint32_t getGeneration() const { return generation; }
    
    // Small root-directory files (config.cfg / log.txt). Always use the ROOT
    // directory, independent of the directory the user is browsing.
    // Set/clear the read-only attribute of an opened file (name as listed:
    // long name or 8.3). Changes one byte of the file's directory entry.
    bool setReadOnly(const FAT32_File& file, const char* name, bool readOnly);
    bool createImageFile(const char* filename, const uint8_t* data, uint32_t size);
    // Same, the content is produced sector by sector: fill() is called with
    // rising offsets and sets `bytes` (<= 512) bytes of `buf`.
    typedef void (*FileFill)(void* ctx, uint32_t offset, uint8_t* buf, uint32_t bytes);
    bool createImageFile(const char* filename, uint32_t size, FileFill fill, void* ctx);
    bool writeSmallFile(const char* filename, const uint8_t* data, uint32_t size);
    bool readSmallFile(const char* filename, uint8_t* buffer, uint32_t maxSize, uint32_t* bytesRead);
    
    // Directory operations
    bool findFile(const char* filename, FAT32_DirEntry* entry);
    bool changeDirectory(const char* dirname);
    bool getCurrentDirectory(char* path, uint32_t maxSize);
    uint32_t getRootCluster() const { return bootSector.root_cluster; }
    // Enter the directory part of "/DIR/SUB/FILE.DSK" (relative paths start at
    // the current directory). Returns a pointer to the file-name part, or
    // nullptr if a directory in the path does not exist.
    const char* enterPathDirectory(const char* path);
    
    // LFN support
    uint8_t calculateLFNChecksum(const uint8_t* shortName);
    
    // Utility
    
    // SD Card/Partition info getters
    uint32_t getPartitionStartSector() const { return partitionStartSector; }
    uint32_t getHiddenSectors() const { return bootSector.hidden_sectors; }
    // A directory holds at most 65536 entries (2 MB): longer chains are corrupt
    uint32_t maxDirClusters() const { return bytesPerCluster ? (2u * 1024u * 1024u) / bytesPerCluster + 1 : 1; }
    uint32_t getBytesPerCluster() const { return bytesPerCluster; }
    uint32_t getSectorsPerCluster() const { return sectorsPerCluster; }
    const char* getVolumeLabel() const;
    uint32_t getTotalSizeMB() const;
    
    // Error handling
    FAT32_Error getLastError() const { return lastError; }
};

#endif // FAT32_H

