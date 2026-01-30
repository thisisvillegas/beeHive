/**
 * SimBee Keyboard - Sim Racing Dashboard v2.0.0
 *
 * T-Display S3 as a wireless BLE keyboard with sim racing features.
 * WiFi + BLE + MQTT + MCP23017 I/O Expander + SimHub + PWM Fan.
 *
 * MCP23017 Port A (PA0-PA7):
 *   PA0: Toggle - MASTER SWITCH (standby/active mode)
 *   PA1: Push - Love bomb (MQTT, placeholder)
 *   PA2: Toggle - 'Y'
 *   PA3: Toggle - 'U'
 *   PA4: Toggle - Wind enable/disable
 *   PA5-PA7: Toggle switches - G, H, J
 *
 * MCP23017 Port B (PB0-PB7):
 *   PB0-PB2: BedLiftBee control (MQTT, placeholder)
 *   PB3-PB7: Push buttons - C, V, B, N, M
 *
 * Features:
 *   - SimHub UDP telemetry receiver (port 5005)
 *   - PWM fan control based on car speed
 *   - Master switch (PA0) for standby/active mode
 *   - Wind toggle (PA4) to enable/disable fans
 *   - Display shows port status and wind state
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiManager.h>
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

// WiFiManager (no hardcoded credentials - uses captive portal)
WiFiManager wifiManager;

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
// Landscape mode (90° CCW rotation)
#define SCREEN_WIDTH 320
#define SCREEN_HEIGHT 170

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
bool wasConnected = false;
String lastKey = "-";
unsigned long lastKeyTime = 0;

// MCP23017 state
bool mcpFound = false;
uint8_t lastPortA = 0xFF;  // All high (pull-ups)
uint8_t currentPortA = 0xFF;
uint8_t lastPortB = 0xFF;  // All high (pull-ups)
uint8_t currentPortB = 0xFF;

// Master switch (PA0) - controls MAX7219 and TFT mode
bool masterEnabled = true;  // ON = active, OFF = standby

// Wind control (PA4)
bool windEnabled = true;  // Toggle with PA4

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
int lastLapMs = 0;

// Lap count
int currentLap = 0;
int totalLaps = 0;

// Position
int position = 0;
int opponents = 0;

// Tyre data
int tyreWearFL = 0, tyreWearFR = 0, tyreWearRL = 0, tyreWearRR = 0;
int tyreTempFL = 0, tyreTempFR = 0, tyreTempRL = 0, tyreTempRR = 0;

// Last triggered pin (for display)
char lastTriggerPort = ' ';  // 'A' or 'B'
int lastTriggerPin = -1;

// MAX7219 view cycling (left button)
int currentView = 0;
#define NUM_VIEWS 4
const char* viewNames[] = {"Speed", "Tire Wear", "Tire Temp", "Lap Times"};

// TFT view cycling (right button)
int tftView = 0;  // 0 = Debug, 1 = Racing
#define NUM_TFT_VIEWS 2
const char* tftViewNames[] = {"Debug", "Racing"};

// ============== Forward Declarations ==============

bool isActuallyConnected();
void connectMQTT();
void publishState();
void setFanSpeed(int percent);
void updateMaxDisplays();
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
// PA0=master, PA1=love bomb, PA2-3=Y,U, PA4=wind, PA5-7=G,H,J
const uint8_t portAKey[] = {
    0,          // PA0 - MASTER SWITCH (special handling)
    0,          // PA1 - Love bomb (MQTT, handled separately)
    'y',        // PA2 - Toggle
    'u',        // PA3 - Toggle
    0,          // PA4 - WIND TOGGLE (special handling)
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
    updateMaxDisplays();

    // pressed = true when pin goes LOW (switch activated)

    if (pin == 0) {
        // PA0: MASTER SWITCH - controls MAX7219 displays and TFT mode
        // pressed (switch ON/LOW) = active mode
        // released (switch OFF/HIGH) = standby mode
        masterEnabled = pressed;
        Serial.printf("[Master] %s\n", masterEnabled ? "ACTIVE" : "STANDBY");

        if (masterEnabled) {
            // Active mode: enable MAX7219, switch to Racing view
            tftView = 1;  // Racing view
            Serial.println("[TFT] Switched to Racing (master ON)");
        } else {
            // Standby mode: blank MAX7219, TFT shows standby
            // MAX7219 will be blanked in updateMaxDisplays()
            // TFT standby view will be drawn in drawStandbyView()
            setFanSpeed(0);  // Turn off fan in standby
            Serial.println("[TFT] Switched to Standby (master OFF)");
        }
        updateMaxDisplays();
        return;
    }

    if (pin == 4) {
        // PA4: Wind enable switch - ON=wind ON, OFF=wind OFF
        // pressed (switch ON/LOW) = enable wind
        // released (switch OFF/HIGH) = disable wind
        windEnabled = pressed;
        Serial.printf("[Wind] %s\n", windEnabled ? "ENABLED" : "DISABLED");
        if (!windEnabled) {
            setFanSpeed(0);  // Turn off fan immediately
        }
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
    updateMaxDisplays();

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

// Helper: extract int value for a key from comma-separated UDP string
int udpParseInt(const String& str, const char* key) {
    int idx = str.indexOf(key);
    if (idx < 0) return -1;  // not found
    int valueStart = idx + strlen(key);
    int valueEnd = str.indexOf(',', valueStart);
    if (valueEnd < 0) valueEnd = str.length();
    return str.substring(valueStart, valueEnd).toInt();
}

void parseSimHubData(const char* data) {
    String str(data);

    // Speed
    int val = udpParseInt(str, "speed=");
    if (val >= 0) {
        carSpeedKmh = val;
        int fanPercent = map(carSpeedKmh, 0, FAN_MAX_SPEED_KMH, 0, 100);
        fanPercent = constrain(fanPercent, 0, 100);
        if (!fanTestMode && windEnabled) {
            setFanSpeed(fanPercent);
        } else if (!windEnabled) {
            setFanSpeed(0);
        }
    }

    // Lap times (milliseconds)
    val = udpParseInt(str, "bestlap=");
    if (val >= 0) sessionBestLapMs = val;
    val = udpParseInt(str, "allbest=");
    if (val >= 0) allTimeBestLapMs = val;
    val = udpParseInt(str, "lastlap=");
    if (val >= 0) lastLapMs = val;

    // Lap count (careful: ",lap=" to avoid matching "bestlap=" or "lastlap=")
    int lapIdx = str.indexOf(",lap=");
    if (lapIdx >= 0) {
        currentLap = str.substring(lapIdx + 5, str.indexOf(',', lapIdx + 5)).toInt();
    } else if (str.startsWith("lap=")) {
        currentLap = str.substring(4, str.indexOf(',')).toInt();
    }
    val = udpParseInt(str, "totallaps=");
    if (val >= 0) totalLaps = val;

    // Position
    val = udpParseInt(str, "pos=");
    if (val >= 0) position = val;
    val = udpParseInt(str, "opponents=");
    if (val >= 0) opponents = val;

    // Tyre wear (percentage remaining)
    val = udpParseInt(str, "twfl=");
    if (val >= 0) tyreWearFL = val;
    val = udpParseInt(str, "twfr=");
    if (val >= 0) tyreWearFR = val;
    val = udpParseInt(str, "twrl=");
    if (val >= 0) tyreWearRL = val;
    val = udpParseInt(str, "twrr=");
    if (val >= 0) tyreWearRR = val;

    // Tyre temperature (Celsius)
    val = udpParseInt(str, "ttfl=");
    if (val >= 0) tyreTempFL = val;
    val = udpParseInt(str, "ttfr=");
    if (val >= 0) tyreTempFR = val;
    val = udpParseInt(str, "ttrl=");
    if (val >= 0) tyreTempRL = val;
    val = udpParseInt(str, "ttrr=");
    if (val >= 0) tyreTempRR = val;

    // Update MAX7219 displays
    updateMaxDisplays();
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

// Blank all MAX7219 displays (standby mode)
void max7219Blank() {
    // Put all displays in shutdown mode (all segments off)
    max7219SendAll(0x0C, 0x00);
}

// Wake up MAX7219 displays (active mode)
void max7219Wake() {
    // Put all displays back in normal operation
    max7219SendAll(0x0C, 0x01);
}

// Show a number right-aligned within a digit range (BCD mode)
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

// Raw segment patterns (for non-BCD mode)
// Bit layout: DP-A-B-C-D-E-F-G (bit 7 to bit 0)
const uint8_t SEG_DIGIT[] = {
    0x7E, 0x30, 0x6D, 0x79, 0x33,  // 0-4
    0x5B, 0x5F, 0x70, 0x7F, 0x7B   // 5-9
};
const uint8_t SEG_A     = 0x77;  // Letter A
const uint8_t SEG_b     = 0x1F;  // Letter b
const uint8_t SEG_t     = 0x0F;  // Letter t (d,e,f,g)
const uint8_t SEG_I     = 0x30;  // Letter I (b,c)
const uint8_t SEG_r     = 0x05;  // Letter r (e,g)
const uint8_t SEG_E     = 0x4F;  // Letter E (a,d,e,f,g)
const uint8_t SEG_n     = 0x15;  // Letter n (c,e,g)
const uint8_t SEG_P     = 0x67;  // Letter P (a,b,e,f,g)
const uint8_t SEG_DASH  = 0x01;  // Middle segment only
const uint8_t SEG_BLANK = 0x00;

// Show full 8-digit lap time: _M.SS.mmm (BCD mode, digits 8-1)
void max7219ShowFullLapTime(int device, int timeMs) {
    max7219Send(device, 0x09, 0xFF);  // BCD decode all digits
    if (timeMs <= 0) {
        max7219Send(device, 8, 0x0F);         // blank
        max7219Send(device, 7, 0x0F);         // blank
        max7219Send(device, 6, 0x0A | 0x80);  // dash with DP
        max7219Send(device, 5, 0x0A);         // dash
        max7219Send(device, 4, 0x0A | 0x80);  // dash with DP
        max7219Send(device, 3, 0x0A);         // dash
        max7219Send(device, 2, 0x0A);         // dash
        max7219Send(device, 1, 0x0A);         // dash
        return;
    }
    int minutes = timeMs / 60000;
    int seconds = (timeMs % 60000) / 1000;
    int ms = timeMs % 1000;

    max7219Send(device, 8, 0x0F);  // blank (or tens of minutes if >= 10)
    max7219Send(device, 7, (minutes >= 10) ? (minutes / 10) : 0x0F);
    max7219Send(device, 6, (minutes % 10) | 0x80);  // M with DP = colon
    max7219Send(device, 5, seconds / 10);
    max7219Send(device, 4, (seconds % 10) | 0x80);  // S with DP = decimal
    max7219Send(device, 3, ms / 100);
    max7219Send(device, 2, (ms / 10) % 10);
    max7219Send(device, 1, ms % 10);
}

// ---- View 0: Speed + Position + Pin ----
void showView0() {
    // Device 0: MPH (left 4) | KMH (right 4) — BCD
    max7219Send(0, 0x09, 0xFF);
    int mph = (int)(carSpeedKmh * 0.621371);
    max7219ShowDigits(0, carSpeedKmh, 1, 4);
    max7219ShowDigits(0, mph, 5, 4);

    // Device 1: Lap N/Total (left 4) | Position/Total (right 4) — BCD
    max7219Send(1, 0x09, 0xFF);
    if (currentLap > 0) {
        max7219Send(1, 8, (currentLap >= 10) ? (currentLap / 10) : 0x0F);
        max7219Send(1, 7, (currentLap % 10) | 0x80);  // DP separator
        max7219Send(1, 6, (totalLaps >= 10) ? (totalLaps / 10) : 0x0F);
        max7219Send(1, 5, totalLaps % 10);
    } else {
        for (int i = 5; i <= 8; i++) max7219Send(1, i, 0x0A);  // dashes
    }
    if (position > 0) {
        int total = opponents + 1;
        max7219Send(1, 4, (position >= 10) ? (position / 10) : 0x0F);
        max7219Send(1, 3, (position % 10) | 0x80);  // DP separator
        max7219Send(1, 2, (total >= 10) ? (total / 10) : 0x0F);
        max7219Send(1, 1, total % 10);
    } else {
        for (int i = 1; i <= 4; i++) max7219Send(1, i, 0x0A);  // dashes
    }

    // Device 2: Session best (left 4) | Last pin (right 4) — Raw segments
    max7219Send(2, 0x09, 0x00);
    if (sessionBestLapMs > 0) {
        int totalSec = sessionBestLapMs / 1000;
        int mins = totalSec / 60;
        int secs = totalSec % 60;
        int tenths = (sessionBestLapMs % 1000) / 100;
        max7219Send(2, 8, SEG_DIGIT[mins % 10] | 0x80);
        max7219Send(2, 7, SEG_DIGIT[secs / 10]);
        max7219Send(2, 6, SEG_DIGIT[secs % 10] | 0x80);
        max7219Send(2, 5, SEG_DIGIT[tenths]);
    } else {
        max7219Send(2, 8, SEG_DASH | 0x80);
        max7219Send(2, 7, SEG_DASH);
        max7219Send(2, 6, SEG_DASH | 0x80);
        max7219Send(2, 5, SEG_DASH);
    }
    if (lastTriggerPin >= 0) {
        max7219Send(2, 4, (lastTriggerPort == 'A') ? SEG_A : SEG_b);
        max7219Send(2, 3, SEG_BLANK);
        max7219Send(2, 2, SEG_BLANK);
        max7219Send(2, 1, SEG_DIGIT[lastTriggerPin % 10]);
    } else {
        for (int i = 1; i <= 4; i++) max7219Send(2, i, SEG_DASH);
    }
}

// ---- View 1: Tire Wear ----
void showView1() {
    // Device 0: Label "tirE" — Raw segments
    max7219Send(0, 0x09, 0x00);
    max7219Send(0, 8, SEG_t);
    max7219Send(0, 7, SEG_I);
    max7219Send(0, 6, SEG_r);
    max7219Send(0, 5, SEG_E);
    max7219Send(0, 4, SEG_BLANK);
    max7219Send(0, 3, SEG_BLANK);
    max7219Send(0, 2, SEG_BLANK);
    max7219Send(0, 1, SEG_BLANK);

    // Device 1: FL (left 4) | FR (right 4) — BCD
    max7219Send(1, 0x09, 0xFF);
    max7219ShowDigits(1, tyreWearFL, 5, 4);
    max7219ShowDigits(1, tyreWearFR, 1, 4);

    // Device 2: RL (left 4) | RR (right 4) — BCD
    max7219Send(2, 0x09, 0xFF);
    max7219ShowDigits(2, tyreWearRL, 5, 4);
    max7219ShowDigits(2, tyreWearRR, 1, 4);
}

// ---- View 2: Tire Temperature ----
void showView2() {
    // Device 0: Label "tEnP" — Raw segments
    max7219Send(0, 0x09, 0x00);
    max7219Send(0, 8, SEG_t);
    max7219Send(0, 7, SEG_E);
    max7219Send(0, 6, SEG_n);
    max7219Send(0, 5, SEG_P);
    max7219Send(0, 4, SEG_BLANK);
    max7219Send(0, 3, SEG_BLANK);
    max7219Send(0, 2, SEG_BLANK);
    max7219Send(0, 1, SEG_BLANK);

    // Device 1: FL (left 4) | FR (right 4) — BCD
    max7219Send(1, 0x09, 0xFF);
    max7219ShowDigits(1, tyreTempFL, 5, 4);
    max7219ShowDigits(1, tyreTempFR, 1, 4);

    // Device 2: RL (left 4) | RR (right 4) — BCD
    max7219Send(2, 0x09, 0xFF);
    max7219ShowDigits(2, tyreTempRL, 5, 4);
    max7219ShowDigits(2, tyreTempRR, 1, 4);
}

// ---- View 3: Lap Times (full precision) ----
void showView3() {
    // Each device shows one time in _M.SS.mmm format
    max7219ShowFullLapTime(0, allTimeBestLapMs);   // Row 1: All-time best
    max7219ShowFullLapTime(1, sessionBestLapMs);   // Row 2: Session best
    max7219ShowFullLapTime(2, lastLapMs);           // Row 3: Last lap
}

// Master display update — call after data changes or view switch
void updateMaxDisplays() {
    // Check master switch - blank displays in standby mode
    if (!masterEnabled) {
        max7219Blank();
        return;
    }

    // Ensure displays are awake
    max7219Wake();

    switch (currentView) {
        case 0: showView0(); break;
        case 1: showView1(); break;
        case 2: showView2(); break;
        case 3: showView3(); break;
    }
}

// ============== Display ==============

// Helper: format milliseconds as lap time string
String formatLapTime(unsigned long ms) {
    if (ms == 0) return "--:--.---";
    int minutes = ms / 60000;
    int seconds = (ms % 60000) / 1000;
    int millis = ms % 1000;
    char buf[12];
    snprintf(buf, sizeof(buf), "%d:%02d.%03d", minutes, seconds, millis);
    return String(buf);
}

// Standby View - shown when master switch is OFF
void drawStandbyView() {
    // Center the standby message
    spr.setTextDatum(MC_DATUM);
    spr.setTextColor(COLOR_DIM);
    spr.drawString("STANDBY", SCREEN_WIDTH/2, SCREEN_HEIGHT/2 - 20, 4);
    spr.setTextColor(0x4208);  // Darker gray
    spr.drawString("Flip master switch to activate", SCREEN_WIDTH/2, SCREEN_HEIGHT/2 + 20, 2);
}

// Debug View - connectivity, ports, wind, key
void drawDebugView() {
    // Left column: Connection status
    int col1 = 10;
    spr.setTextDatum(TL_DATUM);
    spr.setTextColor(COLOR_TEXT);
    spr.drawString("DEBUG", col1, 5, 2);

    wifiConnected = WiFi.status() == WL_CONNECTED;
    mqttConnected = mqtt.connected();
    bool bleOk = isActuallyConnected();

    int y = 28;
    spr.setTextColor(COLOR_DIM);
    spr.drawString("WiFi", col1, y, 2);
    spr.fillCircle(col1 + 50, y + 7, 5, wifiConnected ? COLOR_ACCENT : COLOR_WARN);

    y += 20;
    spr.drawString("MQTT", col1, y, 2);
    spr.fillCircle(col1 + 50, y + 7, 5, mqttConnected ? COLOR_ACCENT : COLOR_WARN);

    y += 20;
    spr.drawString("BLE", col1, y, 2);
    spr.fillCircle(col1 + 50, y + 7, 5, bleOk ? COLOR_ACCENT : COLOR_WARN);

    y += 20;
    spr.drawString("I2C", col1, y, 2);
    spr.fillCircle(col1 + 50, y + 7, 5, mcpFound ? COLOR_ACCENT : COLOR_WARN);

    // Wind status
    y += 22;
    spr.setTextColor(COLOR_DIM);
    spr.drawString("Wind", col1, y, 2);
    spr.setTextColor(windEnabled ? COLOR_ACCENT : COLOR_WARN);
    spr.drawString(windEnabled ? "ON" : "OFF", col1 + 50, y, 2);

    y += 18;
    spr.fillRect(col1, y, 60, 10, COLOR_DIM);
    int barWidth = map(fanSpeedPercent, 0, 100, 0, 60);
    if (barWidth > 0) {
        spr.fillRect(col1, y, barWidth, 10, windEnabled ? COLOR_ACCENT : COLOR_WARN);
    }

    // Center column: Port A/B indicators (vertical)
    int col2 = 100;
    spr.setTextColor(COLOR_TEXT);
    spr.drawString("PORTS", col2, 5, 2);

    spr.setTextColor(COLOR_DIM);
    spr.drawString("A", col2, 28, 1);
    for (int i = 0; i < 8; i++) {
        uint8_t bit = (currentPortA >> i) & 0x01;
        spr.fillCircle(col2 + 15 + i * 12, 32, 4, (bit == 0) ? COLOR_ACCENT : COLOR_DIM);
    }

    spr.drawString("B", col2, 48, 1);
    for (int i = 0; i < 8; i++) {
        uint8_t bit = (currentPortB >> i) & 0x01;
        spr.fillCircle(col2 + 15 + i * 12, 52, 4, (bit == 0) ? COLOR_ACCENT : COLOR_DIM);
    }

    // Last trigger
    spr.setTextColor(COLOR_DIM);
    spr.drawString("Last:", col2, 68, 1);
    if (lastTriggerPin >= 0) {
        spr.setTextColor(COLOR_ACCENT);
        spr.drawString(String(lastTriggerPort) + String(lastTriggerPin), col2 + 35, 68, 1);
    }

    // MAX view
    spr.setTextColor(COLOR_DIM);
    spr.drawString("MAX:", col2, 85, 1);
    spr.setTextColor(COLOR_TEXT);
    spr.drawString(viewNames[currentView], col2 + 35, 85, 1);

    // Last key
    spr.setTextColor(COLOR_DIM);
    spr.drawString("Key:", col2, 102, 1);
    spr.setTextColor(COLOR_TEXT);
    spr.drawString(lastKey.length() > 0 ? lastKey : "-", col2 + 35, 102, 1);

    // Right column: SimHub data preview
    int col3 = 220;
    spr.setTextColor(COLOR_TEXT);
    spr.drawString("SIM", col3, 5, 2);

    spr.setTextColor(COLOR_DIM);
    spr.drawString("Spd", col3, 28, 1);
    spr.setTextColor(COLOR_TEXT);
    spr.drawString(String(carSpeedKmh), col3 + 30, 28, 1);

    spr.setTextColor(COLOR_DIM);
    spr.drawString("Pos", col3, 45, 1);
    spr.setTextColor(COLOR_TEXT);
    spr.drawString(String(position) + "/" + String(opponents + 1), col3 + 30, 45, 1);

    spr.setTextColor(COLOR_DIM);
    spr.drawString("Lap", col3, 62, 1);
    spr.setTextColor(COLOR_TEXT);
    spr.drawString(String(currentLap) + "/" + String(totalLaps), col3 + 30, 62, 1);
}

// Racing View - big speed, position, lap info
void drawRacingView() {
    // Big speed in center-left
    spr.setTextDatum(TL_DATUM);
    spr.setTextColor(COLOR_DIM);
    spr.drawString("SPEED", 10, 5, 2);

    spr.setTextDatum(TL_DATUM);
    spr.setTextColor(COLOR_TEXT);
    spr.drawString(String(carSpeedKmh), 10, 25, 7);  // Big font
    spr.setTextColor(COLOR_DIM);
    spr.drawString("km/h", 10, 85, 2);

    // Position box (top right)
    int posX = 160;
    spr.setTextColor(COLOR_DIM);
    spr.drawString("POS", posX, 5, 2);
    spr.setTextColor(COLOR_ACCENT);
    spr.drawString(String(position), posX, 25, 6);
    spr.setTextColor(COLOR_DIM);
    spr.drawString("/" + String(opponents + 1), posX + 40, 40, 2);

    // Lap box (middle right)
    spr.setTextColor(COLOR_DIM);
    spr.drawString("LAP", posX + 80, 5, 2);
    spr.setTextColor(COLOR_TEXT);
    spr.drawString(String(currentLap), posX + 80, 25, 4);
    spr.setTextColor(COLOR_DIM);
    spr.drawString("/" + String(totalLaps), posX + 110, 30, 2);

    // Best lap time (bottom)
    spr.setTextDatum(TL_DATUM);
    spr.setTextColor(COLOR_DIM);
    spr.drawString("BEST", 160, 70, 2);
    spr.setTextColor(COLOR_ACCENT);
    spr.drawString(formatLapTime(sessionBestLapMs), 160, 90, 4);

    // Last lap (bottom right)
    spr.setTextColor(COLOR_DIM);
    spr.drawString("LAST", 160, 120, 1);
    spr.setTextColor(COLOR_TEXT);
    spr.drawString(formatLapTime(lastLapMs), 200, 120, 2);

    // Wind indicator (bottom left)
    spr.setTextColor(COLOR_DIM);
    spr.drawString("Wind:", 10, 110, 1);
    spr.fillRect(50, 110, 80, 8, COLOR_DIM);
    int barWidth = map(fanSpeedPercent, 0, 100, 0, 80);
    if (barWidth > 0) {
        spr.fillRect(50, 110, barWidth, 8, windEnabled ? COLOR_ACCENT : COLOR_WARN);
    }
}

void drawScreen() {
    spr.fillSprite(COLOR_BG);

    // Check master switch first
    if (!masterEnabled) {
        drawStandbyView();
        spr.pushSprite(0, 0);
        return;
    }

    // Draw current TFT view
    if (tftView == 0) {
        drawDebugView();
    } else {
        drawRacingView();
    }

    // Footer with view indicator
    spr.setTextDatum(TC_DATUM);
    spr.setTextColor(COLOR_DIM);
    String footer = "v2.0.0  [" + String(tftViewNames[tftView]) + "]  BTN2=view";
    spr.drawString(footer, SCREEN_WIDTH/2, SCREEN_HEIGHT - 12, 1);

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
    static unsigned long lastLeftPress = 0;
    static unsigned long lastRightPress = 0;
    const unsigned long debounceMs = 200;

    bool buttonLeft = digitalRead(BUTTON_LEFT);
    bool buttonRight = digitalRead(BUTTON_RIGHT);

    // Left button - cycle MAX7219 view
    if (buttonLeft == LOW && millis() - lastLeftPress > debounceMs) {
        lastLeftPress = millis();
        currentView = (currentView + 1) % NUM_VIEWS;
        updateMaxDisplays();
        drawScreen();
        Serial.printf("[View] Switched to %d: %s\n", currentView, viewNames[currentView]);
    }

    // Right button - toggle TFT view
    if (buttonRight == LOW && millis() - lastRightPress > debounceMs) {
        lastRightPress = millis();
        tftView = (tftView + 1) % NUM_TFT_VIEWS;
        drawScreen();
        Serial.printf("[TFT] Switched to %d: %s\n", tftView, tftViewNames[tftView]);
    }
}

// ============== Setup ==============

void setup() {
    Serial.begin(115200);
    delay(500);

    Serial.println("\n=== SimBee Keyboard v2.0.0 ===");

    // Display first so we can show status
    pinMode(TFT_BACKLIGHT, OUTPUT);
    pinMode(TFT_POWER, OUTPUT);
    digitalWrite(TFT_POWER, HIGH);
    tft.init();
    tft.setRotation(3);
    tft.fillScreen(COLOR_BG);
    tft.invertDisplay(true);
    digitalWrite(TFT_BACKLIGHT, HIGH);
    spr.createSprite(SCREEN_WIDTH, SCREEN_HEIGHT);
    spr.setSwapBytes(true);

    // Show WiFi status on screen
    tft.setTextColor(COLOR_TEXT);
    tft.setTextDatum(MC_DATUM);
    tft.drawString("WiFi Setup...", SCREEN_WIDTH/2, SCREEN_HEIGHT/2 - 20, 2);
    tft.drawString("Connect to: SimBee-Setup", SCREEN_WIDTH/2, SCREEN_HEIGHT/2 + 10, 2);

    // WiFi via WiFiManager (captive portal if not configured)
    Serial.println("[WiFi] Starting WiFiManager...");
    wifiManager.setConfigPortalTimeout(180);  // 3 min timeout

    if (!wifiManager.autoConnect("SimBee-Setup")) {
        Serial.println("[WiFi] Failed to connect - restarting");
        tft.fillScreen(COLOR_BG);
        tft.drawString("WiFi Failed!", SCREEN_WIDTH/2, SCREEN_HEIGHT/2, 2);
        delay(3000);
        ESP.restart();
    }

    Serial.printf("[WiFi] Connected! IP: %s\n", WiFi.localIP().toString().c_str());
    tft.fillScreen(COLOR_BG);
    tft.drawString("Connected!", SCREEN_WIDTH/2, SCREEN_HEIGHT/2 - 10, 2);
    tft.drawString(WiFi.localIP().toString(), SCREEN_WIDTH/2, SCREEN_HEIGHT/2 + 10, 2);
    delay(1000);

    // I2C for MCP23017 (QWIIC port)
    Wire.begin(I2C_SDA, I2C_SCL);
    Wire.setClock(100000);
    Serial.printf("[I2C] Initialized on SDA=%d, SCL=%d\n", I2C_SDA, I2C_SCL);

    // Buttons
    pinMode(BUTTON_LEFT, INPUT_PULLUP);
    pinMode(BUTTON_RIGHT, INPUT_PULLUP);
    
    // Start BLE keyboard (initializes NimBLE)
    Serial.println("[BLE] Initializing keyboard...");
    keyboard.begin();
    
    // Now we can safely use NimBLE functions
    delay(500);
    
    // Keep existing BLE bonds (clearing causes Windows re-pair crash loop)
    Serial.printf("[BLE] Bonds stored: %d\n", NimBLEDevice::getNumBonds());
    
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

        // Initialize master switch state from PA0 (LOW = ON/active)
        masterEnabled = !(currentPortA & 0x01);
        Serial.printf("[Master] Initial state: %s\n", masterEnabled ? "ACTIVE" : "STANDBY");

        // Initialize wind switch state from PA4 (LOW = ON/enabled)
        windEnabled = !(currentPortA & 0x10);
        Serial.printf("[Wind] Initial state: %s\n", windEnabled ? "ENABLED" : "DISABLED");

        // Set initial TFT view based on master switch
        if (masterEnabled) {
            tftView = 1;  // Racing view when active
        }
    }

    // Initialize SimHub UDP listener
    simhubUDP.begin(SIMHUB_PORT);
    Serial.printf("[SimHub] UDP listening on port %d\n", SIMHUB_PORT);
    Serial.printf("[SimHub] Send data to: %s:%d\n", WiFi.localIP().toString().c_str(), SIMHUB_PORT);

    // Initialize MAX7219 7-segment displays (3 daisy-chained)
    max7219Init();
    updateMaxDisplays();   // Show initial view (View 0: Speed)
    Serial.printf("[MAX7219] 3 displays initialized, view: %s\n", viewNames[currentView]);

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
