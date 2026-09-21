#include "Dashboard.h"
#include "Settings.h" // settingsScreen, for the hold-to-open-Settings gesture
#include "core/AppConfig.h"
#include "core/NvsStore.h" // saveConfigToNVS()
#include "core/SharedState.h"
#include "display/DisplayDriver.h" // backlightWrite
#include "gnss/GNSS.h" // gnssMsSinceStationary() — auto-dim's vehicle-stationary gate
#include "icons/Icons.h"
#include "net/WebPortal.h"
#include "audio/AudioPlayer.h" // webPortalIsEnabled()/webPortalRequestEnable() — the 4s hold gesture toggles WiFi
#include <math.h>
#include <string.h>

// ---------------------------------------------------------------------
// VietHUD Dashboard — GPS-only offline speed-limit/camera/sign warning
// display. Radar (HLK-LD2451) and everything it drove (TTC/risk-color
// warning cell, per-target road panel, audio-gate hysteresis, tailgating/
// harsh-brake banners) was removed entirely 2026-09-21 — this project is no
// longer a forward-collision radar display, see the repo's own README for
// the product this became. The layout below is what's left after that
// removal, reflowed: [ speed + speed-limit sign | sign/camera alert card ],
// with a top status bar (GNSS/clock/settings) and a one-line bottom info
// bar (speed-map region/version + GNSS satellite count). Two decisions
// carried over from the original radar-era layout, both because the
// compiled-in LVGL font can't render what a nicer mockup would want
// (verified directly against lv_font_montserrat_14.c's cmap — ASCII +
// ~60 symbol codepoints only, no emoji, no Vietnamese diacritics):
//   - All labels stay English/ASCII (no custom Vietnamese font asset) —
//     the sign/camera alert card's own text uses plain-ASCII transliterated
//     Vietnamese (e.g. "KHU DONG DAN CU") for the same reason.
//   - Warning/sun/moon icons are small generated bitmaps (see
//     ui/icons/Icons.h for how and why) instead of font glyphs.
// GPS/Settings telltales keep using the built-in LV_SYMBOL_* glyphs
// (LV_SYMBOL_GPS/SETTINGS/WIFI) — those aren't emoji, they're already part
// of the compiled font.
// ---------------------------------------------------------------------

// ---------------------------------------------------------------------
// Screen burn-in / image retention mitigation
// ---------------------------------------------------------------------
// Two independent measures, both required because the Dashboard keeps
// several high-contrast elements in a fixed spot for the whole trip (top
// status bar, column dividers, bottom info bar):
//   1. Pixel shift — the entire dashboard is nudged a couple of pixels on a
//      slow cycle so no edge sits on the same physical pixels for hours.
//   2. Auto-dim — backlight drops to a low level after the vehicle has been
//      continuously stationary for a configurable period (default 3 min —
//      user-requested 2026-09-16, "che do tu giam do sang man hinh chi duoc
//      thuc hien khi xe khong chuyen dong sau 3 phut"); a touch OR the
//      vehicle moving again both restore it. Lower luminance dramatically
//      slows image retention on IPS panels. Gated on GNSS speed
//      (gnssMsSinceStationary(), GNSS.h) rather than touch-idle time alone —
//      never dim while the driver is relying on the warning display during
//      an actual drive, even if they never touch the screen.
static const int kPixelShiftOffsets[4][2] = {{0, 0}, {2, 0}, {2, 2}, {0, 2}};
static int pixelShiftIdx = 0;
static uint32_t lastTouchMs = 0;
static bool screenDimmed = false;

void applyConfig() {
    float level = screenDimmed ? 12.0f : cfg.brightness;
    backlightWrite((uint32_t)(level / 100.0f * 255.0f));
}

void wakeScreen() {
    lastTouchMs = millis();
    if (screenDimmed) {
        screenDimmed = false;
        applyConfig();
    }
}

uint32_t lastTouchAtMs() { return lastTouchMs; }

// Semantic risk color — the ONLY place red/amber/purple are used to mean
// "something needs the driver's attention" (speeding, an upcoming sign).
// System telltales (GPS/Settings) use the cyan accent below instead,
// precisely so a "system OK" color is never confused with a "warning"
// color (spec section 14.2, carried over from the radar-era Dashboard).
#define ACCENT_COLOR 0x3FCAD6

// applyTheme()'s last-computed "normal" text color for speedLabel —
// remembered so refreshDashboard() can restore it after an overspeed
// tick's red override without needing to know which theme is active itself.
static lv_color_t currentPrimaryTextColor = lv_color_white();

static void applyTheme(bool daytime); // defined below buildDashboard(), which calls it once at the end to set the initial theme

// ---------------------------------------------------------------------
// Screen + layout constants
// ---------------------------------------------------------------------
lv_obj_t *dashboardScreen;
static lv_obj_t *dashRoot; // everything sits in here so it can be pixel-shifted

// Landscape-only layout constants (rotation 1 or 3 — see AppConfig.h's
// screenRotation — both produce this exact 480x320 logical canvas, per
// Arduino_GFX's own rotation convention, so hardcoding is safe here the same
// way it always was before rotation became configurable). Portrait
// (rotation 0/2, 320x480) has its own separate constants inside
// buildDashboardPortrait() — the two layouts are different ARRANGEMENTS of
// the same information, not one parametric layout stretched to fit either
// shape (a 3-column layout doesn't sensibly reflow into a tall narrow
// screen; see the plan this was built from for why a second, purpose-built
// layout was chosen over trying to make one layout adapt to both).
//
// LS_-prefixed (2026-09-22, landscape-corruption investigation rewrite):
// buildDashboardPortrait() has its OWN locals named scrW/scrH/etc — same
// semantic role, different numbers. They were never actually mixed up
// anywhere (checked), but the shared/unprefixed names were flagged as a
// real hazard for a future edit, so this rewrite renames the landscape set
// to make that class of mistake impossible rather than just improbable.
static const int LS_SCR_W = 480, LS_SCR_H = 320;
static const int LS_TOP_H = 30, LS_BOTTOM_H = 26;
// Split 50/50 (user-requested 2026-09-16, "chia doi man hinh... phan hien
// thi xe sang 1 ben, nua man hinh con lai la cac thong so") — replaces the
// old 96|288|96 three-column split. LS_PARAM_COL (left half) holds every
// number/status readout (speed, speed limit sign, warning, TTC) stacked in
// ONE evenly-spaced column (retuned 2026-09-16, "bo tri hop ly, can bang va
// deu nhau" — an earlier two-sub-column attempt left mismatched gaps, see
// buildDashboardLandscape()'s own comment at that block); LS_ROAD_COL (right
// half) is just the road/target view, now much bigger than the old 288px-
// wide middle column.
static const int LS_PARAM_COL_W = 240, LS_ROAD_COL_W = LS_SCR_W - LS_PARAM_COL_W;
static const int LS_ROAD_COL_X = LS_PARAM_COL_W;
static const int LS_COL_TOP = LS_TOP_H, LS_COL_H = LS_SCR_H - LS_TOP_H - LS_BOTTOM_H;

// --- Top bar ---
static lv_obj_t *gnssIcon, *gnssCaption;
static lv_obj_t *sunIcon, *clockLabel;
static lv_obj_t *gearIcon;
static lv_obj_t *wifiTopIcon; // shown only while WiFi is on — see refreshDashboard()
static lv_obj_t *colDividerLine[2];

// --- Left column: speed + speed limit ---
// Audio icon/state label removed 2026-09-15 (user-requested) — never
// re-added; cfg.audioEnabled (AppConfig.h) is now the sign/camera/speed-
// limit alert-audio master toggle (Settings > Display), with no on-screen
// telltale, same as there being no buzzer output yet either.
static lv_obj_t *leftCol;
static lv_obj_t *speedLabel, *kmhCaption;
// Speed limit shown as an actual Vietnamese regulatory sign (QCVN 41:2019/
// BGTVT's P.127 "Tốc độ tối đa cho phép" — white circle, red ring, black
// number, no other markings), not a text readout — user-requested
// 2026-09-15. speedLimitSign is the ring/circle graphic (hidden when there's
// no valid limit — a real sign has no "unknown" state, so this dashboard
// doesn't invent one); speedLimitValueLabel is a SEPARATE sibling (not a
// child of the sign) positioned on top of it via align_to, so it can keep
// showing a plain "--" (same convention every other not-yet-available
// reading on this dashboard uses) even while the sign graphic itself is
// hidden.
static lv_obj_t *speedLimitSign, *speedLimitValueLabel;

// --- Middle column: sign/camera alert card ---
static lv_obj_t *midCol;
// Upcoming speed-limit-change banner (user-requested 2026-09-21, "canh bao
// gioi han toc do doan duong tiep theo, bao truoc khoang 100m") — one
// shared widget, built once, working unchanged in either orientation (see
// its build site in buildDashboard()). Own dedicated color (blue, a common
// real-world "informational" road-sign color), deliberately NOT red/amber,
// so it never reads as a safety alert the way the speeding overlay does —
// this is advance notice of a rule change ahead, not a danger warning.
static lv_obj_t *aheadLimitLabel;
// Speed-camera-ahead banner (feature-requested 2026-09-21, "tai du lieu ve
// canh bao giao thong, gom camera") — same shared-object pattern as the
// two banners above. Own color (amber) distinct from BOTH aheadLimitLabel's
// blue (an informational rule-change notice) and the TTC/speeding
// overlays' red (imminent collision) — a camera is neither: it's a real,
// location-specific reason to check your speed NOW, warranting more
// attention than a passive rule notice but not a collision alarm.
// Positioned below both of them (y=54 vs. their 2/28) so all three can
// stack without overlapping on the rare tick more than one is visible.
static lv_obj_t *cameraAheadLabel;
static lv_obj_t *trafficSignLabel;

// Dedicated Traffic & Camera Alert Card (Redesigned HUD for GPS Speed & Traffic Signs)
static lv_obj_t *trafficCard = nullptr;
static lv_obj_t *alertBadgeLabel = nullptr;
static lv_obj_t *alertDistLabel = nullptr;
static lv_obj_t *alertUnitLabel = nullptr;
static lv_obj_t *alertSubLabel = nullptr;
static lv_obj_t *alertProgressBar = nullptr;
static lv_obj_t *alertFooterLabel = nullptr;

