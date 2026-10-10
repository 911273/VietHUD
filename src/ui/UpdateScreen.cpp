#include "UpdateScreen.h"
#include "Dashboard.h"
#include "core/AppConfig.h"
#include "core/Version.h"
#include "net/DataUpdater.h"
#include "net/FwUpdater.h"
#include "net/WebPortal.h"
#include "display/DisplayDriver.h"
#include "core/SharedState.h" // gnssSnapshot() — only prompt while stopped
#include "map/SdCardManager.h" // sdMgrIsAvailable() — no data prompt without a mounted card
#include <lvgl.h>
#include <WiFi.h>

static lv_obj_t *updateScreen = nullptr;
static lv_obj_t *wifiStatusLbl = nullptr;
static lv_obj_t *dataLocalLbl = nullptr;
static lv_obj_t *dataRemoteLbl = nullptr;
static lv_obj_t *dataUpdateBtn = nullptr;
static lv_obj_t *dataBtnLbl = nullptr;

static lv_obj_t *fwLocalLbl = nullptr;
static lv_obj_t *fwRemoteLbl = nullptr;
static lv_obj_t *fwSourceLbl = nullptr;
static lv_obj_t *fwUpdateBtn = nullptr;
static lv_obj_t *fwBtnLbl = nullptr;

static lv_obj_t *progressBar = nullptr;
static lv_obj_t *statusLogLbl = nullptr;

static bool s_isOpen = false;
static bool s_checkingFw = false;
static bool s_updatingFw = false;
static FwUpdateInfo s_fwInfo;
static volatile int s_fwProgress = 0;
static char s_fwStatusMsg[80] = "";
static volatile bool s_fwCheckedSinceLink = false; // a firmware check finished on the current Wi-Fi link

static void fwCheckTask(void *) {
    s_checkingFw = true;
    fwUpdaterCheck(s_fwInfo);
    Serial.printf("[fwupd] check done: installed %s, server %s%s\n", s_fwInfo.currentVersion,
                  s_fwInfo.remoteVersion[0] ? s_fwInfo.remoteVersion : "(unreachable)",
                  s_fwInfo.hasUpdate ? " -> UPDATE AVAILABLE" : "");
    s_fwCheckedSinceLink = true;
    s_checkingFw = false;
    vTaskDelete(NULL);
}

static void fwOtaTask(void *) {
    s_updatingFw = true;
    fwUpdaterStart(s_fwInfo.downloadUrl, [](int pct, const char *msg) {
        s_fwProgress = pct;
        if (msg) {
            strncpy(s_fwStatusMsg, msg, sizeof(s_fwStatusMsg) - 1);
            s_fwStatusMsg[sizeof(s_fwStatusMsg) - 1] = '\0';
        }
    });
    s_updatingFw = false;
    vTaskDelete(NULL);
}

static void onBackClicked(lv_event_t *) {
    closeUpdateScreen();
}

static void onDataUpdateClicked(lv_event_t *) {
    if (dataUpdateAvailable()) {
        lv_label_set_text(statusLogLbl, "Restarting into data update mode...");
        dataUpdateSchedule();
    } else {
        dataUpdateCheckStart();
        lv_label_set_text(statusLogLbl, "Re-checking data update...");
    }
}

// Flags are set BEFORE the task is created so a second caller in the same
// tick (auto-check + a button press, or the confirm prompt + openUpdateScreen's
// own auto-check) can't start a duplicate.
static void startFwCheck() {
    if (s_checkingFw || s_updatingFw) return;
    s_checkingFw = true;
    xTaskCreatePinnedToCore(fwCheckTask, "fwCheckTask", 6144, NULL, 1, NULL, 0);
}

static bool startFwOta() {
    if (!s_fwInfo.hasUpdate || s_updatingFw || !s_fwInfo.downloadUrl[0]) return false;
    s_updatingFw = true;
    s_fwProgress = 0;
    strncpy(s_fwStatusMsg, "Starting firmware OTA update...", sizeof(s_fwStatusMsg) - 1);
    xTaskCreatePinnedToCore(fwOtaTask, "fwOtaTask", 8192, NULL, 2, NULL, 0);
    return true;
}

static void onFwUpdateClicked(lv_event_t *) {
    if (startFwOta()) {
        lv_obj_clear_flag(progressBar, LV_OBJ_FLAG_HIDDEN);
        lv_bar_set_value(progressBar, 0, LV_ANIM_OFF);
        lv_label_set_text(statusLogLbl, "Starting firmware OTA update...");
    } else if (!s_checkingFw && !s_updatingFw) {
        lv_label_set_text(statusLogLbl, "Checking for latest firmware...");
        startFwCheck();
    }
}

