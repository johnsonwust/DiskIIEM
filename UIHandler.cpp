#include "UIHandler.h"
#include "FAT32.h"
#include "ConfigManager.h"
#include "Branding.h"
#include "SDFormat.h"
#include "pico/time.h"
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

UIHandler::UIHandler(Display* disp, Buttons* keys_) {
    display = disp;
    keys = keys_;
    floppy = nullptr;
    sdCard = nullptr;
    currentScreen = UI_SCREEN_MAIN;
    selectedIndex = 0;
    menuIndex = 0;
    scrollOffset = 0;
    statusScroll = 0;
    configSelection = 0;
    formatSelection = 0;
    fileCount = 0;
    maxVisibleItems = LIST_ROWS_MAX;   // rows below the title bar
    listWinCount = 0;
    memset(listWin, 0, sizeof(listWin));
    lastRepeatTime = get_absolute_time();
    memset(loadedFileName, 0, sizeof(loadedFileName));
    lastUpdateTime = get_absolute_time();
    loadingEndTime = get_absolute_time();
    loadingReturnScreen = UI_SCREEN_FILE_LIST;
    backlightOn = true;
    lastKeyActivityTime = get_absolute_time();
    needsRefresh = true;
    sdErrorType = SD_ERROR_NONE;
    splashActive = false;
    splashStart = get_absolute_time();
    marqueeSig = 0;
    marqueeStart = get_absolute_time();
    lastCaptures = 0;
    writeShownUntil = get_absolute_time();
    strcpy(listTitle, "Files");
}

UIHandler::~UIHandler() {
}

void UIHandler::init() {
    // main() has put the still boot logo on the LCD before reading the SD
    // card; update() animates it from here on
    splashActive = true;
    splashStart = get_absolute_time();
    needsRefresh = true;
}

void UIHandler::setFloppyEmulator(FloppyEmulator* floppyEmu) {
    floppy = floppyEmu;
}

void UIHandler::setSDCardManager(SDCardManager* sdCardMgr) {
    sdCard = sdCardMgr;
    if (sdCard) previousDetect = sdCard->isCardPresent();
}

void UIHandler::setLoadedFileName(const char* name) {
    if (!name) name = "";
    strncpy(loadedFileName, name, sizeof(loadedFileName) - 1);
    loadedFileName[sizeof(loadedFileName) - 1] = 0;
}

