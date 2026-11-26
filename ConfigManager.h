#ifndef CONFIG_MANAGER_H
#define CONFIG_MANAGER_H

#include <stdint.h>
#include <stdbool.h>
#include "SDCardManager.h"

// Runtime configuration persisted to CONFIG.CFG on the SD card.
typedef struct {
    bool bootLastImage;
    bool weakBit;
    bool soundOn;           // SOUND ON/OFF
    char lastImage[128];    // Last loaded image path, e.g. "/GAMES/LODE.DSK"
} AppConfig;

extern AppConfig g_config;

// Set defaults (SOUND=ON)
void configInit();

// Load config.cfg into g_config (no-op if the card/file is missing).
void configLoad(SDCardManager* sd);

// Save g_config to config.cfg in the ROOT directory (creates it if missing).
// Returns false if the card is missing or the write failed.
bool configSave(SDCardManager* sd);

// Convenience setters that update memory AND write to the SD card.
bool configSetSound(SDCardManager* sd, bool on);
bool configSetLastImage(SDCardManager* sd, const char* path);

// Pointer to the file-name part of a path ("/A/B/X.DSK" -> "X.DSK")
const char* configBaseName(const char* path);


#endif
