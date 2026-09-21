#include "TouchTask.h"
#include "AXS15231BTouch.h"
#include "core/AppConfig.h" // cfg.screenRotation
#include "pincfg.h"
#include "dispcfg.h"
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <esp_task_wdt.h>

static AXS15231BTouch touch(TOUCH_SDA, TOUCH_SCL, TOUCH_INT, TOUCH_ADDR);
static SemaphoreHandle_t touchMutex;
static TouchPoint sharedPoint;

TouchPoint touchSnapshot() {
    xSemaphoreTake(touchMutex, portMAX_DELAY);
    TouchPoint copy = sharedPoint;
    xSemaphoreGive(touchMutex);
    return copy;
}

static void touchTaskFn(void *) {
    if (!touch.begin()) Serial.println("[touch] ERROR: touch I2C init failed");
    // Must numerically match display/DisplayDriver.cpp's gfx rotation —
    // confirmed by reading AXS15231BTouch::getPoint()'s own rotation switch,
    // it uses the identical 0-3 convention against the same native
    // TFT_RES_W/TFT_RES_H calibration range below (that range itself is NOT
    // rotation-dependent — it's the raw panel's native geometry regardless
    // of which way the logical screen is currently rotated).
    touch.setRotation((uint8_t)cfg.screenRotation);
    touch.enableOffsetCorrection(true);
    touch.setOffsets(TOUCH_X_MIN, TOUCH_X_MAX, TFT_RES_W - 1, TOUCH_Y_MIN, TOUCH_Y_MAX, TFT_RES_H - 1);

    esp_task_wdt_add(NULL);
    for (;;) {
        uint16_t x, y;
        bool touched = touch.getPoint(&x, &y);

        xSemaphoreTake(touchMutex, portMAX_DELAY);
        sharedPoint.pressed = touched;
        if (touched) {
            sharedPoint.x = x;
            sharedPoint.y = y;
        }
        xSemaphoreGive(touchMutex);

        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(10)); // sample rate independent of LVGL's own indev poll period
    }
}

void touchTaskStart() {
    touchMutex = xSemaphoreCreateMutex();
    // 2560, not the original 4096 — measured real-hardware stack high-water
    // mark (2026-09-16 RAM audit) never dropped below ~2508 bytes free of
    // 4096, i.e. this task never used more than ~1588 bytes; 2560 keeps a
    // comfortable ~970-byte (~38%) margin over that while giving ~1.5KB
    // back to internal RAM.
    xTaskCreatePinnedToCore(touchTaskFn, "touchTask", 2560, NULL, 3, NULL, 0); // Core 0, alongside SimTask
}
