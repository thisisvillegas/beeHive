/**
 * SimBee Keyboard - Sim Racing Dashboard v1.9.0
 *
 * T-Display S3 as a wireless BLE keyboard with sim racing features.
 * WiFi + BLE + MQTT + MCP23017 I/O Expander + SimHub + PWM Fan.
 *
 * MCP23017 Port A (PA0-PA7):
 *   PA0: Toggle - 'T' + wind enable/disable
 *   PA1: Push - Love bomb (MQTT, placeholder)
 *   PA2-PA7: Toggle switches - Y, U, I, G, H, J
 *
 * MCP23017 Port B (PB0-PB7):
 *   PB0-PB2: BedLiftBee control (MQTT, placeholder)
 *   PB3-PB7: Push buttons - C, V, B, N, M
 *
 * Features:
 *   - SimHub UDP telemetry receiver (port 5005)
 *   - PWM fan control based on car speed
 *   - Wind toggle (PA0) to enable/disable fans
 *   - Display shows port status and wind state
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <Wire.h>
#include <espMqttClient.h>
#include <NimBLEDevice.h>
#include <BleKeyboard.h>
#include <TFT_eSPI.h>
// MAX7219 driven directly (no library needed)

// I2C for QWIIC port on T-Display S3
#define I2C_SDA 43
#define I2C_SCL 44

// MCP23017
#define MCP23017_ADDR 0x27  // All address jumpers open
#define MCP_IODIRA   0x00   // Port A direction
#define MCP_IODIRB   0x01   // Port B direction
#define MCP_GPPUA    0x0C   // Port A pull-ups
#define MCP_GPPUB    0x0D   // Port B pull-ups
#define MCP_GPIOA    0x12   // Port A data
#define MCP_GPIOB    0x13   // Port B data

// WiFi credentials
#define WIFI_SSID "Remus2.0"
#define WIFI_PASS "Z3k3Z3k3"

// MQTT
#define MQTT_SERVER "192.168.0.95"
#define MQTT_PORT 1883
#define DEVICE_ID "simbee-keyboard"
#define COMPANION_ID "c404f416a398"  // kayciBee1
#define BEDLIFT_ID "2ce238d4db1c"    // BedLiftBee

// SimHub UDP
#define SIMHUB_PORT 5005

// MAX7219 7-segment display (daisy-chained)
#define MAX7219_DIN 13
#define MAX7219_CLK 11
#define MAX7219_CS  12
#define NUM_MAX7219 3  // 0=speed, 1=lap times, 2=lap count + pin

// Fan PWM (4-pin PC fan)
#define FAN_PWM_PIN 2
#define FAN_PWM_CHANNEL 0
#define FAN_PWM_FREQ 25000    // 25kHz - standard for 4-pin PWM fans
#define FAN_PWM_RESOLUTION 8  // 0-255 duty cycle
#define FAN_MAX_SPEED_KMH 200 // Speed at which fan hits 100%

// ============== Hardware ==============

#define BUTTON_LEFT 0
#define BUTTON_RIGHT 14
#define TFT_BACKLIGHT 38
#define TFT_POWER 15

// Display
#define SCREEN_WIDTH 170
#define SCREEN_HEIGHT 320

// Colors
#define COLOR_BG      TFT_BLACK
#define COLOR_TEXT    TFT_WHITE
#define COLOR_ACCENT  0x07E0  // Green
#define COLOR_WARN    0xFD20  // Orange
#define COLOR_DIM     0x8410  // Gray

// ============== Globals ==============

TFT_eSPI tft = TFT_eSPI();
TFT_eSprite spr = TFT_eSprite(&tft);

espMqttClient mqtt;

BleKeyboard keyboard("SimBee", "Beehive", 100);

// MQTT topics
String topicState;
String topicSet;
String topicAvail;

// Track connection states
bool wifiConnected = false;
bool mqttConnected = false;
bool lastButtonLeft = HIGH;
bool lastButtonRight = HIGH;
bool wasConnected = false;
int debugPage = 0;  // 0 = main, 1 = connection status
String lastKey = "-";
unsigned long lastKeyTime = 0;

// MCP23017 state
bool mcpFound = false;
uint8_t lastPortA = 0xFF;  // All high (pull-ups)
uint8_t currentPortA = 0xFF;
uint8_t lastPortB = 0xFF;  // All high (pull-ups)
uint8_t currentPortB = 0xFF;

// Wind control
bool windEnabled = true;  // Toggle with PA0

// SimHub UDP
WiFiUDP simhubUDP;
char udpBuffer[256];
unsigned long lastSimHubPacket = 0;
String simhubData = "";

// Fan control
int fanSpeedPercent = 0;      // 0-100%
int carSpeedKmh = 0;          // From SimHub
bool fanTestMode = false;     // True during startup test

// Lap times (milliseconds, 0 = no data)
int sessionBestLapMs = 0;
int allTimeBestLapMs = 0;

// Lap count
int currentLap = 0;
int totalLaps = 0;

// Last triggered pin (for display)
char lastTriggerPort = ' ';  // 'A' or 'B'
int lastTriggerPin = -1;

// ============== Forward Declarations ==============

bool isActuallyConnected();
void connectMQTT();
void publishState();
void setFanSpeed(int percent);
void max7219ShowSpeed(int kmh);
void max7219ShowLaps();
void max7219ShowInfo();
void onMqttConnect(bool sessionPresent);
void onMqttDisconnect(espMqttClientTypes::DisconnectReason reason);
void onMqttMessage(const espMqttClientTypes::MessageProperties& props,
                   const char* topic, const uint8_t* payload, size_t len,
                   size_t index, size_t total);

// ============== MCP23017 ==============

void mcpWrite(uint8_t reg, uint8_t value) {
    Wire.beginTransmission(MCP23017_ADDR);
    Wire.write(reg);
    Wire.write(value);
    Wire.endTransmission();
}

uint8_t mcpRead(uint8_t reg) {
    Wire.beginTransmission(MCP23017_ADDR);
    Wire.write(reg);
    Wire.endTransmission();
    Wire.requestFrom(MCP23017_ADDR, (uint8_t)1);
    return Wire.read();
}

bool mcpInit() {
    Wire.beginTransmission(MCP23017_ADDR);
    if (Wire.endTransmission() != 0) {
        Serial.printf("[MCP] Not found at 0x%02X\n", MCP23017_ADDR);
        return false;
    }
    Serial.printf("[MCP] Found at 0x%02X\n", MCP23017_ADDR);

    // Set all Port A as inputs
    mcpWrite(MCP_IODIRA, 0xFF);
    // Enable pull-ups on Port A
    mcpWrite(MCP_GPPUA, 0xFF);

    // Set all Port B as inputs
    mcpWrite(MCP_IODIRB, 0xFF);
    // Enable pull-ups on Port B
    mcpWrite(MCP_GPPUB, 0xFF);

    return true;
}

// Switch mappings: PA pin -> key to send (0 = special handling)
// PA0=T + wind toggle, PA1=love bomb (later), PA2-4=Y,U,I, PA5-7=G,H,J
const uint8_t portAKey[] = {
    't',        // PA0 - Toggle + wind enable/disable
    0,          // PA1 - Love bomb (MQTT, handled separately)
    'y',        // PA2 - Toggle
    'u',        // PA3 - Toggle
    'i',        // PA4 - Toggle
    'g',        // PA5 - Toggle
    'h',        // PA6 - Toggle
    'j'         // PA7 - Toggle
};

const char* portALabel[] = {
    "T+Wind", "LoveBomb", "Y", "U", "I", "G", "H", "J"
};

// Port B mappings: PB0-2=BedLift (later), PB3-7=C,V,B,N,M
const uint8_t portBKey[] = {
    0,          // PB0 - BedLiftBee RAISE (MQTT, handled separately)
    0,          // PB1 - BedLiftBee LOWER (MQTT, handled separately)
    0,          // PB2 - BedLiftBee STOP (MQTT, handled separately)
    'c',        // PB3 - Push button
    'v',        // PB4 - Push button
    'b',        // PB5 - Push button
    'n',        // PB6 - Push button
    'm'         // PB7 - Push button
};

const char* portBLabel[] = {
    "BedUp", "BedDn", "BedStop", "C", "V", "B", "N", "M"
};

// Send a key press via BLE
void sendKeyPress(uint8_t key, const char* label) {
    NimBLEServer* pServer = NimBLEDevice::getServer();
    int connCount = pServer ? pServer->getConnectedCount() : 0;

    if (connCount > 0) {
        delay(20);
        keyboard.press(key);
        delay(100);
        keyboard.release(key);
        delay(20);

        lastKey = String(label);
        lastKeyTime = millis();
        Serial.printf("[BLE] Sent: %s\n", label);
    } else {
        lastKey = "NO BLE!";
        lastKeyTime = millis();
        Serial.println("[BLE] Not connected - key ignored");
    }
}

// Handle Port A pin change
void handlePortAPin(int pin, bool pressed) {
    // Update last triggered pin for display
    lastTriggerPort = 'A';
    lastTriggerPin = pin;
    max7219ShowInfo();

    // pressed = true when pin goes LOW (switch activated)

    if (pin == 0) {
        // PA0: Wind enable switch - ON=wind ON, OFF=wind OFF
        // pressed (switch ON/LOW) = enable wind
        // released (switch OFF/HIGH) = disable wind
        windEnabled = pressed;
        Serial.printf("[Wind] %s\n", windEnabled ? "ENABLED" : "DISABLED");
        if (!windEnabled) {
            setFanSpeed(0);  // Turn off fan immediately
        }
        // Send T on every toggle
        sendKeyPress('T', portALabel[0]);
        return;
    }

    if (pin == 1) {
        // PA1: Love bomb - send to companion bee
        if (pressed) {
            String topic = "homecontrol/devices/" COMPANION_ID "/set";
            String payload = "{\"capability\":{\"instance\":\"loveBomb\",\"value\":true}}";
            if (mqtt.connected()) {
                mqtt.publish(topic.c_str(), 0, false, payload.c_str());
                Serial.println("[PA1] Love bomb sent to kayciBee!");
                lastKey = "LoveBomb!";
            } else {
                Serial.println("[PA1] Love bomb failed - no MQTT");
                lastKey = "LB:NoMQTT";
            }
            lastKeyTime = millis();
        }
        return;
    }

    // PA2-PA7: Toggle switches
    uint8_t key = portAKey[pin];
    if (key != 0) {
        if (pin == 7) {
            // PA7 (J): Send Ctrl+J for Assetto
            NimBLEServer* pServer = NimBLEDevice::getServer();
            int connCount = pServer ? pServer->getConnectedCount() : 0;
            if (connCount > 0) {
                delay(20);
                keyboard.press(KEY_LEFT_CTRL);
                keyboard.press(key);
                delay(100);
                keyboard.releaseAll();
                delay(20);
                lastKey = "C+J";
                lastKeyTime = millis();
                Serial.println("[BLE] Sent: Ctrl+J");
            } else {
                lastKey = "NO BLE!";
                lastKeyTime = millis();
            }
        } else {
            sendKeyPress(key, portALabel[pin]);
        }
    }
}

// Send BedLiftBee command via MQTT
void sendBedLiftCommand(const char* command, const char* label) {
    String topic = "homecontrol/devices/" BEDLIFT_ID "/set";
    String payload = "{\"capability\":{\"instance\":\"bedLift\",\"value\":\"";
    payload += command;
    payload += "\"}}";

    if (mqtt.connected()) {
        mqtt.publish(topic.c_str(), 0, false, payload.c_str());
        Serial.printf("[BedLift] Sent: %s\n", command);
        lastKey = label;
    } else {
        Serial.printf("[BedLift] %s failed - no MQTT\n", command);
        lastKey = "Bed:NoMQ";
    }
    lastKeyTime = millis();
}

// Handle Port B pin change
void handlePortBPin(int pin, bool pressed) {
    // Update last triggered pin for display
    lastTriggerPort = 'B';
    lastTriggerPin = pin;
    max7219ShowInfo();

    // pressed = true when pin goes LOW (switch activated)

    if (pin == 0) {
        // PB0: BedLiftBee RAISE (toggle - send on flip ON, stop on flip OFF)
        if (pressed) {
            sendBedLiftCommand("raise", "BedUp");
        } else {
            sendBedLiftCommand("stop", "BedStop");
        }
        return;
    }

    if (pin == 1) {
        // PB1: BedLiftBee LOWER (toggle - send on flip ON, stop on flip OFF)
        if (pressed) {
            sendBedLiftCommand("lower", "BedDn");
        } else {
            sendBedLiftCommand("stop", "BedStop");
        }
        return;
    }

    if (pin == 2) {
        // PB2: BedLiftBee STOP (momentary - just send stop on press)
        if (pressed) {
            sendBedLiftCommand("stop", "BedStop");
        }
        return;
    }

    // PB3-PB7: Push buttons - send Ctrl + key (only on press)
    if (pressed) {
        uint8_t key = portBKey[pin];
        if (key != 0) {
            NimBLEServer* pServer = NimBLEDevice::getServer();
            int connCount = pServer ? pServer->getConnectedCount() : 0;

            if (connCount > 0) {
                delay(20);
                keyboard.press(KEY_LEFT_CTRL);
                keyboard.press(key);
                delay(100);
                keyboard.releaseAll();
                delay(20);

                lastKey = "C+" + String((char)key);
                lastKeyTime = millis();
                Serial.printf("[BLE] Sent: Ctrl+%c\n", key);
            } else {
                lastKey = "NO BLE!";
                lastKeyTime = millis();
                Serial.println("[BLE] Not connected - key ignored");
            }
        }
    }
}

void handleMCP() {
    if (!mcpFound) return;

    static unsigned long lastMcpRead = 0;
    if (millis() - lastMcpRead < 50) return;  // Debounce
    lastMcpRead = millis();

    // Read both ports
    uint8_t portA = mcpRead(MCP_GPIOA);
    uint8_t portB = mcpRead(MCP_GPIOB);
    currentPortA = portA;
    currentPortB = portB;

    // Debug: print port values periodically
    static unsigned long lastDebug = 0;
    if (millis() - lastDebug > 2000) {
        lastDebug = millis();
        Serial.printf("[MCP] Port A: 0x%02X  Port B: 0x%02X  Wind: %s\n",
                      portA, portB, windEnabled ? "ON" : "OFF");
    }

    // Check Port A pins for changes
    for (int pin = 0; pin <= 7; pin++) {
        uint8_t lastBit = (lastPortA >> pin) & 0x01;
        uint8_t currBit = (portA >> pin) & 0x01;

        if (currBit != lastBit) {
            bool pressed = (currBit == 0);  // LOW = pressed (pull-up)
            Serial.printf("[MCP] PA%d: %s\n", pin, pressed ? "PRESSED" : "RELEASED");
            handlePortAPin(pin, pressed);
        }
    }

    // Check Port B pins for changes
    for (int pin = 0; pin <= 7; pin++) {
        uint8_t lastBit = (lastPortB >> pin) & 0x01;
        uint8_t currBit = (portB >> pin) & 0x01;

        if (currBit != lastBit) {
            bool pressed = (currBit == 0);  // LOW = pressed (pull-up)
            Serial.printf("[MCP] PB%d: %s\n", pin, pressed ? "PRESSED" : "RELEASED");
            handlePortBPin(pin, pressed);
        }
    }

    lastPortA = portA;
    lastPortB = portB;
}

// ============== Fan Control ==============

void setFanSpeed(int percent) {
    // Clamp to 0-100
    percent = constrain(percent, 0, 100);
    fanSpeedPercent = percent;

    // Map 0-100% to 0-255 duty cycle
    int duty = map(percent, 0, 100, 0, 255);
    ledcWrite(FAN_PWM_PIN, duty);  // ESP32-S3 uses pin directly
}

void parseSimHubData(const char* data) {
    // Expected format: "speed=123,rpm=5000,gear=3"
    // or just "speed=123"

    String str(data);

    // Find speed value
    int speedIdx = str.indexOf("speed=");
    if (speedIdx >= 0) {
        int valueStart = speedIdx + 6;  // length of "speed="
        int valueEnd = str.indexOf(',', valueStart);
        if (valueEnd < 0) valueEnd = str.length();

        String speedStr = str.substring(valueStart, valueEnd);
        carSpeedKmh = speedStr.toInt();

        // Map car speed to fan speed
        int fanPercent = map(carSpeedKmh, 0, FAN_MAX_SPEED_KMH, 0, 100);
        fanPercent = constrain(fanPercent, 0, 100);

        if (!fanTestMode && windEnabled) {
            setFanSpeed(fanPercent);
        } else if (!windEnabled) {
            setFanSpeed(0);  // Keep fan off when wind disabled
        }

        Serial.printf("[Fan] Car: %d km/h -> Fan: %d%% (wind %s)\n",
                      carSpeedKmh, fanPercent, windEnabled ? "ON" : "OFF");

        // Update 7-segment display with speed
        max7219ShowSpeed(carSpeedKmh);
    }

    // Parse session best lap (milliseconds)
    int bestIdx = str.indexOf("bestlap=");
    if (bestIdx >= 0) {
        int valueStart = bestIdx + 8;
        int valueEnd = str.indexOf(',', valueStart);
        if (valueEnd < 0) valueEnd = str.length();
        sessionBestLapMs = str.substring(valueStart, valueEnd).toInt();
    }

    // Parse all-time best lap (milliseconds)
    int allIdx = str.indexOf("allbest=");
    if (allIdx >= 0) {
        int valueStart = allIdx + 8;
        int valueEnd = str.indexOf(',', valueStart);
        if (valueEnd < 0) valueEnd = str.length();
        allTimeBestLapMs = str.substring(valueStart, valueEnd).toInt();
    }

    // Update lap time display if either changed
    if (bestIdx >= 0 || allIdx >= 0) {
        max7219ShowLaps();
    }

    // Parse current lap number (check for ",lap=" or starts with "lap=")
    int lapIdx = str.indexOf(",lap=");
    if (lapIdx >= 0) lapIdx += 1;  // skip the comma
    else if (str.startsWith("lap=")) lapIdx = 0;
    if (lapIdx >= 0) {
        int valueStart = lapIdx + 4;
        int valueEnd = str.indexOf(',', valueStart);
        if (valueEnd < 0) valueEnd = str.length();
        currentLap = str.substring(valueStart, valueEnd).toInt();
    }

    // Parse total laps
    int totalIdx = str.indexOf("totallaps=");
    if (totalIdx >= 0) {
        int valueStart = totalIdx + 10;
        int valueEnd = str.indexOf(',', valueStart);
        if (valueEnd < 0) valueEnd = str.length();
        totalLaps = str.substring(valueStart, valueEnd).toInt();
    }

    // Update device 2 display
    if (lapIdx >= 0 || totalIdx >= 0) {
        max7219ShowInfo();
    }
}

void fanStartupTest() {
    Serial.println("[Fan] Running startup test...");
    fanTestMode = true;

    // Ramp up
    for (int i = 0; i <= 100; i += 10) {
        setFanSpeed(i);
        delay(100);
    }

    // Hold at max
    delay(500);

    // Ramp down
    for (int i = 100; i >= 0; i -= 10) {
        setFanSpeed(i);
        delay(100);
    }

    fanTestMode = false;
    Serial.println("[Fan] Startup test complete");
}

// ============== MAX7219 7-Segment Display (Daisy-Chain) ==============

// Shift out 16 bits (no CS control)
void max7219ShiftOut(uint8_t reg, uint8_t data) {
    for (int i = 7; i >= 0; i--) {
        digitalWrite(MAX7219_CLK, LOW);
        digitalWrite(MAX7219_DIN, (reg >> i) & 1);
        digitalWrite(MAX7219_CLK, HIGH);
    }
    for (int i = 7; i >= 0; i--) {
        digitalWrite(MAX7219_CLK, LOW);
        digitalWrite(MAX7219_DIN, (data >> i) & 1);
        digitalWrite(MAX7219_CLK, HIGH);
    }
}

// Send to a specific device (others get NOP)
void max7219Send(int device, uint8_t reg, uint8_t data) {
    digitalWrite(MAX7219_CS, LOW);
    for (int d = NUM_MAX7219 - 1; d >= 0; d--) {
        if (d == device) max7219ShiftOut(reg, data);
        else max7219ShiftOut(0x00, 0x00);
    }
    digitalWrite(MAX7219_CS, HIGH);
}

// Send same command to all devices
void max7219SendAll(uint8_t reg, uint8_t data) {
    digitalWrite(MAX7219_CS, LOW);
    for (int d = 0; d < NUM_MAX7219; d++) {
        max7219ShiftOut(reg, data);
    }
    digitalWrite(MAX7219_CS, HIGH);
}

void max7219Init() {
    pinMode(MAX7219_DIN, OUTPUT);
    pinMode(MAX7219_CLK, OUTPUT);
    pinMode(MAX7219_CS, OUTPUT);
    digitalWrite(MAX7219_CS, HIGH);

    max7219SendAll(0x0F, 0x00);  // Display test off
    max7219SendAll(0x0C, 0x01);  // Normal operation
    max7219SendAll(0x0B, 0x07);  // Scan limit: all 8 digits
    max7219SendAll(0x09, 0xFF);  // BCD decode for all digits
    max7219SendAll(0x0A, 0x08);  // Intensity: 8/15
    // Clear all digits on all devices
    for (int i = 1; i <= 8; i++) max7219SendAll(i, 0x0F);
}

// Show a number right-aligned within a digit range on a specific device
void max7219ShowDigits(int device, int value, int startDigit, int numDigits) {
    for (int i = 0; i < numDigits; i++) {
        int pos = startDigit + i;
        if (value > 0 || i == 0) {
            max7219Send(device, pos, value % 10);
            value /= 10;
        } else {
            max7219Send(device, pos, 0x0F);  // Blank
        }
    }
}

// Device 0: MPH (left 4) | KMH (right 4)
void max7219ShowSpeed(int kmh) {
    int mph = (int)(kmh * 0.621371);
    max7219ShowDigits(0, kmh, 1, 4);
    max7219ShowDigits(0, mph, 5, 4);
}

// Show a lap time in M.SS.t format on 4 digits of a device
// startDigit = rightmost digit, timeMs = lap time in milliseconds
void max7219ShowLapTime(int device, int startDigit, int timeMs) {
    if (timeMs <= 0) {
        // No data: show -.--.-
        max7219Send(device, startDigit + 3, 0x0A | 0x80);  // - with dp
        max7219Send(device, startDigit + 2, 0x0A);          // -
        max7219Send(device, startDigit + 1, 0x0A | 0x80);  // - with dp
        max7219Send(device, startDigit,     0x0A);          // -
        return;
    }
    int totalSeconds = timeMs / 1000;
    int minutes = totalSeconds / 60;
    int seconds = totalSeconds % 60;
    int tenths = (timeMs % 1000) / 100;

    max7219Send(device, startDigit + 3, (minutes % 10) | 0x80);  // M. (dp = colon)
    max7219Send(device, startDigit + 2, seconds / 10);             // S
    max7219Send(device, startDigit + 1, (seconds % 10) | 0x80);  // S. (dp = decimal)
    max7219Send(device, startDigit,     tenths);                   // t
}

// Device 1: All-time best (left 4) | Session best (right 4)
void max7219ShowLaps() {
    max7219ShowLapTime(1, 1, sessionBestLapMs);   // Right 4: session best
    max7219ShowLapTime(1, 5, allTimeBestLapMs);   // Left 4: all-time best
}

// Raw segment patterns for device 2 (no BCD decode)
// Bit layout: DP-A-B-C-D-E-F-G (bit 7 to bit 0)
const uint8_t SEG_DIGIT[] = {
    0x7E, 0x30, 0x6D, 0x79, 0x33,  // 0-4
    0x5B, 0x5F, 0x70, 0x7F, 0x7B   // 5-9
};
const uint8_t SEG_A     = 0x77;  // Letter A
const uint8_t SEG_b     = 0x1F;  // Letter b
const uint8_t SEG_DASH  = 0x01;  // Middle segment only
const uint8_t SEG_BLANK = 0x00;

// Device 2: Lap count (left 4) | Last pin triggered (right 4)
void max7219ShowInfo() {
    // Use raw segment mode for device 2
    max7219Send(2, 0x09, 0x00);

    // Left 4 (digits 8-5): current lap / total laps
    if (currentLap > 0) {
        // Format: CC.TT (current with dp separator, then total)
        uint8_t dp = 0x80;  // decimal point flag
        max7219Send(2, 8, (currentLap >= 10) ? SEG_DIGIT[currentLap / 10] : SEG_BLANK);
        max7219Send(2, 7, SEG_DIGIT[currentLap % 10] | dp);  // dp as separator
        max7219Send(2, 6, (totalLaps >= 10) ? SEG_DIGIT[totalLaps / 10] : SEG_BLANK);
        max7219Send(2, 5, SEG_DIGIT[totalLaps % 10]);
    } else {
        max7219Send(2, 8, SEG_DASH);
        max7219Send(2, 7, SEG_DASH | 0x80);
        max7219Send(2, 6, SEG_DASH);
        max7219Send(2, 5, SEG_DASH);
    }

    // Right 4 (digits 4-1): last triggered pin (e.g. "A  3" or "b  7")
    if (lastTriggerPin >= 0) {
        max7219Send(2, 4, (lastTriggerPort == 'A') ? SEG_A : SEG_b);
        max7219Send(2, 3, SEG_BLANK);
        max7219Send(2, 2, SEG_BLANK);
        max7219Send(2, 1, SEG_DIGIT[lastTriggerPin % 10]);
    } else {
        for (int i = 1; i <= 4; i++) max7219Send(2, i, SEG_DASH);
    }
}

// ============== Display ==============

void drawScreen() {
    spr.fillSprite(COLOR_BG);

    if (debugPage == 0) {
        // ========== PAGE 1: Main Debug ==========

        // Title (top half - invisible on broken screen, but keeps layout)
        spr.setTextColor(COLOR_TEXT);
        spr.setTextDatum(TC_DATUM);
        spr.drawString("SimBee Keyboard", SCREEN_WIDTH/2, 20, 4);

        // Last key sent
        int keyY = 170;
        spr.setTextDatum(TL_DATUM);
        spr.setTextColor(COLOR_DIM);
        spr.drawString("Last key:", 20, keyY, 2);
        spr.setTextColor(COLOR_TEXT);
        spr.drawString(lastKey, 100, keyY, 2);

        // Time since last key
        if (lastKeyTime > 0) {
            unsigned long ago = (millis() - lastKeyTime) / 1000;
            String agoStr;
            if (ago < 60) {
                agoStr = String(ago) + "s ago";
            } else {
                agoStr = String(ago / 60) + "m ago";
            }
            spr.setTextColor(COLOR_DIM);
            spr.drawString(agoStr, 20, keyY + 20, 1);
        }

        // Fan/Wind display
        int fanY = 210;
        spr.setTextDatum(TL_DATUM);
        spr.setTextColor(COLOR_DIM);
        spr.drawString("Wind:", 20, fanY, 2);

        // Wind status indicator
        spr.setTextColor(windEnabled ? COLOR_ACCENT : COLOR_WARN);
        spr.drawString(windEnabled ? "ON" : "OFF", 70, fanY, 2);

        // Fan speed bar background
        spr.fillRect(100, fanY, 50, 14, COLOR_DIM);
        int barWidth = map(fanSpeedPercent, 0, 100, 0, 50);
        if (barWidth > 0) {
            spr.fillRect(100, fanY, barWidth, 14, windEnabled ? COLOR_ACCENT : COLOR_WARN);
        }
        spr.setTextColor(COLOR_TEXT);
        spr.drawString(String(fanSpeedPercent) + "%", 155, fanY, 1);

        // Port A indicators (PA0-PA7)
        int portAY = 245;
        spr.setTextDatum(TL_DATUM);
        spr.setTextColor(COLOR_DIM);
        spr.drawString("A:", 5, portAY, 1);
        for (int i = 0; i < 8; i++) {
            uint8_t bit = (currentPortA >> i) & 0x01;
            uint16_t col = (bit == 0) ? COLOR_ACCENT : COLOR_DIM;
            spr.fillCircle(25 + i * 18, portAY + 5, 5, col);
        }

        // Port B indicators (PB0-PB7)
        int portBY = 265;
        spr.setTextColor(COLOR_DIM);
        spr.drawString("B:", 5, portBY, 1);
        for (int i = 0; i < 8; i++) {
            uint8_t bit = (currentPortB >> i) & 0x01;
            uint16_t col = (bit == 0) ? COLOR_ACCENT : COLOR_DIM;
            spr.fillCircle(25 + i * 18, portBY + 5, 5, col);
        }

        // Page indicator
        spr.setTextDatum(TC_DATUM);
        spr.setTextColor(COLOR_DIM);
        spr.drawString("[1/2]  BTN1 = next", SCREEN_WIDTH/2, SCREEN_HEIGHT - 25, 1);

    } else {
        // ========== PAGE 2: Connection Status ==========

        // Title in bottom half
        spr.setTextColor(COLOR_TEXT);
        spr.setTextDatum(TC_DATUM);
        spr.drawString("Connections", SCREEN_WIDTH/2, 170, 4);

        spr.setTextDatum(TL_DATUM);

        // WiFi
        int wifiY = 205;
        spr.setTextColor(COLOR_DIM);
        spr.drawString("WiFi:", 20, wifiY, 2);
        wifiConnected = WiFi.status() == WL_CONNECTED;
        if (wifiConnected) {
            spr.setTextColor(COLOR_ACCENT);
            spr.drawString(WiFi.localIP().toString(), 70, wifiY, 2);
        } else {
            spr.setTextColor(COLOR_WARN);
            spr.drawString("Disconnected", 70, wifiY, 2);
        }
        spr.fillCircle(SCREEN_WIDTH - 20, wifiY + 6, 6, wifiConnected ? COLOR_ACCENT : COLOR_WARN);

        // MQTT
        int mqttY = 228;
        spr.setTextColor(COLOR_DIM);
        spr.drawString("MQTT:", 20, mqttY, 2);
        mqttConnected = mqtt.connected();
        spr.setTextColor(mqttConnected ? COLOR_ACCENT : COLOR_WARN);
        spr.drawString(mqttConnected ? "Connected" : "Waiting...", 70, mqttY, 2);
        spr.fillCircle(SCREEN_WIDTH - 20, mqttY + 6, 6, mqttConnected ? COLOR_ACCENT : COLOR_WARN);

        // BLE
        bool connected = isActuallyConnected();
        int bleY = 251;
        spr.setTextColor(COLOR_DIM);
        spr.drawString("BLE:", 20, bleY, 2);
        spr.setTextColor(connected ? COLOR_ACCENT : COLOR_WARN);
        spr.drawString(connected ? "Connected" : "Waiting...", 70, bleY, 2);
        spr.fillCircle(SCREEN_WIDTH - 20, bleY + 6, 6, connected ? COLOR_ACCENT : COLOR_WARN);

        // MCP23017
        int mcpY = 274;
        spr.setTextColor(COLOR_DIM);
        spr.drawString("MCP:", 20, mcpY, 2);
        spr.setTextColor(mcpFound ? COLOR_ACCENT : COLOR_WARN);
        spr.drawString(mcpFound ? "OK" : "Not found", 70, mcpY, 2);
        spr.fillCircle(SCREEN_WIDTH - 20, mcpY + 6, 6, mcpFound ? COLOR_ACCENT : COLOR_WARN);

        // Page indicator
        spr.setTextDatum(TC_DATUM);
        spr.setTextColor(COLOR_DIM);
        spr.drawString("[2/2]  BTN1 = next", SCREEN_WIDTH/2, SCREEN_HEIGHT - 25, 1);
    }

    // Footer (both pages)
    spr.setTextDatum(TC_DATUM);
    spr.setTextColor(COLOR_DIM);
    spr.drawString("v1.9.0", SCREEN_WIDTH/2, SCREEN_HEIGHT - 10, 1);

    spr.pushSprite(0, 0);
}

// ============== Button Handling ==============

void sendKey(char key) {
    if (isActuallyConnected()) {
        keyboard.write(key);
        lastKey = String(key);
        lastKeyTime = millis();
        Serial.printf("[KEY] Sent: %c\n", key);
    } else {
        Serial.println("[KEY] Not connected - ignoring keypress");
    }
}

void handleButtons() {
    bool buttonLeft = digitalRead(BUTTON_LEFT);
    bool buttonRight = digitalRead(BUTTON_RIGHT);
    
    // Left button - toggle debug page
    if (buttonLeft == LOW && lastButtonLeft == HIGH) {
        debugPage = (debugPage + 1) % 2;
        drawScreen();
    }
    
    // Right button - send 'b'
    if (buttonRight == LOW && lastButtonRight == HIGH) {
        sendKey('b');
    }
    
    lastButtonLeft = buttonLeft;
    lastButtonRight = buttonRight;
}

// ============== Setup ==============

void setup() {
    Serial.begin(115200);
    delay(500);

    Serial.println("\n=== SimBee Keyboard v1.9.0 ===");

    // I2C for MCP23017 (QWIIC port)
    Wire.begin(I2C_SDA, I2C_SCL);
    Wire.setClock(100000);  // 100kHz
    Serial.printf("[I2C] Initialized on SDA=%d, SCL=%d\n", I2C_SDA, I2C_SCL);

    // Buttons
    pinMode(BUTTON_LEFT, INPUT_PULLUP);
    pinMode(BUTTON_RIGHT, INPUT_PULLUP);
    
    // Display power
    pinMode(TFT_POWER, OUTPUT);
    digitalWrite(TFT_POWER, HIGH);
    
    // Display init
    tft.init();
    tft.setRotation(0);  // Portrait
    tft.fillScreen(COLOR_BG);
    tft.invertDisplay(true);
    
    // Backlight
    pinMode(TFT_BACKLIGHT, OUTPUT);
    digitalWrite(TFT_BACKLIGHT, HIGH);
    
    // Sprite for flicker-free drawing
    spr.createSprite(SCREEN_WIDTH, SCREEN_HEIGHT);
    spr.setSwapBytes(true);
    
    // Connect to WiFi first
    tft.setTextColor(COLOR_TEXT);
    tft.setTextDatum(MC_DATUM);
    tft.drawString("Connecting WiFi...", SCREEN_WIDTH/2, SCREEN_HEIGHT/2 - 20, 2);
    
    Serial.println("[WiFi] Connecting...");
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    
    // Wait up to 10 seconds for WiFi
    int wifiAttempts = 0;
    while (WiFi.status() != WL_CONNECTED && wifiAttempts < 20) {
        delay(500);
        Serial.print(".");
        wifiAttempts++;
    }
    
    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("\n[WiFi] Connected! IP: %s\n", WiFi.localIP().toString().c_str());
        tft.drawString(WiFi.localIP().toString(), SCREEN_WIDTH/2, SCREEN_HEIGHT/2, 2);
    } else {
        Serial.println("\n[WiFi] Failed to connect");
        tft.drawString("WiFi failed", SCREEN_WIDTH/2, SCREEN_HEIGHT/2, 2);
    }
    
    delay(500);
    
    // Now start BLE
    tft.fillScreen(COLOR_BG);
    tft.drawString("Starting BLE...", SCREEN_WIDTH/2, SCREEN_HEIGHT/2, 2);
    
    // Start BLE keyboard (initializes NimBLE)
    Serial.println("[BLE] Initializing keyboard...");
    keyboard.begin();
    
    // Now we can safely use NimBLE functions
    delay(500);
    
    // Clear any old BLE bonds/pairings
    Serial.println("[BLE] Clearing old bonds...");
    NimBLEDevice::deleteAllBonds();
    
    // Give BLE stack time to stabilize
    delay(1000);
    
    // Force initial state to disconnected
    wasConnected = false;
    Serial.println("[BLE] Device name: SimBee");
    Serial.println("[BLE] Ready for pairing!");
    
    // Setup MQTT (async - non-blocking)
    topicState = "homecontrol/devices/" + String(DEVICE_ID) + "/state";
    topicSet = "homecontrol/devices/" + String(DEVICE_ID) + "/set";
    topicAvail = "homecontrol/devices/" + String(DEVICE_ID) + "/availability";

    mqtt.setServer(MQTT_SERVER, MQTT_PORT);
    mqtt.setClientId(DEVICE_ID);
    mqtt.onConnect(onMqttConnect);
    mqtt.onDisconnect(onMqttDisconnect);
    mqtt.onMessage(onMqttMessage);
    connectMQTT();

    // Initialize MCP23017
    mcpFound = mcpInit();
    if (mcpFound) {
        // Read initial state of both ports
        lastPortA = mcpRead(MCP_GPIOA);
        currentPortA = lastPortA;
        lastPortB = mcpRead(MCP_GPIOB);
        currentPortB = lastPortB;
        Serial.printf("[MCP] Initial state - Port A: 0x%02X  Port B: 0x%02X\n",
                      currentPortA, currentPortB);
    }

    // Initialize SimHub UDP listener
    simhubUDP.begin(SIMHUB_PORT);
    Serial.printf("[SimHub] UDP listening on port %d\n", SIMHUB_PORT);
    Serial.printf("[SimHub] Send data to: %s:%d\n", WiFi.localIP().toString().c_str(), SIMHUB_PORT);

    // Initialize MAX7219 7-segment displays (3 daisy-chained)
    max7219Init();
    max7219ShowSpeed(0);   // Device 0: show 0 until telemetry
    max7219ShowLaps();     // Device 1: show dashes until lap data
    max7219ShowInfo();     // Device 2: show dashes until pin/lap data
    Serial.println("[MAX7219] 3 displays initialized");

    // Initialize fan PWM (ESP32-S3 uses new LEDC API)
    ledcAttach(FAN_PWM_PIN, FAN_PWM_FREQ, FAN_PWM_RESOLUTION);
    ledcWrite(FAN_PWM_PIN, 0);  // Start with fan off
    Serial.printf("[Fan] PWM initialized on GPIO%d @ %dHz\n", FAN_PWM_PIN, FAN_PWM_FREQ);

    // Run fan startup test (ramp up and down)
    fanStartupTest();

    drawScreen();
}

// ============== Loop ==============

// Check actual BLE connection using NimBLE directly
bool isActuallyConnected() {
    if (!NimBLEDevice::getInitialized()) {
        return false;
    }
    NimBLEServer* pServer = NimBLEDevice::getServer();
    if (pServer) {
        return pServer->getConnectedCount() > 0;
    }
    return false;
}

// ============== MQTT (async) ==============

void onMqttConnect(bool sessionPresent) {
    Serial.println("[MQTT] Connected!");
    mqtt.subscribe(topicSet.c_str(), 1);
    mqtt.publish(topicAvail.c_str(), 1, true, "online");
    publishState();
}

void onMqttDisconnect(espMqttClientTypes::DisconnectReason reason) {
    Serial.printf("[MQTT] Disconnected, reason: %d\n", (int)reason);
}

void onMqttMessage(const espMqttClientTypes::MessageProperties& props, 
                   const char* topic, const uint8_t* payload, size_t len, 
                   size_t index, size_t total) {
    String msg;
    for (size_t i = 0; i < len; i++) msg += (char)payload[i];
    Serial.printf("[MQTT] %s: %s\n", topic, msg.c_str());
    // Handle commands here
}

void connectMQTT() {
    if (!WiFi.isConnected() || mqtt.connected()) return;
    Serial.println("[MQTT] Connecting...");
    mqtt.connect();
}

void publishState() {
    if (!mqtt.connected()) return;
    String json = "{\"ble\":" + String(isActuallyConnected() ? "true" : "false") +
                  ",\"rssi\":" + String(WiFi.RSSI()) +
                  ",\"uptime\":" + String(millis() / 1000) + "}";
    mqtt.publish(topicState.c_str(), 0, false, json.c_str());
}

void loop() {
    handleButtons();
    handleMCP();

    // SimHub UDP - non-blocking read
    int packetSize = simhubUDP.parsePacket();
    if (packetSize > 0) {
        int len = simhubUDP.read(udpBuffer, sizeof(udpBuffer) - 1);
        if (len > 0) {
            udpBuffer[len] = '\0';  // Null terminate
            simhubData = String(udpBuffer);
            lastSimHubPacket = millis();
            Serial.printf("[SimHub] Received: %s\n", udpBuffer);

            // Parse and update fan speed
            parseSimHubData(udpBuffer);
        }
    }

    // MQTT reconnect (non-blocking)
    static unsigned long lastMqttRetry = 0;
    if (!mqtt.connected() && millis() - lastMqttRetry > 5000) {
        lastMqttRetry = millis();
        connectMQTT();
    }
    
    // Publish state every 30s
    static unsigned long lastPublish = 0;
    if (mqtt.connected() && millis() - lastPublish > 30000) {
        lastPublish = millis();
        publishState();
    }
    
    // Update display periodically or on connection change
    static unsigned long lastDraw = 0;
    bool connected = isActuallyConnected();
    
    if (connected != wasConnected || millis() - lastDraw > 1000) {
        if (connected != wasConnected) {
            Serial.printf("[BLE] State changed: %s\n", connected ? "CONNECTED" : "DISCONNECTED");
            publishState();
        }
        wasConnected = connected;
        lastDraw = millis();
        drawScreen();
    }
    
    delay(10);
}
