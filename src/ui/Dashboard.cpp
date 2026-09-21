#include "Dashboard.h"
#include "Settings.h" // settingsScreen, for the hold-to-open-Settings gesture
#include "core/AppConfig.h"
#include "core/NvsStore.h" // saveConfigToNVS() — double-tap gesture persists cfg.simpleUiMode immediately
#include "core/SafeDistanceRules.h" // legalMinFollowDistanceM()
#include "core/SharedState.h"
#include "display/DisplayDriver.h" // backlightWrite
#include "gnss/GNSS.h" // gnssMsSinceStationary() — auto-dim's vehicle-stationary gate
#include "icons/Icons.h"
#include "net/WebPortal.h"
#include "audio/AudioPlayer.h" // webPortalIsEnabled()/webPortalRequestEnable() — the 4s hold gesture toggles WiFi
#include <math.h>
#include <string.h>

// ---------------------------------------------------------------------
// Redesigned 2026-09-14 (second pass) into the 3-column layout requested
// directly: [ speed + audio | road + targets | warning + TTC ], with a top
// status bar (RADAR/GNSS/clock/settings) and a one-line bottom info bar
// (lane count / target count / mode). Supersedes the earlier single-panel
// "ADAS-cluster proposal" layout — see git history for that version.
//
// Two decisions carried over unchanged from that discussion, both because
// the compiled-in LVGL font can't render what the mockup asked for
// (verified directly against lv_font_montserrat_14.c's cmap — ASCII +
// ~60 symbol codepoints only, no emoji, no Vietnamese diacritics):
//   - All labels stay English/ASCII (no custom Vietnamese font asset).
//   - Car/warning/sun icons are small generated bitmaps (see
//     ui/icons/Icons.h for how and why) instead of font glyphs or the
//     rectangle silhouettes the previous layout used.
// GPS/Radar/Audio/Settings telltales keep using the built-in LV_SYMBOL_*
// glyphs (LV_SYMBOL_GPS/AUDIO/MUTE/SETTINGS) — those aren't emoji, they're
// already part of the compiled font, exactly like before.
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

// Semantic risk color — the ONLY place red/amber/green are used to mean
// "target danger level". System telltales (GPS/Radar/Audio/ego car) use the
// cyan accent below instead, precisely so a "system OK" color is never
// confused with a "target is safe" color (spec section 14.2).
#define ACCENT_COLOR 0x3FCAD6

// applyTheme()'s last-computed "normal" text color for speedLabel —
// remembered so refreshDashboard() can restore it after an overspeed
// tick's red override without needing to know which theme is active itself.
static lv_color_t currentPrimaryTextColor = lv_color_white();

static lv_color_t riskColor(float ttcS) {
    if (ttcS > 5.0f) return lv_color_hex(0x33CC66);         // GREEN safe
    if (ttcS > cfg.ttcWarnS) return lv_color_hex(0xE0C020); // YELLOW caution
    if (ttcS > cfg.ttcCritS) return lv_color_hex(0xFF8800); // ORANGE warning
    return lv_color_hex(0xFF3333);                          // RED critical
}

static const char *riskLabel(float ttcS) {
    if (ttcS > 5.0f) return "SAFE";
    if (ttcS > cfg.ttcWarnS) return "CAUTION";
    if (ttcS > cfg.ttcCritS) return "WARNING";
    return "CRITICAL";
}

// legalMinFollowDistanceM() itself now lives in core/SafeDistanceRules.h
// (feature-requested 2026-09-21, "cac gia tri tren phai nam trong
// configuration, khong hard-code rai rac trong UI hoac Risk Engine") —
// this file is a pure consumer, no longer the one that encodes the table.

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
static const int SCR_W = 480, SCR_H = 320;
static const int TOP_H = 30, BOTTOM_H = 26;
// Split 50/50 (user-requested 2026-09-16, "chia doi man hinh... phan hien
// thi xe sang 1 ben, nua man hinh con lai la cac thong so") — replaces the
// old 96|288|96 three-column split. PARAM_COL (left half) holds every
// number/status readout (speed, speed limit sign, warning, TTC) stacked in
// ONE evenly-spaced column (retuned 2026-09-16, "bo tri hop ly, can bang va
// deu nhau" — an earlier two-sub-column attempt left mismatched gaps, see
// buildDashboardLandscape()'s own comment at that block); ROAD_COL (right
// half) is just the road/target view, now much bigger than the old 288px-
// wide middle column.
static const int PARAM_COL_W = 240, ROAD_COL_W = SCR_W - PARAM_COL_W;
static const int ROAD_COL_X = PARAM_COL_W;
static const int COL_TOP = TOP_H, COL_H = SCR_H - TOP_H - BOTTOM_H;

// --- Top bar ---
static lv_obj_t *radarRing[3], *radarCenterDot, *radarCaption;
static lv_obj_t *gnssIcon, *gnssCaption;
static lv_obj_t *sunIcon, *clockLabel;
static lv_obj_t *gearIcon;
static lv_obj_t *wifiTopIcon; // shown only while WiFi is on — see refreshDashboard()
static lv_obj_t *colDividerLine[2];

// --- Left column: speed + speed limit ---
// Audio icon/state label removed 2026-09-15 (user-requested) — the
// underlying audio-gate LOGIC (cfg.audioEnabled, radar.audioAllowed —
// LD2451.cpp/AppConfig.h/Settings.cpp's Safety tab) is untouched, this was
// only ever the on-screen telltale for it, same as there being no buzzer
// output yet either (spec: Phase 7).
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