void UIHandler::update() {
    // Update buttons
    keys->update();
    
    pollSDCard();
    if (splashActive) {
        // Boot logo: it ends by itself or with any key (that key press does
        // nothing else). The emulation on core0 runs all the time.
        const int64_t shown = absolute_time_diff_us(splashStart, get_absolute_time());
        if (keys->anyPressed()) {
            splashActive = false;
            waitForKeyRelease = true;
        } else if (shown >= (int64_t)bootLogoDurationMs() * 1000) {
            splashActive = false;
        }
        if (!splashActive) {
            marqueeSig = 0;
            needsRefresh = true;
        }
    }
    if (mediaMissing || waitForKeyRelease || splashActive) {
        keys->clearEvents();
        if (!keys->anyPressed() && !mediaMissing) waitForKeyRelease = false;
    } else {
        handleKeyInput();
    }
    
    // The drive can become empty behind the UI's back (an eject that timed out
    // here is completed later by core0): do not keep showing the old image
    if (loadedFileName[0] != 0 && floppy && !floppy->hasImageLoaded()) {
        loadedFileName[0] = 0;
        floppy->setCurrentFileName("");
        needsRefresh = true;
    }
    
    // Backlight: on when any key is held, off after 15s idle
    absolute_time_t now = get_absolute_time();
    if (keys->anyPressed()) {
        if (!backlightOn) {
            display->setBacklight(true);
            backlightOn = true;
        }
        lastKeyActivityTime = now;
    } else if (!mediaMissing && backlightOn && absolute_time_diff_us(lastKeyActivityTime, now) >= BACKLIGHT_TIMEOUT_US) {
        display->setBacklight(false);
        backlightOn = false;
    }
    
    // Refresh display every 100ms or when needed
    int64_t diff = absolute_time_diff_us(lastUpdateTime, now);
    
    // Check if loading screen timeout has expired
    if (currentScreen == UI_SCREEN_LOADING) {
        if (time_reached(loadingEndTime)) {
            // The message is over (time, or a key): on to the page that follows it
            currentScreen = loadingReturnScreen;
            if (currentScreen == UI_SCREEN_MAIN) menuIndex = 0;
            needsRefresh = true;
        }
    }
    
    if (splashActive) {
        if (needsRefresh || diff > 60000) {
            drawBootLogo(display, (uint32_t)(absolute_time_diff_us(splashStart, now) / 1000));
            display->update();
            lastUpdateTime = now;
            needsRefresh = false;
        }
    } else if (needsRefresh || diff > 100000) {  // 100ms
        switch (currentScreen) {
            case UI_SCREEN_MAIN:
                renderMainScreen();
                break;
            case UI_SCREEN_FILE_LIST:
                renderFileListScreen();
                break;
            case UI_SCREEN_INFO:
                renderInfoScreen();
                break;
            case UI_SCREEN_STATUS:
                renderStatusScreen();
                break;
            case UI_SCREEN_CONFIG:
                renderConfigScreen();
                break;
            case UI_SCREEN_FORMAT:
                renderFormatScreen();
                break;
            case UI_SCREEN_LOADING:
                // Loading screen is rendered explicitly with message
                break;
            case UI_SCREEN_NO_SD_CARD:
                renderNoSDCardScreen();
                break;
            case UI_SCREEN_SD_ERROR:
                renderSDErrorScreen();
                break;
        }
        display->update();
        lastUpdateTime = now;
        needsRefresh = false;
    }
}


void UIHandler::clearFileList() {
    strcpy(listTitle, "Files");
    fileCount = 0;
    listWinCount = 0;
    selectedIndex = 0;
    scrollOffset = 0;
}

// (Re)read the current directory. With `focusName` the cursor is put on that
// entry (if the directory has it) and the window is placed around it;
// otherwise the list starts at the top.
void UIHandler::updateFileList(const char* focusName, bool focusIsDir) {
    clearFileList();
    if (!sdCard) {
        return;
    }
    FAT32* fat32 = sdCard->getFAT32();
    if (!fat32) {
        return;
    }
    // Title: the directory's name ("Files" in the root directory)
    {
        static char path[256];              // the UI runs only on core1
        fat32->getCurrentDirectory(path, sizeof(path));
        const char* last = strrchr(path, '/');
        if (last && last[1] != 0) {
            snprintf(listTitle, sizeof(listTitle), "/%.11s", last + 1);
        }
    }

    const bool focus = focusName && focusName[0] != 0;
    FAT32_ListEntry wanted;
    memset(&wanted, 0, sizeof(wanted));
    if (focus) {
        snprintf(wanted.name, sizeof(wanted.name), "%s", focusName);
        wanted.isDirectory = focusIsDir;
    }
    uint32_t before = 0;
    fileCount = fat32->listCount(focus ? &wanted : nullptr, &before);
    if (focus && fileCount != 0) {
        // Up to two entries above the wanted one, then it and what follows
        FAT32_ListEntry above[2];
        const int na = fat32->listPage(&wanted, true, above, 2);
        for (int i = 0; i < na; i++) listWin[i] = above[i];
        const int nb = fat32->listPage(na ? &above[na - 1] : nullptr, false, &listWin[na], maxVisibleItems - na);
        if (nb > 0 && strcmp(listWin[na].name, wanted.name) == 0 && listWin[na].isDirectory == wanted.isDirectory) {
            listWinCount = na + nb;
            scrollOffset = (int)before - na;
            selectedIndex = (int)before;
            if (listWinCount < maxVisibleItems && (int)fileCount > listWinCount) {
                // Near the end of the list: show the last page completely
                const int n = fat32->listPage(nullptr, true, listWin, maxVisibleItems);
                if (n > 0) {
                    listWinCount = n;
                    scrollOffset = (int)fileCount - n;
                }
            }
            if (scrollOffset >= 0 && selectedIndex >= scrollOffset && selectedIndex < scrollOffset + listWinCount) {
                return;
            }
        }
        selectedIndex = 0;                   // not there (any more): from the top
        scrollOffset = 0;
    }
    listWinCount = fat32->listPage(nullptr, false, listWin, maxVisibleItems);
    if (listWinCount == 0) {
        fileCount = 0;
    }
}