void buildUpdateScreen() {
    if (updateScreen) return;
    int scrW = gfx->width(), scrH = gfx->height();

    updateScreen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(updateScreen, lv_color_hex(0x0A0F18), 0);
    lv_obj_set_style_pad_all(updateScreen, 0, 0);
    lv_obj_clear_flag(updateScreen, LV_OBJ_FLAG_SCROLLABLE);

    // Header (height 32)
    lv_obj_t *header = lv_obj_create(updateScreen);
    lv_obj_set_pos(header, 0, 0);
    lv_obj_set_size(header, scrW, 32);
    lv_obj_set_style_bg_color(header, lv_color_hex(0x131C28), 0);
    lv_obj_set_style_border_width(header, 0, 0);
    lv_obj_set_style_radius(header, 0, 0);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(header);
    lv_label_set_text(title, LV_SYMBOL_REFRESH "  System Update");
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 10, 0);

    lv_obj_t *backBtn = lv_button_create(header);
    lv_obj_set_size(backBtn, 86, 24);
    lv_obj_align(backBtn, LV_ALIGN_RIGHT_MID, -6, 0);
    lv_obj_set_style_bg_color(backBtn, lv_color_hex(0x28384C), 0);
    lv_obj_add_event_cb(backBtn, onBackClicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *backLbl = lv_label_create(backBtn);
    lv_label_set_text(backLbl, LV_SYMBOL_LEFT " Back");
    lv_obj_center(backLbl);

    // WiFi status banner
    wifiStatusLbl = lv_label_create(updateScreen);
    lv_obj_set_pos(wifiStatusLbl, 12, 38);
    lv_obj_set_style_text_color(wifiStatusLbl, lv_color_hex(0x3FCAD6), 0);
    lv_label_set_text(wifiStatusLbl, LV_SYMBOL_WIFI " Connecting to Wi-Fi...");
    lv_obj_set_width(wifiStatusLbl, scrW - 24);
    lv_label_set_long_mode(wifiStatusLbl, LV_LABEL_LONG_DOT);

    // Two cards. Wide screens (3.5", 480px): side by side, text above a full-
    // width button. 320px-wide screens (VietHUD 2.8): side-by-side cards were
    // only 144px wide and clipped "Installed: 2026.10.10.0200" / "(Latest)" and
    // let the button cover the Source line — so there the cards are stacked
    // full-width rows (title + 2 lines on the left, a short button on the right).
    const bool compact = scrW <= 320;
    int cardW = compact ? scrW - 16 : (scrW - 32) / 2;
    int cardY = compact ? 56 : 60;
    int cardH = compact ? 66 : (scrH - cardY - 46 < 175 ? scrH - cardY - 46 : 175);
    int btnH = compact ? 34 : (cardH < 175 ? 28 : 36);
    int btnW = compact ? 92 : cardW - 20;
    int lineDy = compact ? 17 : 24;
    int textW = compact ? cardW - 12 - btnW - 6 : cardW - 20;
    auto styleCard = [&](lv_obj_t *c) {
        lv_obj_set_style_bg_color(c, lv_color_hex(0x162232), 0);
        lv_obj_set_style_border_color(c, lv_color_hex(0x25384E), 0);
        lv_obj_set_style_border_width(c, 1, 0);
        lv_obj_set_style_radius(c, 6, 0);
        if (compact) lv_obj_set_style_pad_all(c, 6, 0);
        lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    };
    auto cardLine = [&](lv_obj_t *card, int row, uint32_t color) {
        lv_obj_t *l = lv_label_create(card);
        lv_obj_set_pos(l, 0, row * lineDy);
        lv_obj_set_width(l, textW);
        lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
        return l;
    };
    auto placeBtn = [&](lv_obj_t *b) {
        lv_obj_set_size(b, btnW, btnH);
        if (compact) lv_obj_align(b, LV_ALIGN_RIGHT_MID, 0, 0);
        else lv_obj_align(b, LV_ALIGN_BOTTOM_MID, 0, -2);
    };

    // Card 1: SpeedMap Data
    lv_obj_t *cardData = lv_obj_create(updateScreen);
    lv_obj_set_pos(cardData, compact ? 8 : 10, cardY);
    lv_obj_set_size(cardData, cardW, cardH);
    styleCard(cardData);
    lv_obj_t *dataTitle = cardLine(cardData, 0, 0x4AA3FF);
    lv_label_set_text(dataTitle, LV_SYMBOL_FILE " SpeedMap Data");

    dataLocalLbl = cardLine(cardData, 1, 0xCCD6E0);
    lv_label_set_text(dataLocalLbl, "Installed: --");

    dataRemoteLbl = cardLine(cardData, 2, 0xCCD6E0);
    lv_label_set_text(dataRemoteLbl, "Server: Checking...");

    dataUpdateBtn = lv_button_create(cardData);
    placeBtn(dataUpdateBtn);
    lv_obj_set_style_bg_color(dataUpdateBtn, lv_color_hex(0x2E6B9E), 0);
    lv_obj_add_event_cb(dataUpdateBtn, onDataUpdateClicked, LV_EVENT_CLICKED, NULL);
    dataBtnLbl = lv_label_create(dataUpdateBtn);
    lv_label_set_text(dataBtnLbl, compact ? "Update" : "Update Data");
    lv_obj_center(dataBtnLbl);

    // Card 2: Firmware OTA
    lv_obj_t *cardFw = lv_obj_create(updateScreen);
    if (compact) lv_obj_set_pos(cardFw, 8, cardY + cardH + 6);
    else lv_obj_set_pos(cardFw, 10 + cardW + 12, cardY);
    lv_obj_set_size(cardFw, cardW, cardH);
    styleCard(cardFw);

    lv_obj_t *fwTitle = cardLine(cardFw, 0, 0x34C46A);
    lv_label_set_text(fwTitle, LV_SYMBOL_SETTINGS " Firmware (OTA)");

    fwLocalLbl = cardLine(cardFw, 1, 0xCCD6E0);
    char curFwBuf[32];
    snprintf(curFwBuf, sizeof(curFwBuf), "Installed: v%s", VIETHUD_FW_VERSION);
    lv_label_set_text(fwLocalLbl, curFwBuf);

    fwRemoteLbl = cardLine(cardFw, 2, 0xCCD6E0);
    lv_label_set_text(fwRemoteLbl, "Server: Checking...");

    fwSourceLbl = cardLine(cardFw, 3, 0x8FA0B4);
    lv_label_set_text(fwSourceLbl, "Source: Pi 4 / GitHub");
    if (compact) lv_obj_add_flag(fwSourceLbl, LV_OBJ_FLAG_HIDDEN); // no 4th line in a 66px row

    fwUpdateBtn = lv_button_create(cardFw);
    placeBtn(fwUpdateBtn);
    lv_obj_set_style_bg_color(fwUpdateBtn, lv_color_hex(0x2A7E4A), 0);
    lv_obj_add_event_cb(fwUpdateBtn, onFwUpdateClicked, LV_EVENT_CLICKED, NULL);
    fwBtnLbl = lv_label_create(fwUpdateBtn);
    lv_label_set_text(fwBtnLbl, compact ? "Update" : "Update Firmware");
    lv_obj_center(fwBtnLbl);

    // Progress bar + Status log at bottom
    progressBar = lv_bar_create(updateScreen);
    lv_obj_set_pos(progressBar, 10, compact ? scrH - 38 : scrH - 42);
    lv_obj_set_size(progressBar, scrW - 20, 10);
    lv_obj_set_style_bg_color(progressBar, lv_color_hex(0x1B2838), LV_PART_MAIN);
    lv_obj_set_style_bg_color(progressBar, lv_color_hex(0x34C46A), LV_PART_INDICATOR);
    lv_bar_set_range(progressBar, 0, 100);
    lv_obj_add_flag(progressBar, LV_OBJ_FLAG_HIDDEN);

    statusLogLbl = lv_label_create(updateScreen);
    lv_obj_set_pos(statusLogLbl, 12, scrH - 26);
    lv_obj_set_style_text_color(statusLogLbl, lv_color_hex(0x8FA0B4), 0);
    lv_label_set_text(statusLogLbl, "Ready.");
}

