/**
 * ESP32 Homecontrol - Async Version
 *
 * Non-blocking firmware using:
 * - espMqttClient (async MQTT)
 * - ESPAsyncWebServer (async HTTP)
 *
 * Much faster and more responsive than the blocking version.
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <espMqttClient.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <ESPmDNS.h>

// ============== Configuration ==============

#define LED_PIN 2
#define DEVICE_TYPE "esp32"
#define FIRMWARE_VERSION "2.0.0-async"
#define DEFAULT_MQTT_PORT 1883

// ============== Global Objects ==============

espMqttClient mqttClient;
AsyncWebServer webServer(80);
Preferences preferences;
WiFiManager wifiManager;

// Timers for non-blocking periodic tasks
unsigned long lastHealthPublish = 0;
unsigned long lastStatePublish = 0;
unsigned long lastReconnectAttempt = 0;

// ============== Device State ==============

struct DeviceConfig {
    char deviceName[32];
    char mqttServer[64];
    int mqttPort;
    char mqttUser[32];
    char mqttPass[32];
    char topicPrefix[32];
} config;

struct DeviceState {
    bool ledOn = false;
    bool mqttConnected = false;
    unsigned long uptime = 0;
    int rssi = 0;
} state;

String deviceId;
String discoveryTopic;
String stateTopic;
String commandTopic;
String availabilityTopic;
String healthTopic;
String logsTopic;

// Debug tracking
int reconnectCount = 0;
String lastError = "none";
unsigned long lastConnectTime = 0;
unsigned long connectionDuration = 0;
String currentClientId;

// ============== Forward Declarations ==============

void setupMQTT();
void connectMQTT();
void publishDiscovery();
void publishState();
void publishHealth();
void loadConfig();
void saveConfig();
String getDeviceId();
void setupWebServer();

// ============== MQTT Callbacks (Async) ==============

void onMqttConnect(bool sessionPresent) {
    Serial.printf("[MQTT] Connected! Session present: %d\n", sessionPresent);
    state.mqttConnected = true;
    lastConnectTime = millis();
    lastError = "none";

    // Subscribe to command topic
    uint16_t packetId = mqttClient.subscribe(commandTopic.c_str(), 1);
    Serial.printf("[MQTT] Subscribed to %s (packet %d)\n", commandTopic.c_str(), packetId);

    // Publish online status
    mqttClient.publish(availabilityTopic.c_str(), 1, true, "online");

    // Publish discovery and initial state
    publishDiscovery();
    publishState();
    publishHealth();
}

void onMqttDisconnect(espMqttClientTypes::DisconnectReason reason) {
    Serial.printf("[MQTT] Disconnected! Reason: %d\n", (int)reason);

    if (state.mqttConnected) {
        connectionDuration = millis() - lastConnectTime;
        Serial.printf("[MQTT] Was connected for %lu ms\n", connectionDuration);
    }

    state.mqttConnected = false;

    switch (reason) {
        case espMqttClientTypes::DisconnectReason::TCP_DISCONNECTED:
            lastError = "TCP_DISCONNECTED";
            break;
        case espMqttClientTypes::DisconnectReason::MQTT_UNACCEPTABLE_PROTOCOL_VERSION:
            lastError = "BAD_PROTOCOL";
            break;
        case espMqttClientTypes::DisconnectReason::MQTT_IDENTIFIER_REJECTED:
            lastError = "ID_REJECTED";
            break;
        case espMqttClientTypes::DisconnectReason::MQTT_SERVER_UNAVAILABLE:
            lastError = "SERVER_UNAVAILABLE";
            break;
        case espMqttClientTypes::DisconnectReason::MQTT_MALFORMED_CREDENTIALS:
            lastError = "BAD_CREDENTIALS";
            break;
        case espMqttClientTypes::DisconnectReason::MQTT_NOT_AUTHORIZED:
            lastError = "NOT_AUTHORIZED";
            break;
        default:
            lastError = "UNKNOWN";
    }
}

void onMqttMessage(const espMqttClientTypes::MessageProperties& properties,
                   const char* topic, const uint8_t* payload, size_t len,
                   size_t index, size_t total) {
    Serial.printf("[MQTT] Message on %s (%d bytes)\n", topic, len);

    // Parse JSON payload
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, payload, len);

    if (error) {
        Serial.printf("[MQTT] JSON parse error: %s\n", error.c_str());
        return;
    }

    // Handle control commands
    if (doc["capability"].is<JsonObject>()) {
        JsonObject capability = doc["capability"];
        const char* instance = capability["instance"];

        if (instance && strcmp(instance, "powerSwitch") == 0) {
            int value = capability["value"];
            state.ledOn = (value == 1);
            digitalWrite(LED_PIN, state.ledOn ? HIGH : LOW);
            Serial.printf("[LED] Set to: %s\n", state.ledOn ? "ON" : "OFF");

            // Publish updated state immediately
            publishState();
        }
    }
}

void onMqttPublish(uint16_t packetId) {
    // Optional: track successful publishes
    // Serial.printf("[MQTT] Publish acknowledged: %d\n", packetId);
}

// ============== Setup ==============

void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println("\n\n=== ESP32 Homecontrol (Async) ===");
    Serial.printf("Firmware: %s\n", FIRMWARE_VERSION);

    // Initialize LED
    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, LOW);

    // Generate device ID
    deviceId = getDeviceId();
    Serial.printf("Device ID: %s\n", deviceId.c_str());

    // Load configuration
    loadConfig();

    // Setup WiFi Manager
    WiFiManagerParameter mqttServerParam("mqtt_server", "MQTT Server", config.mqttServer, 64);
    WiFiManagerParameter mqttPortParam("mqtt_port", "MQTT Port", String(config.mqttPort).c_str(), 6);
    WiFiManagerParameter deviceNameParam("device_name", "Device Name", config.deviceName, 32);

    wifiManager.addParameter(&mqttServerParam);
    wifiManager.addParameter(&mqttPortParam);
    wifiManager.addParameter(&deviceNameParam);

    String apName = "ESP32-Setup-" + deviceId.substring(deviceId.length() - 4);
    wifiManager.setConfigPortalTimeout(180);

    Serial.println("Starting WiFi Manager...");
    if (!wifiManager.autoConnect(apName.c_str())) {
        Serial.println("Failed to connect, restarting...");
        delay(3000);
        ESP.restart();
    }

    // Save any updated parameters
    strcpy(config.mqttServer, mqttServerParam.getValue());
    config.mqttPort = atoi(mqttPortParam.getValue());
    strcpy(config.deviceName, deviceNameParam.getValue());
    saveConfig();

    Serial.printf("WiFi connected! IP: %s\n", WiFi.localIP().toString().c_str());

    // Setup mDNS
    String mdnsName = "esp32-" + deviceId.substring(deviceId.length() - 6);
    if (MDNS.begin(mdnsName.c_str())) {
        Serial.printf("mDNS: http://%s.local\n", mdnsName.c_str());
        MDNS.addService("http", "tcp", 80);
    }

    // Build MQTT topics
    String prefix = String(config.topicPrefix);
    discoveryTopic = prefix + "/discovery/" + deviceId + "/config";
    stateTopic = prefix + "/devices/" + deviceId + "/state";
    commandTopic = prefix + "/devices/" + deviceId + "/set";
    availabilityTopic = prefix + "/devices/" + deviceId + "/availability";
    healthTopic = prefix + "/devices/" + deviceId + "/health";
    logsTopic = prefix + "/devices/" + deviceId + "/logs";

    // Setup MQTT (async)
    setupMQTT();

    // Setup Web Server (async)
    setupWebServer();

    Serial.println("Setup complete!\n");
}

// ============== Main Loop ==============

void loop() {
    // Update state
    state.uptime = millis() / 1000;
    state.rssi = WiFi.RSSI();

    // Reconnect MQTT if needed (non-blocking, with 5s cooldown)
    if (!state.mqttConnected && WiFi.isConnected()) {
        if (millis() - lastReconnectAttempt > 5000) {
            lastReconnectAttempt = millis();
            connectMQTT();
        }
    }

    // Publish health every 5 seconds
    if (state.mqttConnected && millis() - lastHealthPublish > 5000) {
        lastHealthPublish = millis();
        publishHealth();
    }

    // Publish state every 30 seconds
    if (state.mqttConnected && millis() - lastStatePublish > 30000) {
        lastStatePublish = millis();
        publishState();
    }

    // No delay needed! Async handles everything in background.
    // This loop runs as fast as possible for maximum responsiveness.
}

// ============== MQTT Functions ==============

void setupMQTT() {
    // Set callbacks
    mqttClient.onConnect(onMqttConnect);
    mqttClient.onDisconnect(onMqttDisconnect);
    mqttClient.onMessage(onMqttMessage);
    mqttClient.onPublish(onMqttPublish);

    // Configure broker
    mqttClient.setServer(config.mqttServer, config.mqttPort);

    // Set credentials if provided
    if (strlen(config.mqttUser) > 0) {
        mqttClient.setCredentials(config.mqttUser, config.mqttPass);
    }

    // Set keep-alive and client ID
    mqttClient.setKeepAlive(60);

    // Set Last Will Testament
    mqttClient.setWill(availabilityTopic.c_str(), 1, true, "offline");

    Serial.printf("[MQTT] Configured for %s:%d\n", config.mqttServer, config.mqttPort);
}

void connectMQTT() {
    reconnectCount++;
    currentClientId = "esp32-" + deviceId + "-" + String(millis());

    Serial.printf("[MQTT] Connecting... (attempt #%d)\n", reconnectCount);
    Serial.printf("[MQTT] Broker: %s:%d\n", config.mqttServer, config.mqttPort);
    Serial.printf("[MQTT] Client ID: %s\n", currentClientId.c_str());

    mqttClient.setClientId(currentClientId.c_str());
    mqttClient.connect();

    // Connection is async - onMqttConnect will be called when done
}

void publishDiscovery() {
    JsonDocument doc;

    doc["device_id"] = deviceId;
    doc["name"] = config.deviceName;
    doc["type"] = DEVICE_TYPE;
    doc["model"] = "ESP32-WROOM";
    doc["firmware_version"] = FIRMWARE_VERSION;
    doc["ip_address"] = WiFi.localIP().toString();

    JsonArray capabilities = doc["capabilities"].to<JsonArray>();
    JsonObject cap = capabilities.add<JsonObject>();
    cap["type"] = "devices.capabilities.on_off";
    cap["instance"] = "powerSwitch";

    JsonArray sensors = doc["sensors"].to<JsonArray>();
    JsonObject sensor = sensors.add<JsonObject>();
    sensor["type"] = "led";
    sensor["instance"] = "builtin";
    sensor["gpio"] = LED_PIN;

    doc["timestamp"] = state.uptime;

    String payload;
    serializeJson(doc, payload);

    mqttClient.publish(discoveryTopic.c_str(), 1, true, payload.c_str());
    Serial.printf("[MQTT] Published discovery (%d bytes)\n", payload.length());
}

void publishState() {
    JsonDocument doc;

    doc["powerSwitch"] = state.ledOn ? 1 : 0;
    doc["rssi"] = state.rssi;
    doc["uptime"] = state.uptime;
    doc["ip"] = WiFi.localIP().toString();
    doc["timestamp"] = state.uptime;

    String payload;
    serializeJson(doc, payload);

    mqttClient.publish(stateTopic.c_str(), 1, true, payload.c_str());
}

void publishHealth() {
    JsonDocument doc;

    doc["uptime"] = state.uptime;
    doc["wifi_rssi"] = state.rssi;
    doc["wifi_connected"] = WiFi.isConnected();
    doc["mqtt_connected"] = state.mqttConnected;
    doc["mqtt_state"] = state.mqttConnected ? "connected" : "disconnected";
    doc["free_heap"] = ESP.getFreeHeap();
    doc["ip"] = WiFi.localIP().toString();
    doc["reconnect_count"] = reconnectCount;
    doc["last_error"] = lastError;
    doc["led_state"] = state.ledOn;
    doc["timestamp"] = millis();
    doc["client_id"] = currentClientId;
    doc["firmware"] = FIRMWARE_VERSION;
    doc["last_connect_duration_ms"] = connectionDuration;
    doc["time_since_connect_ms"] = lastConnectTime > 0 ? millis() - lastConnectTime : 0;

    String payload;
    serializeJson(doc, payload);

    mqttClient.publish(healthTopic.c_str(), 0, false, payload.c_str());
}

// ============== Async Web Server ==============

void setupWebServer() {
    // Root page - device status
    webServer.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
        String html = R"rawhtml(
<!DOCTYPE html>
<html>
<head>
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <title>ESP32 Homecontrol</title>
    <style>
        body { font-family: -apple-system, sans-serif; background: #1a1a1a; color: #fff; margin: 0; padding: 20px; }
        .container { max-width: 400px; margin: 0 auto; }
        h1 { color: #3b82f6; margin-bottom: 5px; }
        .subtitle { color: #666; margin-bottom: 20px; }
        .card { background: #2a2a2a; border-radius: 12px; padding: 20px; margin-bottom: 15px; }
        .status-row { display: flex; justify-content: space-between; padding: 8px 0; border-bottom: 1px solid #333; }
        .status-row:last-child { border: none; }
        .label { color: #888; }
        .value { color: #fff; font-weight: 500; }
        .online { color: #22c55e; }
        .offline { color: #ef4444; }
        .btn { display: block; width: 100%; padding: 15px; border: none; border-radius: 8px; font-size: 16px; cursor: pointer; margin-top: 10px; box-sizing: border-box; }
        .btn-primary { background: #3b82f6; color: white; }
        .btn-secondary { background: #333; color: #fff; text-align: center; text-decoration: none; }
        .led-status { display: inline-block; width: 12px; height: 12px; border-radius: 50%; margin-right: 8px; }
        .led-on { background: #22c55e; box-shadow: 0 0 10px #22c55e; }
        .led-off { background: #666; }
        .async-badge { background: #10b981; color: white; padding: 2px 8px; border-radius: 4px; font-size: 12px; margin-left: 8px; }
    </style>
</head>
<body>
    <div class="container">
        <h1>)rawhtml";
        html += config.deviceName;
        html += R"rawhtml(<span class="async-badge">ASYNC</span></h1>
        <p class="subtitle">ESP32 Homecontrol Device</p>
        <div class="card">
            <div class="status-row">
                <span class="label">LED Status</span>
                <span class="value"><span class="led-status )rawhtml";
        html += state.ledOn ? "led-on" : "led-off";
        html += R"rawhtml("></span>)rawhtml";
        html += state.ledOn ? "ON" : "OFF";
        html += R"rawhtml(</span>
            </div>
            <div class="status-row">
                <span class="label">WiFi Signal</span>
                <span class="value">)rawhtml";
        html += String(state.rssi) + " dBm";
        html += R"rawhtml(</span>
            </div>
            <div class="status-row">
                <span class="label">MQTT Status</span>
                <span class="value )rawhtml";
        html += state.mqttConnected ? "online" : "offline";
        html += R"rawhtml(">)rawhtml";
        html += state.mqttConnected ? "Connected" : "Disconnected";
        html += R"rawhtml(</span>
            </div>
            <div class="status-row">
                <span class="label">Uptime</span>
                <span class="value">)rawhtml";
        html += String(state.uptime / 3600) + "h " + String((state.uptime % 3600) / 60) + "m";
        html += R"rawhtml(</span>
            </div>
            <div class="status-row">
                <span class="label">Free Heap</span>
                <span class="value">)rawhtml";
        html += String(ESP.getFreeHeap() / 1024) + " KB";
        html += R"rawhtml(</span>
            </div>
            <div class="status-row">
                <span class="label">Firmware</span>
                <span class="value">)rawhtml";
        html += FIRMWARE_VERSION;
        html += R"rawhtml(</span>
            </div>
        </div>
        <button class="btn btn-primary" onclick="fetch('/toggle').then(()=>location.reload())">Toggle LED</button>
        <a href="/config" class="btn btn-secondary">Settings</a>
    </div>
</body>
</html>)rawhtml";

        request->send(200, "text/html", html);
    });

    // Config page
    webServer.on("/config", HTTP_GET, [](AsyncWebServerRequest *request) {
        String html = R"rawhtml(
<!DOCTYPE html>
<html>
<head>
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <title>Settings - ESP32</title>
    <style>
        body { font-family: -apple-system, sans-serif; background: #1a1a1a; color: #fff; margin: 0; padding: 20px; }
        .container { max-width: 400px; margin: 0 auto; }
        h1 { color: #3b82f6; }
        .card { background: #2a2a2a; border-radius: 12px; padding: 20px; margin-bottom: 15px; }
        label { display: block; color: #888; margin-bottom: 5px; margin-top: 15px; }
        input { width: 100%; padding: 12px; border: 1px solid #444; border-radius: 8px; background: #1a1a1a; color: #fff; box-sizing: border-box; }
        .btn { display: block; width: 100%; padding: 15px; border: none; border-radius: 8px; font-size: 16px; cursor: pointer; margin-top: 15px; box-sizing: border-box; }
        .btn-primary { background: #3b82f6; color: white; }
        .btn-secondary { background: #333; color: #fff; text-align: center; text-decoration: none; }
        .btn-danger { background: #dc2626; color: white; }
    </style>
</head>
<body>
    <div class="container">
        <h1>Settings</h1>
        <form method="POST" action="/config">
            <div class="card">
                <h3 style="margin-top:0;color:#888;">Device</h3>
                <label>Device Name</label>
                <input type="text" name="device_name" value=")rawhtml";
        html += config.deviceName;
        html += R"rawhtml(">
            </div>
            <div class="card">
                <h3 style="margin-top:0;color:#888;">MQTT</h3>
                <label>Server Address</label>
                <input type="text" name="mqtt_server" value=")rawhtml";
        html += config.mqttServer;
        html += R"rawhtml(">
                <label>Port</label>
                <input type="number" name="mqtt_port" value=")rawhtml";
        html += String(config.mqttPort);
        html += R"rawhtml(">
                <label>Username (optional)</label>
                <input type="text" name="mqtt_user" value=")rawhtml";
        html += config.mqttUser;
        html += R"rawhtml(">
                <label>Password (optional)</label>
                <input type="password" name="mqtt_pass" value=")rawhtml";
        html += config.mqttPass;
        html += R"rawhtml(">
            </div>
            <button type="submit" class="btn btn-primary">Save Settings</button>
        </form>
        <a href="/" class="btn btn-secondary">Back</a>
        <button class="btn btn-danger" onclick="if(confirm('Reset WiFi settings?')){fetch('/reboot?reset=1').then(()=>alert('Rebooting...'))}">Reset WiFi & Reboot</button>
    </div>
</body>
</html>)rawhtml";

        request->send(200, "text/html", html);
    });

    // Config save (POST)
    webServer.on("/config", HTTP_POST, [](AsyncWebServerRequest *request) {
        if (request->hasParam("device_name", true)) {
            strncpy(config.deviceName, request->getParam("device_name", true)->value().c_str(), sizeof(config.deviceName) - 1);
        }
        if (request->hasParam("mqtt_server", true)) {
            strncpy(config.mqttServer, request->getParam("mqtt_server", true)->value().c_str(), sizeof(config.mqttServer) - 1);
        }
        if (request->hasParam("mqtt_port", true)) {
            config.mqttPort = request->getParam("mqtt_port", true)->value().toInt();
        }
        if (request->hasParam("mqtt_user", true)) {
            strncpy(config.mqttUser, request->getParam("mqtt_user", true)->value().c_str(), sizeof(config.mqttUser) - 1);
        }
        if (request->hasParam("mqtt_pass", true)) {
            strncpy(config.mqttPass, request->getParam("mqtt_pass", true)->value().c_str(), sizeof(config.mqttPass) - 1);
        }

        saveConfig();

        request->redirect("/");

        // Schedule restart
        delay(500);
        ESP.restart();
    });

    // Toggle LED
    webServer.on("/toggle", HTTP_GET, [](AsyncWebServerRequest *request) {
        state.ledOn = !state.ledOn;
        digitalWrite(LED_PIN, state.ledOn ? HIGH : LOW);
        publishState();
        request->send(200, "text/plain", state.ledOn ? "ON" : "OFF");
    });

    // Reboot
    webServer.on("/reboot", HTTP_GET, [](AsyncWebServerRequest *request) {
        if (request->hasParam("reset") && request->getParam("reset")->value() == "1") {
            wifiManager.resetSettings();
        }
        request->send(200, "text/plain", "Rebooting...");
        delay(500);
        ESP.restart();
    });

    // API endpoint for JSON status
    webServer.on("/api/status", HTTP_GET, [](AsyncWebServerRequest *request) {
        JsonDocument doc;
        doc["device_id"] = deviceId;
        doc["device_name"] = config.deviceName;
        doc["led_state"] = state.ledOn;
        doc["mqtt_connected"] = state.mqttConnected;
        doc["wifi_rssi"] = state.rssi;
        doc["uptime"] = state.uptime;
        doc["free_heap"] = ESP.getFreeHeap();
        doc["firmware"] = FIRMWARE_VERSION;
        doc["ip"] = WiFi.localIP().toString();

        String json;
        serializeJson(doc, json);
        request->send(200, "application/json", json);
    });

    webServer.begin();
    Serial.println("[HTTP] Async web server started");
}

// ============== Configuration Storage ==============

void loadConfig() {
    preferences.begin("homecontrol", true);

    preferences.getString("deviceName", config.deviceName, sizeof(config.deviceName));
    if (strlen(config.deviceName) == 0) {
        strcpy(config.deviceName, "ESP32 Device");
    }

    preferences.getString("mqttServer", config.mqttServer, sizeof(config.mqttServer));
    if (strlen(config.mqttServer) == 0) {
        strcpy(config.mqttServer, "192.168.0.95");  // Default to Pi
    }

    config.mqttPort = preferences.getInt("mqttPort", DEFAULT_MQTT_PORT);

    preferences.getString("mqttUser", config.mqttUser, sizeof(config.mqttUser));
    preferences.getString("mqttPass", config.mqttPass, sizeof(config.mqttPass));

    preferences.getString("topicPrefix", config.topicPrefix, sizeof(config.topicPrefix));
    if (strlen(config.topicPrefix) == 0) {
        strcpy(config.topicPrefix, "homecontrol");
    }

    preferences.end();

    Serial.printf("[Config] Loaded: name=%s, mqtt=%s:%d\n",
                  config.deviceName, config.mqttServer, config.mqttPort);
}

void saveConfig() {
    preferences.begin("homecontrol", false);
    preferences.putString("deviceName", config.deviceName);
    preferences.putString("mqttServer", config.mqttServer);
    preferences.putInt("mqttPort", config.mqttPort);
    preferences.putString("mqttUser", config.mqttUser);
    preferences.putString("mqttPass", config.mqttPass);
    preferences.putString("topicPrefix", config.topicPrefix);
    preferences.end();

    Serial.println("[Config] Saved");
}

String getDeviceId() {
    uint64_t chipid = ESP.getEfuseMac();
    char id[13];
    sprintf(id, "%04x%08x", (uint16_t)(chipid >> 32), (uint32_t)chipid);
    return String(id);
}
