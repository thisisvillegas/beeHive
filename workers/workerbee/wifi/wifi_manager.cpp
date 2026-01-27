#include "wifi_manager.h"

// Static callback needs access to instance
static WifiManagerWrapper* instance = nullptr;

WifiManagerWrapper::WifiManagerWrapper() {
    instance = this;
    mqttBrokerParam = nullptr;
    memset(mqttBrokerValue, 0, sizeof(mqttBrokerValue));
    strncpy(mqttBrokerValue, MQTT_BROKER_HOST, sizeof(mqttBrokerValue) - 1);
}

void WifiManagerWrapper::setupParameters() {
    // Add custom parameter for MQTT broker
    mqttBrokerParam = new WiFiManagerParameter(
        "mqtt_broker",           // ID
        "MQTT Broker IP",        // Label
        mqttBrokerValue,         // Default value
        63                       // Max length
    );
    wifiManager.addParameter(mqttBrokerParam);
}

void WifiManagerWrapper::configModeCallback(WiFiManager* myWiFiManager) {
    Serial.println("╔════════════════════════════════════════╗");
    Serial.println("║  Worker Bee - WiFi Setup Mode          ║");
    Serial.println("╠════════════════════════════════════════╣");
    Serial.printf("║  Connect to: %s\n", WIFI_AP_NAME);
    Serial.println("║  Open: http://192.168.4.1              ║");
    Serial.println("╚════════════════════════════════════════╝");

    // TODO: Show setup screen on display
}

bool WifiManagerWrapper::begin() {
    Serial.println("[WiFi] Starting WiFiManager...");

    // Set callbacks
    wifiManager.setAPCallback(configModeCallback);

    // Configure portal timeout
    wifiManager.setConfigPortalTimeout(WIFI_CONFIG_PORTAL_TIMEOUT);

    // Set dark theme for portal (matches our UI)
    wifiManager.setClass("invert");

    // Setup custom parameters
    setupParameters();

    // Try to connect, or start config portal
    bool connected = wifiManager.autoConnect(WIFI_AP_NAME, WIFI_AP_PASSWORD);

    if (connected) {
        Serial.println("[WiFi] Connected!");
        Serial.printf("[WiFi] IP: %s\n", WiFi.localIP().toString().c_str());
        Serial.printf("[WiFi] RSSI: %d dBm\n", WiFi.RSSI());

        // Save custom parameter value
        if (mqttBrokerParam != nullptr) {
            strncpy(mqttBrokerValue, mqttBrokerParam->getValue(),
                    sizeof(mqttBrokerValue) - 1);
            Serial.printf("[WiFi] MQTT Broker: %s\n", mqttBrokerValue);
        }
    } else {
        Serial.println("[WiFi] Failed to connect!");
    }

    return connected;
}

bool WifiManagerWrapper::isConnected() const {
    return WiFi.status() == WL_CONNECTED;
}

int WifiManagerWrapper::getRSSI() const {
    return WiFi.RSSI();
}

String WifiManagerWrapper::getIPAddress() const {
    return WiFi.localIP().toString();
}

String WifiManagerWrapper::getMqttBroker() const {
    return String(mqttBrokerValue);
}

void WifiManagerWrapper::resetSettings() {
    Serial.println("[WiFi] Resetting saved credentials...");
    wifiManager.resetSettings();
}
