#include "NvsStore.h"
#include <Preferences.h>
#include <string.h> // strncpy() — see loadConfigFromNVS()'s wifiSsid/wifiPassword copy

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
    cfg.gnssSpeedFilterAlpha = prefs.getFloat("gnssAlpha", cfg.gnssSpeedFilterAlpha);
    cfg.gnssFixTimeoutS = prefs.getFloat("gnssFixTo", cfg.gnssFixTimeoutS);
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
    prefs.end();
    sanitizeConfig(cfg); // NaN/Inf guard against a corrupted flash page — must run BEFORE clamping, see its comment
    clampConfig(cfg);
}

void saveConfigToNVS(const AppConfig &cfg) {
    prefs.begin("radarcar", false);
    prefs.putFloat("bright", cfg.brightness);
    prefs.putFloat("autoDim", cfg.autoDimMin);
    prefs.putBool("audioEn", cfg.audioEnabled);
    prefs.putFloat("gnssAlpha", cfg.gnssSpeedFilterAlpha);
    prefs.putFloat("gnssFixTo", cfg.gnssFixTimeoutS);
    prefs.putBool("tripLogEn", cfg.tripLoggingEnabled);
    prefs.putString("wifiSsid", cfg.wifiSsid);
    prefs.putString("wifiPass", cfg.wifiPassword);
    prefs.putFloat("screenRot", cfg.screenRotation);
    prefs.putFloat("themeMode", cfg.themeMode);
    prefs.end();
}
