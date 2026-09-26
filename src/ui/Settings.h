#pragma once
#include <lvgl.h>

extern lv_obj_t *settingsScreen;

void buildSettingsScreen();

// Re-read every cfg-bound switch (e.g. after the Dashboard's hold-to-mute
// gesture flipped cfg.audioEnabled behind the Settings screen's back).
void settingsSyncSwitches();