// One page down (+1) or up (-1) while UP / DW is held. The cursor keeps its
// row; the last step stops on the last / first entry.
void UIHandler::listPageMove(int delta) {
    FAT32* fat32 = sdCard ? sdCard->getFAT32() : nullptr;
    if (!fat32 || listWinCount == 0) {
        return;
    }
    static FAT32_ListEntry page[LIST_ROWS_MAX];     // the UI runs only on core1
    const int row = selectedIndex - scrollOffset;
    const int n = (delta > 0) ? fat32->listPage(&listWin[listWinCount - 1], false, page, maxVisibleItems)
                              : fat32->listPage(&listWin[0], true, page, maxVisibleItems);
    if (n < maxVisibleItems) {
        listJump(delta > 0);                         // less than a page left
        return;
    }
    scrollOffset += (delta > 0) ? listWinCount : -n;
    if (scrollOffset < 0) scrollOffset = 0;
    memcpy(listWin, page, sizeof(listWin[0]) * (size_t)n);
    listWinCount = n;
    selectedIndex = scrollOffset + (row < n ? row : n - 1);
    needsRefresh = true;
}

// Jump to the last (toEnd) or first entry: the list wraps around when UP is
// pressed on the first entry or DW on the last one. Holding a key (auto
// repeat) does not wrap, it stops at the end.
void UIHandler::listJump(bool toEnd) {
    FAT32* fat32 = sdCard ? sdCard->getFAT32() : nullptr;
    if (!fat32 || listWinCount == 0 || fileCount == 0) {
        return;
    }
    int n = fat32->listPage(nullptr, toEnd, listWin, maxVisibleItems);
    if (n <= 0) {
        updateFileList(nullptr, false);                         // directory changed: start over
        needsRefresh = true;
        return;
    }
    listWinCount = n;
    if ((uint32_t)n > fileCount) fileCount = (uint32_t)n;
    scrollOffset = toEnd ? (int)fileCount - n : 0;
    selectedIndex = toEnd ? (int)fileCount - 1 : 0;
    needsRefresh = true;
}

// Move the selection one entry down (+1) or up (-1). When it leaves the
// visible window, the window is shifted by one entry read from the card.
void UIHandler::listMove(int delta) {
    if (listWinCount == 0) {
        return;
    }
    FAT32* fat32 = sdCard ? sdCard->getFAT32() : nullptr;
    int target = selectedIndex + delta;
    if (target < 0 || target >= (int)fileCount) {
        return;
    }
    if (target >= scrollOffset && target < scrollOffset + listWinCount) {
        selectedIndex = target;
        needsRefresh = true;
        return;
    }
    if (!fat32) {
        return;
    }
    FAT32_ListEntry e;
    if (delta > 0) {
        if (fat32->listPage(&listWin[listWinCount - 1], false, &e, 1) != 1) {
            fileCount = (uint32_t)(scrollOffset + listWinCount);   // directory shrank meanwhile
            return;
        }
        if (listWinCount < maxVisibleItems) {
            listWin[listWinCount++] = e;
        } else {
            memmove(&listWin[0], &listWin[1], sizeof(listWin[0]) * (listWinCount - 1));
            listWin[listWinCount - 1] = e;
            scrollOffset++;
        }
    } else {
        if (fat32->listPage(&listWin[0], true, &e, 1) != 1) {
            updateFileList(nullptr, false);                         // directory changed: start over
            needsRefresh = true;
            return;
        }
        memmove(&listWin[1], &listWin[0], sizeof(listWin[0]) * (listWinCount - 1));
        listWin[0] = e;
        scrollOffset--;
    }
    selectedIndex = target;
    needsRefresh = true;
}

