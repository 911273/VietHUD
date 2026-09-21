#include "NvsStore.h"
#include <Preferences.h>
#include <string.h> // strncpy() — see loadConfigFromNVS()'s wifiSsid/wifiPassword copy

static Preferences prefs;

void loadConfigFromNVS(AppConfig &cfg) {
    // Read-write (not read-only) so the "radarcar" namespace is created on
    // first boot instead of logging a harmless but noisy NOT_FOUND error.
    prefs.begin("radarcar", false);
    cfg.maxRangeM = prefs.getFloat("maxRange", cfg.maxRangeM);
    cfg.minTargetSpeedKmh = prefs.getFloat("minTgtSpd", cfg.minTargetSpeedKmh);
    cfg.maxTargets = prefs.getFloat("maxTargets", cfg.maxTargets);
    cfg.audioEnableKmh = prefs.getFloat("audioEnKmh", cfg.audioEnableKmh);
    cfg.hysteresisKmh = prefs.getFloat("hyst", cfg.hysteresisKmh);
    cfg.ttcWarnS = prefs.getFloat("ttcWarn", cfg.ttcWarnS);
    cfg.ttcCritS = prefs.getFloat("ttcCrit", cfg.ttcCritS);
    cfg.minConfidence = prefs.getFloat("minConf", cfg.minConfidence);
    cfg.brightness = prefs.getFloat("bright", cfg.brightness);
    cfg.autoDimMin = prefs.getFloat("autoDim", cfg.autoDimMin);
    cfg.showId = prefs.getBool("showId", cfg.showId);
    cfg.showSpeed = prefs.getBool("showSpeed", cfg.showSpeed);
    cfg.showTtc = prefs.getBool("showTtc", cfg.showTtc);
    cfg.showAngle = prefs.getBool("showAngle", cfg.showAngle);
    cfg.audioEnabled = prefs.getBool("audioEn", cfg.audioEnabled);
    cfg.gnssSpeedFilterAlpha = prefs.getFloat("gnssAlpha", cfg.gnssSpeedFilterAlpha);
    cfg.gnssFixTimeoutS = prefs.getFloat("gnssFixTo", cfg.gnssFixTimeoutS);
    cfg.radarDirection = prefs.getFloat("radarDir", cfg.radarDirection);
    cfg.radarSnrLevel = prefs.getFloat("radarSnr", cfg.radarSnrLevel);
    cfg.radarTriggerCount = prefs.getFloat("radarTrig", cfg.radarTriggerCount);
    cfg.radarNoTargetDelayS = prefs.getFloat("radarNoTgt", cfg.radarNoTargetDelayS);
    cfg.radarMountFlipped = prefs.getBool("radarFlip", cfg.radarMountFlipped);
    cfg.laneHalfWidthM = prefs.getFloat("laneHalfW", cfg.laneHalfWidthM);
    cfg.minDistanceM = prefs.getFloat("minDist", cfg.minDistanceM);
    cfg.harshBrakeAccelMps2 = prefs.getFloat("brakeAccel", cfg.harshBrakeAccelMps2);
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
    cfg.screenRotation = prefs.getFloat("screenRot", cfg.screenRotation);
    cfg.themeMode = prefs.getFloat("themeMode", cfg.themeMode);
    cfg.simpleUiMode = prefs.getBool("simpleUiMode", cfg.simpleUiMode);
    cfg.demoMode = prefs.getBool("demoMode", cfg.demoMode);
    prefs.end();
    sanitizeConfig(cfg); // NaN/Inf guard against a corrupted flash page — must run BEFORE clamping, see its comment
    clampConfig(cfg);
}

void saveConfigToNVS(const AppConfig &cfg) {
    prefs.begin("radarcar", false);
    prefs.putFloat("maxRange", cfg.maxRangeM);
    prefs.putFloat("minTgtSpd", cfg.minTargetSpeedKmh);
    prefs.putFloat("maxTargets", cfg.maxTargets);
    prefs.putFloat("audioEnKmh", cfg.audioEnableKmh);
    prefs.putFloat("hyst", cfg.hysteresisKmh);
    prefs.putFloat("ttcWarn", cfg.ttcWarnS);
    prefs.putFloat("ttcCrit", cfg.ttcCritS);
    prefs.putFloat("minConf", cfg.minConfidence);
    prefs.putFloat("bright", cfg.brightness);
    prefs.putFloat("autoDim", cfg.autoDimMin);
    prefs.putBool("showId", cfg.showId);
    prefs.putBool("showSpeed", cfg.showSpeed);
    prefs.putBool("showTtc", cfg.showTtc);
    prefs.putBool("showAngle", cfg.showAngle);
    prefs.putBool("audioEn", cfg.audioEnabled);
    prefs.putFloat("gnssAlpha", cfg.gnssSpeedFilterAlpha);
    prefs.putFloat("gnssFixTo", cfg.gnssFixTimeoutS);
    prefs.putFloat("radarDir", cfg.radarDirection);
    prefs.putFloat("radarSnr", cfg.radarSnrLevel);
    prefs.putFloat("radarTrig", cfg.radarTriggerCount);
    prefs.putFloat("radarNoTgt", cfg.radarNoTargetDelayS);
    prefs.putBool("radarFlip", cfg.radarMountFlipped);
    prefs.putFloat("laneHalfW", cfg.laneHalfWidthM);
    prefs.putFloat("minDist", cfg.minDistanceM);
    prefs.putFloat("brakeAccel", cfg.harshBrakeAccelMps2);
    prefs.putBool("tripLogEn", cfg.tripLoggingEnabled);
    prefs.putString("wifiSsid", cfg.wifiSsid);
    prefs.putString("wifiPass", cfg.wifiPassword);
    prefs.putFloat("screenRot", cfg.screenRotation);
    prefs.putFloat("themeMode", cfg.themeMode);
    prefs.putBool("simpleUiMode", cfg.simpleUiMode);
    prefs.putBool("demoMode", cfg.demoMode);
    prefs.end();
}