void openUpdateScreen() {
    buildUpdateScreen();
    s_isOpen = true;
    lv_screen_load(updateScreen);

    // Auto-trigger background checks
    if (WiFi.status() == WL_CONNECTED) {
        dataUpdateCheckStart();
        startFwCheck();
    }
}

void closeUpdateScreen() {
    s_isOpen = false;
    lv_screen_load(dashboardScreen);
}

bool isUpdateScreenOpen() {
    return s_isOpen;
}

void updateScreenPoll() {
    if (!s_isOpen) return;

    // Refresh Wi-Fi banner
    if (WiFi.status() == WL_CONNECTED) {
        char wb[80];
        snprintf(wb, sizeof(wb), LV_SYMBOL_WIFI " Connected: \"%s\" (IP: %s)",
                 cfg.staSsid, WiFi.localIP().toString().c_str());
        lv_label_set_text(wifiStatusLbl, wb);
        lv_obj_set_style_text_color(wifiStatusLbl, lv_color_hex(0x34C46A), 0);
    } else if (!webPortalRequestedOn()) {
        lv_label_set_text(wifiStatusLbl, LV_SYMBOL_WARNING " Wi-Fi is off (Settings > WiFi)");
        lv_obj_set_style_text_color(wifiStatusLbl, lv_color_hex(0xE5B53A), 0);
    } else {
        lv_label_set_text(wifiStatusLbl, LV_SYMBOL_WARNING " Wi-Fi connecting...");
        lv_obj_set_style_text_color(wifiStatusLbl, lv_color_hex(0xE5B53A), 0);
    }

    // Refresh Data section
    const char *locData = dataUpdateLocalVersion();
    char ldb[40];
    snprintf(ldb, sizeof(ldb), "Installed: %s", locData[0] ? locData : "(none)");
    lv_label_set_text(dataLocalLbl, ldb);

    if (dataUpdateCheckInProgress()) {
        lv_label_set_text(dataRemoteLbl, "Server: Checking...");
        lv_obj_set_style_text_color(dataRemoteLbl, lv_color_hex(0xCCD6E0), 0);
    } else if (dataUpdateAvailable()) {
        char rdb[40];
        snprintf(rdb, sizeof(rdb), "Server: %s (NEW)", dataUpdateRemoteVersion());
        lv_label_set_text(dataRemoteLbl, rdb);
        lv_obj_set_style_text_color(dataRemoteLbl, lv_color_hex(0xE5B53A), 0);
        lv_obj_set_style_bg_color(dataUpdateBtn, lv_color_hex(0x3DA5FF), 0);
    } else {
        lv_label_set_text(dataRemoteLbl, "Server: Up to date");
        lv_obj_set_style_text_color(dataRemoteLbl, lv_color_hex(0x34C46A), 0);
        lv_obj_set_style_bg_color(dataUpdateBtn, lv_color_hex(0x28384C), 0);
    }

    // Refresh Firmware section
    if (s_checkingFw) {
        lv_label_set_text(fwRemoteLbl, "Server: Checking...");
        lv_obj_set_style_text_color(fwRemoteLbl, lv_color_hex(0xCCD6E0), 0);
    } else if (s_fwInfo.hasUpdate) {
        char rfb[40];
        snprintf(rfb, sizeof(rfb), "Server: v%s (NEW)", s_fwInfo.remoteVersion);
        lv_label_set_text(fwRemoteLbl, rfb);
        lv_obj_set_style_text_color(fwRemoteLbl, lv_color_hex(0xE5B53A), 0);
        char srcb[40];
        snprintf(srcb, sizeof(srcb), "Source: %s", s_fwInfo.sourceName);
        lv_label_set_text(fwSourceLbl, srcb);
        lv_obj_set_style_bg_color(fwUpdateBtn, lv_color_hex(0x34C46A), 0);
    } else if (s_fwInfo.remoteVersion[0]) {
        char rfb[40];
        snprintf(rfb, sizeof(rfb), "Server: v%s (Latest)", s_fwInfo.remoteVersion);
        lv_label_set_text(fwRemoteLbl, rfb);
        lv_obj_set_style_text_color(fwRemoteLbl, lv_color_hex(0x34C46A), 0);
        char srcb[40];
        snprintf(srcb, sizeof(srcb), "Source: %s", s_fwInfo.sourceName);
        lv_label_set_text(fwSourceLbl, srcb);
        lv_obj_set_style_bg_color(fwUpdateBtn, lv_color_hex(0x28384C), 0);
    }

    // OTA in progress
    if (s_updatingFw) {
        lv_obj_clear_flag(progressBar, LV_OBJ_FLAG_HIDDEN);
        lv_bar_set_value(progressBar, s_fwProgress, LV_ANIM_OFF);
        if (s_fwStatusMsg[0]) {
            lv_label_set_text(statusLogLbl, s_fwStatusMsg);
        }
    }
}