// Map a FAT32 mount error to the UI error screen type
static SDErrorType mapFat32Error(FAT32_Error err) {
    switch (err) {
        case FAT32_ERROR_EXFAT: return SD_ERROR_EXFAT;
        case FAT32_ERROR_NTFS: return SD_ERROR_NTFS;
        case FAT32_ERROR_FAT12: return SD_ERROR_FAT12;
        case FAT32_ERROR_FAT16: return SD_ERROR_FAT16;
        case FAT32_ERROR_READ_FAILED: return SD_ERROR_READ_FAILED;
        default: return SD_ERROR_UNKNOWN_FS;
    }
}

void UIHandler::pollSDCard() {
    if (!sdCard) return;
    bool detected = sdCard->isCardPresent();
    bool detectChanged = previousDetect != detected;
    previousDetect = detected;
    if (!detectChanged && !time_reached(nextMediaCheck)) return;

    if (sdCard->isInitialized()) {
        nextMediaCheck = make_timeout_time_ms(200);
        // The socket's detect switch only speeds things up: the card counts as
        // removed when it no longer answers (twice, or once right after the
        // switch opened). A bouncing or noisy detect line alone must never drop
        // a working card - that would invalidate the loaded image's file handle.
        const bool removed = detectChanged && !detected;
        if (sdCard->probeCard()) {
            missedProbes = 0;
            return;
        }
        if (++missedProbes < 2 && !removed) return;
        {
            SDLockGuard guard;
            if (floppy) floppy->sdMediaRemoved();
            sdCard->deinit();
        }
        clearFileList();
        showNoSDCard();
        nextMediaCheck = make_timeout_time_ms(500);
        return;
    }

    // Retry even if the socket switch is disconnected or stuck.
    retrySDCard();
    nextMediaCheck = make_timeout_time_ms(sdCard->isInitialized() ? 200 : 1000);
}

// Re-initialise the SD card after insertion or from the SD ERROR screen
void UIHandler::retrySDCard() {
    if (!sdCard) {
        return;
    }
    bool ok;
    {
        SDLockGuard guard;
        if (floppy) floppy->sdMediaRemoved();
        ok = sdCard->init(10000000, false);
    }
    if (ok) {
        missedProbes = 0;
        mediaMissing = false;
        waitForKeyRelease = true;
        // Every (re)initialised card: read its config.cfg at once (created
        // with the current settings if missing) and apply it
        configLoad(sdCard);
        if (floppy) floppy->setWeakBits(g_config.weakBit);
        clearFileList();
        showMainMenu();
    } else if (!sdCard->isInitialized()) {
        showNoSDCard();
    } else {
        missedProbes = 0;
        mediaMissing = false;
        waitForKeyRelease = true;
        showSDError(mapFat32Error(sdCard->getLastFAT32Error()));
    }
}

void UIHandler::showNoSDCard() {
    mediaMissing = true;
    display->setBacklight(true);
    backlightOn = true;
    lastKeyActivityTime = get_absolute_time();
    currentScreen = UI_SCREEN_NO_SD_CARD;
    needsRefresh = true;
}

void UIHandler::showSDError(SDErrorType errorType) {
    sdErrorType = errorType;
    currentScreen = UI_SCREEN_SD_ERROR;
    needsRefresh = true;
}

void UIHandler::showMainMenu() {
    currentScreen = UI_SCREEN_MAIN;
    menuIndex = 0;
    needsRefresh = true;
}


