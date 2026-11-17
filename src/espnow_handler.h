#pragma once
#include <Arduino.h>
#include <stdint.h>

static const uint8_t ESPNOW_BROADCAST_ADDR[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

#define DEVICE_ID_DSP_CONTROLLER    0x01

#define MSG_DSP_PRESET_BROADCAST    0x01
#define MSG_DSP_SET_PRESET          0x02
#define MSG_DSP_GET_PRESET          0x03

struct __attribute__((packed)) MsgHeader {
    uint8_t device_id;
    uint8_t msg_type;
};

struct __attribute__((packed)) MsgDspPreset {
    MsgHeader header;
    uint8_t   preset;
};

void espNowInit();
void espNowAddTrustedPeer(const uint8_t mac[6]);
void espNowBroadcastPreset(int preset);
