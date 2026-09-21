// radar_car — UI verification demo (real sensors, LVGL UI still shared with
// the eventual production build)
//
// GNSS (u-blox M10N, UART2) went real 2026-09-15, radar (HLK-LD2451, UART1)
// went real 2026-09-16 — see gnss/GNSS.h and radar/LD2451.h, and
// include/pincfg.h for both wirings. SimTask (radar/SimTask.h) is fully
// retired now that both sensors it used to fake are real; the file is kept
// for reference/history but no longer started. This build exercises the
// Dashboard layout, target rendering, TTC/risk color coding, the
// audio-gate hysteresis display, the radar/GNSS fail-safe states, and a
// Settings screen (spec sections 16-17, 33-34, 52-61) against real sensor
// data end to end.
//
// Settings changes have real, visible effect (they're not a dead mockup):
// TTC thresholds recolor targets live, max range/min speed get pushed to
// the LD2451 itself at boot (radar/LD2451.cpp's configureRadar()) as well
// as rescaling the Dashboard, the audio-gate hysteresis pair drives the
// Audio status dot, and the Brightness slider drives the *real* backlight
// via LEDC PWM. Settings persist to NVS (ESP32 Preferences) across reboots.
//
// Not implemented from the spec yet: Basic/Advanced gating with long-press
// (section 17) — nothing here is safety-certified yet, so it's left as one
// flat Settings screen. Radar direction/SNR/trigger-count and a lane
// half-width are in Settings > Radar and get pushed to the LD2451 / used by
// LD2451.cpp's classifyRelation() — but a real lane-CORRIDOR model (multiple
// lanes, curvature, etc — Phase 5) still doesn't exist; classifyRelation()
// is a single-corridor lateral-offset check standing in for it. No buzzer
// yet (Phase 7).
//
// This file is a thin orchestrator (module split 2026-09-14 — see
// C:\Users\phamq\.claude\plans\idempotent-herding-zebra.md and
// docs/V1.2_hardening_proposal.md section A): setup()/loop() wire together
// core/ (config + mutex-protected shared state), display/, radar/ (LD2451),
// gnss/ (GNSS), touch/ (TouchTask), net/ (WebPortal — WiFi AP + live
// telemetry/config/OTA over HTTP, added 2026-09-14), map/ (SpeedLimitManager
// — offline microSD speed-limit map matching by GNSS position+heading, added
// 2026-09-15, see map/SpeedMapFormat.h for the binary database spec), and
// ui/ (Dashboard, Settings). The sensor tasks run independently on Core 0;
// this file's setup()/loop() IS the UI/render task on Core 1 (the default Arduino
// loopTask core).

#include <Arduino.h>
#include <lvgl.h>
#include <esp_heap_caps.h>
#include <esp_task_wdt.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h> // uxTaskGetStackHighWaterMark() — TEMP RAM-audit diagnostic, see the [mem] block below

#include "core/AppConfig.h"
#include "core/NvsStore.h"
#include "core/SharedState.h"
#include "display/DisplayDriver.h"
#include "gnss/GNSS.h"
#include "map/SpeedLimitManager.h"
#include "log/TripLogger.h"
#include "net/WebPortal.h"
#include "radar/LD2451.h"
#include "radar/SimTask.h" // TEMP DEMO 2026-09-16 — see radarTaskStart()/simTaskStart() swap below, revert before real driving use
#include "touch/TouchTask.h"
#include "ui/Dashboard.h"
#include "ui/Settings.h"
#include "audio/AudioPlayer.h"

AppConfig cfg; // extern-declared in core/AppConfig.h — see the comment there

static lv_display_t *lvDisplay;
static lv_indev_t *lvTouchIndev;

static uint32_t lastStatsMs = 0;
static uint32_t lastMemStatsMs = 0;

