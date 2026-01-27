/**
 * Worker Bee - ESP32-S3 Touchscreen Monitor
 * Simple single-threaded design with PubSubClient (blocking MQTT)
 * BLE Gamepad support - touch buttons map to gamepad buttons
 */

#include <Arduino.h>
#include <esp_mac.h>
#include <WiFi.h>

// PubSubClient configuration - reduce blocking timeouts
#define MQTT_MAX_PACKET_SIZE 512
#define MQTT_SOCKET_TIMEOUT 1  // 1 second instead of default 15!
#include <PubSubClient.h>

#include "config.h"
#include "display/hosyond_lcd.h"
#include "wifi/wifi_manager.h"
#include "web/web_server.h"
#include <ArduinoJson.h>
#include "ui/ui_init.h"

// BLE Gamepad - DISABLED for debugging freezes
// #include "ble/ble_gamepad.h"

// WiFi manager
WifiManagerWrapper wifiManager;

// MQTT - synchronous/blocking client
WiFiClient wifiClient;
PubSubClient mqttClient(wifiClient);

bool mqttConnected = false;
char availTopic[64];
char healthTopic[64];
char discoveryTopic[64];
char stateTopic[64];
char logsTopic[64];

// Companion bee tracking
char companionAvailTopic[64];
bool companionOnline = false;

// Device ID
char deviceId[32] = {0};

// BLE connection tracking (bleGamepad is global in ble_gamepad.h)
bool bleConnected = false;

// MQTT Logging - circular buffer
#define LOG_BUFFER_SIZE 20
#define LOG_MSG_SIZE 128
char logBuffer[LOG_BUFFER_SIZE][LOG_MSG_SIZE];
int logBufferHead = 0;
int logBufferCount = 0;
uint32_t lastLogPublish = 0;
#define LOG_PUBLISH_INTERVAL 500

// Timing
uint32_t lastHealth = 0;
uint32_t lastState = 0;
uint32_t lastStats = 0;
uint32_t lastMqttReconnect = 0;

void generateDeviceId() {
    uint8_t mac[6];
    esp_efuse_mac_get_default(mac);
    snprintf(deviceId, sizeof(deviceId), "workerbee-%02x%02x%02x",
             mac[3], mac[4], mac[5]);
    Serial.printf("Device ID: %s\n", deviceId);
}

// ============== MQTT Logging ==============

void publishLog(const char* msg) {
    if (!mqttClient.connected()) return;

    JsonDocument doc;
    doc["ts"] = millis() / 1000;
    doc["msg"] = msg;

    char payload[256];
    serializeJson(doc, payload, sizeof(payload));
    mqttClient.publish(logsTopic, payload);
}

void publishBufferedLogs() {
    if (!mqttClient.connected() || logBufferCount == 0) return;
    if (millis() - lastLogPublish < LOG_PUBLISH_INTERVAL) return;

    // Publish oldest log in buffer
    int index = (logBufferHead - logBufferCount + LOG_BUFFER_SIZE) % LOG_BUFFER_SIZE;
    publishLog(logBuffer[index]);
    logBufferCount--;
    lastLogPublish = millis();
}

void mqttLog(const char* format, ...) {
    char buffer[LOG_MSG_SIZE];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);

    // Always print to Serial
    Serial.print(buffer);

    // Add to buffer for MQTT publishing
    strncpy(logBuffer[logBufferHead], buffer, LOG_MSG_SIZE - 1);
    logBuffer[logBufferHead][LOG_MSG_SIZE - 1] = '\0';

    // Remove newline for cleaner MQTT messages
    int len = strlen(logBuffer[logBufferHead]);
    if (len > 0 && logBuffer[logBufferHead][len - 1] == '\n') {
        logBuffer[logBufferHead][len - 1] = '\0';
    }

    logBufferHead = (logBufferHead + 1) % LOG_BUFFER_SIZE;
    if (logBufferCount < LOG_BUFFER_SIZE) {
        logBufferCount++;
    }
}

// ============== MQTT Publishing ==============

