# ESP32-S2 Sennuopu DSP Preset Switcher

A WiFi-controlled preset switcher for the Sennuopu DSPs (DS-M10 Pro), running on an ESP32-S2.
The ESP32-S2 acts as a USB host, communicates with the DSP with USB HID control transfers,
and exposes a browser-based UI with WebSockets so presets can be changed from any device
on the same network.

## Compatibility

Currently only tested on the Sennuopu DS-M10 Pro.

## How it works

1. The ESP32-S2 connects to your WiFi network on boot.
2. It starts a USB host session and waits for the DSP to be plugged in via USB.
3. A HTTP server and WebSocket endpoint (`/ws`) are started; the web UI is served from LittleFS.
4. Opening the IP address in a browser shows six preset trigger buttons and a get current preset button. The current DSP connection status and the currently selected preset are displayed at the top.
5. When the DSP is detected (matched by VID/PID), a 256-byte control transfer buffer is allocated and the web UI status is updated to "Connected"
6. A HID request is sent to the DSP to get the current preset, and the web ui is updated to show the currently selected preset.
7. Clicking a preset button sends a HID `SET_REPORT` and `GET_REPORT` to the DSP, to trigger and read back the active preset — the UI updates in real time.
8. Clicking the get current preset button sends a HID `SET_REPORT` and `GET_REPORT` to the DSP, to read back the active preset on demand.


## Repository layout

```
├── esp32-s2-sennuopu-dsp-preset-switcher.ino   Main sketch
├── usb_host.hpp                                 USB host wrapper
├── secrets.h           (git-ignored)            WiFi credentials — you create this
├── secrets.h.example                            Template for secrets.h
├── data/
│   ├── index.html                               Web UI
│   ├── style.css                                Styles
│   └── app.js                                   WebSocket + UI logic
└── .gitignore
```

## First-time setup

### 1. WiFi credentials

Create a `secrets.h` file based on the `secrets.h.example` and fill in your network details:

```cpp
const char *ssid     = "YOUR_WIFI_SSID";
const char *password = "YOUR_WIFI_PASSWORD";
```

`secrets.h` is listed in `.gitignore` and will never be committed.

### 2. Arduino IDE libraries

Install these libraries via **Tools → Manage Libraries**:

| Library | Tested version |
|---|---|
| ESP Async WebServer | ≥ 1.2.3 |
| AsyncTCP | ≥ 1.1.1 |
| EspTinyUSB-extended-usb-host | ≥ 1.0.0 |

The ESP32 Arduino core (espressif/arduino-esp32) provides `WiFi`, `LittleFS`, and the `mDNS` libraries.

The USB host functionality currently requires the use of my fork of the EspTinyUSB library that extends it with USB host support. The original library may be expanded to add usb host support in the future.
Install it manually:

1. Download or clone **[Gershy13/EspTinyUSB-extended-usb-host](https://github.com/Gershy13/EspTinyUSB-extended-usb-host)**
2. In the Arduino IDE: **Sketch → Include Library → Add .ZIP Library…** and select the downloaded folder (or zip it first if cloned)

### 3. Board settings

| Setting | Value |
|---|---|
| Board | ESP32S2 Dev Module (any S2 board should work, S3 or other ESP32s with usb support may work, but untested) |
| Partition Scheme | Default 4MB with spiffs (any scheme with a data partition should work) |

### 4. Upload the filesystem

The web UI lives in the `data/` folder and must be written to the ESP32's LittleFS partition
**separately** from the sketch itself.

- **Arduino IDE 1.x** — install the
  [arduino-esp32fs-plugin](https://github.com/lorol/arduino-esp32fs-plugin),
  then use **Tools → ESP32 LittleFS Data Upload**.
- **Arduino IDE 2.x** — install the
  [LittleFS uploader](https://github.com/earlephilhower/arduino-littlefs-upload) extension,
  then press **Ctrl+Shift+P → Upload LittleFS to Pico/ESP8266/ESP32**.

Upload the `data/` folder **before or after** flashing the sketch — order does not matter.

### 5. Flash the sketch

Compile and upload normally. Open the Serial Monitor at **115200 baud**.
The IP address and mDNS hostname are printed on connection:

```
WiFi connected
IP: 192.168.1.42
mDNS started — http://esp32dsp.local
HTTP server started
```

Open `http://esp32dsp.local` in any browser on the same network.
If mDNS isn't supported by your OS or router, use the raw IP address instead.


## Adapting for a different DSP

Device-specific initalisation values are near the top of the `.ino`:

```cpp
const uint16_t TARGET_VID = 0x8888;  // USB Vendor ID of your DSP
const uint16_t TARGET_PID = 0x1234;  // USB Product ID of your DSP
const uint16_t  wIndex    = 4;       // HID interface index (wIndex in control transfer)
```

The HID payloads were reverse-engineered from Wireshark USB captures and are defined in
`createPresetPayload()` and `getCurrentPreset()`. To adapt for a different device:

- Capture USB traffic with Wireshark (USBPcap on Windows) while using the vendor software.
- Identify the `SET_REPORT` frames that trigger preset changes.
- Update `hexPrefix` and `preset_map` in `createPresetPayload()` to match.
- Update the `hexInput` in `getCurrentPreset()` with the payload that causes the device
  to return its current preset in a `GET_REPORT` response.
- Update `targetSequence` in `control_transfer_callback()` with the fixed header bytes
  of that response, and adjust the byte offset where the preset number appears.

### Preset payload format (Sennuopu DSP)

```
SET_REPORT (select preset)
  Prefix:  e0 a2 05 00 b7 00 06
  + preset byte:  01 / 02 / 03 / 04 / 05 / 06 (depending on preset number)
  + checksum:     9e / 9f / a0 / a1 / a2 / a3  (for presets 1–6 respectively)
  + padding to 256 bytes (0x00)

SET_REPORT (disconnect)
  e0 a2 05 00 b7 00 03 aa 44
  + padding to 256 bytes (0x00)

SET_REPORT (keepalive poll)
  e0 a2 04 00 b0 00 15 a5
  + padding to 256 bytes (0x00)

SET_REPORT (query current preset)
  e0 a2 05 00 b0 00 04 00 94
+ padding to 256 bytes (0x00)

GET_REPORT response signature (currently selected preset) 
  74 00 e0 a2 70 00 b0 00 00 55 55 55 55 55 00 [preset]
  Preset number is the byte immediately following the signature.
  (15 bytes, searched anywhere in the payload)
  (Preceded by an 8-byte transfer header, so the signature does not start at byte 0.)
```

## Web UI

The UI is three files in `data/`:

| File | Purpose |
|---|---|
| `index.html` | Page structure and preset buttons |
| `style.css` | Layout, button states, countdown animation |
| `app.js` | WebSocket connection, UI updates, preset commands |

After editing any of these files, re-upload the `data/` folder to LittleFS.
No sketch reflash is needed for UI-only changes.

The WebSocket sends and receives JSON:

```json
// Connection Init (ESP32 → Browser) (broadcasted to all connected clients)
{ "dspConnected": true, "currentPreset": 3 }

// Set Preset (Browser → ESP32)
{ "cmd": "setPreset", "preset": 3 }

// Get Current Preset (Browser → ESP32)
{ "cmd": "getPreset" }
```

There is also a web based serial monitor for logging and debugging purposes. 

Note: This is currently limited to 20 messages of history due to the 32 frame limit of the AsyncWebSocket send queue.

## Planned Enhancements

- Integrating ESPNow support for using another ESP to control the presets and read the statuses wirelessly
- Refactor to use proper OOP
