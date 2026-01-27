/**
 * ESP32 Minimal MQTT Test
 *
 * Stripped down firmware to isolate MQTT connectivity issues.
 * No WiFiManager, no WebServer, no complex payloads.
 */

#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>

// Config from secrets file (gitignored)
#include "secrets.h"
// secrets.h must define:
//   #define WIFI_SSID "your-ssid"
//   #define WIFI_PASS "your-password"

// MQTT config
const char* MQTT_SERVER = "192.168.0.95";  // Pi broker (persistence disabled)
const int MQTT_PORT = 1883;
const char* DEVICE_ID = "84d20c1f8a3c";

// Topics
String availabilityTopic = "homecontrol/devices/" + String(DEVICE_ID) + "/availability";
String healthTopic = "homecontrol/devices/" + String(DEVICE_ID) + "/health";

// Clients
WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);

// State
unsigned long lastHealth = 0;
unsigned long lastReconnect = 0;
int reconnectCount = 0;
bool wasConnected = false;

void setup() {
    Serial.begin(115200);
    delay(1000);
    Serial.println("\n\n=============================");
    Serial.println("  Minimal MQTT Test v1");
    Serial.println("=============================\n");

    // Connect WiFi (blocking)
    Serial.printf("Connecting to WiFi: %s\n", WIFI_SSID);
    WiFi.begin(WIFI_SSID, WIFI_PASS);

    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 60) {
        delay(500);
        Serial.print(".");
        attempts++;
    }

    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("\nWiFi connected!\n");
        Serial.printf("  IP: %s\n", WiFi.localIP().toString().c_str());
        Serial.printf("  RSSI: %d dBm\n", WiFi.RSSI());
    } else {
        Serial.println("\nWiFi FAILED - restarting in 5s");
        delay(5000);
        ESP.restart();
    }

    // Setup MQTT
    mqtt.setServer(MQTT_SERVER, MQTT_PORT);
    mqtt.setKeepAlive(60);
    mqtt.setBufferSize(256);

    Serial.printf("\nMQTT target: %s:%d\n", MQTT_SERVER, MQTT_PORT);
    Serial.println("\nStarting main loop...\n");
}

void reconnect() {
    // 5 second cooldown between attempts
    if (millis() - lastReconnect < 5000) return;
    lastReconnect = millis();
    reconnectCount++;

    // Unique client ID with timestamp
    String clientId = "esp32-" + String(DEVICE_ID) + "-" + String(millis());

    Serial.printf("[%lu] MQTT connect attempt #%d\n", millis(), reconnectCount);
    Serial.printf("[%lu]   Client ID: %s\n", millis(), clientId.c_str());
    Serial.printf("[%lu]   WiFi RSSI: %d dBm\n", millis(), WiFi.RSSI());

    // Connect with LWT
    if (mqtt.connect(clientId.c_str(), NULL, NULL,
                     availabilityTopic.c_str(), 1, true, "offline")) {
        Serial.printf("[%lu] MQTT CONNECTED!\n", millis());

        // Publish online
        bool ok = mqtt.publish(availabilityTopic.c_str(), "online", true);
        Serial.printf("[%lu]   Published 'online': %s\n", millis(), ok ? "OK" : "FAILED");

        wasConnected = true;
    } else {
        Serial.printf("[%lu] MQTT FAILED - state: %d\n", millis(), mqtt.state());
    }
}

void loop() {
    // MQTT keep-alive
    mqtt.loop();

    // Track disconnects
    bool isConnected = mqtt.connected();
    if (wasConnected && !isConnected) {
        Serial.printf("[%lu] !!! DISCONNECTED !!! state: %d\n", millis(), mqtt.state());
        wasConnected = false;
    }

    // Reconnect if needed
    if (!isConnected) {
        reconnect();
    }

    // Publish health every 5 seconds
    if (isConnected && millis() - lastHealth > 5000) {
        lastHealth = millis();

        String payload = "{\"up\":" + String(millis() / 1000) +
                         ",\"rssi\":" + String(WiFi.RSSI()) + "}";

        bool ok = mqtt.publish(healthTopic.c_str(), payload.c_str());
        Serial.printf("[%lu] Health: %s (%s)\n", millis(), payload.c_str(), ok ? "OK" : "FAILED");
    }

    delay(10);
}