void publishDiscovery() {
    JsonDocument doc;
    doc["device_id"] = deviceId;
    doc["name"] = getConfigDeviceName();
    doc["type"] = DEVICE_TYPE;
    doc["model"] = "Hosyond ESP32-S3 2.8\" LCD";
    doc["firmware_version"] = FIRMWARE_VERSION;
    doc["ip_address"] = wifiManager.getIPAddress();
    doc["has_display"] = true;
    doc["display_size"] = "320x240";

    JsonArray caps = doc["capabilities"].to<JsonArray>();
    JsonObject cap = caps.add<JsonObject>();
    cap["type"] = "devices.capabilities.on_off";
    cap["instance"] = "powerSwitch";

    doc["timestamp"] = millis() / 1000;

    char payload[512];
    serializeJson(doc, payload, sizeof(payload));
    mqttClient.publish(discoveryTopic, payload, true);  // retained
    Serial.println("[MQTT] Discovery published");
}

void publishState() {
    JsonDocument doc;
    doc["powerSwitch"] = 0;  // No LED control yet
    doc["rssi"] = WiFi.RSSI();
    doc["uptime"] = millis() / 1000;
    doc["ip"] = wifiManager.getIPAddress();
    doc["timestamp"] = millis() / 1000;

    char payload[256];
    serializeJson(doc, payload, sizeof(payload));
    mqttClient.publish(stateTopic, payload, true);  // retained
}

void publishHealth() {
    JsonDocument doc;
    doc["uptime"] = millis() / 1000;
    doc["wifi_rssi"] = WiFi.RSSI();
    doc["wifi_connected"] = WiFi.isConnected();
    doc["mqtt_connected"] = mqttConnected;
    doc["mqtt_state"] = mqttConnected ? "connected" : "disconnected";
    doc["ble_connected"] = bleConnected;
    doc["ble_gamepad"] = BLE_DEVICE_NAME;
    doc["free_heap"] = ESP.getFreeHeap();
    doc["ip"] = wifiManager.getIPAddress();
    doc["firmware"] = FIRMWARE_VERSION;
    doc["timestamp"] = millis();

    char payload[384];
    serializeJson(doc, payload, sizeof(payload));

    if (mqttClient.publish(healthTopic, payload)) {
        Serial.println("[MQTT] Health published");
    } else {
        Serial.println("[MQTT] Health publish FAILED");
    }
}

// ============== MQTT Message Callback ==============

void onMqttMessage(char* topic, byte* payload, unsigned int length) {
    // Check if it's companion availability
    if (strcmp(topic, companionAvailTopic) == 0) {
        bool wasOnline = companionOnline;
        companionOnline = (length >= 6 && strncmp((char*)payload, "online", 6) == 0);

        if (companionOnline != wasOnline) {
            mqttLog("[MQTT] Companion: %s\n", companionOnline ? "ONLINE" : "offline");
            char buf[32];
            snprintf(buf, sizeof(buf), "Companion: %s", companionOnline ? "ONLINE" : "offline");
            uiManager.addLog(buf);
        }
    }
}

void mqttReconnect() {
    if (mqttClient.connected()) {
        if (!mqttConnected) {
            mqttConnected = true;
            mqttLog("[MQTT] Connected!\n");
            uiManager.addLog("MQTT: Connected");

            // Publish all initial messages
            mqttClient.publish(availTopic, "online", true);
            publishDiscovery();
            publishState();
            publishHealth();

            // Subscribe to companion bee availability
            const char* companionId = getConfigCompanionId();
            if (strlen(companionId) > 0) {
                snprintf(companionAvailTopic, sizeof(companionAvailTopic),
                         "homecontrol/devices/%s/availability", companionId);
                mqttClient.subscribe(companionAvailTopic);
                mqttLog("[MQTT] Subscribed to companion: %s\n", companionId);
            }
        }
        return;
    }

    // Not connected - try to reconnect (but don't block too long)
    mqttConnected = false;

    // Only try every 5 seconds
    if (millis() - lastMqttReconnect < 5000) {
        return;
    }
    lastMqttReconnect = millis();

    mqttLog("[MQTT] Connecting to %s:%d...\n", getConfigMqttBroker(), getConfigMqttPort());
    uiManager.addLog("MQTT: Connecting...");

    // Set will message
    if (mqttClient.connect(deviceId, NULL, NULL, availTopic, 1, true, "offline")) {
        mqttConnected = true;
        mqttLog("[MQTT] Connected!\n");
        uiManager.addLog("MQTT: Connected");

        // Publish all initial messages
        mqttClient.publish(availTopic, "online", true);
        publishDiscovery();
        publishState();
        publishHealth();

        // Subscribe to companion bee availability
        const char* companionId = getConfigCompanionId();
        if (strlen(companionId) > 0) {
            snprintf(companionAvailTopic, sizeof(companionAvailTopic),
                     "homecontrol/devices/%s/availability", companionId);
            mqttClient.subscribe(companionAvailTopic);
            mqttLog("[MQTT] Subscribed to companion: %s\n", companionId);
        }
    } else {
        mqttLog("[MQTT] Failed, rc=%d\n", mqttClient.state());
        char buf[32];
        snprintf(buf, sizeof(buf), "MQTT: err %d", mqttClient.state());
        uiManager.addLog(buf);
    }
}

