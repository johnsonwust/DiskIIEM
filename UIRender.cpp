// Screen drawing for UIHandler.
//
// Layout, icons and boot logo were designed for this project (128x64
// monochrome LCD, 5x7 font in a 6x8 cell):
//   - every page has a 9 pixel inverted title bar and 6 text rows below it;
//   - the home page shows the drive (image name, track, head position gauge,
//     activity) on the left and the menu on the right;
//   - long names scroll (one character at a time) instead of being cut off;
//   - lists have icons and a scroll bar.
// Key handling and all SD card work are in UIHandler.cpp.
#include "UIHandler.h"
#include "Branding.h"
#include "ConfigManager.h"
#include "FAT32.h"
#include "pico/time.h"
#include <string.h>
#include <stdio.h>

namespace {

const int W = DISPLAY_WIDTH;        // 128
const int H = DISPLAY_HEIGHT;       // 64
const int BAR_H = 9;                // title bar: y = 0..8
const int ROW_Y0 = 10;              // first text row
const int ROW_H = 9;                // row pitch; 6 rows end at y = 63
const int ROWS = 6;
const uint32_t SPLASH_MS = 1000;    // animated part of the boot logo

// Icons: one uint16_t per row, bit 15 = leftmost pixel.
const uint16_t iconFolder[7] = {
    0x7000,  // .###....
    0x8F00,  // #...####
    0x8100,  // #......#
    0x8100,  // #......#
    0x8100,  // #......#
    0x8100,  // #......#
    0xFF00   // ########
};
const uint16_t iconParent[7] = {
    0x1000,  // ...#....
    0x3800,  // ..###...
    0x7C00,  // .#####..
    0x1000,  // ...#....
    0x1000,  // ...#....
    0x1E00,  // ...####.
    0x0000   // ........
};
const uint16_t iconDiskette[7] = {
    0xFE00,  // #######.   5.25" disk: jacket,
    0x8200,  // #.....#.
    0x9200,  // #..#..#.   hub hole,
    0x8200,  // #.....#.
    0x9200,  // #..#..#.   head slot
    0x9200,  // #..#..#.
    0xFE00   // #######.
};
const uint16_t iconLocked[7] = {
    0x3800,  // ..###...   padlock: write protected image
    0x4400,  // .#...#..
    0x4400,  // .#...#..
    0xFE00,  // #######.
    0xEE00,  // ###.###.
    0xEE00,  // ###.###.
    0xFE00   // #######.
};
const uint16_t iconUnsaved[7] = {
    0x1000,  // ...#...    arrow onto a line: changes still to be
    0x1000,  // ...#...    written to the card
    0x5400,  // .#.#.#.
    0x3800,  // ..###..
    0x1000,  // ...#...
    0x0000,  // .......
    0xFE00   // #######
};
const uint16_t iconRing[7] = {
    0x3800,  // ..###..
    0x4400,  // .#...#.
    0x8200,  // #.....#
    0x8200,  // #.....#
    0x8200,  // #.....#
    0x4400,  // .#...#.
    0x3800   // ..###..
};

// Eight directions, clockwise from "right"
const int8_t DIR_X[8] = {1, 1, 0, -1, -1, -1, 0, 1};
const int8_t DIR_Y[8] = {0, 1, 1, 1, 0, -1, -1, -1};

void fillDisc(Display* d, int cx, int cy, int r, bool color) {
    for (int dy = -r; dy <= r; dy++) {
        for (int dx = -r; dx <= r; dx++) {
            if (dx * dx + dy * dy <= r * r + r) {
                d->setPixel(cx + dx, cy + dy, color);
            }
        }
    }
}

// Rectangle outline with the four corner pixels left out
void roundFrame(Display* d, int x, int y, int w, int h, bool color) {
    d->drawLine(x + 1, y, x + w - 2, y, color);
    d->drawLine(x + 1, y + h - 1, x + w - 2, y + h - 1, color);
    d->drawLine(x, y + 1, x, y + h - 2, color);
    d->drawLine(x + w - 1, y + 1, x + w - 1, y + h - 2, color);
}

// Filled rectangle with the four corner pixels left out
void roundFill(Display* d, int x, int y, int w, int h, bool color) {
    d->fillRect(x, y, w, h, color);
    d->setPixel(x, y, !color);
    d->setPixel(x + w - 1, y, !color);
    d->setPixel(x, y + h - 1, !color);
    d->setPixel(x + w - 1, y + h - 1, !color);
}

// Scroll bar at the right edge of the row area: first..first+shown of total
void scrollBar(Display* d, int first, int shown, int total) {
    if (total <= shown || shown <= 0) return;
    const int top = ROW_Y0, len = H - ROW_Y0;
    for (int y = top; y < H; y += 2) d->setPixel(W - 2, y, true);
    int th = len * shown / total;
    if (th < 5) th = 5;
    int ty = top + (len - th) * first / (total - shown);
    d->fillRect(W - 3, ty, 3, th, true);
}

void copyClipped(char* out, int outSize, const char* text, int chars) {
    if (chars > outSize - 1) chars = outSize - 1;
    if (chars < 0) chars = 0;
    strncpy(out, text, (size_t)chars);
    out[chars] = 0;
}

}  // namespace

