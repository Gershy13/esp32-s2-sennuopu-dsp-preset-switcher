#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <Update.h>
#include <LittleFS.h>
#include "globals.h"
#include "secrets.h"
#include "web_server.h"

// --- Global definitions ---
AsyncWebServer server(80);
AsyncWebSocket ws("/ws");
SennuopuDSP dsp;

unsigned long lastLedMs = 0;
bool ledOn = false;

String logBuf[LOG_BUF_SIZE];
int logHead = 0; // log index
int logCount = 0;

int pendingPreset = 0;
bool pendingGetPreset = false;
bool pendingRestart = false;
uint32_t pendingInitClients[4] = {};
int pendingInitClientCount = 0;

void setLedToPresetColour(int preset)
{
  // Preset colour mapping:
  // 1: Red, 2: Green, 3: Blue, 4: White, 5: Yellow, 6: Magenta
  switch (preset)
  {
  case 1:
    neopixelWrite(RGB_BUILTIN, LED_PRESET_BRIGHTNESS, 0, 0);
    break;
  case 2:
    neopixelWrite(RGB_BUILTIN, 0, LED_PRESET_BRIGHTNESS, 0);
    break;
  case 3:
    neopixelWrite(RGB_BUILTIN, 0, 0, LED_PRESET_BRIGHTNESS);
    break;
  case 4:
    neopixelWrite(RGB_BUILTIN, LED_PRESET_BRIGHTNESS, LED_PRESET_BRIGHTNESS, LED_PRESET_BRIGHTNESS);
    break;
  case 5:
    neopixelWrite(RGB_BUILTIN, LED_PRESET_BRIGHTNESS, LED_PRESET_BRIGHTNESS, 0);
    break;
  case 6:
    neopixelWrite(RGB_BUILTIN, LED_PRESET_BRIGHTNESS, 0, LED_PRESET_BRIGHTNESS);
    break;
  }
}

void setup()
{
  // Init
  Serial.begin(115200);
  pinMode(ONBOARD_BUTTON_PIN, INPUT_PULLUP); // Init onboard button for preset cycling
  neopixelWrite(RGB_BUILTIN, 0, 0, 0);       // Init onboard LED to off

  // WiFi
  Serial.println("[WiFi] Connecting...");
  wsLog("[WiFi] Connecting...");
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  //WiFi.begin(ssid_dev, password_dev); // Uncomment to use dev wifi during development

  // Blink LED while waiting for WiFi connection
  while (WiFi.status() != WL_CONNECTED)
  {
    neopixelWrite(RGB_BUILTIN, 255, 0, 0);
    delay(700);
    neopixelWrite(RGB_BUILTIN, 0, 0, 0);
    delay(700);
    Serial.print('.');
  }

  // Print IP and set LED solid green
  String ip = WiFi.localIP().toString();
  Serial.println();
  Serial.println("[WiFi] Connected!");
  Serial.println("[WiFi] IP:   " + ip);
  wsLog("[WiFi] Connected - IP: " + ip);
  neopixelWrite(RGB_BUILTIN, 0, 255, 0);

  // mDNS
  if (MDNS.begin("esp32dsp"))
    Serial.println("[mDNS] started - http://esp32dsp.local");
  else
    Serial.println("[mDNS] ERROR: failed to start");

  // LittleFS
  Serial.println("[LittleFS] Mounting...");
  if (!LittleFS.begin())
    Serial.println("[LittleFS] ERROR: mount failed");
  else
    Serial.println("[LittleFS] mounted successfully");

  // DSP USB — register callbacks before begin()
  dsp.onLog([](const String &msg) {
    wsLog(msg);
  });
  dsp.onConnected([]() {
    wsLog("[USB] DSP connected and ready");
    notifyClients();
  });
  dsp.onDisconnected([]() {
    wsLog("[USB] DSP disconnected");
    notifyClients();
  });
  dsp.onPreset([](int preset) {
    wsLog("[DSP] Current preset: " + String(preset));
    notifyClients();
    setLedToPresetColour(preset);
  });

  Serial.println("[USB] Initialising...");
  dsp.begin();
  Serial.println("[USB] USB host initialised, waiting for device...");
  wsLog("[USB] Host initialised, waiting for DSP...");

  // WebUI / WebSocket server
  Serial.println("[HTTP] Server starting...");
  ws.onEvent(handleWsEvent);
  server.addHandler(&ws);
  server.serveStatic("/", LittleFS, "/").setDefaultFile("index.html").setCacheControl("no-cache");

  // Device info endpoint
  server.on("/api/info", HTTP_GET, [](AsyncWebServerRequest *request) {
    String json = "{";
    json += "\"ip\":\"" + WiFi.localIP().toString() + "\"";
    json += ",\"hostname\":\"esp32dsp\"";
    json += ",\"chip\":\"" + String(ESP.getChipModel()) + "\"";
    json += ",\"cpuFreqMHz\":" + String(ESP.getCpuFreqMHz());
    json += ",\"flashSizeMB\":" + String(ESP.getFlashChipSize() / 1048576);
    json += ",\"freeHeapKB\":" + String(ESP.getFreeHeap() / 1024);
    json += ",\"uptimeSec\":" + String(millis() / 1000);
    json += "}";
    request->send(200, "application/json", json);
  });

  // Restart endpoint — called by the UI after OTA uploads are complete
  server.on("/api/restart", HTTP_POST, [](AsyncWebServerRequest *request) {
    request->send(200, "application/json", "{\"ok\":true}");
    pendingRestart = true;
  });

  // OTA — firmware upload
  server.on(
      "/api/ota/firmware", HTTP_POST,
      [](AsyncWebServerRequest *request) {
        bool ok = !Update.hasError();
        AsyncWebServerResponse *r = request->beginResponse(
            200, "application/json",
            ok ? "{\"ok\":true}"
               : "{\"ok\":false,\"error\":\"" + String(Update.errorString()) + "\"}");
        r->addHeader("Connection", "close");
        request->send(r);
      },
      [](AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final) {
        if (!index)
        {
          wsLog("[OTA] Firmware upload started: " + filename);
          if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH))
            wsLog("[OTA] ERROR: " + String(Update.errorString()));
        }
        if (Update.write(data, len) != len)
          wsLog("[OTA] Write error: " + String(Update.errorString()));
        if (final)
        {
          if (Update.end(true))
            wsLog("[OTA] Firmware flashed — awaiting reboot command");
          else
            wsLog("[OTA] ERROR: " + String(Update.errorString()));
        }
      });

  // OTA — LittleFS filesystem upload
  server.on(
      "/api/ota/filesystem", HTTP_POST,
      [](AsyncWebServerRequest *request) {
        bool ok = !Update.hasError();
        AsyncWebServerResponse *r = request->beginResponse(
            200, "application/json",
            ok ? "{\"ok\":true}"
               : "{\"ok\":false,\"error\":\"" + String(Update.errorString()) + "\"}");
        r->addHeader("Connection", "close");
        request->send(r);
      },
      [](AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final) {
        if (!index)
        {
          wsLog("[OTA] Storage upload started: " + filename);
          LittleFS.end(); // unmount before overwriting flash
          if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_SPIFFS))
            wsLog("[OTA] ERROR: " + String(Update.errorString()));
        }
        if (Update.write(data, len) != len)
          wsLog("[OTA] Write error: " + String(Update.errorString()));
        if (final)
        {
          if (Update.end(true))
            wsLog("[OTA] Storage flashed — awaiting reboot command");
          else
            wsLog("[OTA] ERROR: " + String(Update.errorString()));
        }
      });

  server.onNotFound([](AsyncWebServerRequest *request)
                    { request->send(404, "text/plain", "Not found"); });
  server.begin();
  Serial.println("[HTTP] Web UI started");
  wsLog("[HTTP] WebUI and WebSocket server started");
}

