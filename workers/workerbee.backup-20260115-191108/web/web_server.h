#ifndef WEB_SERVER_H
#define WEB_SERVER_H

#include <Arduino.h>

// Forward declaration - actual include in .cpp to avoid header conflicts
class AsyncWebServer;

void setupWebServer();
void setWebServerWifiManager(void* wm);  // Pass wifiManager reference

// Config getters (for MQTT setup)
const char* getConfigDeviceName();
const char* getConfigMqttBroker();
uint16_t getConfigMqttPort();
const char* getConfigCompanionId();

extern AsyncWebServer webServer;

#endif // WEB_SERVER_H
