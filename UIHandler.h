#ifndef UI_HANDLER_H
#define UI_HANDLER_H

#include <stdint.h>
#include <stdbool.h>
#include "pico/time.h"
#include "Display.h"
#include "Buttons.h"
#include "FloppyEmulator.h"
#include "SDCardManager.h"
#include "FAT32.h"
#include "PinConfig.h"

// UI Screens
typedef enum {
    UI_SCREEN_MAIN = 0,
    UI_SCREEN_FILE_LIST = 1,
    UI_SCREEN_INFO = 2,
    UI_SCREEN_STATUS = 3,
    UI_SCREEN_LOADING = 4,
    UI_SCREEN_NO_SD_CARD = 5,
    UI_SCREEN_SD_ERROR = 6,
    UI_SCREEN_CONFIG = 7,
    UI_SCREEN_FORMAT = 8
} UIScreen;

// SD Card error types
typedef enum {
    SD_ERROR_NONE = 0,
    SD_ERROR_NOT_PRESENT = 1,
    SD_ERROR_READ_FAILED = 2,
    SD_ERROR_EXFAT = 3,
    SD_ERROR_NTFS = 4,
    SD_ERROR_FAT12 = 5,
    SD_ERROR_FAT16 = 6,
    SD_ERROR_UNKNOWN_FS = 7
} SDErrorType;

class UIHandler {
private:
    Display* display;
    Buttons* keys;
    FloppyEmulator* floppy;
    SDCardManager* sdCard;
    
    UIScreen currentScreen;
    int selectedIndex;       // file list: index of the entry under the cursor
    int menuIndex;           // home page: menu item under the cursor
    int scrollOffset;
    int statusScroll;  // Scroll offset for the SD status screen
    enum ConfigItem { CONFIG_SOUND, CONFIG_BOOT, CONFIG_WEAK, CONFIG_PROTECT, CONFIG_FORMAT, CONFIG_DEBUGLOG };
    int saveDebugLog();   // write WLOGnn.TXT + WTRKnn.BIN; returns nn or -1
    int configSelection;
    int formatSelection;  // 0=NO, 1=YES
    
    // File list state. Only the visible rows are kept in RAM; scrolling past
    // the window reads the neighbouring entry from the card (FAT32::listPage),
    // so directories of any size can be browsed (the old 2 KB text list held
    // at most 64 entries - the first 64 found on the card, not the first 64
    // alphabetically - and silently hid the rest).
    static const int LIST_ROWS_MAX = 6;
    FAT32_ListEntry listWin[LIST_ROWS_MAX];  // entries scrollOffset .. scrollOffset+listWinCount-1
    int listWinCount;
    uint32_t fileCount;
    int maxVisibleItems;
    void clearFileList();
    void listMove(int delta);               // move the selection by one entry (+1 / -1)
    void listJump(bool toEnd);              // wrap around: to the last / first entry
    void listPageMove(int delta);           // one page down / up (key held)

    // Key auto-repeat for UP / DOWN (long lists)
    absolute_time_t lastRepeatTime;
    
    // Loaded file name
    char loadedFileName[64];
    
    // Timing
    absolute_time_t lastUpdateTime;
    bool needsRefresh;
    
    // Loading screen timeout
    absolute_time_t loadingEndTime;
    UIScreen loadingReturnScreen;  // Screen shown when the loading/result message times out
    
    // Backlight auto-off (15s idle)
    bool backlightOn;
    absolute_time_t lastKeyActivityTime;
    static const uint32_t BACKLIGHT_TIMEOUT_US = 15000000;  // 15 s
    
    // Boot logo: main() shows the still picture before the SD card is read,
    // update() animates it for bootLogoDurationMs() (any key skips it)
    bool splashActive;
    absolute_time_t splashStart;

    // Scrolling of a text that is wider than its place (see marquee())
    uint32_t marqueeSig;
    absolute_time_t marqueeStart;
    const char* marquee(const char* text, int window, char* out, int outSize);

    // Home page: write activity indicator
    uint32_t lastCaptures;
    absolute_time_t writeShownUntil;

    // Title of the file list: name of the current directory
    char listTitle[14];

    // Screen rendering (UIRender.cpp)
    void drawTitleBar(const char* title, const char* right);
    void renderMainScreen();
    void renderFileListScreen();
    void renderInfoScreen();
    void renderStatusScreen();
    void renderConfigScreen();
    void renderFormatScreen();
    void renderLoadingScreen(const char* message);
    void renderNoSDCardScreen();
    void renderSDErrorScreen();
    
    // SD Card error state
    SDErrorType sdErrorType;
    
    // Navigation
    void handleKeyInput();
    void updateFileList(const char* focusName, bool focusIsDir);
    void moveSelection(int delta, bool repeat, bool page);   // UP / DW
    void goBack();                                           // RET
    void enterSelection();                                   // ENTR
    void goHome();
    void showMessage(const char* text, uint32_t ms, UIScreen next);
    void loadSelectedImage(const char* name);
    void activateConfigItem();
    void formatCard();
    void retrySDCard();
    bool mediaMissing = false;
    bool waitForKeyRelease = false;
    bool previousDetect = false;
    unsigned missedProbes = 0;
    absolute_time_t nextMediaCheck = {};
    void pollSDCard();
    
public:
    UIHandler(Display* disp, Buttons* keys);
    ~UIHandler();
    
    void init();
    static void drawBootLogo(Display* d, uint32_t elapsedMs);
    static uint32_t bootLogoDurationMs();
    void setFloppyEmulator(FloppyEmulator* floppyEmu);
    void setSDCardManager(SDCardManager* sdCardMgr);
    void setLoadedFileName(const char* name);
    
    void update();  // Call this periodically in main loop
    
    // SD Card status screens
    void showNoSDCard();
    void showSDError(SDErrorType errorType);
    void showMainMenu();  // Return to main menu after SD card is ready
};

#endif // UI_HANDLER_H