// ---------------------------------------------------------------------------
// Boot logo. elapsedMs = 0 is the still picture main() shows before the SD
// card is read; UIHandler::update() then animates it (hub turns, bar fills).
// ---------------------------------------------------------------------------
uint32_t UIHandler::bootLogoDurationMs() {
    return SPLASH_MS;
}

void UIHandler::drawBootLogo(Display* d, uint32_t elapsedMs) {
    d->clear();

    // Name: LOGO_LEFT in double size, LOGO_BOX inverted in a box; centred
    const int leftW = (int)strlen(LOGO_LEFT) * 12, boxW = (int)strlen(LOGO_BOX) * 12 + 6;
    const int nx = (W - leftW - boxW) / 2;
    d->drawStringScaled(nx, 4, LOGO_LEFT, 2, true);
    roundFill(d, nx + leftW, 1, boxW, 20, true);
    d->drawStringScaled(nx + leftW + 4, 4, LOGO_BOX, 2, false);

    // 5.25" disk, 38 x 38 pixels
    const int x0 = 4, y0 = 25, S = 38;
    roundFill(d, x0, y0, S, S, true);
    d->fillRect(x0 + S - 2, y0 + 8, 2, 4, false);           // write-protect notch
    d->fillRect(x0 + 4, y0 + 3, 22, 8, false);              // label
    d->drawLine(x0 + 6, y0 + 5, x0 + 21, y0 + 5, true);
    d->drawLine(x0 + 6, y0 + 8, x0 + 15, y0 + 8, true);
    const int cx = x0 + 19, cy = y0 + 22;
    fillDisc(d, cx, cy, 7, false);                          // centre hole
    fillDisc(d, cx, cy, 5, true);                           // hub of the disk inside
    fillDisc(d, cx, cy, 1, false);                          // spindle hole
    const int ph = (int)((elapsedMs / 70) & 7);             // the hub turns
    d->setPixel(cx + 3 * DIR_X[ph], cy + 3 * DIR_Y[ph], false);
    d->setPixel(cx + 4 * DIR_X[ph], cy + 4 * DIR_Y[ph], false);
    d->fillRect(cx + 9, cy - 5, 2, 2, false);               // index hole
    roundFill(d, x0 + 17, y0 + 31, 5, 6, false);            // head slot

    // Tag line, version, progress bar
    const int tx = 48;
    d->drawString(tx, 27, PRODUCT_TAGLINE, true);
    d->drawString(tx, 38, FW_VERSION, true);
    const uint32_t p = elapsedMs >= SPLASH_MS ? 100 : elapsedMs * 100 / SPLASH_MS;
    roundFrame(d, tx, 53, 77, 8, true);
    if (p > 0) d->fillRect(tx + 2, 55, (int)(73 * p / 100), 4, true);
}

// ---------------------------------------------------------------------------
// Shared parts
// ---------------------------------------------------------------------------

