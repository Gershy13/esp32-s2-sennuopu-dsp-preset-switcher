// --- Networking / Web ---
#include <WiFi.h>
#include <ESPmDNS.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>

// --- USB Host ---
#include "usb/usb_host.h"
#include "usb_host.hpp"

// --- WiFi credentials (keep secrets.h out of version control) ---
#include "secrets.h"

// ====== Constants for your DSP ======
const uint16_t TARGET_VID = 0x8888;
const uint16_t TARGET_PID = 0x1234;
const uint16_t  wIndex   = 4;

// ====== Globals ======
AsyncWebServer server(80);
AsyncWebSocket ws("/ws");

USBhost host;

bool dspConnected  = false;
bool presetFetched = false; // true once we have a confirmed preset from the DSP
usb_device_handle_t dspHandle = nullptr;
int currentPreset = 0;

// ====== Web serial monitor toggle ======
// Set to false to disable all web serial monitor output for debugging.
// When false: wsLog() is a no-op and no log history is replayed on connect.
const bool ENABLE_WEB_SERIAL = true;

// ====== Log ring buffer for web serial monitor (replayed to new clients on connect) ======
// Keep this well below WS_MAX_QUEUED_MESSAGES (32). On connect we send
// LOG_BUF_SIZE history frames + ~3 init frames, so stay under ~28.
const int  LOG_BUF_SIZE = 20;
String     logBuf[LOG_BUF_SIZE];
int        logHead  = 0; // index where the next entry will be written
int        logCount = 0; // how many entries are currently stored

// ====== Deferred actions — set in WS callbacks, executed safely in loop() ======
int      pendingPreset                  = 0;     // preset to send; 0 = none pending
bool     pendingGetPreset               = false; // true = fetch current preset from DSP
uint32_t pendingInitClients[4]          = {};    // client IDs that need their initial state/history sent
int      pendingInitClientCount         = 0;     // how many are waiting

// ====== Heap monitoring ======
const unsigned long HEAP_LOG_INTERVAL_MS = 30000;
unsigned long       lastHeapLogMs        = 0;

// ====== Forward declarations ======
String buildStateJson();
String buildLogJson(const String& msg);
void notifyClients();
void wsLog(const String& msg);
void handleWsEvent(AsyncWebSocket *server, AsyncWebSocketClient *client,
                   AwsEventType type, void *arg, uint8_t *data, size_t len);
size_t hexStringToBytes(const char *hexStr, uint8_t *out, size_t maxLen);
void sendGetReportToDSP();
void sendSetReportToDSP(const uint8_t *payload, size_t length);
void sendPresetToDSP(int preset);
void createPresetPayload(uint8_t preset, uint8_t *hid_payload);
void sendDisconnectToDSP();
void pollDSP();
void getCurrentPreset();
bool handlePresetResponse(const uint8_t *payload, int payload_len);

// ====== Utility/Helper functions ======

// ====== Helper: send a log line to all WebSocket clients (for web serial monitor) ======
void wsLog(const String& msg) {
  if (!ENABLE_WEB_SERIAL) return;
  // Store raw message string in the ring buffer for history replay
  logBuf[logHead] = msg;
  logHead = (logHead + 1) % LOG_BUF_SIZE;
  if (logCount < LOG_BUF_SIZE) logCount++;
  ws.textAll(buildLogJson(msg));
}

// Builds the current DSP state as a JSON string (used by notifyClients and WS connect handler)
String buildStateJson() {
  String json = "{\"dspConnected\":";
  json += (dspConnected ? "true" : "false");
  json += ",\"currentPreset\":";
  json += currentPreset;
  json += "}";
  return json;
}

// Builds a log message as a JSON string (used by wsLog and WS connect handler)
String buildLogJson(const String& msg) {
  return "{\"type\":\"log\",\"msg\":\"" + msg + "\"}";
}


// ====== Helper: broadcast state to all WebSocket clients (for web ui) ======
void notifyClients() {
  ws.textAll(buildStateJson());
}

// ====== Utility: parse hex string into byte array ======
size_t hexStringToBytes(const char *hexStr, uint8_t *out, size_t maxLen) {
  size_t len = strlen(hexStr);
  if (len % 2 != 0) return 0; // malformed input — must be even number of hex chars
  size_t byteCount = 0;
  for (size_t i = 0; i < len && byteCount < maxLen; i += 2) {
    char byteStr[3] = { hexStr[i], hexStr[i + 1], '\0' };
    out[byteCount++] = (uint8_t)strtol(byteStr, nullptr, 16);
  }
  return byteCount;
}

