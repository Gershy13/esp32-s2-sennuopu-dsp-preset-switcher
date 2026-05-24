#pragma once
#include <Arduino.h>
#include "usb/usb_host.h"
#include "usb_host.hpp"

/**
 * SennuopuDSP — Arduino/PlatformIO library for controlling a Sennuopu DSP
 * processor via USB HID on the ESP32-S2. Only tested on the Sennuopu DS-M10 Pro. Dependencies: ESP32TinyUSB-extended-usb-host.
 *
 * Usage:
 *   SennuopuDSP dsp;
 *
 *   dsp.onConnected([]()          { ... });
 *   dsp.onDisconnected([]()       { ... });
 *   dsp.onPreset([](int p)        { ... });
 *   dsp.onLog([](const String &m) { ... }); // optional
 *
 *   dsp.begin();           // call in setup()
 *   dsp.setPreset(3);      // switch to preset 3
 *   dsp.getPreset();       // request current preset from DSP
 *   dsp.poll();            // keepalive — call periodically from loop()
 *
 * This library is a wrapper around the usb_host library and provides a simple interface to control the DSP.
 * Requires the ESP32TinyUSB-extended-usb-host library to be installed.
 * Note: only one instance is supported per application (USB host singleton).
 */
class SennuopuDSP {
public:
    using VoidCb   = void (*)();
    using PresetCb = void (*)(int preset);
    using LogCb    = void (*)(const String &msg);

    SennuopuDSP();

    void begin();
    void setPreset(int preset);
    void getPreset();
    void poll();
    void sendDisconnect();

    bool isConnected()     const { return _connected; }
    bool isPresetFetched() const { return _presetFetched; }
    int  preset()          const { return _preset; }

    void onConnected(VoidCb cb)    { _onConnected = cb; }
    void onDisconnected(VoidCb cb) { _onDisconnected = cb; }
    void onPreset(PresetCb cb)     { _onPreset = cb; }
    void onLog(LogCb cb)           { _onLog = cb; }

private:
    // Static USB host callbacks — bridge to the singleton instance
    static void _clientEventCb(const usb_host_client_event_msg_t *msg, void *arg);
    static void _transferCb(usb_transfer_t *transfer);

    void _handleNewDevice(const usb_host_client_event_msg_t *msg);
    void _handleDeviceGone();
    void _handleTransferComplete(usb_transfer_t *transfer);
    bool _handlePresetResponse(const uint8_t *payload, int len);

    void   _sendSetReport(const uint8_t *payload, size_t len, bool silent = false);
    void   _sendGetReport();
    void   _createPresetPayload(uint8_t preset, uint8_t *out);
    size_t _hexToBytes(const char *hex, uint8_t *out, size_t maxLen);
    void   _log(const String &msg);

    // Device identity constants (from Wireshark capture)
    static const uint16_t _VID   = 0x8888;
    static const uint16_t _PID   = 0x1234;
    static const uint16_t _IFACE = 4; // HID interface / wIndex

    USBhost             _host;
    usb_device_handle_t _handle       = nullptr;
    bool                _connected    = false;
    bool                _presetFetched = false;
    int                 _preset       = 0;

    VoidCb   _onConnected    = nullptr;
    VoidCb   _onDisconnected = nullptr;
    PresetCb _onPreset       = nullptr;
    LogCb    _onLog          = nullptr;

    static SennuopuDSP *_instance;
};
