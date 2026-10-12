#pragma once
// Firmware version — ONE platform version for every VietHUD model (3.5", 2.8",
// Lite 1.54", 4.3" P4): all models are built from the same core at the same
// commit and released together by tools/release.py, so a version number means
// the same feature set on every model. Shown in the web portal / System screen
// and compared against the OTA channel's version.json (FwUpdater.cpp).
// Bump here on every release (tools/release.py checks it matches its argument).
// #ifndef so a build can be stamped explicitly (a bench build stamped older to
// exercise the update prompt).
#include "boards/board.h"
#ifndef VIETHUD_FW_VERSION
#define VIETHUD_FW_VERSION "4.0.0"
#endif

// Hardware model + OTA channel (include/boards/<board>/board_config.h). The
// firmware IMAGES are per model (different display/touch/audio/pins), so each
// model fetches version.json + firmware.bin from its own folder
// firmware/<model id>/ — same version number, different binary.
//
// VIETHUD_MODEL_ID must appear as "model" in the channel's version.json or the
// update is refused (FwUpdater.cpp). Folder separation alone wasn't enough:
// 2026-10-10 the Pi's /viethud/firmware/ (the 3.5" channel) was found serving
// VietHUD Lite's version.json, which would have offered Lite's image to a 3.5".
#define VIETHUD_MODEL BOARD_MODEL
#define VIETHUD_MODEL_ID BOARD_MODEL_ID
#define VIETHUD_FW_CHANNEL BOARD_FW_CHANNEL