// ====== USB Host functions ======

// ====== USB client callback (device detection and connection) ======
void client_event_callback(const usb_host_client_event_msg_t *event_msg, void *arg) {
  if (!event_msg) return;

  if (event_msg->event == USB_HOST_CLIENT_EVENT_NEW_DEV) {
    host.open(event_msg);
    const usb_device_desc_t *dev_desc = host.getDeviceDescriptor();

    if (dev_desc) {
      Serial.println("\n--- [USB] Device detected ---");
      Serial.printf("  Found  VID: 0x%04X  PID: 0x%04X\n", dev_desc->idVendor, dev_desc->idProduct);
      Serial.printf("  Target VID: 0x%04X  PID: 0x%04X\n", TARGET_VID, TARGET_PID);
      wsLog("[USB] Device detected - VID: 0x" + String(dev_desc->idVendor, HEX) + " PID: 0x" + String(dev_desc->idProduct, HEX));
    }

    if (dev_desc && dev_desc->idVendor == TARGET_VID && dev_desc->idProduct == TARGET_PID) {
      Serial.println("  Target DSP matched — allocating control buffer...");
      wsLog("[USB] Target DSP matched, allocating control buffer...");

      if (host.allocateControlTransfer(256)) {
        Serial.println("  Control buffer allocated OK");
        dspConnected = true;
        dspHandle = host.deviceHandle();
        wsLog("[USB] DSP connected and ready");
        notifyClients();
      } else {
        Serial.println("  ERROR: Control buffer allocation failed — DSP will not be marked connected");
        wsLog("[USB] ERROR: Control buffer allocation failed");
      }
    } else if (dev_desc) {
      Serial.println("  Not our target device, ignoring");
    }
  } else if (event_msg->event == USB_HOST_CLIENT_EVENT_DEV_GONE) {
    Serial.println("\n--- [USB] Device disconnected ---");
    wsLog("[USB] DSP disconnected");
    dspConnected  = false;
    presetFetched = false;
    currentPreset = 0;
    dspHandle = nullptr;
    host.close();
    notifyClients();
  }
}

// ====== USB control transfer callback for HID GET_REPORT and SET_REPORT responses — dispatches to the appropriate response handler ======
void control_transfer_callback(usb_transfer_t *transfer) {
  if (transfer->status != USB_TRANSFER_STATUS_COMPLETED) {
    Serial.printf("\n--- [Control Transfer] FAILED (status: %d) ---\n", transfer->status);
    wsLog("[Transfer] FAILED - status: " + String(transfer->status));
    return;
  }

  const uint8_t *payload    = transfer->data_buffer;
  const int      payload_len = transfer->actual_num_bytes;

  if (payload_len <= 0) return;

  Serial.printf("\n--- [Control Transfer] OK (%d bytes) ---\n", payload_len);
  Serial.print("  Data: ");
  char hexByte[4];
  for (int i = 0; i < payload_len; i++) {
    snprintf(hexByte, sizeof(hexByte), "%02X ", payload[i]);
    Serial.print(hexByte);
  }
  Serial.println();

  // Dispatch to response handlers in order of priority.
  // Each handler returns true if it claimed the response.
  // Only matched handlers write to wsLog — unmatched responses (e.g. poll acks) are silently ignored.
  if (handlePresetResponse(payload, payload_len)) return;

  Serial.println("  No handler matched this response");
}

// ====== Send HID SET_REPORT control transfer to DSP ======
void sendSetReportToDSP(const uint8_t *payload, size_t length) {
  const uint16_t set_bRequest     = 0x09;
  const uint16_t set_bmRequestType = 0x21;
  const uint16_t set_wValue       = 0x0200;

  if (!dspConnected || !dspHandle) {
    Serial.println("\n--- [SET_REPORT] ERROR: DSP not connected ---");
    wsLog("[SET_REPORT] ERROR: DSP not connected");
    return;
  }

  Serial.println("\n--- [SET_REPORT] Sending ---");
  wsLog("[SET_REPORT] Sending...");

  esp_err_t err = host.sendControlTransfer(
    set_bmRequestType,
    set_bRequest,
    set_wValue,
    wIndex,
    length,
    payload,
    control_transfer_callback
  );

  if (err != ESP_OK) {
    Serial.printf("  ERROR: SET_REPORT failed (%d)\n", err);
    wsLog("[SET_REPORT] ERROR: Send failed (" + String(err) + ")");
    if (err == ESP_ERR_INVALID_SIZE) {
      Serial.println("  Data too large for buffer — check allocation");
      wsLog("[SET_REPORT] ERROR: Data too large for buffer");
    }
  } else {
    Serial.println("  SET_REPORT sent OK");
    wsLog("[SET_REPORT] Sent OK");
  }
}

