#pragma once
#include "AppConfig.h"

// NVS (ESP32 Preferences) persistence for AppConfig. Namespace "radarcar".
void loadConfigFromNVS(AppConfig &cfg);
void saveConfigToNVS(const AppConfig &cfg);
