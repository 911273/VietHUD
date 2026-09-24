// VietHUD — GPS-only offline speed-limit/camera/traffic-sign warning device.
// Radar (HLK-LD2451) and everything built around it were removed entirely
// 2026-09-21 — this is a full product replacement, not a side-by-side demo
// (see docs/VietHUD_offline_speed_alert_plan.md and README.md). Renamed
// from main_ui_demo.cpp at the same time: this is now the one true app
// entry point, no longer a "UI verification demo" layered on top of
// something else — see git history if the earlier radar/demo-mode content
// is ever needed for reference.
//
// GNSS (u-blox M10N, UART2) has been real since 2026-09-15 — see gnss/GNSS.h
// and include/pincfg.h for the wiring. This build exercises the Dashboard
// layout, the offline microSD speed-limit/camera/sign map matching, and a
// Settings screen against real sensor data end to end.
//
// Settings changes have real, visible effect (they're not a dead mockup):
// the Brightness slider drives the *real* backlight via LEDC PWM, Theme/
// Rotation apply live or on next boot as documented in Settings.cpp, and
// the alert-audio toggle gates both tone chimes and voice playback
// (audio/AudioPlayer.h). Settings persist to NVS (ESP32 Preferences)
// across reboots.
//
// This file is a thin orchestrator (module split 2026-09-14 — see
// docs/V1.2_hardening_proposal.md section A): setup()/loop() wire together
// core/ (config + mutex-protected shared state), display/, gnss/ (GNSS),
// touch/ (TouchTask), net/ (WebPortal — WiFi AP + live telemetry/config/OTA
// over HTTP, added 2026-09-14), map/ (SpeedLimitManager — offline microSD
// speed-limit/camera/sign map matching by GNSS position+heading, added
// 2026-09-15, see map/SpeedMapFormat.h for the binary database spec),
// audio/ (AudioPlayer — tone chimes + queued Vietnamese voice playback), and
// ui/ (Dashboard, Settings). The sensor tasks run independently on Core 0;
// this file's setup()/loop() IS the UI/render task on Core 1 (the default
// Arduino loopTask core).

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
#include "demo/DemoMode.h"
#include "display/DisplayDriver.h"
#include "gnss/GNSS.h"
#include "map/SpeedLimitManager.h"
#include "map/SdCardManager.h"
#include "log/TripLogger.h"
#include "net/WebPortal.h"
#include "touch/TouchTask.h"
#include "ui/Dashboard.h"
#include "ui/Settings.h"
#include "audio/AudioPlayer.h"

AppConfig cfg; // extern-declared in core/AppConfig.h — see the comment there

// VRE boot-logo splash image (src/ui/logo_vre.c, RGB565A8, white keyed to
// transparent) — user-requested 2026-09-24 boot screen.
LV_IMAGE_DECLARE(logo_vre);