// ====== Send HID GET_REPORT control transfer to DSP ======
void sendGetReportToDSP() {
  if (!dspConnected || !dspHandle) {
    Serial.println("\n--- [GET_REPORT] ERROR: DSP not connected ---");
    wsLog("[GET_REPORT] ERROR: DSP not connected");
    return;
  }

  Serial.println("\n--- [GET_REPORT] Sending ---");
  wsLog("[GET_REPORT] Sending...");

  esp_err_t err = host.sendControlTransfer(
    USB_BM_REQUEST_TYPE_DIR_IN | USB_BM_REQUEST_TYPE_TYPE_CLASS | USB_BM_REQUEST_TYPE_RECIP_INTERFACE,
    0x01,         // GET_REPORT
    (1 << 8) | 0, // Report type Input=1, Report ID=0
    wIndex,
    256,
    nullptr,
    control_transfer_callback
  );

  if (err != ESP_OK) {
    Serial.printf("  ERROR: GET_REPORT failed (%d)\n", err);
    wsLog("[GET_REPORT] ERROR: Send failed (" + String(err) + ")");
  }
}

// ====== DSP USB control transfer functions ======

// ====== Send SET_REPORT and GET_REPORT to DSP to get the current preset ======
void getCurrentPreset() {
  // Need to confirm the payload that triggers the preset-info GET_REPORT response
  const char *hexInput = "e0a20500b000040094";
  uint8_t payload[256] = {0};
  uint8_t data[128];
  size_t dataLen = hexStringToBytes(hexInput, data, sizeof(data));
  memcpy(payload, data, dataLen);

  sendSetReportToDSP(payload, sizeof(payload));
  delay(100);
  sendGetReportToDSP();
}

// ====== Response handlers for DSP GET_REPORT responses ======

// ====== Handler: handle getCurrentPreset GET_REPORT response ======
bool handlePresetResponse(const uint8_t *payload, int payload_len) {
  // Signature that identifies a preset-info GET_REPORT response.
  // Preceded by an 8-byte header, so it does not start at byte 0.
  // The preset number is the byte immediately following the signature.
  const uint8_t sig[] = {
    0x74, 0x00, 0xE0, 0xA2, 0x70, 0x00,
    0xB0, 0x00, 0x00, 0x55, 0x55, 0x55, 0x55, 0x55, 0x00
  };
  const size_t sigLen = sizeof(sig);

  for (int i = 0; i <= payload_len - (int)sigLen; i++) {
    if (memcmp(payload + i, sig, sigLen) == 0) {
      int presetOffset = i + (int)sigLen;
      if (payload_len > presetOffset) {
        currentPreset = payload[presetOffset];
        presetFetched = true;
        Serial.printf("  Preset response (signature at byte %d) — current preset: %d\n", i, currentPreset);
        wsLog("[Preset] Current preset: " + String(currentPreset));
        notifyClients();
      } else {
        Serial.println("  Preset response signature matched but payload too short for preset byte");
        wsLog("[Preset] ERROR: Signature matched but payload truncated");
      }
      return true;
    }
  }
  return false;
}

// ====== Create HID SET_REPORT payload from preset number to send to DSP ======
void createPresetPayload(uint8_t preset, uint8_t *hid_payload) {
  // Confirmed payload format from Wireshark:
  // Preset 5: e0 a2 05 00 b7 00 06 05 a2
  // Preset 6: e0 a2 05 00 b7 00 06 06 a3
  // Fixed prefix (7 bytes), then preset_num + preset_hex appended dynamically
  const char *hexPrefix = "e0a20500b70006";

  // Preset mapping: { preset_num, preset_hex }
  const uint8_t preset_map[6][2] = {
    { 0x01, 0x9e }, // P1
    { 0x02, 0x9f }, // P2
    { 0x03, 0xa0 }, // P3
    { 0x04, 0xa1 }, // P4
    { 0x05, 0xa2 }, // P5
    { 0x06, 0xa3 }  // P6
  };

  memset(hid_payload, 0x00, 256);

  if (preset < 1 || preset > 6) {
    Serial.printf("--- [createPresetPayload] ERROR: Invalid preset %d ---\n", preset);
    wsLog("[Preset] ERROR: Invalid preset number: " + String(preset));
    return;
  }

  size_t prefixLen = hexStringToBytes(hexPrefix, hid_payload, 256);
  hid_payload[prefixLen]     = preset_map[preset - 1][0]; // preset_num
  hid_payload[prefixLen + 1] = preset_map[preset - 1][1]; // preset_hex
  // remaining bytes already zero from memset
}

