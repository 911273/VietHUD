#pragma once
#include <Arduino.h>

#pragma pack(push, 1)
struct HanoiAlertRecord {
    int32_t latE7;
    int32_t lonE7;
    uint16_t heading;
    uint8_t type;
    uint8_t speed;
};
#pragma pack(pop)

#define HANOI_ALERT_COUNT 40386
extern const HanoiAlertRecord HANOI_ALERTS[] PROGMEM;
