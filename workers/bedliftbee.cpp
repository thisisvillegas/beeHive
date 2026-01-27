/**
 * BedLiftBee - Happy Jack Bed Lift Controller
 * ESP32-C3 Mini with 2-channel relay module + safety sensors
 *
 * Part of the Bee Colony - uses same patterns as TinyBee1/kayciBee1
 * - WiFiManager for configuration
 * - ESPAsyncWebServer for status/config
 * - espMqttClient for async MQTT
 *
 * Relays simulate pressing the rocker switch:
 * - Relay 1 (GPIO2): UP - raises the bed
 * - Relay 2 (GPIO3): DOWN - lowers the bed
 *
 * Safety Features:
 * - HC-SR04 ultrasonic sensor for distance/position tracking
 * - Reed switch for limit detection (magnet at top or bottom)
 * - Auto-stop when limit reached or obstacle detected
 * - Auto-nudge: reverses briefly off the limit so opposite direction still works
 * - Directional limit: only blocks the direction that caused the limit
 *
 * MQTT Commands:
 * - {"capability":{"instance":"bedLift","value":"raise"}}
 * - {"capability":{"instance":"bedLift","value":"lower"}}
 * - {"capability":{"instance":"bedLift","value":"stop"}}
 * - {"capability":{"instance":"bedLift","value":"sendToTop"}}
 * - {"capability":{"instance":"bedLift","value":"sendToBottom"}}
 * - {"capability":{"instance":"bedLift","value":"calibrate_top"}}
 * - {"capability":{"instance":"bedLift","value":"calibrate_bottom"}}
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

#define DEVICE_TYPE "esp32-c3-bedlift"
#define FIRMWARE_VERSION "1.5.0-bedlift"
#define DEFAULT_MQTT_PORT 1883

// GPIO Pins - Relays
#define RELAY_UP_PIN 2     // GPIO2 - Relay 1 (raises bed)
#define RELAY_DOWN_PIN 3   // GPIO3 - Relay 2 (lowers bed)
#define LED_PIN 8          // GPIO8 - Onboard LED

// GPIO Pins - HC-SR04 Ultrasonic
#define ULTRASONIC_TRIG_PIN 4   // GPIO4 - Trigger
#define ULTRASONIC_ECHO_PIN 5   // GPIO5 - Echo (use voltage divider!)

// GPIO Pins - Reed Switch (single limit switch - stops all movement)
#define REED_LIMIT_PIN 7   // GPIO7 - Limit switch (magnet at top OR bottom)

// Relay active state (most modules are active LOW)
#define RELAY_ON LOW
#define RELAY_OFF HIGH

// Default lift duration in milliseconds
#define DEFAULT_LIFT_DURATION 3000

// Default nudge duration - auto-reverse off limit switch
#define DEFAULT_NUDGE_DURATION 300

// Max duration for send-to-limit mode (safety timeout)
#define MAX_SEND_TO_LIMIT_DURATION 30000

// Safety thresholds
#define MIN_SAFE_DISTANCE_CM 10     // Stop if obstacle closer than this
#define ULTRASONIC_TIMEOUT_US 30000 // 30ms timeout (~5m max range)
#define ULTRASONIC_READ_INTERVAL 100 // Read sensor every 100ms

// ============== Global Objects ==============

espMqttClient mqttClient;
AsyncWebServer webServer(80);
Preferences preferences;
WiFiManager wifiManager;

// Timers
unsigned long lastHealthPublish = 0;
unsigned long lastStatePublish = 0;
unsigned long lastReconnectAttempt = 0;
unsigned long lastUltrasonicRead = 0;

// ============== Device State ==============

struct DeviceConfig {
    char deviceName[32];
    char mqttServer[64];
    int mqttPort;
    char mqttUser[32];
    char mqttPass[32];
    char topicPrefix[32];
    unsigned long liftDuration;   // milliseconds
    unsigned long nudgeDuration;  // milliseconds - auto-reverse off limit
} config;

struct DeviceState {
    bool mqttConnected = false;
    unsigned long uptime = 0;
    int rssi = 0;
    // Bed lift state
    bool isLifting = false;
    bool sendToLimit = false;   // true = ignore timer, run until limit switch
    String liftDirection = "";  // "raise" or "lower"
    unsigned long liftStartTime = 0;
    bool relayUp = false;
    bool relayDown = false;
    // Safety sensors
    float distanceCm = 0;           // Current ultrasonic reading
    bool ultrasonicOk = false;      // Sensor connected and reading valid
    bool atLimit = false;           // Reed switch triggered (at top OR bottom)
    String lastStopReason = "";     // Why we stopped (timer/limit/obstacle/manual)
    String limitDirection = "";     // Direction that caused limit ("raise"/"lower"/"")
    bool isNudging = false;         // Auto-reversing off limit switch
    unsigned long nudgeStartTime = 0;
    // Calibration data
    float calibratedTopCm = 0;      // Distance reading at top
    float calibratedBottomCm = 0;   // Distance reading at bottom
    int positionPercent = -1;       // -1 = uncalibrated, 0-100 = position
} state;

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
void loadConfig();
void saveConfig();
String getDeviceId();
void setupWebServer();
void startLift(const String& direction, bool toLimit = false);
void stopLift();
void stopLift(const String& reason);
void updateLift();
// Nudge (auto-reverse off limit)
void startNudge(const String& awayFrom);
void updateNudge();
// Safety sensors
void setupSensors();
float readUltrasonic();
void readReedSwitch();
void updateSensors();
bool isSafeToMove(const String& direction);
void calibratePosition(const String& position);
int calculatePositionPercent();

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

    // Blink LED to indicate connection
    for (int i = 0; i < 3; i++) {
        digitalWrite(LED_PIN, LOW);
        delay(100);
        digitalWrite(LED_PIN, HIGH);
        delay(100);
    }
}

void onMqttDisconnect(espMqttClientTypes::DisconnectReason reason) {
    mqttLog("[MQTT] Disconnected! Reason: %d\n", (int)reason);
    state.mqttConnected = false;

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

        if (instance && strcmp(instance, "bedLift") == 0) {
            const char* value = capability["value"];

            if (strcmp(value, "raise") == 0) {
                mqttLog("[CMD] RAISE - Starting bed lift UP\n");
                startLift("raise");
            }
            else if (strcmp(value, "lower") == 0) {
                mqttLog("[CMD] LOWER - Starting bed lift DOWN\n");
                startLift("lower");
            }
            else if (strcmp(value, "stop") == 0) {
                mqttLog("[CMD] STOP - Stopping bed lift\n");
                stopLift("manual");
            }
            else if (strcmp(value, "sendToTop") == 0) {
                mqttLog("[CMD] SEND TO TOP - Raising until limit\n");
                startLift("raise", true);
            }
            else if (strcmp(value, "sendToBottom") == 0) {
                mqttLog("[CMD] SEND TO BOTTOM - Lowering until limit\n");
                startLift("lower", true);
            }
            else if (strcmp(value, "calibrate_top") == 0) {
                Serial.println("[CMD] CALIBRATE TOP - Setting current position as top");
                calibratePosition("top");
            }
            else if (strcmp(value, "calibrate_bottom") == 0) {
                Serial.println("[CMD] CALIBRATE BOTTOM - Setting current position as bottom");
                calibratePosition("bottom");
            }
        }
    }
}

// ============== Bed Lift Control ==============

void startLift(const String& direction, bool toLimit) {
    // Safety check before moving
    if (!isSafeToMove(direction)) {
        mqttLog("[LIFT] BLOCKED - Not safe to move %s\n", direction.c_str());
        return;
    }

    // Safety: stop any current operation first
    stopLift("starting_new");

    state.liftDirection = direction;
    state.liftStartTime = millis();
    state.isLifting = true;
    state.sendToLimit = toLimit;
    state.lastStopReason = "";

    if (direction == "raise") {
        digitalWrite(RELAY_UP_PIN, RELAY_ON);
        digitalWrite(RELAY_DOWN_PIN, RELAY_OFF);
        state.relayUp = true;
        state.relayDown = false;
        Serial.printf("[LIFT] Relay UP activated%s\n", toLimit ? " (send to limit)" : "");
    }
    else if (direction == "lower") {
        digitalWrite(RELAY_UP_PIN, RELAY_OFF);
        digitalWrite(RELAY_DOWN_PIN, RELAY_ON);
        state.relayUp = false;
        state.relayDown = true;
        Serial.printf("[LIFT] Relay DOWN activated%s\n", toLimit ? " (send to limit)" : "");
    }

    // LED on while lifting
    digitalWrite(LED_PIN, LOW);

    publishState();
}

void stopLift() {
    stopLift("manual");
}

void stopLift(const String& reason) {
    digitalWrite(RELAY_UP_PIN, RELAY_OFF);
    digitalWrite(RELAY_DOWN_PIN, RELAY_OFF);

    if (state.isLifting) {
        unsigned long duration = millis() - state.liftStartTime;
        mqttLog("[LIFT] Stopped after %lu ms (reason: %s)\n", duration, reason.c_str());
        state.lastStopReason = reason;
    }

    if (state.isNudging) {
        mqttLog("[NUDGE] Cancelled (reason: %s)\n", reason.c_str());
        state.isNudging = false;
        state.nudgeStartTime = 0;
    }

    state.isLifting = false;
    state.sendToLimit = false;
    state.liftDirection = "";
    state.liftStartTime = 0;
    state.relayUp = false;
    state.relayDown = false;

    // LED off when stopped
    digitalWrite(LED_PIN, HIGH);

    publishState();
}

void updateLift() {
    if (!state.isLifting) return;

    // Check timer
    unsigned long elapsed = millis() - state.liftStartTime;
    if (state.sendToLimit) {
        // Safety timeout for send-to-limit mode
        if (elapsed >= MAX_SEND_TO_LIMIT_DURATION) {
            mqttLog("[SAFETY] Send-to-limit timeout (%lu ms) - limit switch may have failed!\n", MAX_SEND_TO_LIMIT_DURATION);
            stopLift("safety_timeout");
            return;
        }
    } else {
        // Normal timed lift
        if (elapsed >= config.liftDuration) {
            Serial.printf("[LIFT] Timer expired (%lu ms)\n", config.liftDuration);
            stopLift("timer");
            return;
        }
    }

    // Safety: Check limit switch (directional)
    if (state.atLimit) {
        if (state.limitDirection == "" || state.limitDirection == state.liftDirection) {
            mqttLog("[SAFETY] Limit reached while %s!\n", state.liftDirection.c_str());
            stopLift("limit");
            return;
        }
        // Moving in opposite direction from limit - allow it
    }

    // Safety: Check ultrasonic distance when lowering
    if (state.liftDirection == "lower" && state.ultrasonicOk) {
        if (state.distanceCm > 0 && state.distanceCm < MIN_SAFE_DISTANCE_CM) {
            mqttLog("[SAFETY] Obstacle detected! Distance: %.1f cm\n", state.distanceCm);
            stopLift("obstacle");
            return;
        }
    }
}

// ============== Safety Sensors ==============

void setupSensors() {
    // Ultrasonic HC-SR04
    pinMode(ULTRASONIC_TRIG_PIN, OUTPUT);
    pinMode(ULTRASONIC_ECHO_PIN, INPUT);
    digitalWrite(ULTRASONIC_TRIG_PIN, LOW);

    // Single reed switch (with internal pullup - LOW when magnet present)
    pinMode(REED_LIMIT_PIN, INPUT_PULLUP);

    Serial.println("[SENSORS] Initialized: Ultrasonic + Reed limit switch");

    // Initial read
    updateSensors();
}

float readUltrasonic() {
    // Send trigger pulse
    digitalWrite(ULTRASONIC_TRIG_PIN, LOW);
    delayMicroseconds(2);
    digitalWrite(ULTRASONIC_TRIG_PIN, HIGH);
    delayMicroseconds(10);
    digitalWrite(ULTRASONIC_TRIG_PIN, LOW);

    // Measure echo pulse duration
    unsigned long duration = pulseIn(ULTRASONIC_ECHO_PIN, HIGH, ULTRASONIC_TIMEOUT_US);

    if (duration == 0) {
        // Timeout - no echo received
        return -1;
    }

    // Calculate distance: speed of sound = 343 m/s = 0.0343 cm/us
    // Distance = (duration * 0.0343) / 2
    float distance = (duration * 0.0343) / 2.0;

    return distance;
}

void readReedSwitch() {
    // Reed switch is normally-closed (NC) type - inverted logic
    // NC switch: no magnet = LOW, magnet present = HIGH
    bool limitTriggered = (digitalRead(REED_LIMIT_PIN) == HIGH);

    // Detect changes and log to MQTT
    if (limitTriggered != state.atLimit) {
        state.atLimit = limitTriggered;

        if (limitTriggered) {
            if (state.isLifting) {
                // Record which direction caused the limit before stopping
                String hitDirection = state.liftDirection;
                state.limitDirection = hitDirection;
                mqttLog("[REED] %s limit hit - stopping and nudging back\n", hitDirection.c_str());
                stopLift("limit");
                startNudge(hitDirection);
            } else if (state.isNudging) {
                // Limit re-triggered during nudge - stop everything
                mqttLog("[REED] Limit triggered during nudge - emergency stop\n");
                state.limitDirection = "";
                stopLift("limit_during_nudge");
            } else {
                // Triggered without active movement (manual push or boot)
                state.limitDirection = "";
                mqttLog("[REED] Limit triggered (no active movement)\n");
            }
        } else {
            // Limit cleared - reset directional lock
            mqttLog("[REED] Limit cleared\n");
            state.limitDirection = "";
        }
    }
}

// Ultrasonic debouncing (kept for future use, but logging disabled)
int ultrasonicFailCount = 0;
#define ULTRASONIC_FAIL_THRESHOLD 5  // Must fail 5x in a row to go offline

void updateSensors() {
    // Read ultrasonic at intervals (avoid blocking too often)
    if (millis() - lastUltrasonicRead >= ULTRASONIC_READ_INTERVAL) {
        lastUltrasonicRead = millis();

        float distance = readUltrasonic();
        bool wasOk = state.ultrasonicOk;

        if (distance > 0) {
            // Good reading - update distance and reset fail counter
            state.distanceCm = distance;
            ultrasonicFailCount = 0;
            state.ultrasonicOk = true;
        } else {
            // Bad reading - increment fail counter
            ultrasonicFailCount++;

            // Only mark offline after multiple consecutive failures
            if (ultrasonicFailCount >= ULTRASONIC_FAIL_THRESHOLD) {
                state.ultrasonicOk = false;
            }
            // Keep last good distance reading (don't clear it)
        }

        // Update position percentage if calibrated
        state.positionPercent = calculatePositionPercent();

        // Ultrasonic logging disabled - using reed switches for limits
    }

    // Reed switch can be read every loop (it's fast)
    readReedSwitch();
}

bool isSafeToMove(const String& direction) {
    // Don't allow new movement during nudge
    if (state.isNudging) {
        Serial.println("[SAFETY] Nudge in progress - cannot move");
        return false;
    }

    // Check limit switch with directional awareness
    if (state.atLimit) {
        if (state.limitDirection == "" || state.limitDirection == direction) {
            // Unknown direction or same direction that caused limit - block
            Serial.printf("[SAFETY] At %s limit - cannot %s\n",
                state.limitDirection.length() ? state.limitDirection.c_str() : "unknown",
                direction.c_str());
            return false;
        }
        // Moving opposite from limit - allow
        Serial.printf("[SAFETY] At %s limit - allowing %s (opposite)\n",
            state.limitDirection.c_str(), direction.c_str());
    }

    // Check ultrasonic when lowering (if sensor is working)
    if (direction == "lower" && state.ultrasonicOk) {
        if (state.distanceCm > 0 && state.distanceCm < MIN_SAFE_DISTANCE_CM) {
            Serial.printf("[SAFETY] Obstacle below! Distance: %.1f cm\n", state.distanceCm);
            return false;
        }
    }

    return true;
}

void calibratePosition(const String& position) {
    if (!state.ultrasonicOk) {
        Serial.println("[CALIBRATE] Error: Ultrasonic sensor not reading");
        return;
    }

    if (position == "top") {
        state.calibratedTopCm = state.distanceCm;
        Serial.printf("[CALIBRATE] Top position set to %.1f cm\n", state.calibratedTopCm);
    } else if (position == "bottom") {
        state.calibratedBottomCm = state.distanceCm;
        Serial.printf("[CALIBRATE] Bottom position set to %.1f cm\n", state.calibratedBottomCm);
    }

    // Recalculate position
    state.positionPercent = calculatePositionPercent();
    publishState();
}

int calculatePositionPercent() {
    // Need both calibration points
    if (state.calibratedTopCm == 0 || state.calibratedBottomCm == 0) {
        return -1;  // Not calibrated
    }
    if (!state.ultrasonicOk) {
        return -1;  // No valid reading
    }

    // Calculate percentage (0% = bottom, 100% = top)
    float range = state.calibratedBottomCm - state.calibratedTopCm;
    if (range <= 0) {
        return -1;  // Invalid calibration
    }

    float fromBottom = state.calibratedBottomCm - state.distanceCm;
    int percent = (int)((fromBottom / range) * 100);

    // Clamp to 0-100
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;

    return percent;
}

// ============== Nudge (Auto-Reverse Off Limit) ==============

void startNudge(const String& awayFrom) {
    state.isNudging = true;
    state.nudgeStartTime = millis();

    // Nudge in the opposite direction
    if (awayFrom == "lower") {
        digitalWrite(RELAY_UP_PIN, RELAY_ON);
        digitalWrite(RELAY_DOWN_PIN, RELAY_OFF);
    } else {
        digitalWrite(RELAY_UP_PIN, RELAY_OFF);
        digitalWrite(RELAY_DOWN_PIN, RELAY_ON);
    }

    // LED on during nudge
    digitalWrite(LED_PIN, LOW);

    mqttLog("[NUDGE] Backing off %s limit (%lu ms)\n", awayFrom.c_str(), config.nudgeDuration);
}

void updateNudge() {
    if (!state.isNudging) return;

    if (millis() - state.nudgeStartTime >= config.nudgeDuration) {
        digitalWrite(RELAY_UP_PIN, RELAY_OFF);
        digitalWrite(RELAY_DOWN_PIN, RELAY_OFF);

        state.isNudging = false;
        state.nudgeStartTime = 0;

        digitalWrite(LED_PIN, HIGH);

        mqttLog("[NUDGE] Complete - reed %s\n", state.atLimit ? "still triggered" : "cleared");
        publishState();
    }
}

// ============== Setup ==============

void setup() {
    Serial.begin(115200);
    delay(500);

    Serial.println("\n\n=== BedLiftBee ===");
    Serial.println("Happy Jack Bed Lift Controller");
    Serial.printf("Firmware: %s\n", FIRMWARE_VERSION);

    // Initialize GPIO
    pinMode(RELAY_UP_PIN, OUTPUT);
    pinMode(RELAY_DOWN_PIN, OUTPUT);
    pinMode(LED_PIN, OUTPUT);

    // Ensure relays are OFF at startup
    digitalWrite(RELAY_UP_PIN, RELAY_OFF);
    digitalWrite(RELAY_DOWN_PIN, RELAY_OFF);
    digitalWrite(LED_PIN, HIGH);  // LED off (active-low)

    Serial.println("[RELAYS] Initialized - both OFF");

    // Setup safety sensors
    setupSensors();

    // Generate device ID
    deviceId = getDeviceId();
    Serial.printf("Device ID: %s\n", deviceId.c_str());

    // Load configuration
    loadConfig();

    // Setup WiFi Manager
    WiFiManagerParameter mqttServerParam("mqtt_server", "MQTT Server", config.mqttServer, 64);
    WiFiManagerParameter mqttPortParam("mqtt_port", "MQTT Port", String(config.mqttPort).c_str(), 6);
    WiFiManagerParameter deviceNameParam("device_name", "Device Name", config.deviceName, 32);
    WiFiManagerParameter liftDurationParam("lift_duration", "Lift Duration (ms)", String(config.liftDuration).c_str(), 8);
    WiFiManagerParameter nudgeDurationParam("nudge_duration", "Nudge Duration (ms)", String(config.nudgeDuration).c_str(), 8);

    wifiManager.addParameter(&mqttServerParam);
    wifiManager.addParameter(&mqttPortParam);
    wifiManager.addParameter(&deviceNameParam);
    wifiManager.addParameter(&liftDurationParam);
    wifiManager.addParameter(&nudgeDurationParam);

    String apName = "BedLiftBee-Setup-" + deviceId.substring(deviceId.length() - 4);
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
    config.liftDuration = atoi(liftDurationParam.getValue());
    if (config.liftDuration < 500) config.liftDuration = DEFAULT_LIFT_DURATION;
    config.nudgeDuration = atoi(nudgeDurationParam.getValue());
    if (config.nudgeDuration < 50) config.nudgeDuration = DEFAULT_NUDGE_DURATION;
    saveConfig();

    Serial.printf("WiFi connected! IP: %s\n", WiFi.localIP().toString().c_str());

    // Setup mDNS
    String mdnsName = "bedliftbee-" + deviceId.substring(deviceId.length() - 6);
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

    // Setup MQTT
    setupMQTT();

    // Setup Web Server
    setupWebServer();

    Serial.println("Setup complete!\n");
}

// ============== Main Loop ==============

void loop() {
    // Update state
    state.uptime = millis() / 1000;
    state.rssi = WiFi.RSSI();

    // Update safety sensors
    updateSensors();

    // Update bed lift timer (includes safety checks)
    updateLift();

    // Update nudge timer (auto-reverse off limit)
    updateNudge();

    // Reconnect MQTT if needed
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

    delay(10);
}

// ============== MQTT Functions ==============

void setupMQTT() {
    mqttClient.onConnect(onMqttConnect);
    mqttClient.onDisconnect(onMqttDisconnect);
    mqttClient.onMessage(onMqttMessage);

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
    currentClientId = "bedliftbee-" + deviceId + "-" + String(millis());

    Serial.printf("[MQTT] Connecting... (attempt #%d)\n", reconnectCount);
    mqttClient.setClientId(currentClientId.c_str());
    mqttClient.connect();
}

void publishDiscovery() {
    JsonDocument doc;

    doc["device_id"] = deviceId;
    doc["name"] = config.deviceName;
    doc["type"] = DEVICE_TYPE;
    doc["model"] = "ESP32-C3 BedLiftBee";
    doc["firmware_version"] = FIRMWARE_VERSION;
    doc["ip_address"] = WiFi.localIP().toString();

    JsonArray capabilities = doc["capabilities"].to<JsonArray>();

    // Bed lift capability
    JsonObject liftCap = capabilities.add<JsonObject>();
    liftCap["type"] = "devices.capabilities.mode";
    liftCap["instance"] = "bedLift";
    JsonObject params = liftCap["parameters"].to<JsonObject>();
    params["dataType"] = "ENUM";
    JsonArray options = params["options"].to<JsonArray>();

    JsonObject opt1 = options.add<JsonObject>();
    opt1["name"] = "Raise";
    opt1["value"] = "raise";

    JsonObject opt2 = options.add<JsonObject>();
    opt2["name"] = "Lower";
    opt2["value"] = "lower";

    JsonObject opt3 = options.add<JsonObject>();
    opt3["name"] = "Stop";
    opt3["value"] = "stop";

    JsonObject opt4 = options.add<JsonObject>();
    opt4["name"] = "Send to Top";
    opt4["value"] = "sendToTop";

    JsonObject opt5 = options.add<JsonObject>();
    opt5["name"] = "Send to Bottom";
    opt5["value"] = "sendToBottom";

    doc["sensors"] = JsonArray();
    doc["timestamp"] = state.uptime;

    String payload;
    serializeJson(doc, payload);

    mqttClient.publish(discoveryTopic.c_str(), 1, true, payload.c_str());
    Serial.printf("[MQTT] Published discovery (%d bytes)\n", payload.length());
}

void publishState() {
    JsonDocument doc;

    doc["isLifting"] = state.isLifting;
    doc["sendToLimit"] = state.sendToLimit;
    doc["direction"] = state.liftDirection;
    doc["relayUp"] = state.relayUp;
    doc["relayDown"] = state.relayDown;
    doc["liftDuration"] = config.liftDuration;
    doc["rssi"] = state.rssi;
    doc["uptime"] = state.uptime;
    doc["ip"] = WiFi.localIP().toString();
    doc["timestamp"] = state.uptime;

    // Safety sensor data
    doc["distanceCm"] = state.distanceCm;
    doc["ultrasonicOk"] = state.ultrasonicOk;
    doc["atLimit"] = state.atLimit;
    doc["positionPercent"] = state.positionPercent;
    doc["lastStopReason"] = state.lastStopReason;
    doc["limitDirection"] = state.limitDirection;
    doc["isNudging"] = state.isNudging;

    if (state.isLifting) {
        unsigned long remaining = config.liftDuration - (millis() - state.liftStartTime);
        doc["liftTimeRemaining"] = remaining;
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
    doc["isLifting"] = state.isLifting;
    doc["direction"] = state.liftDirection;
    doc["timestamp"] = millis();
    doc["client_id"] = currentClientId;
    doc["firmware"] = FIRMWARE_VERSION;

    // Safety sensor data
    doc["distanceCm"] = state.distanceCm;
    doc["ultrasonicOk"] = state.ultrasonicOk;
    doc["atLimit"] = state.atLimit;
    doc["positionPercent"] = state.positionPercent;

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

// ============== Async Web Server ==============

void setupWebServer() {
    // Root page - device status
    webServer.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
        String html = R"rawhtml(
<!DOCTYPE html>
<html>
<head>
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <title>BedLiftBee</title>
    <style>
        body { font-family: -apple-system, sans-serif; background: #1a1a1a; color: #fff; margin: 0; padding: 20px; }
        .container { max-width: 400px; margin: 0 auto; }
        h1 { color: #f97316; margin-bottom: 5px; }
        .subtitle { color: #666; margin-bottom: 20px; }
        .card { background: #2a2a2a; border-radius: 12px; padding: 20px; margin-bottom: 15px; }
        .status-row { display: flex; justify-content: space-between; padding: 8px 0; border-bottom: 1px solid #333; }
        .status-row:last-child { border: none; }
        .label { color: #888; }
        .value { color: #fff; font-weight: 500; }
        .online { color: #22c55e; }
        .offline { color: #ef4444; }
        .btn { display: block; width: 100%; padding: 15px; border: none; border-radius: 8px; font-size: 16px; cursor: pointer; margin-top: 10px; box-sizing: border-box; }
        .btn-raise { background: #22c55e; color: white; }
        .btn-lower { background: #3b82f6; color: white; }
        .btn-stop { background: #ef4444; color: white; }
        .btn-secondary { background: #333; color: #fff; text-align: center; text-decoration: none; }
        .btn-grid { display: grid; grid-template-columns: 1fr 1fr; gap: 10px; }
        .btn-full { grid-column: span 2; }
        .lifting { background: #f97316; animation: pulse 1s infinite; }
        @keyframes pulse { 0%, 100% { opacity: 1; } 50% { opacity: 0.6; } }
        .badge { background: #f97316; color: white; padding: 2px 8px; border-radius: 4px; font-size: 12px; margin-left: 8px; }
    </style>
</head>
<body>
    <div class="container">
        <h1>)rawhtml";
        html += config.deviceName;
        html += R"rawhtml(<span class="badge">BED</span></h1>
        <p class="subtitle">Happy Jack Bed Lift Controller</p>
        <div class="card">
            <div class="status-row">
                <span class="label">Status</span>
                <span class="value )rawhtml";
        html += state.isLifting ? "lifting" : "";
        html += R"rawhtml(">)rawhtml";
        html += state.isLifting ? ("LIFTING " + state.liftDirection) : "IDLE";
        html += R"rawhtml(</span>
            </div>
            <div class="status-row">
                <span class="label">Lift Duration</span>
                <span class="value">)rawhtml";
        html += String(config.liftDuration / 1000.0, 1) + " sec";
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
        </div>

        <div class="card">
            <h3 style="margin-top:0;color:#f97316;">Bed Control</h3>
            <div class="btn-grid">
                <button class="btn btn-raise" onclick="fetch('/lift?dir=raise').then(()=>location.reload())">RAISE</button>
                <button class="btn btn-lower" onclick="fetch('/lift?dir=lower').then(()=>location.reload())">LOWER</button>
                <button class="btn btn-stop btn-full" onclick="fetch('/lift?dir=stop').then(()=>location.reload())">STOP</button>
            </div>
            <h3 style="margin-top:15px;margin-bottom:10px;color:#888;">Send to Limit</h3>
            <div class="btn-grid">
                <button class="btn btn-raise" onclick="fetch('/lift?dir=sendToTop').then(()=>location.reload())">SEND TO TOP</button>
                <button class="btn btn-lower" onclick="fetch('/lift?dir=sendToBottom').then(()=>location.reload())">SEND TO BOTTOM</button>
            </div>
        </div>

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
    <title>Settings - BedLiftBee</title>
    <style>
        body { font-family: -apple-system, sans-serif; background: #1a1a1a; color: #fff; margin: 0; padding: 20px; }
        .container { max-width: 400px; margin: 0 auto; }
        h1 { color: #f97316; }
        .card { background: #2a2a2a; border-radius: 12px; padding: 20px; margin-bottom: 15px; }
        label { display: block; color: #888; margin-bottom: 5px; margin-top: 15px; }
        input { width: 100%; padding: 12px; border: 1px solid #444; border-radius: 8px; background: #1a1a1a; color: #fff; box-sizing: border-box; }
        .btn { display: block; width: 100%; padding: 15px; border: none; border-radius: 8px; font-size: 16px; cursor: pointer; margin-top: 15px; box-sizing: border-box; }
        .btn-primary { background: #f97316; color: white; }
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
                <label>Lift Duration (milliseconds)</label>
                <input type="number" name="lift_duration" value=")rawhtml";
        html += String(config.liftDuration);
        html += R"rawhtml(">
                <label>Nudge Duration (milliseconds)</label>
                <input type="number" name="nudge_duration" value=")rawhtml";
        html += String(config.nudgeDuration);
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
        if (request->hasParam("lift_duration", true)) {
            config.liftDuration = request->getParam("lift_duration", true)->value().toInt();
            if (config.liftDuration < 500) config.liftDuration = DEFAULT_LIFT_DURATION;
        }
        if (request->hasParam("nudge_duration", true)) {
            config.nudgeDuration = request->getParam("nudge_duration", true)->value().toInt();
            if (config.nudgeDuration < 50) config.nudgeDuration = DEFAULT_NUDGE_DURATION;
        }

        saveConfig();
        request->redirect("/");
        delay(500);
        ESP.restart();
    });

    // Lift control endpoint
    webServer.on("/lift", HTTP_GET, [](AsyncWebServerRequest *request) {
        if (request->hasParam("dir")) {
            String dir = request->getParam("dir")->value();
            if (dir == "raise") {
                startLift("raise");
                request->send(200, "text/plain", "RAISING");
            } else if (dir == "lower") {
                startLift("lower");
                request->send(200, "text/plain", "LOWERING");
            } else if (dir == "stop") {
                stopLift();
                request->send(200, "text/plain", "STOPPED");
            } else if (dir == "sendToTop") {
                startLift("raise", true);
                request->send(200, "text/plain", "SENDING TO TOP");
            } else if (dir == "sendToBottom") {
                startLift("lower", true);
                request->send(200, "text/plain", "SENDING TO BOTTOM");
            } else {
                request->send(400, "text/plain", "Invalid direction");
            }
        } else {
            request->send(400, "text/plain", "Missing dir parameter");
        }
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
        doc["isLifting"] = state.isLifting;
        doc["direction"] = state.liftDirection;
        doc["relayUp"] = state.relayUp;
        doc["relayDown"] = state.relayDown;
        doc["liftDuration"] = config.liftDuration;
        doc["mqtt_connected"] = state.mqttConnected;
        doc["wifi_rssi"] = state.rssi;
        doc["uptime"] = state.uptime;
        doc["free_heap"] = ESP.getFreeHeap();
        doc["firmware"] = FIRMWARE_VERSION;
        doc["ip"] = WiFi.localIP().toString();
        // Safety sensors
        doc["distanceCm"] = state.distanceCm;
        doc["ultrasonicOk"] = state.ultrasonicOk;
        doc["atLimit"] = state.atLimit;
        doc["positionPercent"] = state.positionPercent;
        doc["lastStopReason"] = state.lastStopReason;
        doc["calibratedTopCm"] = state.calibratedTopCm;
        doc["calibratedBottomCm"] = state.calibratedBottomCm;

        String json;
        serializeJson(doc, json);
        request->send(200, "application/json", json);
    });

    // Calibration endpoint
    webServer.on("/calibrate", HTTP_GET, [](AsyncWebServerRequest *request) {
        if (request->hasParam("pos")) {
            String pos = request->getParam("pos")->value();
            if (pos == "top" || pos == "bottom") {
                calibratePosition(pos);
                request->send(200, "text/plain", "Calibrated " + pos);
            } else {
                request->send(400, "text/plain", "Invalid position (use top or bottom)");
            }
        } else {
            request->send(400, "text/plain", "Missing pos parameter");
        }
    });

    webServer.begin();
    Serial.println("[HTTP] Async web server started");
}

// ============== Configuration Storage ==============

void loadConfig() {
    preferences.begin("bedliftbee", true);

    preferences.getString("deviceName", config.deviceName, sizeof(config.deviceName));
    if (strlen(config.deviceName) == 0) {
        strcpy(config.deviceName, "BedLiftBee");
    }

    preferences.getString("mqttServer", config.mqttServer, sizeof(config.mqttServer));
    if (strlen(config.mqttServer) == 0) {
        strcpy(config.mqttServer, "192.168.0.95");
    }

    config.mqttPort = preferences.getInt("mqttPort", DEFAULT_MQTT_PORT);
    config.liftDuration = preferences.getULong("liftDuration", DEFAULT_LIFT_DURATION);
    config.nudgeDuration = preferences.getULong("nudgeDuration", DEFAULT_NUDGE_DURATION);

    preferences.getString("mqttUser", config.mqttUser, sizeof(config.mqttUser));
    preferences.getString("mqttPass", config.mqttPass, sizeof(config.mqttPass));

    preferences.getString("topicPrefix", config.topicPrefix, sizeof(config.topicPrefix));
    if (strlen(config.topicPrefix) == 0) {
        strcpy(config.topicPrefix, "homecontrol");
    }

    preferences.end();

    Serial.printf("[Config] Loaded: name=%s, mqtt=%s:%d, duration=%lu, nudge=%lu\n",
                  config.deviceName, config.mqttServer, config.mqttPort, config.liftDuration, config.nudgeDuration);
}

void saveConfig() {
    preferences.begin("bedliftbee", false);
    preferences.putString("deviceName", config.deviceName);
    preferences.putString("mqttServer", config.mqttServer);
    preferences.putInt("mqttPort", config.mqttPort);
    preferences.putULong("liftDuration", config.liftDuration);
    preferences.putULong("nudgeDuration", config.nudgeDuration);
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
