#include "SennuopuDSP.h"

SennuopuDSP *SennuopuDSP::_instance = nullptr;

SennuopuDSP::SennuopuDSP()
{
    _instance = this;
}

// ====== Public API ======

void SennuopuDSP::begin()
{
    _host.registerClientCb(_clientEventCb);
    _host.init();
}

void SennuopuDSP::getPreset()
{
    // Payload that triggers a preset-info GET_REPORT response (from Wireshark)
    const char *hex = "e0a20500b000040094";
    uint8_t payload[256] = {0};
    uint8_t data[128];
    size_t len = _hexToBytes(hex, data, sizeof(data));
    memcpy(payload, data, len);

    _sendSetReport(payload, sizeof(payload));
    delay(100);
    _sendGetReport();
}

void SennuopuDSP::setPreset(int preset)
{
    uint8_t payload[256];
    _createPresetPayload((uint8_t)preset, payload);
    _sendSetReport(payload, sizeof(payload));
    delay(100);
    _sendGetReport(); // acknowledge the trigger — response not parsed for preset info
}

void SennuopuDSP::poll()
{
    // Keepalive payload from Wireshark: e0 a2 04 00 b0 00 15 a5
    if (!_connected || !_handle)
        return;

    const char *hex = "e0a20400b00015a5";
    uint8_t payload[256] = {0};
    uint8_t data[128];
    size_t len = _hexToBytes(hex, data, sizeof(data));
    memcpy(payload, data, len);

    _sendSetReport(payload, sizeof(payload), true); // silent — suppress log for keepalive
}

void SennuopuDSP::sendDisconnect()
{
    // Confirmed disconnect payload from Wireshark: e0 a2 05 00 b7 00 03 aa 44
    const char *hex = "e0a20500b70003aa44";
    uint8_t payload[256] = {0};
    uint8_t data[128];
    size_t len = _hexToBytes(hex, data, sizeof(data));
    memcpy(payload, data, len);
    _sendSetReport(payload, sizeof(payload));
}

// ====== Static callbacks ======

void SennuopuDSP::_clientEventCb(const usb_host_client_event_msg_t *msg, void *arg)
{
    if (!_instance)
        return;
    if (msg->event == USB_HOST_CLIENT_EVENT_NEW_DEV)
        _instance->_handleNewDevice(msg);
    else if (msg->event == USB_HOST_CLIENT_EVENT_DEV_GONE)
        _instance->_handleDeviceGone();
}

void SennuopuDSP::_transferCb(usb_transfer_t *transfer)
{
    if (_instance)
        _instance->_handleTransferComplete(transfer);
}

// ====== Private event handlers ======

void SennuopuDSP::_handleNewDevice(const usb_host_client_event_msg_t *msg)
{
    _host.open(msg);
    const usb_device_desc_t *desc = _host.getDeviceDescriptor();

    if (desc)
    {
        Serial.println("\n--- [USB] Device detected ---");
        Serial.printf("  Found  VID: 0x%04X  PID: 0x%04X\n", desc->idVendor, desc->idProduct);
        Serial.printf("  Target VID: 0x%04X  PID: 0x%04X\n", _VID, _PID);
        _log("[USB] Device detected - VID: 0x" + String(desc->idVendor, HEX) +
             " PID: 0x" + String(desc->idProduct, HEX));
    }

    if (desc && desc->idVendor == _VID && desc->idProduct == _PID)
    {
        Serial.println("  Target DSP matched — allocating control buffer...");
        _log("[USB] Target DSP matched, allocating control buffer...");

        if (_host.allocateControlTransfer(256))
        {
            Serial.println("  Control buffer allocated OK");
            _connected = true;
            _handle = _host.deviceHandle();
            _log("[USB] DSP connected and ready");
            if (_onConnected)
                _onConnected();
        }
        else
        {
            Serial.println("  ERROR: Control buffer allocation failed — DSP will not be marked connected");
            _log("[USB] ERROR: Control buffer allocation failed");
        }
    }
    else if (desc)
    {
        Serial.println("  Not our target device, ignoring");
    }
}

void SennuopuDSP::_handleDeviceGone()
{
    Serial.println("\n--- [USB] DSP disconnected ---");
    _log("[USB] DSP disconnected");
    _connected = false;
    _presetFetched = false;
    _preset = 0;
    _handle = nullptr;
    _host.close();
    if (_onDisconnected)
        _onDisconnected();
}

void SennuopuDSP::_handleTransferComplete(usb_transfer_t *transfer)
{
    if (transfer->status != USB_TRANSFER_STATUS_COMPLETED)
    {
        Serial.printf("\n--- [Control Transfer] FAILED (status: %d) ---\n", transfer->status);
        _log("[Transfer] FAILED - status: " + String(transfer->status));
        return;
    }

    const uint8_t *payload = transfer->data_buffer;
    const int payload_len = transfer->actual_num_bytes;

    if (payload_len <= 0)
        return;

    Serial.printf("\n--- [Control Transfer] OK (%d bytes) ---\n", payload_len);
    Serial.print("  Data: ");
    char hexByte[4];
    for (int i = 0; i < payload_len; i++)
    {
        snprintf(hexByte, sizeof(hexByte), "%02X ", payload[i]);
        Serial.print(hexByte);
    }
    Serial.println();

    // Dispatch to response handlers in order of priority.
    // Each handler returns true if it claimed the response.
    // Unmatched responses (e.g. poll acks) are silently ignored.
    if (_handlePresetResponse(payload, payload_len))
        return;

    Serial.println("[DSP] No handler matched this response");
}

