/**
 * Display Bee Template
 *
 * Extends Basic Bee with display capabilities:
 * - TFT_eSPI display driver with TFT_eSprite (flicker-free)
 * - Battery monitoring (for portable bees)
 * - Companion Bee system (track another bee's status)
 * - Auto-dimming schedule
 * - Screen/view management
 * - Animation framework
 *
 * Designed for: T-Display S3, or any TFT-equipped ESP32
 *
 * To create a new display bee:
 * 1. Copy this template
 * 2. Configure TFT pins for your display
 * 3. Add your custom screens/views
 * 4. Implement your specific UI
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <espMqttClient.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <ESPmDNS.h>
#include <TFT_eSPI.h>
#include <stdarg.h>

// ============== CUSTOMIZE THESE FOR YOUR BEE ==============

#define DEVICE_TYPE "esp32-display"
#define FIRMWARE_VERSION "1.0.0-display"
#define DEFAULT_MQTT_PORT 1883

// Display pins (adjust for your hardware)
#define TFT_BACKLIGHT 38        // Backlight PWM pin
#define TFT_POWER 15            // LCD power enable (if applicable)

// Screen dimensions (adjust for your display)
#define SCREEN_WIDTH 170
#define SCREEN_HEIGHT 320

// Battery monitoring (optional - set to -1 to disable)
#define BATTERY_ADC_PIN 4       // ADC pin for battery voltage
#define BATTERY_SAMPLES 10      // Number of samples to average

// ============== Global Objects ==============

TFT_eSPI tft = TFT_eSPI();
TFT_eSprite displaySprite = TFT_eSprite(&tft);
bool spriteValid = false;

espMqttClient mqttClient;
AsyncWebServer webServer(80);
Preferences preferences;
WiFiManager wifiManager;

// Timers
unsigned long lastHealthPublish = 0;
unsigned long lastStatePublish = 0;
unsigned long lastReconnectAttempt = 0;
unsigned long lastDisplayUpdate = 0;
unsigned long lastBatteryRead = 0;
unsigned long lastBrightnessCheck = 0;

// ============== Display Colors ==============
// Using RGB565 format

#define COLOR_BG         0x0000  // Black
#define COLOR_TEXT       0xFFFF  // White
#define COLOR_ACCENT     0x07FF  // Cyan
#define COLOR_SUCCESS    0x07E0  // Green
#define COLOR_ERROR      0xF800  // Red
#define COLOR_WARNING    0xFFE0  // Yellow
#define COLOR_MUTED      0x8410  // Gray

// ============== Device Configuration ==============

struct DeviceConfig {
    // === Core config ===
    char deviceName[32];
    char mqttServer[64];
    int mqttPort;
    char mqttUser[32];
    char mqttPass[32];
    char topicPrefix[32];

    // === Display Bee config ===
    char companionBeeId[16];    // Device ID of companion bee to monitor
    int dimStartHour;           // Hour to start dimming (0-23)
    int dimEndHour;             // Hour to end dimming (0-23)
    int dimBrightness;          // Brightness during dim hours (0-255)
    int normalBrightness;       // Normal brightness (0-255)

    // === Add your bee-specific config here ===
} config;

// ============== Device State ==============

struct DeviceState {
    // === Core state ===
    bool mqttConnected = false;
    unsigned long uptime = 0;
    int rssi = 0;

    // === Display Bee state ===
    int currentScreen = 0;          // Current view/screen index
    bool displayNeedsUpdate = true; // Flag to trigger redraw

    // Battery
    float batteryVoltage = 0.0f;
    int batteryPercent = -1;        // -1 = no battery detected
    bool batteryCharging = false;

    // Companion Bee
    bool companionOnline = false;

    // Brightness
    int currentBrightness = 255;
    bool isDimmed = false;

    // === Add your bee-specific state here ===
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

// Debug tracking
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

// Display Bee functions
void setupDisplay();
void setupSprite();
void updateDisplay();
void updateBrightness();
void readBattery();
void drawBatteryIcon(int x, int y);
void pingCompanionBee();

// Add your screen drawing functions here
void drawScreen0();  // Main screen
void drawScreen1();  // Secondary screen
// void drawScreen2();

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

// ============== Device ID & Topics ==============

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

    // Core config
    strlcpy(config.deviceName, preferences.getString("deviceName", "DisplayBee").c_str(), sizeof(config.deviceName));
    strlcpy(config.mqttServer, preferences.getString("mqttServer", "192.168.0.95").c_str(), sizeof(config.mqttServer));
    config.mqttPort = preferences.getInt("mqttPort", DEFAULT_MQTT_PORT);
    strlcpy(config.mqttUser, preferences.getString("mqttUser", "").c_str(), sizeof(config.mqttUser));
    strlcpy(config.mqttPass, preferences.getString("mqttPass", "").c_str(), sizeof(config.mqttPass));
    strlcpy(config.topicPrefix, preferences.getString("topicPrefix", "homecontrol").c_str(), sizeof(config.topicPrefix));

    // Display Bee config
    strlcpy(config.companionBeeId, preferences.getString("companionId", "").c_str(), sizeof(config.companionBeeId));
    config.dimStartHour = preferences.getInt("dimStart", 1);
    config.dimEndHour = preferences.getInt("dimEnd", 7);
    config.dimBrightness = preferences.getInt("dimBright", 20);
    config.normalBrightness = preferences.getInt("normBright", 255);

    preferences.end();

    Serial.printf("[Config] Loaded: %s @ %s:%d\n", config.deviceName, config.mqttServer, config.mqttPort);
}

void saveConfig() {
    preferences.begin("bee-config", false);

    // Core config
    preferences.putString("deviceName", config.deviceName);
    preferences.putString("mqttServer", config.mqttServer);
    preferences.putInt("mqttPort", config.mqttPort);
    preferences.putString("mqttUser", config.mqttUser);
    preferences.putString("mqttPass", config.mqttPass);
    preferences.putString("topicPrefix", config.topicPrefix);

    // Display Bee config
    preferences.putString("companionId", config.companionBeeId);
    preferences.putInt("dimStart", config.dimStartHour);
    preferences.putInt("dimEnd", config.dimEndHour);
    preferences.putInt("dimBright", config.dimBrightness);
    preferences.putInt("normBright", config.normalBrightness);

    preferences.end();

    mqttLog("[Config] Saved\n");
}

// ============== Display Setup ==============

void setupDisplay() {
    // Enable LCD power if applicable
    if (TFT_POWER >= 0) {
        pinMode(TFT_POWER, OUTPUT);
        digitalWrite(TFT_POWER, HIGH);
        delay(10);
    }

    tft.init();
    tft.invertDisplay(true);  // T-Display S3 needs this
    tft.setRotation(0);       // Portrait mode

    // Setup backlight PWM
    ledcAttach(TFT_BACKLIGHT, 5000, 8);  // 5kHz, 8-bit
    ledcWrite(TFT_BACKLIGHT, config.normalBrightness);
    state.currentBrightness = config.normalBrightness;

    tft.fillScreen(COLOR_BG);
    tft.setTextColor(COLOR_TEXT);
    tft.setTextDatum(MC_DATUM);
    tft.drawString("Starting...", SCREEN_WIDTH/2, SCREEN_HEIGHT/2, 4);

    mqttLog("[Display] Initialized\n");
}

void setupSprite() {
    // Create sprite for flicker-free rendering
    displaySprite.setColorDepth(16);

    if (displaySprite.createSprite(SCREEN_WIDTH, SCREEN_HEIGHT)) {
        spriteValid = true;
        displaySprite.fillSprite(COLOR_BG);
        mqttLog("[Display] Sprite created: %dx%d\n", SCREEN_WIDTH, SCREEN_HEIGHT);
    } else {
        spriteValid = false;
        mqttLog("[Display] ERROR: Sprite creation failed!\n");
        tft.fillScreen(COLOR_ERROR);
        tft.drawString("Sprite Error!", SCREEN_WIDTH/2, SCREEN_HEIGHT/2, 4);
    }
}

// ============== Battery Monitoring ==============

void readBattery() {
    if (BATTERY_ADC_PIN < 0) return;  // Battery monitoring disabled

    // Average multiple samples
    long sum = 0;
    for (int i = 0; i < BATTERY_SAMPLES; i++) {
        sum += analogRead(BATTERY_ADC_PIN);
        delay(2);
    }
    float adcValue = sum / BATTERY_SAMPLES;

    // Convert to voltage (assuming 2:1 voltage divider, 3.3V reference, 12-bit ADC)
    // Adjust multiplier based on your voltage divider
    float voltage = (adcValue / 4095.0) * 3.3 * 2.0;

    state.batteryVoltage = voltage;

    // Detect charging (voltage > 4.25V typically means USB power)
    state.batteryCharging = (voltage > 4.25);

    // Calculate percentage (3.0V = 0%, 4.2V = 100%)
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

void drawBatteryIcon(int x, int y) {
    if (!spriteValid) return;

    int w = 24, h = 12;

    // Battery outline
    displaySprite.drawRect(x, y, w, h, COLOR_TEXT);
    displaySprite.fillRect(x + w, y + 3, 3, 6, COLOR_TEXT);  // Terminal

    // Fill based on percentage
    if (state.batteryPercent >= 0) {
        int fillW = (state.batteryPercent * (w - 4)) / 100;
        uint16_t fillColor = COLOR_SUCCESS;
        if (state.batteryPercent < 20) fillColor = COLOR_ERROR;
        else if (state.batteryPercent < 50) fillColor = COLOR_WARNING;

        displaySprite.fillRect(x + 2, y + 2, fillW, h - 4, fillColor);

        // Show percentage or charging
        displaySprite.setTextDatum(ML_DATUM);
        displaySprite.setTextColor(COLOR_TEXT, COLOR_BG);
        if (state.batteryCharging) {
            displaySprite.drawString("USB", x + w + 8, y + h/2, 1);
        } else {
            displaySprite.drawString(String(state.batteryPercent) + "%", x + w + 8, y + h/2, 1);
        }
    }
}

// ============== Brightness Control ==============

void updateBrightness() {
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo, 0)) return;

    int hour = timeinfo.tm_hour;
    bool shouldDim = false;

    // Handle overnight range (e.g., 23:00 to 06:00)
    if (config.dimStartHour > config.dimEndHour) {
        shouldDim = (hour >= config.dimStartHour || hour < config.dimEndHour);
    } else {
        shouldDim = (hour >= config.dimStartHour && hour < config.dimEndHour);
    }

    int targetBrightness = shouldDim ? config.dimBrightness : config.normalBrightness;

    if (state.currentBrightness != targetBrightness) {
        state.currentBrightness = targetBrightness;
        state.isDimmed = shouldDim;
        ledcWrite(TFT_BACKLIGHT, state.currentBrightness);
        mqttLog("[Display] Brightness: %d (%s)\n", targetBrightness, shouldDim ? "dimmed" : "normal");
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

// ============== Screen Drawing ==============
// Implement your custom screens here

void drawScreen0() {
    if (!spriteValid) return;

    displaySprite.fillSprite(COLOR_BG);

    // Draw battery icon (top right)
    drawBatteryIcon(SCREEN_WIDTH - 55, 5);

    // Example content - customize for your bee
    displaySprite.setTextColor(COLOR_TEXT, COLOR_BG);
    displaySprite.setTextDatum(MC_DATUM);
    displaySprite.drawString(config.deviceName, SCREEN_WIDTH/2, 60, 4);

    // Status info
    displaySprite.setTextDatum(TL_DATUM);
    displaySprite.setTextColor(COLOR_MUTED, COLOR_BG);
    displaySprite.drawString("WiFi: " + String(state.rssi) + " dBm", 10, 100, 2);
    displaySprite.drawString("MQTT: " + String(state.mqttConnected ? "OK" : "---"), 10, 120, 2);
    displaySprite.drawString("Uptime: " + String(state.uptime) + "s", 10, 140, 2);

    // Companion status
    if (strlen(config.companionBeeId) > 0) {
        displaySprite.setTextColor(state.companionOnline ? COLOR_SUCCESS : COLOR_ERROR, COLOR_BG);
        displaySprite.drawString("Companion: " + String(state.companionOnline ? "Online" : "Offline"), 10, 160, 2);
    }

    displaySprite.pushSprite(0, 0);
}

void drawScreen1() {
    if (!spriteValid) return;

    displaySprite.fillSprite(COLOR_BG);

    // Add your second screen content here
    displaySprite.setTextColor(COLOR_TEXT, COLOR_BG);
    displaySprite.setTextDatum(MC_DATUM);
    displaySprite.drawString("Screen 2", SCREEN_WIDTH/2, SCREEN_HEIGHT/2, 4);

    displaySprite.pushSprite(0, 0);
}

void updateDisplay() {
    // Update interval based on current screen
    unsigned long interval = (state.currentScreen == 0) ? 1000 : 500;

    if (!state.displayNeedsUpdate && millis() - lastDisplayUpdate < interval) {
        return;
    }

    lastDisplayUpdate = millis();
    state.displayNeedsUpdate = false;

    switch (state.currentScreen) {
        case 0: drawScreen0(); break;
        case 1: drawScreen1(); break;
        default: drawScreen0(); break;
    }
}

// ============== MQTT Callbacks ==============

void onMqttConnect(bool sessionPresent) {
    mqttLog("[MQTT] Connected!\n");
    state.mqttConnected = true;
    lastConnectTime = millis();
    lastError = "none";

    // Subscribe to command topic
    mqttClient.subscribe(commandTopic.c_str(), 1);

    // Subscribe to companion availability
    if (strlen(config.companionBeeId) > 0) {
        mqttClient.subscribe(companionAvailabilityTopic.c_str(), 1);
        mqttLog("[MQTT] Subscribed to companion: %s\n", config.companionBeeId);
    }

    // Publish online status
    mqttClient.publish(availabilityTopic.c_str(), 1, true, "online");

    publishDiscovery();
    publishState();
    publishHealth();
}

void onMqttDisconnect(espMqttClientTypes::DisconnectReason reason) {
    mqttLog("[MQTT] Disconnected: %d\n", (int)reason);
    state.mqttConnected = false;
    reconnectCount++;
}

void onMqttMessage(const espMqttClientTypes::MessageProperties& properties,
                   const char* topic, const uint8_t* payload, size_t len,
                   size_t index, size_t total) {

    String topicStr = String(topic);

    // Check if companion availability message
    if (topicStr == companionAvailabilityTopic) {
        String status = String((char*)payload).substring(0, len);
        state.companionOnline = (status == "online");
        state.displayNeedsUpdate = true;
        mqttLog("[Companion] %s is %s\n", config.companionBeeId, status.c_str());
        return;
    }

    // Handle commands
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, payload, len);
    if (error) return;

    if (doc.containsKey("capability")) {
        String instance = doc["capability"]["instance"].as<String>();
        String value = doc["capability"]["value"].as<String>();

        mqttLog("[CMD] %s = %s\n", instance.c_str(), value.c_str());

        // Handle ping command
        if (instance == "ping") {
            // Someone pinged us - could trigger animation
            state.displayNeedsUpdate = true;
        }

        // === ADD YOUR COMMAND HANDLING HERE ===
    }
}

// ============== MQTT Setup ==============

void setupMQTT() {
    currentClientId = String(DEVICE_TYPE) + "-" + deviceId + "-" + String(millis() % 100000);

    mqttClient.setClientId(currentClientId.c_str());
    mqttClient.setServer(config.mqttServer, config.mqttPort);
    mqttClient.setKeepAlive(60);
    mqttClient.setCleanSession(true);
    mqttClient.setWill(availabilityTopic.c_str(), 1, true, "offline");

    if (strlen(config.mqttUser) > 0) {
        mqttClient.setCredentials(config.mqttUser, config.mqttPass);
    }

    mqttClient.onConnect(onMqttConnect);
    mqttClient.onDisconnect(onMqttDisconnect);
    mqttClient.onMessage(onMqttMessage);
}

void connectMQTT() {
    if (state.mqttConnected || !WiFi.isConnected()) return;
    mqttLog("[MQTT] Connecting...\n");
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
    doc["has_display"] = true;
    doc["has_battery"] = (BATTERY_ADC_PIN >= 0);

    if (strlen(config.companionBeeId) > 0) {
        doc["companion_bee"] = config.companionBeeId;
    }

    char buffer[512];
    serializeJson(doc, buffer);
    mqttClient.publish(discoveryTopic.c_str(), 1, true, buffer);
}

void publishState() {
    JsonDocument doc;
    doc["rssi"] = state.rssi;
    doc["uptime"] = state.uptime;
    doc["ip"] = WiFi.localIP().toString();
    doc["display_screen"] = state.currentScreen;

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
    doc["free_heap"] = ESP.getFreeHeap();
    doc["reconnect_count"] = reconnectCount;
    doc["firmware"] = FIRMWARE_VERSION;
    doc["display_screen"] = state.currentScreen;

    // Battery info
    if (BATTERY_ADC_PIN >= 0) {
        doc["battery_voltage"] = state.batteryVoltage;
        doc["battery_percent"] = state.batteryPercent;
        doc["battery_charging"] = state.batteryCharging;
    }

    // Companion status
    if (strlen(config.companionBeeId) > 0) {
        doc["companion_online"] = state.companionOnline;
    }

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
        html += "h1{color:#0f4c75;}a{color:#3282b8;}</style></head><body>";
        html += "<h1>" + String(config.deviceName) + "</h1>";
        html += "<div class='card'><b>Device ID:</b> " + deviceId + "<br>";
        html += "<b>Firmware:</b> " + String(FIRMWARE_VERSION) + "<br>";
        html += "<b>Battery:</b> " + String(state.batteryPercent) + "% (" + String(state.batteryVoltage, 2) + "V)";
        if (state.batteryCharging) html += " [Charging]";
        html += "<br><b>MQTT:</b> " + String(state.mqttConnected ? "Connected" : "Disconnected");
        html += "<br><b>Companion:</b> " + String(strlen(config.companionBeeId) > 0 ? config.companionBeeId : "None") + " - " + String(state.companionOnline ? "Online" : "Offline");
        html += "</div>";
        html += "<p><a href='/config'>Settings</a></p>";
        html += "</body></html>";
        request->send(200, "text/html", html);
    });

    // Config page with display-bee specific settings
    webServer.on("/config", HTTP_GET, [](AsyncWebServerRequest *request) {
        String html = "<!DOCTYPE html><html><head><title>Config</title>";
        html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
        html += "<style>body{font-family:sans-serif;padding:20px;background:#1a1a2e;color:#eee;}";
        html += "input,select{width:100%;padding:8px;margin:5px 0 15px 0;border-radius:4px;border:1px solid #333;background:#16213e;color:#eee;}";
        html += "button{background:#0f4c75;color:white;padding:10px 20px;border:none;border-radius:4px;}";
        html += "h2{color:#3282b8;border-bottom:1px solid #333;padding-bottom:10px;}</style></head><body>";
        html += "<h1>Settings</h1><form method='POST' action='/config'>";

        html += "<h2>Device</h2>";
        html += "<label>Device Name</label><input name='deviceName' value='" + String(config.deviceName) + "'>";

        html += "<h2>MQTT</h2>";
        html += "<label>Server</label><input name='mqttServer' value='" + String(config.mqttServer) + "'>";
        html += "<label>Port</label><input type='number' name='mqttPort' value='" + String(config.mqttPort) + "'>";

        html += "<h2>Companion Bee</h2>";
        html += "<label>Companion Device ID</label><input name='companionId' value='" + String(config.companionBeeId) + "' placeholder='e.g., 503035d4db1c'>";

        html += "<h2>Auto-Dimming</h2>";
        html += "<label>Dim Start Hour (0-23)</label><input type='number' name='dimStart' min='0' max='23' value='" + String(config.dimStartHour) + "'>";
        html += "<label>Dim End Hour (0-23)</label><input type='number' name='dimEnd' min='0' max='23' value='" + String(config.dimEndHour) + "'>";
        html += "<label>Dim Brightness (0-255)</label><input type='number' name='dimBright' min='0' max='255' value='" + String(config.dimBrightness) + "'>";
        html += "<label>Normal Brightness (0-255)</label><input type='number' name='normBright' min='0' max='255' value='" + String(config.normalBrightness) + "'>";

        html += "<button type='submit'>Save</button></form>";
        html += "<p><a href='/'>Back</a> | <a href='/reset'>Reset WiFi</a></p>";
        html += "</body></html>";
        request->send(200, "text/html", html);
    });

    // Config save handler
    webServer.on("/config", HTTP_POST, [](AsyncWebServerRequest *request) {
        if (request->hasParam("deviceName", true))
            strlcpy(config.deviceName, request->getParam("deviceName", true)->value().c_str(), sizeof(config.deviceName));
        if (request->hasParam("mqttServer", true))
            strlcpy(config.mqttServer, request->getParam("mqttServer", true)->value().c_str(), sizeof(config.mqttServer));
        if (request->hasParam("mqttPort", true))
            config.mqttPort = request->getParam("mqttPort", true)->value().toInt();
        if (request->hasParam("companionId", true))
            strlcpy(config.companionBeeId, request->getParam("companionId", true)->value().c_str(), sizeof(config.companionBeeId));
        if (request->hasParam("dimStart", true))
            config.dimStartHour = request->getParam("dimStart", true)->value().toInt();
        if (request->hasParam("dimEnd", true))
            config.dimEndHour = request->getParam("dimEnd", true)->value().toInt();
        if (request->hasParam("dimBright", true))
            config.dimBrightness = request->getParam("dimBright", true)->value().toInt();
        if (request->hasParam("normBright", true))
            config.normalBrightness = request->getParam("normBright", true)->value().toInt();

        saveConfig();
        buildTopics();
        request->redirect("/config?saved=1");
    });

    webServer.on("/reset", HTTP_GET, [](AsyncWebServerRequest *request) {
        request->send(200, "text/html", "<h1>Resetting WiFi...</h1>");
        delay(1000);
        wifiManager.resetSettings();
        ESP.restart();
    });

    webServer.begin();
}

// ============== Setup ==============

void setup() {
    Serial.begin(115200);
    delay(100);
    Serial.println("\n\n=== Display Bee Starting ===");

    deviceId = getDeviceId();
    Serial.printf("[Boot] Device ID: %s\n", deviceId.c_str());

    loadConfig();
    buildTopics();

    // Setup display first (shows boot screen)
    setupDisplay();

    // Setup WiFi
    wifiManager.setConfigPortalTimeout(180);
    String apName = String(config.deviceName) + "-Setup";
    if (!wifiManager.autoConnect(apName.c_str())) {
        ESP.restart();
    }

    Serial.printf("[WiFi] Connected: %s\n", WiFi.localIP().toString().c_str());

    // Setup NTP for dimming schedule
    configTzTime("CST6CDT,M3.2.0,M11.1.0", "pool.ntp.org");

    // Create sprite for flicker-free rendering
    setupSprite();

    // Setup MQTT
    setupMQTT();
    connectMQTT();

    // Setup web server
    setupWebServer();

    Serial.println("=== Display Bee Ready ===\n");
}

// ============== Loop ==============

void loop() {
    state.uptime = millis() / 1000;
    state.rssi = WiFi.RSSI();

    // Read battery every 2 seconds
    if (millis() - lastBatteryRead > 2000) {
        lastBatteryRead = millis();
        readBattery();
    }

    // Check brightness schedule every minute
    if (millis() - lastBrightnessCheck > 60000) {
        lastBrightnessCheck = millis();
        updateBrightness();
    }

    // Update display
    updateDisplay();

    // MQTT reconnection
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

    publishBufferedLogs();

    // === ADD YOUR BEE'S LOOP LOGIC HERE ===
}
