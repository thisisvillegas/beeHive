#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include "../config.h"

/**
 * WiFi Manager Wrapper for Worker Bee
 *
 * Provides:
 *   - Captive portal for initial setup
 *   - Auto-reconnect on disconnect
 *   - Custom parameters for MQTT broker
 */

class WifiManagerWrapper {
public:
    WifiManagerWrapper();

    // Initialize and connect (blocks until connected or timeout)
    bool begin();

    // Check connection status
    bool isConnected() const;

    // Get current RSSI
    int getRSSI() const;

    // Get IP address as string
    String getIPAddress() const;

    // Get MQTT broker from saved config (if custom)
    String getMqttBroker() const;

    // Reset saved credentials (for testing)
    void resetSettings();

private:
    WiFiManager wifiManager;
    WiFiManagerParameter* mqttBrokerParam;
    char mqttBrokerValue[64];

    void setupParameters();
    void saveConfigCallback();
    static void configModeCallback(WiFiManager* myWiFiManager);
};

#endif // WIFI_MANAGER_H