bool SennuopuDSP::_handlePresetResponse(const uint8_t *payload, int payload_len)
{
    // Signature that identifies a preset-info GET_REPORT response.
    // Preceded by an 8-byte header; preset number is the byte immediately after.
    const uint8_t sig[] = {
        0x74, 0x00, 0xE0, 0xA2, 0x70, 0x00,
        0xB0, 0x00, 0x00, 0x55, 0x55, 0x55, 0x55, 0x55, 0x00};
    const size_t sigLen = sizeof(sig);

    for (int i = 0; i <= payload_len - (int)sigLen; i++)
    {
        if (memcmp(payload + i, sig, sigLen) == 0)
        {
            int presetOffset = i + (int)sigLen;
            if (payload_len > presetOffset)
            {
                _preset = payload[presetOffset];
                _presetFetched = true;
                Serial.printf("[DSP] Preset response (signature at byte %d) — current preset: %d\n", i, _preset);
                _log("[DSP] Current preset: " + String(_preset));
                if (_onPreset)
                    _onPreset(_preset);
            }
            else
            {
                Serial.println("[DSP] Preset response signature matched but payload too short for preset byte");
                _log("[DSP] ERROR: Signature matched but payload truncated");
            }
            return true;
        }
    }
    return false;
}

// ====== Private helpers ======

void SennuopuDSP::_sendSetReport(const uint8_t *payload, size_t length, bool silent)
{
    if (!_connected || !_handle)
    {
        Serial.println("\n--- [SET_REPORT] ERROR: DSP not connected ---");
        if (!silent) _log("[SET_REPORT] ERROR: DSP not connected");
        return;
    }

    Serial.println("\n--- [SET_REPORT] Sending ---");
    if (!silent) _log("[SET_REPORT] Sending...");

    esp_err_t err = _host.sendControlTransfer(0x21, 0x09, 0x0200, _IFACE,
                                              length, payload, _transferCb);
    if (err != ESP_OK)
    {
        Serial.printf("  ERROR: SET_REPORT failed (%d)\n", err);
        if (!silent) _log("[SET_REPORT] ERROR: Send failed (" + String(err) + ")");
        if (err == ESP_ERR_INVALID_SIZE)
        {
            Serial.println("  Data too large for buffer — check allocation");
            if (!silent) _log("[SET_REPORT] ERROR: Data too large for buffer");
        }
    }
    else
    {
        Serial.println("  SET_REPORT sent OK");
        if (!silent) _log("[SET_REPORT] Sent OK");
    }
}

void SennuopuDSP::_sendGetReport()
{
    if (!_connected || !_handle)
    {
        Serial.println("\n--- [GET_REPORT] ERROR: DSP not connected ---");
        _log("[GET_REPORT] ERROR: DSP not connected");
        return;
    }

    Serial.println("\n--- [GET_REPORT] Sending ---");
    _log("[GET_REPORT] Sending...");

    esp_err_t err = _host.sendControlTransfer(
        USB_BM_REQUEST_TYPE_DIR_IN | USB_BM_REQUEST_TYPE_TYPE_CLASS | USB_BM_REQUEST_TYPE_RECIP_INTERFACE,
        0x01,         // GET_REPORT
        (1 << 8) | 0, // Report type Input=1, Report ID=0
        _IFACE,
        256,
        nullptr,
        _transferCb);

    if (err != ESP_OK)
    {
        Serial.printf("ERROR: GET_REPORT failed (%d)\n", err);
        _log("[GET_REPORT] ERROR: Send failed (" + String(err) + ")");
    }
}

void SennuopuDSP::_createPresetPayload(uint8_t preset, uint8_t *hid_payload)
{
    // Confirmed payload format from Wireshark:
    // Preset 5: e0 a2 05 00 b7 00 06 05 a2
    // Preset 6: e0 a2 05 00 b7 00 06 06 a3
    // Fixed prefix (7 bytes), preset_num + preset_hex appended dynamically.
    const char *hexPrefix = "e0a20500b70006";
    const uint8_t preset_map[6][2] = {
        {0x01, 0x9e}, // P1
        {0x02, 0x9f}, // P2
        {0x03, 0xa0}, // P3
        {0x04, 0xa1}, // P4
        {0x05, 0xa2}, // P5
        {0x06, 0xa3}  // P6
    };

    memset(hid_payload, 0x00, 256);

    if (preset < 1 || preset > 6)
    {
        Serial.printf("--- [createPresetPayload] ERROR: Invalid preset %d ---\n", preset);
        _log("[Preset] ERROR: Invalid preset number: " + String(preset));
        return;
    }

    size_t prefixLen = _hexToBytes(hexPrefix, hid_payload, 256);
    hid_payload[prefixLen]     = preset_map[preset - 1][0]; // preset_num
    hid_payload[prefixLen + 1] = preset_map[preset - 1][1]; // preset_hex
    // remaining bytes already zero from memset
}

size_t SennuopuDSP::_hexToBytes(const char *hex, uint8_t *out, size_t maxLen)
{
    size_t len = strlen(hex);
    if (len % 2 != 0)
        return 0; // malformed — must be even number of hex chars
    size_t byteCount = 0;
    for (size_t i = 0; i < len && byteCount < maxLen; i += 2)
    {
        char byteStr[3] = {hex[i], hex[i + 1], '\0'};
        out[byteCount++] = (uint8_t)strtol(byteStr, nullptr, 16);
    }
    return byteCount;
}

void SennuopuDSP::_log(const String &msg)
{
    if (_onLog)
        _onLog(msg);
}