// Title bar. right == nullptr: show the head position ("T17", WOZ: "T17.25").
// The "unsaved" icon appears left of it while changes wait to be written.
void UIHandler::drawTitleBar(const char* title, const char* right) {
    display->fillRect(0, 0, W, BAR_H, true);
    display->drawString(2, 1, title, false);
    char trk[12];
    if (!right) {
        trk[0] = 0;
        if (floppy) {
            if (floppy->isWoz()) {
                int q = floppy->getQuarterTrack();
                if (q % 4) snprintf(trk, sizeof(trk), "T%d.%02d", q / 4, (q % 4) * 25);
                else snprintf(trk, sizeof(trk), "T%d", q / 4);
            } else {
                snprintf(trk, sizeof(trk), "T%d", floppy->getCurrentTrack());
            }
        }
        right = trk;
    }
    int x = W - 1 - (int)strlen(right) * 6;
    display->drawString(x, 1, right, false);
    if (floppy && floppy->hasImageLoaded() && floppy->getGCRTrackCacheDirty()) {
        display->drawBitmap(x - 10, 1, iconUnsaved, 7, 7, false);
    }
}

// Text that does not fit into `window` characters scrolls: it rests 1.2 s,
// moves one character every 0.25 s, rests at the end and starts again. Only
// one scrolling text is on the screen at a time (its position restarts when
// the text changes).
const char* UIHandler::marquee(const char* text, int window, char* out, int outSize) {
    const int len = (int)strlen(text);
    uint32_t sig = 2166136261u ^ (uint32_t)currentScreen;
    for (const char* p = text; *p; p++) sig = (sig ^ (uint8_t)*p) * 16777619u;
    const absolute_time_t now = get_absolute_time();
    if (sig != marqueeSig) {
        marqueeSig = sig;
        marqueeStart = now;
    }
    int offset = 0;
    if (len > window) {
        const int span = len - window;
        const int64_t rest = 1200, step = 250;
        int64_t t = absolute_time_diff_us(marqueeStart, now) / 1000;
        t %= rest + span * step + rest;
        if (t > rest) {
            offset = (int)((t - rest) / step);
            if (offset > span) offset = span;
        }
    }
    copyClipped(out, outSize, text + offset, window);
    return out;
}

