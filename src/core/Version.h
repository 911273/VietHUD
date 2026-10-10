#pragma once
// Firmware version string — shown in the web portal (System tab, /api/v1/update/state)
// and used by the data manifest's future "min firmware" gate. Bump on every
// release flashed to real devices.
// VietHUD 2.8 has its own OTA channel, so its own release line too (3.1.1 =
// first 2.8 release with auto update check). #ifndef so a bench build can be
// stamped older (-DVIETHUD_FW_VERSION='"3.1.0"') to exercise the update prompt.
#ifndef VIETHUD_FW_VERSION
#if defined(VIETHUD_BOARD_ES3C28P)
#define VIETHUD_FW_VERSION "3.1.1"
#else
#define VIETHUD_FW_VERSION "3.1.0"
#endif
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
#if defined(VIETHUD_BOARD_ES3C28P)
#define VIETHUD_MODEL "VietHUD 2.8"
#define VIETHUD_MODEL_ID "viethud28"
#define VIETHUD_FW_CHANNEL "firmware/viethud28"
#else
#define VIETHUD_MODEL "VietHUD 3.5"
#define VIETHUD_MODEL_ID "viethud35"
#define VIETHUD_FW_CHANNEL "firmware"
#endif