// Hardware test aid: write the write/commit event log (WLOGnn.TXT) and the
// bits of the track currently in the cache (WTRKnn.BIN) into the card's root
// directory, nn = first free number. Events are copied from core0's ring
// while it may still be logging (a line may be torn; harmless for a log).
//
// The text is produced twice: once to learn its length, then again for every
// 512 byte sector of the file while it is written (only the characters inside
// that sector are kept). So the log needs no buffer of its own size.
struct LogText {
    uint32_t len;        // characters produced so far
    uint32_t winStart;   // file offset of the sector being filled
    uint8_t* win;        // nullptr: only count
    uint32_t winLen;
};
__attribute__((format(printf, 2, 3)))
static void logPut(LogText& t, const char* fmt, ...) {
    char line[200];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    if (n >= (int)sizeof(line)) n = (int)sizeof(line) - 1;
    if (t.win) {
        for (int i = 0; i < n; i++) {
            const uint32_t pos = t.len + (uint32_t)i;
            if (pos >= t.winStart && pos - t.winStart < t.winLen) t.win[pos - t.winStart] = (uint8_t)line[i];
        }
    }
    t.len += (uint32_t)n;
}

struct LogSource { FloppyEmulator* floppy; uint32_t count; };

static void logBuild(const LogSource& src, LogText& lt) {
    FloppyEmulator* floppy = src.floppy;
    const uint32_t count = src.count;
    const uint32_t size = FloppyEmulator::DBG_EVENTS;
    const uint32_t first = count > size ? count - size : 0;
    logPut(lt, PRODUCT_NAME " " FW_VERSION " write log\r\n");
    logPut(lt, "image=%s type=%d events=%u (showing %u)\r\n", floppy->getCurrentFileName(), (int)floppy->getCurrentFileType(),
        (unsigned)count, (unsigned)(count - first));
    // Counters of the write path and the pin levels (they were on the Status
    // page): each stage of a write, from the controller to the SD card
    {
        const volatile FloppyEmulator::WriteStats& w = floppy->wstats;
        const uint32_t lv = floppy->monitorLevels();
        logPut(lt, "counters\r\n");
        logPut(lt, "  Wr   write sessions %u, stored into the track %u\r\n", (unsigned)w.captures, (unsigned)w.stored);
        logPut(lt, "  Bit  last write: %u bits, %u skipped at its start, track %d\r\n", (unsigned)w.lastBits,
            (unsigned)w.lastSkip, (int)w.lastTrack);
        logPut(lt, "  Cm   commits %u, last one: %u sectors decoded, %u changed\r\n", (unsigned)w.commits,
            (unsigned)w.lastGood, (unsigned)w.lastChanged);
        logPut(lt, "  SD   track writes to the card: %u ok, %u failed\r\n", (unsigned)w.sdOk, (unsigned)w.sdFail);
        logPut(lt, "  Gl   WRITE spikes rejected %u\r\n", (unsigned)w.glitches);
        logPut(lt, "  WE   WRITE_EN falling edges %u, with this drive selected %u\r\n", (unsigned)w.weFalls,
            (unsigned)w.weFallsSel);
        logPut(lt, "  WR   WRITE level changes %u\r\n", (unsigned)w.wrEdges);
        logPut(lt, "pins now (level at the Pico pin): DRIVE_SEL#=%u WRITE_EN#=%u WRITE=%u WRPROT out=%u, write protect %s\r\n",
            (unsigned)((lv >> GPIO_DRIVE_SEL) & 1), (unsigned)((lv >> GPIO_WRITE_ENABLE) & 1), (unsigned)((lv >> GPIO_WRITE) & 1),
            (unsigned)gpio_get_out_level(GPIO_WR_PROTECT), floppy->isWriteProtectSignalled() ? "signalled" : "not signalled");
        logPut(lt, "cache: track=%d bits=%u (WTRKnn.BIN: raw track bits, MSB first)\r\n", floppy->currentTrackKey(),
            (unsigned)floppy->currentTrackBitCount());
    }
    logPut(lt, "time_ms  event\r\n");
    logPut(lt, "  W trk bits span skip start q store bursts glitches   (write session)\r\n");
    logPut(lt, "  T from to q                              (track switch)\r\n");
    logPut(lt, "  C trk good changed completed             (commit to image)\r\n");
    logPut(lt, "  F trk ok                                 (track written to SD)\r\n");
    logPut(lt, "  L type size trackbits                    (image loaded)\r\n");
    for (uint32_t i = first; i < count; i++) {
        const FloppyEmulator::DbgEvent& e = floppy->dbgRing[i % size];
        logPut(lt, "%8u.%03u %c ", (unsigned)(e.timeUs / 1000), (unsigned)(e.timeUs % 1000), e.type ? e.type : '?');
        switch (e.type) {
            case 'W': logPut(lt, "trk=%d bits=%d span=%d skip=%d start=%d q=%d store=%d bursts=%d gl=%d\r\n", e.a, (int)e.b,
                          (int)((uint32_t)e.c >> 16), (int)(e.c & 0xFFFF), (int)e.d, (int)((uint32_t)e.e >> 24),
                          (int)((e.e >> 23) & 1), (int)((e.e >> 16) & 0x7F), (int)(e.e & 0xFFFF)); break;
            case 'T': logPut(lt, "from=%d to=%d q=%d\r\n", e.a, (int)e.b, (int)e.c); break;
            case 'C': logPut(lt, "trk=%d good=%d changed=%d completed=%d\r\n", e.a, (int)e.b, (int)e.c, (int)e.d); break;
            case 'F': logPut(lt, "trk=%d ok=%d\r\n", e.a, (int)e.b); break;
            case 'L': logPut(lt, "type=%d size=%d trackbits=%d\r\n", e.a, (int)e.b, (int)e.c); break;
            default:  logPut(lt, "%d %d %d %d %d\r\n", e.a, (int)e.b, (int)e.c, (int)e.d, (int)e.e); break;
        }
    }
}