// ---------------------------------------------------------------------------
// Home page: the drive on the left, the menu on the right
// ---------------------------------------------------------------------------
void UIHandler::renderMainScreen() {
    display->clear();
    drawTitleBar(PRODUCT_NAME, "");

    const bool loaded = floppy && floppy->hasImageLoaded() && loadedFileName[0] != 0;
    char buf[24];

    // Image name (14 characters, longer names scroll)
    display->drawString(0, 12, loaded ? marquee(loadedFileName, 14, buf, sizeof(buf)) : "No disk", true);

    // Track number, double size
    int track = 0, quarter = 0, maxQuarter = 34 * 4;
    if (floppy) {
        track = floppy->getCurrentTrack();
        quarter = track * 4;
        if (floppy->isWoz()) {
            quarter = floppy->getQuarterTrack();
            track = quarter / 4;
            maxQuarter = 39 * 4;
        }
    }
    snprintf(buf, sizeof(buf), "%02d", track);
    display->drawStringScaled(0, 22, buf, 2, true);
    display->drawString(27, 22, "TRK", true);
    if (quarter % 4) {
        snprintf(buf, sizeof(buf), ".%02d", (quarter % 4) * 25);
        display->drawString(27, 30, buf, true);
    }
    if (loaded) {
        const char* type = "DSK";
        DiskFileType ft = floppy->getCurrentFileType();
        if (ft == DISK_FILE_TYPE_NIC) type = "NIC";
        else if (ft == DISK_FILE_TYPE_WOZ) type = floppy->getWozInfo().version == 1 ? "WOZ1" : "WOZ2";
        display->drawString(54, 22, type, true);
        display->drawString(54, 30, floppy->isImageWriteProtected() ? "PROT" : "R/W", true);
    }

    // Head position gauge: track 0 at the left, a tick every 5 tracks
    const int gx = 0, gy = 40, gw = 83, gh = 7;
    display->drawRect(gx, gy, gw, gh, true, false);
    for (int t = 0; t <= maxQuarter; t += 20) {
        display->setPixel(gx + 3 + t * (gw - 7) / maxQuarter, gy + gh, true);
    }
    if (quarter < 0) quarter = 0;
    if (quarter > maxQuarter) quarter = maxQuarter;
    display->fillRect(gx + 2 + quarter * (gw - 7) / maxQuarter, gy + 1, 3, gh - 2, true);

    // Activity: the ring turns while the computer has the drive switched on
    const bool driveOn = floppy && floppy->isDriveSelected();
    const uint32_t captures = floppy ? floppy->wstats.captures : 0;
    if (captures != lastCaptures) {
        lastCaptures = captures;
        writeShownUntil = make_timeout_time_ms(500);
    }
    const bool writing = !time_reached(writeShownUntil);
    display->drawBitmap(0, 53, iconRing, 7, 7, true);
    display->setPixel(3, 56, true);
    if (driveOn) {
        const int ph = (int)((to_ms_since_boot(get_absolute_time()) / 125) & 7);
        display->setPixel(3 + DIR_X[ph], 56 + DIR_Y[ph], true);
        display->setPixel(3 + 2 * DIR_X[ph], 56 + 2 * DIR_Y[ph], true);
    }
    // UNSAVED: something the computer wrote is not on the SD card yet (it is
    // written as soon as the drive is idle) - do not switch off now
    const bool unsaved = floppy && floppy->hasImageLoaded() && floppy->getGCRTrackCacheDirty();
    display->drawString(11, 53, writing ? "WRITING" : (unsaved ? "UNSAVED" : (driveOn ? "READING" : "IDLE")), true);

    // Menu
    display->drawLine(85, BAR_H, 85, H - 1, true);
    static const char* const items[4] = {"Files", "Status", "Info", "Config"};
    for (int i = 0; i < 4; i++) {
        const int y = 12 + i * 13;
        const bool sel = (i == menuIndex);
        if (sel) display->fillRect(86, y, W - 86, 11, true);
        display->drawString(90, y + 2, items[i], !sel);
    }
}

// ---------------------------------------------------------------------------
// File list: 6 rows, icon + name, the selected long name scrolls
// ---------------------------------------------------------------------------
void UIHandler::renderFileListScreen() {
    display->clear();
    char pos[16] = "";
    if (fileCount != 0 && listWinCount != 0) {
        snprintf(pos, sizeof(pos), "%d/%u", selectedIndex + 1, (unsigned)fileCount);
    }
    drawTitleBar(listTitle, pos);

    // NOTE: the list is read when the page is entered or a directory is
    // changed, never here (this runs every 100 ms).
    if (fileCount == 0 || listWinCount == 0) {
        display->drawString((W - 14 * 6) / 2, 30, "No files found", true);
        return;
    }
    const int nameChars = 19;
    for (int i = 0; i < listWinCount && i < maxVisibleItems; i++) {
        const int y = ROW_Y0 + i * ROW_H;
        const bool sel = (scrollOffset + i == selectedIndex);
        const FAT32_ListEntry& e = listWin[i];
        if (sel) display->fillRect(0, y, W - 4, ROW_H, true);
        const uint16_t* icon = e.isDirectory ? (strcmp(e.name, "..") == 0 ? iconParent : iconFolder)
                                             : (e.readOnly ? iconLocked : iconDiskette);
        display->drawBitmap(1, y + 1, icon, 8, 7, !sel);
        char line[24];
        if (sel) marquee(e.name, nameChars, line, sizeof(line));
        else copyClipped(line, sizeof(line), e.name, nameChars);
        display->drawString(10, y + 1, line, !sel);
    }
    scrollBar(display, scrollOffset, maxVisibleItems, (int)fileCount);
}

