#include "AudioPlayer.h"
#include "core/AppConfig.h" // cfg.audioEnabled — master alert-audio toggle (Settings > Display)
#include <Arduino.h>
#include <driver/i2s.h>
#include <math.h>
#include <string.h>
#include <SD_MMC.h>
#include <AudioFileSourceFS.h>
#include <AudioGeneratorMP3.h>
#include <AudioOutputI2S.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>

#define I2S_PORT        I2S_NUM_0
#define I2S_SAMPLE_RATE 16000
#define I2S_BCLK_PIN    42
#define I2S_LRCK_PIN    2
#define I2S_DOUT_PIN    41

static uint8_t s_volume = 80;
static bool s_initialized = false;

// Installs the legacy driver/i2s.h tone driver on I2S_NUM_0. Split out of
// audioInit() (2026-09-21, Task E) so voiceTaskFn() below can call it again
// after ESP8266Audio's AudioOutputI2S releases the port — see that
// function's own comment for why the two APIs can't share it
// simultaneously.
static bool installToneI2S() {
    i2s_config_t i2s_config = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
        .sample_rate = I2S_SAMPLE_RATE,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 4,
        .dma_buf_len = 256,
        .use_apll = false,
        .tx_desc_auto_clear = true,
        .fixed_mclk = 0
    };

    i2s_pin_config_t pin_config = {
        .bck_io_num = I2S_BCLK_PIN,
        .ws_io_num = I2S_LRCK_PIN,
        .data_out_num = I2S_DOUT_PIN,
        .data_in_num = I2S_PIN_NO_CHANGE
    };

    esp_err_t err = i2s_driver_install(I2S_PORT, &i2s_config, 0, NULL);
    if (err != ESP_OK) {
        Serial.printf("[audio] i2s_driver_install failed: 0x%x\n", err);
        return false;
    }

    err = i2s_set_pin(I2S_PORT, &pin_config);
    if (err != ESP_OK) {
        Serial.printf("[audio] i2s_set_pin failed: 0x%x\n", err);
        return false;
    }
    return true;
}

void audioInit() {
    if (s_initialized) return;
    if (installToneI2S()) {
        s_initialized = true;
        Serial.println("[audio] NS4168 I2S audio driver initialized on BCLK=42, LRCK=2, DOUT=41");
    }
}

void audioSetVolume(uint8_t percent) {
    if (percent > 100) percent = 100;
    s_volume = percent;
}

uint8_t audioGetVolume() {
    return s_volume;
}

void audioPlayTone(uint16_t freqHz, uint16_t durationMs) {
    if (!s_initialized || s_volume == 0 || freqHz == 0) return;

    int totalSamples = (I2S_SAMPLE_RATE * durationMs) / 1000;
    int16_t buffer[128];
    float phase = 0.0f;
    float phaseInc = (2.0f * (float)M_PI * (float)freqHz) / (float)I2S_SAMPLE_RATE;
    float amplitude = (float)(s_volume * 327); // Scale 0..100% to ~32700

    int samplesRemaining = totalSamples;
    while (samplesRemaining > 0) {
        int chunk = (samplesRemaining > 128) ? 128 : samplesRemaining;
        for (int i = 0; i < chunk; i++) {
            // Apply slight envelope smoothing at start and end
            float env = 1.0f;
            int sampleIdx = totalSamples - samplesRemaining + i;
            if (sampleIdx < 160) {
                env = (float)sampleIdx / 160.0f;
            } else if (sampleIdx > totalSamples - 160) {
                env = (float)(totalSamples - sampleIdx) / 160.0f;
            }
            buffer[i] = (int16_t)(sinf(phase) * amplitude * env);
            phase += phaseInc;
            if (phase >= 2.0f * (float)M_PI) phase -= 2.0f * (float)M_PI;
        }
        size_t bytesWritten = 0;
        i2s_write(I2S_PORT, buffer, chunk * sizeof(int16_t), &bytesWritten, portMAX_DELAY);
        samplesRemaining -= chunk;
    }

    // Flush small silent buffer at the end to prevent click
    memset(buffer, 0, sizeof(buffer));
    size_t bw = 0;
    i2s_write(I2S_PORT, buffer, 64 * sizeof(int16_t), &bw, portMAX_DELAY);
}

void audioPlayCameraAlert() {
    if (!cfg.audioEnabled) return;
    // Distinct double chime for camera ahead: High -> Higher
    audioPlayTone(880, 120);  // A5
    delay(40);
    audioPlayTone(1320, 180); // E6
}

void audioPlayOverspeedAlert() {
    if (!cfg.audioEnabled) return;
    // Rapid urgent alarm: 3 short high-pitch beeps
    for (int i = 0; i < 3; i++) {
        audioPlayTone(1760, 80); // A6
        if (i < 2) delay(40);
    }
}

void audioPlaySignNotice() {
    if (!cfg.audioEnabled) return;
    // Gentle informative notification chime: Medium -> High
    audioPlayTone(660, 100);  // E5
    delay(30);
    audioPlayTone(880, 150);  // A5
}

void audioPlayBeep(uint8_t count) {
    for (uint8_t i = 0; i < count; i++) {
        audioPlayTone(1000, 80);
        if (i + 1 < count) delay(50);
    }
}

