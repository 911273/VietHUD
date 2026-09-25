#include "SharedState.h"
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

volatile float g_boardTempC = 0; // published by ui/Dashboard.cpp, read by net/WebPortal.cpp

static SemaphoreHandle_t stateMutex;
static GnssSnapshot gnssState;
static RoadInfoSnapshot roadInfoState;
static MapViewSnapshot mapViewState;

void sharedStateInit() { stateMutex = xSemaphoreCreateMutex(); }

void gnssPublish(const GnssSnapshot &s) {
    xSemaphoreTake(stateMutex, portMAX_DELAY);
    gnssState = s;
    xSemaphoreGive(stateMutex);
}

GnssSnapshot gnssSnapshot() {
    xSemaphoreTake(stateMutex, portMAX_DELAY);
    GnssSnapshot copy = gnssState;
    xSemaphoreGive(stateMutex);
    return copy;
}

void roadInfoPublish(const RoadInfoSnapshot &s) {
    xSemaphoreTake(stateMutex, portMAX_DELAY);
    roadInfoState = s;
    xSemaphoreGive(stateMutex);
}

RoadInfoSnapshot roadInfoSnapshot() {
    xSemaphoreTake(stateMutex, portMAX_DELAY);
    RoadInfoSnapshot copy = roadInfoState;
    xSemaphoreGive(stateMutex);
    return copy;
}

void mapViewPublish(const MapViewSnapshot &s) {
    xSemaphoreTake(stateMutex, portMAX_DELAY);
    mapViewState = s;
    xSemaphoreGive(stateMutex);
}

MapViewSnapshot mapViewSnapshot() {
    xSemaphoreTake(stateMutex, portMAX_DELAY);
    MapViewSnapshot copy = mapViewState;
    xSemaphoreGive(stateMutex);
    return copy;
}
