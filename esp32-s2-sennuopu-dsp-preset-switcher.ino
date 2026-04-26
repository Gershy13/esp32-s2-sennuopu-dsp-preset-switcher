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

bool dspConnected = false;
usb_device_handle_t dspHandle = nullptr;
int currentPreset = 0; // 1..6 as per your UI

// ====== Log ring buffer for web serial monitor (replayed to new clients on connect) ======
const int  LOG_BUF_SIZE = 50;
String     logBuf[LOG_BUF_SIZE];
int        logHead  = 0; // index where the next entry will be written
int        logCount = 0; // how many entries are currently stored

// ====== Forward declarations ======
void notifyClients();
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

// ====== Utility/Helper functions ======

// ====== Helper: send a log line to all WebSocket clients (for web serial monitor) ======
void wsLog(const String& msg) {
  String json = "{\"type\":\"log\",\"msg\":\"" + msg + "\"}";
  logBuf[logHead] = json;
  logHead = (logHead + 1) % LOG_BUF_SIZE;
  if (logCount < LOG_BUF_SIZE) logCount++;
  ws.textAll(json);
}

// ====== Helper: broadcast state to all WebSocket clients (for web ui) ======
void notifyClients() {
  String json = "{";
  json += "\"dspConnected\":";
  json += (dspConnected ? "true" : "false");
  json += ",";
  json += "\"currentPreset\":";
  json += currentPreset;
  json += "}";
  ws.textAll(json);
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
    dspConnected = false;
    dspHandle = nullptr;
    host.close();
    notifyClients();
  }
}

// ====== USB control transfer callback (handles HID GET_REPORT and SET_REPORT responses from DSP) ======
void control_transfer_callback(usb_transfer_t *transfer) {
  if (transfer->status != USB_TRANSFER_STATUS_COMPLETED) {
    Serial.printf("\n--- [Control Transfer] FAILED (status: %d) ---\n", transfer->status);
    wsLog("[Transfer] FAILED - status: " + String(transfer->status));
    return;
  }

  uint8_t *payload    = transfer->data_buffer;
  int      payload_len = transfer->actual_num_bytes;

  if (payload_len > 0) {
    Serial.printf("\n--- [Control Transfer] OK (%d bytes) ---\n", payload_len);
    Serial.print("  Data: ");
    String hexStr = "";
    char hexByte[4];
    for (int i = 0; i < payload_len; i++) {
      snprintf(hexByte, sizeof(hexByte), "%02X ", payload[i]);
      Serial.print(hexByte);
      hexStr += hexByte;
    }
    Serial.println();
    wsLog("[Transfer] OK " + String(payload_len) + "b: " + hexStr);

    // If the response matches the GET_REPORT preset response signature, parse the preset
    const uint8_t targetSequence[] = {
      0x74, 0x00, 0xE0, 0xA2, 0x70, 0x00,
      0xB0, 0x00, 0x00, 0x55, 0x55, 0x55, 0x55, 0x55, 0x00
    };
    const size_t targetLen = sizeof(targetSequence);

    if (payload_len >= (int)targetLen && memcmp(payload, targetSequence, targetLen) == 0) {
      if (payload_len > (int)targetLen) {
        currentPreset = payload[targetLen];
        Serial.printf("  Preset response — current preset: %d\n", currentPreset);
        wsLog("[Transfer] Preset response - current preset: " + String(currentPreset));
      }
      // Log 16-bit interpretation while exact format is still being determined
      if (payload_len >= (int)targetLen + 2) {
        uint16_t preset16 = (payload[targetLen] << 8) | payload[targetLen + 1];
        Serial.printf("  Preset (16-bit read): %d\n", preset16);
        wsLog("[Transfer] Preset (16-bit read): " + String(preset16));
      }
      notifyClients();
    }
  }
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

// ====== Send HID SET_REPORT to DSP to poll the DSP (not used in current flow) ======
void pollDSP() {
  if (!dspConnected || !dspHandle) return;
  // Confirmed polling/keepalive payload from Wireshark: e0 a2 04 00 b0 00 15 a5
  const char *hexInput = "e0a20400b00015a5";
  uint8_t payload[256] = {0};
  uint8_t data[128];
  size_t dataLen = hexStringToBytes(hexInput, data, sizeof(data));
  memcpy(payload, data, dataLen);
  sendSetReportToDSP(payload, sizeof(payload));
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
      Serial.printf("\n--- [WebSocket] Client %u connected ---\n", client->id());
      // Replay buffered log history to the newly connected client
      {
        int start = (logHead - logCount + LOG_BUF_SIZE) % LOG_BUF_SIZE;
        for (int i = 0; i < logCount; i++) {
          client->text(logBuf[(start + i) % LOG_BUF_SIZE]);
        }
      }
      notifyClients();
      wsLog("[WebSocket] Client " + String(client->id()) + " connected");
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

      if (msg.indexOf("\"cmd\"") >= 0 && msg.indexOf("setPreset") >= 0) {
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
              sendPresetToDSP(preset);
              delay(300); // allow DSP time to process the preset change
              getCurrentPreset();
            }
          }
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
  ws.cleanupClients();
}