// ---------------------------------------------------------------------------
// Disk info
// ---------------------------------------------------------------------------
void UIHandler::renderInfoScreen() {
    display->clear();
    drawTitleBar("Disk info", nullptr);
    if (!floppy) return;

    const char* fileName = floppy->getCurrentFileName();
    if (!fileName || fileName[0] == 0) {
        display->drawString(2, ROW_Y0 + 1, "No file loaded", true);
        return;
    }
    const char* nameOnly = strrchr(fileName, '/');
    nameOnly = nameOnly ? nameOnly + 1 : fileName;
    const DiskFileType fileType = floppy->getCurrentFileType();
    const bool woz = (fileType == DISK_FILE_TYPE_WOZ);
    const WozInfo& w = floppy->getWozInfo();

    char buf[32];
    int row = 0;
    display->drawString(1, ROW_Y0 + 1, marquee(nameOnly, 21, buf, sizeof(buf)), true);
    row++;

    char value[5][24];
    const char* label[5];
    int n = 0;
    label[n] = "Type";
    if (woz) {
        // WP = write protected inside the file, FX = FLUX tracks,
        // SD = large image, tracks are read from the card on demand
        snprintf(value[n], sizeof(value[0]), "WOZ%u%s%s%s", (unsigned)w.version, w.writeProtected ? " WP" : "",
                 w.fluxMapValid ? " FX" : "", floppy->isWozFromSD() ? " SD" : "");
    } else {
        snprintf(value[n], sizeof(value[0]), "%s %u KB", fileType == DISK_FILE_TYPE_NIC ? "NIC" : "DSK",
                 (unsigned)((fileType == DISK_FILE_TYPE_NIC ? APPLE_II_NIC_DISK_SIZE : APPLE_II_DISK_SIZE) / 1024));
    }
    n++;
    label[n] = "Track";
    if (woz) {
        int q = floppy->getQuarterTrack();
        snprintf(value[n], sizeof(value[0]), "%d.%02d of %u", q / 4, (q % 4) * 25, (unsigned)w.trackCount);
    } else {
        snprintf(value[n], sizeof(value[0]), "%d of 34", floppy->getCurrentTrack());
    }
    n++;
    if (woz) {
        label[n] = "Bit";
        snprintf(value[n], sizeof(value[0]), "%u.%03u us", (unsigned)(w.bitTiming * 125 / 1000),
                 (unsigned)(w.bitTiming * 125 % 1000));
        n++;
    }
    label[n] = "Write";
    snprintf(value[n], sizeof(value[0]), "%s", floppy->isImageWriteProtected() ? "protected" : "enabled");
    n++;
    label[n] = "Saved";
    snprintf(value[n], sizeof(value[0]), "%s", floppy->getGCRTrackCacheDirty() ? "no, pending" : "yes");
    n++;
    for (int i = 0; i < n && row < ROWS; i++, row++) {
        const int y = ROW_Y0 + row * ROW_H + 1;
        display->drawString(1, y, label[i], true);
        display->drawString(43, y, value[i], true);
    }
}

