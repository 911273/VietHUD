// LCDwiki ES3C28P (VietHUD 2.8") — core/Board.h implementation.
// Pins/capabilities: include/boards/es3c28p/board_config.h.
#include "core/Board.h"
#include "boards/board.h"
#include "touch/FT6336Touch.h"
#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <Wire.h>

// ES8311 mono codec, slave mode, MCLK = 256 x fs on its MCLK pin. With a
// fixed 256fs ratio the codec's clock coefficients are the same for every
// sample rate this firmware uses (16 kHz tones, 22.05/44.1 kHz MP3s), so it
// is configured once here and never touched again — I2S reinstalls between
// tone and voice playback just change fs, the ratio stays 256.
// Register values follow Espressif's esp-bsp es8311 driver (coefficient row
// {pre_div 1, pre_mult 1, adc_div 1, dac_div 1, fs_mode 0, lrck 0x00FF,
// bclk_div 4, adc_osr 0x10, dac_osr 0x10}).
static bool es8311Write(uint8_t reg, uint8_t val) {
    Wire.beginTransmission(ES8311_ADDR);
    Wire.write(reg);
    Wire.write(val);
    return Wire.endTransmission() == 0;
}

static bool es8311Init() {
    if (!es8311Write(0x00, 0x1F)) return false; // reset
    delay(20);
    es8311Write(0x00, 0x00);
    es8311Write(0x00, 0x80); // power on, slave mode
    es8311Write(0x01, 0x3F); // all clocks on, MCLK from the MCLK pin
    es8311Write(0x02, 0x00); // pre_div 1, pre_mult 1
    es8311Write(0x03, 0x10); // fs_mode single speed, adc_osr
    es8311Write(0x04, 0x10); // dac_osr
    es8311Write(0x05, 0x00); // adc_div 1, dac_div 1
    es8311Write(0x06, 0x03); // bclk_div 4 (ignored in slave mode)
    es8311Write(0x07, 0x00); // lrck_h
    es8311Write(0x08, 0xFF); // lrck_l
    es8311Write(0x09, 0x0C); // SDP in: I2S, 16-bit
    es8311Write(0x0A, 0x0C); // SDP out: I2S, 16-bit
    es8311Write(0x0D, 0x01); // analog power up
    es8311Write(0x0E, 0x02); // PGA + ADC modulator
    es8311Write(0x12, 0x00); // DAC power up
    es8311Write(0x13, 0x10); // output to HP/line drive
    es8311Write(0x1C, 0x6A); // ADC EQ bypass, DC offset cancel
    es8311Write(0x37, 0x08); // DAC EQ bypass
    es8311Write(0x31, 0x00); // DAC unmute
    return es8311Write(0x32, 0xBF); // DAC volume 0 dB (volume is applied in software)
}

void boardAmpEnable(bool on) { digitalWrite(AUDIO_AMP_EN_PIN, on ? LOW : HIGH); }

void boardEarlyInit() {
    pinMode(AUDIO_AMP_EN_PIN, OUTPUT);
    boardAmpEnable(false);

    // FT6336 hardware reset, then let it boot before the touch task polls it.
    pinMode(TOUCH_RST, OUTPUT);
    digitalWrite(TOUCH_RST, LOW);
    delay(10);
    digitalWrite(TOUCH_RST, HIGH);

    Wire.begin(TOUCH_SDA, TOUCH_SCL, 400000);
    Wire.setTimeOut(20);
    bool ok = es8311Init();
    Serial.printf("[board] VietHUD 2.8 (ES3C28P): ES8311 %s\n", ok ? "OK" : "NOT RESPONDING");
    delay(200); // FT6336 needs ~200 ms after reset
    boardAmpEnable(true);
}

// ILI9341V on plain 4-wire SPI (FSPI), behind the same Arduino_Canvas wrapper
// as the 3.5" panel so rotation, the cache-friendly blit and every
// gfx->width()/height() caller work unchanged — the canvas is only 150 KB
// here and a full-frame push is ~31 ms at 40 MHz.
// ips = true: this is an IPS ILI9341V that needs display inversion (BGR order
// as the driver sets it) — confirmed on the real unit 2026-10-10 with a 4-way
// invert x RGB/BGR test pattern; without it every colour is inverted.
Arduino_GFX *boardCreatePanel() {
    Arduino_DataBus *bus = new Arduino_ESP32SPI(TFT_DC, TFT_CS, TFT_SCK, TFT_MOSI, TFT_MISO, FSPI);
    return new Arduino_ILI9341(bus, GFX_NOT_DEFINED, 0, true);
}

// FT6336 reports panel pixels directly (no calibration range). The I2C bus is
// already up — boardEarlyInit() owns it (the ES8311 sits on it too).
static FT6336Touch touch(TOUCH_ADDR, TFT_RES_W, TFT_RES_H);
bool boardTouchBegin() { return touch.begin(); }
void boardTouchSetRotation(uint8_t rotation) { touch.setRotation(rotation); }
bool boardTouchRead(uint16_t *x, uint16_t *y) { return touch.getPoint(x, y); }
