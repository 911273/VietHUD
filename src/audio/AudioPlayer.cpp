#include "AudioPlayer.h"
#include <Arduino.h>
#include <driver/i2s.h>
#include <math.h>

#define I2S_PORT        I2S_NUM_0
#define I2S_SAMPLE_RATE 16000
#define I2S_BCLK_PIN    42
#define I2S_LRCK_PIN    2
#define I2S_DOUT_PIN    41

static uint8_t s_volume = 80;
static bool s_initialized = false;

void audioInit() {
    if (s_initialized) return;

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
        return;
    }

    err = i2s_set_pin(I2S_PORT, &pin_config);
    if (err != ESP_OK) {
        Serial.printf("[audio] i2s_set_pin failed: 0x%x\n", err);
        return;
    }

    s_initialized = true;
    Serial.println("[audio] NS4168 I2S audio driver initialized on BCLK=42, LRCK=2, DOUT=41");
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
    // Distinct double chime for camera ahead: High -> Higher
    audioPlayTone(880, 120);  // A5
    delay(40);
    audioPlayTone(1320, 180); // E6
}

void audioPlayOverspeedAlert() {
    // Rapid urgent alarm: 3 short high-pitch beeps
    for (int i = 0; i < 3; i++) {
        audioPlayTone(1760, 80); // A6
        if (i < 2) delay(40);
    }
}

void audioPlaySignNotice() {
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
