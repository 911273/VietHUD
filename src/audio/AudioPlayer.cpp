#include "AudioPlayer.h"
#include "map/SdCardManager.h" // sdMgrExists() — self-test picks the voice set on the card
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

#include "core/Board.h" // boardAmpEnable() — VietHUD 2.8 speaker amp
#include "pincfg.h"

#define I2S_PORT        I2S_NUM_0
#define I2S_SAMPLE_RATE 16000
// I2S pins, MCLK and the amp/codec name come from the board config
// (include/boards/<board>/board_config.h). A codec (ES8311) needs a real
// MCLK = 256 x fs; a plain I2S amp (NS4168) has none (BOARD_I2S_MCLK_PIN -1).
#if BOARD_I2S_MCLK_PIN >= 0
#define I2S_MCLK_OUT    BOARD_I2S_MCLK_PIN
#else
#define I2S_MCLK_OUT    I2S_PIN_NO_CHANGE
#endif
#define AUDIO_HW_NAME   BOARD_AUDIO_NAME

// Extra digital gain on the decoded VOICE stream, on top of the 0-100% volume.
// The spoken clips are mastered well below full scale, so at 100% volume they
// were quiet with plenty of headroom left (user report 2026-09-24: "100% still
// small, no distortion"). Pushed to 2.0x. If any clip ever sounds harsh/clipped
// ("rè"), lower this; tones are already at full-scale and unaffected by it.
#define VOICE_GAIN_BOOST 2.0f

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

static const char *const kVoicePackDirs[] = {
    "male_north",     // 0: Nam Bac (GOFA)
    "female_north",   // 1: Nu Bac (GOFA)
    "male_south",     // 2: Nam Nam (GOFA)
    "female_south",   // 3: Nu Nam (GOFA)
    "male_central",   // 4: Nam Trung (GOFA)
    "female_central", // 5: Nu Trung (GOFA)
    "vi"              // 6: Goc WYN / Mac dinh
};
static int s_voicePack = 0;

void audioSetVoicePack(int packId) {
    if (packId < 0 || packId > 6) packId = 0;
    s_voicePack = packId;
    Serial.printf("[audio] voice pack set to %d (%s)\n", s_voicePack, kVoicePackDirs[s_voicePack]);
}

int audioGetVoicePack() {
    return s_voicePack;
}

