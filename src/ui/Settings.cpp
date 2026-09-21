#include "Settings.h"
#include "Dashboard.h" // dashboardScreen (Back button), applyConfig()
#include "display/DisplayDriver.h" // gfx->width()/height() — orientation-aware layout
#include "core/AppConfig.h"
#include "core/NvsStore.h"
#include "core/SharedState.h" // radarSnapshot()/gnssSnapshot()/roadInfoSnapshot() for the Sensors diagnostics panel
#include "map/SpeedLimitManager.h" // speedSourceStr() — Speed Map group in the Sensors tab
#include "net/WebPortal.h"    // webPortalIsEnabled()/webPortalRequestEnable() — WiFi tab
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
// 23 in use as of the Radar tracking group (2026-09-15) — this had silently
// reached exactly 20/20 (this array's old size) before that addition, i.e.
// the "some headroom left" this comment used to claim was already stale;
// 32 gives real headroom again, checked by counting addSliderRow() call
// sites directly rather than trusting the previous comment a second time.
static SliderBinding sliderBindings[32];
static int sliderCount = 0;

struct SwitchBinding {
    lv_obj_t *sw;
    bool *target;
};
// Bumped 8->12 (2026-09-16, alongside adding tripLoggingEnabled) — that
// addition would have made this array exactly full (8/8) with zero headroom
// left, the same "silently reached exactly N/N" situation sliderBindings'
// own comment above already warns against repeating.
static SwitchBinding switchBindings[12]; // 8 in use as of tripLoggingEnabled (2026-09-16)
static int switchCount = 0;

static lv_obj_t *settingsStatusLabel;

// WiFi tab widgets — declared up top since onWifiSwitchChanged()/
// onWifiKbReadyOrCancel() below (both fairly early in the file) reference
// them; built in buildSettingsScreen() near the bottom, same
// declare-early/build-late split refreshSensorsPanel()'s own widget
// pointers already use.
static lv_obj_t *wifiEnableSwitch, *wifiSsidTa, *wifiPasswordTa;

static void onSliderChanged(lv_event_t *e) {
    SliderBinding *b = (SliderBinding *)lv_event_get_user_data(e);
    int32_t raw = lv_slider_get_value(b->slider);
    *(b->target) = raw / b->divisor;
    char buf[24];
    snprintf(buf, sizeof(buf), "%.1f%s", (double)(*(b->target)), b->unit);
    lv_label_set_text(b->valLabel, buf);
    lv_label_set_text(settingsStatusLabel, "");
    clampConfig(cfg);
    // applyConfig() re-writes the backlight PWM duty — only the brightness
    // slider needs that, but this used to fire on EVERY slider's every
    // drag tick (an LVGL slider fires VALUE_CHANGED many times per drag),
    // so dragging e.g. "Max range" was rewriting the backlight duty
    // dozens of times for no reason. User-reported 2026-09-15 as Settings
    // feeling laggy.
    if (b->target == &cfg.brightness) applyConfig();
    // Rotation can't apply live (see AppConfig.h's screenRotation comment —
    // both screens are laid out once at boot for whichever orientation was
    // active then) — say so immediately rather than let the slider silently
    // do nothing, which is what every OTHER slider here does instead.
    if (b->target == &cfg.screenRotation) {
        lv_label_set_text(settingsStatusLabel, "Restart to apply rotation");
    }
}

static void onSwitchChanged(lv_event_t *e) {
    SwitchBinding *b = (SwitchBinding *)lv_event_get_user_data(e);
    *(b->target) = lv_obj_has_state(b->sw, LV_STATE_CHECKED);
    lv_label_set_text(settingsStatusLabel, "");
    clampConfig(cfg); // no switch affects brightness, so no applyConfig() call needed here
    // Demo mode only takes effect at boot (main_ui_demo.cpp picks
    // simTaskStart() vs. radarTaskStart() once in setup()) — same reasoning
    // as screenRotation's own restart requirement, see AppConfig.h's
    // demoMode comment.
    if (b->target == &cfg.demoMode) {
        lv_label_set_text(settingsStatusLabel, "Restart to apply demo mode");
    }
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
    }
    lv_keyboard_set_textarea(wifiKeyboard, NULL);
    lv_obj_add_flag(wifiKeyboard, LV_OBJ_FLAG_HIDDEN);
}

