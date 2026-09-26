#include "NvsStore.h"
#include <Preferences.h>
#include <string.h> // strncpy() — see loadConfigFromNVS()'s wifiSsid/wifiPassword copy
#include <stdio.h>  // snprintf() — per-slot NVS keys for the saved-network list

static Preferences prefs;

void loadConfigFromNVS(AppConfig &cfg) {
    // Read-write (not read-only) so the "radarcar" namespace is created on
    // first boot instead of logging a harmless but noisy NOT_FOUND error.
    // Namespace name kept as "radarcar" (not renamed to "viethud") so an
    // existing device's saved settings (brightness, WiFi credentials, etc.)
    // aren't silently reset by this product rename — NVS namespaces aren't
    // user-visible anywhere, so there's no cosmetic reason to churn it.
    prefs.begin("radarcar", false);
    cfg.brightness = prefs.getFloat("bright", cfg.brightness);
    cfg.autoDimMin = prefs.getFloat("autoDim", cfg.autoDimMin);
    cfg.audioEnabled = prefs.getBool("audioEn", cfg.audioEnabled);
    cfg.audioVolume = prefs.getFloat("audioVol", cfg.audioVolume);
    cfg.gnssSpeedFilterAlpha = prefs.getFloat("gnssAlpha", cfg.gnssSpeedFilterAlpha);
    cfg.gnssFixTimeoutS = prefs.getFloat("gnssFixTo", cfg.gnssFixTimeoutS);
    cfg.gnssSpeedCalibrationPct = prefs.getFloat("gnssCalPct", cfg.gnssSpeedCalibrationPct);
    cfg.overspeedOffsetKmh = prefs.getFloat("overspeedOff", cfg.overspeedOffsetKmh);
    cfg.aheadLimitWarnDistM = prefs.getFloat("aheadWarnM", cfg.aheadLimitWarnDistM);
    cfg.cameraWarnDistM = prefs.getFloat("camWarnM", cfg.cameraWarnDistM);
    cfg.tripLoggingEnabled = prefs.getBool("tripLogEn", cfg.tripLoggingEnabled);
    // getString(), not getFloat() — see AppConfig.h's wifiSsid/wifiPassword
    // comment. Copied into the fixed buffers rather than kept as a String:
    // AppConfig's own fields are plain char arrays (same reasoning as every
    // other field here being a plain float/bool, not a heap-backed type).
    String ssid = prefs.getString("wifiSsid", cfg.wifiSsid);
    strncpy(cfg.wifiSsid, ssid.c_str(), sizeof(cfg.wifiSsid) - 1);
    cfg.wifiSsid[sizeof(cfg.wifiSsid) - 1] = '\0';
    String pass = prefs.getString("wifiPass", cfg.wifiPassword);
    strncpy(cfg.wifiPassword, pass.c_str(), sizeof(cfg.wifiPassword) - 1);
    cfg.wifiPassword[sizeof(cfg.wifiPassword) - 1] = '\0';
    String sta = prefs.getString("staSsid", cfg.staSsid);
    strncpy(cfg.staSsid, sta.c_str(), sizeof(cfg.staSsid) - 1);
    cfg.staSsid[sizeof(cfg.staSsid) - 1] = '\0';
    String stap = prefs.getString("staPass", cfg.staPassword);
    strncpy(cfg.staPassword, stap.c_str(), sizeof(cfg.staPassword) - 1);
    cfg.staPassword[sizeof(cfg.staPassword) - 1] = '\0';

    // WiFi Manager saved networks (2026-09-26).
    cfg.savedNetworkCount = prefs.getInt("netCount", 0);
    for (int i = 0; i < AppConfig::kMaxSavedNetworks; i++) {
        char ks[12], kp[12];
        snprintf(ks, sizeof(ks), "netS%d", i);
        snprintf(kp, sizeof(kp), "netP%d", i);
        String s = prefs.getString(ks, "");
        String p = prefs.getString(kp, "");
        strncpy(cfg.savedNetworks[i].ssid, s.c_str(), sizeof(cfg.savedNetworks[i].ssid) - 1);
        cfg.savedNetworks[i].ssid[sizeof(cfg.savedNetworks[i].ssid) - 1] = '\0';
        strncpy(cfg.savedNetworks[i].password, p.c_str(), sizeof(cfg.savedNetworks[i].password) - 1);
        cfg.savedNetworks[i].password[sizeof(cfg.savedNetworks[i].password) - 1] = '\0';
    }
    // Migrate a pre-WiFi-Manager single STA credential into slot 0 once, so an
    // existing device's saved network isn't lost by the upgrade.
    if (cfg.savedNetworkCount <= 0 && cfg.staSsid[0]) {
        strncpy(cfg.savedNetworks[0].ssid, cfg.staSsid, sizeof(cfg.savedNetworks[0].ssid) - 1);
        cfg.savedNetworks[0].ssid[sizeof(cfg.savedNetworks[0].ssid) - 1] = '\0';
        strncpy(cfg.savedNetworks[0].password, cfg.staPassword, sizeof(cfg.savedNetworks[0].password) - 1);
        cfg.savedNetworks[0].password[sizeof(cfg.savedNetworks[0].password) - 1] = '\0';
        cfg.savedNetworkCount = 1;
    }

    cfg.wifiAutoOffMin = prefs.getFloat("wifiAutoOff", cfg.wifiAutoOffMin);
    String durl = prefs.getString("dataUrl", cfg.dataUpdateUrl);
    strncpy(cfg.dataUpdateUrl, durl.c_str(), sizeof(cfg.dataUpdateUrl) - 1);
    cfg.dataUpdateUrl[sizeof(cfg.dataUpdateUrl) - 1] = '\0';
    cfg.screenRotation = prefs.getFloat("screenRot", cfg.screenRotation);
    cfg.themeMode = prefs.getFloat("themeMode", cfg.themeMode);
    cfg.mapSource = prefs.getFloat("mapSource", cfg.mapSource);
    cfg.showVectorRoads = prefs.getBool("showVecRoads", cfg.showVectorRoads);
    cfg.showVehicleTrail = prefs.getBool("showTrail", cfg.showVehicleTrail);
    cfg.showRasterMap = prefs.getBool("showRaster", cfg.showRasterMap);
    cfg.mapHeadingUp = prefs.getBool("mapHeadUp", cfg.mapHeadingUp);
    cfg.defaultLimitKmh = prefs.getFloat("defLimitKmh", cfg.defaultLimitKmh);

    // One-time config migration (2026-09-25). Older builds shipped a 90 km/h
    // fallback and persisted it, so devices carry a stale defLimitKmh=90 in NVS
    // that overrides the current 50 km/h code default. The user wants 50 shown
    // when a road's limit is unknown (VN urban baseline). Reset it to the code
    // default exactly ONCE (guarded by a schema-version key) so a later manual
    // change the user makes is still respected and never re-clobbered.
    const uint32_t kCfgSchemaVer = 2;
    uint32_t cfgVer = prefs.getUInt("cfgVer", 0);
    if (cfgVer < kCfgSchemaVer) {
        cfg.defaultLimitKmh = 50.0f;
        prefs.putFloat("defLimitKmh", 50.0f);
        prefs.putUInt("cfgVer", kCfgSchemaVer);
    }
    prefs.end();
    sanitizeConfig(cfg); // NaN/Inf guard against a corrupted flash page — must run BEFORE clamping, see its comment
    clampConfig(cfg);
}

