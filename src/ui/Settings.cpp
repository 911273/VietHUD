#include "Settings.h"
#include "Dashboard.h" // dashboardScreen (Back button), applyConfig()
#include "display/DisplayDriver.h" // gfx->width()/height() — orientation-aware layout
#include "core/AppConfig.h"
#include "core/NvsStore.h"
#include "core/SharedState.h" // gnssSnapshot()/roadInfoSnapshot() for the Sensors diagnostics panel
#include "demo/DemoMode.h"    // scripted UI demo — Settings > Display switch
#include "map/RasterMapManager.h"
#include "map/SpeedLimitManager.h" // speedSourceStr() — Speed Map group in the Sensors tab
#include "net/WebPortal.h"    // webPortalIsEnabled()/webPortalRequestEnable() — WiFi tab
#include "net/DataUpdater.h"  // dataUpdateStart()/GetStatus() — WiFi tab "Update data" button
#include <string.h>

// ---------------------------------------------------------------------
// Settings screen (spec sections 16-17, 55-61)
// ---------------------------------------------------------------------
struct SliderBinding {
    lv_obj_t *slider;
    lv_obj_t *valLabel;
    float *target;
    float divisor;
    const char *unit;
};
// 7 in use (Brightness, Dim-after-stopped, GNSS speed-filter smoothing, GNSS
// fix timeout, GPS speed calibration, speed-limit-ahead warn distance,
// camera warn distance — the last 3 added 2026-09-22) after radar removal
// 2026-09-21 dropped the ~19 radar-only slider rows this array used to size
// against (was 32 wide for that reason — see git history). Checked by
// counting addSliderRow() call sites directly rather than trusting an old
// comment.
static SliderBinding sliderBindings[12]; // bumped from 8 (2026-09-24): 9 slider rows now (added Overspeed offset + Default limit)
static int sliderCount = 0;

struct SwitchBinding {
    lv_obj_t *sw;
    bool *target;
};
// 2 in use (Alert audio enabled, Trip logging) after radar removal
// 2026-09-21 dropped the radar-only demo-mode/mount-flip switches this
// array used to size against (was 12 wide for that reason).
static SwitchBinding switchBindings[10];
static int switchCount = 0;

static lv_obj_t *settingsStatusLabel;
static lv_obj_t *restartConfirmOverlay = nullptr;

// WiFi tab widgets — declared up top since onWifiSwitchChanged()/
// onWifiKbReadyOrCancel() below (both fairly early in the file) reference
// them; built in buildSettingsScreen() near the bottom, same
// declare-early/build-late split refreshSensorsPanel()'s own widget
// pointers already use.
static lv_obj_t *wifiEnableSwitch, *wifiSsidTa, *wifiPasswordTa;
static lv_obj_t *staSsidTa = nullptr, *staPasswordTa = nullptr; // Internet (station) creds — WiFi tab
static lv_obj_t *staStatusVal = nullptr;                        // STA connection info row
// "WiFi setup" overlay: scan nearby networks + pick one + enter password, on a
// non-scrolling full-screen modal so the on-screen keyboard works reliably
// (2026-09-25 — user couldn't type a password on the scrollable WiFi tab).
static lv_obj_t *wifiScanOverlay = nullptr, *scanList = nullptr, *scanStatusLbl = nullptr;
static lv_obj_t *scanSelLbl = nullptr, *scanPassTa = nullptr;
static char g_selectedStaSsid[33] = "";
static int g_lastRenderedScan = -99;
// Demo mode (Settings > Display) — like WiFi's switch above, deliberately not
// an AppConfig/NVS field, so it can never survive a reboot into real driving.
// See demo/DemoMode.h.
static lv_obj_t *demoEnableSwitch, *demoSceneVal;

// Formats a settings value: no decimal for whole numbers ("100 m", "3 min"),
// one decimal otherwise ("0.3", "1.5"). Cleaner than the old always-"%.1f"
// which showed "100.0 m" / "3.0 min" (2026-09-24 UI polish for glanceability).
static void fmtSettingVal(char *buf, size_t n, float v, const char *unit) {
    if (v == (float)(long)v) snprintf(buf, n, "%ld%s", (long)v, unit ? unit : "");
    else snprintf(buf, n, "%.1f%s", (double)v, unit ? unit : "");
}