// ---------------------------------------------------------------------
// Automatic update check + confirmation prompt (2026-10-10, same flow as
// VietHUD Lite): once the device is on an Internet Wi-Fi it checks for new
// firmware by itself (Pi 4 LAN -> mDNS -> Tailscale -> GitHub, see
// FwUpdater.cpp), and again every kAutoRecheckMs while connected. Unlike
// Lite it never flashes on its own: a new firmware (or a new map/alert data
// set, found by WebPortal's own check) raises a Yes/Later prompt, and only
// while the car is stopped — the dashboard keeps its warnings while driving,
// and an OTA flash would silence them for a minute. "Later" mutes that
// version until the next power-on.
// ---------------------------------------------------------------------
LV_FONT_DECLARE(lv_font_vn_14);
LV_FONT_DECLARE(lv_font_vn_20);

static const uint32_t kAutoFirstDelayMs = 10000;            // let the link + DNS settle
static const uint32_t kAutoRecheckMs = 6UL * 3600UL * 1000UL;
static const float kPromptMaxKmh = 5.0f;                    // only ask while stopped
static const float kPromptDismissKmh = 15.0f;               // car pulled away: close (not declined)

enum PromptKind : uint8_t { PROMPT_NONE = 0, PROMPT_FW, PROMPT_DATA };
static PromptKind s_promptKind = PROMPT_NONE;
static lv_obj_t *s_promptLayer = nullptr;
static char s_declinedFw[16] = "";
static char s_declinedData[24] = "";

