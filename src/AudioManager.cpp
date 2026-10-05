#include "AudioManager.h"
#include <math.h>

#define I2S_PORT        I2S_NUM_0
#define I2S_SAMPLE_RATE 16000

AudioManager audio;

AudioManager::AudioManager() : m_volume(85), m_queue(nullptr) {}

bool AudioManager::begin() {
    // 1. Configure I2S Driver
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
        .bck_io_num = PIN_I2S_BCLK,
        .ws_io_num = PIN_I2S_LRCK,
        .data_out_num = PIN_I2S_DOUT,
        .data_in_num = I2S_PIN_NO_CHANGE
    };

    i2s_driver_uninstall(I2S_PORT);
    vTaskDelay(pdMS_TO_TICKS(10));

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

    // 2. Create command queue and background task on Core 0
    m_queue = xQueueCreate(10, sizeof(SoundType));
    if (m_queue) {
        xTaskCreatePinnedToCore(audioTask, "audioTask", 3072, this, 1, NULL, 0);
    }

    return true;
}

void AudioManager::setVolume(uint8_t volumePercent) {
    if (volumePercent > 100) volumePercent = 100;
    m_volume = volumePercent;
}

uint8_t AudioManager::volumeUp(uint8_t step) {
    if (m_volume + step > 100) {
        m_volume = 100;
    } else {
        m_volume += step;
    }
    return m_volume;
}

uint8_t AudioManager::volumeDown(uint8_t step) {
    if (m_volume <= step) {
        m_volume = 0;
    } else {
        m_volume -= step;
    }
    return m_volume;
}

void AudioManager::playToneRaw(uint16_t freqHz, uint16_t durationMs) {
    if (m_volume == 0 || freqHz == 0) return;

    int totalSamples = (I2S_SAMPLE_RATE * durationMs) / 1000;
    int16_t buffer[128];
    float phase = 0.0f;
    float phaseInc = (2.0f * (float)M_PI * (float)freqHz) / (float)I2S_SAMPLE_RATE;
    float amplitude = (float)(m_volume * 327); // Scale 0..100% to ~32700

    int samplesRemaining = totalSamples;
    while (samplesRemaining > 0) {
        int chunk = (samplesRemaining > 128) ? 128 : samplesRemaining;
        for (int i = 0; i < chunk; i++) {
            // Apply slight attack/decay envelope smoothing to avoid clicks
            float env = 1.0f;
            int sampleIdx = totalSamples - samplesRemaining + i;
            if (sampleIdx < 120) {
                env = (float)sampleIdx / 120.0f;
            } else if (sampleIdx > totalSamples - 120) {
                env = (float)(totalSamples - sampleIdx) / 120.0f;
            }
            buffer[i] = (int16_t)(sinf(phase) * amplitude * env);
            phase += phaseInc;
            if (phase >= 2.0f * (float)M_PI) phase -= 2.0f * (float)M_PI;
        }
        size_t bytesWritten = 0;
        i2s_write(I2S_PORT, buffer, chunk * sizeof(int16_t), &bytesWritten, portMAX_DELAY);
        samplesRemaining -= chunk;
    }

    // Flush small silent buffer to finish smoothly
    memset(buffer, 0, sizeof(buffer));
    size_t bytesWritten = 0;
    i2s_write(I2S_PORT, buffer, sizeof(buffer), &bytesWritten, portMAX_DELAY);
}

void AudioManager::audioTask(void *param) {
    AudioManager *self = (AudioManager *)param;
    SoundType sound;

    while (true) {
        if (xQueueReceive(self->m_queue, &sound, portMAX_DELAY) == pdTRUE) {
            switch (sound) {
                case SOUND_CLICK:
                    // Short crisp click beep (1500 Hz, 35ms)
                    self->playToneRaw(1500, 35);
                    break;

                case SOUND_MODE_SWITCH:
                    // 2 rising beeps (880 Hz -> 1320 Hz)
                    self->playToneRaw(880, 50);
                    vTaskDelay(pdMS_TO_TICKS(25));
                    self->playToneRaw(1320, 70);
                    break;

                case SOUND_WARN:
                    // 2 warning beeps
                    self->playToneRaw(1100, 60);
                    vTaskDelay(pdMS_TO_TICKS(35));
                    self->playToneRaw(1100, 60);
                    break;

                case SOUND_OVERSPEED:
                    // 3 urgent high-pitch beeps: TÍT - TÍT - TÍT (1800 Hz)
                    for (int i = 0; i < 3; i++) {
                        self->playToneRaw(1800, 60);
                        if (i < 2) vTaskDelay(pdMS_TO_TICKS(35));
                    }
                    break;

                case SOUND_STARTUP:
                    // Ascending cheerful power-on chime
                    self->playToneRaw(523, 70);
                    vTaskDelay(pdMS_TO_TICKS(20));
                    self->playToneRaw(659, 70);
                    vTaskDelay(pdMS_TO_TICKS(20));
                    self->playToneRaw(784, 70);
                    vTaskDelay(pdMS_TO_TICKS(20));
                    self->playToneRaw(1046, 120);
                    break;

                case SOUND_ALERT_CAMERA:
                    // 2 crisp alert beeps (1300 Hz -> 1600 Hz)
                    self->playToneRaw(1300, 70);
                    vTaskDelay(pdMS_TO_TICKS(40));
                    self->playToneRaw(1600, 90);
                    break;

                case SOUND_ALERT_TRAFFIC_LIGHT:
                    // Single clear alert ping (1200 Hz)
                    self->playToneRaw(1200, 100);
                    break;

                case SOUND_ALERT_RESIDENT:
                    // 2 gentle rising chimes (700 Hz -> 900 Hz)
                    self->playToneRaw(700, 80);
                    vTaskDelay(pdMS_TO_TICKS(30));
                    self->playToneRaw(900, 80);
                    break;

                default:
                    break;
            }
        }
    }
}

void AudioManager::playClick() {
    SoundType s = SOUND_CLICK;
    if (m_queue) xQueueSend(m_queue, &s, 0);
}

void AudioManager::playModeSwitch() {
    SoundType s = SOUND_MODE_SWITCH;
    if (m_queue) xQueueSend(m_queue, &s, 0);
}

void AudioManager::playWarn() {
    SoundType s = SOUND_WARN;
    if (m_queue) xQueueSend(m_queue, &s, 0);
}

void AudioManager::playOverspeed() {
    SoundType s = SOUND_OVERSPEED;
    if (m_queue) xQueueSend(m_queue, &s, 0);
}

void AudioManager::playStartup() {
    SoundType s = SOUND_STARTUP;
    if (m_queue) xQueueSend(m_queue, &s, 0);
}

void AudioManager::playAlertCamera() {
    SoundType s = SOUND_ALERT_CAMERA;
    if (m_queue) xQueueSend(m_queue, &s, 0);
}

void AudioManager::playAlertTrafficLight() {
    SoundType s = SOUND_ALERT_TRAFFIC_LIGHT;
    if (m_queue) xQueueSend(m_queue, &s, 0);
}

void AudioManager::playAlertResident() {
    SoundType s = SOUND_ALERT_RESIDENT;
    if (m_queue) xQueueSend(m_queue, &s, 0);
}