static void logFill(void* ctx, uint32_t offset, uint8_t* buf, uint32_t bytes) {
    memset(buf, ' ', bytes);                 // should the text have become shorter meanwhile
    LogText lt{0, offset, buf, bytes};
    logBuild(*static_cast<const LogSource*>(ctx), lt);
}

int UIHandler::saveDebugLog() {
    if (!floppy || !sdCard || !sdCard->getFAT32()) return -1;
    FAT32* fat = sdCard->getFAT32();
    LogSource source{floppy, floppy->dbgCount};
    LogText probe{0, 0, nullptr, 0};
    logBuild(source, probe);
    const uint32_t textLen = probe.len;

    char saved[256];
    fat->getCurrentDirectory(saved, sizeof(saved));
    fat->changeDirectory("/");
    int result = -1;
    for (int n = 0; n < 100 && result < 0; n++) {
        char logName[16], trkName[16];
        snprintf(logName, sizeof(logName), "WLOG%02d.TXT", n);
        snprintf(trkName, sizeof(trkName), "WTRK%02d.BIN", n);
        FAT32_File existing;
        if (fat->openFile(logName, 0, &existing)) continue;
        if (!fat->createImageFile(logName, textLen, logFill, &source)) break;
        uint32_t bytes = (floppy->currentTrackBitCount() + 7) / 8;
        if (floppy->currentTrackBits() && bytes) {
            fat->createImageFile(trkName, floppy->currentTrackBits(), bytes);
        }
        result = n;
    }
    fat->changeDirectory("/");
    if (strcmp(saved, "/") != 0) {
        char restore[260];
        snprintf(restore, sizeof(restore), "%s/.", saved);
        fat->enterPathDirectory(restore);
    }
    clearFileList();
    return result;
}
