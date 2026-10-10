#include "Board.h"
#include "pincfg.h"
#include <Arduino.h>

#if defined(VIETHUD_BOARD_ES3C28P)
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

#else

void boardEarlyInit() {}
void boardAmpEnable(bool) {}

#endif