// Arduino-ESP32's cores/esp32/main.cpp declares this as a WEAK function
// (default 8192 bytes, see ARDUINO_LOOP_STACK_SIZE there) specifically so a
// sketch can override it — this is that override. Raised to 16384
// 2026-09-22 after a REAL, reproducible crash on hardware: "Guru Meditation
// Error: Core 1 panic'ed (Unhandled debug exception) — Stack canary
// watchpoint triggered (loopTask)" while map/RasterMapManager.cpp's
// renderBackground() retried a failed SD read every 2s directly from
// updateMapCanvas() (ui/Dashboard.cpp), itself called every ~150ms tick from
// this file's own loop() — i.e. loopTask, the Arduino default/UI task this
// whole file's setup()/loop() runs as (see this file's own header comment).
// uxTaskGetStackHighWaterMark() had already been observed at a mere 278
// bytes free right before the crash (this file's own [mem] block), so this
// isn't a guess at a hypothetical risk — it's a confirmed near-miss that
// then became a real one. Doubling to 16384 is the standard, minimal-risk
// fix for genuine stack pressure (loopTask is UI-only work, not a
// tightly-budgeted small task like the sensor tasks elsewhere in this
// project) — it does not touch or explain away whatever in
// RasterMapManager.cpp/SD_MMC's own call depth is actually consuming that
// much stack, which is a separate thing worth understanding on its own.
size_t getArduinoLoopTaskStackSize(void) { return 16384; }

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
// Boot splash (user-requested 2026-09-24): show the VRE logo for ~2s with a
// modern motion — scale-in with a slight overshoot + fade-in, a brief hold,
// then a fade-out that hands over to the Dashboard with a cross-fade. Driven
// by a manual pump loop (we're single-threaded in setup() here) so the timing
// is exact and the handover happens precisely at the end. dashboardScreen must
// already be built before this is called (it's the cross-fade target).
static void showBootSplash() {
    lv_obj_t *splash = lv_obj_create(NULL);
    lv_obj_remove_style_all(splash);
    lv_obj_set_style_bg_opa(splash, LV_OPA_COVER, 0);
    // Subtle vertical gradient (deep navy -> near-black) for a modern feel.
    lv_obj_set_style_bg_color(splash, lv_color_hex(0x0A1526), 0);
    lv_obj_set_style_bg_grad_color(splash, lv_color_hex(0x04060A), 0);
    lv_obj_set_style_bg_grad_dir(splash, LV_GRAD_DIR_VER, 0);
    lv_obj_clear_flag(splash, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *logo = lv_image_create(splash);
    lv_image_set_src(logo, &logo_vre);
    lv_obj_center(logo);
    lv_image_set_pivot(logo, logo_vre.header.w / 2, logo_vre.header.h / 2);
    lv_image_set_scale(logo, 128);          // start at 0.5x
    lv_obj_set_style_opa(logo, LV_OPA_TRANSP, 0);
    lv_screen_load(splash);

    const uint32_t IN = 700, HOLD_END = 1550, TOTAL = 2000;
    const float c1 = 1.70158f, c3 = c1 + 1.0f; // ease-out-back overshoot
    uint32_t t0 = millis(), lastTick = t0;
    for (;;) {
        uint32_t now = millis();
        uint32_t el = now - t0;
        int scale; lv_opa_t opa;
        if (el < IN) {
            float p = (float)el / (float)IN;
            float p1 = p - 1.0f;
            float eb = 1.0f + c3 * p1 * p1 * p1 + c1 * p1 * p1; // overshoots ~1.1 then settles to 1
            scale = 128 + (int)((256 - 128) * eb);
            float o = (float)el / 500.0f; if (o > 1.0f) o = 1.0f;
            opa = (lv_opa_t)(255.0f * o);
        } else if (el < HOLD_END) {
            scale = 256; opa = LV_OPA_COVER;
        } else if (el < TOTAL) {
            float q = (float)(el - HOLD_END) / (float)(TOTAL - HOLD_END);
            scale = 256 + (int)(44 * q);                 // gentle grow on the way out
            opa = (lv_opa_t)(255.0f * (1.0f - q));
        } else {
            break;
        }
        lv_image_set_scale(logo, scale);
        lv_obj_set_style_opa(logo, opa, 0);
        lv_tick_inc(now - lastTick); lastTick = now;
        lv_timer_handler();
        esp_task_wdt_reset();
        delay(8);
    }
    // Cross-fade to the Dashboard; auto_del=true frees the splash afterwards.
    lv_screen_load_anim(dashboardScreen, LV_SCR_LOAD_ANIM_FADE_ON, 300, 0, true);
    // Pump the transition so it actually plays before setup() moves on.
    uint32_t tt = millis(), lt = tt;
    while (millis() - tt < 340) {
        uint32_t now = millis();
        lv_tick_inc(now - lt); lt = now;
        lv_timer_handler();
        esp_task_wdt_reset();
        delay(8);
    }
}

// ---------------------------------------------------------------------
void setup() {
    Serial.begin(115200);
    delay(200);
    Serial.println("\n[viethud] VietHUD — offline GPS speed-limit/camera/sign warning device");
    // Shared with log/TripLogger.cpp, which persists the same string into
    // every session's CSV — see tripLogResetReasonStr()'s own comment for
    // why that matters (a reset mid-drive has no serial monitor attached).
    Serial.printf("[viethud] last reset reason: %s\n", tripLogResetReasonStr());

    // Last-resort recovery for a genuinely stuck I2C transaction (touch
    // driver's own endTransmission()/requestFrom() error handling + bus
    // reset covers transactions that fail promptly, but a real hardware
    // hang confirmed 2026-09-14 blocked loop() outright with no error ever
    // returned). If loop() doesn't come back around to feed this within 5s,
    // the watchdog panics and reboots instead of freezing forever.
    esp_task_wdt_init(10, true);
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
        Serial.println("[viethud] WARN: internal RAM draw buffers failed, falling back to PSRAM");
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

    sdMgrMount();
    buildDashboard();
    buildSettingsScreen();
    showBootSplash(); // ~2s animated VRE logo, then cross-fades to the Dashboard

    sharedStateInit();
    gnssTaskStart();  // Core 0 — real GNSS M10N on UART2
    webPortalInit(); // Core 0 — WiFi AP + local web server, starts with WiFi OFF — see net/WebPortal.h
    speedLimitManagerStart(); // Core 0 — microSD speed-limit/camera/sign map matching, see map/SpeedLimitManager.h
    tripLoggerStart(); // Core 0 — microSD trip/event CSV logging, see log/TripLogger.h
    demoModeStart(); // Core 0 — scripted UI demo, idles until switched on in Settings > Display (demo/DemoMode.h)
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

    Serial.println("[viethud] running — hold the screen 1s to open Settings");
}

void loop() {
    esp_task_wdt_reset();

    static uint32_t lastTick = millis();
    static uint32_t lastHb = 0;
    uint32_t now0 = millis();
    // Kept temporarily: if a freeze recurs, the last [hb] line before the
    // next boot banner pinpoints how long loop() actually ran before it
    // stuck, without needing to reproduce with a debugger attached.
    if (now0 - lastHb > 1000) { lastHb = now0; Serial.printf("[hb %lu]\n", now0); }

    // TEMPORARY serial-triggered demo toggle (2026-09-22) — added purely to
    // verify the live background-map feature while the touch controller is
    // reporting a stuck/wrong X coordinate on real hardware (a real,
    // pre-existing bug, separate from the map work — see [touch] log lines
    // showing x=0 on every touch regardless of where the panel was actually
    // pressed), which makes the Settings > Display > Demo mode SWITCH
    // unreachable by touch right now. Type 'd' + Enter in the serial
    // monitor to toggle it without touching the screen at all. Remove once
    // the touch calibration bug is fixed and the real switch is reachable
    // again — this bypasses the UI entirely and isn't meant to ship.
    while (Serial.available()) {
        char c = (char)Serial.read();
        if (c == 'd') {
            bool now2 = !demoModeIsEnabled();
            demoModeSetEnabled(now2);
            Serial.printf("[debug] demo mode toggled via serial -> %s\n", now2 ? "ON" : "OFF");
        }
    }

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
        // Live background map's own draw cost (ui/Dashboard.h) — separate
        // line since it only increments on the (usually rare) ticks that
        // actually redrew the map canvas, not every frame like the counters
        // above; see the plan's own note that the spec's "<=10ms" render
        // target needs measuring on real hardware, not assuming.
        if (g_mapDrawCount) {
            Serial.printf("[perf] map redraws=%lu  draw=%luus avg\n", g_mapDrawCount, g_mapDrawUs / g_mapDrawCount);
        }
        g_mapDrawUs = g_mapDrawCount = 0;
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