static void closePrompt() {
    if (s_promptLayer) lv_obj_delete(s_promptLayer);
    s_promptLayer = nullptr;
    s_promptKind = PROMPT_NONE;
}

static void onPromptLater(lv_event_t *) {
    if (s_promptKind == PROMPT_FW) {
        strncpy(s_declinedFw, s_fwInfo.remoteVersion, sizeof(s_declinedFw) - 1);
    } else if (s_promptKind == PROMPT_DATA) {
        strncpy(s_declinedData, dataUpdateRemoteVersion(), sizeof(s_declinedData) - 1);
    }
    Serial.println("[update] user chose: later");
    closePrompt();
}

static void onPromptUpdate(lv_event_t *) {
    PromptKind kind = s_promptKind;
    closePrompt();
    if (kind == PROMPT_FW) {
        Serial.printf("[update] user confirmed firmware v%s\n", s_fwInfo.remoteVersion);
        if (startFwOta()) openUpdateScreen(); // progress bar + log live there
    } else if (kind == PROMPT_DATA) {
        Serial.printf("[update] user confirmed data %s\n", dataUpdateRemoteVersion());
        dataUpdateSchedule(); // reboots into data-update mode
    }
}

static lv_obj_t *makePromptButton(lv_obj_t *parent, const char *text, uint32_t color, lv_event_cb_t cb) {
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_height(b, 40);
    lv_obj_set_flex_grow(b, 1);
    lv_obj_set_style_bg_color(b, lv_color_hex(color), 0);
    lv_obj_set_ext_click_area(b, 6);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l = lv_label_create(b);
    lv_obj_set_style_text_font(l, &lv_font_vn_20, 0);
    lv_label_set_text(l, text);
    lv_obj_center(l);
    return b;
}

