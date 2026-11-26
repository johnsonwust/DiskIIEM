#include "ConfigManager.h"
#include "FAT32.h"
#include "PinConfig.h"
#include "hardware/gpio.h"
#include <string.h>
#include <stdio.h>

AppConfig g_config;

void configInit() {
    g_config.soundOn = true;
    g_config.bootLastImage = true;
    g_config.weakBit = true;
    memset(g_config.lastImage, 0, sizeof(g_config.lastImage));
}

static void trimTrailing(char* s) {
    int n = strlen(s);
    while (n > 0 && (s[n - 1] == '\r' || s[n - 1] == '\n' || s[n - 1] == ' ')) {
        s[--n] = 0;
    }
}

// Keys of config.cfg (bit per key, to find keys missing from the file)
enum {
    KEY_SOUND = 1, KEY_BOOT = 2, KEY_WEAK = 4, KEY_LAST = 8,
    KEY_ALL = KEY_SOUND | KEY_BOOT | KEY_WEAK | KEY_LAST
};

// Parse one "[KEY]=VALUE" line; returns the key found (0 = none)
static int parseLine(const char* line) {
    if (strncmp(line, "[SOUND]=", 8) == 0) {
        g_config.soundOn = (line[8] == '1');
        return KEY_SOUND;
    } else if (strncmp(line, "[BOOT LAST IMAGE]=", 18) == 0) {
        g_config.bootLastImage = (line[18] == '1');
        return KEY_BOOT;
    } else if (strncmp(line, "[WEAK BIT]=", 11) == 0) {
        g_config.weakBit = (line[11] == '1');
        return KEY_WEAK;
    } else if (strncmp(line, "[LAST IMAGE]=", 13) == 0) {
        strncpy(g_config.lastImage, line + 13, sizeof(g_config.lastImage) - 1);
        g_config.lastImage[sizeof(g_config.lastImage) - 1] = 0;
        return KEY_LAST;
    }
    return 0;
}

// Read config.cfg from the root directory into g_config. Called at power-on
// and every time a card has been (re)initialised. If the file does not exist,
// or lacks any of the settings (e.g. written by an older firmware), it is
// written at once with all current settings (values from the file where
// present, the current ones otherwise).
void configLoad(SDCardManager* sd) {
    if (!sd || !sd->isInitialized()) return;

    FAT32* fat32 = sd->getFAT32();
    if (!fat32) return;

    static uint8_t buffer[512];
    uint32_t bytesRead = 0;
    // Always read from the ROOT directory (same place configSave() writes to)
    if (!fat32->readSmallFile("CONFIG.CFG", buffer, sizeof(buffer) - 1, &bytesRead)) {
        // config.cfg missing or empty: create it with the current settings
        configSave(sd);
        return;
    }
    buffer[bytesRead] = 0;

    // Split into lines and parse each (skip a UTF-8 BOM if the file was edited on a PC)
    char* line = (char*)buffer;
    if (bytesRead >= 3 && buffer[0] == 0xEF && buffer[1] == 0xBB && buffer[2] == 0xBF) {
        line += 3;
    }
    int found = 0;
    while (line && *line) {
        char* nl = strchr(line, '\n');
        if (nl) *nl = 0;
        trimTrailing(line);
        if (*line) found |= parseLine(line);
        line = nl ? nl + 1 : NULL;
    }
    if (found != KEY_ALL) {
        configSave(sd);   // complete the file with every setting
    }
}

bool configSave(SDCardManager* sd) {
    if (!sd || !sd->isInitialized()) return false;
    FAT32* fat32 = sd->getFAT32();
    if (!fat32) return false;

    static char content[256];
    snprintf(content, sizeof(content),
             "[SOUND]=%d\n[BOOT LAST IMAGE]=%d\n[WEAK BIT]=%d\n[LAST IMAGE]=%s\n",
             g_config.soundOn ? 1 : 0,
             g_config.bootLastImage ? 1 : 0,
             g_config.weakBit ? 1 : 0,
             g_config.lastImage[0] ? g_config.lastImage : "");

    return fat32->writeSmallFile("CONFIG.CFG", (const uint8_t*)content, (uint32_t)strlen(content));
}

bool configSetSound(SDCardManager* sd, bool on) {
    g_config.soundOn = on;
    return configSave(sd);
}

bool configSetLastImage(SDCardManager* sd, const char* path) {
    if (!path) path = "";
    strncpy(g_config.lastImage, path, sizeof(g_config.lastImage) - 1);
    g_config.lastImage[sizeof(g_config.lastImage) - 1] = 0;
    return configSave(sd);
}

const char* configBaseName(const char* path) {
    if (!path) return "";
    const char* base = path;
    for (const char* p = path; *p; p++) {
        if (*p == '/' || *p == '\\') base = p + 1;
    }
    return base;
}