// ====== Send preset to DSP (builds payload dynamically based on preset number, then sends SET_REPORT and GET_REPORT) ======
void sendPresetToDSP(int preset) {
  uint8_t payload[256];
  createPresetPayload((uint8_t)preset, payload);
  sendSetReportToDSP(payload, sizeof(payload));
  delay(100);
  sendGetReportToDSP(); // acknowledge the trigger — response not parsed for preset info
}

// ====== DSP keepalive poll — sent silently every POLL_INTERVAL_MS ======
// Bypasses sendSetReportToDSP intentionally to avoid spamming the serial monitor.
// Keepalive payload from Wireshark: e0 a2 04 00 b0 00 15 a5
void pollDSP() {
  if (!dspConnected || !dspHandle) return;

  const char *hexInput = "e0a20400b00015a5";
  uint8_t payload[256] = {0};
  uint8_t data[128];
  size_t dataLen = hexStringToBytes(hexInput, data, sizeof(data));
  memcpy(payload, data, dataLen);

  esp_err_t err = host.sendControlTransfer(0x21, 0x09, 0x0200, wIndex,
                                           sizeof(payload), payload,
                                           control_transfer_callback);
  if (err != ESP_OK) {
    Serial.printf("[Poll] Keepalive failed (%d)\n", err);
    wsLog("[Poll] ERROR: Keepalive failed (" + String(err) + ")");
  }
}

// ====== Send HID SET_REPORT to DSP to disconnect from the DSP (not used in current flow) ======
void sendDisconnectToDSP() {
  // Confirmed disconnect payload from Wireshark: e0 a2 05 00 b7 00 03 aa 44
  const char *hexInput = "e0a20500b70003aa44";
  uint8_t payload[256] = {0};
  uint8_t data[128];
  size_t dataLen = hexStringToBytes(hexInput, data, sizeof(data));
  memcpy(payload, data, dataLen);
  sendSetReportToDSP(payload, sizeof(payload));
}

// ====== Web Server/WebSocket functions ======

// ====== Handle WebSocket events (connect / disconnect / messages) for web ui ======
void handleWsEvent(AsyncWebSocket *server, AsyncWebSocketClient *client,
                   AwsEventType type, void *arg, uint8_t *data, size_t len) {
  switch (type) {
    case WS_EVT_CONNECT:
      // Close excess clients immediately — before doing anything else
      ws.cleanupClients(2);
      Serial.printf("\n--- [WebSocket] Client %u connected ---\n", client->id());
      // Queue all per-client work for loop() — calling ws.textAll() or even
      // client->text() with large payloads from inside a WS event callback can
      // drop frames because the handshake may not be fully settled yet.
      if (pendingInitClientCount < 4) {
        pendingInitClients[pendingInitClientCount++] = client->id();
      }
      if (dspConnected && !presetFetched) {
        pendingGetPreset = true;
      }
      break;

    case WS_EVT_DISCONNECT:
      Serial.printf("\n--- [WebSocket] Client %u disconnected ---\n", client->id());
      wsLog("[WebSocket] Client " + String(client->id()) + " disconnected");
      break;

    case WS_EVT_DATA: {
      String msg;
      msg.reserve(len);
      for (size_t i = 0; i < len; i++) msg += (char)data[i];
      msg.trim();

      if (msg.indexOf("\"cmd\"") >= 0) {
        if (msg.indexOf("setPreset") >= 0) {
          int pKey = msg.indexOf("\"preset\"");
          if (pKey >= 0) {
            int colon = msg.indexOf(':', pKey);
            if (colon >= 0) {
              int end = msg.indexOf('}', colon);
              String pStr = msg.substring(colon + 1, end);
              pStr.trim();
              int preset = pStr.toInt();

              if (preset >= 1 && preset <= 6) {
                Serial.printf("\n--- [WebSocket] Set preset: %d ---\n", preset);
                wsLog("[WebSocket] Set preset: " + String(preset));
                pendingPreset = preset; // executed in loop() to avoid blocking the AsyncTCP task
              }
            }
          }
        } else if (msg.indexOf("getPreset") >= 0) {
          Serial.println("\n--- [WebSocket] Get current preset requested ---");
          wsLog("[WebSocket] Get current preset requested");
          getCurrentPreset();
        }
      }
      break;
    }

    case WS_EVT_PONG:
    case WS_EVT_ERROR:
      break;
  }
}