// Row layout tuned for the ~372px-wide / 260px-tall LANDSCAPE category
// content panel (see buildSettingsScreen) — panels don't scroll (except the
// Sensors tab), so every category's total row count x pitch must stay under
// 260px there. Tightened from 30/24px to 26/22px 2026-09-14 when the Radar
// tab grew to 8 sliders + a diagnostic row; re-check this budget before
// adding more rows to any tab.
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
    snprintf(buf, sizeof(buf), "%.1f%s", (double)(*target), unit);
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
static ChoiceBinding choiceBindings[4]; // 3 in use as of Direction (2026-09-21) — 1 slot free
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
        lv_label_set_text(settingsStatusLabel, "Restart to apply rotation");
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
// "check" fields (Fix, Satellites, Speed, Radar status, Target count).
// No binding/callback, unlike the slider/switch rows above: the value
// label is just handed back so refreshSensorsPanel() can update it on a
// timer. Kept as plain named lv_obj_t* pointers below rather than a generic
// array — there are only 6 of them and each needs different formatting.
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

// The radar Link diagnostic row's value ("frames=... errs=... qty=... alarm=...
// snr=...") is long enough (~40 chars) that it doesn't fit the ~160px-wide
// value column the two-column addReadonlyRow() layout above gives every
// other (short: "OK"/a number/"12.3 km/h") readonly row — confirmed by
// character count against the panel's own ~372px content width, not
// assumed: at this font, ~150px only fits ~20 characters, well under 40.
// Stacked instead: name on its own line, value below spanning nearly the
// full panel width, which comfortably fits the whole string on one line.
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

static lv_obj_t *radarStatusVal, *gnssFixVal, *gnssSatsVal, *gnssSpeedRawVal, *gnssSpeedFilteredVal, *targetCountVal;
static lv_obj_t *radarDiagVal; // Settings > Radar tab — frames/errors/last-frame detail, more than the brief OK/FAULT above
static lv_obj_t *wifiStatusVal; // Settings > WiFi tab — see refreshSensorsPanel()
// Settings > Sensors > "Speed Map" group — see refreshSensorsPanel(). Region/
// version come from speedLimitManagerGetInfo() (static once loaded at boot);
// the rest come from roadInfoSnapshot() (updates every ~500ms).
static lv_obj_t *speedMapStatusVal, *speedMapRegionVal, *speedMapVersionVal;
static lv_obj_t *speedMapLimitVal, *speedMapSourceVal, *speedMapMatchVal, *speedMapRoadIdVal;

