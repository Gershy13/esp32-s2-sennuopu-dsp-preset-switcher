#include "web_server.h"
#include "globals.h"

// ====== Send a log line to all WebSocket clients (web serial monitor) ======
void wsLog(const String &msg)
{
  if (!ENABLE_WEB_SERIAL)
    return;
  // Store raw message string in the ring buffer for history replay
  logBuf[logHead] = msg;
  logHead = (logHead + 1) % LOG_BUF_SIZE;
  if (logCount < LOG_BUF_SIZE)
    logCount++;
  ws.textAll(buildLogJson(msg));
}

// ====== Builds the current DSP state as a JSON string ======
String buildStateJson()
{
  String json = "{\"dspConnected\":";
  json += (dsp.isConnected() ? "true" : "false");
  json += ",\"currentPreset\":";
  json += dsp.preset();
  json += "}";
  return json;
}

// ====== Builds a log message as a JSON string ======
String buildLogJson(const String &msg)
{
  return "{\"type\":\"log\",\"msg\":\"" + msg + "\"}";
}

// ====== Broadcast current DSP state to all WebSocket clients ======
void notifyClients()
{
  ws.textAll(buildStateJson());
}

// ====== Handle WebSocket events (connect / disconnect / messages) ======
void handleWsEvent(AsyncWebSocket *server, AsyncWebSocketClient *client,
                   AwsEventType type, void *arg, uint8_t *data, size_t len)
{
  switch (type)
  {
  case WS_EVT_CONNECT:
    // Close excess clients immediately — before doing anything else
    ws.cleanupClients(2);
    Serial.printf("\n--- [WebSocket] Client %u connected ---\n", client->id());
    // Queue all per-client work for loop() — calling ws.textAll() or even
    // client->text() with large payloads from inside a WS event callback can
    // drop frames because the handshake may not be fully settled yet.
    if (pendingInitClientCount < 4)
      pendingInitClients[pendingInitClientCount++] = client->id();
    if (dsp.isConnected() && !dsp.isPresetFetched())
      pendingGetPreset = true;
    break;

  case WS_EVT_DISCONNECT:
    Serial.printf("\n--- [WebSocket] Client %u disconnected ---\n", client->id());
    wsLog("[WebSocket] Client " + String(client->id()) + " disconnected");
    break;

  case WS_EVT_DATA:
  {
    String msg;
    msg.reserve(len);
    for (size_t i = 0; i < len; i++)
      msg += (char)data[i];
    msg.trim();

    if (msg.indexOf("\"cmd\"") >= 0)
    {
      if (msg.indexOf("setPreset") >= 0)
      {
        int pKey = msg.indexOf("\"preset\"");
        if (pKey >= 0)
        {
          int colon = msg.indexOf(':', pKey);
          if (colon >= 0)
          {
            int end = msg.indexOf('}', colon);
            String pStr = msg.substring(colon + 1, end);
            pStr.trim();
            int preset = pStr.toInt();

            if (preset >= 1 && preset <= 6)
            {
              Serial.printf("\n--- [WebSocket] Set preset: %d ---\n", preset);
              wsLog("[WebSocket] Set preset: " + String(preset));
              pendingPreset = preset; // executed in loop() to avoid blocking the AsyncTCP task
            }
          }
        }
      }
      else if (msg.indexOf("getPreset") >= 0)
      {
        Serial.println("\n--- [WebSocket] Get current preset requested ---");
        wsLog("[WebSocket] Get current preset requested");
        dsp.getPreset();
      }
    }
    break;
  }

  case WS_EVT_PONG:
  case WS_EVT_ERROR:
    break;
  }
}
