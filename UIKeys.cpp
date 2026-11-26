// Key handling of the user interface: what each key does on each page.
//
// A key acts when it goes down. UP and DW repeat while they are held: after
// 0.4 s ten steps per second; in the file list, after 1.5 s, one page every
// 0.2 s. A repeated step never wraps around (it stops at the end of a list),
// a single press does.
// While a message box is shown, any key closes it and does nothing else.
#include "UIHandler.h"
#include "FAT32.h"
#include "Beeper.h"
#include "ConfigManager.h"
#include "SDFormat.h"
#include "pico/time.h"
#include <string.h>
#include <stdio.h>

namespace {
const uint32_t REPEAT_DELAY_MS = 400;
const uint32_t REPEAT_STEP_US = 100000;
const uint32_t PAGE_DELAY_MS = 1500;
const uint32_t PAGE_STEP_US = 200000;

const uint32_t MSG_SHORT_MS = 600;      // "OK"
const uint32_t MSG_INFO_MS = 2000;      // something was done
const uint32_t MSG_ERROR_MS = 4000;     // something went wrong (any key closes it sooner)

// Path of the current directory / of the selected image (the UI runs only on core1)
char s_cwd[256];
char s_fullPath[384];
}

// Show a message box; afterwards (time out or key) the UI continues on `next`
void UIHandler::showMessage(const char* text, uint32_t ms, UIScreen next) {
    currentScreen = UI_SCREEN_LOADING;
    loadingReturnScreen = next;
    renderLoadingScreen(text);
    display->update();
    loadingEndTime = make_timeout_time_ms(ms);
    needsRefresh = true;
}

void UIHandler::goHome() {
    currentScreen = UI_SCREEN_MAIN;
    menuIndex = 0;
    needsRefresh = true;
}

void UIHandler::handleKeyInput() {
    const absolute_time_t now = get_absolute_time();
    bool up = keys->wasPressed(KEY_INDEX_UP);
    bool dw = keys->wasPressed(KEY_INDEX_DW);
    const bool ret = keys->wasPressed(KEY_INDEX_RET);
    const bool entr = keys->wasPressed(KEY_INDEX_ENTR);

    // Auto repeat of UP / DW (only while exactly one of them is held)
    bool repeat = false, page = false;
    if (up || dw) {
        lastRepeatTime = now;
    } else {
        const uint32_t heldDw = keys->heldMs(KEY_INDEX_DW);
        const uint32_t heldUp = keys->heldMs(KEY_INDEX_UP);
        const uint32_t held = heldDw ? heldDw : heldUp;
        if (held >= REPEAT_DELAY_MS && !(heldDw && heldUp)) {
            const bool fast = currentScreen == UI_SCREEN_FILE_LIST && held >= PAGE_DELAY_MS;
            if (absolute_time_diff_us(lastRepeatTime, now) >= (int64_t)(fast ? PAGE_STEP_US : REPEAT_STEP_US)) {
                lastRepeatTime = now;
                repeat = true;
                page = fast;
                (heldDw ? dw : up) = true;
            }
        }
    }

    if (currentScreen == UI_SCREEN_LOADING) {
        if (!repeat && (up || dw || ret || entr)) {
            beepKey();
            loadingEndTime = now;              // update() leaves the message
        }
        return;
    }

    if (up || dw) {
        if (!repeat) beepKey();
        moveSelection(dw ? 1 : -1, repeat, page);
    }
    if (ret) {
        beepKey();
        goBack();
    }
    if (entr) {
        beepKey();
        enterSelection();
    }
}

// UP (-1) / DW (+1)
void UIHandler::moveSelection(int delta, bool repeat, bool page) {
    switch (currentScreen) {
        case UI_SCREEN_FILE_LIST: {
            const bool atTop = selectedIndex <= 0;
            const bool atEnd = selectedIndex + 1 >= (int)fileCount;
            if ((delta < 0 && atTop) || (delta > 0 && atEnd)) {
                if (!repeat) listJump(delta < 0);      // single press: wrap around
            } else if (page) {
                listPageMove(delta);
            } else {
                listMove(delta);
            }
            break;
        }
        case UI_SCREEN_MAIN: {
            const int n = menuIndex + delta;
            if (n >= 0 && n <= 3) menuIndex = n;
            else if (!repeat) menuIndex = (n < 0) ? 3 : 0;
            needsRefresh = true;
            break;
        }
        case UI_SCREEN_CONFIG: {
            const int n = configSelection + delta;
            if (n >= 0 && n <= CONFIG_DEBUGLOG) configSelection = n;
            else if (!repeat) configSelection = (n < 0) ? CONFIG_DEBUGLOG : 0;
            needsRefresh = true;
            break;
        }
        case UI_SCREEN_STATUS:
            statusScroll += delta;                     // clamped to the content when drawn
            if (statusScroll < 0) statusScroll = 0;
            needsRefresh = true;
            break;
        case UI_SCREEN_FORMAT:
            if (!repeat) {
                formatSelection = formatSelection ? 0 : 1;
                needsRefresh = true;
            }
            break;
        default:
            break;
    }
}