// ============== Button Handler ==============

void sendLoveBomb() {
    if (!mqttClient.connected()) {
        mqttLog("[BTN] MQTT not connected\n");
        uiManager.addLog("BTN: No MQTT!");
        return;
    }

    const char* companionId = getConfigCompanionId();
    if (strlen(companionId) == 0) {
        mqttLog("[BTN] No companion set\n");
        uiManager.addLog("BTN: No companion!");
        return;
    }

    // Send love bomb to companion's /set topic
    char topic[80];
    snprintf(topic, sizeof(topic), "homecontrol/devices/%s/set", companionId);

    const char* payload = "{\"capability\":{\"instance\":\"loveBomb\",\"value\":true}}";
    if (mqttClient.publish(topic, payload)) {
        mqttLog("[BTN] Love bomb sent to %s\n", companionId);
        uiManager.addLog("Love bomb sent!");
    } else {
        mqttLog("[BTN] Love bomb failed\n");
        uiManager.addLog("BTN: Send failed!");
    }
}

void onButtonPressed(int buttonNum) {
    mqttLog("[BTN] Button %d pressed\n", buttonNum);

    // BLE gamepad DISABLED for debugging
    // if (bleGamepad.isConnected()) { ... }

    // Button 1 also sends love bomb via MQTT
    if (buttonNum == 1) {
        sendLoveBomb();
    }
    // Add more button handlers here as needed
}

void setup() {
    Serial.begin(115200);
    delay(1000);

    Serial.println("\n========================================");
    Serial.println("  Worker Bee ESP32-S3 Starting...");
    Serial.printf("  Firmware: %s\n", FIRMWARE_VERSION);
    Serial.printf("  Free heap: %lu bytes\n", ESP.getFreeHeap());
    Serial.println("========================================\n");

    generateDeviceId();

    // Initialize display first
    uiManager.begin();
    uiManager.setButtonCallback(onButtonPressed);
    uiManager.addLog("Display ready");

    // BLE Gamepad - COMPLETELY DISABLED for freeze debugging
    mqttLog("[BLE] Gamepad DISABLED\n");
    uiManager.addLog("BLE: Disabled");

    // Connect WiFi
    uiManager.addLog("WiFi connecting...");
    if (wifiManager.begin()) {
        mqttLog("[WiFi] Connected: %s\n", wifiManager.getIPAddress().c_str());
        char buf[32];
        snprintf(buf, sizeof(buf), "WiFi: %s", wifiManager.getIPAddress().c_str());
        uiManager.addLog(buf);

        // Web server
        setWebServerWifiManager(&wifiManager);
        setupWebServer();
        uiManager.addLog("Web server ready");

        // MQTT topics
        snprintf(availTopic, sizeof(availTopic), "homecontrol/devices/%s/availability", deviceId);
        snprintf(healthTopic, sizeof(healthTopic), "homecontrol/devices/%s/health", deviceId);
        snprintf(discoveryTopic, sizeof(discoveryTopic), "homecontrol/discovery/%s/config", deviceId);
        snprintf(stateTopic, sizeof(stateTopic), "homecontrol/devices/%s/state", deviceId);
        snprintf(logsTopic, sizeof(logsTopic), "homecontrol/devices/%s/logs", deviceId);

        // MQTT setup - PubSubClient is synchronous
        mqttClient.setServer(getConfigMqttBroker(), getConfigMqttPort());
        mqttClient.setBufferSize(512);  // Larger buffer for JSON payloads
        mqttClient.setKeepAlive(60);
        mqttClient.setCallback(onMqttMessage);  // For companion tracking

        char mqttBuf[48];
        snprintf(mqttBuf, sizeof(mqttBuf), "MQTT: %s:%d", getConfigMqttBroker(), getConfigMqttPort());
        uiManager.addLog(mqttBuf);
    } else {
        mqttLog("[WiFi] Failed\n");
        uiManager.addLog("WiFi: FAILED");
    }

    mqttLog("Worker Bee operational!\n");
}

