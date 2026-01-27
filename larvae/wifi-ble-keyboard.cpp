/**
 * WiFi + BLE Keyboard Template
 * 
 * ESP32-S3 template with both WiFi and BLE HID keyboard.
 * Tested on T-Display S3 (LilyGo).
 * 
 * Features:
 *   - WiFi connectivity (hardcoded credentials)
 *   - BLE HID keyboard (NimBLE-based)
 *   - TFT display with status
 *   - Two button inputs
 * 
 * To use this template:
 *   1. Copy to workers/ directory with your device name
 *   2. Update DEVICE_NAME and FIRMWARE_VERSION
 *   3. Set your WIFI_SSID and WIFI_PASS
 *   4. Customize button->key mappings in handleButtons()
 *   5. Add PlatformIO environment (copy from simbee-keyboard)
 * 
 * Hardware (T-Display S3):
 *   - Left button:  GPIO 0
 *   - Right button: GPIO 14
 *   - Display: ST7789 170x320 (8-bit parallel)
 *   - Backlight: GPIO 38
 *   - Power: GPIO 15
 */

#include <Arduino.h>
#include <WiFi.h>
#include <NimBLEDevice.h>
#include <BleKeyboard.h>
#include <TFT_eSPI.h>

// ============== CUSTOMIZE THESE ==============

#define DEVICE_NAME "MyBee"
#define FIRMWARE_VERSION "1.0.0"

// WiFi credentials
#define WIFI_SSID "your-ssid"
#define WIFI_PASS "your-password"

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

BleKeyboard keyboard(DEVICE_NAME, "Beehive", 100);

// Track connection states
bool bleConnected = false;
bool wifiConnected = false;
bool lastButtonLeft = HIGH;
bool lastButtonRight = HIGH;
bool wasConnected = false;
String lastKey = "-";
unsigned long lastKeyTime = 0;

// ============== Forward Declarations ==============

bool isActuallyConnected();
void drawScreen();
void sendKey(char key);
void handleButtons();

// ============== BLE Connection Check ==============

// Check actual BLE connection using NimBLE directly
// (keyboard.isConnected() can return false positives)
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

// ============== Display ==============

void drawScreen() {
    spr.fillSprite(COLOR_BG);
    
    // Title
    spr.setTextColor(COLOR_TEXT);
    spr.setTextDatum(TC_DATUM);
    spr.drawString(DEVICE_NAME, SCREEN_WIDTH/2, 20, 4);
    
    // WiFi Status
    int wifiY = 70;
    spr.setTextDatum(TL_DATUM);
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
    spr.fillCircle(SCREEN_WIDTH - 30, wifiY + 6, 6, wifiConnected ? COLOR_ACCENT : COLOR_WARN);
    
    // BLE Status
    bool connected = isActuallyConnected();
    int statusY = 100;
    
    spr.setTextDatum(TL_DATUM);
    spr.setTextColor(COLOR_DIM);
    spr.drawString("BLE:", 20, statusY, 2);
    
    if (connected) {
        spr.setTextColor(COLOR_ACCENT);
        spr.drawString("Connected", 70, statusY, 2);
    } else {
        spr.setTextColor(COLOR_WARN);
        spr.drawString("Waiting...", 70, statusY, 2);
    }
    spr.fillCircle(SCREEN_WIDTH - 30, statusY + 6, 6, connected ? COLOR_ACCENT : COLOR_WARN);
    
    // Last key sent
    int keyY = 150;
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
        spr.drawString(agoStr, 20, keyY + 25, 1);
    }
    
    // Instructions - customize these for your keys
    int helpY = 220;
    spr.setTextColor(COLOR_DIM);
    spr.setTextDatum(TC_DATUM);
    spr.drawString("Left btn: 'a'", SCREEN_WIDTH/2, helpY, 2);
    spr.drawString("Right btn: 'b'", SCREEN_WIDTH/2, helpY + 25, 2);
    
    // Footer
    spr.drawString("v" FIRMWARE_VERSION, SCREEN_WIDTH/2, SCREEN_HEIGHT - 30, 1);
    
    // Push to display
    spr.pushSprite(0, 0);
}

// ============== Keyboard ==============

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

// For special keys, use keyboard.write() with constants like:
//   KEY_RETURN, KEY_ESC, KEY_BACKSPACE, KEY_TAB, KEY_SPACE
//   KEY_UP_ARROW, KEY_DOWN_ARROW, KEY_LEFT_ARROW, KEY_RIGHT_ARROW
//   KEY_LEFT_CTRL, KEY_LEFT_SHIFT, KEY_LEFT_ALT, KEY_LEFT_GUI
// Or for key combos: keyboard.press(KEY_LEFT_CTRL); keyboard.press('c'); keyboard.releaseAll();

void handleButtons() {
    bool buttonLeft = digitalRead(BUTTON_LEFT);
    bool buttonRight = digitalRead(BUTTON_RIGHT);
    
    // Left button - customize this key
    if (buttonLeft == LOW && lastButtonLeft == HIGH) {
        sendKey('a');
    }
    
    // Right button - customize this key
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
    
    Serial.printf("\n=== %s v%s ===\n", DEVICE_NAME, FIRMWARE_VERSION);
    
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
    tft.invertDisplay(true);  // T-Display S3 needs this
    
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
    Serial.printf("[BLE] Device name: %s\n", DEVICE_NAME);
    Serial.println("[BLE] Ready for pairing!");
    
    drawScreen();
}

// ============== Loop ==============

void loop() {
    handleButtons();
    
    // Update display periodically or on connection change
    static unsigned long lastDraw = 0;
    static unsigned long lastStatusLog = 0;
    
    bool connected = isActuallyConnected();
    
    // Log status every 5 seconds for debugging
    if (millis() - lastStatusLog > 5000) {
        lastStatusLog = millis();
        Serial.printf("[Status] WiFi=%d BLE=%d\n", 
                      WiFi.status() == WL_CONNECTED, connected);
    }
    
    if (connected != wasConnected || millis() - lastDraw > 1000) {
        if (connected != wasConnected) {
            Serial.printf("[BLE] State changed: %s\n", connected ? "CONNECTED" : "DISCONNECTED");
        }
        wasConnected = connected;
        lastDraw = millis();
        drawScreen();
    }
    
    delay(10);
}