void saveConfigToNVS(const AppConfig &cfg) {
    prefs.begin("radarcar", false);
    prefs.putFloat("bright", cfg.brightness);
    prefs.putFloat("autoDim", cfg.autoDimMin);
    prefs.putBool("audioEn", cfg.audioEnabled);
    prefs.putFloat("audioVol", cfg.audioVolume);
    prefs.putFloat("gnssAlpha", cfg.gnssSpeedFilterAlpha);
    prefs.putFloat("gnssFixTo", cfg.gnssFixTimeoutS);
    prefs.putFloat("gnssCalPct", cfg.gnssSpeedCalibrationPct);
    prefs.putFloat("overspeedOff", cfg.overspeedOffsetKmh);
    prefs.putFloat("aheadWarnM", cfg.aheadLimitWarnDistM);
    prefs.putFloat("camWarnM", cfg.cameraWarnDistM);
    prefs.putBool("tripLogEn", cfg.tripLoggingEnabled);
    prefs.putString("wifiSsid", cfg.wifiSsid);
    prefs.putString("wifiPass", cfg.wifiPassword);
    prefs.putString("staSsid", cfg.staSsid);
    prefs.putString("staPass", cfg.staPassword);
    prefs.putInt("netCount", cfg.savedNetworkCount);
    for (int i = 0; i < AppConfig::kMaxSavedNetworks; i++) {
        char ks[12], kp[12];
        snprintf(ks, sizeof(ks), "netS%d", i);
        snprintf(kp, sizeof(kp), "netP%d", i);
        prefs.putString(ks, cfg.savedNetworks[i].ssid);
        prefs.putString(kp, cfg.savedNetworks[i].password);
    }
    prefs.putFloat("wifiAutoOff", cfg.wifiAutoOffMin);
    prefs.putString("dataUrl", cfg.dataUpdateUrl);
    prefs.putFloat("screenRot", cfg.screenRotation);
    prefs.putFloat("themeMode", cfg.themeMode);
    prefs.putFloat("mapSource", cfg.mapSource);
    prefs.putBool("showVecRoads", cfg.showVectorRoads);
    prefs.putBool("showTrail", cfg.showVehicleTrail);
    prefs.putBool("showRaster", cfg.showRasterMap);
    prefs.putBool("mapHeadUp", cfg.mapHeadingUp);
    prefs.putFloat("defLimitKmh", cfg.defaultLimitKmh);
    prefs.end();
}
