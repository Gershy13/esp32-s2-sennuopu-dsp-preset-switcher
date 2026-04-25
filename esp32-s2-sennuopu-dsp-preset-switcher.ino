// --- Networking / Web ---
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>

// --- USB Host ---
#include "usb/usb_host.h"
#include "usb_host.hpp"

// --- WiFi Config---
const char     *ssid     = "YourNetworkName";
const char     *password = "YourPassword";

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

// ====== USB client callback ======
void client_event_callback(const usb_host_client_event_msg_t *event_msg, void *arg) {
  if (!event_msg) return;

  if (event_msg->event == USB_HOST_CLIENT_EVENT_NEW_DEV) {
    host.open(event_msg);
    const usb_device_desc_t *dev_desc = host.getDeviceDescriptor();

    if (dev_desc) {
      Serial.printf("Device connected - VID: 0x%04X, PID: 0x%04X\n", dev_desc->idVendor, dev_desc->idProduct);
      Serial.printf("Looking for   - VID: 0x%04X, PID: 0x%04X\n", TARGET_VID, TARGET_PID);
    }

    if (dev_desc && dev_desc->idVendor == TARGET_VID && dev_desc->idProduct == TARGET_PID) {
      Serial.println("Target DSP device connected!");
      Serial.println("Attempting to allocate 256-byte control transfer buffer...");

      if (host.allocateControlTransfer(256)) {
        Serial.println("Control transfer buffer allocated successfully");
        dspConnected = true;
        dspHandle = host.deviceHandle();
        notifyClients();
      } else {
        Serial.println("Failed to allocate control transfer buffer");
        Serial.println("Device will not be marked as connected");
      }
    }
  } else if (event_msg->event == USB_HOST_CLIENT_EVENT_DEV_GONE) {
    Serial.println("Device disconnected");
    dspConnected = false;
    dspHandle = nullptr;
    host.close();
    notifyClients();
  }
}

void control_transfer_callback(usb_transfer_t *transfer) {
  if (transfer->status != USB_TRANSFER_STATUS_COMPLETED) {
    Serial.printf("Control transfer failed with status: %d\n", transfer->status);
    return;
  }

  Serial.println("Control transfer completed successfully!");

  uint8_t *payload    = transfer->data_buffer;
  int      payload_len = transfer->actual_num_bytes;

  if (payload_len > 0) {
    Serial.print("Received data: ");
    for (int i = 0; i < payload_len; i++) {
      Serial.printf("%02X ", payload[i]);
    }
    Serial.println();

    // If the response matches the GET_REPORT preset response signature, parse the preset
    const uint8_t targetSequence[] = {
      0x74, 0x00, 0xE0, 0xA2, 0x70, 0x00,
      0xB0, 0x00, 0x00, 0x55, 0x55, 0x55, 0x55, 0x55, 0x00
    };
    const size_t targetLen = sizeof(targetSequence);

    if (payload_len >= (int)targetLen && memcmp(payload, targetSequence, targetLen) == 0) {
      if (payload_len > (int)targetLen) {
        currentPreset = payload[targetLen];
        Serial.printf("Current Preset found: %d\n", currentPreset);
      }
      // Log 16-bit interpretation while exact format is still being determined
      if (payload_len >= (int)targetLen + 2) {
        uint16_t preset16 = (payload[targetLen] << 8) | payload[targetLen + 1];
        Serial.printf("Current Preset (16-bit read): %d\n", preset16);
      }
      notifyClients();
    }
  }
}

// ====== Helper: broadcast state to all WebSocket clients ======
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

// VERIFY THIS FUNCTION WORKS, WRITTEN BY LLM
// ====== Send HID GET_REPORT to DSP ======
void sendGetReportToDSP() {
  if (!dspConnected || !dspHandle) {
    Serial.println("Cannot send GET_REPORT: Device not connected or handle invalid");
    return;
  }

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
    Serial.printf("Failed to send GET_REPORT: %d\n", err);
  }
}

// ====== Send preset to DSP (builds payload then sends SET_REPORT) ======
void sendPresetToDSP(int preset) {
  uint8_t payload[256];
  createPresetPayload((uint8_t)preset, payload);
  sendSetReportToDSP(payload, sizeof(payload));
  delay(100);
  sendGetReportToDSP();
}

