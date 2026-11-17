#include "espnow_handler.h"
#include "globals.h"
#include <esp_now.h>
#include <WiFi.h>
#include "web_server.h"

#define MAX_TRUSTED_PEERS 10
static uint8_t trustedPeers[MAX_TRUSTED_PEERS][6];
static int trustedPeerCount = 0;

static bool isTrustedPeer(const uint8_t *mac)
{
    for (int i = 0; i < trustedPeerCount; i++)
        if (memcmp(trustedPeers[i], mac, 6) == 0) return true;
    return false;
}

// Runs in the WiFi task context — keep work minimal and defer side-effects
// via the existing pending* flags that loop() picks up.
static void onDataRecv(const uint8_t *mac_addr, const uint8_t *data, int len)
{
    if (len < (int)sizeof(MsgHeader))
        return;

    const MsgHeader *hdr = reinterpret_cast<const MsgHeader *>(data);

    if (hdr->msg_type == MSG_DSP_SET_PRESET)
    {
        if (len < (int)sizeof(MsgDspPreset))
            return;

        if (!isTrustedPeer(mac_addr))
        {
            Serial.printf("[ESP-NOW] Ignored SET_PRESET from untrusted %02X:%02X:%02X:%02X:%02X:%02X\n",
                          mac_addr[0], mac_addr[1], mac_addr[2],
                          mac_addr[3], mac_addr[4], mac_addr[5]);
            return;
        }

        const MsgDspPreset *msg = reinterpret_cast<const MsgDspPreset *>(data);
        int preset = msg->preset;
        if (preset >= 1 && preset <= 6)
        {
            pendingPreset = preset;
            wsLog("[ESP-NOW] Set preset request received: " + String(preset));
        }
    }
    else if (hdr->msg_type == MSG_DSP_GET_PRESET)
    {
        if (!isTrustedPeer(mac_addr))
        {
            Serial.printf("[ESP-NOW] Ignored GET_PRESET from untrusted %02X:%02X:%02X:%02X:%02X:%02X\n",
                          mac_addr[0], mac_addr[1], mac_addr[2],
                          mac_addr[3], mac_addr[4], mac_addr[5]);
            return;
        }

        // Query DSP in loop(); onPreset callback broadcasts the result
        pendingGetPreset = true;
        wsLog("[ESP-NOW] Get preset request received");
    }
}

static void onDataSent(const uint8_t *mac_addr, esp_now_send_status_t status)
{
    if (status != ESP_NOW_SEND_SUCCESS)
        wsLog("[ESP-NOW] WARNING: broadcast send failed");
}

void espNowInit()
{
    if (esp_now_init() != ESP_OK)
    {
        Serial.println("[ESP-NOW] ERROR: init failed");
        wsLog("[ESP-NOW] ERROR: init failed");
        return;
    }

    esp_now_register_recv_cb(onDataRecv);
    esp_now_register_send_cb(onDataSent);

    // Register broadcast peer — any device on the same channel will receive sends
    esp_now_peer_info_t peerInfo = {};
    memcpy(peerInfo.peer_addr, ESPNOW_BROADCAST_ADDR, 6);
    peerInfo.channel = 0;
    peerInfo.encrypt = false;

    if (esp_now_add_peer(&peerInfo) != ESP_OK)
    {
        Serial.println("[ESP-NOW] ERROR: failed to add broadcast peer");
        wsLog("[ESP-NOW] ERROR: failed to add broadcast peer");
        return;
    }

    String info = "[ESP-NOW] Initialised — MAC: " + WiFi.macAddress()
                + "  channel: " + String(WiFi.channel());
    Serial.println(info);
    wsLog(info);
}

void espNowAddTrustedPeer(const uint8_t mac[6])
{
    if (trustedPeerCount >= MAX_TRUSTED_PEERS)
    {
        wsLog("[ESP-NOW] ERROR: trusted peer list full");
        return;
    }
    memcpy(trustedPeers[trustedPeerCount++], mac, 6);
    Serial.printf("[ESP-NOW] Trusted peer added: %02X:%02X:%02X:%02X:%02X:%02X\n",
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

void espNowBroadcastPreset(int preset)
{
    MsgDspPreset msg;
    msg.header.device_id = DEVICE_ID_DSP_CONTROLLER;
    msg.header.msg_type  = MSG_DSP_PRESET_BROADCAST;
    msg.preset           = static_cast<uint8_t>(preset);

    esp_err_t result = esp_now_send(ESPNOW_BROADCAST_ADDR,
                                    reinterpret_cast<uint8_t *>(&msg),
                                    sizeof(msg));
    if (result != ESP_OK)
        wsLog("[ESP-NOW] ERROR: broadcast failed (" + String(esp_err_to_name(result)) + ")");
}