static void refreshSensorsPanel(lv_timer_t *) {
    // Only worth doing while Settings is actually the screen on-screen —
    // same class of bug as Dashboard.cpp's simTimerCb (fixed alongside
    // this, user-reported 2026-09-15 laggy Settings interactions): a timer
    // that keeps doing work for a screen nobody's looking at just steals
    // CPU from whatever IS being interacted with.
    if (lv_screen_active() != settingsScreen) return;

    RadarSnapshot radar = radarSnapshot();
    GnssSnapshot gnss = gnssSnapshot();

    lv_label_set_text(radarStatusVal, radar.online ? "OK" : "FAULT");
    lv_obj_set_style_text_color(radarStatusVal, radar.online ? lv_color_hex(0x33CC66) : lv_color_hex(0xFF3B30), 0);

    int activeCount = 0;
    for (int i = 0; i < MAX_TARGETS; i++)
        if (radar.targets[i].active) activeCount++;
    lv_label_set_text_fmt(targetCountVal, "%d", activeCount);

    // User-reported 2026-09-16 seeing radar FAULT and wanting to be sure
    // the link is genuinely OK — this is the evidence: live frame/error
    // counters straight from radar/LD2451.cpp's parser, not just a
    // collapsed OK/FAULT verdict. A healthy link looks like "frames"
    // climbing every refresh with "errs" staying flat.
    lv_label_set_text_fmt(radarDiagVal, "frames=%lu errs=%lu qty=%u alarm=%u snr=%u", radar.framesParsed,
                           radar.parseErrors, radar.lastTargetQty, radar.lastAlarm, radar.lastSnr);

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

    char wifiBuf[48];
    bool wifiOn = webPortalIsEnabled();
    webPortalStatusText(wifiBuf, sizeof(wifiBuf));
    lv_label_set_text(wifiStatusVal, wifiBuf);
    lv_obj_set_style_text_color(wifiStatusVal, wifiOn ? lv_color_hex(0x33CC66) : lv_color_hex(0x7C8A9A), 0);
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
        snprintf(buf, sizeof(buf), "%.1f%s", (double)(*b.target), b.unit);
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
static const int kCategoryCount = 5;
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

    static const char *kCategoryNames[kCategoryCount] = {"Radar", "Safety", "Display", "Sensors", "WiFi"};
    for (int i = 0; i < kCategoryCount; i++) {
        lv_obj_t *btn = lv_button_create(navRail);
        lv_obj_set_size(btn, NAV_W - 8, 46);
        lv_obj_set_pos(btn, 0, i * 52);
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
        // Every OTHER tab's content fits inside BODY_H with no scrolling
        // (each one's own row-budget comment says so). The Sensors tab
        // (i==3) doesn't anymore once the Speed Map diagnostics group is
        // added below — 7 more fields' worth of rows genuinely doesn't fit
        // in 260px alongside the existing GNSS/radar live-status rows, and
        // unlike the Radar tab's earlier close call, tightening row pitch
        // further isn't enough headroom this time. Scrolling is the
        // correct answer for a diagnostics tab that's allowed to be long
        // (spec section 25 calls it "for kiểm tra/cấu hình," not the
        // driving screen), not a workaround.
        //
        // Display (i==2) joined the scrolling tabs 2026-09-16 for the same
        // reason: Brightness+autoDim+4 switches+Theme+Rotation already used
        // 242 of the 260px budget, and adding the new "Simple layout"
        // switch (see cfg.simpleUiMode) pushed it to 268 — 8px over, which
        // would have silently clipped the bottom of the Rotation row
        // instead of just not being able to add anything else.
        //
        // Radar (i==0) joined 2026-09-21 for the same reason again: its 8
        // sliders + the wide Link diagnostic row already used 242px, and
        // adding the new "Radar mounted flipped 180" switch (see
        // cfg.radarMountFlipped) pushed it to 268 — same 8px overage.
        if (i == 3 || i == 2 || i == 0) {
            lv_obj_set_scroll_dir(panel, LV_DIR_VER);
            lv_obj_set_scrollbar_mode(panel, LV_SCROLLBAR_MODE_AUTO);
        } else {
            lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
        }
        lv_obj_add_flag(panel, LV_OBJ_FLAG_HIDDEN);
        categoryPanels[i] = panel;
    }

    int y;
    y = 4;
    addSliderRow(categoryPanels[0], y, "Max range", &cfg.maxRangeM, 10, 100, 1.0f, " m");
    addSliderRow(categoryPanels[0], y, "Min target speed", &cfg.minTargetSpeedKmh, 0, 30, 1.0f, " km/h");
    addSliderRow(categoryPanels[0], y, "Max targets", &cfg.maxTargets, 1, 5, 1.0f, "");
    // Real LD2451 calibration (spec section 16.1) — pushed to the module at
    // boot by radar/LD2451.cpp's configureRadar(). Choice buttons, not a
    // slider (matches the vendor's own BLE config app, which shows this as
    // a labeled picker — e.g. "Stay away and approach" for the module's
    // raw value 2 — not a bare number; a slider here hid which of the 3
    // discrete states was active, same "nut gat chua hop ly" complaint
    // Theme/Rotation above already got 2026-09-16). Index order matches
    // LD2451's own byte encoding exactly (0=away,1=approach,2=both), so
    // onChoiceBtnClicked's `*b->target = (float)i` still writes the same
    // values configureRadar()'s command 0x0002 byte 2 already expects.
    static const char *kDirectionLabels[3] = {"Away", "Approach", "Both"};
    addChoiceRow(categoryPanels[0], y, "Direction", &cfg.radarDirection, kDirectionLabels, 3);
    addSliderRow(categoryPanels[0], y, "SNR sensitivity", &cfg.radarSnrLevel, 0, 8, 1.0f, "");
    addSliderRow(categoryPanels[0], y, "Trigger count", &cfg.radarTriggerCount, 1, 10, 1.0f, "");
    addSliderRow(categoryPanels[0], y, "No-target delay", &cfg.radarNoTargetDelayS, 0, 30, 1.0f, " s");
    // Lane classification width (radar/LD2451.cpp's classifyRelation()) —
    // half of the real lane width a target's lateral offset gets compared
    // against to decide SAME_LANE vs. ADJACENT_LEFT/RIGHT. Added 2026-09-14
    // alongside the switch from a fixed angle-cone to this distance-based
    // check, so it's tunable without a firmware rebuild once real targets
    // let someone judge whether it's classifying correctly.
    addSliderRow(categoryPanels[0], y, "Lane half-width", &cfg.laneHalfWidthM, 5, 50, 10.0f, " m");
    // Mounting flip (see core/AppConfig.h's radarMountFlipped comment for
    // why this is only a 180-degree flip, not a 4-way rotation like the
    // Display tab's screen Rotation row). Applies live — LD2451.cpp reads
    // cfg.radarMountFlipped fresh every frame at raw-angle-decode time, no
    // restart needed.
    addSwitchRow(categoryPanels[0], y, "Radar mounted flipped 180", &cfg.radarMountFlipped);
    // Live evidence for "is the radar link actually OK" (user-reported
    // 2026-09-16) right next to the knobs that affect it — see
    // refreshSensorsPanel() for what updates this. SNR is the raw per-target
    // byte (0-255, see SharedState.h's SimTarget::snr) — not yet folded into
    // confidence (no calibration data exists), shown here so real values can
    // be observed against real targets before that formula gets written.
    // Wide row (see addWideReadonlyRow): this value string is too long for
    // the standard two-column readonly layout.
    radarDiagVal = addWideReadonlyRow(categoryPanels[0], y, "Link");

    y = 4;
    addSliderRow(categoryPanels[1], y, "Audio enable speed", &cfg.audioEnableKmh, 40, 120, 1.0f, " km/h");
    addSliderRow(categoryPanels[1], y, "Hysteresis", &cfg.hysteresisKmh, 1, 10, 1.0f, " km/h");
    addSliderRow(categoryPanels[1], y, "TTC warning", &cfg.ttcWarnS, 10, 100, 10.0f, " s");
    addSliderRow(categoryPanels[1], y, "TTC critical", &cfg.ttcCritS, 5, 50, 10.0f, " s");
    addSliderRow(categoryPanels[1], y, "Min confidence", &cfg.minConfidence, 0, 100, 100.0f, "");
    // Tailgating/harsh-brake thresholds (added 2026-09-16 — see
    // core/AppConfig.h's minDistanceM/harshBrakeAccelMps2 comments). Grouped
    // with the other Safety thresholds above since both feed radar.audioAllowed
    // the same way ttcWarnS/ttcCritS/minConfidence already do.
    addSliderRow(categoryPanels[1], y, "Min distance", &cfg.minDistanceM, 1, 20, 1.0f, " m");
    addSliderRow(categoryPanels[1], y, "Harsh brake sensitivity", &cfg.harshBrakeAccelMps2, 10, 100, 10.0f, " m/s2");
    addSwitchRow(categoryPanels[1], y, "Audio enabled", &cfg.audioEnabled);

    y = 4;
    addSliderRow(categoryPanels[2], y, "Brightness", &cfg.brightness, 5, 100, 1.0f, " %");
    // Label updated 2026-09-16 alongside the gate itself changing from
    // touch-idle to vehicle-stationary time (see Dashboard.cpp) — "stopped"
    // says what actually starts the timer now.
    addSliderRow(categoryPanels[2], y, "Dim after stopped", &cfg.autoDimMin, 0, 30, 1.0f, " min");
    addSwitchRow(categoryPanels[2], y, "Show target ID", &cfg.showId);
    addSwitchRow(categoryPanels[2], y, "Show target speed", &cfg.showSpeed);
    addSwitchRow(categoryPanels[2], y, "Show target TTC", &cfg.showTtc);
    addSwitchRow(categoryPanels[2], y, "Show target angle", &cfg.showAngle);
    // Theme (user-requested 2026-09-15): 0=Auto follows GNSS.cpp's real
    // sunrise/sunset calc (unchanged default behavior), 1=Light/2=Dark force
    // it either way. Applies live — ui/Dashboard.cpp's refreshDashboard()
    // re-checks cfg.themeMode on the same tick it already checks
    // gnss.daytime, no restart needed (see applyTheme()'s call site there).
    // Choice buttons, not a slider (user-reported 2026-09-16, "nut gat chua
    // hop ly") — a 3-way enum on a slider hid which option was active.
    static const char *kThemeLabels[3] = {"Auto", "Light", "Dark"};
    addChoiceRow(categoryPanels[2], y, "Theme", &cfg.themeMode, kThemeLabels, 3);
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
    addChoiceRow(categoryPanels[2], y, "Rotation", &cfg.screenRotation, kRotationLabels, 4);
    // Simple layout (user-requested 2026-09-16, "giao dien don gian ... tang
    // tap trung"): hides roadArea's lane-line/target-icon graphic and shows
    // only the same-lane primary target's distance as one large number —
    // see AppConfig.h's simpleUiMode comment for why this is a bool/switch
    // (not a 3rd choice row: the Display tab's 260px budget had no room
    // left, see buildSettingsScreen's own row-budget comment). Applies live
    // — ui/Dashboard.cpp's refreshDashboard() re-checks it every tick like
    // Theme above, no restart needed.
    addSwitchRow(categoryPanels[2], y, "Simple layout", &cfg.simpleUiMode);

    // Sensors — real-hardware check/configure/calibrate (spec 16.6/16.7).
    // GNSS (u-blox M10N, UART2) went real 2026-09-15, radar (HLK-LD2451,
    // UART1) went real 2026-09-16 — both rows below reflect live hardware
    // state, not simulated data. Radar's own calibration knobs (Direction/
    // SNR/Trigger count/Lane half-width) live in the Radar tab instead of
    // here, next to the Link diagnostic row they affect.
    y = 4;
    // Demo mode (user-requested 2026-09-16, "them 1 nut bat tat thu nghiem
    // trong menu") — placed here rather than the Radar tab (already at its
    // row-budget limit, see that tab's own comment) or Display (same
    // problem after Theme/Rotation's choice rows). Sensors already scrolls
    // (below), so there's no overflow risk, and this IS fundamentally a
    // "which sensor data source" toggle — thematically the closest fit.
    addSwitchRow(categoryPanels[3], y, "Demo mode (fake radar)", &cfg.demoMode);
    // Trip logging (added 2026-09-16, see log/TripLogger.h) — placed here for
    // the same "Sensors tab already scrolls, no row-budget pressure" reason
    // demoMode's own comment above gives.
    addSwitchRow(categoryPanels[3], y, "Trip logging (SD card)", &cfg.tripLoggingEnabled);
    y += 6; // extra breathing room before the real live-status rows below

    lv_obj_t *sensorsHeader1 = lv_label_create(categoryPanels[3]);
    lv_label_set_text(sensorsHeader1, "Live status");
    lv_obj_set_style_text_color(sensorsHeader1, lv_color_hex(0x7C8A9A), 0);
    lv_obj_set_pos(sensorsHeader1, 4, y);
    y += 20;
    radarStatusVal = addReadonlyRow(categoryPanels[3], y, "Radar");
    targetCountVal = addReadonlyRow(categoryPanels[3], y, "Active targets");
    gnssFixVal = addReadonlyRow(categoryPanels[3], y, "GNSS fix");
    gnssSatsVal = addReadonlyRow(categoryPanels[3], y, "Satellites");
    gnssSpeedRawVal = addReadonlyRow(categoryPanels[3], y, "Speed (raw)");
    gnssSpeedFilteredVal = addReadonlyRow(categoryPanels[3], y, "Speed (filtered)");

    y += 6;
    lv_obj_t *sensorsHeader2 = lv_label_create(categoryPanels[3]);
    lv_label_set_text(sensorsHeader2, "GNSS calibration");
    lv_obj_set_style_text_color(sensorsHeader2, lv_color_hex(0x7C8A9A), 0);
    lv_obj_set_pos(sensorsHeader2, 4, y);
    y += 20;
    addSliderRow(categoryPanels[3], y, "Speed filter smoothing", &cfg.gnssSpeedFilterAlpha, 5, 90, 100.0f, "");
    addSliderRow(categoryPanels[3], y, "Fix timeout", &cfg.gnssFixTimeoutS, 10, 100, 10.0f, " s");

    // Radar tracking calibration (radar/LD2451.cpp's updateTracks(), spec
    // section 9.1's "phải là configurable/tunable" persistence values) —
    // placed here rather than the Radar tab (already at its row-budget
    // limit, see that tab's own comment) and this tab already scrolls.
    y += 6;
    lv_obj_t *sensorsHeader4 = lv_label_create(categoryPanels[3]);
    lv_label_set_text(sensorsHeader4, "Radar tracking");
    lv_obj_set_style_text_color(sensorsHeader4, lv_color_hex(0x7C8A9A), 0);
    lv_obj_set_pos(sensorsHeader4, 4, y);
    y += 20;
    addSliderRow(categoryPanels[3], y, "Confirm frames", &cfg.radarTrackConfirmFrames, 2, 3, 1.0f, "");
    addSliderRow(categoryPanels[3], y, "Loss timeout", &cfg.radarTrackLossS, 2, 20, 10.0f, " s");
    addSliderRow(categoryPanels[3], y, "Filter smoothing", &cfg.radarTrackFilterAlpha, 10, 90, 100.0f, "");

    // Speed Map diagnostics (spec section 25) — offline microSD map-matching
    // status, see map/SpeedLimitManager.h. This group is why categoryPanels[3]
    // needed to become scrollable above: it doesn't fit in 260px alongside
    // everything already in this tab.
    y += 6;
    lv_obj_t *sensorsHeader3 = lv_label_create(categoryPanels[3]);
    lv_label_set_text(sensorsHeader3, "Speed Map");
    lv_obj_set_style_text_color(sensorsHeader3, lv_color_hex(0x7C8A9A), 0);
    lv_obj_set_pos(sensorsHeader3, 4, y);
    y += 20;
    speedMapStatusVal = addReadonlyRow(categoryPanels[3], y, "Status");
    speedMapRegionVal = addReadonlyRow(categoryPanels[3], y, "Region");
    speedMapVersionVal = addReadonlyRow(categoryPanels[3], y, "Version");
    speedMapLimitVal = addReadonlyRow(categoryPanels[3], y, "Current limit");
    speedMapSourceVal = addReadonlyRow(categoryPanels[3], y, "Source");
    speedMapMatchVal = addReadonlyRow(categoryPanels[3], y, "Match");
    speedMapRoadIdVal = addReadonlyRow(categoryPanels[3], y, "Road ID");

    // WiFi tab (user-requested 2026-09-14 alongside the Dashboard's 4s hold
    // gesture — see net/WebPortal.h). Defaults OFF every boot; this switch
    // and the gesture both funnel through the same webPortalRequestEnable().
    bool wifiTabNarrow = lv_obj_get_width(categoryPanels[4]) < kNarrowPanelThreshold;
    y = 4;
    {
        lv_obj_t *nameLbl = lv_label_create(categoryPanels[4]);
        lv_label_set_text(nameLbl, "WiFi enabled");
        lv_obj_set_style_text_color(nameLbl, lv_color_hex(0xCCD6E0), 0);
        lv_obj_set_pos(nameLbl, 4, y + 3);
        wifiEnableSwitch = lv_switch_create(categoryPanels[4]);
        lv_obj_set_pos(wifiEnableSwitch, lv_obj_get_width(categoryPanels[4]) - 46, y);
        lv_obj_add_event_cb(wifiEnableSwitch, onWifiSwitchChanged, LV_EVENT_VALUE_CHANGED, NULL);
        y += 26;
    }

    // Textareas stack label-above-field in a narrow (portrait) panel, same
    // threshold/reasoning as addSliderRow()'s own narrow case — the fixed
    // x=140/width=220 landscape layout below would run off the edge of a
    // ~220px-wide portrait content column otherwise.
    lv_obj_t *ssidLbl = lv_label_create(categoryPanels[4]);
    lv_label_set_text(ssidLbl, "SSID");
    lv_obj_set_style_text_color(ssidLbl, lv_color_hex(0xCCD6E0), 0);
    wifiSsidTa = lv_textarea_create(categoryPanels[4]);
    lv_textarea_set_one_line(wifiSsidTa, true);
    lv_textarea_set_max_length(wifiSsidTa, sizeof(cfg.wifiSsid) - 1);
    lv_textarea_set_text(wifiSsidTa, cfg.wifiSsid);
    if (wifiTabNarrow) {
        lv_obj_set_pos(ssidLbl, 4, y);
        lv_obj_set_pos(wifiSsidTa, 4, y + 18);
        lv_obj_set_size(wifiSsidTa, lv_obj_get_width(categoryPanels[4]) - 8, 28);
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
    lv_obj_t *passLbl = lv_label_create(categoryPanels[4]);
    lv_label_set_text(passLbl, "Password");
    lv_obj_set_style_text_color(passLbl, lv_color_hex(0xCCD6E0), 0);
    wifiPasswordTa = lv_textarea_create(categoryPanels[4]);
    lv_textarea_set_one_line(wifiPasswordTa, true);
    lv_textarea_set_password_mode(wifiPasswordTa, true);
    lv_textarea_set_max_length(wifiPasswordTa, sizeof(cfg.wifiPassword) - 1);
    lv_textarea_set_placeholder_text(wifiPasswordTa, "(unchanged)");
    if (wifiTabNarrow) {
        lv_obj_set_pos(passLbl, 4, y);
        lv_obj_set_pos(wifiPasswordTa, 4, y + 18);
        lv_obj_set_size(wifiPasswordTa, lv_obj_get_width(categoryPanels[4]) - 8, 28);
        y += 50;
    } else {
        lv_obj_set_pos(passLbl, 4, y + 6);
        lv_obj_set_pos(wifiPasswordTa, 140, y);
        lv_obj_set_size(wifiPasswordTa, 220, 28);
        y += 34;
    }
    lv_obj_add_event_cb(wifiPasswordTa, onWifiTaClicked, LV_EVENT_CLICKED, NULL);

    wifiStatusVal = addWideReadonlyRow(categoryPanels[4], y, "Status");

    // NOT called eagerly here: buildSettingsScreen() runs before
    // sharedStateInit() in main_ui_demo.cpp's setup(), so
    // radarSnapshot()/gnssSnapshot()'s mutex doesn't exist yet — an eager
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
    lv_obj_add_event_cb(
        restartBtn, [](lv_event_t *) { ESP.restart(); }, LV_EVENT_CLICKED, NULL);
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

    buildConfirmOverlay(settingsScreen); // created last so it covers everything (and now the keyboard too)
}
