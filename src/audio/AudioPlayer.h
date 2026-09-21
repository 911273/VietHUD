#pragma once
#include <stdint.h>
#include <stdbool.h>

// Audio driver for JC3248W535 onboard NS4168 I2S Power Amplifier
// Pins: BCLK=GPIO 42, LRCK=GPIO 2, DOUT=GPIO 41
// Provides:
//  - I2S DMA initialization
//  - Crisp PCM tone/chime generation (speed-camera/sign/speeding alerts)
//  - Multi-tone alert sequences (camera chime, overspeed alarm, sign notifications)
//  - Volume control (0 - 100%)
//  - Queued Vietnamese voice playback (real MP3s from the microSD card,
//    added 2026-09-21 alongside radar removal — see AudioPlayer.cpp)

void audioInit();
void audioSetVolume(uint8_t percent);
uint8_t audioGetVolume();
void audioPlayTone(uint16_t freqHz, uint16_t durationMs);
void audioPlayCameraAlert();
void audioPlayOverspeedAlert();
void audioPlaySignNotice();
void audioPlayBeep(uint8_t count);
void audioUpdate();

// Enqueues one Vietnamese voice clip for sequential playback on its own
// FreeRTOS task (Core 0) — path is relative to /speedmap/sounds/vi/ on the
// microSD card (e.g. "tocdogioihan.mp3", "speed/80.mp3"; see
// data/speedmap/sounds/vi/ for what's actually staged there). Call it
// multiple times in a row to queue a short sequence (e.g.
// ["speedcamera.mp3", "80.mp3"]) — the task drains one clip at a time in
// the order enqueued. Non-blocking: silently drops the request if the
// queue is already full (8 deep) rather than blocking the caller
// (ui/Dashboard.cpp's refreshDashboard(), which must never stall on audio).
// A no-op if cfg.audioEnabled is false or volume is 0 — callers don't need
// to check either themselves.
void audioQueueVoice(const char *filename);