void audioPreviewVoice(int packId) {
    audioSetVoicePack(packId);
    audioQueueVoice("camera_ahead.mp3");
}
#define AUDIO_QUEUE_LEN 20 // 2026-09-24: 12 -> 20, headroom for the audio self-test burst (audioSelfTest)
enum : uint8_t {
    CMD_VOICE = 0, CMD_TONE_CAMERA, CMD_TONE_OVERSPEED, CMD_TONE_SIGN, CMD_TONE_BEEP, CMD_TONE_STARTUP,
    CMD_TONE_GPSREADY, CMD_TONE_GPSLOST
};
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
        .mck_io_num = I2S_MCLK_OUT,
        .bck_io_num = I2S_BCLK_PIN,
        .ws_io_num = I2S_LRCK_PIN,
        .data_out_num = I2S_DOUT_PIN,
        .data_in_num = I2S_PIN_NO_CHANGE
    };

    i2s_driver_uninstall(I2S_PORT);
    vTaskDelay(pdMS_TO_TICKS(10));

    esp_err_t err = ESP_FAIL;
    for (int retry = 0; retry < 3; retry++) {
        err = i2s_driver_install(I2S_PORT, &i2s_config, 0, NULL);
        if (err == ESP_OK) {
            err = i2s_set_pin(I2S_PORT, &pin_config);
            if (err == ESP_OK) {
                i2s_zero_dma_buffer(I2S_PORT);
                return true;
            }
            i2s_driver_uninstall(I2S_PORT);
        }
        vTaskDelay(pdMS_TO_TICKS(15));
    }
    Serial.printf("[audio] installToneI2S failed after 3 tries: 0x%x\n", err);
    return false;
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
        Serial.printf("[audio] %s I2S audio driver initialized on BCLK=%d, LRCK=%d, DOUT=%d\n", AUDIO_HW_NAME,
                      I2S_BCLK_PIN, I2S_LRCK_PIN, I2S_DOUT_PIN);
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
    if (s_volume == 0 || freqHz == 0) return;
    if (s_i2sMutex && xSemaphoreTake(s_i2sMutex, 0) != pdTRUE) return;
    if (!s_initialized) {
        if (installToneI2S()) {
            s_initialized = true;
        } else {
            if (s_i2sMutex) xSemaphoreGive(s_i2sMutex);
            return;
        }
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
// Startup jingle (2026-09-24): a cheerful power-on melody — an ascending
// C-major arpeggio (C5-E5-G5-C6) with a short lift at the end. ~1s, plays on
// the audio task during the boot splash. Pure tones, so it works even before
// the SD card's voice files are available (or if none are installed).
static void toneStartup() {
    static const struct { uint16_t freq, durMs, gapMs; } kJingle[] = {
        {523, 110, 25}, {659, 110, 25}, {784, 110, 25}, {1047, 160, 45}, {880, 90, 20}, {1047, 240, 0}};
    for (const auto &n : kJingle) {
        audioPlayTone(n.freq, n.durMs);
        if (n.gapMs) vTaskDelay(pdMS_TO_TICKS(n.gapMs));
    }
}
// GPS ready (first fix): a short, friendly ascending 3-note motif — "device is
// ready". Deliberately different from the alert chimes (which are 2 notes).
static void toneGpsReady() {
    audioPlayTone(784, 90); vTaskDelay(pdMS_TO_TICKS(30));
    audioPlayTone(1047, 90); vTaskDelay(pdMS_TO_TICKS(30));
    audioPlayTone(1319, 160);
}
// GPS lost while driving: a descending low double-note — reads as "something's
// wrong / signal dropped", distinct from the cheerful ready motif.
static void toneGpsLost() {
    audioPlayTone(660, 150); vTaskDelay(pdMS_TO_TICKS(40));
    audioPlayTone(440, 260);
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
void audioPlayStartupJingle() { enqueueCmd(CMD_TONE_STARTUP, nullptr); }
void audioPlayGpsReady() { enqueueCmd(CMD_TONE_GPSREADY, nullptr); }
void audioPlayGpsLost() { enqueueCmd(CMD_TONE_GPSLOST, nullptr); }

// Diagnostic: enqueue every alert (tone chimes + each Vietnamese voice clip) so
// they can be heard/verified in one pass. Each voice logs "[audio] playing
// voice: ..." (or "file not found") as the task reaches it. Serial 'a' triggers
// it; the audio task drains the burst over ~20s (queue is 20 deep).
void audioSelfTest() {
    Serial.println("[audio] SELF-TEST: jingle + all chimes + all voice clips");
    audioPlayStartupJingle();
    enqueueCmd(CMD_TONE_CAMERA, nullptr);
    enqueueCmd(CMD_TONE_OVERSPEED, nullptr);
    enqueueCmd(CMD_TONE_SIGN, nullptr);
    enqueueCmd(CMD_TONE_GPSREADY, nullptr);
    enqueueCmd(CMD_TONE_GPSLOST, nullptr);
    static const char *kAll[] = {
        "welcome/voice.mp3", "speedcamera.mp3", "slowdown/voice.mp3", "tocdogioihan.mp3",
        "batdaukhudancu.mp3", "hetkhudongdancu.mp3", "camvuot.mp3", "hetcamvuot.mp3",
        "tramthuphi.mp3", "chuydentinhieugiaothong.mp3", "sapdenbienbao.mp3", "speed/50.mp3"};
    // The 2026-10-01 generated alert set (tools/gen_voice_prompts.py), played
    // instead of the old one-clip prompts when the card has it — both lists
    // together would overflow the 20-deep queue.
    static const char *kGenerated[] = {
        "welcome/voice.mp3", "camera_ahead.mp3", "overspeed.mp3", "speed_next/50.mp3",
        "resident_start.mp3", "resident_end.mp3", "no_overtake_start.mp3", "no_overtake_end.mp3",
        "toll_ahead.mp3", "light_ahead.mp3", "danger_ahead.mp3", "tunnel_ahead.mp3"};
    if (sdMgrExists("/speedmap/sounds/vi/tunnel_ahead.mp3")) {
        for (auto f : kGenerated) audioQueueVoice(f);
        return;
    }
    for (auto f : kAll) audioQueueVoice(f);
}

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
        if (c.kind == CMD_TONE_STARTUP) { toneStartup(); continue; }
        if (c.kind == CMD_TONE_GPSREADY) { toneGpsReady(); continue; }
        if (c.kind == CMD_TONE_GPSLOST) { toneGpsLost(); continue; }

        // CMD_VOICE
        char path[128];
        const char *baseDir = sdMgrGetBaseDir();
        bool found = false;

        // Requirement 2: All maps must use the startup welcome voice greeting
        // "sounds/welcome/voice.mp3" (e.g. from speedmap_gofa/speedmap/sounds/welcome/voice.mp3).
        // It must NOT be replaced by pack-specific greetings (like dan-duong-gofa).
        if (strcmp(c.file, "welcome/voice.mp3") == 0) {
            snprintf(path, sizeof(path), "%s/sounds/welcome/voice.mp3", baseDir);
            if (SD_MMC.exists(path)) {
                found = true;
            } else {
                snprintf(path, sizeof(path), "/speedmap_gofa/sounds/welcome/voice.mp3");
                if (SD_MMC.exists(path)) {
                    found = true;
                } else {
                    snprintf(path, sizeof(path), "/speedmap/sounds/welcome/voice.mp3");
                    if (SD_MMC.exists(path)) {
                        found = true;
                    } else {
                        snprintf(path, sizeof(path), "/speedmap_wyn/sounds/welcome/voice.mp3");
                        if (SD_MMC.exists(path)) {
                            found = true;
                        } else {
                            snprintf(path, sizeof(path), "/speedmap/sounds/vi/welcome/voice.mp3");
                            if (SD_MMC.exists(path)) found = true;
                        }
                    }
                }
            }
        }
        else if (s_voicePack >= 0 && s_voicePack < 6) {
            // Check 1: in current active baseDir
            snprintf(path, sizeof(path), "%s/sounds/packs/%s/%s", baseDir, kVoicePackDirs[s_voicePack], c.file);
            if (SD_MMC.exists(path)) {
                found = true;
            } else {
                // Check 2: in /speedmap_gofa
                snprintf(path, sizeof(path), "/speedmap_gofa/sounds/packs/%s/%s", kVoicePackDirs[s_voicePack], c.file);
                if (SD_MMC.exists(path)) {
                    found = true;
                } else {
                    // Check 3: in /speedmap
                    snprintf(path, sizeof(path), "/speedmap/sounds/packs/%s/%s", kVoicePackDirs[s_voicePack], c.file);
                    if (SD_MMC.exists(path)) {
                        found = true;
                    }
                }
            }
        }
        if (!found) {
            snprintf(path, sizeof(path), "%s/sounds/vi/%s", baseDir, c.file);
            if (!SD_MMC.exists(path)) {
                snprintf(path, sizeof(path), "/speedmap/sounds/vi/%s", c.file);
            }
        }
        Serial.printf("[audio] playing voice: %s\n", path);

        if (s_i2sMutex) xSemaphoreTake(s_i2sMutex, portMAX_DELAY);
        boardAmpEnable(false); // MCLK stops during the driver swap — mute the amp so it doesn't pop
        i2s_driver_uninstall(I2S_PORT);
        s_initialized = false;
        vTaskDelay(pdMS_TO_TICKS(10));

        AudioFileSourceFS source(SD_MMC, path);
        if (source.isOpen()) {
            AudioOutputI2S out;
#if BOARD_I2S_MCLK_PIN >= 0
            out.SetPinout(I2S_BCLK_PIN, I2S_LRCK_PIN, I2S_DOUT_PIN, I2S_MCLK_OUT);
#else
            out.SetPinout(I2S_BCLK_PIN, I2S_LRCK_PIN, I2S_DOUT_PIN);
#endif
            out.SetGain((float)s_volume / 100.0f * VOICE_GAIN_BOOST);
            AudioGeneratorMP3 mp3;
            bool begun = mp3.begin(&source, &out);
            boardAmpEnable(true);
            if (begun) {
                while (mp3.isRunning()) {
                    if (!mp3.loop()) mp3.stop();
                    vTaskDelay(1);
                }
            } else {
                Serial.printf("[audio] voice: mp3.begin() failed for %s\n", path);
            }
            out.stop();
        } else {
            Serial.printf("[audio] voice: file not found: %s\n", path);
        }
        vTaskDelay(pdMS_TO_TICKS(10));

        boardAmpEnable(false);
        if (installToneI2S()) s_initialized = true;
        boardAmpEnable(true);
        if (s_i2sMutex) xSemaphoreGive(s_i2sMutex);
    }
}

void audioQueueVoice(const char *filename) { enqueueCmd(CMD_VOICE, filename); }