// Timing diagnostics
uint32_t lastLoopTime = 0;
uint32_t loopCount = 0;
#define SLOW_LOOP_THRESHOLD 100  // Log if loop takes > 100ms

void loop() {
    uint32_t loopStart = millis();
    uint32_t now = loopStart;

    // Detect slow loops
    if (lastLoopTime > 0 && (now - lastLoopTime) > SLOW_LOOP_THRESHOLD) {
        mqttLog("[PERF] Slow loop: %lums gap at %lus\n", now - lastLoopTime, now / 1000);
    }
    lastLoopTime = now;
    loopCount++;

    // BLE tracking DISABLED - was causing issues even when not initialized
    // bool currentBleConnected = bleGamepad.isConnected();
    // if (currentBleConnected != bleConnected) { ... }

    // MQTT reconnect, loop, and log publishing - TIMED
    uint32_t t1 = millis();
    mqttReconnect();
    uint32_t t2 = millis();
    if (t2 - t1 > 50) mqttLog("[PERF] mqttReconnect: %lums\n", t2 - t1);

    mqttClient.loop();
    uint32_t t3 = millis();
    if (t3 - t2 > 50) mqttLog("[PERF] mqttClient.loop: %lums\n", t3 - t2);

    publishBufferedLogs();
    uint32_t t4 = millis();
    if (t4 - t3 > 50) mqttLog("[PERF] publishBufferedLogs: %lums\n", t4 - t3);

    // Update LVGL (handles display + touch) - TIMED
    uiManager.update();
    uint32_t t5 = millis();
    if (t5 - t4 > 50) mqttLog("[PERF] uiManager.update: %lums\n", t5 - t4);

    // Update stats on display every 500ms
    if (now - lastStats >= 500) {
        lastStats = now;
        uint32_t statsStart = millis();
        uiManager.updateSystemStats(
            wifiManager.isConnected(),
            wifiManager.getRSSI(),
            wifiManager.getIPAddress().c_str(),
            ESP.getFreeHeap(),
            mqttConnected,
            getConfigMqttBroker(),
            getConfigMqttPort(),
            deviceId,
            now / 1000,  // uptime in seconds
            FIRMWARE_VERSION,
            companionOnline,
            getConfigCompanionId()
        );
        uint32_t statsEnd = millis();
        if (statsEnd - statsStart > 50) mqttLog("[PERF] updateSystemStats: %lums\n", statsEnd - statsStart);
    }

    // Publish state every 30 seconds
    if (mqttConnected && now - lastState >= 30000) {
        lastState = now;
        uint32_t stateStart = millis();
        publishState();
        uint32_t stateEnd = millis();
        if (stateEnd - stateStart > 50) mqttLog("[PERF] publishState: %lums\n", stateEnd - stateStart);
        else mqttLog("[PERF] publishState OK: %lums at %lus\n", stateEnd - stateStart, now / 1000);
    }

    // Publish health every 5 seconds
    if (mqttConnected && now - lastHealth >= 5000) {
        lastHealth = now;
        uint32_t healthStart = millis();
        publishHealth();
        uint32_t healthEnd = millis();
        if (healthEnd - healthStart > 50) mqttLog("[PERF] publishHealth: %lums\n", healthEnd - healthStart);
    }

    // Log total loop time if slow
    uint32_t loopEnd = millis();
    if (loopEnd - loopStart > 100) {
        mqttLog("[PERF] Total loop: %lums at %lus\n", loopEnd - loopStart, now / 1000);
    }

    delay(10);  // Small delay for stability
}
