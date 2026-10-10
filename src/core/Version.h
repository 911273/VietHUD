#pragma once
// Firmware version string — shown in the web portal (System tab, /api/v1/update/state)
// and used by the data manifest's future "min firmware" gate. Bump on every
// release flashed to real devices.
// Each model has its own release line (BOARD_FW_VERSION in its board config).
// #ifndef so a build can be stamped explicitly (tools/release_firmware.py, or a
// bench build stamped older to exercise the update prompt).
#include "boards/board.h"
#ifndef VIETHUD_FW_VERSION
#define VIETHUD_FW_VERSION BOARD_FW_VERSION
#endif

// Hardware model + OTA channel. VietHUD 2.8 (ES3C28P, 320x240 ILI9341) is a
// separate product from the 3.5" JC3248W535 build: its firmware images are
// NOT interchangeable (different display/touch/audio/pins), so each model
// fetches version.json + firmware.bin from its own folder.
//
// VIETHUD_MODEL_ID must appear as "model" in the channel's version.json or the
// update is refused (FwUpdater.cpp). Folder separation alone wasn't enough:
// 2026-10-10 the Pi's /viethud/firmware/ (the 3.5" channel) was found serving
// VietHUD Lite's version.json, which would have offered Lite's image to a 3.5".
#define VIETHUD_MODEL BOARD_MODEL
#define VIETHUD_MODEL_ID BOARD_MODEL_ID
#define VIETHUD_FW_CHANNEL BOARD_FW_CHANNEL