void audioUpdate() {
    // Optional periodic hook for streaming audio
}

// ---------------------------------------------------------------------
// Queued Vietnamese voice playback (added 2026-09-21, Task E — replaces
// tone-only alerts with real speech for the sign/camera/speed-limit
// warnings that used to be radar's territory). Real MP3 decode via
// ESP8266Audio (AudioGeneratorMP3 + AudioFileSourceFS + AudioOutputI2S),
// reading straight off the microSD card this project already mounts via
// SD_MMC (map/SdCardManager.cpp's sdMgrMount() / TripLogger's
// sdMgrAppendLine() bring the peripheral up at boot — this code never
// calls SD_MMC.begin() itself, same "don't remount separately" rule every
// other SD consumer here follows... except this one CAN'T go through
// SdCardManager.h's sole-SD-owner functions, since those only expose the
// speedmap/triplog-shaped operations it already needed, not raw streamed
// file reads for an audio decoder — AudioFileSourceFS needs the fs::FS
// object directly. This is a deliberate, narrow exception to that rule:
// SD_MMC's own File API is safe to call from multiple tasks as long as
// they don't step on each other's open handles, and voice playback and
// the speedmap/triplog readers never run inside the same call at once in
// practice (this task blocks for the whole length of a clip, and nothing
// else here holds a file open across a yield).
// ---------------------------------------------------------------------
#define VOICE_DIR "/speedmap/sounds/vi/"
#define VOICE_FILENAME_MAX 48
#define VOICE_QUEUE_LEN 8

static QueueHandle_t s_voiceQueue = NULL;

static void voiceTaskFn(void *) {
    for (;;) {
        char filename[VOICE_FILENAME_MAX];
        if (xQueueReceive(s_voiceQueue, filename, portMAX_DELAY) != pdTRUE) continue;
        if (!cfg.audioEnabled || s_volume == 0) continue; // dropped, not queued-and-silent — avoids a stale backlog playing late after audio gets re-enabled

        char path[VOICE_FILENAME_MAX + sizeof(VOICE_DIR)];
        snprintf(path, sizeof(path), VOICE_DIR "%s", filename);

        // ESP8266Audio's AudioOutputI2S installs ITS OWN i2s_std (new IDF
        // driver) channel on I2S_NUM_0 — audioPlayTone()'s legacy
        // driver/i2s.h API already owns that same port, and the two can't
        // both be installed at once. Voice cues are short, discrete events
        // (a couple seconds every so often), not a continuous stream, so
        // tearing the tone driver down for the duration of one clip and
        // reinstalling it after is a fine trade — never verified on real
        // hardware yet (no SD card mounted on the dev machine this was
        // built on), see the project's own final report for what that
        // means for confidence here.
        i2s_driver_uninstall(I2S_PORT);
        s_initialized = false;

        AudioFileSourceFS source(SD_MMC, path);
        if (source.isOpen()) {
            AudioOutputI2S out;
            out.SetPinout(I2S_BCLK_PIN, I2S_LRCK_PIN, I2S_DOUT_PIN);
            out.SetGain((float)s_volume / 100.0f);
            AudioGeneratorMP3 mp3;
            if (mp3.begin(&source, &out)) {
                while (mp3.isRunning()) {
                    if (!mp3.loop()) mp3.stop();
                    vTaskDelay(1);
                }
            } else {
                Serial.printf("[audio] voice: mp3.begin() failed for %s\n", path);
            }
        } else {
            Serial.printf("[audio] voice: file not found: %s\n", path);
        }

        // Hand I2S back to the tone generator so the next audioPlayTone()
        // call (or the next voice clip's own teardown/reinstall) works.
        if (installToneI2S()) s_initialized = true;
    }
}

void audioQueueVoice(const char *filename) {
    if (!cfg.audioEnabled) return;
    if (!s_voiceQueue) {
        s_voiceQueue = xQueueCreate(VOICE_QUEUE_LEN, VOICE_FILENAME_MAX);
        if (!s_voiceQueue) return;
        // Core 0, alongside every other sensor/IO task here — this task
        // spends almost all its time blocked (either on the queue, or
        // inside mp3.loop()'s own blocking I2S writes), so it doesn't
        // compete meaningfully with GNSS/map/web for CPU. Stack sized
        // generously (ESP8266Audio's MP3 decoder + its own internal
        // buffers are heavier than this project's other small tasks) —
        // not yet measured against real playback on real hardware; revisit
        // with uxTaskGetStackHighWaterMark() once a card + speakers are
        // actually available to test with.
        xTaskCreatePinnedToCore(voiceTaskFn, "audioVoice", 6144, NULL, 1, NULL, 0);
    }
    char buf[VOICE_FILENAME_MAX];
    strncpy(buf, filename, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    // Non-blocking (timeout=0) — if the queue's already full, drop the
    // newest request rather than block the caller (ui/Dashboard.cpp's
    // refreshDashboard(), which must never stall on audio). A backlog of
    // stale voice cues playing out minutes late would be worse than
    // skipping one.
    xQueueSend(s_voiceQueue, buf, 0);
}