// Requests a GET_REPORT from the DSP and lets control_transfer_callback parse
// the preset if the response matches the known preset-readback signature.
// NOTE: The DSP appears to send preset data in response to a preset change
// trigger, not as a standalone queryable state. This GET_REPORT is most
// useful when called shortly after sendPresetToDSP(), while the DSP still
// has readback data to return. Behaviour when called cold is unconfirmed.
void getCurrentPreset() {
  sendGetReportToDSP();
}


void sendSetReportToDSP(const uint8_t *payload, size_t length) {
    const uint16_t set_bRequest = 0x09;
    const uint16_t set_bmRequestType = 0x21;
    const uint16_t set_wValue = 0x0200;

    if (dspConnected && dspHandle) {
        esp_err_t err = host.sendControlTransfer(
            set_bmRequestType,
            set_bRequest,
            set_wValue,
            wIndex,
            length,         // size of the payload
            payload,        // pointer to payload buffer
            control_transfer_callback
        );

        if (err != ESP_OK) {
            Serial.printf("Failed to send HID SET_REPORT: %d\n", err);
            if (err == ESP_ERR_INVALID_SIZE) {
                Serial.println("Error: Data too large for buffer. Buffer may not be allocated properly.");
            }
        } else {
            Serial.println("HID SET_REPORT sent successfully!");
        }
    } else {
        Serial.println("Cannot send HID SET_REPORT: Device not connected or handle invalid");
    }
}



// ====== Handle WebSocket events (connect / disconnect / messages) ======
void handleWsEvent(AsyncWebSocket *server, AsyncWebSocketClient *client,
                   AwsEventType type, void *arg, uint8_t *data, size_t len) {
  switch (type) {
    case WS_EVT_CONNECT:
      Serial.printf("WS: client %u connected\n", client->id());
      notifyClients();
      break;

    case WS_EVT_DISCONNECT:
      Serial.printf("WS: client %u disconnected\n", client->id());
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
              Serial.printf("Setting preset via WS: %d\n", preset);
              // TODO: Add loading/timeout on buttons to limit how often preset can change (2-3 second debounce)
              sendPresetToDSP(preset);
              delay(100);
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
    Serial.println("Invalid preset number!");
    return;
  }

  size_t prefixLen = hexStringToBytes(hexPrefix, hid_payload, 256);
  hid_payload[prefixLen]     = preset_map[preset - 1][0]; // preset_num
  hid_payload[prefixLen + 1] = preset_map[preset - 1][1]; // preset_hex
  // remaining bytes already zero from memset
}

// Not used in current flow — available for future periodic keepalive polling if needed
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

void sendDisconnectToDSP() {
  // Confirmed disconnect payload from Wireshark: e0 a2 05 00 b7 00 03 aa 44
  const char *hexInput = "e0a20500b70003aa44";
  uint8_t payload[256] = {0};
  uint8_t data[128];
  size_t dataLen = hexStringToBytes(hexInput, data, sizeof(data));
  memcpy(payload, data, dataLen);
  sendSetReportToDSP(payload, sizeof(payload));
}