// --- Middle column: distance readout + road + targets ---
static lv_obj_t *midCol;
static lv_obj_t *primaryDistLabel;
// Legal minimum following-distance caption (user-requested 2026-09-21,
// "hien thi khoang cach giua 2 xe cho phep theo luat giao thong dua tren
// toc do di chuyen") — only used in Simple layout, where roadArea is
// hidden and there's real open space below the big distance number; in
// Full layout the same info is appended inline into primaryDistLabel's own
// string instead (see legalMinFollowDistanceM()'s call site) since there's
// no spare vertical room there (roadArea starts just a few px below it).
static lv_obj_t *legalDistCaption;
static lv_obj_t *roadArea;
static lv_obj_t *targetIcon[MAX_TARGETS];
static lv_obj_t *targetLabels[MAX_TARGETS];
static lv_obj_t *primaryRing; // single shared ring, repositioned onto whichever target is primary
static lv_obj_t *faultLabel;
// Sudden-closing-speed ("harsh braking ahead") banner, added 2026-09-16 —
// deliberately its own object, not folded into warningFlashOverlay below:
// that overlay is reserved for TTC-based imminent-collision risk only (see
// applyTheme()'s "one color = one specific risk" comment on speedLabel) and
// this is a materially different signal (a sudden closing-rate spike, which
// can fire even while TTC itself is still SAFE/CAUTION). One shared object
// works for both orientations since it's created once in buildDashboard()
// itself, not per-layout — see that function.
static lv_obj_t *harshBrakeLabel;
// Upcoming speed-limit-change banner (user-requested 2026-09-21, "canh bao
// gioi han toc do doan duong tiep theo, bao truoc khoang 100m") — same
// shared-object/one-shared-widget-for-both-orientations pattern as
// harshBrakeLabel above. Own dedicated color (blue, a common real-world
// "informational" road-sign color), deliberately NOT red/amber, so it
// never reads as a safety alert the way the TTC/speeding overlays do —
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
static lv_obj_t *alertRadarHint = nullptr;

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

    // Badge pill at top
    alertBadgeLabel = lv_label_create(trafficCard);
    lv_obj_set_style_bg_color(alertBadgeLabel, lv_color_hex(0x202A36), 0);
    lv_obj_set_style_bg_opa(alertBadgeLabel, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(alertBadgeLabel, lv_color_hex(0x88A0B8), 0);
    lv_obj_set_style_text_font(alertBadgeLabel, &lv_font_montserrat_14, 0);
    lv_obj_set_style_pad_hor(alertBadgeLabel, 10, 0);
    lv_obj_set_style_pad_ver(alertBadgeLabel, 3, 0);
    lv_obj_set_style_radius(alertBadgeLabel, 12, 0);
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

    // Subtitle / limit speed
    alertSubLabel = lv_label_create(trafficCard);
    lv_obj_set_style_text_font(alertSubLabel, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(alertSubLabel, lv_color_hex(0xCCD4DC), 0);
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

    // Radar target hint at very bottom
    alertRadarHint = lv_label_create(trafficCard);
    lv_obj_set_style_text_font(alertRadarHint, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(alertRadarHint, lv_color_hex(0x607890), 0);
    lv_obj_align(alertRadarHint, LV_ALIGN_BOTTOM_MID, 0, -6);
    lv_label_set_text(alertRadarHint, "Radar: San sang");
}

// --- Right column: warning + TTC ---
static lv_obj_t *rightCol;
static lv_obj_t *warningIconObj;
static lv_obj_t *riskWordLabel;
static lv_obj_t *ttcValueLabel, *ttcCaption;

// --- Bottom info bar ---
static lv_obj_t *bottomInfoLabel;

// --- Full-screen WARNING/CRITICAL flash overlay ---
static lv_obj_t *warningFlashOverlay;
// Full-screen speeding overlay (user-requested 2026-09-16, "neu vuot qua
// toc do toi da thi cung canh bao bang layer mau toan man hinh", then "qua
// toc do cung nhap nhay de tang su chu y") — deliberately its own object/
// color, not folded into warningFlashOverlay above, for the exact same
// "one color = one specific risk" reason harshBrakeLabel's own comment
// gives: warningFlashOverlay's red/amber already mean "imminent collision"
// (TTC-based); reusing either color here would make "you're speeding" read
// as "you're about to hit something". Blinks (own rate, distinct from both
// TTC tiers) — see refreshDashboard()'s own comment on this overlay.
static lv_obj_t *speedingFlashOverlay;

// --- WiFi on/off gesture toast (see onDashLongPressedRepeat()/showWifiToast() below) ---
static lv_obj_t *wifiToastLabel;
static uint32_t wifiToastUntilMs = 0;
static void showWifiToast(bool on); // defined below buildDashboard(), called from onDashLongPressedRepeat() above it

// Landscape-only road-panel geometry, inside midCol's own coordinate space
// (portrait's road panel is positioned/sized separately in
// buildDashboardPortrait(), which doesn't use these).
static const int ROAD_X = 8, ROAD_Y = 32; // a couple px below primaryDistLabel, avoids touching it
static const int ROAD_W = ROAD_COL_W - 2 * ROAD_X;  // 224 (was 272 when the road column was 288 wide)
static const int ROAD_H = COL_H - ROAD_Y - 6;       // small bottom margin only — no ego-car caption anymore

// Reads roadArea's ACTUAL current size rather than a fixed constant — makes
// this callable unchanged from both buildDashboardLandscape() (which sizes
// roadArea to ROAD_W/ROAD_H above) and buildDashboardPortrait() (which sizes
// it differently for a tall narrow screen), and keeps refreshDashboard()'s
// own target-placement math (which reads the same real size) automatically
// consistent with whatever this function actually drew.
static void buildRoadLanes() {
    int roadW = lv_obj_get_width(roadArea), roadH = lv_obj_get_height(roadArea);
    int centerX = roadW / 2;
    int topHalf = (int)(roadW * 0.16f); // road is narrow near the top (far away)
    int botHalf = roadW / 2;            // and fills the full width at the bottom (near)
    int top = 6, bottom = roadH - 6;

    // NOTE: lv_line_set_points() stores a POINTER into whatever buffer you
    // pass — it does not copy — so each line needs its OWN stable storage.
    // Two edges used to share one buffer here; the second memcpy silently
    // overwrote the first line's points too, so both edges rendered the
    // same line on top of each other. Confirmed on real hardware
    // 2026-09-14. Fixed by giving each line its own array slot, never
    // reused across different line objects — applied below to every line
    // this function draws (edges, dashed dividers, distance gridlines).
    static lv_point_precise_t edgePts[2][2];
    auto edgeLine = [&](int slot, int topX, int botX) {
        lv_obj_t *line = lv_line_create(roadArea);
        lv_obj_set_style_line_color(line, lv_color_hex(0x3A4A5C), 0);
        lv_obj_set_style_line_width(line, 2, 0);
        lv_obj_set_style_line_rounded(line, true, 0);
        lv_point_precise_t localPts[2] = {{(lv_value_precise_t)topX, (lv_value_precise_t)top},
                                           {(lv_value_precise_t)botX, (lv_value_precise_t)bottom}};
        memcpy(edgePts[slot], localPts, sizeof(localPts));
        lv_line_set_points(line, edgePts[slot], 2);
        lv_obj_clear_flag(line, LV_OBJ_FLAG_CLICKABLE);
    };
    edgeLine(0, centerX - topHalf, centerX - botHalf);
    edgeLine(1, centerX + topHalf, centerX + botHalf);

    // Two dashed lane dividers, symmetric about centerX, marking the edge
    // of MY lane (spec-style ego corridor) — not 3 equal lanes anymore.
    // User-requested 2026-09-15: the ego lane should read as the dominant
    // lane, with the two side lanes only partially visible at the edges
    // (so a car sitting in/entering one still shows up, without implying
    // it's sharing equal billing with the lane you're actually in). Fixed
    // at kMainLaneFrac of the full road width regardless of lane count —
    // there's no real lane-corridor model yet (Phase 5), same reasoning
    // AppConfig.h gives for not having a real laneWidth setting.
    static const float kMainLaneFrac = 0.6f; // ego lane share of total width; each side sliver gets (1-0.6)/2 = 20%
    int dashCount = 5;
    int span = bottom - top;
    static lv_point_precise_t dashPts[2][5][2];
    auto dashedDivider = [&](int dividerSlot, float sideSign) {
        for (int i = 0; i < dashCount; i++) {
            int y0 = top + (span * i) / dashCount + 4;
            int y1 = top + (span * (i + 1)) / dashCount - 8;
            if (y1 <= y0) continue;
            float t0 = (float)(y0 - top) / (float)span, t1 = (float)(y1 - top) / (float)span;
            int x0 = centerX + (int)(sideSign * (topHalf + (botHalf - topHalf) * t0) * kMainLaneFrac);
            int x1 = centerX + (int)(sideSign * (topHalf + (botHalf - topHalf) * t1) * kMainLaneFrac);

            lv_obj_t *dash = lv_line_create(roadArea);
            lv_obj_set_style_line_color(dash, lv_color_hex(0x556678), 0);
            lv_obj_set_style_line_width(dash, 2, 0);
            lv_point_precise_t local[2] = {{(lv_value_precise_t)x0, (lv_value_precise_t)y0},
                                            {(lv_value_precise_t)x1, (lv_value_precise_t)y1}};
            memcpy(dashPts[dividerSlot][i], local, sizeof(local));
            lv_line_set_points(dash, dashPts[dividerSlot][i], 2);
            lv_obj_clear_flag(dash, LV_OBJ_FLAG_CLICKABLE);
        }
    };
    dashedDivider(0, -1.0f);
    dashedDivider(1, 1.0f);

    // Distance gridlines at 25/50/75% of the current max range (spec
    // section 14.1's road view didn't have any distance reference at all —
    // user-requested 2026-09-15, "chia lane duong theo cac muc khoang cach
    // toi da la 100m"). Fractions, not fixed meter values: since a
    // gridline's screen position only depends on the FRACTION of max range
    // (normDist, same as target placement below), not on the max-range
    // value itself, these never need to move even when cfg.maxRangeM
    // changes in Settings — only what real-world distance they'd label
    // would change, and there's no on-screen label to update. 100% would
    // sit right on the panel's own top border, so it's skipped as
    // redundant.
    static lv_point_precise_t gridPts[3][2];
    static const float kGridFractions[3] = {0.25f, 0.5f, 0.75f};
    for (int i = 0; i < 3; i++) {
        // Mirrors the target screenY formula in refreshDashboard() so a
        // target at exactly this fraction of max range sits on the line.
        int y = roadH - (int)(kGridFractions[i] * (roadH - 24)) - 12;
        if (y < top) y = top;
        if (y > bottom) y = bottom;
        float t = (float)(y - top) / (float)span;
        int halfW = (int)(topHalf + (botHalf - topHalf) * t);

        lv_obj_t *grid = lv_line_create(roadArea);
        lv_obj_set_style_line_color(grid, lv_color_hex(0x223040), 0);
        lv_obj_set_style_line_width(grid, 1, 0);
        lv_point_precise_t local[2] = {{(lv_value_precise_t)(centerX - halfW), (lv_value_precise_t)y},
                                        {(lv_value_precise_t)(centerX + halfW), (lv_value_precise_t)y}};
        memcpy(gridPts[i], local, sizeof(local));
        lv_line_set_points(grid, gridPts[i], 2);
        lv_obj_clear_flag(grid, LV_OBJ_FLAG_CLICKABLE);
    }
}

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

// Three gestures share the same press-and-hold on dashRoot, distinguished by
// duration: ~1s (HOLD_PRESS_MS, the existing ring animation's own duration,
// user-requested 2026-09-14) opens Settings; holding to 2s+ instead toggles
// Simple layout (user-requested 2026-09-21, "bam giu de chuyen sang che do
// simple va nguoc lai"); holding all the way to 4s+ toggles WiFi. Each
// longer tier suppresses the shorter one(s) that would otherwise fire on
// release — a 4s WiFi hold doesn't ALSO open Settings, and a 2s
// Simple-layout hold doesn't either. LVGL's indev only exposes ONE
// long-press threshold directly (lv_indev_set_long_press_time(), already
// used for the 1s point) — the 2s/4s points are measured by hand from
// pressStartMs, sampled on LV_EVENT_LONG_PRESSED_REPEAT (which LVGL fires
// periodically for as long as the press continues past the 1s mark).
static uint32_t pressStartMs = 0;
static bool longPressFired = false;    // past the 1s mark at least
static bool simpleModeToggledThisPress = false; // past the 2s mark — latched so it can't fire twice for one press
static bool wifiToggledThisPress = false; // past the 4s mark — latched so it can't fire twice for one press
static const uint32_t kSimpleModeHoldMs = 2000;
static const uint32_t kWifiHoldMs = 4000; // bumped from 3000 (2026-09-21) to make room for the new 2s Simple-layout tier in between

// Double-tap gesture (user-requested 2026-09-21, "double click vao man
// hinh de chuyen giua che do don gian va che do binh thuong") — toggles
// cfg.simpleUiMode the same way the 2s hold above does; kept as a second,
// alternative trigger for the same action rather than replacing the hold
// gesture with it, since the user asked for the hold gesture separately
// without saying to remove this one. Tracked separately from the
// press-and-hold state above: a "quick tap" here means release happened
// before the 1s long-press mark (same threshold the Settings-open gesture
// already uses), so this can never fire from the same physical touch as
// any of the three hold gestures — all are mutually exclusive by
// construction.
static uint32_t lastQuickTapMs = 0;
// Widened 400->600->2000 (2026-09-21, user-reported "thu double tap mai
// khong duoc") — real-hardware [tapDbg] capture showed the user's actual
// gap between two taps running 1283-3279ms, an order of magnitude past a
// typical mouse-double-click window; 2000ms comfortably covers that
// without meaningfully risking two UNRELATED taps within 2s of each other,
// since nothing else on this Dashboard responds to a quick tap at all.
static const uint32_t kDoubleTapWindowMs = 2000;

// Guards onDashReleasedOrLost's body from running more than once per
// physical press — added 2026-09-21 debugging the double-tap gesture not
// registering. LVGL can fire BOTH LV_EVENT_RELEASED and LV_EVENT_PRESS_LOST
// for what a user experiences as one clean tap (this board's touch
// controller has a known occasional stuck/glitch quirk — see
// AXS15231BTouch.cpp — that can plausibly trigger this). Without this
// guard, a single real tap firing both events would consume the double-tap
// window against itself (the 2nd spurious event reads as "the 2nd tap of a
// pair" a few milliseconds after the 1st), leaving no window open for the
// user's actual 2nd physical tap.
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
    simpleModeToggledThisPress = false;
    wifiToggledThisPress = false;
    releaseHandledThisPress = false;
}

static void onDashLongPressed(lv_event_t *) { longPressFired = true; }

static void onDashLongPressedRepeat(lv_event_t *) {
    uint32_t heldMs = millis() - pressStartMs;
    if (!simpleModeToggledThisPress && heldMs >= kSimpleModeHoldMs) {
        simpleModeToggledThisPress = true;
        cfg.simpleUiMode = !cfg.simpleUiMode;
        saveConfigToNVS(cfg);
        Serial.printf("[uidemo] Simple layout %s via 2s hold gesture\n", cfg.simpleUiMode ? "ON" : "OFF");
    }
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
    if (simpleModeToggledThisPress) return; // already handled above — don't also open Settings
    if (longPressFired) {
        lv_screen_load(settingsScreen);
        return;
    }

    // Quick-tap double-click: toggles Simple layout (see cfg.simpleUiMode's
    // own comment for what the two modes are). Persisted immediately, not
    // left to Settings' own Save button — a gesture made directly on the
    // Dashboard has no separate "confirm" step the way a Settings session
    // does, so the choice needs to survive a restart on its own.
    uint32_t now = millis();
    if (now - lastQuickTapMs < kDoubleTapWindowMs) {
        lastQuickTapMs = 0; // consume the pair — a 3rd rapid tap starts a fresh pair, doesn't re-trigger instantly
        cfg.simpleUiMode = !cfg.simpleUiMode;
        saveConfigToNVS(cfg);
        Serial.printf("[uidemo] Simple layout %s via double-tap gesture\n", cfg.simpleUiMode ? "ON" : "OFF");
    } else {
        lastQuickTapMs = now;
    }
}

// Small helper: a borderless, non-interactive circular object — used for
// the radar-sweep rings and the primary-target lock ring. Plain vector
// dots/rings, not bitmaps, so they cost nothing extra against the
// flash/PSRAM budget.
static lv_obj_t *makeDot(lv_obj_t *parent, int diam) {
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_set_size(o, diam, diam);
    lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE);
    return o;
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
// below. This is the ORIGINAL layout, unmoved except for being pulled into
// its own function so buildDashboard() can pick it or the portrait one.
// ---------------------------------------------------------------------
static void buildDashboardLandscape(lv_obj_t *scr) {
    // ---------------- Top status bar ----------------
    // Two zones now (was three), matching the 50/50 body split below:
    // topLeft sits over PARAM_COL (radar + GNSS — both sensor-health
    // telltales, natural fit alongside the numeric readouts on that side),
    // topRight sits over ROAD_COL (sun/clock + settings gear — trip/time
    // info alongside the road view). Reworked 2026-09-16 for the "chia doi
    // man hinh" request; topRight's internal arrangement (sun-left/clock-
    // growing-right, gear pinned right, wifi left of gear) is copied
    // unchanged from buildDashboardPortrait()'s own topRight, already
    // proven there.
    //
    // Status is color-only, no OK/FAULT/SEARCH words (direct request):
    // GREEN = normal, RED = fault (nothing received/offline), blinking
    // AMBER = pending/searching (module alive, just no fix yet). RADAR
    // only has two real states in this sim (online/offline), so it never
    // blinks — see refreshDashboard().
    lv_obj_t *topLeft = makePane(scr, 0, 0, PARAM_COL_W, TOP_H);
    lv_obj_t *topRight = makePane(scr, ROAD_COL_X, 0, ROAD_COL_W, TOP_H);

    // radarCx shifted 17->35 (user-requested 2026-09-16, "can chinh can doi
    // lai vi tri bieu tuong radar, GNSS, time") — previously the radar
    // cluster+caption sat jammed against topLeft's left edge while GNSS sat
    // jammed against its right edge, leaving a big dead gap between them.
    // topLeft is 240px wide; treating it as two even 120px zones (matching
    // the param-cell grid's own even-split reasoning) and centering the
    // radar cluster+"RADAR" caption block (~69px wide: 18px cluster + 6px
    // gap + ~45px text) in the LEFT zone gives (120-69)/2 = ~26px left
    // margin, i.e. cluster center at 26+9 = 35.
    static const int kRadarDiam[3] = {6, 12, 18};
    int radarCx = 35, radarCy = TOP_H / 2;
    for (int i = 0; i < 3; i++) {
        radarRing[i] = lv_obj_create(topLeft);
        lv_obj_set_size(radarRing[i], kRadarDiam[i], kRadarDiam[i]);
        lv_obj_set_pos(radarRing[i], radarCx - kRadarDiam[i] / 2, radarCy - kRadarDiam[i] / 2);
        lv_obj_set_style_radius(radarRing[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(radarRing[i], LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(radarRing[i], 1, 0);
        lv_obj_clear_flag(radarRing[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(radarRing[i], LV_OBJ_FLAG_CLICKABLE);
    }
    radarCenterDot = makeDot(topLeft, 3);
    lv_obj_set_pos(radarCenterDot, radarCx - 1, radarCy - 1);

    // Chained with lv_obj_align_to() off each element's ACTUAL rendered
    // size, not guessed pixel gaps — a fixed offset guess is exactly how
    // "some icons overlapping" happened (user-reported 2026-09-15): the
    // guessed gaps didn't account for the real glyph widths. align_to
    // can't overlap regardless of font metrics, so this is fixed for good,
    // not just for this font/size.
    radarCaption = lv_label_create(topLeft);
    lv_label_set_text(radarCaption, "RADAR");
    lv_obj_align_to(radarCaption, radarRing[2], LV_ALIGN_OUT_RIGHT_MID, 6, 0); // ring[2] = the outer 18px ring, the cluster's true right edge

    // GNSS centered in topLeft's RIGHT 120px zone (was pinned hard to
    // topLeft's own right edge, leaving the same kind of dead-gap imbalance
    // radarCx's own comment above describes). Icon+"GNSS" caption is ~64px
    // wide (20px GPS glyph + 4px gap + ~40px text); centered in a 120px zone
    // gives (120-64)/2 = 28px margin, so the icon's left edge sits at
    // 120 (zone start) + 28 = 148 from topLeft's own left edge.
    gnssIcon = lv_label_create(topLeft);
    lv_label_set_text(gnssIcon, LV_SYMBOL_GPS);
    lv_obj_align(gnssIcon, LV_ALIGN_LEFT_MID, 148, 0);

    gnssCaption = lv_label_create(topLeft);
    lv_label_set_text(gnssCaption, "GNSS");
    lv_obj_align_to(gnssCaption, gnssIcon, LV_ALIGN_OUT_RIGHT_MID, 4, 0);

    // Sun/moon icon anchored first (fixed 22x22, constant size) and the
    // clock text chained off its RIGHT edge, growing away from it — the
    // other order (icon chained off the clock) would break the moment the
    // clock's digit count/width changes. Centered in topRight's LEFT 120px
    // zone (was pinned to topRight's own left edge — user-requested
    // 2026-09-16 rebalance, same reasoning as radarCx/gnssIcon above): block
    // is ~71px wide (22px icon + 4px gap + ~45px "HH:MM" text), centered in
    // 120px gives (120-71)/2 = ~25px left margin.
    sunIcon = makeIcon(topRight, &sun_icon);
    lv_obj_align(sunIcon, LV_ALIGN_LEFT_MID, 25, 0);

    clockLabel = lv_label_create(topRight);
    lv_obj_align_to(clockLabel, sunIcon, LV_ALIGN_OUT_RIGHT_MID, 4, 0);

    // Decorative only — the actual entry point is the long-press-anywhere
    // gesture on dashRoot (deliberately not a short tap, so it can't be hit
    // by accident while driving; spec section 17).
    gearIcon = lv_label_create(topRight);
    lv_label_set_text(gearIcon, LV_SYMBOL_SETTINGS);
    lv_obj_align(gearIcon, LV_ALIGN_RIGHT_MID, -4, 0);

    // WiFi indicator (user-requested 2026-09-14) — chained off gearIcon's
    // actual left edge via align_to, same "no guessed pixel gaps" reasoning
    // as radarCaption/gnssCaption above. Cyan accent, not red/amber/green:
    // this is a system telltale ("WiFi radio is on"), not a target-risk
    // color (spec section 14.2 — see ACCENT_COLOR's own comment). Hidden by
    // default: WiFi itself defaults OFF at boot (net/WebPortal.h), and
    // refreshDashboard() only un-hides this once webPortalIsEnabled() is
    // actually true.
    wifiTopIcon = lv_label_create(topRight);
    lv_label_set_text(wifiTopIcon, LV_SYMBOL_WIFI);
    lv_obj_set_style_text_color(wifiTopIcon, lv_color_hex(ACCENT_COLOR), 0);
    lv_obj_align_to(wifiTopIcon, gearIcon, LV_ALIGN_OUT_LEFT_MID, -8, 0);
    lv_obj_clear_flag(wifiTopIcon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(wifiTopIcon, LV_OBJ_FLAG_HIDDEN);

    // ---------------- Column divider ----------------
    // Just one now (was two, for the old 3-column split), at the PARAM_COL
    // / ROAD_COL boundary. Slot 1 becomes a degenerate (zero-length,
    // hidden) placeholder — applyTheme() unconditionally recolors
    // colDividerLine[0]/[1], same reasoning buildDashboardPortrait() already
    // uses for having no real divider at all.
    static lv_point_precise_t divPts[2][2];
    {
        lv_obj_t *line = lv_line_create(scr);
        lv_obj_set_style_line_width(line, 1, 0);
        lv_point_precise_t local[2] = {{(lv_value_precise_t)PARAM_COL_W, (lv_value_precise_t)COL_TOP},
                                        {(lv_value_precise_t)PARAM_COL_W, (lv_value_precise_t)(COL_TOP + COL_H)}};
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

    // ---------------- Param column (left half): speed, limit, warning, TTC ----------------
    // A 2x2 grid now (user-requested 2026-09-16: "chia thanh bang 2x2, Toc do
    // thuc te/Toc do toi da, Canh bao/TTC") — replaces the single evenly-
    // spaced column from the previous pass (which itself replaced an even
    // earlier mismatched two-sub-column split; see git history/prior
    // comments in this area for that lineage). leftCol is still the outer
    // 240x264 container; four equal 120x132 cells sit inside it as its own
    // children, each hosting one group centered via TOP_MID relative to
    // ITS cell — applyTheme() only themes specific widgets by name, never
    // leftCol or these cells, so this restructuring is safe.
    leftCol = makePane(scr, 0, COL_TOP, PARAM_COL_W, COL_H);
    static const int kCellW = PARAM_COL_W / 2, kCellH = COL_H / 2; // 120 x 132
    lv_obj_t *cellSpeed = makePane(leftCol, 0, 0, kCellW, kCellH);           // top-left: actual speed
    lv_obj_t *cellLimit = makePane(leftCol, kCellW, 0, kCellW, kCellH);      // top-right: speed limit sign
    lv_obj_t *cellWarn = makePane(leftCol, 0, kCellH, kCellW, kCellH);       // bottom-left: warning
    lv_obj_t *cellTtc = makePane(leftCol, kCellW, kCellH, kCellW, kCellH);   // bottom-right: TTC

    // Top-left — ego speed. Bumped 28->36->48 (user-requested 2026-09-16 then
    // 2026-09-21, "tang kich thuoc cac so hien thi len nua") —
    // LV_FONT_MONTSERRAT_48 enabled in lv_conf.h (already needed for Simple
    // layout's distance number). y positioned (not simply centered in the
    // cell) so this number's own vertical center lands on the SAME line as
    // speedLimitValueLabel's center in the cell beside it (user-requested
    // 2026-09-16, "vi tri so nen can thang hang voi nhau") — the sign is
    // centered in its 132px cell (top=(132-88)/2=22, center=22+44=66, per
    // its own diameter), so speedLabel's top is placed at 66 -
    // line_height/2 = 66-52/2 = 40 (font 48's line_height is 52, confirmed
    // from lv_font_montserrat_48.c) to match that same 66px center line.
    speedLabel = lv_label_create(cellSpeed);
    lv_obj_set_style_text_font(speedLabel, &lv_font_montserrat_48, 0);
    lv_obj_align(speedLabel, LV_ALIGN_TOP_MID, 0, 40);

    // Pushed 86->94 (number's own bottom edge moved from 46+40=86 to
    // 40+52=92 when its font grew — this caption must clear that, +2px gap).
    kmhCaption = lv_label_create(cellSpeed);
    lv_label_set_text(kmhCaption, "km/h");
    lv_obj_align(kmhCaption, LV_ALIGN_TOP_MID, 0, 94);

    // Top-right — speed limit sign, 88px diameter, vertically centered in
    // the 132px cell ((132-88)/2 = 22) and horizontally centered in the
    // 120px cell (comfortably clears the sign's own 9px border on both
    // sides: (120-88)/2 = 16).
    //
    // Speed limit for the current road segment (user-requested 2026-09-15,
    // "ngay dưới phần hiển thị tốc độ xe chạy" — right below ego speed,
    // later refined to look like an actual Vietnamese speed-limit sign) —
    // sourced from map/SpeedLimitManager.cpp's microSD map-matching via
    // RoadInfoSnapshot.
    //
    // A real QCVN 41:2019/BGTVT P.127 sign: white disc, thick red ring,
    // bold black number, nothing else printed on it — drawn as plain LVGL
    // vector primitives (a circular obj + a label), not a generated bitmap:
    // there's no photographic detail here for a bitmap to earn its keep on
    // (contrast the car/warning/sun/moon icons in icons/Icons.h, which
    // exist BECAUSE their shapes aren't expressible as flat vector
    // primitives), and a plain lv_obj circle stays trivially resizable and
    // needs no Python/Pillow regeneration step.
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

    // Sibling, not a child of speedLimitSign — see this pair's declaration
    // comment for why (needs to keep showing "--" while the sign itself is
    // hidden). align_to centers it on the sign's actual geometry rather
    // than a second guessed offset, same reasoning every other icon+label
    // pairing on this dashboard already follows.
    // Bumped 24->28->32 (user-requested 2026-09-16 then 2026-09-21, "tang
    // kich thuoc cac so hien thi len nua") — kept short of matching
    // speedLabel's 48: this number sits inside a FIXED 88px sign graphic
    // (unlike speedLabel's open cell), and a 3-digit limit like "100"/"120"
    // at 36+ risks visually crowding the sign's own 9px red ring. 32 is the
    // largest size that stays clearly inside the ~70px usable interior (88
    // diameter - 2x9 border) for a 3-digit number — re-check by eye on real
    // hardware if a wider font ever gets substituted.
    speedLimitValueLabel = lv_label_create(cellLimit);
    lv_obj_set_style_text_font(speedLimitValueLabel, &lv_font_montserrat_32, 0);
    lv_obj_align_to(speedLimitValueLabel, speedLimitSign, LV_ALIGN_CENTER, 0, 0);

    // Bottom-left — warning (30px icon + 18px word, centered as a block:
    // top margin (132-48)/2 = 42).
    warningIconObj = makeIcon(cellWarn, &warning_icon); // fixed true color — see Icons.h
    lv_obj_align(warningIconObj, LV_ALIGN_TOP_MID, 0, 42);

    riskWordLabel = lv_label_create(cellWarn);
    lv_obj_align(riskWordLabel, LV_ALIGN_TOP_MID, 0, 76);

    // Bottom-right — TTC. Bumped 24->36 (user-requested 2026-09-21, "tang
    // kich thuoc cac so hien thi len nua") — top kept at 42 (same start as
    // before), caption pushed 76->86 since the number's own bottom edge
    // moved from 42+27=69 to 42+40=82 (font 36's line_height=40) — needs to
    // clear that plus a small gap.
    ttcValueLabel = lv_label_create(cellTtc);
    lv_obj_set_style_text_font(ttcValueLabel, &lv_font_montserrat_36, 0);
    lv_obj_set_style_text_color(ttcValueLabel, lv_color_white(), 0);
    lv_obj_align(ttcValueLabel, LV_ALIGN_TOP_MID, 0, 42);

    ttcCaption = lv_label_create(cellTtc);
    lv_label_set_text(ttcCaption, "TTC");
    lv_obj_align(ttcCaption, LV_ALIGN_TOP_MID, 0, 86);

    // ---------------- Road column (right half): distance + road + targets ----------------
    midCol = makePane(scr, ROAD_COL_X, COL_TOP, ROAD_COL_W, COL_H);

    buildTrafficCard(midCol, 224, 224);

    primaryDistLabel = lv_label_create(midCol);
    lv_obj_set_style_text_font(primaryDistLabel, &lv_font_montserrat_24, 0);
    lv_obj_align(primaryDistLabel, LV_ALIGN_TOP_MID, 0, 2);

    // Simple-layout-only (see its own declaration comment) — positioned
    // below where primaryDistLabel sits once centered by the simpleUiMode
    // block (48pt font, line_height 52, so its bottom edge is roughly
    // 26px below CENTER; +14px gap).
    legalDistCaption = lv_label_create(midCol);
    lv_obj_align(legalDistCaption, LV_ALIGN_CENTER, 0, 40);
    lv_obj_add_flag(legalDistCaption, LV_OBJ_FLAG_HIDDEN);

    // roadArea's own colors are NOT themed — it stays a consistently dark
    // panel in both Day and Night (like the schematic "road view" in most
    // cluster/nav UIs), only the chrome around it switches with
    // gnss.daytime. See applyTheme() below.
    roadArea = lv_obj_create(midCol);
    lv_obj_set_pos(roadArea, ROAD_X, ROAD_Y);
    lv_obj_set_size(roadArea, ROAD_W, ROAD_H);
    lv_obj_set_style_bg_color(roadArea, lv_color_hex(0x12181F), 0);
    lv_obj_set_style_border_color(roadArea, lv_color_hex(0x243040), 0);
    lv_obj_set_style_border_width(roadArea, 1, 0);
    lv_obj_set_style_radius(roadArea, 8, 0);
    lv_obj_clear_flag(roadArea, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(roadArea, LV_OBJ_FLAG_CLICKABLE);
    // Same root cause as Settings.cpp's 2026-09-16 invisible-choice-button
    // bug: lv_obj_get_width/height() only reflect a pending lv_obj_set_size()
    // after the next redraw (LVGL's own documented behavior), but this whole
    // screen is built inside setup() before LVGL has ever redrawn once.
    // buildRoadLanes() right below reads roadArea's size back — without this,
    // it silently got 0x0 and drew every lane line/dash/grid mark with zero
    // extent, i.e. invisible, in every orientation (user-reported 2026-09-16,
    // "khong hien thi vach chia lane duong").
    lv_obj_update_layout(roadArea);
    lv_obj_add_flag(roadArea, LV_OBJ_FLAG_HIDDEN); // trafficCard takes visual precedence

    buildRoadLanes();

    faultLabel = lv_label_create(roadArea);
    lv_obj_set_style_text_color(faultLabel, lv_color_hex(0xFF5544), 0);
    lv_obj_set_style_text_align(faultLabel, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(faultLabel);
    lv_obj_add_flag(faultLabel, LV_OBJ_FLAG_HIDDEN);

    // Primary-target lock ring — spatial referencing (AR-HUD study: exact
    // on-target highlighting gives the most stable visual attention).
    // Created once here, just repositioned/shown in refreshDashboard().
    primaryRing = lv_obj_create(roadArea);
    lv_obj_set_style_radius(primaryRing, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(primaryRing, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(primaryRing, 2, 0);
    lv_obj_set_style_border_color(primaryRing, lv_color_hex(ACCENT_COLOR), 0);
    lv_obj_clear_flag(primaryRing, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(primaryRing, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(primaryRing, LV_OBJ_FLAG_HIDDEN);

    for (int i = 0; i < MAX_TARGETS; i++) {
        targetIcon[i] = makeIcon(roadArea, &car_icon);
        lv_obj_set_style_image_recolor_opa(targetIcon[i], LV_OPA_COVER, 0);
        lv_obj_add_flag(targetIcon[i], LV_OBJ_FLAG_HIDDEN);

        targetLabels[i] = lv_label_create(roadArea);
        lv_obj_set_style_text_color(targetLabels[i], lv_color_hex(0xCCCCCC), 0);
        lv_obj_add_flag(targetLabels[i], LV_OBJ_FLAG_HIDDEN);
    }

    // ---------------- Bottom info bar ----------------
    bottomInfoLabel = lv_label_create(scr);
    lv_obj_align(bottomInfoLabel, LV_ALIGN_BOTTOM_MID, 0, -6);
}

// ---------------------------------------------------------------------
// Portrait content (rotation 0/2, 320x480) — a different ARRANGEMENT of the
// exact same widgets/information as buildDashboardLandscape() above, not a
// parametric reflow of it (see this file's own layout-constants comment for
// why). Stacked top-to-bottom instead of 3 side-by-side columns: status bar,
// then speed+limit-sign side by side, then the road/target view (given the
// most vertical room — a receding road naturally suits a tall screen better
// than it suited landscape's 3-way split), then TTC/warning, then the same
// bottom info bar.
// ---------------------------------------------------------------------
static void buildDashboardPortrait(lv_obj_t *scr) {
    const int scrW = 320, scrH = 480; // both rotation 0 and 2 produce exactly this — see AppConfig.h's screenRotation
    const int pTopH = 30, pBottomH = 26;
    const int pSpeedRowH = 116;
    const int pWarnRowH = 40;
    const int pPad = 8;

    // ---------------- Top status bar (one row, all three telltales) ----------------
    lv_obj_t *topLeft = makePane(scr, 0, 0, scrW / 2, pTopH);
    lv_obj_t *topRight = makePane(scr, scrW / 2, 0, scrW / 2, pTopH);

    static const int kRadarDiam[3] = {6, 12, 18};
    int radarCx = 17, radarCy = pTopH / 2;
    for (int i = 0; i < 3; i++) {
        radarRing[i] = lv_obj_create(topLeft);
        lv_obj_set_size(radarRing[i], kRadarDiam[i], kRadarDiam[i]);
        lv_obj_set_pos(radarRing[i], radarCx - kRadarDiam[i] / 2, radarCy - kRadarDiam[i] / 2);
        lv_obj_set_style_radius(radarRing[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(radarRing[i], LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(radarRing[i], 1, 0);
        lv_obj_clear_flag(radarRing[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(radarRing[i], LV_OBJ_FLAG_CLICKABLE);
    }
    radarCenterDot = makeDot(topLeft, 3);
    lv_obj_set_pos(radarCenterDot, radarCx - 1, radarCy - 1);

    radarCaption = lv_label_create(topLeft);
    lv_label_set_text(radarCaption, "RADAR");
    lv_obj_align_to(radarCaption, radarRing[2], LV_ALIGN_OUT_RIGHT_MID, 6, 0);

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

    // ---------------- Road/target view — the big middle area ----------------
    midCol = makePane(scr, 0, pTopH + pSpeedRowH, scrW, scrH - pTopH - pSpeedRowH - pWarnRowH - pBottomH);
    // lv_obj_get_height(midCol) below needs midCol's just-set size committed
    // now rather than at the next redraw (see roadArea's own update_layout
    // comment in buildDashboardLandscape() for the full explanation) —
    // otherwise it reads back 0 and roadArea below ends up with a negative
    // height.
    lv_obj_update_layout(midCol);

    primaryDistLabel = lv_label_create(midCol);
    lv_obj_set_style_text_font(primaryDistLabel, &lv_font_montserrat_24, 0);
    lv_obj_align(primaryDistLabel, LV_ALIGN_TOP_MID, 0, 2);

    // Simple-layout-only — see its declaration comment and the landscape
    // builder's identical block above.
    legalDistCaption = lv_label_create(midCol);
    lv_obj_align(legalDistCaption, LV_ALIGN_CENTER, 0, 40);
    lv_obj_add_flag(legalDistCaption, LV_OBJ_FLAG_HIDDEN);

    roadArea = lv_obj_create(midCol);
    lv_obj_set_pos(roadArea, pPad, 32);
    lv_obj_set_size(roadArea, scrW - 2 * pPad, lv_obj_get_height(midCol) - 32 - 6);
    lv_obj_set_style_bg_color(roadArea, lv_color_hex(0x12181F), 0);
    lv_obj_set_style_border_color(roadArea, lv_color_hex(0x243040), 0);
    lv_obj_set_style_border_width(roadArea, 1, 0);
    lv_obj_set_style_radius(roadArea, 8, 0);
    lv_obj_clear_flag(roadArea, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(roadArea, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_update_layout(roadArea); // see buildDashboardLandscape()'s own comment on this call

    buildRoadLanes();

    faultLabel = lv_label_create(roadArea);
    lv_obj_set_style_text_color(faultLabel, lv_color_hex(0xFF5544), 0);
    lv_obj_set_style_text_align(faultLabel, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(faultLabel);
    lv_obj_add_flag(faultLabel, LV_OBJ_FLAG_HIDDEN);

    primaryRing = lv_obj_create(roadArea);
    lv_obj_set_style_radius(primaryRing, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(primaryRing, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(primaryRing, 2, 0);
    lv_obj_set_style_border_color(primaryRing, lv_color_hex(ACCENT_COLOR), 0);
    lv_obj_clear_flag(primaryRing, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(primaryRing, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(primaryRing, LV_OBJ_FLAG_HIDDEN);

    for (int i = 0; i < MAX_TARGETS; i++) {
        targetIcon[i] = makeIcon(roadArea, &car_icon);
        lv_obj_set_style_image_recolor_opa(targetIcon[i], LV_OPA_COVER, 0);
        lv_obj_add_flag(targetIcon[i], LV_OBJ_FLAG_HIDDEN);

        targetLabels[i] = lv_label_create(roadArea);
        lv_obj_set_style_text_color(targetLabels[i], lv_color_hex(0xCCCCCC), 0);
        lv_obj_add_flag(targetLabels[i], LV_OBJ_FLAG_HIDDEN);
    }

    // ---------------- Warning/TTC row — one horizontal strip ----------------
    rightCol = makePane(scr, 0, scrH - pBottomH - pWarnRowH, scrW, pWarnRowH);

    warningIconObj = makeIcon(rightCol, &warning_icon);
    lv_obj_align(warningIconObj, LV_ALIGN_LEFT_MID, 16, 0);

    riskWordLabel = lv_label_create(rightCol);
    lv_obj_align_to(riskWordLabel, warningIconObj, LV_ALIGN_OUT_RIGHT_MID, 10, 0);

    ttcCaption = lv_label_create(rightCol);
    lv_label_set_text(ttcCaption, "TTC");
    lv_obj_align(ttcCaption, LV_ALIGN_RIGHT_MID, -16, -12);

    // Bumped 24->28 only (not matching landscape's 36) — this whole row is
    // a cramped 40px-tall strip (pWarnRowH), unlike landscape's 132px-tall
    // cell; 28 is as far as this can grow without the number visibly
    // colliding with ttcCaption above it in the same tight row.
    ttcValueLabel = lv_label_create(rightCol);
    lv_obj_set_style_text_font(ttcValueLabel, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(ttcValueLabel, lv_color_white(), 0);
    lv_obj_align(ttcValueLabel, LV_ALIGN_RIGHT_MID, -16, 10);

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
    int scrW = gfx->width(), scrH = gfx->height(); // dynamic — SCR_W/SCR_H below are landscape-only constants, wrong for portrait
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

    // Full-screen color flash for WARNING/CRITICAL risk (user-requested
    // 2026-09-15: the color-coded distance warning should flash the WHOLE
    // screen via an overlay, not stay confined to small text/icons —
    // amber/yellow for the medium ("vừa") tier, red for the dangerous
    // ("nguy hiểm") tier). Translucent, not opaque, so speed/TTC/etc stay
    // readable through it. Toggled via the HIDDEN flag on a slow (250 or
    // 500ms) cadence in refreshDashboard() — gated so the color/visibility
    // only change when they actually need to, never every 150ms tick: a
    // full-screen invalidation is the single most expensive thing this UI
    // can ask the Canvas driver to do (see DisplayDriver.h's full-frame-
    // flush note and the roadArea critical-border comment below for the
    // measured cost of getting this gating wrong).
    warningFlashOverlay = lv_obj_create(scr);
    lv_obj_set_pos(warningFlashOverlay, 0, 0);
    lv_obj_set_size(warningFlashOverlay, scrW, scrH);
    lv_obj_set_style_border_width(warningFlashOverlay, 0, 0);
    lv_obj_set_style_radius(warningFlashOverlay, 0, 0);
    lv_obj_set_style_bg_opa(warningFlashOverlay, LV_OPA_30, 0);
    lv_obj_clear_flag(warningFlashOverlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(warningFlashOverlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(warningFlashOverlay, LV_OBJ_FLAG_HIDDEN);

    // Speeding overlay (user-requested 2026-09-16) — same shared-object/
    // translucent/HIDDEN-toggle pattern as warningFlashOverlay just above,
    // own dedicated color (0xB33DC6, matches STATUS_SPEEDING defined below
    // buildDashboard() — same "not visible here yet" reason harshBrakeLabel
    // right below hardcodes STATUS_AMBER's value too) so it never gets
    // mistaken for a collision-risk flash. Fixed color at build time, not
    // reassigned in refreshDashboard() like warningFlashOverlay's color is
    // — speeding has no tiers to switch between.
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

    // Sudden-closing-speed banner (added 2026-09-16) — a small pill at the
    // top-center of the FULL screen (scr, not roadArea — same "one shared
    // object, built once here" reasoning as warningFlashOverlay right
    // above, so it works unchanged in either orientation). Deliberately
    // solid/opaque and drawn after the top status bar/road panel, so it can
    // briefly cover a corner of either during the rare, urgent moment it's
    // actually shown — acceptable same as warningFlashOverlay itself
    // temporarily dimming readability during a real CRITICAL event.
    harshBrakeLabel = lv_label_create(scr);
    lv_label_set_text(harshBrakeLabel, "SUDDEN CLOSING SPEED");
    lv_obj_set_style_bg_color(harshBrakeLabel, lv_color_hex(0xE0C020), 0); // matches STATUS_AMBER (defined below buildDashboard(), not visible here yet)
    lv_obj_set_style_bg_opa(harshBrakeLabel, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(harshBrakeLabel, lv_color_black(), 0);
    lv_obj_set_style_pad_hor(harshBrakeLabel, 10, 0);
    lv_obj_set_style_pad_ver(harshBrakeLabel, 4, 0);
    lv_obj_set_style_radius(harshBrakeLabel, 6, 0);
    lv_obj_align(harshBrakeLabel, LV_ALIGN_TOP_MID, 0, 2);
    lv_obj_add_flag(harshBrakeLabel, LV_OBJ_FLAG_HIDDEN);

    // Positioned just below harshBrakeLabel (y=28 vs. its y=2) so the two
    // stack rather than overlap on the rare tick both happen to be visible
    // at once. Text set live in refreshDashboard() (the actual upcoming
    // limit number isn't known until then).
    aheadLimitLabel = lv_label_create(scr);
    lv_obj_set_style_bg_color(aheadLimitLabel, lv_color_hex(0x2F7CE0), 0);
    lv_obj_set_style_bg_opa(aheadLimitLabel, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(aheadLimitLabel, lv_color_white(), 0);
    lv_obj_set_style_pad_hor(aheadLimitLabel, 10, 0);
    lv_obj_set_style_pad_ver(aheadLimitLabel, 4, 0);
    lv_obj_set_style_radius(aheadLimitLabel, 6, 0);
    lv_obj_align(aheadLimitLabel, LV_ALIGN_TOP_MID, 0, 28);
    lv_obj_add_flag(aheadLimitLabel, LV_OBJ_FLAG_HIDDEN);

    // Positioned below both banners above (y=54) — see cameraAheadLabel's
    // own declaration comment for the color/stacking reasoning.
    cameraAheadLabel = lv_label_create(scr);
    lv_obj_set_style_bg_color(cameraAheadLabel, lv_color_hex(0xE0A020), 0);
    lv_obj_set_style_bg_opa(cameraAheadLabel, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(cameraAheadLabel, lv_color_black(), 0);
    lv_obj_set_style_pad_hor(cameraAheadLabel, 10, 0);
    lv_obj_set_style_pad_ver(cameraAheadLabel, 4, 0);
    lv_obj_set_style_radius(cameraAheadLabel, 6, 0);
    lv_obj_align(cameraAheadLabel, LV_ALIGN_TOP_MID, 0, 54);
    lv_obj_add_flag(cameraAheadLabel, LV_OBJ_FLAG_HIDDEN);

    // Traffic sign banner (Khu dan cu, cam vuot, tram thu phi, den tin hieu)
    trafficSignLabel = lv_label_create(scr);
    lv_obj_set_style_bg_color(trafficSignLabel, lv_color_hex(0x2080C0), 0);
    lv_obj_set_style_bg_opa(trafficSignLabel, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(trafficSignLabel, lv_color_white(), 0);
    lv_obj_set_style_pad_hor(trafficSignLabel, 10, 0);
    lv_obj_set_style_pad_ver(trafficSignLabel, 4, 0);
    lv_obj_set_style_radius(trafficSignLabel, 6, 0);
    lv_obj_align(trafficSignLabel, LV_ALIGN_TOP_MID, 0, 82);
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

// Day/Night color theme for the chrome AROUND the road panel — background,
// captions, primary readouts, gear icon, column dividers, bottom info bar
// (user-requested 2026-09-15, driven by the same gnss.daytime sunrise/
// sunset calc already used for the clock icon). The road panel itself
// (background/lane lines/target labels) stays a consistently dark
// schematic view in both themes — same reasoning most cluster/nav UIs keep
// their map/camera area dark regardless of day/night, see the comment
// where roadArea is created. Risk/status colors (red/amber/green,
// ACCENT_COLOR) also stay theme-independent everywhere: changing a safety
// color's hue between Day and Night would undermine the "color only means
// risk" rule (spec section 14.2), not serve it.
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

    lv_obj_set_style_text_color(radarCaption, captionText, 0);
    lv_obj_set_style_text_color(gnssCaption, captionText, 0);
    lv_obj_set_style_text_color(clockLabel, clockText, 0);
    lv_obj_set_style_text_color(gearIcon, gearColor, 0);
    for (int i = 0; i < 2; i++) lv_obj_set_style_line_color(colDividerLine[i], dividerColor, 0);

    // speedLabel's themed color is remembered (not just applied) because
    // refreshDashboard() overrides it to red on top of this whenever the
    // driver is over the matched speed limit — same theme-independent-
    // override pattern riskWordLabel/ttcValueLabel already use for their
    // own safety coloring, just layered on a label that ALSO needs a normal
    // themed color the rest of the time.
    currentPrimaryTextColor = primaryText;
    lv_obj_set_style_text_color(speedLabel, primaryText, 0);
    lv_obj_set_style_text_color(kmhCaption, captionText, 0);
    // speedLimitSign/speedLimitValueLabel are NOT themed here — a real
    // speed-limit sign is white/red/black regardless of day or night (spec
    // section 14.2's "a safety color must mean the same thing in both
    // themes" reasoning extends naturally to "a regulatory sign doesn't
    // recolor itself for you either"). See refreshDashboard().
    lv_obj_set_style_text_color(primaryDistLabel, primaryText, 0);
    lv_obj_set_style_text_color(ttcCaption, captionText, 0);

    lv_obj_set_style_text_color(bottomInfoLabel, bottomText, 0);
}

// Named lerpf (not lerp) — the newer toolchain used by env:uidemo3
// (Arduino-ESP32 3.x / a more recent libstdc++) already declares
// `float lerp(float,float,float)` at global scope via <cmath>/C++20,
// and a same-signature redeclaration is a hard conflict there.
static float lerpf(float a, float b, float t) { return a + (b - a) * t; }

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
    RadarSnapshot radar = radarSnapshot();
    GnssSnapshot gnss = gnssSnapshot();

    // Auto-wake the instant real movement resumes (not just on touch) — the
    // dim-while-stationary gate above only dims in the first place once
    // parked, so waking symmetrically on the same signal (rather than
    // leaving the driver to tap the screen themselves) is the safety-first
    // completion of that same 2026-09-16 request.
    if (screenDimmed && gnss.fix && gnss.egoSpeedKmh > kGnssMotionThresholdKmh) wakeScreen();

    // --- Top bar: color-only status (no OK/FAULT/SEARCH words) ---
    // RADAR only has two real states in this sim (online/offline) — no
    // "searching" telemetry exists for it, so it's a plain green/red dot.
    lv_color_t radarColor = radar.online ? lv_color_hex(STATUS_GREEN) : lv_color_hex(STATUS_RED);
    for (int i = 0; i < 3; i++) lv_obj_set_style_border_color(radarRing[i], radarColor, 0);
    lv_obj_set_style_bg_color(radarCenterDot, radarColor, 0);

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
    // or cfg.themeMode itself changing) — same gating rule as the roadArea
    // critical-border flash below: touching lv_obj_set_style_*
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

    // Simple layout (user-requested 2026-09-16, cfg.simpleUiMode — see
    // AppConfig.h's comment): swap the graphical road view for one big
    // same-lane-distance number, gated on an actual mode CHANGE, same
    // "don't restyle every tick for nothing" discipline as the theme block
    // just above. primaryDistLabel's own text/color is still set further
    // below by the existing distance-readout code (unchanged) — this block
    // only handles what differs between the two layouts: whether roadArea
    // is visible at all, and the label's font/position.
    static bool lastSimpleUiMode = false;
    static bool simpleUiModeInit = false;
    if (!simpleUiModeInit || cfg.simpleUiMode != lastSimpleUiMode) {
        simpleUiModeInit = true;
        lastSimpleUiMode = cfg.simpleUiMode;
        if (cfg.simpleUiMode) {
            lv_obj_add_flag(roadArea, LV_OBJ_FLAG_HIDDEN);
            // 48, not 36 — user-requested 2026-09-16 "con so khoang cach to
            // hon nua" after first seeing 36 on real hardware. Largest
            // built-in Montserrat size (lv_conf.h); still comfortably fits
            // "999 m" within roadArea's narrowest real width (landscape's
            // ~224px column).
            lv_obj_set_style_text_font(primaryDistLabel, &lv_font_montserrat_48, 0);
            lv_obj_align(primaryDistLabel, LV_ALIGN_CENTER, 0, 0);
            // legalDistCaption's own text is set every tick further below
            // (needs the current speed) — just make it visible here.
            lv_obj_clear_flag(legalDistCaption, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_clear_flag(roadArea, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_text_font(primaryDistLabel, &lv_font_montserrat_24, 0);
            lv_obj_align(primaryDistLabel, LV_ALIGN_TOP_MID, 0, 2);
            lv_obj_add_flag(legalDistCaption, LV_OBJ_FLAG_HIDDEN); // Full layout shows the same info inline instead — see below
        }
    }

    // --- Left column ---
    // Demo-mode display overlay (user-requested 2026-09-16, "demo (fake
    // sensor) khong hien thi toc do toi da, toc do thuc te"): SimTask only
    // fakes radar targets — GNSS itself is deliberately NEVER faked (see
    // gnssTaskStart()'s own "unaffected by demoMode — always real" comment,
    // a real-position safety invariant that must not be touched). That
    // means indoors, with no real fix, ego speed and the speed-limit sign
    // had nothing to show at all — defeating demo mode's whole point of
    // previewing every Dashboard function before a real drive. This overlay
    // only kicks in when cfg.demoMode is on AND there's genuinely no real
    // fix yet, so a real fix outdoors always wins over the fake numbers the
    // instant one arrives. Display-only: does not touch gnss/road, so
    // nothing downstream (audio gate, TTC, auto-dim wake, the GNSS status
    // icon) is affected — this only fills the two cells the user pointed
    // at. Fake limit is a fixed, plausible urban value (50 km/h) so the
    // oscillating fake speed also demonstrates the "over the limit" red
    // color at its peaks.
    bool demoNoFix = cfg.demoMode && !gnss.fix;
    float demoSpeedKmh = 0;
    if (demoNoFix) {
        demoSpeedKmh = 45.0f + 45.0f * sinf(millis() / 8000.0f);
        if (demoSpeedKmh < 0) demoSpeedKmh = 0;
    }
    static const float kDemoSpeedLimitKmh = 50.0f;

    // Speed limit read first: whether the driver is currently speeding
    // decides speedLabel's own color just below, so the compliance check
    // needs to happen before speedLabel is touched. map/SpeedLimitManager.cpp
    // only ever sets RoadInfoSnapshot.valid once it has BOTH a loaded map
    // and a current GNSS fix matched against it — checking .valid alone is
    // suffient, no separate gnss.fix check needed for the sign itself.
    RoadInfoSnapshot road = roadInfoSnapshot();
    bool roadValidOrDemo = road.valid || demoNoFix;
    float effectiveSpeedLimitKmh = road.valid ? road.speedLimitKmh : kDemoSpeedLimitKmh;
    static const float kSpeedingMarginKmh = 5.0f; // absorbs GPS speed-filter noise right at the boundary, same hysteresis-style reasoning as the audio-gate's own margin (AppConfig.h's hysteresisKmh)
    bool speeding = roadValidOrDemo && (gnss.fix || demoNoFix) &&
                    (demoNoFix ? demoSpeedKmh : gnss.egoSpeedKmh) > effectiveSpeedLimitKmh + kSpeedingMarginKmh;

    if (gnss.fix || demoNoFix) {
        // NOTE: LVGL's builtin vsnprintf has %f support compiled out when
        // LV_USE_FLOAT=0 (our lv_conf.h) — passing %f to lv_label_set_text_fmt
        // silently corrupts the varargs and crashes (LoadProhibited).
        // Confirmed on real hardware 2026-09-14. Format floats with the real
        // libc snprintf into a buffer instead, then set the plain string.
        char buf[16];
        snprintf(buf, sizeof(buf), "%.0f", demoNoFix ? demoSpeedKmh : gnss.egoSpeedKmh);
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
    // then "qua toc do cung nhap nhay de tang su chu y"). Uses
    // speedingFlashOverlay, its OWN dedicated color/object — not
    // warningFlashOverlay below, which stays reserved for imminent-
    // collision TTC risk, a fundamentally different kind of danger;
    // conflating "you're speeding" with "you're about to hit something"
    // under the same color would blur exactly the "one color = one
    // specific risk" rule spec section 14.2 exists to protect. Blinks at
    // 400ms — deliberately between the TTC overlay's own two rates
    // (500ms WARNING, 250ms CRITICAL below) so it reads as its own
    // distinct urgency, not a re-skin of either TTC tier. Same "only touch
    // the flag on an actual blink-phase flip" gating as the TTC overlay
    // uses, not unconditionally every 150ms tick.
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
    if (roadValidOrDemo) {
        char buf[8];
        snprintf(buf, sizeof(buf), "%.0f", (double)effectiveSpeedLimitKmh);
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
        lastAheadVisible = road.aheadLimitValid;
        lastAheadLimit = road.aheadSpeedLimitKmh;
        if (road.aheadLimitValid) {
            char buf[32];
            snprintf(buf, sizeof(buf), "Ahead: %.0f km/h in %.0fm", (double)road.aheadSpeedLimitKmh,
                     (double)road.aheadDistanceM);
            lv_label_set_text(aheadLimitLabel, buf);
            lv_obj_align(aheadLimitLabel, LV_ALIGN_TOP_MID, 0, 28); // re-center: text width just changed
            lv_obj_clear_flag(aheadLimitLabel, LV_OBJ_FLAG_HIDDEN);
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
            lv_obj_align(cameraAheadLabel, LV_ALIGN_TOP_MID, 0, 54);
            lv_obj_clear_flag(cameraAheadLabel, LV_OBJ_FLAG_HIDDEN);
            if (newlyVisible) {
                audioPlayCameraAlert();
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
            lv_obj_align(trafficSignLabel, LV_ALIGN_TOP_MID, 0, 82);
            lv_obj_clear_flag(trafficSignLabel, LV_OBJ_FLAG_HIDDEN);
            if (newlyVisible) {
                audioPlaySignNotice();
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

        // Mini radar hint at bottom of card
        if (radar.online && radar.primaryIdx >= 0) {
            char rBuf[40];
            snprintf(rBuf, sizeof(rBuf), "Radar: Xe truoc %.0fm (TTC %.1fs)",
                     (double)radar.targets[radar.primaryIdx].distanceM,
                     (double)radar.targets[radar.primaryIdx].ttcS);
            lv_label_set_text(alertRadarHint, rBuf);
            lv_obj_set_style_text_color(alertRadarHint, lv_color_hex(0xE0C020), 0);
        } else if (radar.online) {
            lv_label_set_text(alertRadarHint, "Radar: San sang (Khong xe truoc)");
            lv_obj_set_style_text_color(alertRadarHint, lv_color_hex(0x4A6278), 0);
        } else {
            lv_label_set_text(alertRadarHint, "Radar: Khong ket noi");
            lv_obj_set_style_text_color(alertRadarHint, lv_color_hex(0x4A6278), 0);
        }
    }

    // --- Middle column: road panel border flash on CRITICAL ---
    static bool lastCriticalState = false;
    bool criticalNow = radar.online && radar.primaryIdx >= 0 && radar.targets[radar.primaryIdx].ttcS <= cfg.ttcCritS;
    if (criticalNow != lastCriticalState) {
        lastCriticalState = criticalNow;
        if (criticalNow) {
            lv_obj_set_style_border_color(roadArea, lv_color_hex(0xFF3B30), 0);
            lv_obj_set_style_border_width(roadArea, 3, 0);
        } else {
            lv_obj_set_style_border_color(roadArea, lv_color_hex(0x243040), 0);
            lv_obj_set_style_border_width(roadArea, 1, 0);
        }
    }

    // --- Full-screen WARNING/CRITICAL flash ---
    // Two tiers only, not all four risk levels: CAUTION/SAFE stay
    // inline-color-only, no screen flash — flashing on every caution would
    // desensitize the driver to it well before it actually matters. Same
    // gating discipline as the roadArea border above: color only changes
    // on a tier transition, visibility only changes on an actual blink-
    // phase flip, never unconditionally every tick.
    {
        bool isCritical = false, shouldFlash = false;
        if (radar.online && radar.primaryIdx >= 0) {
            float primaryTtc = radar.targets[radar.primaryIdx].ttcS;
            isCritical = primaryTtc <= cfg.ttcCritS;
            bool isWarning = !isCritical && primaryTtc <= cfg.ttcWarnS;
            shouldFlash = isCritical || isWarning;
        }

        static bool lastFlashCritical = false;
        static bool lastFlashVisible = false;
        bool flashOn = false;
        if (shouldFlash) {
            uint32_t period = isCritical ? 250 : 500; // faster blink = more urgent
            flashOn = (millis() / period) % 2 == 0;
            if (isCritical != lastFlashCritical) {
                lastFlashCritical = isCritical;
                lv_obj_set_style_bg_color(warningFlashOverlay, lv_color_hex(isCritical ? STATUS_RED : STATUS_AMBER), 0);
            }
        }
        if (flashOn != lastFlashVisible) {
            lastFlashVisible = flashOn;
            if (flashOn) lv_obj_clear_flag(warningFlashOverlay, LV_OBJ_FLAG_HIDDEN);
            else lv_obj_add_flag(warningFlashOverlay, LV_OBJ_FLAG_HIDDEN);
        }
    }

    if (!radar.online) {
        lv_label_set_text(faultLabel, "RADAR FAULT\n(no signal)");
        lv_obj_clear_flag(faultLabel, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(faultLabel, LV_OBJ_FLAG_HIDDEN);
    }

    // Sudden-closing-speed banner — visibility only, no blink (unlike the
    // TTC flash above): this is already a rare, momentary event by
    // construction (radar.harshBrakeWarning only fires while the closing
    // rate is actually accelerating fast), so a steady banner reads clearly
    // without needing a blink to draw attention.
    if (radar.harshBrakeWarning) lv_obj_clear_flag(harshBrakeLabel, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(harshBrakeLabel, LV_OBJ_FLAG_HIDDEN);

    // Dynamic, not the fixed ROAD_W/ROAD_H constants — those only describe
    // the landscape layout; reading roadArea's real size here keeps this
    // math correct regardless of which orientation actually built it (see
    // buildRoadLanes()'s own comment for the same reasoning).
    int roadW = lv_obj_get_width(roadArea), roadH = lv_obj_get_height(roadArea);
    int activeCount = 0;
    bool primaryRingPlaced = false;
    static const int kCarBaseW = 40, kCarBaseH = 33; // matches car_icon's real 40x33 (front-view design, 2026-09-15)

    for (int i = 0; i < MAX_TARGETS; i++) {
        SimTarget &t = radar.targets[i];
        if (!t.active) {
            lv_obj_add_flag(targetIcon[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(targetLabels[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        activeCount++;

        // Reads the real independently-filtered channel directly (target-
        // tracking requirement #9, 2026-09-21) instead of recomputing it
        // from distance*sin(angle) — t.angleDeg is now only a derived
        // diagnostic value (see SharedState.h's SimTarget::lateralM
        // comment), not the authoritative lateral-position source.
        float lateral = t.lateralM;
        float normDist = t.distanceM / cfg.maxRangeM;
        if (normDist > 1) normDist = 1;
        if (normDist < 0) normDist = 0;

        int screenX = roadW / 2 + (int)((lateral / 6.0f) * (roadW / 2));
        if (screenX < 16) screenX = 16;
        if (screenX > roadW - 16) screenX = roadW - 16;
        int screenY = roadH - (int)(normDist * (roadH - 24)) - 12;

        // Perspective: bigger/closer zoom when near, smaller when far.
        uint32_t zoom = (uint32_t)lerpf(90.0f, 280.0f, 1.0f - normDist);
        int apparentW = (kCarBaseW * (int)zoom) / 256;
        int apparentH = (kCarBaseH * (int)zoom) / 256;

        lv_color_t color;
        if (t.relation == OPPOSITE) color = lv_color_hex(0x4488FF);
        else if (t.relation == UNKNOWN) color = lv_color_hex(0x8899AA);
        else color = riskColor(t.ttcS);

        lv_image_set_scale(targetIcon[i], zoom);
        lv_obj_set_pos(targetIcon[i], screenX - kCarBaseW / 2, screenY - kCarBaseH / 2);
        lv_obj_set_style_image_recolor(targetIcon[i], color, 0);
        lv_obj_clear_flag(targetIcon[i], LV_OBJ_FLAG_HIDDEN);

        // Spatial-referencing lock ring around the primary target only.
        if (i == radar.primaryIdx) {
            int ringD = (apparentW > apparentH ? apparentW : apparentH) + 18;
            lv_obj_set_size(primaryRing, ringD, ringD);
            lv_obj_set_pos(primaryRing, screenX - ringD / 2, screenY - ringD / 2);
            lv_obj_clear_flag(primaryRing, LV_OBJ_FLAG_HIDDEN);
            primaryRingPlaced = true;
        }

        // Per-target label content follows the Display settings toggles.
        char buf[48];
        size_t used = 0;
        if (cfg.showId) {
            used += snprintf(buf + used, sizeof(buf) - used, "#%d%s", i + 1, i == radar.primaryIdx ? "P" : "");
        }
        if (cfg.showSpeed) {
            used += snprintf(buf + used, sizeof(buf) - used, "%s%.0fkm/h", used ? " " : "",
                              (double)(t.closingSpeedMps * 3.6f));
        }
        if (cfg.showTtc) {
            if (isinf(t.ttcS)) used += snprintf(buf + used, sizeof(buf) - used, "%sTTC-", used ? " " : "");
            else used += snprintf(buf + used, sizeof(buf) - used, "%sTTC%.1f", used ? " " : "", (double)t.ttcS);
        }
        if (cfg.showAngle) {
            used += snprintf(buf + used, sizeof(buf) - used, "%s%.0fdeg", used ? " " : "", (double)t.angleDeg);
        }
        if (used == 0) buf[0] = '\0';

        if (buf[0]) {
            lv_label_set_text(targetLabels[i], buf);
            lv_obj_set_pos(targetLabels[i], screenX + apparentW / 2 + 3, screenY - 6);
            lv_obj_clear_flag(targetLabels[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(targetLabels[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (!primaryRingPlaced) lv_obj_add_flag(primaryRing, LV_OBJ_FLAG_HIDDEN);

    // Distance-to-primary-target readout at the top of the middle column.
    // Tailgating (tooClose) turns it red on top of its normal themed color —
    // same theme-independent-override pattern as speedLabel's own speeding
    // check above (see applyTheme()'s comment on currentPrimaryTextColor);
    // deliberately not folded into riskColor()/the TTC flash overlay, since
    // tooClose can be true while ttcS itself is still INFINITY (stop-and-go
    // traffic, not closing) — a fundamentally different risk than TTC's.
    bool primaryTooCloseNow = radar.primaryIdx >= 0 && radar.targets[radar.primaryIdx].tooClose;
    // Legal minimum following distance (see legalMinFollowDistanceM()'s own
    // comment) — uses the same real-or-demo ego speed the demo-mode
    // overlay above already computed, so this preview works in demo mode
    // too instead of needing a real >=60km/h GNSS fix to ever see it.
    float effectiveEgoSpeedKmh = demoNoFix ? demoSpeedKmh : gnss.egoSpeedKmh;
    float legalM = legalMinFollowDistanceM(effectiveEgoSpeedKmh);
    // AN TOAN / KHONG DAT verdict (feature-requested 2026-09-21, requirement
    // #4) — purely a DISPLAY comparison of the real radar distance against
    // the legal figure above; deliberately NOT fed into radar.audioAllowed
    // or the TTC flash overlay (requirement: "khong dung khoang cach phap
    // ly de thay the TTC" — this and the collision-risk engine stay two
    // separate signals). Only meaningful when the law actually fixes a
    // number for the current speed (legalM >= 0) and a real target exists.
    bool legalDistanceKnown = legalM >= 0 && radar.primaryIdx >= 0;
    bool legalDistanceFail = legalDistanceKnown && radar.targets[radar.primaryIdx].distanceM < legalM;
    if (radar.primaryIdx >= 0) {
        char buf[32];
        if (cfg.simpleUiMode || legalM < 0) {
            // Simple layout shows the legal figure (and verdict) in its own
            // separate caption below instead (see legalDistCaption) — keep
            // this label a single clean number there. Also plain when no
            // legal figure applies at all (speed < 60 km/h — the law
            // itself doesn't fix one, nothing to append).
            snprintf(buf, sizeof(buf), "%.0f m", (double)radar.targets[radar.primaryIdx].distanceM);
        } else {
            // No room for the full "OK"/"FAIL" word here too (this string
            // is already close to this column's real width at font 24) —
            // the color below (shared with tooClose's own red) is Full
            // layout's verdict signal; Simple layout's roomier caption
            // spells the word out.
            snprintf(buf, sizeof(buf), "%.0f m (law >=%.0fm)", (double)radar.targets[radar.primaryIdx].distanceM,
                      (double)legalM);
        }
        lv_label_set_text(primaryDistLabel, buf);
    } else {
        lv_label_set_text(primaryDistLabel, "-- m");
    }
    lv_obj_set_style_text_color(primaryDistLabel,
                                  (primaryTooCloseNow || legalDistanceFail) ? lv_color_hex(0xFF3B30) : currentPrimaryTextColor,
                                  0);

    if (cfg.simpleUiMode) {
        if (legalM >= 0) {
            char lbuf[32];
            snprintf(lbuf, sizeof(lbuf), "Law min: %.0f m (%s)", (double)legalM,
                      legalDistanceKnown ? (legalDistanceFail ? "KHONG DAT" : "AN TOAN") : "--");
            lv_label_set_text(legalDistCaption, lbuf);
            lv_obj_set_style_text_color(legalDistCaption, legalDistanceFail ? lv_color_hex(0xFF3B30) : currentPrimaryTextColor, 0);
            lv_obj_clear_flag(legalDistCaption, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(legalDistCaption, LV_OBJ_FLAG_HIDDEN);
        }
    }

    // --- Right column: warning panel ---
    lv_color_t rc = (radar.primaryIdx >= 0) ? riskColor(radar.targets[radar.primaryIdx].ttcS) : lv_color_hex(0x33CC66);
    // SAFE is deliberately not shown at all (user-requested 2026-09-21, "bo
    // luon phan ky hieu canh bao Safe tren man hinh") — the whole point of
    // a warning icon/word is to draw the eye to an actual risk; showing it
    // for "nothing's wrong" every single tick (the common case) is exactly
    // the clutter Simple layout's own "tang tap trung" reasoning already
    // argues against. CAUTION/WARNING/CRITICAL still show as before.
    bool showWarning = radar.primaryIdx >= 0 && radar.targets[radar.primaryIdx].ttcS <= 5.0f;
    if (showWarning) {
        lv_label_set_text(riskWordLabel, riskLabel(radar.targets[radar.primaryIdx].ttcS));
        lv_obj_set_style_text_color(riskWordLabel, rc, 0);
        lv_obj_clear_flag(warningIconObj, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(riskWordLabel, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(warningIconObj, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(riskWordLabel, LV_OBJ_FLAG_HIDDEN);
    }

    if (radar.primaryIdx >= 0) {
        SimTarget &p = radar.targets[radar.primaryIdx];
        char buf[16];
        if (isinf(p.ttcS)) snprintf(buf, sizeof(buf), "--");
        else snprintf(buf, sizeof(buf), "%.1fs", (double)p.ttcS);
        lv_label_set_text(ttcValueLabel, buf);
    } else {
        lv_label_set_text(ttcValueLabel, "--");
    }
    lv_obj_set_style_text_color(ttcValueLabel, rc, 0);

    // --- Bottom info bar ---
    // Lane count and mode are display-only placeholders: no lane-corridor
    // model exists yet (Phase 5) and no Day/Night engine exists yet (Phase
    // 9), so neither is a real, settable AppConfig field — see
    // core/AppConfig.h's own note on the same trim rule. Plain ASCII " | "
    // separators, not a middle-dot: the compiled Montserrat font is missing
    // that glyph and LVGL logs (blocking Serial.printf, with
    // LV_LOG_PRINTF=1) every time it tries to draw one — confirmed on real
    // hardware 2026-09-14 in the previous layout's bottom bar.
    char infoBuf[48];
    snprintf(infoBuf, sizeof(infoBuf), "3 lanes | %d targets | AUTO", activeCount);
    lv_label_set_text(bottomInfoLabel, infoBuf);
}

// Only refresh while the Dashboard is the screen actually on-screen — this
// used to run unconditionally every 150ms even while Settings was open,
// burning CPU on a screen nobody could see (2 mutex reads, ~5 targets'
// worth of trig + lv_image_set_scale/recolor/pos calls, ~20 more
// lv_obj_set_style_*/label calls for the top/left/right columns — all for
// nothing). User-reported 2026-09-15 as Settings feeling laggy; this was
// competing with Settings' own touch handling for the same Core 1 loop().
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