static void touch_read_cb(lv_indev_t *, lv_indev_data_t *data) {
    TouchPoint p = touchSnapshot();
    static bool wasTouched = false; // only log on DOWN/UP transitions, not every poll
    if (p.pressed) {
        data->point.x = p.x;
        data->point.y = p.y;
        data->state = LV_INDEV_STATE_PRESSED;
        wakeScreen();
        if (!wasTouched) Serial.printf("[touch] DOWN x=%u y=%u\n", p.x, p.y);
        wasTouched = true;
    } else {
        if (wasTouched) Serial.println("[touch] UP");
        wasTouched = false;
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

// ---------------------------------------------------------------------
void setup() {
    Serial.begin(115200);
    delay(200);
    // Banner corrected 2026-09-21 — it still claimed "simulated data, no
    // real sensors" long after the LD2451 and M10N both went real (see this
    // file's own header), which is exactly the kind of stale line that
    // makes a log misleading when something goes wrong in the field.
    Serial.println("\n[uidemo] radar_car — real LD2451 radar + M10N GNSS + offline speed map");
    // Shared with log/TripLogger.cpp, which persists the same string into
    // every session's CSV — see tripLogResetReasonStr()'s own comment for
    // why that matters (a reset mid-drive has no serial monitor attached).
    Serial.printf("[uidemo] last reset reason: %s\n", tripLogResetReasonStr());

    // Last-resort recovery for a genuinely stuck I2C transaction (touch
    // driver's own endTransmission()/requestFrom() error handling + bus
    // reset covers transactions that fail promptly, but a real hardware
    // hang confirmed 2026-09-14 blocked loop() outright with no error ever
    // returned). If loop() doesn't come back around to feed this within 5s,
    // the watchdog panics and reboots instead of freezing forever.
    esp_task_wdt_init(5, true);
    esp_task_wdt_add(NULL);

    loadConfigFromNVS(cfg);

    backlightBegin();
    applyConfig(); // sets initial backlight duty from loaded cfg.brightness

    displayBegin();
    touchTaskStart();
    audioInit();
    audioPlayBeep(1);

    lv_init();
    lvDisplay = lv_display_create(gfx->width(), gfx->height());
    lv_display_set_flush_cb(lvDisplay, dispFlushCb);

    // LVGL renders every pixel into these buffers, so they belong in internal
    // SRAM — PSRAM is several times slower per access and it showed. 20 rows
    // each keeps both buffers at ~38 KB total, which internal RAM can spare.
    size_t bufBytes = (size_t)gfx->width() * 20 * sizeof(lv_color_t);
    lv_color_t *drawBuf1 = (lv_color_t *)heap_caps_malloc(bufBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    lv_color_t *drawBuf2 = (lv_color_t *)heap_caps_malloc(bufBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!drawBuf1 || !drawBuf2) {
        Serial.println("[uidemo] WARN: internal RAM draw buffers failed, falling back to PSRAM");
        if (drawBuf1) heap_caps_free(drawBuf1);
        if (drawBuf2) heap_caps_free(drawBuf2);
        drawBuf1 = (lv_color_t *)heap_caps_malloc(bufBytes, MALLOC_CAP_SPIRAM);
        drawBuf2 = (lv_color_t *)heap_caps_malloc(bufBytes, MALLOC_CAP_SPIRAM);
    }
    lv_display_set_buffers(lvDisplay, drawBuf1, drawBuf2, bufBytes, LV_DISPLAY_RENDER_MODE_PARTIAL);

    lvTouchIndev = lv_indev_create();
    lv_indev_set_type(lvTouchIndev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(lvTouchIndev, touch_read_cb);
    // REVERTED from 10ms 2026-09-14: combined with 400kHz I2C this made the
    // touch controller chronically lock up (see AXS15231BTouch.cpp begin()).
    // 20ms is a middle ground between that and LVGL's laggier ~30ms default.
    lv_timer_set_period(lv_indev_get_read_timer(lvTouchIndev), 20);
    lv_indev_set_long_press_time(lvTouchIndev, HOLD_PRESS_MS);      // hold 1s anywhere on the dashboard = Settings

    buildDashboard();
    buildSettingsScreen();
    lv_screen_load(dashboardScreen);

    sharedStateInit();
    // cfg.demoMode (Settings > Radar > "Demo mode", user-requested
    // 2026-09-16 "them 1 nut bat tat thu nghiem trong menu") swaps the real
    // HLK-LD2451 task for radar/SimTask.cpp's simulated moving targets —
    // lets the Dashboard be exercised/tuned without real hardware (e.g.
    // indoors). Defaults OFF; only takes effect at boot (this check), not
    // live — see AppConfig.h's demoMode comment for why.
    if (cfg.demoMode) {
        Serial.println("[uidemo] DEMO MODE ON (Settings > Radar) — simulated radar targets, not real LD2451 data");
        simTaskStart(); // Core 0 — fake radar targets, see radar/SimTask.cpp
    } else {
        radarTaskStart(); // Core 0 — real HLK-LD2451 on UART1
    }
    gnssTaskStart();  // Core 0 — real GNSS M10N on UART2 (unaffected by demoMode — always real)
    webPortalInit(); // Core 0 — WiFi AP + local web server, starts with WiFi OFF — see net/WebPortal.h
    speedLimitManagerStart(); // Core 0 — microSD speed-limit map matching, see map/SpeedLimitManager.h
    tripLoggerStart(); // Core 0 — microSD trip/event CSV logging, see log/TripLogger.h
    // (Was disabled 2026-09-15 after two SPI-peripheral-contention
    // regressions — see pincfg.h's SD_MMC_CLK_PIN comment. Root cause: the
    // TF slot was never SPI at all, it's the ESP32-S3's dedicated SD_MMC
    // peripheral in 1-bit mode — confirmed from the vendor-adjacent demo
    // project's own working code. SdCardManager.cpp now uses SD_MMC.h,
    // which shares no hardware with the display's QSPI or the touch I2C
    // bus, so this is re-enabled.)
    refreshDashboard();
    lv_timer_create(simTimerCb, 150, NULL);
    lv_timer_create(burnInTimerCb, 60000, NULL);

    Serial.println("[uidemo] running — hold the screen 1s to open Settings");
}

void loop() {
    esp_task_wdt_reset();

    // TEMP diagnostic (2026-09-21, "kiem tra va phan tich file log") — see
    // setup()'s own comment. Fires once, well past the USB-CDC
    // reattachment gap, so the dump actually lands in a capturable serial
    // log instead of a boot-time print that's already lost by the time
    // anything can connect and start reading.
    static uint32_t lastTick = millis();
    static uint32_t lastHb = 0;
    uint32_t now0 = millis();
    // Kept temporarily: if a freeze recurs, the last [hb] line before the
    // next boot banner pinpoints how long loop() actually ran before it
    // stuck, without needing to reproduce with a debugger attached.
    if (now0 - lastHb > 1000) { lastHb = now0; Serial.printf("[hb %lu]\n", now0); }
    uint32_t now = millis();
    lv_tick_inc(now - lastTick);
    lastTick = now;
    lv_timer_handler();

    // Report where the frame time actually goes, so tuning stops being guesswork.
    if (now - lastStatsMs > 5000) {
        lastStatsMs = now;
        if (g_flushCount) {
            Serial.printf("[perf] frames=%lu  flush=%luus avg  canvas-copy=%luus avg\n", g_flushCount,
                          g_flushUs / g_flushCount, g_renderUs / g_flushCount);
        }
        g_flushUs = g_renderUs = g_flushCount = 0;
    }

    // Slow-leak early warning: if internal-RAM free space keeps trending
    // down run over run while its all-time minimum (tracked by the IDF
    // itself) keeps dropping too, something is leaking — worth catching
    // from a serial log over a multi-hour/day drive long before it actually
    // runs out and crashes. Internal RAM specifically (MALLOC_CAP_INTERNAL),
    // not the combined/default heap: LVGL objects and every FreeRTOS task
    // stack in this app come from internal RAM (see the draw-buffer
    // allocation above), while PSRAM (tracked separately below) sits mostly
    // idle and isn't the scarce resource here — mixing the two into one
    // "free heap" figure would silently include PSRAM's much larger, mostly
    // static headroom and mask a real internal-RAM leak trend. 30s cadence
    // is plenty for a slow trend; no need for anything finer-grained than a
    // human skimming the log.
    if (now - lastMemStatsMs > 30000) {
        lastMemStatsMs = now;
        Serial.printf("[mem] freeInternal=%uKB minFreeInternalEver=%uKB freePsram=%uKB\n",
                      (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                      (unsigned)(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024),
                      (unsigned)(ESP.getFreePsram() / 1024));
        // LVGL's own internal pool (LV_MEM_SIZE, lv_conf.h) is a separate
        // budget from the general internal-RAM heap tracked just above —
        // kept as a permanent low-cost telemetry line (added 2026-09-16
        // RAM audit) since that pool's own real max_used is what
        // LV_MEM_SIZE should be sized against, not a guess; see lv_conf.h's
        // own comment for the measurement this constant was picked from.
        lv_mem_monitor_t mon;
        lv_mem_monitor(&mon);
        Serial.printf("[mem] lvgl pool total=%uB maxUsed=%uB (%u%%) freeBiggest=%uB fragPct=%u%%\n",
                      (unsigned)mon.total_size, (unsigned)mon.max_used, mon.used_pct,
                      (unsigned)mon.free_biggest_size, mon.frag_pct);
        // This loop() IS the Arduino framework's own loopTask (Core 1), not
        // one this project creates itself, but its stack still comes out of
        // internal RAM like every other task's, so it's tracked here too.
        Serial.printf("[mem] loopTask stack high-water mark: %u bytes free\n",
                      (unsigned)(uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t)));
    }
    delay(1);
}
