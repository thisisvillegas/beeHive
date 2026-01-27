#include "mqtt_client.h"
#include <WiFi.h>

// MQTT client
espMqttClient mqttClient;

// State
static char deviceId[32] = {0};
static char companionId[16] = {0};
static bool companionOnline = false;
static bool mqttConnected = false;
static char mqttStatus[16] = "offline";

// Topics
static char availTopic[64];
static char healthTopic[64];
static char companionAvailTopic[64];
static char companionSetTopic[64];

// Callbacks
void onMqttConnect(bool sessionPresent) {
    mqttConnected = true;
    strcpy(mqttStatus, "online");
    Serial.println("[MQTT] Connected!");

    // Publish online
    mqttClient.publish(availTopic, 1, true, "online");

    // Subscribe to companion availability if set
    if (strlen(companionId) > 0) {
        mqttClient.subscribe(companionAvailTopic, 1);
        Serial.printf("[MQTT] Subscribed to companion: %s\n", companionAvailTopic);
    }
}

void onMqttDisconnect(espMqttClientTypes::DisconnectReason reason) {
    mqttConnected = false;
    strcpy(mqttStatus, "offline");
    Serial.printf("[MQTT] Disconnected, reason: %d\n", (int)reason);
}

void onMqttMessage(const espMqttClientTypes::MessageProperties& props,
                   const char* topic, const uint8_t* payload, size_t len,
                   size_t index, size_t total) {
    // Check if companion availability message
    if (strcmp(topic, companionAvailTopic) == 0) {
        companionOnline = (len >= 6 && strncmp((char*)payload, "online", 6) == 0);
        Serial.printf("[MQTT] Companion %s is %s\n", companionId, companionOnline ? "ONLINE" : "offline");
    }
}

void setupMqtt(const char* broker, uint16_t port, const char* myDeviceId) {
    strncpy(deviceId, myDeviceId, sizeof(deviceId) - 1);

    // Build topics
    snprintf(availTopic, sizeof(availTopic), "homecontrol/devices/%s/availability", deviceId);
    snprintf(healthTopic, sizeof(healthTopic), "homecontrol/devices/%s/health", deviceId);

    // Setup callbacks
    mqttClient.onConnect(onMqttConnect);
    mqttClient.onDisconnect(onMqttDisconnect);
    mqttClient.onMessage(onMqttMessage);

    // Configure
    mqttClient.setServer(broker, port);
    mqttClient.setKeepAlive(60);
    mqttClient.setWill(availTopic, 1, true, "offline");
    mqttClient.setClientId(deviceId);

    Serial.printf("[MQTT] Setup: %s:%d as %s\n", broker, port, deviceId);

    // Connect
    mqttClient.connect();
}

void mqttLoop() {
    mqttClient.loop();
}

bool isMqttConnected() {
    return mqttConnected;
}

void setCompanionBee(const char* id) {
    strncpy(companionId, id, sizeof(companionId) - 1);
    snprintf(companionAvailTopic, sizeof(companionAvailTopic),
             "homecontrol/devices/%s/availability", companionId);
    snprintf(companionSetTopic, sizeof(companionSetTopic),
             "homecontrol/devices/%s/set", companionId);

    // Subscribe if already connected
    if (mqttConnected && strlen(companionId) > 0) {
        mqttClient.subscribe(companionAvailTopic, 1);
    }
}

bool isCompanionOnline() {
    return companionOnline;
}

void pingCompanionBee() {
    if (!mqttConnected || strlen(companionId) == 0) return;

    char payload[64];
    snprintf(payload, sizeof(payload), "{\"ping\":\"from %s\"}", deviceId);
    mqttClient.publish(companionSetTopic, 0, false, payload);
    Serial.printf("[MQTT] Pinged companion: %s\n", companionId);
}

void publishHealth(int rssi, uint32_t heap) {
    if (!mqttConnected) return;

    char payload[128];
    snprintf(payload, sizeof(payload),
             "{\"rssi\":%d,\"heap\":%lu,\"uptime\":%lu}",
             rssi, heap, millis() / 1000);
    mqttClient.publish(healthTopic, 0, false, payload);
}

void publishAvailability(bool online) {
    if (!mqttConnected) return;
    mqttClient.publish(availTopic, 1, true, online ? "online" : "offline");
}

const char* getMqttStatus() {
    return mqttStatus;
}
