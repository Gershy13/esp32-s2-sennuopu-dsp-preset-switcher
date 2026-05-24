#pragma once
#include <Arduino.h>
#include <ESPAsyncWebServer.h>

String buildStateJson();
String buildLogJson(const String &msg);
void notifyClients();
void wsLog(const String &msg);
void handleWsEvent(AsyncWebSocket *server, AsyncWebSocketClient *client,
                   AwsEventType type, void *arg, uint8_t *data, size_t len);
