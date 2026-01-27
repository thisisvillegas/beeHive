/**
 * Basic Bee Template
 *
 * Core functionality that ALL bees share:
 * - WiFiManager for easy network setup
 * - espMqttClient (async, non-blocking MQTT)
 * - ESPAsyncWebServer for status/config pages
 * - Preferences for persistent config storage
 * - MQTT logging with buffered publishing
 * - Health/State/Discovery publishing
 * - Battery monitoring (optional)
 * - Companion Bee system (optional)
 *
 * To create a new bee:
 * 1. Copy this template
 * 2. Update DEVICE_TYPE and FIRMWARE_VERSION
 * 3. Configure BATTERY_ADC_PIN (-1 to disable)
 * 4. Add your GPIO pins and specialized state
 * 5. Implement your command handling in onMqttMessage()
 * 6. Add your sensors/actuators setup and loop logic
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

// ============== CUSTOMIZE THESE FOR YOUR BEE ==============

#define DEVICE_TYPE "esp32-basic"        // Change for your bee type
#define FIRMWARE_VERSION "1.0.0-basic"   // Your firmware version
#define DEFAULT_MQTT_PORT 1883

// Battery monitoring (set to -1 to disable)
#define BATTERY_ADC_PIN -1              // ADC pin for battery voltage (-1 = disabled)
#define BATTERY_SAMPLES 10              // Number of samples to average

// Add your GPIO pins here
// #define MY_SENSOR_PIN 4
// #define MY_ACTUATOR_PIN 5

// ============== Global Objects ==============

espMqttClient mqttClient;
AsyncWebServer webServer(80);
Preferences preferences;
WiFiManager wifiManager;

// Timers for non-blocking periodic tasks
unsigned long lastHealthPublish = 0;
unsigned long lastStatePublish = 0;
unsigned long lastReconnectAttempt = 0;
unsigned long lastBatteryRead = 0;

// ============== Device Configuration ==============

struct DeviceConfig {
    // === Core config (all bees have these) ===
    char deviceName[32];
    char mqttServer[64];
    int mqttPort;
    char mqttUser[32];
    char mqttPass[32];
    char topicPrefix[32];

    // === Companion Bee ===
    char companionBeeId[16];    // Device ID of companion bee to monitor

    // === Add your bee-specific config here ===
    // int myCustomSetting = 100;
} config;

// ============== Device State ==============

struct DeviceState {
    // === Core state (all bees have these) ===
    bool mqttConnected = false;
    unsigned long uptime = 0;
    int rssi = 0;

    // === Battery state ===
    float batteryVoltage = 0.0f;
    int batteryPercent = -1;        // -1 = no battery / disabled
    bool batteryCharging = false;

    // === Companion Bee state ===
    bool companionOnline = false;

    // === Add your bee-specific state here ===
    // bool mySensorValue = false;
} state;

// ============== MQTT Topics ==============

String deviceId;
String discoveryTopic;
String stateTopic;
String commandTopic;
String availabilityTopic;
String healthTopic;
String logsTopic;
String companionAvailabilityTopic;

// ============== MQTT Log Buffer ==============

#define LOG_BUFFER_SIZE 20
#define LOG_MSG_SIZE 128
char logBuffer[LOG_BUFFER_SIZE][LOG_MSG_SIZE];
int logBufferHead = 0;
int logBufferCount = 0;
unsigned long lastLogPublish = 0;
#define LOG_PUBLISH_INTERVAL 500

// ============== Debug Tracking ==============

int reconnectCount = 0;
String lastError = "none";
unsigned long lastConnectTime = 0;
String currentClientId;

// ============== Forward Declarations ==============

// Core functions
void setupMQTT();
void connectMQTT();
void publishDiscovery();
void publishState();
void publishHealth();
void publishLog(const char* msg);
void publishBufferedLogs();
void mqttLog(const char* format, ...);
void loadConfig();
void saveConfig();
String getDeviceId();
void setupWebServer();
void buildTopics();

// Battery & Companion functions
void readBattery();
void pingCompanionBee();

// Add your bee-specific function declarations here
// void setupMySensor();
// void handleMyCommand(JsonDocument& doc);

// ============== MQTT Logging ==============

void publishLog(const char* msg) {
    int idx = (logBufferHead + logBufferCount) % LOG_BUFFER_SIZE;
    strncpy(logBuffer[idx], msg, LOG_MSG_SIZE - 1);
    logBuffer[idx][LOG_MSG_SIZE - 1] = '\0';
    if (logBufferCount < LOG_BUFFER_SIZE) {
        logBufferCount++;
    } else {
        logBufferHead = (logBufferHead + 1) % LOG_BUFFER_SIZE;
    }
}

void mqttLog(const char* format, ...) {
    char buffer[LOG_MSG_SIZE];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);

    Serial.print(buffer);
    publishLog(buffer);
}

void publishBufferedLogs() {
    if (!state.mqttConnected || logBufferCount == 0) return;
    if (millis() - lastLogPublish < LOG_PUBLISH_INTERVAL) return;

    lastLogPublish = millis();

    JsonDocument doc;
    JsonArray logs = doc["logs"].to<JsonArray>();

    while (logBufferCount > 0) {
        JsonObject entry = logs.add<JsonObject>();
        entry["ts"] = state.uptime;
        entry["msg"] = logBuffer[logBufferHead];
        logBufferHead = (logBufferHead + 1) % LOG_BUFFER_SIZE;
        logBufferCount--;
    }

    char buffer[1024];
    serializeJson(doc, buffer);
    mqttClient.publish(logsTopic.c_str(), 0, false, buffer);
}

// ============== Device ID ==============

String getDeviceId() {
    uint64_t mac = ESP.getEfuseMac();
    char macStr[13];
    snprintf(macStr, sizeof(macStr), "%04x%08x",
             (uint16_t)(mac >> 32), (uint32_t)mac);
    return String(macStr);
}

void buildTopics() {
    String prefix = String(config.topicPrefix);
    discoveryTopic = prefix + "/discovery/" + deviceId + "/config";
    stateTopic = prefix + "/devices/" + deviceId + "/state";
    commandTopic = prefix + "/devices/" + deviceId + "/set";
    availabilityTopic = prefix + "/devices/" + deviceId + "/availability";
    healthTopic = prefix + "/devices/" + deviceId + "/health";
    logsTopic = prefix + "/devices/" + deviceId + "/logs";

    // Companion bee topic
    if (strlen(config.companionBeeId) > 0) {
        companionAvailabilityTopic = prefix + "/devices/" + String(config.companionBeeId) + "/availability";
    }
}

// ============== Configuration ==============

void loadConfig() {
    preferences.begin("bee-config", true);

    strlcpy(config.deviceName, preferences.getString("deviceName", "BasicBee").c_str(), sizeof(config.deviceName));
    strlcpy(config.mqttServer, preferences.getString("mqttServer", "192.168.0.95").c_str(), sizeof(config.mqttServer));
    config.mqttPort = preferences.getInt("mqttPort", DEFAULT_MQTT_PORT);
    strlcpy(config.mqttUser, preferences.getString("mqttUser", "").c_str(), sizeof(config.mqttUser));
    strlcpy(config.mqttPass, preferences.getString("mqttPass", "").c_str(), sizeof(config.mqttPass));
    strlcpy(config.topicPrefix, preferences.getString("topicPrefix", "homecontrol").c_str(), sizeof(config.topicPrefix));
    strlcpy(config.companionBeeId, preferences.getString("companionId", "").c_str(), sizeof(config.companionBeeId));

    // Load your bee-specific config here
    // config.myCustomSetting = preferences.getInt("mySetting", 100);

    preferences.end();

    Serial.printf("[Config] Loaded: %s @ %s:%d\n", config.deviceName, config.mqttServer, config.mqttPort);
    if (strlen(config.companionBeeId) > 0) {
        Serial.printf("[Config] Companion Bee: %s\n", config.companionBeeId);
    }
}

void saveConfig() {
    preferences.begin("bee-config", false);

    preferences.putString("deviceName", config.deviceName);
    preferences.putString("mqttServer", config.mqttServer);
    preferences.putInt("mqttPort", config.mqttPort);
    preferences.putString("mqttUser", config.mqttUser);
    preferences.putString("mqttPass", config.mqttPass);
    preferences.putString("topicPrefix", config.topicPrefix);
    preferences.putString("companionId", config.companionBeeId);

    // Save your bee-specific config here
    // preferences.putInt("mySetting", config.myCustomSetting);

    preferences.end();

    mqttLog("[Config] Saved\n");
}

// ============== Battery Monitoring ==============

void readBattery() {
    if (BATTERY_ADC_PIN < 0) return;  // Battery monitoring disabled

    // Average multiple samples for stability
    long sum = 0;
    for (int i = 0; i < BATTERY_SAMPLES; i++) {
        sum += analogRead(BATTERY_ADC_PIN);
        delay(2);
    }
    float adcValue = sum / BATTERY_SAMPLES;

    // Convert to voltage (assuming 2:1 voltage divider, 3.3V reference, 12-bit ADC)
    // Adjust multiplier based on your voltage divider ratio
    float voltage = (adcValue / 4095.0) * 3.3 * 2.0;

    state.batteryVoltage = voltage;

    // Detect charging (voltage > 4.25V typically means USB power)
    state.batteryCharging = (voltage > 4.25);

    // Calculate percentage (LiPo: 3.0V = 0%, 4.2V = 100%)
    if (voltage < 2.5) {
        state.batteryPercent = -1;  // No battery detected
    } else if (voltage >= 4.2) {
        state.batteryPercent = 100;
    } else if (voltage <= 3.0) {
        state.batteryPercent = 0;
    } else {
        state.batteryPercent = (int)((voltage - 3.0) / 1.2 * 100);
    }
}

// ============== Companion Bee ==============

void pingCompanionBee() {
    if (strlen(config.companionBeeId) == 0 || !state.mqttConnected) return;

    String topic = String(config.topicPrefix) + "/devices/" + String(config.companionBeeId) + "/set";

    JsonDocument doc;
    doc["capability"]["instance"] = "ping";
    doc["capability"]["value"] = "ping";
    doc["from"] = deviceId;

    char buffer[128];
    serializeJson(doc, buffer);
    mqttClient.publish(topic.c_str(), 0, false, buffer);

    mqttLog("[Companion] Pinged %s\n", config.companionBeeId);
}

// ============== MQTT Callbacks ==============

void onMqttConnect(bool sessionPresent) {
    mqttLog("[MQTT] Connected! Session: %d\n", sessionPresent);
    state.mqttConnected = true;
    lastConnectTime = millis();
    lastError = "none";

    // Subscribe to command topic
    mqttClient.subscribe(commandTopic.c_str(), 1);

    // Subscribe to companion availability if configured
    if (strlen(config.companionBeeId) > 0) {
        mqttClient.subscribe(companionAvailabilityTopic.c_str(), 1);
        mqttLog("[MQTT] Watching companion: %s\n", config.companionBeeId);
    }

    // Publish online status (retained)
    mqttClient.publish(availabilityTopic.c_str(), 1, true, "online");

    // Publish discovery and initial state
    publishDiscovery();
    publishState();
    publishHealth();
}

void onMqttDisconnect(espMqttClientTypes::DisconnectReason reason) {
    mqttLog("[MQTT] Disconnected: %d\n", (int)reason);
    state.mqttConnected = false;
    reconnectCount++;

    switch (reason) {
        case espMqttClientTypes::DisconnectReason::TCP_DISCONNECTED:
            lastError = "TCP_DISCONNECTED";
            break;
        case espMqttClientTypes::DisconnectReason::MQTT_SERVER_UNAVAILABLE:
            lastError = "SERVER_UNAVAILABLE";
            break;
        default:
            lastError = "UNKNOWN";
    }
}

void onMqttMessage(const espMqttClientTypes::MessageProperties& properties,
                   const char* topic, const uint8_t* payload, size_t len,
                   size_t index, size_t total) {

    String topicStr = String(topic);

    // Check if companion availability message
    if (strlen(config.companionBeeId) > 0 && topicStr == companionAvailabilityTopic) {
        String status = String((char*)payload).substring(0, len);
        state.companionOnline = (status == "online");
        mqttLog("[Companion] %s is %s\n", config.companionBeeId, status.c_str());
        return;
    }

    // Handle commands
    mqttLog("[MQTT] Message on %s\n", topic);

    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, payload, len);
    if (error) {
        mqttLog("[MQTT] JSON error: %s\n", error.c_str());
        return;
    }

    if (doc.containsKey("capability")) {
        String instance = doc["capability"]["instance"].as<String>();
        String value = doc["capability"]["value"].as<String>();

        mqttLog("[CMD] %s = %s\n", instance.c_str(), value.c_str());

        // Handle ping command
        if (instance == "ping") {
            mqttLog("[Ping] Received from %s\n", doc["from"].as<String>().c_str());
            // You could trigger an LED blink or sound here
        }

        // === ADD YOUR COMMAND HANDLING HERE ===
        // if (instance == "myActuator") {
        //     if (value == "on") { ... }
        // }
    }
}

// ============== MQTT Setup ==============

void setupMQTT() {
    currentClientId = String(DEVICE_TYPE) + "-" + deviceId + "-" + String(millis() % 100000);

    mqttClient.setClientId(currentClientId.c_str());
    mqttClient.setServer(config.mqttServer, config.mqttPort);
    mqttClient.setKeepAlive(60);
    mqttClient.setCleanSession(true);

    // Set Last Will (offline message when disconnected unexpectedly)
    mqttClient.setWill(availabilityTopic.c_str(), 1, true, "offline");

    if (strlen(config.mqttUser) > 0) {
        mqttClient.setCredentials(config.mqttUser, config.mqttPass);
    }

    mqttClient.onConnect(onMqttConnect);
    mqttClient.onDisconnect(onMqttDisconnect);
    mqttClient.onMessage(onMqttMessage);

    mqttLog("[MQTT] Setup complete, client: %s\n", currentClientId.c_str());
}

void connectMQTT() {
    if (state.mqttConnected || !WiFi.isConnected()) return;

    mqttLog("[MQTT] Connecting to %s:%d...\n", config.mqttServer, config.mqttPort);
    mqttClient.connect();
}

// ============== Publishing ==============

void publishDiscovery() {
    JsonDocument doc;
    doc["device_id"] = deviceId;
    doc["device_name"] = config.deviceName;
    doc["device_type"] = DEVICE_TYPE;
    doc["firmware"] = FIRMWARE_VERSION;
    doc["ip"] = WiFi.localIP().toString();
    doc["mac"] = WiFi.macAddress();
    doc["has_battery"] = (BATTERY_ADC_PIN >= 0);

    if (strlen(config.companionBeeId) > 0) {
        doc["companion_bee"] = config.companionBeeId;
    }

    // === ADD YOUR BEE'S CAPABILITIES HERE ===
    JsonArray caps = doc["capabilities"].to<JsonArray>();
    // caps.add("myCapability");

    char buffer[512];
    serializeJson(doc, buffer);
    mqttClient.publish(discoveryTopic.c_str(), 1, true, buffer);
}

void publishState() {
    JsonDocument doc;
    doc["rssi"] = state.rssi;
    doc["uptime"] = state.uptime;
    doc["ip"] = WiFi.localIP().toString();
    doc["timestamp"] = state.uptime;

    // === ADD YOUR BEE'S STATE HERE ===
    // doc["mySensorValue"] = state.mySensorValue;

    char buffer[256];
    serializeJson(doc, buffer);
    mqttClient.publish(stateTopic.c_str(), 1, true, buffer);
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
    doc["timestamp"] = millis();
    doc["client_id"] = currentClientId;
    doc["firmware"] = FIRMWARE_VERSION;

    // Battery info (if enabled)
    if (BATTERY_ADC_PIN >= 0) {
        doc["battery_voltage"] = state.batteryVoltage;
        doc["battery_percent"] = state.batteryPercent;
        doc["battery_charging"] = state.batteryCharging;
    }

    // Companion info (if configured)
    if (strlen(config.companionBeeId) > 0) {
        doc["companion_online"] = state.companionOnline;
    }

    // === ADD YOUR BEE'S HEALTH DATA HERE ===
    // doc["myHealthMetric"] = someValue;

    char buffer[512];
    serializeJson(doc, buffer);
    mqttClient.publish(healthTopic.c_str(), 0, false, buffer);
}

// ============== Web Server ==============

void setupWebServer() {
    // Main status page
    webServer.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
        String html = "<!DOCTYPE html><html><head><title>" + String(config.deviceName) + "</title>";
        html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
        html += "<style>body{font-family:sans-serif;padding:20px;background:#1a1a2e;color:#eee;}";
        html += ".card{background:#16213e;padding:15px;border-radius:8px;margin:10px 0;}";
        html += "h1{color:#0f4c75;}a{color:#3282b8;}.ok{color:#22c55e;}.err{color:#ef4444;}</style></head><body>";
        html += "<h1>" + String(config.deviceName) + "</h1>";
        html += "<div class='card'>";
        html += "<b>Device ID:</b> " + deviceId + "<br>";
        html += "<b>Firmware:</b> " + String(FIRMWARE_VERSION) + "<br>";
        html += "<b>IP:</b> " + WiFi.localIP().toString() + "<br>";
        html += "<b>RSSI:</b> " + String(state.rssi) + " dBm<br>";
        html += "<b>Uptime:</b> " + String(state.uptime) + "s<br>";
        html += "<b>Free Heap:</b> " + String(ESP.getFreeHeap()) + " bytes<br>";
        html += "<b>MQTT:</b> <span class='" + String(state.mqttConnected ? "ok" : "err") + "'>";
        html += String(state.mqttConnected ? "Connected" : "Disconnected") + "</span>";
        html += "</div>";

        // Battery section (if enabled)
        if (BATTERY_ADC_PIN >= 0) {
            html += "<div class='card'><b>Battery</b><br>";
            if (state.batteryPercent >= 0) {
                html += String(state.batteryPercent) + "% (" + String(state.batteryVoltage, 2) + "V)";
                if (state.batteryCharging) html += " - Charging";
            } else {
                html += "Not detected";
            }
            html += "</div>";
        }

        // Companion section (if configured)
        if (strlen(config.companionBeeId) > 0) {
            html += "<div class='card'><b>Companion Bee:</b> " + String(config.companionBeeId) + "<br>";
            html += "<span class='" + String(state.companionOnline ? "ok" : "err") + "'>";
            html += String(state.companionOnline ? "Online" : "Offline") + "</span>";
            html += " <a href='/ping'>Send Ping</a>";
            html += "</div>";
        }

        html += "<p><a href='/config'>Settings</a> | <a href='/api/status'>API Status</a></p>";
        html += "</body></html>";
        request->send(200, "text/html", html);
    });

    // Config page
    webServer.on("/config", HTTP_GET, [](AsyncWebServerRequest *request) {
        String html = "<!DOCTYPE html><html><head><title>Config - " + String(config.deviceName) + "</title>";
        html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
        html += "<style>body{font-family:sans-serif;padding:20px;background:#1a1a2e;color:#eee;}";
        html += "input,select{width:100%;padding:8px;margin:5px 0 15px 0;border-radius:4px;border:1px solid #333;background:#16213e;color:#eee;}";
        html += "button{background:#0f4c75;color:white;padding:10px 20px;border:none;border-radius:4px;cursor:pointer;}";
        html += "button:hover{background:#3282b8;}h2{color:#3282b8;margin-top:20px;}</style></head><body>";
        html += "<h1>Settings</h1><form method='POST' action='/config'>";

        html += "<h2>Device</h2>";
        html += "<label>Device Name</label><input name='deviceName' value='" + String(config.deviceName) + "'>";

        html += "<h2>MQTT</h2>";
        html += "<label>Server</label><input name='mqttServer' value='" + String(config.mqttServer) + "'>";
        html += "<label>Port</label><input type='number' name='mqttPort' value='" + String(config.mqttPort) + "'>";
        html += "<label>Username</label><input name='mqttUser' value='" + String(config.mqttUser) + "'>";
        html += "<label>Password</label><input type='password' name='mqttPass' value='" + String(config.mqttPass) + "'>";
        html += "<label>Topic Prefix</label><input name='topicPrefix' value='" + String(config.topicPrefix) + "'>";

        html += "<h2>Companion Bee</h2>";
        html += "<label>Companion Device ID (leave empty to disable)</label>";
        html += "<input name='companionId' value='" + String(config.companionBeeId) + "' placeholder='e.g., 503035d4db1c'>";

        // === ADD YOUR BEE-SPECIFIC CONFIG FIELDS HERE ===

        html += "<br><button type='submit'>Save</button></form>";
        html += "<p><a href='/'>Back</a> | <a href='/reset'>Reset WiFi</a></p>";
        html += "</body></html>";
        request->send(200, "text/html", html);
    });

    // Config save handler
    webServer.on("/config", HTTP_POST, [](AsyncWebServerRequest *request) {
        if (request->hasParam("deviceName", true)) {
            strlcpy(config.deviceName, request->getParam("deviceName", true)->value().c_str(), sizeof(config.deviceName));
        }
        if (request->hasParam("mqttServer", true)) {
            strlcpy(config.mqttServer, request->getParam("mqttServer", true)->value().c_str(), sizeof(config.mqttServer));
        }
        if (request->hasParam("mqttPort", true)) {
            config.mqttPort = request->getParam("mqttPort", true)->value().toInt();
        }
        if (request->hasParam("mqttUser", true)) {
            strlcpy(config.mqttUser, request->getParam("mqttUser", true)->value().c_str(), sizeof(config.mqttUser));
        }
        if (request->hasParam("mqttPass", true)) {
            strlcpy(config.mqttPass, request->getParam("mqttPass", true)->value().c_str(), sizeof(config.mqttPass));
        }
        if (request->hasParam("topicPrefix", true)) {
            strlcpy(config.topicPrefix, request->getParam("topicPrefix", true)->value().c_str(), sizeof(config.topicPrefix));
        }
        if (request->hasParam("companionId", true)) {
            strlcpy(config.companionBeeId, request->getParam("companionId", true)->value().c_str(), sizeof(config.companionBeeId));
        }
        // === HANDLE YOUR BEE-SPECIFIC CONFIG PARAMS HERE ===

        saveConfig();
        buildTopics();
        request->redirect("/config?saved=1");

        // Reconnect MQTT with new settings
        mqttClient.disconnect();
        setupMQTT();
    });

    // Ping companion endpoint
    webServer.on("/ping", HTTP_GET, [](AsyncWebServerRequest *request) {
        if (strlen(config.companionBeeId) > 0) {
            pingCompanionBee();
            request->send(200, "text/html", "<h1>Ping Sent!</h1><p>Pinged " + String(config.companionBeeId) + "</p><p><a href='/'>Back</a></p>");
        } else {
            request->send(400, "text/html", "<h1>No Companion</h1><p>Configure a companion bee first.</p><p><a href='/config'>Settings</a></p>");
        }
    });

    // API status endpoint
    webServer.on("/api/status", HTTP_GET, [](AsyncWebServerRequest *request) {
        JsonDocument doc;
        doc["device_id"] = deviceId;
        doc["device_name"] = config.deviceName;
        doc["firmware"] = FIRMWARE_VERSION;
        doc["uptime"] = state.uptime;
        doc["rssi"] = state.rssi;
        doc["mqtt_connected"] = state.mqttConnected;
        doc["free_heap"] = ESP.getFreeHeap();

        if (BATTERY_ADC_PIN >= 0) {
            doc["battery_voltage"] = state.batteryVoltage;
            doc["battery_percent"] = state.batteryPercent;
            doc["battery_charging"] = state.batteryCharging;
        }

        if (strlen(config.companionBeeId) > 0) {
            doc["companion_id"] = config.companionBeeId;
            doc["companion_online"] = state.companionOnline;
        }

        // === ADD YOUR BEE'S STATUS HERE ===

        String response;
        serializeJson(doc, response);
        request->send(200, "application/json", response);
    });

    // WiFi reset
    webServer.on("/reset", HTTP_GET, [](AsyncWebServerRequest *request) {
        request->send(200, "text/html", "<h1>WiFi Reset</h1><p>Resetting WiFi settings... Device will restart.</p>");
        delay(1000);
        wifiManager.resetSettings();
        ESP.restart();
    });

    webServer.begin();
    mqttLog("[Web] Server started on port 80\n");
}

// ============== Setup ==============

void setup() {
    Serial.begin(115200);
    delay(100);
    Serial.println("\n\n=== Basic Bee Starting ===");

    // Get device ID from MAC
    deviceId = getDeviceId();
    Serial.printf("[Boot] Device ID: %s\n", deviceId.c_str());

    // Load configuration
    loadConfig();
    buildTopics();

    // === SETUP YOUR GPIO PINS HERE ===
    // pinMode(MY_SENSOR_PIN, INPUT);
    // pinMode(MY_ACTUATOR_PIN, OUTPUT);

    // Setup WiFi with captive portal
    wifiManager.setConfigPortalTimeout(180);
    wifiManager.setConnectTimeout(30);

    String apName = String(config.deviceName) + "-Setup";
    if (!wifiManager.autoConnect(apName.c_str())) {
        Serial.println("[WiFi] Failed to connect, restarting...");
        ESP.restart();
    }

    Serial.printf("[WiFi] Connected! IP: %s\n", WiFi.localIP().toString().c_str());

    // Setup mDNS
    String hostname = String(config.deviceName);
    hostname.toLowerCase();
    hostname.replace(" ", "-");
    if (MDNS.begin(hostname.c_str())) {
        MDNS.addService("http", "tcp", 80);
        Serial.printf("[mDNS] Hostname: %s.local\n", hostname.c_str());
    }

    // Setup MQTT
    setupMQTT();
    connectMQTT();

    // Setup web server
    setupWebServer();

    Serial.println("=== Basic Bee Ready ===\n");
}

// ============== Loop ==============

void loop() {
    // Update core state
    state.uptime = millis() / 1000;
    state.rssi = WiFi.RSSI();

    // Read battery every 2 seconds (if enabled)
    if (BATTERY_ADC_PIN >= 0 && millis() - lastBatteryRead > 2000) {
        lastBatteryRead = millis();
        readBattery();
    }

    // === ADD YOUR SENSOR READING HERE ===
    // readMySensor();

    // Reconnect MQTT if needed (non-blocking, 5s cooldown)
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

    // === ADD YOUR BEE'S LOOP LOGIC HERE ===
}
