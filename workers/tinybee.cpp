/**
 * ESP32-C3 Homecontrol
 *
 * Async firmware for ESP32-C3 Mini:
 * - espMqttClient (async MQTT)
 * - ESPAsyncWebServer (async HTTP)
 * - Single core RISC-V, 160MHz
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <espMqttClient.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <ESPmDNS.h>
#include <stdarg.h>

// ============== Configuration ==============

#define LED_PIN 8  // GPIO8 - common LED pin for ESP32-C3 Mini boards
// Note: Some C3 boards use RGB LED on GPIO8, may need to adjust
#define DEVICE_TYPE "esp32-c3"
#define FIRMWARE_VERSION "1.1.0-c3"
#define DEFAULT_MQTT_PORT 1883

// Switch Panel GPIO assignments
#define SW1_PIN 0   // Toggle Switch 1
#define SW2_PIN 1   // Toggle Switch 2
#define SW3_PIN 2   // Toggle Switch 3
#define SW4_PIN 3   // Toggle Switch 4
#define START_PIN 5 // Engine Start push button
#define NUM_SWITCHES 5

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
    // Switch panel state (active-low: true = switch is ON)
    bool switches[NUM_SWITCHES] = {false, false, false, false, false};
    bool lastSwitches[NUM_SWITCHES] = {false, false, false, false, false};
    unsigned long startButtonPressTime = 0;
    bool startButtonHandled = false;
} state;

// Switch names for MQTT and UI
const char* switchNames[NUM_SWITCHES] = {
    "switch1",   // SW1 - Toggle 1
    "switch2",   // SW2 - Toggle 2
    "switch3",   // SW3 - Toggle 3
    "switch4",   // SW4 - Toggle 4
    "startButton" // Engine Start
};

const char* switchLabels[NUM_SWITCHES] = {
    "Switch 1",
    "Switch 2",
    "Switch 3",
    "Switch 4",
    "Engine Start"
};

String deviceId;
String discoveryTopic;
String stateTopic;
String commandTopic;
String availabilityTopic;
String healthTopic;
String logsTopic;

// Log buffer for MQTT publishing
#define LOG_BUFFER_SIZE 20
#define LOG_MSG_SIZE 128
char logBuffer[LOG_BUFFER_SIZE][LOG_MSG_SIZE];
int logBufferHead = 0;
int logBufferCount = 0;
unsigned long lastLogPublish = 0;
#define LOG_PUBLISH_INTERVAL 500

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
void publishLog(const char* msg);
void publishBufferedLogs();
void mqttLog(const char* format, ...);
void publishSwitchChange(int switchIndex, bool isOn);
void readSwitches();
void setupSwitchPanel();
void loadConfig();
void saveConfig();
String getDeviceId();
void setupWebServer();

// ============== MQTT Callbacks ==============

void onMqttConnect(bool sessionPresent) {
    mqttLog("[MQTT] Connected! Session present: %d\n", sessionPresent);
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
    mqttLog("[MQTT] Disconnected! Reason: %d\n", (int)reason);

    if (state.mqttConnected) {
        connectionDuration = millis() - lastConnectTime;
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
            digitalWrite(LED_PIN, state.ledOn ? LOW : HIGH);  // Active-low LED
            Serial.printf("[LED] Set to: %s\n", state.ledOn ? "ON" : "OFF");

            // Publish updated state immediately
            publishState();
        }
    }
}

void onMqttPublish(uint16_t packetId) {
    // Optional: track successful publishes
}

// ============== Setup ==============

void setup() {
    Serial.begin(115200);
    delay(500);

    Serial.println("\n\n=== ESP32-C3 Homecontrol ===");
    Serial.printf("Firmware: %s\n", FIRMWARE_VERSION);

    // Initialize LED (active-low: HIGH = off, LOW = on)
    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, HIGH);  // Start with LED off

    // Initialize switch panel inputs
    setupSwitchPanel();

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

    String apName = "ESP32C3-Setup-" + deviceId.substring(deviceId.length() - 4);
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
    String mdnsName = "esp32c3-" + deviceId.substring(deviceId.length() - 6);
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

    // Read switch panel (checks for changes and publishes)
    readSwitches();

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

    // Publish buffered logs
    publishBufferedLogs();
}

// ============== MQTT Functions ==============

void setupMQTT() {
    mqttClient.onConnect(onMqttConnect);
    mqttClient.onDisconnect(onMqttDisconnect);
    mqttClient.onMessage(onMqttMessage);
    mqttClient.onPublish(onMqttPublish);

    mqttClient.setServer(config.mqttServer, config.mqttPort);

    if (strlen(config.mqttUser) > 0) {
        mqttClient.setCredentials(config.mqttUser, config.mqttPass);
    }

    mqttClient.setKeepAlive(60);
    mqttClient.setWill(availabilityTopic.c_str(), 1, true, "offline");

    Serial.printf("[MQTT] Configured for %s:%d\n", config.mqttServer, config.mqttPort);
}

void connectMQTT() {
    reconnectCount++;
    currentClientId = "esp32c3-" + deviceId + "-" + String(millis());

    Serial.printf("[MQTT] Connecting... (attempt #%d)\n", reconnectCount);
    mqttClient.setClientId(currentClientId.c_str());
    mqttClient.connect();
}

void publishDiscovery() {
    JsonDocument doc;

    doc["device_id"] = deviceId;
    doc["name"] = config.deviceName;
    doc["type"] = DEVICE_TYPE;
    doc["model"] = "ESP32-C3 Mission Control";
    doc["firmware_version"] = FIRMWARE_VERSION;
    doc["ip_address"] = WiFi.localIP().toString();

    JsonArray capabilities = doc["capabilities"].to<JsonArray>();

    // LED control
    JsonObject ledCap = capabilities.add<JsonObject>();
    ledCap["type"] = "devices.capabilities.on_off";
    ledCap["instance"] = "powerSwitch";

    // Switch panel inputs (read-only sensors)
    for (int i = 0; i < NUM_SWITCHES; i++) {
        JsonObject swCap = capabilities.add<JsonObject>();
        swCap["type"] = "devices.capabilities.binary_sensor";
        swCap["instance"] = switchNames[i];
        swCap["name"] = switchLabels[i];
    }

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

    // Include switch panel state
    JsonObject switches = doc["switches"].to<JsonObject>();
    for (int i = 0; i < NUM_SWITCHES; i++) {
        switches[switchNames[i]] = state.switches[i] ? 1 : 0;
    }

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

    String payload;
    serializeJson(doc, payload);

    mqttClient.publish(healthTopic.c_str(), 0, false, payload.c_str());
}

// ============== MQTT Logging ==============

void publishLog(const char* msg) {
    if (!mqttClient.connected()) return;

    JsonDocument doc;
    doc["ts"] = state.uptime;
    doc["msg"] = msg;

    String payload;
    serializeJson(doc, payload);
    mqttClient.publish(logsTopic.c_str(), 0, false, payload.c_str());
}

void publishBufferedLogs() {
    if (!mqttClient.connected() || logBufferCount == 0) return;
    if (millis() - lastLogPublish < LOG_PUBLISH_INTERVAL) return;

    // Publish oldest log from buffer
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

    // Add to MQTT buffer (circular buffer)
    strncpy(logBuffer[logBufferHead], buffer, LOG_MSG_SIZE - 1);
    logBuffer[logBufferHead][LOG_MSG_SIZE - 1] = '\0';
    logBufferHead = (logBufferHead + 1) % LOG_BUFFER_SIZE;
    if (logBufferCount < LOG_BUFFER_SIZE) {
        logBufferCount++;
    }
}

// ============== Switch Panel ==============

void setupSwitchPanel() {
    // Configure all switch pins as inputs with internal pullup
    // When switch is OFF: pin reads HIGH (pulled up)
    // When switch is ON: pin reads LOW (grounded through switch)
    pinMode(SW1_PIN, INPUT_PULLUP);
    pinMode(SW2_PIN, INPUT_PULLUP);
    pinMode(SW3_PIN, INPUT_PULLUP);
    pinMode(SW4_PIN, INPUT_PULLUP);
    pinMode(START_PIN, INPUT_PULLUP);

    // Read initial state
    const int pins[NUM_SWITCHES] = {SW1_PIN, SW2_PIN, SW3_PIN, SW4_PIN, START_PIN};
    for (int i = 0; i < NUM_SWITCHES; i++) {
        state.switches[i] = (digitalRead(pins[i]) == LOW);  // Active-low
        state.lastSwitches[i] = state.switches[i];
    }

    Serial.println("[SWITCHES] Panel initialized");
    for (int i = 0; i < NUM_SWITCHES; i++) {
        Serial.printf("  %s: %s\n", switchLabels[i], state.switches[i] ? "ON" : "OFF");
    }
}

void readSwitches() {
    const int pins[NUM_SWITCHES] = {SW1_PIN, SW2_PIN, SW3_PIN, SW4_PIN, START_PIN};

    for (int i = 0; i < NUM_SWITCHES; i++) {
        bool currentState = (digitalRead(pins[i]) == LOW);  // Active-low

        // Check if state changed
        if (currentState != state.lastSwitches[i]) {
            // Simple debounce: wait a bit and re-read
            delay(20);
            currentState = (digitalRead(pins[i]) == LOW);

            if (currentState != state.lastSwitches[i]) {
                state.switches[i] = currentState;
                state.lastSwitches[i] = currentState;

                mqttLog("[SWITCH] %s: %s\n", switchLabels[i], currentState ? "ON" : "OFF");

                // Flash LED on any switch change
                digitalWrite(LED_PIN, LOW);   // LED on
                delay(50);
                digitalWrite(LED_PIN, HIGH);  // LED off

                // Publish change to MQTT
                if (state.mqttConnected) {
                    publishSwitchChange(i, currentState);
                }

                // Special handling for Engine Start button - SEND LOVE BOMB!
                if (i == 4 && currentState) {  // START button pressed
                    mqttLog("[SWITCH] ENGINE START pressed - SENDING LOVE BOMB!\n");

                    // Send love message to kayciBee1 (T-Display S3)
                    if (state.mqttConnected) {
                        // Pick random animation type (1=pulse, 2=shower, 3=burst)
                        int animType = random(1, 4);

                        JsonDocument loveDoc;
                        JsonObject capability = loveDoc["capability"].to<JsonObject>();
                        capability["instance"] = "loveMessage";
                        capability["type"] = animType;

                        String lovePayload;
                        serializeJson(loveDoc, lovePayload);

                        // kayciBee1's device ID is c404f416a398
                        String loveTopic = String(config.topicPrefix) + "/devices/c404f416a398/set";
                        mqttClient.publish(loveTopic.c_str(), 0, false, lovePayload.c_str());

                        mqttLog("[LOVE] Sent animation type %d to kayciBee1\n", animType);
                    }
                }
            }
        }
    }
}

void publishSwitchChange(int switchIndex, bool isOn) {
    JsonDocument doc;

    doc["switch"] = switchNames[switchIndex];
    doc["label"] = switchLabels[switchIndex];
    doc["state"] = isOn ? "on" : "off";
    doc["value"] = isOn ? 1 : 0;
    doc["timestamp"] = millis();

    String payload;
    serializeJson(doc, payload);

    // Publish to a dedicated switch topic
    String switchTopic = String(config.topicPrefix) + "/devices/" + deviceId + "/switch";
    mqttClient.publish(switchTopic.c_str(), 0, false, payload.c_str());

    Serial.printf("[MQTT] Published switch change: %s = %s\n", switchLabels[switchIndex], isOn ? "ON" : "OFF");
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
    <title>ESP32-C3 Homecontrol</title>
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
        .badge { background: #06b6d4; color: white; padding: 2px 8px; border-radius: 4px; font-size: 12px; margin-left: 8px; }
        .switch-grid { display: grid; grid-template-columns: repeat(2, 1fr); gap: 10px; }
        .switch-item { background: #1a1a1a; border-radius: 8px; padding: 12px; text-align: center; }
        .switch-item.on { background: #164e27; border: 1px solid #22c55e; }
        .switch-item.off { background: #1a1a1a; border: 1px solid #333; }
        .switch-name { font-size: 12px; color: #888; margin-bottom: 4px; }
        .switch-state { font-size: 18px; font-weight: bold; }
        .switch-item.on .switch-state { color: #22c55e; }
        .switch-item.off .switch-state { color: #666; }
        .start-btn { grid-column: span 2; background: #7f1d1d; border: 2px solid #dc2626; }
        .start-btn.on { background: #dc2626; box-shadow: 0 0 20px #dc2626; }
    </style>
</head>
<body>
    <div class="container">
        <h1>)rawhtml";
        html += config.deviceName;
        html += R"rawhtml(<span class="badge">C3</span></h1>
        <p class="subtitle">ESP32-C3 Mini</p>
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

        <div class="card">
            <h3 style="margin-top:0;color:#f97316;">Switch Panel</h3>
            <div class="switch-grid">)rawhtml";

        // Add switch status items
        for (int i = 0; i < 4; i++) {
            html += "<div class=\"switch-item ";
            html += state.switches[i] ? "on" : "off";
            html += "\"><div class=\"switch-name\">";
            html += switchLabels[i];
            html += "</div><div class=\"switch-state\">";
            html += state.switches[i] ? "ON" : "OFF";
            html += "</div></div>";
        }

        // Engine Start button (spans 2 columns)
        html += "<div class=\"switch-item start-btn ";
        html += state.switches[4] ? "on" : "off";
        html += "\"><div class=\"switch-name\">Engine Start</div><div class=\"switch-state\">";
        html += state.switches[4] ? "ENGAGED" : "READY";
        html += "</div></div>";

        html += R"rawhtml(
            </div>
        </div>

        <button class="btn btn-primary" onclick="fetch('/toggle').then(()=>location.reload())">Toggle LED</button>
        <a href="/config" class="btn btn-secondary">Settings</a>
    </div>
    <script>setTimeout(()=>location.reload(), 2000);</script>
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
    <title>Settings - ESP32-C3</title>
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

        saveConfig();
        request->redirect("/");
        delay(500);
        ESP.restart();
    });

    // Toggle LED
    webServer.on("/toggle", HTTP_GET, [](AsyncWebServerRequest *request) {
        state.ledOn = !state.ledOn;
        digitalWrite(LED_PIN, state.ledOn ? LOW : HIGH);  // Active-low LED
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

    // API endpoint
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

        // Include switch panel state
        JsonObject switches = doc["switches"].to<JsonObject>();
        for (int i = 0; i < NUM_SWITCHES; i++) {
            switches[switchNames[i]] = state.switches[i];
        }

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
        strcpy(config.deviceName, "ESP32-C3 Node");
    }

    preferences.getString("mqttServer", config.mqttServer, sizeof(config.mqttServer));
    if (strlen(config.mqttServer) == 0) {
        strcpy(config.mqttServer, "192.168.0.95");
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