// RET
void UIHandler::goBack() {
    switch (currentScreen) {
        case UI_SCREEN_MAIN:
            break;
        case UI_SCREEN_FORMAT:
            currentScreen = UI_SCREEN_CONFIG;
            needsRefresh = true;
            break;
        case UI_SCREEN_FILE_LIST: {
            // In a sub directory: one level up, with the cursor on the directory
            // that was left. In the root directory: back to the home page.
            FAT32* fat32 = sdCard ? sdCard->getFAT32() : nullptr;
            if (fat32) {
                fat32->getCurrentDirectory(s_cwd, sizeof(s_cwd));
                const char* last = strrchr(s_cwd, '/');
                if (last && last[1] != 0) {
                    char left[64];
                    snprintf(left, sizeof(left), "%s", last + 1);
                    if (fat32->changeDirectory("..")) {
                        updateFileList(left, true);
                        needsRefresh = true;
                        break;
                    }
                }
            }
            goHome();
            break;
        }
        default:
            goHome();
            break;
    }
}

// ENTR
void UIHandler::enterSelection() {
    switch (currentScreen) {
        case UI_SCREEN_MAIN:
            if (menuIndex == 0) {
                currentScreen = UI_SCREEN_FILE_LIST;
                // The list is kept while it is valid, so the cursor is where it
                // was. Otherwise read it and start on the loaded image.
                if (listWinCount == 0) {
                    updateFileList(loadedFileName, false);
                }
            } else if (menuIndex == 1) {
                currentScreen = UI_SCREEN_STATUS;
                statusScroll = 0;
            } else if (menuIndex == 2) {
                currentScreen = UI_SCREEN_INFO;
            } else {
                currentScreen = UI_SCREEN_CONFIG;
                configSelection = 0;
            }
            needsRefresh = true;
            break;
        case UI_SCREEN_FILE_LIST: {
            const int row = selectedIndex - scrollOffset;
            FAT32* fat32 = sdCard ? sdCard->getFAT32() : nullptr;
            if (!fat32 || row < 0 || row >= listWinCount || listWin[row].name[0] == 0) {
                break;
            }
            if (listWin[row].isDirectory) {
                char name[64];
                snprintf(name, sizeof(name), "%s", listWin[row].name);
                const bool up = strcmp(name, "..") == 0;
                char left[64] = "";
                if (up) {
                    fat32->getCurrentDirectory(s_cwd, sizeof(s_cwd));
                    const char* last = strrchr(s_cwd, '/');
                    if (last) snprintf(left, sizeof(left), "%s", last + 1);
                }
                if (fat32->changeDirectory(name)) {
                    updateFileList(up ? left : nullptr, true);
                }
                needsRefresh = true;
            } else if (floppy) {
                loadSelectedImage(listWin[row].name);
            }
            break;
        }
        case UI_SCREEN_INFO:
        case UI_SCREEN_STATUS:
            goHome();
            break;
        case UI_SCREEN_NO_SD_CARD:
        case UI_SCREEN_SD_ERROR:
            retrySDCard();
            break;
        case UI_SCREEN_CONFIG:
            activateConfigItem();
            break;
        case UI_SCREEN_FORMAT:
            if (formatSelection == 0) {
                currentScreen = UI_SCREEN_CONFIG;
                needsRefresh = true;
            } else {
                formatCard();
            }
            break;
        default:
            break;
    }
}