static void onSliderChanged(lv_event_t *e) {
    SliderBinding *b = (SliderBinding *)lv_event_get_user_data(e);
    int32_t raw = lv_slider_get_value(b->slider);
    *(b->target) = raw / b->divisor;
    char buf[24];
    fmtSettingVal(buf, sizeof(buf), *(b->target), b->unit);
    lv_label_set_text(b->valLabel, buf);
    lv_label_set_text(settingsStatusLabel, "");
    clampConfig(cfg);
    // applyConfig() re-writes the backlight PWM duty — only the brightness
    // slider needs that, but this used to fire on EVERY slider's every
    // drag tick (an LVGL slider fires VALUE_CHANGED many times per drag),
    // so dragging e.g. "Max range" was rewriting the backlight duty
    // dozens of times for no reason. User-reported 2026-09-15 as Settings
    // feeling laggy.
    if (b->target == &cfg.brightness || b->target == &cfg.audioVolume) applyConfig(); // both are pushed to hardware in applyConfig()
    // Rotation can't apply live (see AppConfig.h's screenRotation comment —
    // both screens are laid out once at boot for whichever orientation was
    // active then) — say so immediately rather than let the slider silently
    // do nothing, which is what every OTHER slider here does instead.
    if (b->target == &cfg.screenRotation) {
        saveConfigToNVS(cfg);
        lv_label_set_text(settingsStatusLabel, "Da luu xoay man hinh! Khoi dong lai de ap dung...");
        if (restartConfirmOverlay) {
            lv_obj_clear_flag(restartConfirmOverlay, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void onSwitchChanged(lv_event_t *e) {
    SwitchBinding *b = (SwitchBinding *)lv_event_get_user_data(e);
    *(b->target) = lv_obj_has_state(b->sw, LV_STATE_CHECKED);
    clampConfig(cfg); // no switch affects brightness, so no applyConfig() call needed here
    // Persist immediately so toggles (esp. the map mode: JPEG/vector/heading-up)
    // survive a reboot without needing the footer Save — user-requested
    // 2026-09-24 ("ghi nhớ lưu chọn bản đồ ... sau khi khởi động"). Switches are
    // discrete/infrequent, so a NVS write per toggle is fine (unlike sliders).
    saveConfigToNVS(cfg);
    lv_label_set_text(settingsStatusLabel, "Da luu");
}

// Not wired through the generic SwitchBinding/onSwitchChanged above:
// WiFi's on/off state isn't an AppConfig/NVS field (see AppConfig.h's
// wifiSsid/wifiPassword comment — only credentials persist, never on/off),
// so there's no `bool *target` in cfg for the generic binding to point at.
// This talks to net/WebPortal.h directly instead, same call the Dashboard's
// WiFi hold gesture (4s tier) makes.
static void onWifiSwitchChanged(lv_event_t *e) {
    bool on = lv_obj_has_state(wifiEnableSwitch, LV_STATE_CHECKED);
    webPortalRequestEnable(on);
    Serial.printf("[uidemo] WiFi %s via Settings switch\n", on ? "ON" : "OFF");
}

// Same non-AppConfig treatment as WiFi's switch above — see its comment and
// demo/DemoMode.h for why this state must not persist.
static void onDemoSwitchChanged(lv_event_t *) {
    // Diagnostic print (user-reported 2026-09-22: "gat Demo mode khong an,
    // Settings tu thoat" — a switch tap seemingly doing nothing followed by
    // the 5s idle-return firing, which only happens if NO touch registered
    // at all in that window, not even a miss elsewhere on screen). This
    // confirms whether the tap is reaching this handler at all, before
    // assuming the ext_click_area enlargement just below is the real fix.
    Serial.println("[settings] Demo mode switch event fired");
    demoModeSetEnabled(lv_obj_has_state(demoEnableSwitch, LV_STATE_CHECKED));
}

// On-screen keyboard for wifiSsidTa/wifiPasswordTa — this screen's first use
// of lv_textarea/lv_keyboard (every other input here is a slider/switch, no
// text entry has ever been needed before). Bound to LV_EVENT_CLICKED rather
// than LV_EVENT_FOCUSED: LVGL's focus/group model is built around encoder-
// style indevs, and this app only ever has a pointer/touch indev with no
// lv_group in use elsewhere — CLICKED is unambiguous regardless of that and
// is exactly the gesture a touchscreen keyboard should respond to anyway.
static lv_obj_t *wifiKeyboard;

static void onWifiTaClicked(lv_event_t *e) {
    lv_obj_t *ta = lv_event_get_target_obj(e);
    lv_keyboard_set_textarea(wifiKeyboard, ta);
    lv_obj_clear_flag(wifiKeyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(wifiKeyboard);
}

// Commits whichever textarea the keyboard is currently bound to into cfg —
// called on the keyboard's own Ready/Cancel (both close it; Cancel doesn't
// revert here since there's no separate "draft" copy to revert TO, same
// as every slider/switch in this screen applying live and only NVS-
// persisting on the footer's Save button).
static void onWifiKbReadyOrCancel(lv_event_t *) {
    lv_obj_t *ta = lv_keyboard_get_textarea(wifiKeyboard);
    if (ta == wifiSsidTa) {
        strncpy(cfg.wifiSsid, lv_textarea_get_text(ta), sizeof(cfg.wifiSsid) - 1);
        cfg.wifiSsid[sizeof(cfg.wifiSsid) - 1] = '\0';
    } else if (ta == wifiPasswordTa) {
        strncpy(cfg.wifiPassword, lv_textarea_get_text(ta), sizeof(cfg.wifiPassword) - 1);
        cfg.wifiPassword[sizeof(cfg.wifiPassword) - 1] = '\0';
    } else if (ta == staSsidTa) {
        strncpy(cfg.staSsid, lv_textarea_get_text(ta), sizeof(cfg.staSsid) - 1);
        cfg.staSsid[sizeof(cfg.staSsid) - 1] = '\0';
    } else if (ta == staPasswordTa || ta == scanPassTa) {
        strncpy(cfg.staPassword, lv_textarea_get_text(ta), sizeof(cfg.staPassword) - 1);
        cfg.staPassword[sizeof(cfg.staPassword) - 1] = '\0';
    }
    lv_keyboard_set_textarea(wifiKeyboard, NULL);
    lv_obj_add_flag(wifiKeyboard, LV_OBJ_FLAG_HIDDEN);
}

// ---- "WiFi setup" overlay: scan + pick + password + connect ----
static void onScanItemClicked(lv_event_t *e) {
    lv_obj_t *btn = lv_event_get_target_obj(e);
    int idx = (int)(intptr_t)lv_obj_get_user_data(btn);
    if (webPortalScanResult(idx, g_selectedStaSsid, sizeof(g_selectedStaSsid), nullptr, nullptr)) {
        if (scanSelLbl) {
            char b[48];
            snprintf(b, sizeof(b), "Chon: %s", g_selectedStaSsid);
            lv_label_set_text(scanSelLbl, b);
        }
    }
}
static void onWifiScanBtnClicked(lv_event_t *) {
    webPortalRequestEnable(true); // scanning needs the radio on
    webPortalStartScan();
    g_lastRenderedScan = -99;
    if (scanStatusLbl) lv_label_set_text(scanStatusLbl, "Dang quet...");
    if (scanList) lv_obj_clean(scanList);
}
static void onWifiScanOpen(lv_event_t *) {
    lv_obj_clear_flag(wifiScanOverlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(wifiScanOverlay);
    g_selectedStaSsid[0] = '\0';
    if (scanSelLbl) lv_label_set_text(scanSelLbl, "Chon: (chua chon)");
    if (scanPassTa) lv_textarea_set_text(scanPassTa, "");
    onWifiScanBtnClicked(nullptr); // auto-scan on open
}
static void onWifiScanClose(lv_event_t *) {
    lv_obj_add_flag(wifiKeyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(wifiScanOverlay, LV_OBJ_FLAG_HIDDEN);
}
static void onWifiScanConnect(lv_event_t *) {
    if (g_selectedStaSsid[0] == '\0') {
        if (scanStatusLbl) lv_label_set_text(scanStatusLbl, "Chua chon mang!");
        return;
    }
    strncpy(cfg.staSsid, g_selectedStaSsid, sizeof(cfg.staSsid) - 1);
    cfg.staSsid[sizeof(cfg.staSsid) - 1] = '\0';
    const char *pw = lv_textarea_get_text(scanPassTa);
    if (pw && pw[0]) { // blank = keep existing password
        strncpy(cfg.staPassword, pw, sizeof(cfg.staPassword) - 1);
        cfg.staPassword[sizeof(cfg.staPassword) - 1] = '\0';
    }
    saveConfigToNVS(cfg);
    webPortalReconnectSta(); // apply the new station creds on the web task
    if (staSsidTa) lv_textarea_set_text(staSsidTa, cfg.staSsid); // reflect in the manual field
    onWifiScanClose(nullptr);
}

static void buildWifiScanOverlay(lv_obj_t *parent) {
    wifiScanOverlay = lv_obj_create(parent);
    lv_obj_set_pos(wifiScanOverlay, 0, 0);
    lv_obj_set_size(wifiScanOverlay, gfx->width(), gfx->height());
    lv_obj_set_style_bg_color(wifiScanOverlay, lv_color_hex(0x0B0F14), 0);
    lv_obj_set_style_bg_opa(wifiScanOverlay, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(wifiScanOverlay, 0, 0);
    lv_obj_set_style_radius(wifiScanOverlay, 0, 0);
    lv_obj_clear_flag(wifiScanOverlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(wifiScanOverlay, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *title = lv_label_create(wifiScanOverlay);
    lv_label_set_text(title, "Ket noi Internet (WiFi)");
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_set_pos(title, 8, 6);

    lv_obj_t *rescan = lv_button_create(wifiScanOverlay);
    lv_obj_set_size(rescan, 90, 30);
    lv_obj_align(rescan, LV_ALIGN_TOP_RIGHT, -8, 4);
    lv_obj_add_event_cb(rescan, onWifiScanBtnClicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *rescanLbl = lv_label_create(rescan);
    lv_label_set_text(rescanLbl, "Quet lai");
    lv_obj_center(rescanLbl);

    scanStatusLbl = lv_label_create(wifiScanOverlay);
    lv_label_set_text(scanStatusLbl, "");
    lv_obj_set_style_text_color(scanStatusLbl, lv_color_hex(0x9FB2C6), 0);
    lv_obj_set_pos(scanStatusLbl, 8, 30);

    // Left: scrollable list of found networks.
    scanList = lv_list_create(wifiScanOverlay);
    lv_obj_set_pos(scanList, 4, 50);
    lv_obj_set_size(scanList, 280, gfx->height() - 58);
    lv_obj_set_style_bg_color(scanList, lv_color_hex(0x151C24), 0);

    // Right column: selected + password + buttons.
    int rx = 292;
    scanSelLbl = lv_label_create(wifiScanOverlay);
    lv_label_set_text(scanSelLbl, "Chon: (chua chon)");
    lv_obj_set_style_text_color(scanSelLbl, lv_color_hex(0xCCD6E0), 0);
    lv_obj_set_width(scanSelLbl, gfx->width() - rx - 6);
    lv_label_set_long_mode(scanSelLbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(scanSelLbl, rx, 50);

    scanPassTa = lv_textarea_create(wifiScanOverlay);
    lv_textarea_set_one_line(scanPassTa, true);
    lv_textarea_set_password_mode(scanPassTa, true);
    lv_textarea_set_max_length(scanPassTa, sizeof(cfg.staPassword) - 1);
    lv_textarea_set_placeholder_text(scanPassTa, "Mat khau");
    lv_obj_set_pos(scanPassTa, rx, 90);
    lv_obj_set_size(scanPassTa, gfx->width() - rx - 6, 30);
    lv_obj_add_event_cb(scanPassTa, onWifiTaClicked, LV_EVENT_CLICKED, NULL);

    lv_obj_t *connBtn = lv_button_create(wifiScanOverlay);
    lv_obj_set_size(connBtn, gfx->width() - rx - 6, 34);
    lv_obj_set_pos(connBtn, rx, 128);
    lv_obj_set_style_bg_color(connBtn, lv_color_hex(0x2E7D4F), 0);
    lv_obj_add_event_cb(connBtn, onWifiScanConnect, LV_EVENT_CLICKED, NULL);
    lv_obj_t *connLbl = lv_label_create(connBtn);
    lv_label_set_text(connLbl, "Ket noi");
    lv_obj_center(connLbl);

    lv_obj_t *closeBtn = lv_button_create(wifiScanOverlay);
    lv_obj_set_size(closeBtn, gfx->width() - rx - 6, 30);
    lv_obj_set_pos(closeBtn, rx, 166);
    lv_obj_set_style_bg_color(closeBtn, lv_color_hex(0x44505C), 0);
    lv_obj_add_event_cb(closeBtn, onWifiScanClose, LV_EVENT_CLICKED, NULL);
    lv_obj_t *closeLbl = lv_label_create(closeBtn);
    lv_label_set_text(closeLbl, "Dong");
    lv_obj_center(closeLbl);
}

// Rebuilds the scan list when a scan finishes (called from refreshSensorsPanel
// while the overlay is open). Kept out of the WiFi task — only reads the cached
// results via webPortalScanResult().
static void refreshScanListIfOpen() {
    if (!wifiScanOverlay || lv_obj_has_flag(wifiScanOverlay, LV_OBJ_FLAG_HIDDEN)) return;
    int st = webPortalScanState();
    if (st == -2) {
        if (scanStatusLbl) lv_label_set_text(scanStatusLbl, "Dang quet...");
        return;
    }
    if (st >= 0 && st != g_lastRenderedScan) {
        g_lastRenderedScan = st;
        lv_obj_clean(scanList);
        for (int i = 0; i < st; i++) {
            char ssid[33];
            int rssi = 0;
            bool locked = false;
            webPortalScanResult(i, ssid, sizeof(ssid), &rssi, &locked);
            char item[64];
            snprintf(item, sizeof(item), "%s  %s%ddBm", ssid, locked ? "* " : "", rssi);
            lv_obj_t *b = lv_list_add_button(scanList, NULL, item);
            lv_obj_set_user_data(b, (void *)(intptr_t)i);
            lv_obj_add_event_cb(b, onScanItemClicked, LV_EVENT_CLICKED, NULL);
        }
        char s[40];
        snprintf(s, sizeof(s), st ? "Tim thay %d mang (* = co khoa)" : "Khong thay mang 2.4GHz", st);
        if (scanStatusLbl) lv_label_set_text(scanStatusLbl, s);
    }
}

// Row layout tuned for the ~372px-wide / 260px-tall LANDSCAPE category
// content panel (see buildSettingsScreen) — panels don't scroll (except the
// Sensors tab), so every category's total row count x pitch must stay under
// 260px there. Tightened from 30/24px to 26/22px 2026-09-14 (originally for
// the now-removed Radar tab); re-check this budget before adding more rows
// to any tab.
//
// Width-aware (user-requested 2026-09-15, screen rotation): portrait's
// content panel is ~220px wide instead of landscape's ~380px — too narrow
// for name+slider+value on one line at the fixed x=148/x=306 offsets below,
// so under a threshold this stacks the slider+value onto their own line
// under the name instead of computing fractional widths from an arbitrary
// panel size. Landscape's own numbers are UNCHANGED (same offsets as
// before) specifically so the already-verified landscape layout stays
// pixel-identical — this is an ADDED case, not a generalization that also
// touches the existing one.
static const int kNarrowPanelThreshold = 300;

static void addSliderRow(lv_obj_t *parent, int &y, const char *name, float *target, int32_t minRaw, int32_t maxRaw,
                          float divisor, const char *unit) {
    bool narrow = lv_obj_get_width(parent) < kNarrowPanelThreshold;

    lv_obj_t *nameLbl = lv_label_create(parent);
    lv_label_set_text(nameLbl, name);
    lv_obj_set_style_text_color(nameLbl, lv_color_hex(0xCCD6E0), 0);

    lv_obj_t *slider = lv_slider_create(parent);
    lv_slider_set_range(slider, minRaw, maxRaw);
    lv_slider_set_value(slider, (int32_t)((*target) * divisor), LV_ANIM_OFF);

    lv_obj_t *valLbl = lv_label_create(parent);
    lv_obj_set_style_text_color(valLbl, lv_color_white(), 0);

    int rowH;
    if (narrow) {
        lv_obj_set_pos(nameLbl, 4, y);
        int sliderW = lv_obj_get_width(parent) - 4 - 54 - 6; // left margin, value column, gap
        if (sliderW < 60) sliderW = 60; // floor for an extreme-case panel — stays usable, may just crowd the value column
        lv_obj_set_size(slider, sliderW, 10);
        lv_obj_set_pos(slider, 4, y + 19);
        lv_obj_set_pos(valLbl, lv_obj_get_width(parent) - 54, y + 16);
        rowH = 38;
    } else {
        lv_obj_set_size(slider, 150, 10);
        lv_obj_set_pos(nameLbl, 4, y + 5);
        lv_obj_set_pos(slider, 148, y + 8);
        lv_obj_set_pos(valLbl, 306, y + 5);
        rowH = 26;
    }

    SliderBinding *b = &sliderBindings[sliderCount++];
    b->slider = slider;
    b->valLabel = valLbl;
    b->target = target;
    b->divisor = divisor;
    b->unit = unit;
    lv_obj_add_event_cb(slider, onSliderChanged, LV_EVENT_VALUE_CHANGED, b);

    char buf[24];
    fmtSettingVal(buf, sizeof(buf), *target, unit);
    lv_label_set_text(valLbl, buf);

    y += rowH;
}

// Small fixed set of named choices (Theme: Auto/Light/Dark, Rotation:
// 0/90/180/270) — a slider hides which discrete value is active and makes
// picking a specific one fiddly (user-reported 2026-09-16: "nut gat chua
// hop ly", asked for tick/choice buttons per option instead). Renders one
// row of `count` checkable buttons, all mutually exclusive (manual
// check/uncheck below — LVGL has no built-in radio-group for plain buttons).
struct ChoiceBinding {
    lv_obj_t *btns[4]; // 4 covers every current use (Theme=3, Rotation=4, Direction=3)
    int count;
    float *target;
};
static ChoiceBinding choiceBindings[8]; // 3 in use as of Direction (2026-09-21) — 1 slot free
static int choiceCount = 0;

static void onChoiceBtnClicked(lv_event_t *e) {
    lv_obj_t *clicked = lv_event_get_target_obj(e);
    ChoiceBinding *b = (ChoiceBinding *)lv_event_get_user_data(e);
    for (int i = 0; i < b->count; i++) {
        if (b->btns[i] == clicked) {
            *b->target = (float)i;
            lv_obj_add_state(b->btns[i], LV_STATE_CHECKED);
        } else {
            lv_obj_clear_state(b->btns[i], LV_STATE_CHECKED);
        }
    }
    clampConfig(cfg);
    lv_label_set_text(settingsStatusLabel, "");
    // Rotation can't apply live (see AppConfig.h's screenRotation comment —
    // both screens are laid out once at boot for whichever orientation was
    // active then) — say so immediately rather than let the button silently
    // do nothing until the next restart.
    if (b->target == &cfg.screenRotation) {
        saveConfigToNVS(cfg);
        lv_label_set_text(settingsStatusLabel, "Da luu xoay man hinh! Khoi dong lai de ap dung...");
        if (restartConfirmOverlay) {
            lv_obj_clear_flag(restartConfirmOverlay, LV_OBJ_FLAG_HIDDEN);
        }
    }
    // Map source IS a choice row (addChoiceRow at the Map tab), so its apply
    // logic must live HERE, not in onSliderChanged — it used to be only in the
    // slider handler, which this choice row never dispatches to, so picking a
    // map source silently did nothing (found in the 2026-09-24 UI audit).
    if (b->target == &cfg.mapSource) {
        RasterMapManager::instance().setMapSource((uint8_t)(int)cfg.mapSource);
        saveConfigToNVS(cfg);
        lv_label_set_text(settingsStatusLabel, "Da doi nguon ban do!");
    }
}

static void addChoiceRow(lv_obj_t *parent, int &y, const char *name, float *target, const char *const *labels,
                          int count) {
    lv_obj_t *nameLbl = lv_label_create(parent);
    lv_label_set_text(nameLbl, name);
    lv_obj_set_style_text_color(nameLbl, lv_color_hex(0xCCD6E0), 0);
    lv_obj_set_pos(nameLbl, 4, y);
    y += 15;

    int parentW = lv_obj_get_width(parent);
    const int gap = 4, leftMargin = 4, rightMargin = 4;
    int btnW = (parentW - leftMargin - rightMargin - gap * (count - 1)) / count;
    if (btnW > 90) btnW = 90; // don't stretch to absurd width on a wide landscape panel
    const int btnH = 22;

    ChoiceBinding *b = &choiceBindings[choiceCount++];
    b->count = count;
    b->target = target;
    int sel = (int)(*target);
    for (int i = 0; i < count; i++) {
        lv_obj_t *btn = lv_btn_create(parent);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_CHECKABLE);
        lv_obj_set_size(btn, btnW, btnH);
        lv_obj_set_pos(btn, leftMargin + i * (btnW + gap), y);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x2E3B4E), 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x2F7CE0), LV_STATE_CHECKED);
        if (i == sel) lv_obj_add_state(btn, LV_STATE_CHECKED);
        lv_obj_t *lbl = lv_label_create(btn);
        lv_label_set_text(lbl, labels[i]);
        lv_obj_center(lbl);
        lv_obj_add_event_cb(btn, onChoiceBtnClicked, LV_EVENT_CLICKED, b);
        b->btns[i] = btn;
    }
    y += btnH + 6;
}

static void addSwitchRow(lv_obj_t *parent, int &y, const char *name, bool *target) {
    // Switches don't need the slider's stacked-row treatment — the switch
    // widget itself is small regardless of panel width, and every switch
    // label in this app is short enough to clear even a ~220px-wide
    // portrait column before the switch's own position below.
    int swX = lv_obj_get_width(parent) - 46;

    lv_obj_t *nameLbl = lv_label_create(parent);
    lv_label_set_text(nameLbl, name);
    lv_obj_set_style_text_color(nameLbl, lv_color_hex(0xCCD6E0), 0);
    lv_obj_set_pos(nameLbl, 4, y + 3);

    lv_obj_t *sw = lv_switch_create(parent);
    lv_obj_set_pos(sw, swX, y);
    if (*target) lv_obj_add_state(sw, LV_STATE_CHECKED);

    SwitchBinding *b = &switchBindings[switchCount++];
    b->sw = sw;
    b->target = target;
    lv_obj_add_event_cb(sw, onSwitchChanged, LV_EVENT_VALUE_CHANGED, b);

    y += 26;
}

// Read-only diagnostic row (name + live value) — spec section 16.6/16.7
// "check" fields (GNSS fix, satellites, speed, Speed Map status). No
// binding/callback, unlike the slider/switch rows above: the value label is
// just handed back so refreshSensorsPanel() can update it on a timer. Kept
// as plain named lv_obj_t* pointers below rather than a generic array —
// there are only a handful and each needs different formatting.
// Value column position is relative to the parent's real width (not a
// fixed x=220) for the same portrait-width reasoning addSliderRow() gives —
// every value shown through this row is short enough (a number, "OK",
// "12.3 km/h") that it doesn't need the stacked treatment, just a value
// column that isn't hardcoded off the edge of a narrower panel.
static lv_obj_t *addReadonlyRow(lv_obj_t *parent, int &y, const char *name) {
    lv_obj_t *nameLbl = lv_label_create(parent);
    lv_label_set_text(nameLbl, name);
    lv_obj_set_style_text_color(nameLbl, lv_color_hex(0xCCD6E0), 0);
    lv_obj_set_pos(nameLbl, 4, y + 5);

    lv_obj_t *valLbl = lv_label_create(parent);
    lv_obj_set_style_text_color(valLbl, lv_color_white(), 0);
    lv_obj_set_pos(valLbl, lv_obj_get_width(parent) - 90, y + 5);

    y += 22;
    return valLbl;
}

// For a value string long enough (e.g. the WiFi tab's "ON, IP=192.168.4.1")
// that it doesn't comfortably fit the ~160px-wide value column the
// two-column addReadonlyRow() layout above gives every other (short:
// "OK"/a number/"12.3 km/h") readonly row. Stacked instead: name on its own
// line, value below spanning nearly the full panel width.
static lv_obj_t *addWideReadonlyRow(lv_obj_t *parent, int &y, const char *name) {
    lv_obj_t *nameLbl = lv_label_create(parent);
    lv_label_set_text(nameLbl, name);
    lv_obj_set_style_text_color(nameLbl, lv_color_hex(0xCCD6E0), 0);
    lv_obj_set_pos(nameLbl, 4, y);
    y += 15;

    lv_obj_t *valLbl = lv_label_create(parent);
    lv_obj_set_style_text_color(valLbl, lv_color_white(), 0);
    lv_obj_set_pos(valLbl, 4, y);
    y += 19;
    return valLbl;
}

static lv_obj_t *gnssFixVal, *gnssSatsVal, *gnssSpeedRawVal, *gnssSpeedFilteredVal;
static lv_obj_t *wifiStatusVal; // Settings > WiFi tab — see refreshSensorsPanel()
static lv_obj_t *dataUpdateStatusVal = nullptr; // Settings > WiFi tab, online data update progress
static void onDataUpdateBtnClicked(lv_event_t *) {
    if (cfg.dataUpdateUrl[0] == '\0') {
        if (dataUpdateStatusVal) lv_label_set_text(dataUpdateStatusVal, "Chua dat URL (Config)");
        return;
    }
    if (dataUpdateStatusVal) lv_label_set_text(dataUpdateStatusVal, "Khoi dong lai de cap nhat...");
    dataUpdateSchedule(); // reboots into update mode (download runs there with RAM free for TLS)
}
// Settings > Sensors > "Speed Map" group — see refreshSensorsPanel(). Region/
// version come from speedLimitManagerGetInfo() (static once loaded at boot);
// the rest come from roadInfoSnapshot() (updates every ~500ms).
static lv_obj_t *mapFileVal = nullptr, *mapTilesCountVal = nullptr, *mapZoomVal = nullptr, *mapStatusVal = nullptr;
static lv_obj_t *speedMapStatusVal, *speedMapRegionVal, *speedMapVersionVal;
static lv_obj_t *speedMapLimitVal, *speedMapSourceVal, *speedMapMatchVal, *speedMapRoadIdVal;

static void refreshSensorsPanel(lv_timer_t *) {
    // Only worth doing while Settings is actually the screen on-screen —
    // same class of bug as Dashboard.cpp's simTimerCb (fixed alongside
    // this, user-reported 2026-09-15 laggy Settings interactions): a timer
    // that keeps doing work for a screen nobody's looking at just steals
    // CPU from whatever IS being interacted with.
    if (lv_screen_active() != settingsScreen) return;

    GnssSnapshot gnss = gnssSnapshot();

    // Three states — see Dashboard.cpp's gnssStatusWord for why "no fix
    // yet" (SEARCHING, normal while cold-starting/indoors) must read
    // differently from "nothing received at all" (FAULT, an actual
    // wiring/power/baud problem). User-reported confusion 2026-09-15.
    if (gnss.fix) {
        lv_label_set_text(gnssFixVal, "OK");
        lv_obj_set_style_text_color(gnssFixVal, lv_color_hex(0x33CC66), 0);
    } else if (gnss.linkAlive) {
        lv_label_set_text(gnssFixVal, "SEARCHING");
        lv_obj_set_style_text_color(gnssFixVal, lv_color_hex(0xE0C020), 0);
    } else {
        lv_label_set_text(gnssFixVal, "FAULT");
        lv_obj_set_style_text_color(gnssFixVal, lv_color_hex(0xFF3B30), 0);
    }
    lv_label_set_text_fmt(gnssSatsVal, "%d", gnss.satCount);

    // Floats -> snprintf into a buffer, never lv_label_set_text_fmt("%f"...)
    // directly: LV_USE_FLOAT=0 strips float support from LVGL's builtin
    // vsnprintf and it silently corrupts the varargs (LoadProhibited crash,
    // confirmed on real hardware 2026-09-14 — see Dashboard.cpp/README).
    char buf[16];
    snprintf(buf, sizeof(buf), "%.1f km/h", (double)gnss.rawSpeedKmh);
    lv_label_set_text(gnssSpeedRawVal, buf);
    snprintf(buf, sizeof(buf), "%.1f km/h", (double)gnss.egoSpeedKmh);
    lv_label_set_text(gnssSpeedFilteredVal, buf);

    // Which demo scene is on screen right now, so the tour can be described
    // while watching it (and so it's obvious at a glance that the numbers on
    // the Dashboard are synthetic, not a real fix).
    lv_label_set_text(demoSceneVal, demoModeSceneName());
    lv_obj_set_style_text_color(demoSceneVal, demoModeIsEnabled() ? lv_color_hex(0xE0C020) : lv_color_hex(0x7C8A9A),
                                 0);

    char wifiBuf[48];
    bool wifiOn = webPortalIsEnabled();
    webPortalStatusText(wifiBuf, sizeof(wifiBuf));
    lv_label_set_text(wifiStatusVal, wifiBuf);
    lv_obj_set_style_text_color(wifiStatusVal, wifiOn ? lv_color_hex(0x33CC66) : lv_color_hex(0x7C8A9A), 0);

    refreshScanListIfOpen(); // populate the WiFi-setup overlay's list when a scan finishes

    if (staStatusVal) {
        char staBuf[80];
        webPortalStaInfo(staBuf, sizeof(staBuf));
        lv_label_set_text(staStatusVal, staBuf);
        // green when "Da noi" (connected), amber while connecting, grey/red otherwise
        uint32_t col = 0x7C8A9A;
        if (strncmp(staBuf, "Da noi", 6) == 0) col = 0x33CC66;
        else if (strncmp(staBuf, "Dang", 4) == 0) col = 0xE0C020;
        else if (strncmp(staBuf, "Khong thay", 10) == 0 || strncmp(staBuf, "Sai", 3) == 0) col = 0xFF3B30;
        lv_obj_set_style_text_color(staStatusVal, lv_color_hex(col), 0);
    }

    if (dataUpdateStatusVal) {
        DataUpdateStatus du = dataUpdateGetStatus();
        char dbuf[80];
        uint32_t col = 0x7C8A9A;
        if (du.state == DU_RUNNING) {
            snprintf(dbuf, sizeof(dbuf), "%s %d/%d (%d%%)", du.message, du.filesDone, du.filesTotal, du.percent);
            col = 0xE0C020;
        } else if (du.state == DU_SUCCESS) { snprintf(dbuf, sizeof(dbuf), "%s", du.message); col = 0x33CC66; }
        else if (du.state == DU_FAILED) { snprintf(dbuf, sizeof(dbuf), "%s", du.message); col = 0xFF3B30; }
        else snprintf(dbuf, sizeof(dbuf), "san sang");
        lv_label_set_text(dataUpdateStatusVal, dbuf);
        lv_obj_set_style_text_color(dataUpdateStatusVal, lv_color_hex(col), 0);
    }
    // Keeps the switch honest if WiFi was toggled by the Dashboard's 4s hold
    // gesture rather than this switch itself — lv_obj_add/remove_state()
    // (not lv_switch_set_state()) so this doesn't re-fire onWifiSwitchChanged
    // and loop back into another webPortalRequestEnable() call, same pattern
    // onRestoreDefaults() already uses for its own switch rows.
    if (wifiOn) lv_obj_add_state(wifiEnableSwitch, LV_STATE_CHECKED);
    else lv_obj_clear_state(wifiEnableSwitch, LV_STATE_CHECKED);

    // Speed Map — spec section 25. Region/version only change if the SD
    // card is swapped and the device rebooted, so a single mapLoaded check
    // gates all the "static" fields; the rest (limit/source/match/road)
    // come from the live 500ms RoadInfoSnapshot regardless.
    // Map tab diagnostic fields
    if (mapFileVal) {
        lv_label_set_text(mapFileVal, RasterMapManager::instance().getCurrentSourcePath());
        char mBuf[32];
        snprintf(mBuf, sizeof(mBuf), "%u tiles", (unsigned int)RasterMapManager::instance().getTileCount());
        lv_label_set_text(mapTilesCountVal, mBuf);
        snprintf(mBuf, sizeof(mBuf), "z%u .. z%u", (unsigned int)RasterMapManager::instance().getMinZoom(),
                 (unsigned int)RasterMapManager::instance().getMaxZoom());
        lv_label_set_text(mapZoomVal, mBuf);
        lv_label_set_text(mapStatusVal, RasterMapManager::instance().isLoaded() ? "Active (OK)" : "File missing");
    }

    SpeedMapMetadata mapInfo;
    bool mapLoaded = speedLimitManagerGetInfo(mapInfo);
    if (mapLoaded) {
        lv_label_set_text(speedMapStatusVal, "CONNECTED");
        lv_obj_set_style_text_color(speedMapStatusVal, lv_color_hex(0x33CC66), 0);
        lv_label_set_text_fmt(speedMapRegionVal, "%.16s", mapInfo.region);
        lv_label_set_text_fmt(speedMapVersionVal, "%.16s", mapInfo.mapVersion);
    } else {
        lv_label_set_text(speedMapStatusVal, "NOT CONNECTED");
        lv_obj_set_style_text_color(speedMapStatusVal, lv_color_hex(0x7C8A9A), 0);
        lv_label_set_text(speedMapRegionVal, "--");
        lv_label_set_text(speedMapVersionVal, "--");
    }

    RoadInfoSnapshot road = roadInfoSnapshot();
    if (road.valid) {
        lv_label_set_text_fmt(speedMapLimitVal, "%.0f km/h", (double)road.speedLimitKmh);
    } else {
        lv_label_set_text(speedMapLimitVal, "--");
    }
    lv_label_set_text(speedMapSourceVal, road.mapLoaded ? speedSourceStr(road.source) : "--");
    if (road.mapLoaded && road.roadId != 0) {
        // Confidence -> HIGH/MEDIUM/LOW bucket, spec section 16 — the exact
        // 0.0-1.0 number is available via /api/speedmap/debug for anyone
        // who wants it, not shown on this screen to keep it scannable.
        const char *bucket = road.confidence >= 0.8f ? "HIGH" : (road.confidence >= 0.5f ? "MEDIUM" : "LOW");
        lv_label_set_text_fmt(speedMapMatchVal, "%s (%.2f)", bucket, (double)road.confidence);
        lv_label_set_text_fmt(speedMapRoadIdVal, "%lu", (unsigned long)road.roadId);
    } else {
        lv_label_set_text(speedMapMatchVal, "--");
        lv_label_set_text(speedMapRoadIdVal, "--");
    }
}

static void onBackToDashboard(lv_event_t *) { lv_screen_load(dashboardScreen); }

// Auto-return to the Dashboard after 5s of no touch anywhere (user-requested
// 2026-09-15) — reads Dashboard.cpp's shared touch timestamp rather than
// tracking its own, since there's only one touch source for the whole app;
// wakeScreen() already updates it on every touch regardless of which screen
// is showing. Checked once a second, not on every tick: a 5s timeout only
// needs ~1s granularity, and this runs even while idle on Settings so it
// isn't worth being more precise than a human would notice.
static void checkIdleReturnToDashboard(lv_timer_t *) {
    if (lv_screen_active() != settingsScreen) return;
    static const uint32_t kIdleTimeoutMs = 5000;
    if (millis() - lastTouchAtMs() > kIdleTimeoutMs) {
        lv_screen_load(dashboardScreen);
    }
}

// Save is confirmed through a modal rather than writing straight away: the
// settings being written include the safety thresholds, and NVS writes have
// limited endurance, so an accidental brush of the button should not persist.
static lv_obj_t *confirmOverlay;

static void closeConfirm(lv_event_t *) { lv_obj_add_flag(confirmOverlay, LV_OBJ_FLAG_HIDDEN); }

static void onConfirmSaveYes(lv_event_t *) {
    saveConfigToNVS(cfg);
    lv_obj_add_flag(confirmOverlay, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(settingsStatusLabel, "Saved to NVS");
    Serial.println("[uidemo] config saved to NVS");
}

static void onSaveConfig(lv_event_t *) { lv_obj_clear_flag(confirmOverlay, LV_OBJ_FLAG_HIDDEN); }

// Second, separate overlay for the footer's "Restart" button (added
// 2026-09-22) — a real device reboot is more disruptive than anything else
// on this screen, including Save above, yet used to fire on a single tap
// with NO confirmation at all, and with a deliberately ENLARGED touch
// hit-area on top of that (see restartBtn's own comment — a touch-accuracy
// accommodation). Confirmed on real hardware: the pre-existing touch
// controller glitch (TouchTask.cpp's "stuck" bus-reset recovery) was
// spuriously landing in that enlarged zone and silently triggering a real
// ESP.restart() — which looks exactly like a random crash from the
// driver's seat, not an accidental button press. A second stray touch
// hitting this modal's own Restart button too (normal-sized, not enlarged)
// is far less likely than one hitting the original button's 20px-padded
// zone, so this closes the actual gap rather than just relocating it.
// restartConfirmOverlay declared at file top
static void closeRestartConfirm(lv_event_t *) { lv_obj_add_flag(restartConfirmOverlay, LV_OBJ_FLAG_HIDDEN); }
static void onConfirmRestartYes(lv_event_t *) { ESP.restart(); }
static void onRestartBtnClicked(lv_event_t *) { lv_obj_clear_flag(restartConfirmOverlay, LV_OBJ_FLAG_HIDDEN); }

static void buildRestartConfirmOverlay(lv_obj_t *parent) {
    restartConfirmOverlay = lv_obj_create(parent);
    lv_obj_set_pos(restartConfirmOverlay, 0, 0);
    lv_obj_set_size(restartConfirmOverlay, gfx->width(), gfx->height());
    lv_obj_set_style_bg_color(restartConfirmOverlay, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(restartConfirmOverlay, LV_OPA_60, 0);
    lv_obj_set_style_border_width(restartConfirmOverlay, 0, 0);
    lv_obj_set_style_radius(restartConfirmOverlay, 0, 0);
    lv_obj_clear_flag(restartConfirmOverlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(restartConfirmOverlay, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *box = lv_obj_create(restartConfirmOverlay);
    lv_obj_set_size(box, 300, 130);
    lv_obj_center(box);
    lv_obj_set_style_bg_color(box, lv_color_hex(0x1B222A), 0);
    lv_obj_set_style_border_color(box, lv_color_hex(0x3A4A5C), 0);
    lv_obj_set_style_radius(box, 8, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *msg = lv_label_create(box);
    lv_label_set_text(msg, "Restart the device now?");
    lv_obj_set_style_text_color(msg, lv_color_white(), 0);
    lv_obj_align(msg, LV_ALIGN_TOP_MID, 0, 6);

    lv_obj_t *noBtn = lv_button_create(box);
    lv_obj_set_size(noBtn, 110, 36);
    lv_obj_align(noBtn, LV_ALIGN_BOTTOM_LEFT, 0, -4);
    lv_obj_set_style_bg_color(noBtn, lv_color_hex(0x44505C), 0);
    lv_obj_set_ext_click_area(noBtn, 10);
    lv_obj_add_event_cb(noBtn, closeRestartConfirm, LV_EVENT_CLICKED, NULL);
    lv_obj_t *noLbl = lv_label_create(noBtn);
    lv_label_set_text(noLbl, "Cancel");
    lv_obj_center(noLbl);

    lv_obj_t *yesBtn = lv_button_create(box);
    lv_obj_set_size(yesBtn, 110, 36);
    lv_obj_align(yesBtn, LV_ALIGN_BOTTOM_RIGHT, 0, -4);
    lv_obj_set_style_bg_color(yesBtn, lv_color_hex(0x8A3A3A), 0);
    lv_obj_set_ext_click_area(yesBtn, 10);
    lv_obj_add_event_cb(yesBtn, onConfirmRestartYes, LV_EVENT_CLICKED, NULL);
    lv_obj_t *yesLbl = lv_label_create(yesBtn);
    lv_label_set_text(yesLbl, "Restart");
    lv_obj_center(yesLbl);
}

static void buildConfirmOverlay(lv_obj_t *parent) {
    confirmOverlay = lv_obj_create(parent);
    lv_obj_set_pos(confirmOverlay, 0, 0);
    lv_obj_set_size(confirmOverlay, gfx->width(), gfx->height());
    lv_obj_set_style_bg_color(confirmOverlay, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(confirmOverlay, LV_OPA_60, 0);
    lv_obj_set_style_border_width(confirmOverlay, 0, 0);
    lv_obj_set_style_radius(confirmOverlay, 0, 0);
    lv_obj_clear_flag(confirmOverlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(confirmOverlay, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *box = lv_obj_create(confirmOverlay);
    lv_obj_set_size(box, 300, 130);
    lv_obj_center(box);
    lv_obj_set_style_bg_color(box, lv_color_hex(0x1B222A), 0);
    lv_obj_set_style_border_color(box, lv_color_hex(0x3A4A5C), 0);
    lv_obj_set_style_radius(box, 8, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *msg = lv_label_create(box);
    lv_label_set_text(msg, "Save settings to memory?");
    lv_obj_set_style_text_color(msg, lv_color_white(), 0);
    lv_obj_align(msg, LV_ALIGN_TOP_MID, 0, 6);

    lv_obj_t *noBtn = lv_button_create(box);
    lv_obj_set_size(noBtn, 110, 36);
    lv_obj_align(noBtn, LV_ALIGN_BOTTOM_LEFT, 0, -4);
    lv_obj_set_style_bg_color(noBtn, lv_color_hex(0x44505C), 0);
    lv_obj_set_ext_click_area(noBtn, 10);
    lv_obj_add_event_cb(noBtn, closeConfirm, LV_EVENT_CLICKED, NULL);
    lv_obj_t *noLbl = lv_label_create(noBtn);
    lv_label_set_text(noLbl, "Cancel");
    lv_obj_center(noLbl);

    lv_obj_t *yesBtn = lv_button_create(box);
    lv_obj_set_size(yesBtn, 110, 36);
    lv_obj_align(yesBtn, LV_ALIGN_BOTTOM_RIGHT, 0, -4);
    lv_obj_set_style_bg_color(yesBtn, lv_color_hex(0x2E7D4F), 0);
    lv_obj_set_ext_click_area(yesBtn, 10);
    lv_obj_add_event_cb(yesBtn, onConfirmSaveYes, LV_EVENT_CLICKED, NULL);
    lv_obj_t *yesLbl = lv_label_create(yesBtn);
    lv_label_set_text(yesLbl, "Save");
    lv_obj_center(yesLbl);
}

static void onRestoreDefaults(lv_event_t *) {
    cfg = AppConfig();
    clampConfig(cfg);
    applyConfig();
    for (int i = 0; i < sliderCount; i++) {
        SliderBinding &b = sliderBindings[i];
        lv_slider_set_value(b.slider, (int32_t)((*b.target) * b.divisor), LV_ANIM_OFF);
        char buf[24];
        fmtSettingVal(buf, sizeof(buf), *b.target, b.unit);
        lv_label_set_text(b.valLabel, buf);
    }
    for (int i = 0; i < switchCount; i++) {
        SwitchBinding &b = switchBindings[i];
        if (*b.target) lv_obj_add_state(b.sw, LV_STATE_CHECKED);
        else lv_obj_clear_state(b.sw, LV_STATE_CHECKED);
    }
    // WiFi SSID/password aren't in sliderBindings/switchBindings (see
    // onWifiSwitchChanged()'s comment) — refreshed by hand. Password field
    // stays blank, same as every other time it's displayed (see
    // wifiPasswordTa's own build comment) — cfg.wifiPassword IS reset
    // underneath, just never echoed into the field.
    lv_textarea_set_text(wifiSsidTa, cfg.wifiSsid);
    lv_textarea_set_text(wifiPasswordTa, "");
    lv_label_set_text(settingsStatusLabel, "Restored defaults");
    Serial.println("[uidemo] config restored to defaults (not yet saved)");
}

// Category menu: left nav rail + one content panel per category, all
// pre-built and toggled via LV_OBJ_FLAG_HIDDEN (no rebuild/flicker on
// switching). Whole screen fits with no scrolling; Save/Defaults stay in a
// fixed footer visible from every category.
static const int kCategoryCount = 4;
static lv_obj_t *categoryPanels[kCategoryCount];
static lv_obj_t *navButtons[kCategoryCount];

static void selectCategory(int idx) {
    for (int i = 0; i < kCategoryCount; i++) {
        if (i == idx) {
            lv_obj_clear_flag(categoryPanels[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_bg_color(navButtons[i], lv_color_hex(0x2E4A66), 0);
        } else {
            lv_obj_add_flag(categoryPanels[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_bg_color(navButtons[i], lv_color_hex(0x151C24), 0);
        }
    }
}
static void onNavCategory(lv_event_t *e) { selectCategory((int)(intptr_t)lv_event_get_user_data(e)); }

lv_obj_t *settingsScreen;

void buildSettingsScreen() {
    settingsScreen = lv_obj_create(NULL);
    // Pure black, not 0x0B0F14 — same request/reasoning as Dashboard.cpp's
    // applyTheme() night background (2026-09-16, screen longevity/glare).
    lv_obj_set_style_bg_color(settingsScreen, lv_color_hex(0x000000), 0);
    lv_obj_set_style_pad_all(settingsScreen, 0, 0);
    lv_obj_clear_flag(settingsScreen, LV_OBJ_FLAG_SCROLLABLE);

    // scrW/scrH, not hardcoded 480/320 — Settings, like Dashboard.cpp, now
    // has to fit whichever orientation is currently active (AppConfig.h's
    // screenRotation). Unlike Dashboard, Settings keeps the SAME
    // header/nav-rail/content-panel/footer structure for every orientation
    // (least risky adaptation — no new interaction model to design/verify);
    // only the row-builders below (addSliderRow() etc.) branch on the
    // resulting content panel width, which is what actually differs.
    int scrW = gfx->width(), scrH = gfx->height();
    const int HEADER_H = 26, FOOTER_H = 34;
    const int NAV_W = 96;
    const int BODY_TOP = HEADER_H, BODY_H = scrH - HEADER_H - FOOTER_H;

    // Header
    lv_obj_t *header = lv_obj_create(settingsScreen);
    lv_obj_set_pos(header, 0, 0);
    lv_obj_set_size(header, scrW, HEADER_H);
    lv_obj_set_style_bg_color(header, lv_color_hex(0x151C24), 0);
    lv_obj_set_style_border_width(header, 0, 0);
    lv_obj_set_style_radius(header, 0, 0);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(header);
    lv_label_set_text(title, "Settings");
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 10, 0);

    lv_obj_t *backBtn = lv_button_create(header);
    lv_obj_set_size(backBtn, 64, 20);
    lv_obj_align(backBtn, LV_ALIGN_RIGHT_MID, -6, 0);
    // This button sits inside a thin bar right at the very top edge of the
    // panel — the least accurate/reliable region of a capacitive touch
    // sensor (edge effects). Real-hardware complaint 2026-09-14: "không
    // Back hay Save được" (Back/Save unresponsive). A bigger *touch* target
    // than the *visual* button, rather than enlarging the button itself,
    // fixes the near-miss taps without changing the compact header look.
    lv_obj_set_ext_click_area(backBtn, 20);
    lv_obj_add_event_cb(backBtn, onBackToDashboard, LV_EVENT_CLICKED, NULL);
    lv_obj_t *backLbl = lv_label_create(backBtn);
    lv_label_set_text(backLbl, "< Back");
    lv_obj_center(backLbl);

    // Left nav rail
    lv_obj_t *navRail = lv_obj_create(settingsScreen);
    lv_obj_set_pos(navRail, 0, BODY_TOP);
    lv_obj_set_size(navRail, NAV_W, BODY_H);
    lv_obj_set_style_bg_color(navRail, lv_color_hex(0x0E141B), 0);
    lv_obj_set_style_border_width(navRail, 0, 0);
    lv_obj_set_style_radius(navRail, 0, 0);
    lv_obj_set_style_pad_all(navRail, 4, 0);
    lv_obj_clear_flag(navRail, LV_OBJ_FLAG_SCROLLABLE);

    static const char *kCategoryNames[kCategoryCount] = {"Display", "Map", "Sensors", "WiFi"};
    for (int i = 0; i < kCategoryCount; i++) {
        lv_obj_t *btn = lv_button_create(navRail);
        lv_obj_set_size(btn, NAV_W - 8, 42);
        lv_obj_set_pos(btn, 0, i * 48);
        lv_obj_set_style_radius(btn, 4, 0);
        lv_obj_set_ext_click_area(btn, 5); // small — buttons are stacked with only a 6px gap
        lv_obj_add_event_cb(btn, onNavCategory, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *lbl = lv_label_create(btn);
        lv_label_set_text(lbl, kCategoryNames[i]);
        lv_obj_center(lbl);
        navButtons[i] = btn;
    }

    // Content panels (one per category, same rect, toggled via HIDDEN)
    for (int i = 0; i < kCategoryCount; i++) {
        lv_obj_t *panel = lv_obj_create(settingsScreen);
        lv_obj_set_pos(panel, NAV_W + 4, BODY_TOP);
        lv_obj_set_size(panel, scrW - NAV_W - 4, BODY_H);
        // Root cause of the 2026-09-16 "khong thay nut nhan" / mispositioned-
        // switch bug: lv_obj_get_width()/height() only reflect a *pending*
        // lv_obj_set_size() after the next redraw (LVGL's own documented
        // behavior, lv_obj_pos.h) — this screen is built entirely inside
        // setup(), before lv_timer_handler() has ever run once, so every
        // row-builder below that reads lv_obj_get_width(parent) (addSliderRow,
        // addSwitchRow, addChoiceRow, ...) was silently reading back 0 the
        // whole time since the rotation refactor introduced width-aware rows,
        // not just for the new Theme/Rotation buttons. Forcing the layout
        // commit here, once per panel right after sizing it, makes every
        // later lv_obj_get_width(categoryPanels[i]) call return the real
        // value instead.
        lv_obj_update_layout(panel);
        lv_obj_set_style_bg_color(panel, lv_color_hex(0x000000), 0); // pure black — see settingsScreen's own comment above
        lv_obj_set_style_border_width(panel, 0, 0);
        lv_obj_set_style_pad_all(panel, 4, 0);
        // Display and WiFi both fit inside BODY_H with no scrolling. Sensors
        // (i==1) doesn't — the GNSS live-status rows plus the Speed Map
        // diagnostics group (7 fields) genuinely don't fit in 260px, and
        // it's allowed to be long (spec section 25 calls it "for kiểm
        // tra/cấu hình," not the driving screen) rather than needing rows
        // trimmed to squeeze in. The old Radar/Safety tabs that used to also
        // need scrolling here are gone entirely (radar removed 2026-09-21).
        if (i == 2 || i == 3) { // Sensors + WiFi tabs scroll (2026-09-25: WiFi now has AP + STA + data-update sections)
            lv_obj_set_scroll_dir(panel, LV_DIR_VER);
            lv_obj_set_scrollbar_mode(panel, LV_SCROLLBAR_MODE_AUTO);
        } else {
            lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
        }
        lv_obj_add_flag(panel, LV_OBJ_FLAG_HIDDEN);
        categoryPanels[i] = panel;
    }

    int y;
    // Display tab (categoryPanels[0]) — brightness/dim/theme/rotation plus
    // the master alert-audio toggle (moved here from the old Safety tab,
    // which no longer exists — see cfg.audioEnabled's own AppConfig.h
    // comment for why it's still just a plain on/off, repurposed rather
    // than removed, when radar was taken out 2026-09-21).
    y = 4;
    addSwitchRow(categoryPanels[0], y, "Alert audio enabled", &cfg.audioEnabled);
    addSliderRow(categoryPanels[0], y, "Volume", &cfg.audioVolume, 0, 100, 1.0f, " %");
    addSliderRow(categoryPanels[0], y, "Brightness", &cfg.brightness, 5, 100, 1.0f, " %");
    // Label updated 2026-09-16 alongside the gate itself changing from
    // touch-idle to vehicle-stationary time (see Dashboard.cpp) — "stopped"
    // says what actually starts the timer now.
    addSliderRow(categoryPanels[0], y, "Dim after stopped", &cfg.autoDimMin, 0, 30, 1.0f, " min");
    // Theme (user-requested 2026-09-15): 0=Auto follows GNSS.cpp's real
    // sunrise/sunset calc (unchanged default behavior), 1=Light/2=Dark force
    // it either way. Applies live — ui/Dashboard.cpp's refreshDashboard()
    // re-checks cfg.themeMode on the same tick it already checks
    // gnss.daytime, no restart needed (see applyTheme()'s call site there).
    // Choice buttons, not a slider (user-reported 2026-09-16, "nut gat chua
    // hop ly") — a 3-way enum on a slider hid which option was active.
    static const char *kThemeLabels[3] = {"Auto", "Light", "Dark"};
    addChoiceRow(categoryPanels[0], y, "Theme", &cfg.themeMode, kThemeLabels, 3);
    // Rotation: does NOT apply live — see onChoiceBtnClicked()'s special
    // case for this field and AppConfig.h's screenRotation comment for why
    // (both Dashboard and Settings are laid out once at boot for whichever
    // orientation is active then). 0/1/2/3 matches Arduino_GFX's own
    // rotation convention exactly (display/DisplayDriver.cpp passes this
    // straight through) — 1 is today's boot default (landscape). Choice
    // buttons rather than a slider for the same reason as Theme above, and
    // because a held slider drag was what triggered the touch controller's
    // known stuck-bus quirk (2026-09-14) during the user's own testing.
    static const char *kRotationLabels[4] = {"0", "90", "180", "270"};
    addChoiceRow(categoryPanels[0], y, "Rotation", &cfg.screenRotation, kRotationLabels, 4);

    // Demo mode (user-requested 2026-09-22, "demo hien thi truoc de toi chinh
    // sua") — plays a scripted tour of every Dashboard state so the UI can be
    // reviewed without a GNSS fix or a speed-map database on the card. Built
    // by hand rather than via addSwitchRow() for the same reason as the WiFi
    // tab's own switch: there's no bool in cfg for the generic binding to
    // point at, and there must not be (demo/DemoMode.h).
    {
        lv_obj_t *nameLbl = lv_label_create(categoryPanels[0]);
        lv_label_set_text(nameLbl, "Demo mode (xem UI)");
        lv_obj_set_style_text_color(nameLbl, lv_color_hex(0xCCD6E0), 0);
        lv_obj_set_pos(nameLbl, 4, y + 3);
        demoEnableSwitch = lv_switch_create(categoryPanels[0]);
        lv_obj_set_pos(demoEnableSwitch, lv_obj_get_width(categoryPanels[0]) - 46, y);
        // A stock lv_switch's default hit area is small (~40x20px) — same
        // "enlarge the touch target, not just the visual size" fix this
        // file already applies to backBtn/restoreBtn/restartBtn/saveBtn
        // after real user reports of unresponsiveness (those all sit at a
        // panel edge, a specifically worse region for capacitive touch;
        // this one doesn't, but user-reported 2026-09-22 unresponsiveness
        // here too — "gat Demo mode khong an, Settings tu thoat" — a small
        // control is still a small control wherever it sits).
        lv_obj_set_ext_click_area(demoEnableSwitch, 20);
        lv_obj_add_event_cb(demoEnableSwitch, onDemoSwitchChanged, LV_EVENT_VALUE_CHANGED, NULL);
        y += 26;
    }
    demoSceneVal = addReadonlyRow(categoryPanels[0], y, "Demo scene");

    // -----------------------------------------------------------------
    // Map tab (categoryPanels[1]) - Map Source (Carto / OSM) & Layers
    // -----------------------------------------------------------------
    y = 4;
    // Carto is the only source with street-level detail (z14/z15); OSM &
    // Voyager top out at coarse z13 everywhere incl. Hanoi (measured
    // 2026-09-24), so Carto is the default and marked "HD" here to steer the
    // user away from the low-detail ones.
    static const char *kMapSourceLabels[3] = {"Carto HD", "OSM (co ban)", "OSM Dark"};
    addChoiceRow(categoryPanels[1], y, "Map Source", &cfg.mapSource, kMapSourceLabels, 3);
    addSwitchRow(categoryPanels[1], y, "Ban do JPEG (nen anh)", &cfg.showRasterMap);
    addSwitchRow(categoryPanels[1], y, "Vector Roads overlay", &cfg.showVectorRoads);
    addSwitchRow(categoryPanels[1], y, "Huong xe len tren (xoay)", &cfg.mapHeadingUp);
    addSwitchRow(categoryPanels[1], y, "Vehicle Trail (track)", &cfg.showVehicleTrail);

    y += 8;
    lv_obj_t *mapHeader = lv_label_create(categoryPanels[1]);
    lv_label_set_text(mapHeader, "SD Card Map File");
    lv_obj_set_style_text_color(mapHeader, lv_color_hex(0x7C8A9A), 0);
    lv_obj_set_pos(mapHeader, 4, y);
    y += 20;

    mapFileVal = addReadonlyRow(categoryPanels[1], y, "Active file");
    mapTilesCountVal = addReadonlyRow(categoryPanels[1], y, "Tile count");
    mapZoomVal = addReadonlyRow(categoryPanels[1], y, "Zoom range");
    mapStatusVal = addReadonlyRow(categoryPanels[1], y, "Status");

    // Sensors tab (categoryPanels[2]) — real-hardware check/configure
    // (spec 16.6/16.7). GNSS (u-blox M10N, UART2) went real 2026-09-15;
    // radar (HLK-LD2451) went real 2026-09-16 and was removed entirely
    // 2026-09-21 (GPS-only VietHUD product) — the old Radar-status/
    // Radar-tracking rows that used to live here are gone with it.
    y = 4;
    // Trip logging (added 2026-09-16, see log/TripLogger.h).
    addSwitchRow(categoryPanels[2], y, "Trip logging (SD card)", &cfg.tripLoggingEnabled);
    y += 6; // extra breathing room before the real live-status rows below

    lv_obj_t *sensorsHeader1 = lv_label_create(categoryPanels[2]);
    lv_label_set_text(sensorsHeader1, "Live status");
    lv_obj_set_style_text_color(sensorsHeader1, lv_color_hex(0x7C8A9A), 0);
    lv_obj_set_pos(sensorsHeader1, 4, y);
    y += 20;
    gnssFixVal = addReadonlyRow(categoryPanels[2], y, "GNSS fix");
    gnssSatsVal = addReadonlyRow(categoryPanels[2], y, "Satellites");
    gnssSpeedRawVal = addReadonlyRow(categoryPanels[2], y, "Speed (raw)");
    gnssSpeedFilteredVal = addReadonlyRow(categoryPanels[2], y, "Speed (filtered)");

    y += 6;
    lv_obj_t *sensorsHeader2 = lv_label_create(categoryPanels[2]);
    lv_label_set_text(sensorsHeader2, "GNSS calibration");
    lv_obj_set_style_text_color(sensorsHeader2, lv_color_hex(0x7C8A9A), 0);
    lv_obj_set_pos(sensorsHeader2, 4, y);
    y += 20;
    addSliderRow(categoryPanels[2], y, "Speed filter smoothing", &cfg.gnssSpeedFilterAlpha, 5, 90, 100.0f, "");
    addSliderRow(categoryPanels[2], y, "Fix timeout", &cfg.gnssFixTimeoutS, 10, 100, 10.0f, " s");
    // GPS speed calibration + ahead-warning distances (user-requested
    // 2026-09-22, "hieu chinh toc do GPS, khoang cach canh bao toc do/
    // camera phia truoc") — see AppConfig.h's own comment on each field for
    // why calibration is a percentage and why only these two distances (not
    // every alert type) are exposed here. Both distance ranges capped at
    // 100m max/default 2026-09-22 ("khoang cach toi da de canh bao la 100m,
    // mac dinh la 100m") — see AppConfig.h's clampConfig() for the matching
    // clamp.
    addSliderRow(categoryPanels[2], y, "Speed calibration", &cfg.gnssSpeedCalibrationPct, -150, 150, 10.0f, " %");
    // Overspeed warning threshold (was un-tunable — the field existed in
    // AppConfig but no control reached it; exposed 2026-09-24). Warning fires
    // when egoSpeed > limit + this offset. 0..10 km/h.
    addSliderRow(categoryPanels[2], y, "Overspeed offset", &cfg.overspeedOffsetKmh, 0, 10, 1.0f, " km/h");
    // Fallback speed limit shown where the map can't resolve one (0 = off/"--").
    // Default 50 = VN urban baseline (user-requested 2026-09-24).
    addSliderRow(categoryPanels[2], y, "Default limit (unknown)", &cfg.defaultLimitKmh, 0, 90, 1.0f, " km/h");
    // NOTE: the old "Speed-limit-ahead dist" / "Camera warn dist" sliders were
    // removed 2026-09-24 — they did nothing. The lookahead uses a DYNAMIC,
    // speed-based warn distance (computeDynamicWarnDistance: ~100-600m scaling
    // with speed) in SpeedLimitManager.cpp, not cfg.aheadLimitWarnDistM/
    // cameraWarnDistM. Those cfg fields are now legacy/unused.

    // Speed Map diagnostics (spec section 25) — offline microSD map-matching
    // status, see map/SpeedLimitManager.h. This group is why categoryPanels[2]
    // needs to be scrollable above: it doesn't fit in 260px alongside
    // everything already in this tab.
    y += 6;
    lv_obj_t *sensorsHeader3 = lv_label_create(categoryPanels[2]);
    lv_label_set_text(sensorsHeader3, "Speed Map");
    lv_obj_set_style_text_color(sensorsHeader3, lv_color_hex(0x7C8A9A), 0);
    lv_obj_set_pos(sensorsHeader3, 4, y);
    y += 20;
    speedMapStatusVal = addReadonlyRow(categoryPanels[2], y, "Status");
    speedMapRegionVal = addReadonlyRow(categoryPanels[2], y, "Region");
    speedMapVersionVal = addReadonlyRow(categoryPanels[2], y, "Version");
    speedMapLimitVal = addReadonlyRow(categoryPanels[2], y, "Current limit");
    speedMapSourceVal = addReadonlyRow(categoryPanels[2], y, "Source");
    speedMapMatchVal = addReadonlyRow(categoryPanels[2], y, "Match");
    speedMapRoadIdVal = addReadonlyRow(categoryPanels[2], y, "Road ID");

    // WiFi tab (user-requested 2026-09-14 alongside the Dashboard's 4s hold
    // gesture — see net/WebPortal.h). Defaults OFF every boot; this switch
    // and the gesture both funnel through the same webPortalRequestEnable().
    bool wifiTabNarrow = lv_obj_get_width(categoryPanels[3]) < kNarrowPanelThreshold;
    y = 4;
    {
        lv_obj_t *nameLbl = lv_label_create(categoryPanels[3]);
        lv_label_set_text(nameLbl, "WiFi enabled");
        lv_obj_set_style_text_color(nameLbl, lv_color_hex(0xCCD6E0), 0);
        lv_obj_set_pos(nameLbl, 4, y + 3);
        wifiEnableSwitch = lv_switch_create(categoryPanels[3]);
        lv_obj_set_pos(wifiEnableSwitch, lv_obj_get_width(categoryPanels[3]) - 46, y);
        lv_obj_add_event_cb(wifiEnableSwitch, onWifiSwitchChanged, LV_EVENT_VALUE_CHANGED, NULL);
        y += 26;
    }

    // Textareas stack label-above-field in a narrow (portrait) panel, same
    // threshold/reasoning as addSliderRow()'s own narrow case — the fixed
    // x=140/width=220 landscape layout below would run off the edge of a
    // ~220px-wide portrait content column otherwise.
    lv_obj_t *ssidLbl = lv_label_create(categoryPanels[3]);
    lv_label_set_text(ssidLbl, "SSID");
    lv_obj_set_style_text_color(ssidLbl, lv_color_hex(0xCCD6E0), 0);
    wifiSsidTa = lv_textarea_create(categoryPanels[3]);
    lv_textarea_set_one_line(wifiSsidTa, true);
    lv_textarea_set_max_length(wifiSsidTa, sizeof(cfg.wifiSsid) - 1);
    lv_textarea_set_text(wifiSsidTa, cfg.wifiSsid);
    if (wifiTabNarrow) {
        lv_obj_set_pos(ssidLbl, 4, y);
        lv_obj_set_pos(wifiSsidTa, 4, y + 18);
        lv_obj_set_size(wifiSsidTa, lv_obj_get_width(categoryPanels[3]) - 8, 28);
        y += 50;
    } else {
        lv_obj_set_pos(ssidLbl, 4, y + 6);
        lv_obj_set_pos(wifiSsidTa, 140, y);
        lv_obj_set_size(wifiSsidTa, 220, 28);
        y += 34;
    }
    lv_obj_add_event_cb(wifiSsidTa, onWifiTaClicked, LV_EVENT_CLICKED, NULL);

    // Password field starts BLANK, never pre-filled with cfg.wifiPassword —
    // same "don't echo a saved secret back into a form" reasoning as the
    // /config web page's own password field (net/WebPortal.cpp). Leaving it
    // blank and tapping elsewhere keeps the existing password unchanged —
    // onWifiKbReadyOrCancel() only overwrites cfg.wifiPassword with
    // whatever's actually typed.
    lv_obj_t *passLbl = lv_label_create(categoryPanels[3]);
    lv_label_set_text(passLbl, "Password");
    lv_obj_set_style_text_color(passLbl, lv_color_hex(0xCCD6E0), 0);
    wifiPasswordTa = lv_textarea_create(categoryPanels[3]);
    lv_textarea_set_one_line(wifiPasswordTa, true);
    lv_textarea_set_password_mode(wifiPasswordTa, true);
    lv_textarea_set_max_length(wifiPasswordTa, sizeof(cfg.wifiPassword) - 1);
    lv_textarea_set_placeholder_text(wifiPasswordTa, "(unchanged)");
    if (wifiTabNarrow) {
        lv_obj_set_pos(passLbl, 4, y);
        lv_obj_set_pos(wifiPasswordTa, 4, y + 18);
        lv_obj_set_size(wifiPasswordTa, lv_obj_get_width(categoryPanels[3]) - 8, 28);
        y += 50;
    } else {
        lv_obj_set_pos(passLbl, 4, y + 6);
        lv_obj_set_pos(wifiPasswordTa, 140, y);
        lv_obj_set_size(wifiPasswordTa, 220, 28);
        y += 34;
    }
    lv_obj_add_event_cb(wifiPasswordTa, onWifiTaClicked, LV_EVENT_CLICKED, NULL);

    wifiStatusVal = addWideReadonlyRow(categoryPanels[3], y, "AP");

    // --- Internet (Station) section: join a phone hotspot / home WiFi so the
    // device gets internet for online data updates + NTP time (2026-09-25). ---
    lv_obj_t *staHdr = lv_label_create(categoryPanels[3]);
    lv_label_set_text(staHdr, "Internet (noi WiFi/hotspot)");
    lv_obj_set_style_text_color(staHdr, lv_color_hex(0x6FB4FF), 0);
    lv_obj_set_pos(staHdr, 4, y + 4);
    y += 26;

    // Easy path: scan nearby networks + pick + enter password on a full-screen
    // overlay (keyboard works there). The manual fields below stay as a fallback.
    lv_obj_t *scanOpenBtn = lv_button_create(categoryPanels[3]);
    lv_obj_set_pos(scanOpenBtn, 4, y);
    lv_obj_set_size(scanOpenBtn, lv_obj_get_width(categoryPanels[3]) - 8, 34);
    lv_obj_set_style_bg_color(scanOpenBtn, lv_color_hex(0x2E5D8A), 0);
    lv_obj_add_event_cb(scanOpenBtn, onWifiScanOpen, LV_EVENT_CLICKED, NULL);
    lv_obj_t *scanOpenLbl = lv_label_create(scanOpenBtn);
    lv_label_set_text(scanOpenLbl, "Tim & ket noi WiFi");
    lv_obj_center(scanOpenLbl);
    y += 42;

    lv_obj_t *staSsidLbl = lv_label_create(categoryPanels[3]);
    lv_label_set_text(staSsidLbl, "Ten mang");
    lv_obj_set_style_text_color(staSsidLbl, lv_color_hex(0xCCD6E0), 0);
    staSsidTa = lv_textarea_create(categoryPanels[3]);
    lv_textarea_set_one_line(staSsidTa, true);
    lv_textarea_set_max_length(staSsidTa, sizeof(cfg.staSsid) - 1);
    lv_textarea_set_text(staSsidTa, cfg.staSsid);
    lv_textarea_set_placeholder_text(staSsidTa, "(de trong = tat)");
    if (wifiTabNarrow) {
        lv_obj_set_pos(staSsidLbl, 4, y);
        lv_obj_set_pos(staSsidTa, 4, y + 18);
        lv_obj_set_size(staSsidTa, lv_obj_get_width(categoryPanels[3]) - 8, 28);
        y += 50;
    } else {
        lv_obj_set_pos(staSsidLbl, 4, y + 6);
        lv_obj_set_pos(staSsidTa, 140, y);
        lv_obj_set_size(staSsidTa, 220, 28);
        y += 34;
    }
    lv_obj_add_event_cb(staSsidTa, onWifiTaClicked, LV_EVENT_CLICKED, NULL);

    lv_obj_t *staPassLbl = lv_label_create(categoryPanels[3]);
    lv_label_set_text(staPassLbl, "Mat khau");
    lv_obj_set_style_text_color(staPassLbl, lv_color_hex(0xCCD6E0), 0);
    staPasswordTa = lv_textarea_create(categoryPanels[3]);
    lv_textarea_set_one_line(staPasswordTa, true);
    lv_textarea_set_password_mode(staPasswordTa, true);
    lv_textarea_set_max_length(staPasswordTa, sizeof(cfg.staPassword) - 1);
    lv_textarea_set_placeholder_text(staPasswordTa, "(unchanged)");
    if (wifiTabNarrow) {
        lv_obj_set_pos(staPassLbl, 4, y);
        lv_obj_set_pos(staPasswordTa, 4, y + 18);
        lv_obj_set_size(staPasswordTa, lv_obj_get_width(categoryPanels[3]) - 8, 28);
        y += 50;
    } else {
        lv_obj_set_pos(staPassLbl, 4, y + 6);
        lv_obj_set_pos(staPasswordTa, 140, y);
        lv_obj_set_size(staPasswordTa, 220, 28);
        y += 34;
    }
    lv_obj_add_event_cb(staPasswordTa, onWifiTaClicked, LV_EVENT_CLICKED, NULL);

    staStatusVal = addWideReadonlyRow(categoryPanels[3], y, "Internet");

    // --- Online data update section ---
    lv_obj_t *duHdr = lv_label_create(categoryPanels[3]);
    lv_label_set_text(duHdr, "Cap nhat du lieu ban do");
    lv_obj_set_style_text_color(duHdr, lv_color_hex(0x6FB4FF), 0);
    lv_obj_set_pos(duHdr, 4, y + 4);
    y += 26;

    // Online data update (2026-09-25): pulls new map/warning data from GitHub
    // (cfg.dataUpdateUrl) over the internet. Needs WiFi station connected.
    lv_obj_t *duBtn = lv_button_create(categoryPanels[3]);
    lv_obj_set_pos(duBtn, 0, y);
    lv_obj_set_size(duBtn, lv_obj_get_width(categoryPanels[3]) - 8, 34);
    lv_obj_set_style_bg_color(duBtn, lv_color_hex(0x2E5D8A), 0);
    lv_obj_set_ext_click_area(duBtn, 8);
    lv_obj_add_event_cb(duBtn, onDataUpdateBtnClicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *duLbl = lv_label_create(duBtn);
    lv_label_set_text(duLbl, "Cap nhat du lieu online");
    lv_obj_center(duLbl);
    y += 42;
    dataUpdateStatusVal = addWideReadonlyRow(categoryPanels[3], y, "Update");

    // NOT called eagerly here: buildSettingsScreen() runs before
    // sharedStateInit() in main_ui_demo.cpp's setup(), so
    // gnssSnapshot()'s mutex doesn't exist yet — an eager
    // call here crashed real hardware 2026-09-15 (xQueueSemaphoreTake
    // assert on a NULL queue, boot-looping). The 500ms timer's first tick
    // fires from loop() well after sharedStateInit() has run, so the rows
    // just start blank for under a second instead.
    lv_timer_create(refreshSensorsPanel, 500, NULL); // live values only need to be as fresh as a human reads them
    lv_timer_create(checkIdleReturnToDashboard, 1000, NULL);

    selectCategory(0);

    // Footer — always visible regardless of selected category
    lv_obj_t *footer = lv_obj_create(settingsScreen);
    lv_obj_set_pos(footer, 0, scrH - FOOTER_H);
    lv_obj_set_size(footer, scrW, FOOTER_H);
    lv_obj_set_style_bg_color(footer, lv_color_hex(0x151C24), 0);
    lv_obj_set_style_border_width(footer, 0, 0);
    lv_obj_set_style_radius(footer, 0, 0);
    lv_obj_clear_flag(footer, LV_OBJ_FLAG_SCROLLABLE);

    settingsStatusLabel = lv_label_create(footer);
    lv_obj_set_style_text_color(settingsStatusLabel, lv_color_hex(0x66CC88), 0);
    lv_label_set_text(settingsStatusLabel, "");
    lv_obj_align(settingsStatusLabel, LV_ALIGN_LEFT_MID, 12, 0);

    lv_obj_t *restoreBtn = lv_button_create(footer);
    lv_obj_set_size(restoreBtn, 90, 24);
    lv_obj_align(restoreBtn, LV_ALIGN_RIGHT_MID, -108, 0);
    lv_obj_set_ext_click_area(restoreBtn, 20); // see backBtn comment — bottom-edge bar, same fix
    lv_obj_add_event_cb(restoreBtn, onRestoreDefaults, LV_EVENT_CLICKED, NULL);
    lv_obj_t *restoreLbl = lv_label_create(restoreBtn);
    lv_label_set_text(restoreLbl, "Defaults");
    lv_obj_center(restoreLbl);

    // Convenience for the rotation slider's "Restart to apply rotation"
    // message (onSliderChanged()) — ESP.restart() directly rather than
    // asking the user to power-cycle by hand. Placed left of Defaults, same
    // 90px/click-area treatment; a bit snug against settingsStatusLabel in
    // portrait's narrower ~320px footer when that specific long message is
    // showing, but this button is otherwise blank/unused so it's a cosmetic
    // crowding at worst, not a functional collision.
    lv_obj_t *restartBtn = lv_button_create(footer);
    lv_obj_set_size(restartBtn, 70, 24);
    lv_obj_align(restartBtn, LV_ALIGN_RIGHT_MID, -208, 0);
    lv_obj_set_style_bg_color(restartBtn, lv_color_hex(0x44505C), 0);
    lv_obj_set_ext_click_area(restartBtn, 20);
    lv_obj_add_event_cb(restartBtn, onRestartBtnClicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *restartLbl = lv_label_create(restartBtn);
    lv_label_set_text(restartLbl, "Restart");
    lv_obj_center(restartLbl);

    lv_obj_t *saveBtn = lv_button_create(footer);
    lv_obj_set_size(saveBtn, 90, 24);
    lv_obj_align(saveBtn, LV_ALIGN_RIGHT_MID, -8, 0);
    lv_obj_set_style_bg_color(saveBtn, lv_color_hex(0x2E7D4F), 0);
    lv_obj_set_ext_click_area(saveBtn, 20);
    lv_obj_add_event_cb(saveBtn, onSaveConfig, LV_EVENT_CLICKED, NULL);
    lv_obj_t *saveLbl = lv_label_create(saveBtn);
    lv_label_set_text(saveLbl, "Save");
    lv_obj_center(saveLbl);

    // On-screen keyboard for the WiFi tab's SSID/password fields — hidden
    // until onWifiTaClicked() binds+shows it, moved to the foreground at
    // that point since it's created here (before buildConfirmOverlay) and
    // would otherwise sit behind it in z-order.
    wifiKeyboard = lv_keyboard_create(settingsScreen);
    lv_obj_set_size(wifiKeyboard, scrW, 150);
    lv_obj_set_pos(wifiKeyboard, 0, scrH - 150);
    lv_obj_add_flag(wifiKeyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(wifiKeyboard, onWifiKbReadyOrCancel, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(wifiKeyboard, onWifiKbReadyOrCancel, LV_EVENT_CANCEL, NULL);

    buildWifiScanOverlay(settingsScreen); // full-screen "WiFi setup" (scan + password) — see its own comment
    lv_obj_move_foreground(wifiKeyboard); // keep the keyboard above the overlay when both show

    buildConfirmOverlay(settingsScreen); // created last so it covers everything (and now the keyboard too)
    buildRestartConfirmOverlay(settingsScreen);
}
