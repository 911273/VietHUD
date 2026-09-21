#pragma once
#include <lvgl.h>

// Must match the lv_indev_set_long_press_time() call in main's setup().
#define HOLD_PRESS_MS 1000

extern lv_obj_t *dashboardScreen;

void buildDashboard();
void refreshDashboard();
void simTimerCb(lv_timer_t *);
void burnInTimerCb(lv_timer_t *);

// Backlight/burn-in helpers — Settings' brightness/auto-dim controls and the
// touch indev callback (wake-on-touch) both need these.
void applyConfig();
void wakeScreen();

// millis() timestamp of the last touch anywhere in the app (wakeScreen() is
// called from the touch indev callback regardless of which screen is
// active) — Settings' idle-return-to-Dashboard timer reads this so "no
// interaction" means no touch at all, not just no touch on Settings
// specifically (there's only one touch source app-wide, so this is the
// same signal either way, just already tracked here for auto-dim).
uint32_t lastTouchAtMs();