void loop()
{
  // Restart after a successful OTA flash
  if (pendingRestart)
  {
    delay(500);
    ESP.restart();
  }

  // Set LED based on DSP connection status
  if (dsp.isConnected())
  {
    if (dsp.isPresetFetched())
      setLedToPresetColour(dsp.preset());
    else
      neopixelWrite(RGB_BUILTIN, 0, 0, LED_PRESET_BRIGHTNESS); // Solid blue — connected, preset not yet known
  }
  else
  {
    // Blink LED blue while waiting for DSP connection
    unsigned long now = millis();
    if (now - lastLedMs >= LED_BLINK_INTERVAL_MS)
    {
      lastLedMs = now;
      ledOn = !ledOn;
      neopixelWrite(RGB_BUILTIN, 0, 0, ledOn ? 255 : 0);
    }
  }

  // Limit number of simultaneous websocket clients to 4 and clean up older ones
  ws.cleanupClients(4);

  // Send log history to any newly connected clients
  if (pendingInitClientCount > 0)
  {
    for (int i = 0; i < pendingInitClientCount; i++)
    {
      AsyncWebSocketClient *c = ws.client(pendingInitClients[i]);
      if (c && c->status() == WS_CONNECTED)
      {
        // Send the full history as a single JSON frame on connection of a new client.
        if (ENABLE_WEB_SERIAL && logCount > 0)
        {
          int start = (logHead - logCount + LOG_BUF_SIZE) % LOG_BUF_SIZE;
          String history = "{\"type\":\"history\",\"msgs\":[";
          for (int j = 0; j < logCount; j++)
          {
            if (j > 0)
              history += ",";
            history += "\"";
            history += logBuf[(start + j) % LOG_BUF_SIZE];
            history += "\"";
          }
          history += "]}";
          c->text(history);
        }
        c->text(buildStateJson());
        if (ENABLE_WEB_SERIAL)
        {
          c->text(buildLogJson("[WebSocket] Client " + String(c->id()) + " connected"));
        }
      }
    }
    pendingInitClientCount = 0;
  }

  // Get preset from DSP when requested
  if (pendingGetPreset && dsp.isConnected())
  {
    pendingGetPreset = false;
    wsLog("[DSP] Getting current preset...");
    dsp.getPreset();
  }

  // Send preset to DSP if requested via WebSocket
  if (pendingPreset > 0)
  {
    int preset = pendingPreset;
    pendingPreset = 0;
    dsp.setPreset(preset);
    delay(1000); // Wait for preset to be sent to DSP and then get the latest preset
    dsp.getPreset();
  }

  // Handle onboard button press to cycle through presets
  if (digitalRead(ONBOARD_BUTTON_PIN) == LOW)
  {
    int next = dsp.preset() + 1;
    if (next > 6)
      next = 1;
    dsp.setPreset(next);
  }
}