// Load the image `name` of the current directory into the drive
void UIHandler::loadSelectedImage(const char* name) {
    FAT32* fat32 = sdCard->getFAT32();
    char filename[64];
    snprintf(filename, sizeof(filename), "%s", name);

    showMessage("Loading...", 60000, UI_SCREEN_FILE_LIST);

    // Full path of the image (for LAST IMAGE in config.cfg)
    fat32->getCurrentDirectory(s_cwd, sizeof(s_cwd));
    int pathLen = snprintf(s_fullPath, sizeof(s_fullPath), "%s/%s", (strcmp(s_cwd, "/") == 0) ? "" : s_cwd, filename);
    if (pathLen <= 0 || pathLen >= (int)sizeof(s_fullPath)) {
        s_fullPath[0] = 0;                 // too long to remember: no auto-load
    }

    // The previous image must be written back and the drive stopped BEFORE the
    // new file overwrites the image buffer
    if (!floppy->ejectImage()) {
        // core0 is still writing the old image back: loading now would corrupt that file
        showMessage(floppy->hasPendingMediaRecovery() ? "SD SAVE PENDING" : "DRIVE BUSY, RETRY", MSG_ERROR_MS,
                    UI_SCREEN_FILE_LIST);
        return;
    }
    if (floppy->takeUnsavedLost()) {
        // Changes of the previous image could not be written to the card
        // (after several attempts): say so before going on
        renderLoadingScreen("LAST SAVE FAILED!");
        display->update();
        sleep_ms(2000);
        renderLoadingScreen("Loading...");
        display->update();
    }

    // The file is opened as a handle: its cluster chain is checked once and all
    // later track writes go through it
    uint32_t bytesRead = 0;
    FAT32_File imageFile;
    bool ok = sdCard->loadDiskImage(filename, floppy->getDiskImage(), floppy->getDiskImageSize(), &bytesRead, &imageFile);
    const char* errorText = "ERROR";
    if (ok) {
        // Name and handle first, so that a track save can never go to the previous file
        floppy->setSDCardManager(sdCard);
        floppy->setCurrentFileName(filename);
        floppy->setImageFile(imageFile);
        floppy->setImagePath(s_fullPath);
        ok = floppy->loadDiskImage(floppy->getDiskImage(), bytesRead);     // checks a WOZ file
        if (!ok) {
            if (floppy->isWoz()) {
                errorText = wozResultText(floppy->getLastWozResult());
            }
            floppy->setCurrentFileName("");                                // the drive stays empty
        }
    }

    if (ok) {
        setLoadedFileName(filename);
        configSetLastImage(sdCard, pathLen < (int)sizeof(g_config.lastImage) ? s_fullPath : "");
        // On to the home page: it shows the drive with the new disk
        menuIndex = 0;
        showMessage("OK", MSG_SHORT_MS, UI_SCREEN_MAIN);
    } else {
        loadedFileName[0] = 0;
        showMessage(errorText, MSG_ERROR_MS, UI_SCREEN_FILE_LIST);
    }
}

// ENTR on a row of the Config page
void UIHandler::activateConfigItem() {
    const char* message = nullptr;
    uint32_t ms = MSG_ERROR_MS;
    static char saved[24];
    switch (configSelection) {
        case CONFIG_SOUND:
        case CONFIG_BOOT:
        case CONFIG_WEAK: {
            bool& value = configSelection == CONFIG_SOUND ? g_config.soundOn :
                          configSelection == CONFIG_BOOT ? g_config.bootLastImage : g_config.weakBit;
            value = !value;
            if (!configSave(sdCard)) {
                value = !value;
                message = "SAVE FAILED";
            }
            if (floppy) floppy->setWeakBits(g_config.weakBit);
            break;
        }
        case CONFIG_PROTECT:
            if (!floppy || !floppy->hasImageLoaded()) {
                message = "NO IMAGE LOADED";
            } else if (!floppy->toggleImageWriteProtection()) {
                message = "PROTECT FAILED";
            } else {
                clearFileList();               // the icon in the list changes
            }
            break;
        case CONFIG_FORMAT:
            currentScreen = UI_SCREEN_FORMAT;
            formatSelection = 0;
            break;
        case CONFIG_DEBUGLOG: {
            const int n = saveDebugLog();
            if (n < 0) {
                message = "LOG FAILED";
            } else {
                snprintf(saved, sizeof(saved), "SAVED WLOG%02d.TXT", n);
                message = saved;
                ms = MSG_INFO_MS;
            }
            break;
        }
    }
    needsRefresh = true;
    if (message) {
        showMessage(message, ms, UI_SCREEN_CONFIG);
    }
}

// Format the SD card (confirmed on the Format page)
void UIHandler::formatCard() {
    renderLoadingScreen("Formatting...");
    display->update();
    bool ok = false;
    int failStep = 0;
    // Stop the emulated drive and write its pending tracks first: the image
    // file is about to disappear. If core0 cannot finish that in time, do NOT
    // format (it may still be writing to the card).
    const bool driveIdle = !floppy || floppy->ejectImage();
    if (!driveIdle) failStep = 12;                                         // drive still busy
    else if (!sdCard || !sdCard->isInitialized()) failStep = 13;           // no card
    if (sdCard && sdCard->isInitialized() && driveIdle) {
        if (floppy) {
            floppy->takeUnsavedLost();                                     // the card is erased anyway
            floppy->setCurrentFileName("");
        }
        loadedFileName[0] = 0;

        ok = formatSDCard(sdCard, &failStep);
        sdCard->remount();
        if (ok && !sdCard->getFAT32()) {
            failStep = 11;                                                 // formatted, but the new volume does not mount
        }
        if (ok && sdCard->getFAT32()) {
            g_config.lastImage[0] = 0;                                     // new card: config.cfg again, no last image
            configSave(sdCard);
        } else {
            ok = false;
        }
    }
    clearFileList();
    char text[24];
    snprintf(text, sizeof(text), ok ? "Format OK" : "Format failed E%d", failStep);
    menuIndex = 0;
    showMessage(text, ok ? MSG_INFO_MS : MSG_ERROR_MS + 2000, UI_SCREEN_MAIN);
}
