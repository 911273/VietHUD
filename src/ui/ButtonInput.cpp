#include "ButtonInput.h"
#include "Dashboard.h"     // dashboardToast(), applyConfig(), wakeScreen()
#include "Settings.h"      // settingsSyncSwitches()
#include "UpdateScreen.h"  // prompt + Update screen
#include "core/AppConfig.h"
#include "core/Board.h"
#include "core/NvsStore.h"
#include "net/WebPortal.h"
#include "audio/AudioPlayer.h"
#include <Arduino.h>
#include <lvgl.h>

namespace {

enum Gesture : uint8_t { G_NONE = 0, G_SHORT, G_LONG, G_DOUBLE };

// Same timings as the stand-alone Lite firmware's ButtonManager.
const uint32_t kDebounceMs = 30;
const uint32_t kLongMs = 650;
const uint32_t kDoubleGapMs = 300;

struct Key {
    uint8_t mask;
    bool down = false;
    uint32_t downAt = 0, upAt = 0;
    bool longFired = false;
    bool pendingShort = false; // released once, waiting to see if a second press follows
};

Key keys[3] = {{BOARD_KEY_LEFT}, {BOARD_KEY_CENTER}, {BOARD_KEY_RIGHT}};

Gesture step(Key &k, bool pressed, uint32_t now) {
    if (pressed && !k.down) {
        if (now - k.upAt < kDebounceMs) return G_NONE;
        k.down = true;
        k.downAt = now;
        k.longFired = false;
        if (k.pendingShort && now - k.upAt <= kDoubleGapMs) {
            k.pendingShort = false;
            k.longFired = true; // swallow this press's release
            return G_DOUBLE;
        }
        return G_NONE;
    }
    if (pressed && k.down && !k.longFired && now - k.downAt >= kLongMs) {
        k.longFired = true;
        k.pendingShort = false;
        return G_LONG; // fires while still held
    }
    if (!pressed && k.down) {
        if (now - k.downAt < kDebounceMs) return G_NONE; // bounce
        k.down = false;
        k.upAt = now;
        if (!k.longFired) k.pendingShort = true;
        return G_NONE;
    }
    if (!pressed && k.pendingShort && now - k.upAt > kDoubleGapMs) {
        k.pendingShort = false;
        return G_SHORT;
    }
    return G_NONE;
}

void saveAndToast(const char *msg) {
    saveConfigToNVS(cfg);
    dashboardToast(msg, 1200);
}

void changeVolume(int delta) {
    float v = cfg.audioVolume + delta;
    cfg.audioVolume = v < 0 ? 0 : (v > 100 ? 100 : v);
    applyConfig();
    char b[40];
    snprintf(b, sizeof(b), LV_SYMBOL_VOLUME_MAX "  %.0f %%", (double)cfg.audioVolume);
    saveAndToast(b);
    audioPlayBeep(1);
}

void handle(uint8_t key, Gesture g) {
    if (g == G_NONE) return;
    wakeScreen();
    Serial.printf("[buttons] key %u gesture %u\n", key, g);

    if (updatePromptActive()) {
        if (g == G_SHORT) updatePromptAnswer(key != BOARD_KEY_LEFT);
        return;
    }
    if (isUpdateScreenOpen()) {
        if (key == BOARD_KEY_CENTER && g == G_SHORT) closeUpdateScreen();
        return;
    }
    switch (key) {
    case BOARD_KEY_LEFT:
        if (g == G_SHORT) changeVolume(-10);
        else if (g == G_LONG) {
            cfg.audioEnabled = !cfg.audioEnabled;
            settingsSyncSwitches();
            saveAndToast(cfg.audioEnabled ? LV_SYMBOL_VOLUME_MAX "  Sound: ON" : LV_SYMBOL_MUTE "  Sound: OFF");
            if (cfg.audioEnabled) audioPlayBeep(1);
        }
        break;
    case BOARD_KEY_RIGHT:
        if (g == G_SHORT) changeVolume(+10);
        else if (g == G_LONG) {
            int b = ((int)(cfg.brightness + 0.5f) / 25 + 1) * 25; // 25 -> 50 -> 75 -> 100 -> 25
            cfg.brightness = b > 100 ? 25 : b;
            applyConfig();
            char t[32];
            snprintf(t, sizeof(t), LV_SYMBOL_EYE_OPEN "  %.0f %%", (double)cfg.brightness);
            saveAndToast(t);
        }
        break;
    case BOARD_KEY_CENTER:
        if (g == G_DOUBLE) {
            bool on = !webPortalRequestedOn();
            webPortalRequestEnable(on);
            dashboardToast(on ? LV_SYMBOL_WIFI "  Wi-Fi: ON" : LV_SYMBOL_WIFI "  Wi-Fi: OFF", 1500);
        } else if (g == G_LONG) {
            openUpdateScreen();
        }
        break;
    }
}

} // namespace

void buttonInputPoll() {
    uint8_t raw = boardButtonsRaw();
    uint32_t now = millis();
    for (Key &k : keys) handle(k.mask, step(k, (raw & k.mask) != 0, now));
}
