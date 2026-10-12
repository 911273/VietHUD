#pragma once
// Names the Lite screen code expects from the stand-alone firmware's
// WiFiOTAManager. On the platform, Wi-Fi is net/WebPortal and OTA is
// net/FwUpdater; LiteUi.cpp drives the Wi-Fi/OTA page with these states.
#include "core/Version.h"

#define FW_VERSION VIETHUD_FW_VERSION
#define FW_BUILD __DATE__
#define PI4_LAN_IP "192.168.1.65"       // shown on the Wi-Fi/OTA page (same routes as net/FwUpdater.cpp)
#define PI4_TAILSCALE_IP "100.107.34.92"

enum OTAWorkflowState : uint8_t {
    OTA_STEP_IDLE = 0,
    OTA_STEP_CONNECTING_WIFI, // 1
    OTA_STEP_WIFI_CONNECTED,  // 2
    OTA_STEP_CHECKING_UPDATE, // 3
    OTA_STEP_DOWNLOADING,     // 4
    OTA_STEP_UP_TO_DATE,      // 5
    OTA_STEP_FAILED,          // 6
    OTA_STEP_REBOOTING,       // 7
};
