#include "UpdateScreen.h"
#include "Dashboard.h"
#include "core/AppConfig.h"
#include "core/Version.h"
#include "net/DataUpdater.h"
#include "net/FwUpdater.h"
#include "net/WebPortal.h"
#include "display/DisplayDriver.h"
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

static void fwCheckTask(void *) {
    s_checkingFw = true;
    fwUpdaterCheck(s_fwInfo);
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

static void onFwUpdateClicked(lv_event_t *) {
    if (s_fwInfo.hasUpdate && !s_updatingFw && s_fwInfo.downloadUrl[0]) {
        lv_obj_clear_flag(progressBar, LV_OBJ_FLAG_HIDDEN);
        lv_bar_set_value(progressBar, 0, LV_ANIM_OFF);
        lv_label_set_text(statusLogLbl, "Starting firmware OTA update...");
        xTaskCreatePinnedToCore(fwOtaTask, "fwOtaTask", 8192, NULL, 2, NULL, 0);
    } else if (!s_checkingFw && !s_updatingFw) {
        lv_label_set_text(statusLogLbl, "Checking for latest firmware...");
        xTaskCreatePinnedToCore(fwCheckTask, "fwCheckTask", 6144, NULL, 1, NULL, 0);
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

    // Container for 2 cards
    int cardW = (scrW - 32) / 2;
    int cardH = 175;
    int cardY = 60;

    // Card 1: SpeedMap Data
    lv_obj_t *cardData = lv_obj_create(updateScreen);
    lv_obj_set_pos(cardData, 10, cardY);
    lv_obj_set_size(cardData, cardW, cardH);
    lv_obj_set_style_bg_color(cardData, lv_color_hex(0x162232), 0);
    lv_obj_set_style_border_color(cardData, lv_color_hex(0x25384E), 0);
    lv_obj_set_style_border_width(cardData, 1, 0);
    lv_obj_set_style_radius(cardData, 6, 0);
    lv_obj_clear_flag(cardData, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *dataTitle = lv_label_create(cardData);
    lv_label_set_text(dataTitle, LV_SYMBOL_FILE " SpeedMap Data");
    lv_obj_set_style_text_color(dataTitle, lv_color_hex(0x4AA3FF), 0);
    lv_obj_set_pos(dataTitle, 0, 0);

    dataLocalLbl = lv_label_create(cardData);
    lv_obj_set_pos(dataLocalLbl, 0, 24);
    lv_obj_set_style_text_color(dataLocalLbl, lv_color_hex(0xCCD6E0), 0);
    lv_label_set_text(dataLocalLbl, "Installed: --");

    dataRemoteLbl = lv_label_create(cardData);
    lv_obj_set_pos(dataRemoteLbl, 0, 48);
    lv_obj_set_style_text_color(dataRemoteLbl, lv_color_hex(0xCCD6E0), 0);
    lv_label_set_text(dataRemoteLbl, "Server: Checking...");

    dataUpdateBtn = lv_button_create(cardData);
    lv_obj_set_size(dataUpdateBtn, cardW - 20, 36);
    lv_obj_align(dataUpdateBtn, LV_ALIGN_BOTTOM_MID, 0, -2);
    lv_obj_set_style_bg_color(dataUpdateBtn, lv_color_hex(0x2E6B9E), 0);
    lv_obj_add_event_cb(dataUpdateBtn, onDataUpdateClicked, LV_EVENT_CLICKED, NULL);
    dataBtnLbl = lv_label_create(dataUpdateBtn);
    lv_label_set_text(dataBtnLbl, "Update Data");
    lv_obj_center(dataBtnLbl);

    // Card 2: Firmware OTA
    lv_obj_t *cardFw = lv_obj_create(updateScreen);
    lv_obj_set_pos(cardFw, 10 + cardW + 12, cardY);
    lv_obj_set_size(cardFw, cardW, cardH);
    lv_obj_set_style_bg_color(cardFw, lv_color_hex(0x162232), 0);
    lv_obj_set_style_border_color(cardFw, lv_color_hex(0x25384E), 0);
    lv_obj_set_style_border_width(cardFw, 1, 0);
    lv_obj_set_style_radius(cardFw, 6, 0);
    lv_obj_clear_flag(cardFw, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *fwTitle = lv_label_create(cardFw);
    lv_label_set_text(fwTitle, LV_SYMBOL_SETTINGS " Firmware (OTA)");
    lv_obj_set_style_text_color(fwTitle, lv_color_hex(0x34C46A), 0);
    lv_obj_set_pos(fwTitle, 0, 0);

    fwLocalLbl = lv_label_create(cardFw);
    lv_obj_set_pos(fwLocalLbl, 0, 24);
    lv_obj_set_style_text_color(fwLocalLbl, lv_color_hex(0xCCD6E0), 0);
    char curFwBuf[32];
    snprintf(curFwBuf, sizeof(curFwBuf), "Installed: v%s", VIETHUD_FW_VERSION);
    lv_label_set_text(fwLocalLbl, curFwBuf);

    fwRemoteLbl = lv_label_create(cardFw);
    lv_obj_set_pos(fwRemoteLbl, 0, 48);
    lv_obj_set_style_text_color(fwRemoteLbl, lv_color_hex(0xCCD6E0), 0);
    lv_label_set_text(fwRemoteLbl, "Server: Checking...");

    fwSourceLbl = lv_label_create(cardFw);
    lv_obj_set_pos(fwSourceLbl, 0, 72);
    lv_obj_set_style_text_color(fwSourceLbl, lv_color_hex(0x8FA0B4), 0);
    lv_label_set_text(fwSourceLbl, "Source: Pi 4 / GitHub");

    fwUpdateBtn = lv_button_create(cardFw);
    lv_obj_set_size(fwUpdateBtn, cardW - 20, 36);
    lv_obj_align(fwUpdateBtn, LV_ALIGN_BOTTOM_MID, 0, -2);
    lv_obj_set_style_bg_color(fwUpdateBtn, lv_color_hex(0x2A7E4A), 0);
    lv_obj_add_event_cb(fwUpdateBtn, onFwUpdateClicked, LV_EVENT_CLICKED, NULL);
    fwBtnLbl = lv_label_create(fwUpdateBtn);
    lv_label_set_text(fwBtnLbl, "Update Firmware");
    lv_obj_center(fwBtnLbl);

    // Progress bar + Status log at bottom
    progressBar = lv_bar_create(updateScreen);
    lv_obj_set_pos(progressBar, 10, scrH - 42);
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
        if (!s_checkingFw && !s_updatingFw) {
            xTaskCreatePinnedToCore(fwCheckTask, "fwCheckTask", 6144, NULL, 1, NULL, 0);
        }
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