// ====== General Arduino/ESP32 functions ======
void setup() {
  Serial.begin(115200);
  delay(100);

  Serial.println("\n========================================");
  Serial.println("    ESP32 DSP Preset Switcher");
  Serial.println("========================================");

  // ---- WiFi ----
  Serial.println("\n--- [WiFi] Connecting ---");
  wsLog("[WiFi] Connecting...");
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  while (WiFi.status() != WL_CONNECTED) {
    delay(400);
    Serial.print('.');
  }

  String ip = WiFi.localIP().toString();
  Serial.println();
  Serial.println("  Connected!");
  Serial.println("  IP:   " + ip);
  wsLog("[WiFi] Connected - IP: " + ip);

  if (MDNS.begin("esp32dsp")) {
    Serial.println("  mDNS: http://esp32dsp.local");
    wsLog("[WiFi] mDNS started - http://esp32dsp.local");
  } else {
    Serial.println("  mDNS: FAILED to start");
    wsLog("[WiFi] ERROR: mDNS failed to start");
  }

  // ---- Filesystem ----
  Serial.println("\n--- [LittleFS] Mounting ---");
  if (!LittleFS.begin()) {
    Serial.println("  ERROR: Mount failed — did you upload the data folder?");
    wsLog("[LittleFS] ERROR: Mount failed");
  } else {
    Serial.println("  Mounted OK");
    wsLog("[LittleFS] Mounted OK");
  }

  // ---- USB Host ----
  Serial.println("\n--- [USB Host] Initialising ---");
  host.registerClientCb(client_event_callback);
  host.init();
  Serial.println("  USB host initialised, waiting for device...");
  wsLog("[USB] Host initialised, waiting for DSP...");

  // ---- Web Server / WebSocket ----
  Serial.println("\n--- [HTTP Server] Starting ---");
  ws.onEvent(handleWsEvent);
  server.addHandler(&ws);
  server.serveStatic("/", LittleFS, "/").setDefaultFile("index.html");
  server.onNotFound([](AsyncWebServerRequest *request) {
    request->send(404, "text/plain", "Not found");
  });

  server.begin();
  Serial.println("  Server started on port 80");
  Serial.println("\n========================================\n");
  wsLog("[HTTP] Server started - http://esp32dsp.local");
}

void loop() {
  ws.cleanupClients(2); // allow at most 2 simultaneous clients; evict older ones aggressively

  // Send initial state and log history to any newly connected clients.
  // Done here (not in WS_EVT_CONNECT) so the handshake is fully settled before we write.
  if (pendingInitClientCount > 0) {
    for (int i = 0; i < pendingInitClientCount; i++) {
      AsyncWebSocketClient *c = ws.client(pendingInitClients[i]);
      if (c && c->status() == WS_CONNECTED) {
        // Send history entries individually — individual frames from loop() are safe
        // and we know they land (the connect/heap messages below use the same path).
        // A single large batched frame gets silently dropped by AsyncWebSocket.
        if (ENABLE_WEB_SERIAL && logCount > 0) {
          int start = (logHead - logCount + LOG_BUF_SIZE) % LOG_BUF_SIZE;
          for (int i = 0; i < logCount; i++) {
            c->text(buildLogJson(logBuf[(start + i) % LOG_BUF_SIZE]));
          }
        }
        c->text(buildStateJson());
        if (ENABLE_WEB_SERIAL) {
          c->text(buildLogJson("[WebSocket] Client " + String(c->id()) + " connected"));
          c->text(buildLogJson("[Heap] Free: " + String(ESP.getFreeHeap()) + "b  Min free: " + String(ESP.getMinFreeHeap()) + "b"));
        }
      }
    }
    pendingInitClientCount = 0;
  }

  if (pendingGetPreset && dspConnected) {
    pendingGetPreset = false;
    wsLog("[Preset] Fetching current preset from DSP...");
    getCurrentPreset();
  }

  if (pendingPreset > 0) {
    int preset = pendingPreset;
    pendingPreset = 0;
    sendPresetToDSP(preset);
    delay(1000); // safe here — runs in the Arduino main task, not the AsyncTCP task
    getCurrentPreset();
  }

  unsigned long now = millis();
  if (now - lastHeapLogMs >= HEAP_LOG_INTERVAL_MS) {
    lastHeapLogMs = now;
    String heapMsg = "[Heap] Free: " + String(ESP.getFreeHeap()) + "b  Min free: " + String(ESP.getMinFreeHeap()) + "b";
    Serial.println(heapMsg);
    wsLog(heapMsg);
  }
}
