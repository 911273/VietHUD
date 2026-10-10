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
#elif defined(VIETHUD_BOARD_JC3248W535) || !defined(VIETHUD_BOARD_DEFINED_ELSEWHERE)
// JC3248W535 is also the default, so the bring-up/rawtest envs (no flag) keep working.
#include "boards/jc3248w535/board_config.h"
#endif