// ---------------------------------------------------------------------------
// SD card page
// ---------------------------------------------------------------------------
void UIHandler::renderStatusScreen() {
    display->clear();
    drawTitleBar("SD card", nullptr);

    char lines[8][32];
    int lineCount = 0;

    if (sdCard && sdCard->isInitialized()) {
        snprintf(lines[lineCount++], sizeof(lines[0]), "SD Card: OK");
        // Card class and addressing mode ("fix": the volume was only found
        // with the other addressing mode, see SDCardManager::mountFat32)
        snprintf(lines[lineCount++], sizeof(lines[0]), "Type: %s %s%s", sdCard->isHighCapacity() ? "SDHC" : "SDSC",
                 sdCard->usesBlockAddressing() ? "blk" : "byte", sdCard->addressingWasCorrected() ? " fix" : "");

        uint32_t speed = sdCard->getCurrentBaudrate();
        if (speed >= 1000000) {
            snprintf(lines[lineCount++], sizeof(lines[0]), "Speed: %u MHz", (unsigned)(speed / 1000000));
        } else {
            snprintf(lines[lineCount++], sizeof(lines[0]), "Speed: %u kHz", (unsigned)(speed / 1000));
        }

        FAT32* fat32 = sdCard->getFAT32();
        if (fat32) {
            const char* label = fat32->getVolumeLabel();
            if (label && label[0] != 0) {
                snprintf(lines[lineCount++], sizeof(lines[0]), "Label: %s", label);
            }
            uint32_t sizeMB = fat32->getTotalSizeMB();
            if (sizeMB >= 1024) {
                snprintf(lines[lineCount++], sizeof(lines[0]), "Size: %u.%u GB",
                         (unsigned)(sizeMB / 1024), (unsigned)((sizeMB % 1024) * 10 / 1024));
            } else {
                snprintf(lines[lineCount++], sizeof(lines[0]), "Size: %u MB", (unsigned)sizeMB);
            }
            uint32_t partStart = fat32->getPartitionStartSector();
            if (partStart > 0) {
                snprintf(lines[lineCount++], sizeof(lines[0]), "Part@: %u", (unsigned)partStart);
            }
            snprintf(lines[lineCount++], sizeof(lines[0]), "Cluster: %u KB", (unsigned)(fat32->getBytesPerCluster() / 1024));
        } else {
            snprintf(lines[lineCount++], sizeof(lines[0]), "No FAT32 volume");
        }
    } else {
        snprintf(lines[lineCount++], sizeof(lines[0]), "SD Card: Not Found");
        snprintf(lines[lineCount++], sizeof(lines[0]), "Insert SD card");
    }

    // (The write path counters and pin levels that used to follow here are
    //  written into the debug log: Config > Save debug log.)

    int maxScroll = (lineCount > ROWS) ? (lineCount - ROWS) : 0;
    if (statusScroll > maxScroll) statusScroll = maxScroll;
    if (statusScroll < 0) statusScroll = 0;
    for (int i = 0; i < ROWS && (statusScroll + i) < lineCount; i++) {
        display->drawString(1, ROW_Y0 + i * ROW_H + 1, lines[statusScroll + i], true);
    }
    scrollBar(display, statusScroll, ROWS, lineCount);
}

// ---------------------------------------------------------------------------
// Config: label left, value right, the selected row inverted
// ---------------------------------------------------------------------------
void UIHandler::renderConfigScreen() {
    display->clear();
    drawTitleBar("Config", nullptr);

    const char* labels[] = {"Sound", "Boot last image", "Weak bit", "Write protect", "Format SD", "Save debug log"};
    const char* values[] = {
        g_config.soundOn ? "ON" : "OFF",
        g_config.bootLastImage ? "YES" : "NO",
        g_config.weakBit ? "YES" : "NO",
        !floppy || !floppy->hasImageLoaded() ? "--" :
            (floppy->isImageWriteProtected() ? "YES" : "NO"),
        ">",
        ">"
    };
    const int count = CONFIG_DEBUGLOG + 1;
    const int first = configSelection < ROWS ? 0 : configSelection - ROWS + 1;
    for (int row = 0; row < ROWS && first + row < count; ++row) {
        const int item = first + row;
        const int y = ROW_Y0 + row * ROW_H;
        const bool sel = (item == configSelection);
        if (sel) display->fillRect(0, y, count > ROWS ? W - 4 : W, ROW_H, true);
        display->drawString(2, y + 1, labels[item], !sel);
        const int vx = (count > ROWS ? W - 6 : W - 2) - (int)strlen(values[item]) * 6;
        display->drawString(vx, y + 1, values[item], !sel);
    }
    scrollBar(display, first, ROWS, count);
}

// ---------------------------------------------------------------------------
// Format confirmation: two buttons, NO is the default
// ---------------------------------------------------------------------------
void UIHandler::renderFormatScreen() {
    display->clear();
    drawTitleBar("Format SD?", "");
    display->drawString(1, 14, "All files on the card", true);
    display->drawString(1, 23, "will be erased.", true);

    static const char* const text[2] = {"NO", "YES"};
    for (int i = 0; i < 2; i++) {
        const int x = 16 + i * 56, y = 41, w = 41, h = 13;
        const bool sel = (formatSelection == i);
        if (sel) roundFill(display, x, y, w, h, true);
        else roundFrame(display, x, y, w, h, true);
        display->drawString(x + (w - (int)strlen(text[i]) * 6) / 2 + 1, y + 3, text[i], !sel);
    }
}

