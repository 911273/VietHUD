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
#include <freertos/semphr.h>

#define I2S_PORT        I2S_NUM_0
#define I2S_SAMPLE_RATE 16000
#define I2S_BCLK_PIN    42
#define I2S_LRCK_PIN    2
#define I2S_DOUT_PIN    41

static uint8_t s_volume = 80;
static bool s_initialized = false;
// Serializes access to the single I2S_NUM_0 port between the tone generator
// (audioPlayTone, called from the UI task) and the voice task's teardown/
// reinstall of the port (voiceTaskFn, Core 0). Added 2026-09-24: without it,
// a tone that passed the `s_initialized` check could still i2s_write into a
// port the voice task had just uninstalled (a real data race the original
// code's own comment admitted was never tested). audioPlayTone takes it with
// a 0 timeout (skips the chime if voice currently owns the port — the voice
// line IS the alert then), so the UI thread never blocks on audio.
static SemaphoreHandle_t s_i2sMutex = NULL;

// Unified audio command queue (2026-09-24): ALL audio — tone chimes AND voice
// clips — now plays on ONE background task (Core 0), not the UI/render thread.
// Before, audioPlayCameraAlert()/SignNotice()/OverspeedAlert() ran the tone
// i2s_write(portMAX_DELAY) + delay() sequences inline from refreshDashboard()
// on loopTask, stalling the UI ~250-340ms per alert (visible jank, WDT
// pressure). Now the UI just enqueues a command and returns immediately.
#define VOICE_DIR "/speedmap/sounds/vi/"
#define VOICE_FILENAME_MAX 48
#define AUDIO_QUEUE_LEN 12
enum : uint8_t { CMD_VOICE = 0, CMD_TONE_CAMERA, CMD_TONE_OVERSPEED, CMD_TONE_SIGN, CMD_TONE_BEEP };
struct AudioCmd {
    uint8_t kind;
    char file[VOICE_FILENAME_MAX]; // CMD_VOICE: mp3 name; CMD_TONE_BEEP: file[0]=count
};
static QueueHandle_t s_audioQueue = NULL;
static void audioTaskFn(void *); // defined below

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
    if (!s_i2sMutex) s_i2sMutex = xSemaphoreCreateMutex();
    if (!s_audioQueue) {
        s_audioQueue = xQueueCreate(AUDIO_QUEUE_LEN, sizeof(AudioCmd));
        if (s_audioQueue) {
            // Core 0, priority 1 — spends nearly all its time blocked on the
            // queue or inside blocking I2S writes, so it doesn't compete with
            // GNSS/map/web. 6144-byte stack covers the ESP8266Audio MP3 decoder.
            xTaskCreatePinnedToCore(audioTaskFn, "audioTask", 6144, NULL, 1, NULL, 0);
        }
    }
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
    // Skip (don't block) if the voice task currently owns the I2S port — the
    // voice line is the alert in that case. Also closes the race where the
    // port gets uninstalled between the check above and i2s_write below.
    if (s_i2sMutex && xSemaphoreTake(s_i2sMutex, 0) != pdTRUE) return;
    if (!s_initialized) { // re-check under the lock: voice may have just torn it down
        if (s_i2sMutex) xSemaphoreGive(s_i2sMutex);
        return;
    }

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
    if (s_i2sMutex) xSemaphoreGive(s_i2sMutex);
}

// --- Tone sequences: run ON THE AUDIO TASK (the vTaskDelay gaps here never
// touch the UI thread). audioPlayTone() itself is the blocking primitive. ---
static void toneCamera() { audioPlayTone(880, 120); vTaskDelay(pdMS_TO_TICKS(40)); audioPlayTone(1320, 180); }
static void toneOverspeed() {
    for (int i = 0; i < 3; i++) { audioPlayTone(1760, 80); if (i < 2) vTaskDelay(pdMS_TO_TICKS(40)); }
}
static void toneSign() { audioPlayTone(660, 100); vTaskDelay(pdMS_TO_TICKS(30)); audioPlayTone(880, 150); }
static void toneBeep(int n) {
    for (int i = 0; i < n; i++) { audioPlayTone(1000, 80); if (i + 1 < n) vTaskDelay(pdMS_TO_TICKS(50)); }
}

static void enqueueCmd(uint8_t kind, const char *file) {
    if (!cfg.audioEnabled || !s_audioQueue) return;
    AudioCmd c;
    c.kind = kind;
    c.file[0] = '\0';
    if (file) { strncpy(c.file, file, sizeof(c.file) - 1); c.file[sizeof(c.file) - 1] = '\0'; }
    xQueueSend(s_audioQueue, &c, 0); // non-blocking; drop if full — UI must never stall on audio
}

// Public alert API — now NON-BLOCKING: just enqueue, the audio task plays it.
void audioPlayCameraAlert() { enqueueCmd(CMD_TONE_CAMERA, nullptr); }
void audioPlayOverspeedAlert() { enqueueCmd(CMD_TONE_OVERSPEED, nullptr); }
void audioPlaySignNotice() { enqueueCmd(CMD_TONE_SIGN, nullptr); }
void audioPlayBeep(uint8_t count) { char b[2] = {(char)(count ? count : 1), 0}; enqueueCmd(CMD_TONE_BEEP, b); }

void audioUpdate() {
    // Optional periodic hook for streaming audio
}

// ---------------------------------------------------------------------
// Audio task (Core 0) — drains the command queue and plays tones + Vietnamese
// voice MP3s sequentially, so nothing audio ever runs on the UI thread. Voice
// decode is ESP8266Audio (AudioGeneratorMP3 + AudioFileSourceFS +
// AudioOutputI2S) reading straight off the SD card this project already mounts.
// The I2S port is shared: the ESP8266Audio output installs its OWN i2s_std
// channel on I2S_NUM_0, so a voice clip tears down the legacy tone driver for
// its duration and reinstalls it after (guarded by s_i2sMutex). Because tones
// and voice now run on this ONE task, they're naturally serialized.
// ---------------------------------------------------------------------
static void audioTaskFn(void *) {
    for (;;) {
        AudioCmd c;
        if (xQueueReceive(s_audioQueue, &c, portMAX_DELAY) != pdTRUE) continue;
        if (!cfg.audioEnabled || s_volume == 0) continue; // dropped, not queued-silent

        if (c.kind == CMD_TONE_CAMERA) { toneCamera(); continue; }
        if (c.kind == CMD_TONE_OVERSPEED) { toneOverspeed(); continue; }
        if (c.kind == CMD_TONE_SIGN) { toneSign(); continue; }
        if (c.kind == CMD_TONE_BEEP) { toneBeep(c.file[0] ? (uint8_t)c.file[0] : 1); continue; }

        // CMD_VOICE
        char path[VOICE_FILENAME_MAX + sizeof(VOICE_DIR)];
        snprintf(path, sizeof(path), VOICE_DIR "%s", c.file);

        if (s_i2sMutex) xSemaphoreTake(s_i2sMutex, portMAX_DELAY);
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

        if (installToneI2S()) s_initialized = true;
        if (s_i2sMutex) xSemaphoreGive(s_i2sMutex);
    }
}

void audioQueueVoice(const char *filename) { enqueueCmd(CMD_VOICE, filename); }