static void showPrompt(PromptKind kind) {
    closePrompt();
    s_promptKind = kind;
    int scrW = gfx->width(), scrH = gfx->height();

    // Full-screen dimmer on the top layer: visible over any screen, and it
    // swallows touches so the dashboard's hold gestures don't fire beneath it.
    s_promptLayer = lv_obj_create(lv_layer_top());
    lv_obj_set_pos(s_promptLayer, 0, 0);
    lv_obj_set_size(s_promptLayer, scrW, scrH);
    lv_obj_set_style_bg_color(s_promptLayer, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_promptLayer, LV_OPA_60, 0);
    lv_obj_set_style_border_width(s_promptLayer, 0, 0);
    lv_obj_set_style_radius(s_promptLayer, 0, 0);
    lv_obj_add_flag(s_promptLayer, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_promptLayer, LV_OBJ_FLAG_SCROLLABLE);

    int cardW = scrW - 24 > 360 ? 360 : scrW - 24;
    lv_obj_t *card = lv_obj_create(s_promptLayer);
    lv_obj_set_width(card, cardW);
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x131C28), 0);
    lv_obj_set_style_border_color(card, lv_color_hex(0x3DA5FF), 0);
    lv_obj_set_style_border_width(card, 2, 0);
    lv_obj_set_style_radius(card, 12, 0);
    lv_obj_set_style_pad_all(card, 12, 0);
    lv_obj_set_style_pad_row(card, 10, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(card);
    lv_obj_set_style_text_font(title, &lv_font_vn_20, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0x3DA5FF), 0);
    lv_label_set_text(title, kind == PROMPT_FW ? LV_SYMBOL_DOWNLOAD " Có bản cập nhật mới"
                                                : LV_SYMBOL_DOWNLOAD " Có dữ liệu bản đồ mới");

    char body[220];
    if (kind == PROMPT_FW) {
        snprintf(body, sizeof(body), "Firmware v%s (đang dùng v%s)\n%s%sCập nhật mất khoảng 1 phút, máy sẽ tự khởi động lại.",
                 s_fwInfo.remoteVersion, s_fwInfo.currentVersion, s_fwInfo.releaseNotes,
                 s_fwInfo.releaseNotes[0] ? "\n" : "");
    } else {
        snprintf(body, sizeof(body), "Dữ liệu %s (đang dùng %s)\nMáy sẽ khởi động lại để tải và cài dữ liệu.",
                 dataUpdateRemoteVersion(), dataUpdateLocalVersion()[0] ? dataUpdateLocalVersion() : "chưa có");
    }
    lv_obj_t *msg = lv_label_create(card);
    lv_obj_set_width(msg, lv_pct(100));
    lv_label_set_long_mode(msg, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(msg, &lv_font_vn_14, 0);
    lv_obj_set_style_text_color(msg, lv_color_hex(0xE0E8F0), 0);
    lv_label_set_text(msg, body);

    lv_obj_t *row = lv_obj_create(card);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_pad_column(row, 10, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    makePromptButton(row, "Để sau", 0x3A4656, onPromptLater);
    makePromptButton(row, "Cập nhật", 0x2A9E55, onPromptUpdate);

    Serial.printf("[update] prompt shown: %s\n", kind == PROMPT_FW ? "firmware" : "data");
}

void updatePromptPoll() {
    static uint32_t sLastMs = 0;
    uint32_t now = millis();
    if (now - sLastMs < 500) return; // twice a second is plenty
    sLastMs = now;

    // 1. Auto-check firmware on every new Wi-Fi link, then periodically.
    static bool sWasSta = false;
    static uint32_t sNextCheckMs = 0;
    bool sta = WiFi.status() == WL_CONNECTED;
    if (sta && !sWasSta) {
        sNextCheckMs = now + kAutoFirstDelayMs;
        s_fwCheckedSinceLink = false;
    }
    sWasSta = sta;
    if (sta && !s_checkingFw && !s_updatingFw && (int32_t)(now - sNextCheckMs) >= 0) {
        Serial.println("[update] auto-checking firmware on the server...");
        startFwCheck();
        sNextCheckMs = now + kAutoRecheckMs;
    }

    // 2. Ask — only while stopped, never over the Update screen or a running OTA.
    GnssSnapshot g = gnssSnapshot();
    bool stopped = !g.fix || g.egoSpeedKmh < kPromptMaxKmh;
    if (s_promptKind != PROMPT_NONE) {
        if (g.fix && g.egoSpeedKmh > kPromptDismissKmh) {
            Serial.println("[update] vehicle moving -> prompt closed, will ask again when stopped");
            closePrompt();
        }
        return;
    }
    if (!stopped || s_updatingFw || s_isOpen || fwUpdaterIsRunning()) return;
    if (s_fwInfo.hasUpdate && s_fwInfo.downloadUrl[0] && strcmp(s_declinedFw, s_fwInfo.remoteVersion) != 0) {
        showPrompt(PROMPT_FW);
    } else if (!s_checkingFw && s_fwCheckedSinceLink && sdMgrIsAvailable() && !sdMgrDataOnFlash() && dataUpdateAvailable() &&
               strcmp(s_declinedData, dataUpdateRemoteVersion()) != 0) {
        // Data only AFTER this link's firmware check (firmware first), and only
        // with a mounted card: with the card missing the local data version reads
        // as empty, so every server version looks "new" — and the data-update
        // reboot would then just fail with "Cannot read the SD card" (seen
        // 2026-10-10). A blank card can still be filled from the Update screen.
        showPrompt(PROMPT_DATA);
    }
}

// Button boards (no touch): answer the update prompt from ui/ButtonInput.cpp.
bool updatePromptActive() { return s_promptKind != PROMPT_NONE; }
void updatePromptAnswer(bool update) {
    if (s_promptKind == PROMPT_NONE) return;
    if (update) onPromptUpdate(nullptr);
    else onPromptLater(nullptr);
}