// ====== Minimal HTML UI (served from flash) ======
const char INDEX_HTML[] PROGMEM = R"HTML(
<!DOCTYPE html>
<html>
<head>
  <meta name="viewport" content="width=device-width, initial-scale=1.0, user-scalable=no">
  <title>ESP32 DSP Preset Switcher</title>
  <style>
    body { font-family: Arial, sans-serif; color: #444; text-align: center; }
    .title { font-size: 30px; font-weight: bold; letter-spacing: 2px; margin: 30px 0 20px; }
    .counter { font-size: 60px; font-weight: 300; margin: 10px; }
    .btn-container { display: grid; grid-template-columns: repeat(2, 1fr); gap: 15px; max-width: 400px; margin: 30px auto; }
    .status-red { font-size: 24px; margin: 10px; color: #d32f2f; }
    .status-green { font-size: 24px; margin: 10px; color: #388E3C; }
    button { padding: 20px; font-size: 18px; border: none; border-radius: 8px; background: #0078D7; color: white; cursor: pointer; transition: 0.3s; position: relative; overflow: hidden; /* so the animation stays inside the button */}
    button:hover { background: #005a9e; }
    .active { background: #34A853 !important; }
    button:disabled {
        background: #ccc !important;
        cursor: not-allowed;
        transform: none !important;
    }

    button::after {
        content: "";
        position: absolute;
        top: 0;
        right: 0;
        height: 100%;
        width: 0%;
        background: rgba(0,0,0,0.2); /* semi-transparent overlay */
        z-index: 1;
    }

    button.countdown::after {
        animation: countdownFill 2s linear forwards;
    }

    @keyframes countdownFill {
        0% { width: 0%; }
        100% { width: 100%; }
    }
  </style>
</head>
<body>
  <h1 class="title">ESP32 DSP Preset Switcher</h1>
  <h2>DSP Status: <span id="dspStatus" class="status-red">Connecting...</span></h2>
  <h2>Current Preset: <span id="currentPreset" class="counter">N/A</span></h2>

  <div class="btn-container">
    <button id="btn1" onclick="setPreset(1)">Preset 1</button>
    <button id="btn2" onclick="setPreset(2)">Preset 2</button>
    <button id="btn3" onclick="setPreset(3)">Preset 3</button>
    <button id="btn4" onclick="setPreset(4)">Preset 4</button>
    <button id="btn5" onclick="setPreset(5)">Preset 5</button>
    <button id="btn6" onclick="setPreset(6)">Preset 6</button>
  </div>

  <script>
    let ws;

    function connectWS() {
      const proto = (location.protocol === 'https:') ? 'wss' : 'ws';
      ws = new WebSocket(proto + '://' + location.host + '/ws');

      ws.onopen = () => { console.log('WS connected'); };

      ws.onmessage = (ev) => {
        try {
          const data = JSON.parse(ev.data);
          updateUI(data);
        } catch (e) {
          console.warn('Bad JSON:', ev.data);
        }
      };

      ws.onclose = () => {
        console.log('WS closed, retrying in 1s...');
        setTimeout(connectWS, 1000);
      };
    }

    function updateUI(data) {
      const statusEl = document.getElementById('dspStatus');
      statusEl.innerText = data.dspConnected ? "Connected" : "Disconnected";
      statusEl.classList.toggle('status-green', data.dspConnected);
      statusEl.classList.toggle('status-red', !data.dspConnected);

      document.getElementById('currentPreset').innerText = data.currentPreset || 'N/A';

      for (let i = 1; i <= 6; i++) {
        document.getElementById('btn' + i).classList.toggle('active', i === data.currentPreset);
      }
    }

    function setPreset(num) {
        if (!ws || ws.readyState !== WebSocket.OPEN) return;

        const buttons = document.querySelectorAll('button');
        
        // Disable all buttons and start countdown animation
        buttons.forEach(btn => {
            btn.disabled = true;
            btn.classList.add('countdown');
        });

        // Send command to ESP32
        ws.send(JSON.stringify({ cmd: "setPreset", preset: num }));

        // After 2s, re-enable and reset animations
        setTimeout(() => {
            buttons.forEach(btn => {
            btn.disabled = false;
            btn.classList.remove('countdown'); // Remove so next time it can re-trigger
            });
        }, 2000);
    }

    connectWS();
  </script>
</body>
</html>
)HTML";

// ====== HTTP Handlers ======
void handleRoot(AsyncWebServerRequest *request) {
  request->send_P(200, "text/html", INDEX_HTML);
}

void setup() {
  Serial.begin(115200);
  delay(100);

  // ---- WiFi ----
  Serial.println("Connecting to WiFi...");
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  while (WiFi.status() != WL_CONNECTED) {
    delay(400);
    Serial.print('.');
  }

  Serial.println();
  Serial.println("WiFi connected");
  Serial.print("IP: ");
  Serial.println(WiFi.localIP());

  // ---- USB Host ----
  host.registerClientCb(client_event_callback);
  host.init();

  // ---- Web Server / WebSocket ----
  ws.onEvent(handleWsEvent);
  server.addHandler(&ws);
  server.on("/", HTTP_GET, handleRoot);
  server.onNotFound([](AsyncWebServerRequest *request) {
    request->send(404, "text/plain", "Not found");
  });

  server.begin();
  Serial.println("HTTP server started");
}

void loop() {
  // ESPAsyncWebServer and USB host task run independently
  // ws.cleanupClients(); // uncomment if aggressive client cleanup is needed
}
