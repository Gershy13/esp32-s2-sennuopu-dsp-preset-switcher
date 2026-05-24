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
├── src/
│   ├── main.cpp                                 Main sketch
│   └── web_server.cpp / web_server.h            HTTP server, WebSocket, OTA endpoints
├── secrets.h           (git-ignored)            WiFi credentials — you create this
├── secrets.h.example                            Template for secrets.h
├── platformio.ini                               PlatformIO build configuration
├── data/
│   ├── index.html                               Web UI (structure)
│   ├── css/
│   │   ├── base.css                             Theme variables, reset, body
│   │   ├── main.css                             Headings, preset buttons, serial monitor
│   │   └── modal.css                            Settings modal, OTA, slots, reboot
│   └── js/
│       ├── websocket.js                         WebSocket connection, preset control
│       ├── serial.js                            Serial monitor log
│       └── modal.js                             Settings modal, device info, OTA upload, reboot
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

### 2. Dependencies

All dependencies are declared in `platformio.ini` and fetched automatically by PlatformIO on first build:

| Library | `lib_deps` entry |
|---|---|
| ESPAsyncWebServer | `esp32async/ESPAsyncWebServer@^3.11.0` |
| AsyncTCP | `esp32async/AsyncTCP@^3.4.10` |
| EspTinyUSB-extended-usb-host | `https://github.com/Gershy13/EspTinyUSB-extended-usb-host.git` |

The ESP32 Arduino core (espressif/arduino-esp32) provides `WiFi`, `LittleFS`, and `mDNS`.

The USB host functionality requires my fork of EspTinyUSB which extends it with USB host support. It is pulled directly from GitHub via the `lib_deps` URL above — no manual install needed.

### 3. Board settings

Tested on the **ESP32-S2-DevKitC-1 N8R2** (8 MB flash, 2 MB PSRAM). The relevant `platformio.ini` settings are:

| Setting | Value |
|---|---|
| `board` | `esp32-s2-saola-1` |
| `board_build.flash_size` | `8MB` |
| `board_upload.flash_size` | `8MB` |
| `board_build.flash_mode` | `qio` |
| `board_build.partitions` | `default_8MB.csv` (OTA-enabled, built-in PlatformIO table) |

The OTA partition scheme reserves two equal-sized app slots (`ota_0` / `ota_1`) so firmware can be updated wirelessly without a USB cable.

### 4. Upload the filesystem

The web UI lives in the `data/` folder and must be written to the ESP32's LittleFS partition
**separately** from the sketch itself.

Using PlatformIO:
```
pio run --target upload    # flash firmware
pio run --target uploadfs  # flash LittleFS
```

Upload the `data/` folder **before or after** flashing the firmware — order does not matter.

### 5. Flash the firmware

```
pio run --target upload
```

Open the serial monitor at **115200 baud** — the IP address and mDNS hostname are printed on connection:

```
WiFi connected
IP: 192.168.1.42
mDNS started — http://esp32dsp.local
HTTP server started
```

Open `http://esp32dsp.local` in any browser on the same network.
If mDNS isn't supported by your OS or router, use the raw IP address instead.


## Adapting for a different DSP

Device-specific initialisation values are near the top of `src/main.cpp`:

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

The UI is split across `data/css/` and `data/js/` subdirectories:

| File | Purpose |
|---|---|
| `index.html` | Page structure, preset buttons, settings modal |
| `css/base.css` | Theme variables (light/dark), reset, body |
| `css/main.css` | Settings cog, headings, preset buttons, serial monitor |
| `css/modal.css` | Settings modal, device info, dark mode toggle, OTA slots, reboot button |
| `js/websocket.js` | WebSocket connection, UI updates, preset commands |
| `js/serial.js` | Serial monitor log (append, clear, auto-scroll, show/hide) |
| `js/modal.js` | Settings modal, device info fetch, dark mode, OTA drag-and-drop upload, reboot + reconnect poll |

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

### Settings modal

A gear icon (top-right) opens a settings panel with:

- **Device Info** — IP address, hostname, chip model, CPU frequency, flash size, free heap, uptime (fetched live from `/api/info`).
- **Appearance** — dark/light mode toggle (persisted in `localStorage`).
- **Firmware & Storage Update** — drag-and-drop (or click-to-browse) area that accepts one or two `.bin` files. Files with `littlefs` in the name are assigned to the Storage slot; all other `.bin` files go to the Firmware slot. Both are uploaded sequentially to `/api/ota/firmware` and `/api/ota/filesystem` with a live progress bar per slot.
- **Reboot** — enabled only after at least one image has been successfully flashed. Sends a POST to `/api/restart`, then polls `/api/info` until the device is back online and automatically reloads the page.

## Planned Enhancements

- Integrating ESPNow support for using another ESP to control the presets and read the statuses wirelessly
