#pragma once
// Board selection — one place maps the env's -DVIETHUD_BOARD_* flag to that
// board's configuration (pins, panel, identity, capability flags). Shared code
// includes this (or the legacy pincfg.h / dispcfg.h shims) and keys off the
// BOARD_* capability macros, never off a board name.
//
// Adding a board: include/boards/<name>/board_config.h + src/boards/<name>/
// (board.cpp implementing core/Board.h), a branch here, and an env in
// platformio.ini with -DVIETHUD_BOARD_<NAME> and +<boards/<name>/*>.
#if defined(VIETHUD_BOARD_ES3C28P)
#include "boards/es3c28p/board_config.h"
#elif defined(VIETHUD_BOARD_LITE154)
#include "boards/lite154/board_config.h"
#elif defined(VIETHUD_BOARD_JC4880P443C)
#include "boards/jc4880p443c/board_config.h"
#elif defined(VIETHUD_BOARD_JC3248W535) || !defined(VIETHUD_BOARD_DEFINED_ELSEWHERE)
// JC3248W535 is also the default, so the bring-up/rawtest envs (no flag) keep working.
#include "boards/jc3248w535/board_config.h"
#endif

// Defaults for capabilities most boards don't have, so a board config only
// lists what it actually has.
#ifndef BOARD_SD_4BIT
#define BOARD_SD_4BIT 0 // 1-bit SD_MMC (CLK/CMD/D0)
#endif
#ifndef BOARD_SD_LDO_CHAN
#define BOARD_SD_LDO_CHAN -1 // card power not from an on-chip LDO
#endif
#ifndef BOARD_FLASH_DATA_READONLY
#define BOARD_FLASH_DATA_READONLY 0
#endif
// Online data channel (folder on the Pi / GitHub) this board updates from:
// "speedmap" = the full shared set (card boards); a flash-only board gets the
// cut of the same set that fits its partition (tools/build_flash_data.py).
// Screen family: 0 = the LVGL dashboard with the live map (3.5", 2.8", 4.3");
// 1 = the Lite's own plain-GFX HUD, no LVGL, no map (src/boards/lite154/ui).
#ifndef BOARD_UI_HIGH_CONTRAST // Settings palette: true-black panels, brighter text (ui/Settings.cpp uiColor)
#define BOARD_UI_HIGH_CONTRAST 0
#endif
#ifndef BOARD_UI_LITE
#define BOARD_UI_LITE 0
#endif
#ifndef BOARD_HAS_MAP
#define BOARD_HAS_MAP (!BOARD_UI_LITE)
#endif
#ifndef BOARD_DATA_PROFILE
#define BOARD_DATA_PROFILE "speedmap"
#endif
#ifndef BOARD_DATA_URL // "" = no online speed-map/alert channel for this board
#define BOARD_DATA_URL "http://192.168.1.65/viethud/" BOARD_DATA_PROFILE "/" // Pi (net/DataSources.h)
#endif
// Road-name channel (net/NamesUpdater.h): OSM street names, own signed manifest.
#ifndef BOARD_NAMES_CHANNEL
#define BOARD_NAMES_CHANNEL "roadnames"
#endif