// ---------------------------------------------------------------------------
// Message in a box (Loading..., OK, errors, Formatting...)
// ---------------------------------------------------------------------------
void UIHandler::renderLoadingScreen(const char* message) {
    display->clear();
    drawTitleBar(PRODUCT_NAME, "");

    char text[22];
    copyClipped(text, sizeof(text), message, 21);
    const int len = (int)strlen(text);
    int w = len * 6 + 16;
    if (w > W) w = W;
    const int x = (W - w) / 2, y = 26, h = 19;
    roundFrame(display, x, y, w, h, true);
    if (w < W - 1) {                                        // shadow
        display->drawLine(x + 2, y + h, x + w - 1, y + h, true);
        display->drawLine(x + w, y + 2, x + w, y + h - 1, true);
    }
    display->drawString((W - len * 6) / 2 + 1, y + 6, text, true);
}

// ---------------------------------------------------------------------------
// No card: card outline with a blinking question mark
// ---------------------------------------------------------------------------
void UIHandler::renderNoSDCardScreen() {
    display->clear();
    drawTitleBar(PRODUCT_NAME, "");

    const int x = 10, y = 20, w = 24, h = 32, cut = 7;
    display->drawLine(x, y, x + w - 1 - cut, y, true);
    display->drawLine(x + w - 1 - cut, y, x + w - 1, y + cut, true);
    display->drawLine(x + w - 1, y + cut, x + w - 1, y + h - 1, true);
    display->drawLine(x + w - 1, y + h - 1, x, y + h - 1, true);
    display->drawLine(x, y + h - 1, x, y, true);
    for (int i = 0; i < 4; i++) {
        display->fillRect(x + 3 + i * 3, y + 3, 2, 5, true);  // contacts
    }
    if ((to_ms_since_boot(get_absolute_time()) / 500) & 1) {
        display->drawStringScaled(x + 7, y + 13, "?", 2, true);
    }
    display->drawString(44, 26, "NO SD CARD", true);
    display->drawString(42, 40, "Insert SD card", true);
}

// ---------------------------------------------------------------------------
// The card answers but cannot be used
// ---------------------------------------------------------------------------
void UIHandler::renderSDErrorScreen() {
    display->clear();
    drawTitleBar("SD Card Error", "");

    int yPos = 13;
    const char* line1;
    bool cardAnswered = sdCard && sdCard->isInitialized();
    if (!cardAnswered) {
        display->drawString(1, yPos, "No response!", true);
        display->drawString(1, yPos + 10, "Check SD card", true);
        display->drawString(1, 54, "ENTR: retry", true);
        return;
    }
    switch (sdErrorType) {
        case SD_ERROR_EXFAT:       line1 = "exFAT detected!"; break;
        case SD_ERROR_NTFS:        line1 = "NTFS detected!"; break;
        case SD_ERROR_FAT12:
        case SD_ERROR_FAT16:       line1 = "FAT12/16 detected"; break;
        case SD_ERROR_READ_FAILED: line1 = "Read error!"; break;
        case SD_ERROR_UNKNOWN_FS:
        default:                   line1 = "Unknown format!"; break;
    }
    display->drawString(1, yPos, line1, true);
    yPos += 11;
    if (sdErrorType == SD_ERROR_READ_FAILED) {
        display->drawString(1, yPos, "Check SD card", true);
        display->drawString(1, yPos + 9, "or try another", true);
    } else {
        // The card answered, so the built-in formatter can be used
        display->drawString(1, yPos, "Needs FAT32. PC or", true);
        display->drawString(1, yPos + 9, "RET>Config>Format", true);
    }
    display->drawString(1, 54, "ENTR: retry", true);
}
