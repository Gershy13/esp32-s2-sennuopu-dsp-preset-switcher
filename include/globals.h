#pragma once
#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include "SennuopuDSP.h"

// Configuration constants
const bool ENABLE_WEB_SERIAL = true;
const int LED_PRESET_BRIGHTNESS = 100;
const unsigned long LED_BLINK_INTERVAL_MS = 300;
const int LOG_BUF_SIZE = 100;
const int ONBOARD_BUTTON_PIN = 0;

// Shared objects (defined in main.cpp)
extern AsyncWebServer server;
extern AsyncWebSocket ws;
extern SennuopuDSP dsp;

// LED state (defined in main.cpp)
extern unsigned long lastLedMs;
extern bool ledOn;

// Log ring buffer (defined in main.cpp)
extern String logBuf[];
extern int logHead;
extern int logCount;

// Pending actions (defined in main.cpp)
extern int pendingPreset;
extern bool pendingGetPreset;
extern uint32_t pendingInitClients[];
extern int pendingInitClientCount;