static void buildTrafficCard(lv_obj_t *parent, int w, int h) {
    trafficCard = lv_obj_create(parent);
    lv_obj_set_size(trafficCard, w, h);
    lv_obj_set_pos(trafficCard, (lv_obj_get_width(parent) - w) / 2, 8);
    lv_obj_set_style_bg_color(trafficCard, lv_color_hex(0x111720), 0);
    lv_obj_set_style_border_color(trafficCard, lv_color_hex(0x283848), 0);
    lv_obj_set_style_border_width(trafficCard, 2, 0);
    lv_obj_set_style_radius(trafficCard, 10, 0);
    lv_obj_set_style_pad_all(trafficCard, 4, 0);
    lv_obj_clear_flag(trafficCard, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(trafficCard, LV_OBJ_FLAG_CLICKABLE);

    // Badge pill at top. Explicit width + clip (2026-09-22 — see this
    // file's buildDashboardLandscape() comment on bottomInfoLabel for why
    // every runtime-text label in this dashboard now gets one): the longest
    // real value here is "DEN TIN HIEU GIAO THONG" (refreshDashboard()),
    // which was already comfortably inside the card at content-hug size, so
    // pinning the width to the card's own usable interior just forecloses
    // ever going wider than the card instead of trusting that to stay true.
    alertBadgeLabel = lv_label_create(trafficCard);
    lv_obj_set_style_bg_color(alertBadgeLabel, lv_color_hex(0x202A36), 0);
    lv_obj_set_style_bg_opa(alertBadgeLabel, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(alertBadgeLabel, lv_color_hex(0x88A0B8), 0);
    lv_obj_set_style_text_font(alertBadgeLabel, &lv_font_montserrat_14, 0);
    lv_obj_set_style_pad_hor(alertBadgeLabel, 10, 0);
    lv_obj_set_style_pad_ver(alertBadgeLabel, 3, 0);
    lv_obj_set_style_radius(alertBadgeLabel, 12, 0);
    lv_obj_set_width(alertBadgeLabel, w - 16);
    lv_label_set_long_mode(alertBadgeLabel, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(alertBadgeLabel, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(alertBadgeLabel, LV_ALIGN_TOP_MID, 0, 6);
    lv_label_set_text(alertBadgeLabel, "DUONG THONG THOANG");

    // Big countdown distance number
    alertDistLabel = lv_label_create(trafficCard);
    lv_obj_set_style_text_font(alertDistLabel, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(alertDistLabel, lv_color_white(), 0);
    lv_obj_align(alertDistLabel, LV_ALIGN_CENTER, -10, -18);
    lv_label_set_text(alertDistLabel, "--");

    alertUnitLabel = lv_label_create(trafficCard);
    lv_obj_set_style_text_font(alertUnitLabel, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(alertUnitLabel, lv_color_hex(0x8899AA), 0);
    lv_obj_align_to(alertUnitLabel, alertDistLabel, LV_ALIGN_OUT_RIGHT_BOTTOM, 4, -8);
    lv_label_set_text(alertUnitLabel, "");

    // Subtitle / limit speed. Explicit width + clip, same reasoning as
    // alertBadgeLabel above — refreshDashboard() sets this to several
    // different runtime-built strings (e.g. "GPS Tot (%d ve tinh)").
    alertSubLabel = lv_label_create(trafficCard);
    lv_obj_set_style_text_font(alertSubLabel, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(alertSubLabel, lv_color_hex(0xCCD4DC), 0);
    lv_obj_set_width(alertSubLabel, w - 16);
    lv_label_set_long_mode(alertSubLabel, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(alertSubLabel, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(alertSubLabel, LV_ALIGN_CENTER, 0, 30);
    lv_label_set_text(alertSubLabel, "Dang tim GPS...");

    // Progress bar
    alertProgressBar = lv_bar_create(trafficCard);
    lv_obj_set_size(alertProgressBar, w - 36, 6);
    lv_obj_align(alertProgressBar, LV_ALIGN_BOTTOM_MID, 0, -32);
    lv_bar_set_range(alertProgressBar, 0, 350);
    lv_bar_set_value(alertProgressBar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(alertProgressBar, lv_color_hex(0x202B38), 0);
    lv_obj_set_style_bg_color(alertProgressBar, lv_color_hex(0xE0A020), LV_PART_INDICATOR);
    lv_obj_set_style_radius(alertProgressBar, 3, 0);
    lv_obj_set_style_radius(alertProgressBar, 3, LV_PART_INDICATOR);

    // Region/version hint at very bottom (was a live radar-target hint
    // before radar removal 2026-09-21 — see bottomInfoLabel below, which
    // already shows this same speed-map info in the outer bottom bar; this
    // card-local line stays a static "Offline VietHUD" caption instead of
    // duplicating a value that changes every tick right above it).
    // Static text (never rewritten after this), but given the same
    // explicit width + clip as every other label in this card for
    // consistency — cheap insurance, not because this one was suspected.
    alertFooterLabel = lv_label_create(trafficCard);
    lv_obj_set_style_text_font(alertFooterLabel, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(alertFooterLabel, lv_color_hex(0x607890), 0);
    lv_obj_set_width(alertFooterLabel, w - 16);
    lv_label_set_long_mode(alertFooterLabel, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(alertFooterLabel, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(alertFooterLabel, LV_ALIGN_BOTTOM_MID, 0, -6);
    lv_label_set_text(alertFooterLabel, "VietHUD - Offline GPS speed guide");
}

// --- Bottom info bar ---
static lv_obj_t *bottomInfoLabel;

// Full-screen speeding overlay (user-requested 2026-09-16, "neu vuot qua
// toc do toi da thi cung canh bao bang layer mau toan man hinh", then "qua
// toc do cung nhap nhay de tang su chu y") — its own dedicated color
// (0xB33DC6, distinct from the red/amber the sign/camera banners use) so
// speeding never reads as "you're about to hit something" or gets confused
// with a routine sign notice. Blinks at its own rate — see
// refreshDashboard()'s own comment on this overlay.
static lv_obj_t *speedingFlashOverlay;

// --- WiFi on/off gesture toast (see onDashLongPressedRepeat()/showWifiToast() below) ---
static lv_obj_t *wifiToastLabel;
static uint32_t wifiToastUntilMs = 0;
static void showWifiToast(bool on); // defined below buildDashboard(), called from onDashLongPressedRepeat() above it

// Long-press-to-open-Settings feedback: a ring that sweeps closed over the
// hold duration, so a press registers visually right away instead of the
// screen doing nothing for a full second (spec section 17 discoverability).
static const int kHoldRingDiam = 48;
static lv_obj_t *holdRing;
static lv_anim_t holdRingAnim;

static void holdRingAnimCb(void *var, int32_t v) { lv_arc_set_value((lv_obj_t *)var, v); }

static void hideHoldRing() {
    lv_anim_delete(holdRing, holdRingAnimCb);
    lv_obj_add_flag(holdRing, LV_OBJ_FLAG_HIDDEN);
}

// Two gestures share the same press-and-hold on dashRoot, distinguished by
// duration: ~1s (HOLD_PRESS_MS, the existing ring animation's own duration,
// user-requested 2026-09-14) opens Settings; holding to 4s+ instead toggles
// WiFi. The longer tier suppresses the shorter one on release — a 4s WiFi
// hold doesn't ALSO open Settings. LVGL's indev only exposes ONE long-press
// threshold directly (lv_indev_set_long_press_time(), already used for the
// 1s point) — the 4s point is measured by hand from pressStartMs, sampled
// on LV_EVENT_LONG_PRESSED_REPEAT (which LVGL fires periodically for as
// long as the press continues past the 1s mark).
//
// A third tier (2s hold) and a double-tap gesture used to live here too,
// both toggling cfg.simpleUiMode (a radar-target-distance display mode) —
// removed 2026-09-21 alongside cfg.simpleUiMode itself when radar was taken
// out entirely (there's no more target distance for that mode to show).
static uint32_t pressStartMs = 0;
static bool longPressFired = false;    // past the 1s mark at least
static bool wifiToggledThisPress = false; // past the 4s mark — latched so it can't fire twice for one press
static const uint32_t kWifiHoldMs = 4000;

// Guards onDashReleasedOrLost's body from running more than once per
// physical press. LVGL can fire BOTH LV_EVENT_RELEASED and LV_EVENT_PRESS_LOST
// for what a user experiences as one clean tap (this board's touch
// controller has a known occasional stuck/glitch quirk — see
// AXS15231BTouch.cpp — that can plausibly trigger this).
static bool releaseHandledThisPress = false;

static void onDashPressed(lv_event_t *e) {
    lv_indev_t *indev = lv_event_get_indev(e);
    if (!indev) return;
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    lv_obj_set_pos(holdRing, p.x - kHoldRingDiam / 2, p.y - kHoldRingDiam / 2);
    lv_arc_set_value(holdRing, 0);
    lv_obj_clear_flag(holdRing, LV_OBJ_FLAG_HIDDEN);

    lv_anim_init(&holdRingAnim);
    lv_anim_set_var(&holdRingAnim, holdRing);
    lv_anim_set_exec_cb(&holdRingAnim, holdRingAnimCb);
    lv_anim_set_values(&holdRingAnim, 0, 100);
    lv_anim_set_time(&holdRingAnim, HOLD_PRESS_MS);
    lv_anim_start(&holdRingAnim);

    pressStartMs = millis();
    longPressFired = false;
    wifiToggledThisPress = false;
    releaseHandledThisPress = false;
}

static void onDashLongPressed(lv_event_t *) { longPressFired = true; }

static void onDashLongPressedRepeat(lv_event_t *) {
    uint32_t heldMs = millis() - pressStartMs;
    if (wifiToggledThisPress) return;
    if (heldMs < kWifiHoldMs) return;
    wifiToggledThisPress = true;
    hideHoldRing();
    bool nowOn = !webPortalIsEnabled();
    webPortalRequestEnable(nowOn);
    Serial.printf("[uidemo] WiFi %s via 4s hold gesture\n", nowOn ? "ON" : "OFF");
    showWifiToast(nowOn);
}

static void onDashReleasedOrLost(lv_event_t *) {
    hideHoldRing();
    if (releaseHandledThisPress) return; // see its own declaration comment
    releaseHandledThisPress = true;
    if (wifiToggledThisPress) return; // already handled above — don't also open Settings
    if (longPressFired) {
        lv_screen_load(settingsScreen);
        return;
    }
}

// Plain, non-interactive container — used for the top-bar row and the three
// columns. Transparent/borderless so it's invisible except for its children.
static lv_obj_t *makePane(lv_obj_t *parent, int x, int y, int w, int h) {
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_set_style_radius(o, 0, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE);
    return o;
}

static lv_obj_t *makeIcon(lv_obj_t *parent, const lv_image_dsc_t *src) {
    lv_obj_t *img = lv_image_create(parent);
    lv_image_set_src(img, src);
    lv_obj_clear_flag(img, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(img, LV_OBJ_FLAG_CLICKABLE);
    return img;
}

// ---------------------------------------------------------------------
// Landscape content (rotation 1/3, 480x320) — everything between the shared
// dashRoot setup and the shared full-screen overlays in buildDashboard()
// below. Rewritten from scratch 2026-09-22 while chasing a real-hardware
// landscape rendering-corruption report (bottom bar text appearing
// vertically, a large isolated glyph) — the original arithmetic checked out
// on paper and no root cause was confirmed via LVGL logs, so rather than
// keep guessing this function was redone plain, with every dynamic-text
// label given an explicit width + clipped long-mode (see bottomInfoLabel
// below) so unconstrained content-hug sizing can't itself be a factor,
// whatever the real cause turns out to be. Same visual result as before:
// [ speed + speed-limit sign | sign/camera alert card ], top status bar,
// one-line bottom info bar. Still plain lv_obj_create/lv_obj_set_pos/
// lv_obj_set_size throughout (via makePane()) — no flex/grid anywhere in
// this file, deliberately, so there's no auto-sizing container that could
// ever collapse a child to near-zero.
// ---------------------------------------------------------------------
static void buildDashboardLandscape(lv_obj_t *scr) {
    // ---------------- Top status bar ----------------
    // Two zones, matching the 50/50 body split below: topLeft sits over
    // LS_PARAM_COL, topRight over LS_ROAD_COL (sun/clock + settings gear —
    // trip/time info alongside the alert card). topRight's internal
    // arrangement (sun-left/clock-growing-right, gear pinned right, wifi
    // left of gear) matches buildDashboardPortrait()'s own topRight.
    //
    // Status is color-only, no OK/FAULT/SEARCH words (direct request):
    // GREEN = normal, RED = fault (nothing received at all), blinking AMBER
    // = pending/searching (module alive, just no fix yet) — see
    // refreshDashboard(). GNSS centers alone in topLeft's full 240px width
    // (there's no second telltale sharing this bar anymore since radar was
    // removed 2026-09-21).
    lv_obj_t *topLeft = makePane(scr, 0, 0, LS_PARAM_COL_W, LS_TOP_H);
    lv_obj_t *topRight = makePane(scr, LS_ROAD_COL_X, 0, LS_ROAD_COL_W, LS_TOP_H);

    // GNSS icon+"GNSS" caption is ~64px wide (20px GPS glyph + 4px gap +
    // ~40px text); centered in topLeft's full 240px width gives (240-64)/2
    // = 88px left margin.
    gnssIcon = lv_label_create(topLeft);
    lv_label_set_text(gnssIcon, LV_SYMBOL_GPS);
    lv_obj_align(gnssIcon, LV_ALIGN_LEFT_MID, 88, 0);

    gnssCaption = lv_label_create(topLeft);
    lv_label_set_text(gnssCaption, "GNSS");
    lv_obj_align_to(gnssCaption, gnssIcon, LV_ALIGN_OUT_RIGHT_MID, 4, 0);

    // Sun/moon icon anchored first (fixed 22x22, constant size) and the
    // clock text chained off its RIGHT edge, growing away from it — the
    // other order (icon chained off the clock) would break the moment the
    // clock's digit count/width changes. Centered in topRight's LEFT 120px
    // zone: block is ~71px wide (22px icon + 4px gap + ~45px "HH:MM" text),
    // centered in 120px gives (120-71)/2 = ~25px left margin.
    sunIcon = makeIcon(topRight, &sun_icon);
    lv_obj_align(sunIcon, LV_ALIGN_LEFT_MID, 25, 0);

    // clockLabel shows a fixed-format "HH:MM" or "--:--" (see
    // refreshDashboard()) — never more than 5 chars, but pinned to an
    // explicit width + clip anyway (same blanket rule this rewrite applies
    // to every runtime-text label) rather than trusting content-hug sizing.
    clockLabel = lv_label_create(topRight);
    lv_obj_set_width(clockLabel, 46);
    lv_label_set_long_mode(clockLabel, LV_LABEL_LONG_CLIP);
    lv_obj_align_to(clockLabel, sunIcon, LV_ALIGN_OUT_RIGHT_MID, 4, 0);

    // Decorative only — the actual entry point is the long-press-anywhere
    // gesture on dashRoot (deliberately not a short tap, so it can't be hit
    // by accident while driving; spec section 17).
    gearIcon = lv_label_create(topRight);
    lv_label_set_text(gearIcon, LV_SYMBOL_SETTINGS);
    lv_obj_align(gearIcon, LV_ALIGN_RIGHT_MID, -4, 0);

    // WiFi indicator (user-requested 2026-09-14) — chained off gearIcon's
    // actual left edge via align_to, same "no guessed pixel gaps" reasoning
    // as gnssCaption above. Cyan accent, not red/amber/green: this is a
    // system telltale ("WiFi radio is on"), not a target-risk color (spec
    // section 14.2 — see ACCENT_COLOR's own comment). Hidden by default:
    // WiFi itself defaults OFF at boot (net/WebPortal.h), and
    // refreshDashboard() only un-hides this once webPortalIsEnabled() is
    // actually true.
    wifiTopIcon = lv_label_create(topRight);
    lv_label_set_text(wifiTopIcon, LV_SYMBOL_WIFI);
    lv_obj_set_style_text_color(wifiTopIcon, lv_color_hex(ACCENT_COLOR), 0);
    lv_obj_align_to(wifiTopIcon, gearIcon, LV_ALIGN_OUT_LEFT_MID, -8, 0);
    lv_obj_clear_flag(wifiTopIcon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(wifiTopIcon, LV_OBJ_FLAG_HIDDEN);

    // ---------------- Column divider ----------------
    // At the LS_PARAM_COL / LS_ROAD_COL boundary. Slot 1 is a degenerate
    // (zero-length, hidden) placeholder — applyTheme() unconditionally
    // recolors colDividerLine[0]/[1], same reasoning buildDashboardPortrait()
    // uses for having no real divider at all.
    static lv_point_precise_t divPts[2][2];
    {
        lv_obj_t *line = lv_line_create(scr);
        lv_obj_set_style_line_width(line, 1, 0);
        lv_point_precise_t local[2] = {{(lv_value_precise_t)LS_PARAM_COL_W, (lv_value_precise_t)LS_COL_TOP},
                                        {(lv_value_precise_t)LS_PARAM_COL_W, (lv_value_precise_t)(LS_COL_TOP + LS_COL_H)}};
        memcpy(divPts[0], local, sizeof(local));
        lv_line_set_points(line, divPts[0], 2);
        lv_obj_clear_flag(line, LV_OBJ_FLAG_CLICKABLE);
        colDividerLine[0] = line;
    }
    {
        lv_obj_t *line = lv_line_create(scr);
        lv_point_precise_t local[2] = {{0, 0}, {0, 0}};
        memcpy(divPts[1], local, sizeof(local));
        lv_line_set_points(line, divPts[1], 2);
        lv_obj_add_flag(line, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(line, LV_OBJ_FLAG_CLICKABLE);
        colDividerLine[1] = line;
    }

    // ---------------- Param column (left half): speed + speed limit ----------------
    // A 2x1 stack: leftCol is the outer 240x264 container, two equal
    // 240x132 cells stack inside it — applyTheme() only themes specific
    // widgets by name, never leftCol or these cells, so this structure is
    // safe to change freely.
    leftCol = makePane(scr, 0, LS_COL_TOP, LS_PARAM_COL_W, LS_COL_H);
    static const int kCellH = LS_COL_H / 2; // 132
    lv_obj_t *cellSpeed = makePane(leftCol, 0, 0, LS_PARAM_COL_W, kCellH);       // top: actual speed
    lv_obj_t *cellLimit = makePane(leftCol, 0, kCellH, LS_PARAM_COL_W, kCellH);  // bottom: speed limit sign

    // Top — ego speed. y positioned (not simply centered in the cell) so
    // this number's own vertical center lands on the SAME line as
    // speedLimitValueLabel's center in the cell beside it — the sign is
    // centered in its 132px cell (top=(132-88)/2=22, center=22+44=66, per
    // its own diameter), so speedLabel's top is placed at 66 -
    // line_height/2 = 66-52/2 = 40 (font 48's line_height is 52, confirmed
    // from lv_font_montserrat_48.c) to match that same 66px center line.
    // Explicit width + clip: "--" or a 1-3 digit speed, never more, but
    // pinned rather than left to hug its content, per this rewrite's rule.
    speedLabel = lv_label_create(cellSpeed);
    lv_obj_set_style_text_font(speedLabel, &lv_font_montserrat_48, 0);
    lv_obj_set_width(speedLabel, LS_PARAM_COL_W);
    lv_label_set_long_mode(speedLabel, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(speedLabel, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(speedLabel, LV_ALIGN_TOP_MID, 0, 40);

    // Pushed 86->94 (number's own bottom edge moved from 46+40=86 to
    // 40+52=92 when its font grew — this caption must clear that, +2px gap).
    kmhCaption = lv_label_create(cellSpeed);
    lv_label_set_text(kmhCaption, "km/h");
    lv_obj_align(kmhCaption, LV_ALIGN_TOP_MID, 0, 94);

    // Bottom — speed limit sign, 88px diameter, centered in the now
    // full-width 240x132 cell (comfortably clears the sign's own 9px
    // border either way: (240-88)/2 = 76 horizontal, (132-88)/2 = 22
    // vertical).
    //
    // Speed limit for the current road segment — sourced from
    // map/SpeedLimitManager.cpp's microSD map-matching via
    // RoadInfoSnapshot.
    //
    // A real QCVN 41:2019/BGTVT P.127 sign: white disc, thick red ring,
    // bold black number, nothing else printed on it — drawn as plain LVGL
    // vector primitives (a circular obj + a label), not a generated bitmap:
    // there's no photographic detail here for a bitmap to earn its keep on,
    // and a plain lv_obj circle stays trivially resizable.
    static const int kSignDiam = 88;
    speedLimitSign = lv_obj_create(cellLimit);
    lv_obj_set_size(speedLimitSign, kSignDiam, kSignDiam);
    lv_obj_align(speedLimitSign, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_radius(speedLimitSign, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(speedLimitSign, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(speedLimitSign, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(speedLimitSign, lv_color_hex(0xE30613), 0); // standard traffic-sign red
    lv_obj_set_style_border_width(speedLimitSign, 9, 0);
    lv_obj_set_style_pad_all(speedLimitSign, 0, 0);
    lv_obj_clear_flag(speedLimitSign, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(speedLimitSign, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(speedLimitSign, LV_OBJ_FLAG_HIDDEN); // shown only once a real limit is matched — see refreshDashboard()

    // Sibling, not a child of speedLimitSign — needs to keep showing "--"
    // while the sign itself is hidden. align_to centers it on the sign's
    // actual geometry rather than a guessed offset. Explicit width + clip:
    // at most a 3-digit limit ("120") ever renders here.
    speedLimitValueLabel = lv_label_create(cellLimit);
    lv_obj_set_style_text_font(speedLimitValueLabel, &lv_font_montserrat_32, 0);
    lv_obj_set_width(speedLimitValueLabel, kSignDiam - 2 * 9); // clears the sign's own 9px red ring both sides
    lv_label_set_long_mode(speedLimitValueLabel, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(speedLimitValueLabel, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align_to(speedLimitValueLabel, speedLimitSign, LV_ALIGN_CENTER, 0, 0);

    // ---------------- Road column (right half): sign/camera alert card ----------------
    // buildTrafficCard() (shared with buildDashboardPortrait(), see its own
    // definition above) is the column's only content; sized to use most of
    // the vertical room (LS_COL_H is 264 here, the card sits at a fixed
    // y=8, so 250 leaves a clean 6px bottom margin).
    midCol = makePane(scr, LS_ROAD_COL_X, LS_COL_TOP, LS_ROAD_COL_W, LS_COL_H);

    buildTrafficCard(midCol, 224, 250);

    // ---------------- Bottom info bar ----------------
    // Explicit width + LV_LABEL_LONG_CLIP (2026-09-22 rewrite) — this is the
    // exact label a real-hardware report described rendering vertically
    // down the screen edge, character by character. That can only happen if
    // a label is left in LVGL's default content-hug/wrap sizing AND
    // whatever it's measuring its available width against collapses to
    // something tiny; pinning an explicit width and a non-wrapping long
    // mode here makes that entire failure class structurally impossible for
    // this label regardless of what upstream condition might ever cause it.
    bottomInfoLabel = lv_label_create(scr);
    lv_obj_set_width(bottomInfoLabel, LS_SCR_W - 16); // comfortable margin, never touches screen edges
    lv_label_set_long_mode(bottomInfoLabel, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(bottomInfoLabel, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(bottomInfoLabel, LV_ALIGN_BOTTOM_MID, 0, -6);
}

// ---------------------------------------------------------------------
// Portrait content (rotation 0/2, 320x480) — a different ARRANGEMENT of the
// exact same widgets/information as buildDashboardLandscape() above, not a
// parametric reflow of it (see this file's own layout-constants comment for
// why). Stacked top-to-bottom: status bar, then speed+limit-sign side by
// side, then the sign/camera alert card (given the most vertical room —
// used to be a receding radar road/target view here before radar was
// removed 2026-09-21; the alert card fills that same freed space now).
// ---------------------------------------------------------------------
static void buildDashboardPortrait(lv_obj_t *scr) {
    const int scrW = 320, scrH = 480; // both rotation 0 and 2 produce exactly this — see AppConfig.h's screenRotation
    const int pTopH = 30, pBottomH = 26;
    const int pSpeedRowH = 116;
    const int pPad = 8;

    // ---------------- Top status bar ----------------
    // topLeft used to also hold a radar status cluster (removed 2026-09-21
    // alongside radar itself) — GNSS/clock/sun/gear/wifi all still live in
    // topRight unchanged, topLeft is just empty space now.
    lv_obj_t *topRight = makePane(scr, scrW / 2, 0, scrW / 2, pTopH);

    // Narrower than landscape's top-mid zone, so GNSS/clock/sun share the
    // right half instead of their own dedicated middle zone.
    gnssIcon = lv_label_create(topRight);
    lv_label_set_text(gnssIcon, LV_SYMBOL_GPS);
    lv_obj_align(gnssIcon, LV_ALIGN_LEFT_MID, 4, 0);

    gnssCaption = lv_label_create(topRight);
    lv_label_set_text(gnssCaption, "GNSS");
    lv_obj_align_to(gnssCaption, gnssIcon, LV_ALIGN_OUT_RIGHT_MID, 4, 0);

    sunIcon = makeIcon(topRight, &sun_icon);
    lv_obj_align(sunIcon, LV_ALIGN_RIGHT_MID, -26, 0); // leaves room for gearIcon further right

    clockLabel = lv_label_create(topRight);
    lv_obj_align_to(clockLabel, sunIcon, LV_ALIGN_OUT_LEFT_MID, -4, 0);

    gearIcon = lv_label_create(topRight);
    lv_label_set_text(gearIcon, LV_SYMBOL_SETTINGS);
    lv_obj_align(gearIcon, LV_ALIGN_RIGHT_MID, -4, 0);

    wifiTopIcon = lv_label_create(topRight);
    lv_label_set_text(wifiTopIcon, LV_SYMBOL_WIFI);
    lv_obj_set_style_text_color(wifiTopIcon, lv_color_hex(ACCENT_COLOR), 0);
    lv_obj_align_to(wifiTopIcon, gearIcon, LV_ALIGN_OUT_LEFT_MID, -8, 0);
    lv_obj_clear_flag(wifiTopIcon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(wifiTopIcon, LV_OBJ_FLAG_HIDDEN);

    // applyTheme() unconditionally recolors colDividerLine[0]/[1] — portrait
    // has no 3-column layout to divide, so these exist purely as degenerate
    // (zero-length, hidden) placeholders rather than adding a null-check to
    // applyTheme() for landscape's sake.
    static lv_point_precise_t divPts[2][2];
    for (int slot = 0; slot < 2; slot++) {
        lv_obj_t *line = lv_line_create(scr);
        lv_point_precise_t local[2] = {{0, 0}, {0, 0}};
        memcpy(divPts[slot], local, sizeof(local));
        lv_line_set_points(line, divPts[slot], 2);
        lv_obj_add_flag(line, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(line, LV_OBJ_FLAG_CLICKABLE);
        colDividerLine[slot] = line;
    }

    // ---------------- Speed + speed-limit sign row ----------------
    lv_obj_t *speedRow = makePane(scr, 0, pTopH, scrW, pSpeedRowH);

    // Bumped 28->48 (user-requested 2026-09-21, "tang kich thuoc cac so
    // hien thi len nua") — pSpeedRowH is 116px, comfortable room for font
    // 48's 52px line_height starting at the existing top=6 (bottom=58).
    speedLabel = lv_label_create(speedRow);
    lv_obj_set_style_text_font(speedLabel, &lv_font_montserrat_48, 0);
    lv_obj_align(speedLabel, LV_ALIGN_TOP_LEFT, 24, 6);

    // Pushed 44->62 to clear the bigger number above (bottom edge moved
    // from 6+30=36 to 6+52=58, +4px gap).
    kmhCaption = lv_label_create(speedRow);
    lv_label_set_text(kmhCaption, "km/h");
    lv_obj_align(kmhCaption, LV_ALIGN_TOP_LEFT, 24, 62);

    static const int kSignDiam = 92; // bigger than landscape's 76 — portrait has width to spare here
    speedLimitSign = lv_obj_create(speedRow);
    lv_obj_set_size(speedLimitSign, kSignDiam, kSignDiam);
    lv_obj_align(speedLimitSign, LV_ALIGN_TOP_RIGHT, -24, 6);
    lv_obj_set_style_radius(speedLimitSign, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(speedLimitSign, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(speedLimitSign, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(speedLimitSign, lv_color_hex(0xE30613), 0);
    lv_obj_set_style_border_width(speedLimitSign, 9, 0);
    lv_obj_set_style_pad_all(speedLimitSign, 0, 0);
    lv_obj_clear_flag(speedLimitSign, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(speedLimitSign, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(speedLimitSign, LV_OBJ_FLAG_HIDDEN);

    // Bumped 24->32 (user-requested 2026-09-21) — matches landscape's own
    // speedLimitValueLabel choice; this sign is even bigger (92 vs 88
    // diameter) so there's at least as much room.
    speedLimitValueLabel = lv_label_create(speedRow);
    lv_obj_set_style_text_font(speedLimitValueLabel, &lv_font_montserrat_32, 0);
    lv_obj_align_to(speedLimitValueLabel, speedLimitSign, LV_ALIGN_CENTER, 0, 0);

    // ---------------- Sign/camera alert card — the big middle area ----------------
    // Used to hold a radar road/target view (primaryDistLabel, roadArea,
    // per-target icons/labels) plus a separate Warning/TTC strip below it —
    // both removed 2026-09-21 alongside radar itself. buildTrafficCard() now
    // fills this whole reclaimed area instead.
    midCol = makePane(scr, 0, pTopH + pSpeedRowH, scrW, scrH - pTopH - pSpeedRowH - pBottomH);
    lv_obj_update_layout(midCol); // see buildDashboardLandscape()'s own comment on why this commit-now call matters

    buildTrafficCard(midCol, scrW - 2 * pPad, lv_obj_get_height(midCol) - 16);

    // ---------------- Bottom info bar ----------------
    bottomInfoLabel = lv_label_create(scr);
    lv_obj_align(bottomInfoLabel, LV_ALIGN_BOTTOM_MID, 0, -6);
}

void buildDashboard() {
    lastTouchMs = millis(); // burn-in idle timer starts counting from boot

    dashboardScreen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(dashboardScreen, lv_color_hex(0x000000), 0);
    lv_obj_set_style_pad_all(dashboardScreen, 0, 0);
    lv_obj_clear_flag(dashboardScreen, LV_OBJ_FLAG_SCROLLABLE);

    // Root container: holds the whole dashboard so the burn-in pixel shift
    // can nudge everything at once, and so a long press anywhere on it opens
    // Settings (spec section 17 suggests long-press for the settings entry).
    int scrW = gfx->width(), scrH = gfx->height(); // dynamic — LS_SCR_W/LS_SCR_H below are landscape-only constants, wrong for portrait
    dashRoot = lv_obj_create(dashboardScreen);
    lv_obj_set_pos(dashRoot, 0, 0);
    lv_obj_set_size(dashRoot, scrW, scrH);
    lv_obj_set_style_bg_color(dashRoot, lv_color_hex(0x000000), 0);
    lv_obj_set_style_border_width(dashRoot, 0, 0);
    lv_obj_set_style_radius(dashRoot, 0, 0);
    lv_obj_set_style_pad_all(dashRoot, 0, 0);
    lv_obj_clear_flag(dashRoot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(dashRoot, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(dashRoot, onDashPressed, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(dashRoot, onDashLongPressed, LV_EVENT_LONG_PRESSED, NULL);
    lv_obj_add_event_cb(dashRoot, onDashLongPressedRepeat, LV_EVENT_LONG_PRESSED_REPEAT, NULL);
    lv_obj_add_event_cb(dashRoot, onDashReleasedOrLost, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(dashRoot, onDashReleasedOrLost, LV_EVENT_PRESS_LOST, NULL);

    lv_obj_t *scr = dashRoot;

    if (scrH > scrW) {
        buildDashboardPortrait(scr);
    } else {
        buildDashboardLandscape(scr);
    }

    // Full-screen speeding overlay (user-requested 2026-09-16) — translucent,
    // not opaque, so speed/limit/etc stay readable through it. Toggled via
    // the HIDDEN flag on its own blink cadence in refreshDashboard() —
    // gated so the color/visibility only change when they actually need to,
    // never every 150ms tick: a full-screen invalidation is the single most
    // expensive thing this UI can ask the Canvas driver to do (see
    // DisplayDriver.h's full-frame-flush note). Used to share this pattern
    // with a TTC-based warningFlashOverlay (red/amber, imminent-collision
    // risk) — removed 2026-09-21 alongside radar itself, so speeding is now
    // the only full-screen flash this Dashboard has.
    speedingFlashOverlay = lv_obj_create(scr);
    lv_obj_set_pos(speedingFlashOverlay, 0, 0);
    lv_obj_set_size(speedingFlashOverlay, scrW, scrH);
    lv_obj_set_style_border_width(speedingFlashOverlay, 0, 0);
    lv_obj_set_style_radius(speedingFlashOverlay, 0, 0);
    lv_obj_set_style_bg_color(speedingFlashOverlay, lv_color_hex(0xB33DC6), 0);
    lv_obj_set_style_bg_opa(speedingFlashOverlay, LV_OPA_30, 0);
    lv_obj_clear_flag(speedingFlashOverlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(speedingFlashOverlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(speedingFlashOverlay, LV_OBJ_FLAG_HIDDEN);

    // Positioned at the FULL screen's top-center (scr, not the alert card —
    // one shared object, built once here, works unchanged in either
    // orientation). Used to stack below a radar "sudden closing speed"
    // banner (y=28 vs. that banner's y=2) — that banner is gone with radar
    // (2026-09-21), so this now sits at the top spot itself. Text set live
    // in refreshDashboard() (the actual upcoming limit number isn't known
    // until then).
    aheadLimitLabel = lv_label_create(scr);
    lv_obj_set_style_bg_color(aheadLimitLabel, lv_color_hex(0x2F7CE0), 0);
    lv_obj_set_style_bg_opa(aheadLimitLabel, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(aheadLimitLabel, lv_color_white(), 0);
    lv_obj_set_style_pad_hor(aheadLimitLabel, 10, 0);
    lv_obj_set_style_pad_ver(aheadLimitLabel, 4, 0);
    lv_obj_set_style_radius(aheadLimitLabel, 6, 0);
    lv_obj_align(aheadLimitLabel, LV_ALIGN_TOP_MID, 0, 2);
    lv_obj_add_flag(aheadLimitLabel, LV_OBJ_FLAG_HIDDEN);

    // Positioned below aheadLimitLabel above (y=28) — see its own
    // declaration comment for the color/stacking reasoning.
    cameraAheadLabel = lv_label_create(scr);
    lv_obj_set_style_bg_color(cameraAheadLabel, lv_color_hex(0xE0A020), 0);
    lv_obj_set_style_bg_opa(cameraAheadLabel, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(cameraAheadLabel, lv_color_black(), 0);
    lv_obj_set_style_pad_hor(cameraAheadLabel, 10, 0);
    lv_obj_set_style_pad_ver(cameraAheadLabel, 4, 0);
    lv_obj_set_style_radius(cameraAheadLabel, 6, 0);
    lv_obj_align(cameraAheadLabel, LV_ALIGN_TOP_MID, 0, 28);
    lv_obj_add_flag(cameraAheadLabel, LV_OBJ_FLAG_HIDDEN);

    // Traffic sign banner (Khu dan cu, cam vuot, tram thu phi, den tin hieu)
    trafficSignLabel = lv_label_create(scr);
    lv_obj_set_style_bg_color(trafficSignLabel, lv_color_hex(0x2080C0), 0);
    lv_obj_set_style_bg_opa(trafficSignLabel, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(trafficSignLabel, lv_color_white(), 0);
    lv_obj_set_style_pad_hor(trafficSignLabel, 10, 0);
    lv_obj_set_style_pad_ver(trafficSignLabel, 4, 0);
    lv_obj_set_style_radius(trafficSignLabel, 6, 0);
    lv_obj_align(trafficSignLabel, LV_ALIGN_TOP_MID, 0, 54);
    lv_obj_add_flag(trafficSignLabel, LV_OBJ_FLAG_HIDDEN);

    // Hold-to-open-Settings progress ring — created last so it draws on top
    // of everything else, including the flash overlay above. Hidden until
    // a press starts; see onDashPressed().
    holdRing = lv_arc_create(scr);
    lv_obj_set_size(holdRing, kHoldRingDiam, kHoldRingDiam);
    lv_arc_set_bg_angles(holdRing, 0, 360);
    lv_arc_set_rotation(holdRing, 270);
    lv_arc_set_range(holdRing, 0, 100);
    lv_arc_set_value(holdRing, 0);
    lv_obj_remove_style(holdRing, NULL, LV_PART_KNOB);
    lv_obj_set_style_arc_width(holdRing, 5, LV_PART_MAIN);
    lv_obj_set_style_arc_width(holdRing, 5, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(holdRing, lv_color_hex(0x2A3441), LV_PART_MAIN);
    lv_obj_set_style_arc_color(holdRing, lv_color_hex(0x4AA3FF), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(holdRing, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_clear_flag(holdRing, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(holdRing, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(holdRing, LV_OBJ_FLAG_HIDDEN);

    // WiFi on/off confirmation toast (user-requested 2026-09-14 alongside
    // the WiFi hold gesture, now the 4s tier) — the gesture itself has no
    // other on-screen feedback (unlike the 1s Settings-open point, which
    // visibly navigates away, or the 2s Simple-layout point, which visibly
    // swaps the whole layout), so without this a successful WiFi hold would
    // look like nothing happened at all. Self-hiding: shown here, hidden again once
    // wifiToastUntilMs passes, checked each refreshDashboard() tick rather
    // than a dedicated timer/anim — same reasoning burnInTimerCb's own
    // idle-dim check gives for reusing an existing periodic tick instead of
    // adding another one for a rarely-firing check.
    wifiToastLabel = lv_label_create(scr);
    lv_obj_set_style_text_font(wifiToastLabel, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(wifiToastLabel, lv_color_white(), 0);
    lv_obj_set_style_bg_color(wifiToastLabel, lv_color_hex(0x151C24), 0);
    lv_obj_set_style_bg_opa(wifiToastLabel, LV_OPA_90, 0);
    lv_obj_set_style_pad_all(wifiToastLabel, 12, 0);
    lv_obj_set_style_radius(wifiToastLabel, 8, 0);
    lv_obj_center(wifiToastLabel);
    lv_obj_clear_flag(wifiToastLabel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(wifiToastLabel, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(wifiToastLabel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(wifiToastLabel); // above the flash overlay/hold ring too

    applyTheme(true); // initial default matches GnssSnapshot's own daytime=true default, until a real fix says otherwise
}

static void showWifiToast(bool on) {
    lv_label_set_text(wifiToastLabel, on ? "WiFi ON" : "WiFi OFF");
    lv_obj_clear_flag(wifiToastLabel, LV_OBJ_FLAG_HIDDEN);
    wifiToastUntilMs = millis() + 1500;
}

// Day/Night color theme for the chrome — background, captions, primary
// readouts, gear icon, column dividers, bottom info bar (user-requested
// 2026-09-15, driven by the same gnss.daytime sunrise/sunset calc already
// used for the clock icon). The sign/camera alert card (buildTrafficCard())
// stays a consistently dark panel in both themes instead — same reasoning
// most cluster/nav UIs keep their map/camera area dark regardless of day/
// night. Risk/status colors (red/amber/purple, ACCENT_COLOR) also stay
// theme-independent everywhere: changing a safety color's hue between Day
// and Night would undermine the "color only means risk" rule (spec section
// 14.2), not serve it.
static void applyTheme(bool daytime) {
    lv_color_t rootBg, captionText, primaryText, clockText, gearColor, dividerColor, bottomText;
    if (daytime) {
        rootBg = lv_color_hex(0xE9EDF1);
        captionText = lv_color_hex(0x5A6672);
        primaryText = lv_color_hex(0x14181C);
        clockText = lv_color_hex(0x2A323A);
        gearColor = lv_color_hex(0x8A96A2);
        dividerColor = lv_color_hex(0xC7CFD6);
        bottomText = lv_color_hex(0x5A6672);
    } else {
        // Pure black, not the earlier dark navy 0x0B0F14 — user-requested
        // 2026-09-16 ("tang tuoi tho man hinh va giam choi mat, tang tap
        // trung thi su dung mau den cho nen"): less backlight bleed-through
        // at night (less glare, easier to focus on the road), and less
        // sustained non-black pixel drive over the display's lifetime.
        // Daytime keeps its light background above unchanged — that's a
        // deliberate outdoor-sunlight-readability choice, not something this
        // request touches.
        rootBg = lv_color_hex(0x000000);
        captionText = lv_color_hex(0x8899AA);
        primaryText = lv_color_white();
        clockText = lv_color_hex(0xCCD4DC);
        gearColor = lv_color_hex(0x556678);
        dividerColor = lv_color_hex(0x1C2530);
        bottomText = lv_color_hex(0xAAB4C0);
    }

    lv_obj_set_style_bg_color(dashboardScreen, rootBg, 0);
    lv_obj_set_style_bg_color(dashRoot, rootBg, 0);

    lv_obj_set_style_text_color(gnssCaption, captionText, 0);
    lv_obj_set_style_text_color(clockLabel, clockText, 0);
    lv_obj_set_style_text_color(gearIcon, gearColor, 0);
    for (int i = 0; i < 2; i++) lv_obj_set_style_line_color(colDividerLine[i], dividerColor, 0);

    // speedLabel's themed color is remembered (not just applied) because
    // refreshDashboard() overrides it to red on top of this whenever the
    // driver is over the matched speed limit — a theme-independent override
    // on a label that ALSO needs a normal themed color the rest of the time.
    currentPrimaryTextColor = primaryText;
    lv_obj_set_style_text_color(speedLabel, primaryText, 0);
    lv_obj_set_style_text_color(kmhCaption, captionText, 0);
    // speedLimitSign/speedLimitValueLabel are NOT themed here — a real
    // speed-limit sign is white/red/black regardless of day or night (spec
    // section 14.2's "a safety color must mean the same thing in both
    // themes" reasoning extends naturally to "a regulatory sign doesn't
    // recolor itself for you either"). See refreshDashboard().

    lv_obj_set_style_text_color(bottomInfoLabel, bottomText, 0);
}

// GREEN = normal, RED = fault, blinking AMBER = pending/searching — see the
// top-bar build comment. blinkOn flips at ~1Hz (500ms/500ms) off the same
// clock refreshDashboard() already runs on (150ms ticks from simTimerCb),
// no separate lv_anim needed.
#define STATUS_GREEN 0x33CC66
#define STATUS_RED 0xFF3B30
#define STATUS_AMBER 0xE0C020
#define STATUS_AMBER_DIM 0x4A4222
// Distinct hue from every TTC-risk color above (green/amber/red) — see
// speedingFlashOverlay's own declaration comment for why speeding needs its
// own color rather than reusing STATUS_RED.
#define STATUS_SPEEDING 0xB33DC6

// Queues the numbered voice clip for kmh (data/speedmap/sounds/vi/speed/
// <N>.mp3) — ONLY if it exactly matches one of the values actually staged
// there (Task C/E, 2026-09-21). Deliberately does not round/approximate to
// the nearest available number: speaking "80" for an actual 82 km/h limit
// would be misinformation, not a rounding nicety, so an unmatched value
// just gets no spoken number (the tone chime + on-screen text still show
// it exactly).
static void queueSpeedVoice(float kmh) {
    static const int kKnownSpeeds[] = {20, 30, 35, 40, 45, 50, 60, 70, 80, 90, 100, 120};
    int kmhInt = (int)lroundf(kmh);
    for (size_t i = 0; i < sizeof(kKnownSpeeds) / sizeof(kKnownSpeeds[0]); i++) {
        if (kKnownSpeeds[i] == kmhInt) {
            char speedFile[24];
            snprintf(speedFile, sizeof(speedFile), "speed/%d.mp3", kmhInt);
            audioQueueVoice(speedFile);
            return;
        }
    }
}

void refreshDashboard() {
    // Auto-hide the WiFi toggle toast (see showWifiToast()) — checked here
    // rather than a dedicated timer since refreshDashboard() already runs
    // every 150ms while the Dashboard is visible, same reasoning as every
    // other "reuse the existing tick" gate in this function.
    if (!lv_obj_has_flag(wifiToastLabel, LV_OBJ_FLAG_HIDDEN) && millis() > wifiToastUntilMs) {
        lv_obj_add_flag(wifiToastLabel, LV_OBJ_FLAG_HIDDEN);
    }

    // Single mutex-protected read per refresh — everything below uses these
    // local copies, never the live shared state (see core/SharedState.h).
    GnssSnapshot gnss = gnssSnapshot();

    // Auto-wake the instant real movement resumes (not just on touch) — the
    // dim-while-stationary gate above only dims in the first place once
    // parked, so waking symmetrically on the same signal (rather than
    // leaving the driver to tap the screen themselves) is the safety-first
    // completion of that same 2026-09-16 request.
    if (screenDimmed && gnss.fix && gnss.egoSpeedKmh > kGnssMotionThresholdKmh) wakeScreen();

    // --- Top bar: color-only status (no OK/FAULT/SEARCH words) ---
    // GNSS has a real third state: module alive and sending valid NMEA but
    // no fix yet (normal while cold-starting/indoors, not an error) versus
    // nothing received at all (an actual wiring/power/baud problem) — see
    // gnss/GNSS.cpp's linkAlive. Blink amber for the pending case so it
    // reads as "in progress" rather than steady-state.
    bool blinkOn = (millis() / 500) % 2 == 0;
    lv_color_t gnssColor;
    if (gnss.fix) gnssColor = lv_color_hex(STATUS_GREEN);
    else if (gnss.linkAlive) gnssColor = blinkOn ? lv_color_hex(STATUS_AMBER) : lv_color_hex(STATUS_AMBER_DIM);
    else gnssColor = lv_color_hex(STATUS_RED);
    lv_obj_set_style_text_color(gnssIcon, gnssColor, 0); // text itself set once at build — see buildDashboard()

    // WiFi icon — shown only while actually on (net/WebPortal.h). Gated on
    // an actual state CHANGE, not called unconditionally every tick, same
    // discipline as lastDaytime/lastCriticalState below — a HIDDEN-flag
    // toggle is cheap on its own, but there's no reason to touch it 6-7x/s
    // for a value that only ever changes on a user gesture.
    static bool lastWifiOn = false;
    bool wifiOn = webPortalIsEnabled();
    if (wifiOn != lastWifiOn) {
        lastWifiOn = wifiOn;
        if (wifiOn) lv_obj_clear_flag(wifiTopIcon, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(wifiTopIcon, LV_OBJ_FLAG_HIDDEN);
    }

    // Real local time from the GNSS fix (spec section 15.2 wants sunrise/
    // sunset off GNSS date/time/position eventually — this is the time
    // half). GPS gives UTC only; local offset is estimated from longitude
    // (round(lon/15)), which is exact for this deployment (Vietnam, UTC+7)
    // and a reasonable approximation generally without a timezone DB.
    if (gnss.timeValid) {
        int offsetHours = (int)lroundf(gnss.lonDeg / 15.0f);
        int localHour = ((gnss.utcHour + offsetHours) % 24 + 24) % 24;
        char buf[8];
        snprintf(buf, sizeof(buf), "%02d:%02d", localHour, gnss.utcMinute);
        lv_label_set_text(clockLabel, buf);
    } else {
        lv_label_set_text(clockLabel, "--:--");
    }
    // Sun during the day, moon at night — gnss.daytime comes from GNSS.cpp's
    // sunrise/sunset calc off real date/time/lat/lon (spec section 15.2).
    // Defaults to sun (daytime=true) while timeValid is false, so this is
    // safe to call unconditionally before any fix.
    lv_image_set_src(sunIcon, gnss.daytime ? &sun_icon : &moon_icon);

    // Theme mode (user-requested 2026-09-15, Settings > Display): Auto
    // follows the real sunrise/sunset calc below unchanged; Light/Dark
    // override it either way. Re-themes only on an actual CHANGE in the
    // resulting effective value (either gnss.daytime changing under Auto,
    // or cfg.themeMode itself changing) — touching lv_obj_set_style_*
    // unconditionally every 150ms would mean re-invalidating the whole
    // screen background + every themed label on every tick for nothing.
    bool effectiveDaytime = cfg.themeMode == 1.0f    ? true
                             : cfg.themeMode == 2.0f ? false
                                                      : gnss.daytime; // 0 = Auto
    static bool lastDaytime = true;
    static float lastThemeMode = -1; // forces the very first tick to apply, whatever cfg.themeMode loaded as
    if (effectiveDaytime != lastDaytime || cfg.themeMode != lastThemeMode) {
        lastDaytime = effectiveDaytime;
        lastThemeMode = cfg.themeMode;
        applyTheme(effectiveDaytime);
    }

    // --- Left column ---
    // Speed limit read first: whether the driver is currently speeding
    // decides speedLabel's own color just below, so the compliance check
    // needs to happen before speedLabel is touched. map/SpeedLimitManager.cpp
    // only ever sets RoadInfoSnapshot.valid once it has BOTH a loaded map
    // and a current GNSS fix matched against it — checking .valid alone is
    // suffient, no separate gnss.fix check needed for the sign itself.
    RoadInfoSnapshot road = roadInfoSnapshot();
    static const float kSpeedingMarginKmh = 5.0f; // absorbs GPS speed-filter noise right at the boundary
    bool speeding = road.valid && gnss.fix && gnss.egoSpeedKmh > road.speedLimitKmh + kSpeedingMarginKmh;

    if (gnss.fix) {
        // NOTE: LVGL's builtin vsnprintf has %f support compiled out when
        // LV_USE_FLOAT=0 (our lv_conf.h) — passing %f to lv_label_set_text_fmt
        // silently corrupts the varargs and crashes (LoadProhibited).
        // Confirmed on real hardware 2026-09-14. Format floats with the real
        // libc snprintf into a buffer instead, then set the plain string.
        char buf[16];
        snprintf(buf, sizeof(buf), "%.0f", (double)gnss.egoSpeedKmh);
        lv_label_set_text(speedLabel, buf);
    } else {
        lv_label_set_text(speedLabel, "--");
    }
    // Overspeed is the "risk engine" side of map matching, not just a
    // passive sign readout: the EGO speed itself turns red once over the
    // matched segment's limit (a real sign has fixed colors — see its own
    // build comment — so the warning has to live somewhere else). This is
    // the direct replacement for what used to be the sign's own compliance
    // color before the sign became an authentic, fixed-color regulatory
    // sign.
    lv_obj_set_style_text_color(speedLabel, speeding ? lv_color_hex(STATUS_RED) : currentPrimaryTextColor, 0);

    // Full-screen speeding overlay (user-requested 2026-09-16, "neu vuot
    // qua toc do toi da thi cung canh bao bang layer mau toan man hinh",
    // then "qua toc do cung nhap nhay de tang su chu y"). Blinks at 400ms —
    // its own distinct rate/color (0xB33DC6) so it's never mistaken for a
    // routine sign/camera banner. Same "only touch the flag on an actual
    // blink-phase flip" gating as every other flash/banner in this
    // function, not unconditionally every 150ms tick.
    {
        static bool lastSpeedingVisible = false;
        bool speedingFlashOn = speeding && (millis() / 400) % 2 == 0;
        if (speedingFlashOn != lastSpeedingVisible) {
            lastSpeedingVisible = speedingFlashOn;
            if (speedingFlashOn) lv_obj_clear_flag(speedingFlashOverlay, LV_OBJ_FLAG_HIDDEN);
            else lv_obj_add_flag(speedingFlashOverlay, LV_OBJ_FLAG_HIDDEN);
        }
    }

    // Speed-limit sign: hidden entirely (not just showing "--" inside a
    // ring) when there's no valid match — a real sign has no "unknown"
    // state to render. Re-centers the value label on every update (not
    // just once at build time) since "60" and "100" aren't the same width
    // and a stale center would look off — align_to recomputes from the
    // sign's actual geometry, same "no guessed offsets" rule this file
    // uses everywhere else.
    if (road.valid) {
        char buf[8];
        snprintf(buf, sizeof(buf), "%.0f", (double)road.speedLimitKmh);
        lv_label_set_text(speedLimitValueLabel, buf);
        lv_obj_set_style_text_color(speedLimitValueLabel, lv_color_black(), 0);
        lv_obj_clear_flag(speedLimitSign, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_label_set_text(speedLimitValueLabel, "--");
        lv_obj_set_style_text_color(speedLimitValueLabel, lv_color_hex(0x7C8A9A), 0);
        lv_obj_add_flag(speedLimitSign, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_align_to(speedLimitValueLabel, speedLimitSign, LV_ALIGN_CENTER, 0, 0);

    // Upcoming speed-limit-change banner (see aheadLimitLabel's own
    // declaration comment) — gated on an actual state CHANGE (visibility
    // OR the number itself), same discipline as every other banner/flash
    // in this function, not just set unconditionally every 150ms tick.
    static bool lastAheadVisible = false;
    static float lastAheadLimit = -1;
    if (road.aheadLimitValid != lastAheadVisible || road.aheadSpeedLimitKmh != lastAheadLimit) {
        bool newlyVisible = (road.aheadLimitValid && !lastAheadVisible);
        lastAheadVisible = road.aheadLimitValid;
        lastAheadLimit = road.aheadSpeedLimitKmh;
        if (road.aheadLimitValid) {
            char buf[32];
            snprintf(buf, sizeof(buf), "Ahead: %.0f km/h in %.0fm", (double)road.aheadSpeedLimitKmh,
                     (double)road.aheadDistanceM);
            lv_label_set_text(aheadLimitLabel, buf);
            lv_obj_align(aheadLimitLabel, LV_ALIGN_TOP_MID, 0, 2); // re-center: text width just changed
            lv_obj_clear_flag(aheadLimitLabel, LV_OBJ_FLAG_HIDDEN);
            // No existing tone call for this banner before Task E (it was
            // visual-only) — audioPlaySignNotice()'s gentle chime is reused
            // here rather than inventing a third tone shape, same "cheap,
            // instant" role it already plays for the sign banner below.
            if (newlyVisible) {
                audioPlaySignNotice();
                audioQueueVoice("tocdogioihan.mp3");
                queueSpeedVoice(road.aheadSpeedLimitKmh);
            }
        } else {
            lv_obj_add_flag(aheadLimitLabel, LV_OBJ_FLAG_HIDDEN);
        }
    }

    // Speed-camera-ahead banner with audio chime trigger
    static bool lastCameraVisible = false;
    static float lastCameraDist = -1;
    if (road.cameraAheadValid != lastCameraVisible || fabsf(road.cameraAheadDistanceM - lastCameraDist) > 5.0f) {
        bool newlyVisible = (road.cameraAheadValid && !lastCameraVisible);
        lastCameraVisible = road.cameraAheadValid;
        lastCameraDist = road.cameraAheadDistanceM;
        if (road.cameraAheadValid) {
            char buf[40];
            if (road.cameraSpeedLimitKmh >= 0) {
                snprintf(buf, sizeof(buf), "Camera in %.0fm (%.0f km/h)", (double)road.cameraAheadDistanceM,
                          (double)road.cameraSpeedLimitKmh);
            } else {
                snprintf(buf, sizeof(buf), "Camera in %.0fm", (double)road.cameraAheadDistanceM);
            }
            lv_label_set_text(cameraAheadLabel, buf);
            lv_obj_align(cameraAheadLabel, LV_ALIGN_TOP_MID, 0, 28);
            lv_obj_clear_flag(cameraAheadLabel, LV_OBJ_FLAG_HIDDEN);
            if (newlyVisible) {
                audioPlayCameraAlert(); // immediate tone chime — cheap, instant, plays while the voice line below queues
                audioQueueVoice("speedcamera.mp3");
                if (road.cameraSpeedLimitKmh >= 0) queueSpeedVoice(road.cameraSpeedLimitKmh);
            }
        } else {
            lv_obj_add_flag(cameraAheadLabel, LV_OBJ_FLAG_HIDDEN);
        }
    }

    // Traffic sign banner (Khu dan cu, Cam vuot, Tram thu phi, Den tin hieu)
    static bool lastSignVisible = false;
    static int lastSignType = 0;
    static float lastSignDist = -1;

    bool signVisible = road.residentAreaAheadValid || road.noOvertakingAheadValid ||
                       road.tollBoothAheadValid || road.trafficLightAheadValid;

    int currentSignType = 0;
    float currentSignDist = -1;
    char signBuf[48] = "";

    if (road.residentAreaAheadValid) {
        currentSignType = 2;
        currentSignDist = road.residentAreaAheadDistM;
        snprintf(signBuf, sizeof(signBuf), "%s in %.0fm",
                 road.residentAreaIsStart ? "Khu dan cu" : "Het khu dan cu", (double)currentSignDist);
    } else if (road.noOvertakingAheadValid) {
        currentSignType = 3;
        currentSignDist = road.noOvertakingAheadDistM;
        snprintf(signBuf, sizeof(signBuf), "%s in %.0fm",
                 road.noOvertakingIsStart ? "Cam vuot" : "Het cam vuot", (double)currentSignDist);
    } else if (road.tollBoothAheadValid) {
        currentSignType = 5;
        currentSignDist = road.tollBoothAheadDistM;
        snprintf(signBuf, sizeof(signBuf), "Tram thu phi in %.0fm", (double)currentSignDist);
    } else if (road.trafficLightAheadValid) {
        currentSignType = 6;
        currentSignDist = road.trafficLightAheadDistM;
        snprintf(signBuf, sizeof(signBuf), "Den tin hieu in %.0fm", (double)currentSignDist);
    }

    if (signVisible != lastSignVisible || currentSignType != lastSignType ||
        fabsf(currentSignDist - lastSignDist) > 5.0f) {
        bool newlyVisible = (signVisible && !lastSignVisible);
        lastSignVisible = signVisible;
        lastSignType = currentSignType;
        lastSignDist = currentSignDist;

        if (signVisible) {
            lv_label_set_text(trafficSignLabel, signBuf);
            if (currentSignType == 2) {
                lv_obj_set_style_bg_color(trafficSignLabel, lv_color_hex(0x2080C0), 0); // Cyan/blue
            } else if (currentSignType == 3) {
                lv_obj_set_style_bg_color(trafficSignLabel, lv_color_hex(0xD04020), 0); // Red/Orange
            } else if (currentSignType == 5) {
                lv_obj_set_style_bg_color(trafficSignLabel, lv_color_hex(0x7050B0), 0); // Purple
            } else {
                lv_obj_set_style_bg_color(trafficSignLabel, lv_color_hex(0x209060), 0); // Green
            }
            lv_obj_align(trafficSignLabel, LV_ALIGN_TOP_MID, 0, 54);
            lv_obj_clear_flag(trafficSignLabel, LV_OBJ_FLAG_HIDDEN);
            if (newlyVisible) {
                audioPlaySignNotice(); // immediate tone chime — cheap, instant, plays while the voice line below queues
                // Only resident-area/no-overtaking/toll/traffic-light have a
                // matching voice asset in data/speedmap/sounds/vi/ (Task C) —
                // no fallback fabricated for anything else; see AudioPlayer.h.
                switch (currentSignType) {
                    case 2: audioQueueVoice(road.residentAreaIsStart ? "batdaukhudancu.mp3" : "hetkhudongdancu.mp3"); break;
                    case 3: audioQueueVoice(road.noOvertakingIsStart ? "camvuot.mp3" : "hetcamvuot.mp3"); break;
                    case 5: audioQueueVoice("tramthuphi.mp3"); break;
                    case 6: audioQueueVoice("chuydentinhieugiaothong.mp3"); break;
                    default: break;
                }
            }
        } else {
            lv_obj_add_flag(trafficSignLabel, LV_OBJ_FLAG_HIDDEN);
        }
    }

    // --- Update Traffic & Camera Alert Card (HUD) ---
    if (trafficCard) {
        if (road.cameraAheadValid) {
            // Priority 1: Speed & Enforcement Camera
            lv_label_set_text(alertBadgeLabel, "CAMERA PHAT NGUOI");
            lv_obj_set_style_bg_color(alertBadgeLabel, lv_color_hex(0xE0A020), 0); // Amber
            lv_obj_set_style_text_color(alertBadgeLabel, lv_color_black(), 0);

            char dBuf[16];
            snprintf(dBuf, sizeof(dBuf), "%.0f", (double)road.cameraAheadDistanceM);
            lv_label_set_text(alertDistLabel, dBuf);
            lv_label_set_text(alertUnitLabel, "m");

            char sBuf[32];
            if (road.cameraSpeedLimitKmh >= 0) {
                snprintf(sBuf, sizeof(sBuf), "Gioi han: %.0f km/h", (double)road.cameraSpeedLimitKmh);
            } else {
                snprintf(sBuf, sizeof(sBuf), "Camera giam sat");
            }
            lv_label_set_text(alertSubLabel, sBuf);

            int prog = 350 - (int)road.cameraAheadDistanceM;
            if (prog < 0) prog = 0;
            if (prog > 350) prog = 350;
            lv_bar_set_value(alertProgressBar, prog, LV_ANIM_OFF);
            lv_color_t pColor = (road.cameraAheadDistanceM < 100.0f) ? lv_color_hex(0xFF3B30) : lv_color_hex(0xE0A020);
            lv_obj_set_style_bg_color(alertProgressBar, pColor, LV_PART_INDICATOR);
            lv_obj_set_style_border_color(trafficCard, pColor, 0);
        } else if (road.residentAreaAheadValid) {
            // Priority 2: Resident Area (Khu dong dan cu)
            lv_label_set_text(alertBadgeLabel, road.residentAreaIsStart ? "KHU DONG DAN CU" : "HET KHU DAN CU");
            lv_obj_set_style_bg_color(alertBadgeLabel, lv_color_hex(0x2080C0), 0); // Blue
            lv_obj_set_style_text_color(alertBadgeLabel, lv_color_white(), 0);

            char dBuf[16];
            snprintf(dBuf, sizeof(dBuf), "%.0f", (double)road.residentAreaAheadDistM);
            lv_label_set_text(alertDistLabel, dBuf);
            lv_label_set_text(alertUnitLabel, "m");
            lv_label_set_text(alertSubLabel, "Toi da 50-60 km/h");

            int prog = 350 - (int)road.residentAreaAheadDistM;
            if (prog < 0) prog = 0;
            lv_bar_set_value(alertProgressBar, prog, LV_ANIM_OFF);
            lv_obj_set_style_bg_color(alertProgressBar, lv_color_hex(0x2080C0), LV_PART_INDICATOR);
            lv_obj_set_style_border_color(trafficCard, lv_color_hex(0x2080C0), 0);
        } else if (road.noOvertakingAheadValid) {
            // Priority 3: No Overtaking
            lv_label_set_text(alertBadgeLabel, road.noOvertakingIsStart ? "DOAN DUONG CAM VUOT" : "HET CAM VUOT");
            lv_obj_set_style_bg_color(alertBadgeLabel, lv_color_hex(0xD04020), 0); // Red
            lv_obj_set_style_text_color(alertBadgeLabel, lv_color_white(), 0);

            char dBuf[16];
            snprintf(dBuf, sizeof(dBuf), "%.0f", (double)road.noOvertakingAheadDistM);
            lv_label_set_text(alertDistLabel, dBuf);
            lv_label_set_text(alertUnitLabel, "m");
            lv_label_set_text(alertSubLabel, "Chu y vach ke duong");

            int prog = 350 - (int)road.noOvertakingAheadDistM;
            if (prog < 0) prog = 0;
            lv_bar_set_value(alertProgressBar, prog, LV_ANIM_OFF);
            lv_obj_set_style_bg_color(alertProgressBar, lv_color_hex(0xD04020), LV_PART_INDICATOR);
            lv_obj_set_style_border_color(trafficCard, lv_color_hex(0xD04020), 0);
        } else if (road.tollBoothAheadValid) {
            // Priority 4: Toll Booth
            lv_label_set_text(alertBadgeLabel, "TRAM THU PHI (BOT)");
            lv_obj_set_style_bg_color(alertBadgeLabel, lv_color_hex(0x7050B0), 0); // Purple
            lv_obj_set_style_text_color(alertBadgeLabel, lv_color_white(), 0);

            char dBuf[16];
            snprintf(dBuf, sizeof(dBuf), "%.0f", (double)road.tollBoothAheadDistM);
            lv_label_set_text(alertDistLabel, dBuf);
            lv_label_set_text(alertUnitLabel, "m");
            lv_label_set_text(alertSubLabel, "Chuan bi phi duong bo");

            int prog = 350 - (int)road.tollBoothAheadDistM;
            if (prog < 0) prog = 0;
            lv_bar_set_value(alertProgressBar, prog, LV_ANIM_OFF);
            lv_obj_set_style_bg_color(alertProgressBar, lv_color_hex(0x7050B0), LV_PART_INDICATOR);
            lv_obj_set_style_border_color(trafficCard, lv_color_hex(0x7050B0), 0);
        } else if (road.trafficLightAheadValid) {
            // Priority 5: Traffic Light
            lv_label_set_text(alertBadgeLabel, "DEN TIN HIEU GIAO THONG");
            lv_obj_set_style_bg_color(alertBadgeLabel, lv_color_hex(0x209060), 0); // Green
            lv_obj_set_style_text_color(alertBadgeLabel, lv_color_white(), 0);

            char dBuf[16];
            snprintf(dBuf, sizeof(dBuf), "%.0f", (double)road.trafficLightAheadDistM);
            lv_label_set_text(alertDistLabel, dBuf);
            lv_label_set_text(alertUnitLabel, "m");
            lv_label_set_text(alertSubLabel, "Chu y tin hieu ngat");

            int prog = 350 - (int)road.trafficLightAheadDistM;
            if (prog < 0) prog = 0;
            lv_bar_set_value(alertProgressBar, prog, LV_ANIM_OFF);
            lv_obj_set_style_bg_color(alertProgressBar, lv_color_hex(0x209060), LV_PART_INDICATOR);
            lv_obj_set_style_border_color(trafficCard, lv_color_hex(0x209060), 0);
        } else {
            // Default: Clear road ahead
            lv_label_set_text(alertBadgeLabel, "DUONG THONG THOANG");
            lv_obj_set_style_bg_color(alertBadgeLabel, lv_color_hex(0x1B2430), 0);
            lv_obj_set_style_text_color(alertBadgeLabel, lv_color_hex(0x6A829A), 0);

            lv_label_set_text(alertDistLabel, "OK");
            lv_label_set_text(alertUnitLabel, "");

            if (gnss.fix) {
                char sBuf[32];
                snprintf(sBuf, sizeof(sBuf), "GPS Tot (%d ve tinh)", gnss.satCount);
                lv_label_set_text(alertSubLabel, sBuf);
            } else {
                lv_label_set_text(alertSubLabel, "Dang tim ve tinh GPS...");
            }
            lv_bar_set_value(alertProgressBar, 0, LV_ANIM_OFF);
            lv_obj_set_style_border_color(trafficCard, lv_color_hex(0x223040), 0);
        }

        // alertFooterLabel's text is otherwise static (set once at build,
        // see buildTrafficCard()) — it used to be a live "radar target
        // ahead" hint, updated every tick here; radar is gone (2026-09-21)
        // so there's nothing live to report in this card anymore.
    }

    // --- Bottom info bar ---
    // Used to show live target count ("3 lanes | %d targets | AUTO") from
    // the now-removed radar road panel — replaced with the two GPS-only
    // facts worth a permanent glance: whether the offline speed-map
    // database actually loaded, and how many satellites GNSS currently
    // sees. Plain ASCII " | " separators, not a middle-dot: the compiled
    // Montserrat font is missing that glyph and LVGL logs (blocking
    // Serial.printf, with LV_LOG_PRINTF=1) every time it tries to draw one
    // — confirmed on real hardware 2026-09-14.
    char infoBuf[48];
    snprintf(infoBuf, sizeof(infoBuf), "%s | %d sats | VietHUD", road.mapLoaded ? "MAP OK" : "NO MAP", gnss.satCount);
    lv_label_set_text(bottomInfoLabel, infoBuf);
}

// Only refresh while the Dashboard is the screen actually on-screen — this
// used to run unconditionally every 150ms even while Settings was open,
// burning CPU on a screen nobody could see. User-reported 2026-09-15 as
// Settings feeling laggy; this was competing with Settings' own touch
// handling for the same Core 1 loop().
void simTimerCb(lv_timer_t *) {
    if (lv_screen_active() == dashboardScreen) refreshDashboard();
}

// Runs once a minute: nudges the whole dashboard by a couple of pixels so no
// static edge burns in, and applies the idle auto-dim. The pixel shift only
// matters while the Dashboard is visible (nothing to un-burn-in on a hidden
// screen); auto-dim stays screen-agnostic — idle is idle whether the driver
// left it on Dashboard or Settings.
void burnInTimerCb(lv_timer_t *) {
    if (lv_screen_active() == dashboardScreen) {
        pixelShiftIdx = (pixelShiftIdx + 1) % 4;
        lv_obj_set_pos(dashRoot, kPixelShiftOffsets[pixelShiftIdx][0], kPixelShiftOffsets[pixelShiftIdx][1]);
    }

    // Gated on the VEHICLE being stationary (user-requested 2026-09-16,
    // "che do tu giam do sang man hinh chi duoc thuc hien khi xe khong
    // chuyen dong sau 3 phut"), not touch-idle time as before — a driver
    // watching the road without touching the screen shouldn't have it dim
    // out from under them while actually driving. gnssMsSinceStationary()
    // itself resets to ~0 the instant real speed or a lost fix is seen (see
    // GNSS.cpp), so this is safe by construction even through a GPS dropout.
    if (cfg.autoDimMin > 0 && !screenDimmed) {
        uint32_t stationaryMs = gnssMsSinceStationary();
        if (stationaryMs > (uint32_t)(cfg.autoDimMin * 60000.0f)) {
            screenDimmed = true;
            applyConfig();
            Serial.println("[uidemo] vehicle stationary -> screen dimmed (burn-in mitigation)");
        }
    }
}
