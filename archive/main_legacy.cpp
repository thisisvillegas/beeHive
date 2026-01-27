/**
 * ESP32 Homecontrol - MVP
 *
 * Features:
 * - WiFi Manager with captive portal for initial setup
 * - MQTT client for communication with homecontrol
 * - Built-in LED control for proof of concept
 * - Web UI for status and configuration
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <PubSubClient.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <ESPmDNS.h>

// ============== Configuration ==============

#define LED_PIN 2                    // Built-in LED on most ESP32 boards
#define DEVICE_TYPE "esp32"
#define FIRMWARE_VERSION "1.0.0"

// Default MQTT settings (can be changed via web UI)
#define DEFAULT_MQTT_PORT 1883
#define MQTT_RECONNECT_DELAY 5000

// ============== Global Objects ==============

WiFiClient wifiClient;
PubSubClient mqttClient(wifiClient);
WebServer webServer(80);
Preferences preferences;
WiFiManager wifiManager;

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
    bool ledOn;
    bool mqttConnected;
    unsigned long lastMqttAttempt;
    unsigned long uptime;
    int rssi;
} state;

String deviceId;
String discoveryTopic;
String stateTopic;
String commandTopic;
String availabilityTopic;

// ============== Forward Declarations ==============

void setupMQTT();
void mqttCallback(char* topic, byte* payload, unsigned int length);
void reconnectMQTT();
bool publishDiscovery();
bool publishState();
void publishAvailability(bool online);
void handleRoot();
void handleConfig();
void handleConfigSave();
void handleToggleLED();
void handleReboot();
void loadConfig();
void saveConfig();
String getDeviceId();
void publishHealth();

// ============== Debug Helpers ==============

// Track reconnection attempts and errors
int reconnectCount = 0;
const char* lastError = "none";
String healthTopic;
String logsTopic;

// Connection tracking for diagnostics
unsigned long lastConnectTime = 0;      // When we last successfully connected
unsigned long lastDisconnectTime = 0;   // When we last disconnected
unsigned long connectionDuration = 0;   // How long the last connection lasted
String currentClientId;                 // Track the client ID we're using

// Log buffer for storing recent logs (circular buffer)
#define LOG_BUFFER_SIZE 20
#define LOG_LINE_SIZE 128
char logBuffer[LOG_BUFFER_SIZE][LOG_LINE_SIZE];
int logBufferHead = 0;
int logBufferCount = 0;

// Add log to buffer
void addToLogBuffer(const char* msg) {
    strncpy(logBuffer[logBufferHead], msg, LOG_LINE_SIZE - 1);
    logBuffer[logBufferHead][LOG_LINE_SIZE - 1] = '\0';
    logBufferHead = (logBufferHead + 1) % LOG_BUFFER_SIZE;
    if (logBufferCount < LOG_BUFFER_SIZE) logBufferCount++;
}

// Publish a single log line to MQTT
void publishLog(const char* msg) {
    if (mqttClient.connected() && logsTopic.length() > 0) {
        JsonDocument doc;
        doc["ts"] = millis();
        doc["msg"] = msg;
        String payload;
        serializeJson(doc, payload);
        mqttClient.publish(logsTopic.c_str(), payload.c_str());
    }
}

// Publish all buffered logs (called after reconnect)
void publishBufferedLogs() {
    if (!mqttClient.connected() || logBufferCount == 0) return;

    int start = (logBufferHead - logBufferCount + LOG_BUFFER_SIZE) % LOG_BUFFER_SIZE;
    for (int i = 0; i < logBufferCount; i++) {
        int idx = (start + i) % LOG_BUFFER_SIZE;
        publishLog(logBuffer[idx]);
        mqttClient.loop(); // Process between publishes
    }
}

// Debug log with timestamp - writes to Serial and buffers for MQTT
void debugLog(const char* format, ...) {
    char buffer[LOG_LINE_SIZE];

    // Format timestamp
    int offset = snprintf(buffer, sizeof(buffer), "[%lu] ", millis());

    // Format message
    va_list args;
    va_start(args, format);
    vsnprintf(buffer + offset, sizeof(buffer) - offset, format, args);
    va_end(args);

    // Output to serial
    Serial.println(buffer);

    // Add to buffer
    addToLogBuffer(buffer);

    // Try to publish immediately if connected
    publishLog(buffer);
}

// Translate PubSubClient state codes to readable strings
const char* getMqttStateString(int state) {
    switch(state) {
        case -4: return "TIMEOUT";
        case -3: return "LOST";
        case -2: return "FAILED";
        case -1: return "DISCONNECTED";
        case 0:  return "CONNECTED";
        case 1:  return "BAD_PROTOCOL";
        case 2:  return "BAD_CLIENT_ID";
        case 3:  return "UNAVAILABLE";
        case 4:  return "BAD_CREDENTIALS";
        case 5:  return "UNAUTHORIZED";
        default: return "UNKNOWN";
    }
}

// ============== Setup ==============

void setup() {
    Serial.begin(115200);
    delay(1000);
    Serial.println("\n\n=== ESP32 Homecontrol MVP ===");

    // Initialize LED
    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, LOW);
    state.ledOn = false;

    // Generate device ID from MAC address
    deviceId = getDeviceId();
    Serial.printf("Device ID: %s\n", deviceId.c_str());

    // Load saved configuration
    loadConfig();

    // Setup WiFi Manager
    // Custom parameters for MQTT configuration
    WiFiManagerParameter mqttServerParam("mqtt_server", "MQTT Server", config.mqttServer, 64);
    WiFiManagerParameter mqttPortParam("mqtt_port", "MQTT Port", String(config.mqttPort).c_str(), 6);
    WiFiManagerParameter deviceNameParam("device_name", "Device Name", config.deviceName, 32);

    wifiManager.addParameter(&mqttServerParam);
    wifiManager.addParameter(&mqttPortParam);
    wifiManager.addParameter(&deviceNameParam);

    // Set AP name with device ID suffix
    String apName = "ESP32-Setup-" + deviceId.substring(deviceId.length() - 4);

    // Try to connect, if fail start config portal
    wifiManager.setConfigPortalTimeout(180); // 3 minute timeout

    Serial.println("Starting WiFi Manager...");
    if (!wifiManager.autoConnect(apName.c_str())) {
        Serial.println("Failed to connect, restarting...");
        delay(3000);
        ESP.restart();
    }

    // Save parameters if they were updated
    strcpy(config.mqttServer, mqttServerParam.getValue());
    config.mqttPort = atoi(mqttPortParam.getValue());
    strcpy(config.deviceName, deviceNameParam.getValue());
    saveConfig();

    Serial.println("WiFi connected!");
    Serial.printf("IP Address: %s\n", WiFi.localIP().toString().c_str());

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

    debugLog("Topics configured:");
    debugLog("  state: %s", stateTopic.c_str());
    debugLog("  command: %s", commandTopic.c_str());
    debugLog("  health: %s", healthTopic.c_str());
    debugLog("  logs: %s", logsTopic.c_str());

    // Setup MQTT
    setupMQTT();

    // Setup Web Server routes
    webServer.on("/", handleRoot);
    webServer.on("/config", HTTP_GET, handleConfig);
    webServer.on("/config", HTTP_POST, handleConfigSave);
    webServer.on("/toggle", handleToggleLED);
    webServer.on("/reboot", handleReboot);
    webServer.begin();

    Serial.println("Web server started");
    Serial.println("Setup complete!\n");
}

// ============== Main Loop ==============

void loop() {
    // Handle web server
    webServer.handleClient();

    // Always call MQTT loop - handles keep-alive and incoming messages
    mqttClient.loop();

    // Track connection state changes and log them
    bool isConnected = mqttClient.connected();
    if (isConnected != state.mqttConnected) {
        debugLog("MQTT: Connection state changed: %s -> %s",
                 state.mqttConnected ? "CONNECTED" : "DISCONNECTED",
                 isConnected ? "CONNECTED" : "DISCONNECTED");

        if (!isConnected && state.mqttConnected) {
            // Just disconnected - calculate how long we were connected
            lastDisconnectTime = millis();
            connectionDuration = lastDisconnectTime - lastConnectTime;
            lastError = getMqttStateString(mqttClient.state());
            debugLog("MQTT: DISCONNECTED after %lu ms", connectionDuration);
            debugLog("MQTT: Disconnect reason: %s (state=%d)", lastError, mqttClient.state());
            debugLog("MQTT: Client ID was: %s", currentClientId.c_str());
            debugLog("MQTT: WiFi RSSI: %d dBm, IP: %s", WiFi.RSSI(), WiFi.localIP().toString().c_str());
        }

        state.mqttConnected = isConnected;
    }

    // Reconnect if not connected
    if (!state.mqttConnected) {
        reconnectMQTT();
    }

    // Update state
    state.uptime = millis() / 1000;
    state.rssi = WiFi.RSSI();

    // Publish health every 5 seconds (visible in homecontrol dashboard)
    static unsigned long lastHealth = 0;
    if (millis() - lastHealth > 5000) {
        lastHealth = millis();
        if (state.mqttConnected) {
            publishHealth();
        }
        // Log status to serial regardless of connection
        debugLog("STATUS: mqtt=%s wifi=%ddBm heap=%lu uptime=%lus reconnects=%d",
                 isConnected ? "OK" : "NO", state.rssi, ESP.getFreeHeap(), state.uptime, reconnectCount);
    }

    // Publish state every 30 seconds
    static unsigned long lastStatePublish = 0;
    if (state.mqttConnected && millis() - lastStatePublish > 30000) {
        lastStatePublish = millis();
        publishState();
    }

    delay(10);
}

// ============== MQTT Functions ==============

void setupMQTT() {
    mqttClient.setServer(config.mqttServer, config.mqttPort);
    mqttClient.setCallback(mqttCallback);
    mqttClient.setBufferSize(1024);
    mqttClient.setKeepAlive(60);  // 60 second keep-alive
}

void reconnectMQTT() {
    // Simple 5-second cooldown between attempts
    if (millis() - state.lastMqttAttempt < 5000) {
        return;
    }
    state.lastMqttAttempt = millis();
    reconnectCount++;

    debugLog("MQTT: ========== Connection Attempt #%d ==========", reconnectCount);
    debugLog("MQTT: Target: %s:%d", config.mqttServer, config.mqttPort);
    debugLog("MQTT: WiFi RSSI: %d dBm", WiFi.RSSI());
    debugLog("MQTT: Free heap: %lu bytes", ESP.getFreeHeap());
    debugLog("MQTT: State before connect: %s", getMqttStateString(mqttClient.state()));

    // Generate unique client ID with timestamp (like the simulator does)
    // This prevents session conflicts when reconnecting quickly
    currentClientId = "esp32-" + deviceId + "-" + String(millis());
    debugLog("MQTT: Client ID: %s", currentClientId.c_str());

    // Use NULL for empty username/password
    const char* user = strlen(config.mqttUser) > 0 ? config.mqttUser : NULL;
    const char* pass = strlen(config.mqttPass) > 0 ? config.mqttPass : NULL;

    unsigned long connectStart = millis();

    // Connect with last will message
    if (mqttClient.connect(currentClientId.c_str(), user, pass,
                           availabilityTopic.c_str(), 1, true, "offline")) {
        unsigned long connectTime = millis() - connectStart;
        lastConnectTime = millis();
        debugLog("MQTT: SUCCESS - Connected in %lu ms!", connectTime);
        lastError = "none";

        // Subscribe to command topic
        bool subOk = mqttClient.subscribe(commandTopic.c_str());
        debugLog("MQTT: Subscribe to %s: %s", commandTopic.c_str(), subOk ? "OK" : "FAILED");

        // Publish online status - log success/failure
        bool onlineOk = mqttClient.publish(availabilityTopic.c_str(), "online", true);
        debugLog("MQTT: Publish 'online' to %s: %s", availabilityTopic.c_str(), onlineOk ? "OK" : "FAILED");

        // Give the broker a moment to process
        mqttClient.loop();
        delay(10);

        // Publish discovery and state
        bool discOk = publishDiscovery();
        debugLog("MQTT: Publish discovery: %s", discOk ? "OK" : "FAILED");

        bool stateOk = publishState();
        debugLog("MQTT: Publish state: %s", stateOk ? "OK" : "FAILED");

        publishHealth();

        // Publish any buffered logs from before we connected
        publishBufferedLogs();

        state.mqttConnected = true;
        debugLog("MQTT: ========== Setup complete ==========");
    } else {
        unsigned long connectTime = millis() - connectStart;
        lastError = getMqttStateString(mqttClient.state());
        debugLog("MQTT: FAILED after %lu ms - %s (rc=%d)", connectTime, lastError, mqttClient.state());
        state.mqttConnected = false;
    }
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
    Serial.printf("MQTT message on %s\n", topic);

    // Parse JSON payload
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, payload, length);

    if (error) {
        Serial.printf("JSON parse error: %s\n", error.c_str());
        return;
    }

    // Handle control commands
    if (doc.containsKey("capability")) {
        JsonObject capability = doc["capability"];
        const char* instance = capability["instance"];

        if (strcmp(instance, "powerSwitch") == 0) {
            int value = capability["value"];
            state.ledOn = (value == 1);
            digitalWrite(LED_PIN, state.ledOn ? HIGH : LOW);
            Serial.printf("LED set to: %s\n", state.ledOn ? "ON" : "OFF");

            // Publish updated state
            publishState();
        }
    }
}

bool publishDiscovery() {
    JsonDocument doc;

    doc["device_id"] = deviceId;
    doc["name"] = config.deviceName;
    doc["type"] = DEVICE_TYPE;
    doc["model"] = "ESP32-WROOM";
    doc["firmware_version"] = FIRMWARE_VERSION;
    doc["ip_address"] = WiFi.localIP().toString();

    // Capabilities - LED is treated as on_off switch
    JsonArray capabilities = doc.createNestedArray("capabilities");
    JsonObject cap = capabilities.createNestedObject();
    cap["type"] = "devices.capabilities.on_off";
    cap["instance"] = "powerSwitch";

    // Sensors info
    JsonArray sensors = doc.createNestedArray("sensors");
    JsonObject sensor = sensors.createNestedObject();
    sensor["type"] = "led";
    sensor["instance"] = "builtin";
    sensor["gpio"] = LED_PIN;

    doc["timestamp"] = state.uptime;

    String payload;
    serializeJson(doc, payload);

    bool result = mqttClient.publish(discoveryTopic.c_str(), payload.c_str(), true);
    Serial.printf("Published discovery to %s: %s\n", discoveryTopic.c_str(), result ? "OK" : "FAILED");
    return result;
}

bool publishState() {
    JsonDocument doc;

    doc["powerSwitch"] = state.ledOn ? 1 : 0;
    doc["rssi"] = state.rssi;
    doc["uptime"] = state.uptime;
    doc["ip"] = WiFi.localIP().toString();
    doc["timestamp"] = state.uptime;

    String payload;
    serializeJson(doc, payload);

    return mqttClient.publish(stateTopic.c_str(), payload.c_str(), true);
}

void publishAvailability(bool online) {
    mqttClient.publish(availabilityTopic.c_str(), online ? "online" : "offline", true);
}

void publishHealth() {
    JsonDocument doc;

    doc["uptime"] = millis() / 1000;
    doc["wifi_rssi"] = WiFi.RSSI();
    doc["wifi_connected"] = WiFi.isConnected();
    doc["mqtt_connected"] = mqttClient.connected();
    doc["mqtt_state"] = getMqttStateString(mqttClient.state());
    doc["free_heap"] = ESP.getFreeHeap();
    doc["ip"] = WiFi.localIP().toString();
    doc["reconnect_count"] = reconnectCount;
    doc["last_error"] = lastError;
    doc["led_state"] = state.ledOn;
    doc["timestamp"] = millis();

    // New diagnostic fields
    doc["client_id"] = currentClientId;
    doc["last_connect_duration_ms"] = connectionDuration;
    doc["time_since_connect_ms"] = lastConnectTime > 0 ? millis() - lastConnectTime : 0;

    String payload;
    serializeJson(doc, payload);

    if (mqttClient.publish(healthTopic.c_str(), payload.c_str())) {
        debugLog("Health: heap=%lu rssi=%d reconnects=%d connected_for=%lums",
                 ESP.getFreeHeap(), WiFi.RSSI(), reconnectCount,
                 lastConnectTime > 0 ? millis() - lastConnectTime : 0);
    }
}

// ============== Web Server Handlers ==============

void handleRoot() {
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
        .btn { display: block; width: 100%; padding: 15px; border: none; border-radius: 8px; font-size: 16px; cursor: pointer; margin-top: 10px; }
        .btn-primary { background: #3b82f6; color: white; }
        .btn-secondary { background: #333; color: #fff; }
        .btn-danger { background: #dc2626; color: white; }
        .led-status { display: inline-block; width: 12px; height: 12px; border-radius: 50%; margin-right: 8px; }
        .led-on { background: #22c55e; box-shadow: 0 0 10px #22c55e; }
        .led-off { background: #666; }
    </style>
</head>
<body>
    <div class="container">
        <h1>)rawhtml";
    html += config.deviceName;
    html += R"rawhtml(</h1>
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
                <span class="label">IP Address</span>
                <span class="value">)rawhtml";
    html += WiFi.localIP().toString();
    html += R"rawhtml(</span>
            </div>
        </div>

        <button class="btn btn-primary" onclick="fetch('/toggle').then(function(){location.reload();})">Toggle LED</button>
        <a href="/config" class="btn btn-secondary" style="text-align:center;text-decoration:none;">Settings</a>
    </div>
</body>
</html>
)rawhtml";

    webServer.send(200, "text/html", html);
}

void handleConfig() {
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
        .btn { display: block; width: 100%; padding: 15px; border: none; border-radius: 8px; font-size: 16px; cursor: pointer; margin-top: 15px; }
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
        <button class="btn btn-danger" onclick="if(confirm('Reset WiFi settings?')){fetch('/reboot?reset=1').then(function(){alert('Rebooting...');})}">Reset WiFi & Reboot</button>
    </div>
</body>
</html>
)rawhtml";

    webServer.send(200, "text/html", html);
}

void handleConfigSave() {
    if (webServer.hasArg("device_name")) {
        strncpy(config.deviceName, webServer.arg("device_name").c_str(), sizeof(config.deviceName) - 1);
    }
    if (webServer.hasArg("mqtt_server")) {
        strncpy(config.mqttServer, webServer.arg("mqtt_server").c_str(), sizeof(config.mqttServer) - 1);
    }
    if (webServer.hasArg("mqtt_port")) {
        config.mqttPort = webServer.arg("mqtt_port").toInt();
    }
    if (webServer.hasArg("mqtt_user")) {
        strncpy(config.mqttUser, webServer.arg("mqtt_user").c_str(), sizeof(config.mqttUser) - 1);
    }
    if (webServer.hasArg("mqtt_pass")) {
        strncpy(config.mqttPass, webServer.arg("mqtt_pass").c_str(), sizeof(config.mqttPass) - 1);
    }

    saveConfig();

    webServer.sendHeader("Location", "/");
    webServer.send(302);

    // Restart to apply changes
    delay(1000);
    ESP.restart();
}

void handleToggleLED() {
    state.ledOn = !state.ledOn;
    digitalWrite(LED_PIN, state.ledOn ? HIGH : LOW);
    publishState();
    webServer.send(200, "text/plain", state.ledOn ? "ON" : "OFF");
}

void handleReboot() {
    if (webServer.hasArg("reset") && webServer.arg("reset") == "1") {
        wifiManager.resetSettings();
    }
    webServer.send(200, "text/plain", "Rebooting...");
    delay(1000);
    ESP.restart();
}

// ============== Configuration Storage ==============

void loadConfig() {
    preferences.begin("homecontrol", true);

    // Load with defaults
    preferences.getString("deviceName", config.deviceName, sizeof(config.deviceName));
    if (strlen(config.deviceName) == 0) {
        strcpy(config.deviceName, "ESP32 Device");
    }

    preferences.getString("mqttServer", config.mqttServer, sizeof(config.mqttServer));
    if (strlen(config.mqttServer) == 0) {
        strcpy(config.mqttServer, "192.168.1.1");  // Default - user should change
    }

    config.mqttPort = preferences.getInt("mqttPort", DEFAULT_MQTT_PORT);

    preferences.getString("mqttUser", config.mqttUser, sizeof(config.mqttUser));
    preferences.getString("mqttPass", config.mqttPass, sizeof(config.mqttPass));

    preferences.getString("topicPrefix", config.topicPrefix, sizeof(config.topicPrefix));
    if (strlen(config.topicPrefix) == 0) {
        strcpy(config.topicPrefix, "homecontrol");
    }

    preferences.end();

    Serial.printf("Config loaded: name=%s, mqtt=%s:%d\n",
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

    Serial.println("Config saved");
}

String getDeviceId() {
    // Use eFuse MAC which works before WiFi is initialized
    uint64_t chipid = ESP.getEfuseMac();
    char id[13];
    sprintf(id, "%04x%08x", (uint16_t)(chipid >> 32), (uint32_t)chipid);
    return String(id);
}
