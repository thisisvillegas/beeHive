/**
 * SimBee - BLE Keyboard + MQTT Command Hub
 *
 * T-Display S3 based wireless controller for sim racing and home automation
 * - espMqttClient (async MQTT)
 * - ESPAsyncWebServer (async HTTP)
 * - TFT_eSPI (display driver)
 *
 * Display: 170x320 ST7789
 * Buttons: GPIO 0 (left), GPIO 14 (right)
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
#include <time.h>
#include <math.h>
#include <stdarg.h>
// #include <BleKeyboard.h>  // Disabled - causes WiFi/display interference

// ============== Hardware Configuration ==============

#define BUTTON_LEFT 0
#define BUTTON_RIGHT 14
#define TFT_BACKLIGHT 38
#define TFT_POWER 15  // LCD power enable pin
#define BATTERY_ADC_PIN 4  // Battery voltage ADC (via voltage divider)

// T-Display S3 has no dedicated LED, use virtual LED on display
#define VIRTUAL_LED true

// Battery voltage calibration (LiPo via voltage divider)
#define BATTERY_MAX_VOLTAGE 4.2f   // Fully charged
#define BATTERY_MIN_VOLTAGE 3.0f   // Empty (safe cutoff)
#define BATTERY_DIVIDER_RATIO 2.0f // Voltage divider halves the reading
#define BATTERY_USB_THRESHOLD 4.3f // Above this = USB power (can't read true battery)

// LiPo voltage-to-percentage lookup table (non-linear discharge curve)
// Based on typical LiPo discharge profile - NOT linear!
// When USB connected (>4.3V), we're reading charging voltage, not battery
struct BatteryLookup {
    float voltage;
    int percent;
};
const BatteryLookup BATTERY_CURVE[] = {
    {4.20f, 100},
    {4.15f, 95},
    {4.10f, 90},
    {4.05f, 85},
    {4.00f, 80},
    {3.95f, 75},
    {3.90f, 70},
    {3.85f, 65},
    {3.80f, 60},
    {3.75f, 55},
    {3.70f, 50},  // Nominal voltage
    {3.65f, 45},
    {3.60f, 40},
    {3.55f, 35},
    {3.50f, 30},
    {3.45f, 25},
    {3.40f, 20},
    {3.35f, 15},
    {3.30f, 10},
    {3.20f, 5},
    {3.00f, 0},   // Cutoff
};
const int BATTERY_CURVE_SIZE = sizeof(BATTERY_CURVE) / sizeof(BATTERY_CURVE[0]);

#define DEVICE_TYPE "esp32-s3-simbee"
#define FIRMWARE_VERSION "1.0.0-simbee"
#define DEFAULT_MQTT_PORT 1883

// Display dimensions (portrait mode - will be swapped in landscape)
#define SCREEN_WIDTH 170
#define SCREEN_HEIGHT 320
#define SCREEN_WIDTH_LANDSCAPE 320
#define SCREEN_HEIGHT_LANDSCAPE 170

// Colors
#define COLOR_BG        TFT_BLACK  // Black background (same as debug)
#define COLOR_TEXT      0xFFFF  // White
#define COLOR_ACCENT    0x3B7F  // Blue (#3b82f6)
#define COLOR_SUCCESS   0x2668  // Green (#22c55e)
#define COLOR_ERROR     0xF800  // Red
#define COLOR_MUTED     0x8C71  // Lighter gray for better contrast
#define COLOR_CARD      0x1083  // Dark blue-gray card

// Debug view colors (simple black & white)
#define COLOR_DEBUG_BG    TFT_BLACK  // Pure black
#define COLOR_DEBUG_TEXT  TFT_WHITE  // White text
#define COLOR_DEBUG_DIM   0x8410     // Gray for labels
#define COLOR_DEBUG_WARN  0xFFE0     // Yellow for warnings

// Heart animation colors (RGB565 format)
#define COLOR_HEART      0xF800  // Red
#define COLOR_HEART_GLOW 0xFB20  // Light red/coral
#define COLOR_PINK       0xF81F  // Pink/magenta
#define COLOR_LIGHT_PINK 0xFDDF  // Soft pink
#define COLOR_DARK_RED   0xA000  // Dark red

// Tiny Chef colors (based on the actual character)
#define COLOR_CHEF_HAT   0xF79E  // Cream/beige chef hat
#define COLOR_CHEF_BODY  0x6C69  // Olive/sage green body
#define COLOR_CHEF_MITT  0x3D5E  // Blue oven mitts
#define COLOR_MITT_DOT   0x2958  // Darker blue dots on mitts
#define COLOR_APRON_RED  0xF800  // Apron stripe - red
#define COLOR_APRON_ORG  0xFC60  // Apron stripe - orange
#define COLOR_APRON_YEL  0xFFE0  // Apron stripe - yellow
#define COLOR_APRON_GRN  0x07E0  // Apron stripe - green
#define COLOR_APRON_BLU  0x001F  // Apron stripe - blue
#define COLOR_ORANGE     0xFD20  // Warm accent (for sun)

// NTP and Location Configuration (Austin, Texas)
#define NTP_SERVER1 "pool.ntp.org"
#define NTP_SERVER2 "time.nist.gov"
#define TZ_OFFSET -6  // CST (UTC-6), DST handled by configTzTime
#define TZ_INFO "CST6CDT,M3.2.0,M11.1.0"  // US Central Time with DST

// Austin, TX coordinates for sunrise/sunset
#define LOCATION_LAT  30.2672
#define LOCATION_LON -97.7431

// ============== Global Objects ==============

TFT_eSPI tft = TFT_eSPI();
TFT_eSprite heartSprite = TFT_eSprite(&tft);  // Sprite for flicker-free animation
espMqttClient mqttClient;
AsyncWebServer webServer(80);
Preferences preferences;
WiFiManager wifiManager;
// BLE Keyboard disabled - library causes WiFi/display interference
// BleKeyboard bleKeyboard("SimBee", "Beehive", 100);
// bool bleStarted = false;

// ============== Heart Animation System ==============

enum AnimationType { ANIM_NONE = 0, ANIM_PULSE = 1, ANIM_SHOWER = 2, ANIM_BURST = 3, ANIM_SEND = 4 };

struct Particle {
    float x, y;
    float vx, vy;
    float scale;
    uint8_t life;
    uint16_t color;
};

#define MAX_PARTICLES 25
Particle particles[MAX_PARTICLES];

struct {
    AnimationType type = ANIM_NONE;
    unsigned long endTime = 0;
    unsigned long lastFrame = 0;
    float phase = 0;           // For pulse sine wave
    float burstScale = 0;      // For burst growth
    bool burstExploded = false;
    int previousScreen = 0;    // To return after animation
    char message[64] = "";     // Custom message text
    float sendX = 0;           // For send animation X position
    float sendScale = 1.0;     // For send animation shrink
} heartAnim;

// Default messages for each animation type
const char* defaultMessages[] = {
    "",                    // ANIM_NONE
    "Thinking of you",     // ANIM_PULSE
    "Love you!",           // ANIM_SHOWER
    "XOXO",                // ANIM_BURST
    "Sent!"                // ANIM_SEND
};

// ============== Chef Animation System ==============

// Chef animation state
struct {
    int frame = 0;
    unsigned long lastFrame = 0;
    bool facingRight = true;
    float bobOffset = 0;
} chefAnim;

// Time tracking
bool ntpSynced = false;
int sunriseHour = 6, sunriseMin = 45;   // Default values
int sunsetHour = 18, sunsetMin = 30;    // Will be calculated
unsigned long lastSunCalc = 0;

// Timers for non-blocking periodic tasks
unsigned long lastHealthPublish = 0;
unsigned long lastStatePublish = 0;
unsigned long lastReconnectAttempt = 0;
unsigned long lastDisplayUpdate = 0;
unsigned long debugDrawCount = 0;  // Track how many times debug screen draws

// Button state
bool lastButtonLeft = HIGH;
bool lastButtonRight = HIGH;
unsigned long buttonLeftPressTime = 0;
unsigned long buttonRightPressTime = 0;

// Display state
// Screen 0 = Debug (landscape), 1 = Kayci (portrait), 2 = Animation
int currentScreen = 0;
bool displayNeedsUpdate = true;
bool isLandscape = true;  // Track current orientation
bool spriteValid = false; // Track if sprite was created successfully

// ============== Device State ==============

struct DeviceConfig {
    char deviceName[32];
    char mqttServer[64];
    int mqttPort;
    char mqttUser[32];
    char mqttPass[32];
    char topicPrefix[32];
    char companionBeeId[16];  // Device ID of companion bee (e.g., TinyBee1)
    // Dimming schedule (24-hour format)
    int dimStartHour = 1;     // Start dimming at 1 AM
    int dimEndHour = 7;       // End dimming at 7 AM
    int dimBrightness = 20;   // Brightness during dim period (0-255)
    int normalBrightness = 255; // Normal brightness (0-255)
} config;

struct DeviceState {
    bool ledOn = false;
    bool mqttConnected = false;
    unsigned long uptime = 0;
    int rssi = 0;
    // Battery
    float batteryVoltage = 0.0f;
    int batteryPercent = -1;  // -1 = no battery detected
    bool batteryCharging = false;
    // Battery debug info
    int batteryRawAdc = 0;        // Raw 12-bit ADC value
    float batteryAdcVoltage = 0;  // Voltage before divider (0-3.3V)
    float batteryMinVoltage = 99; // Min seen this session
    float batteryMaxVoltage = 0;  // Max seen this session
    bool batteryUsbPower = false; // USB power detected
    float batteryPrevVoltage = 0; // For trend detection
    // Companion bee status
    bool tinyBeeOnline = false;
    unsigned long lastTinyBeeSeen = 0;
} state;

// Brightness state
int currentBrightness = 255;
bool isDimmed = false;

// Battery history for predictions
#define BATTERY_HISTORY_SIZE 60  // 60 samples (1 hour at 1-min intervals)
#define BATTERY_SAMPLE_INTERVAL 60000  // 1 minute between samples

struct BatteryHistoryEntry {
    unsigned long timestamp;  // millis() when sampled
    float voltage;
    bool usbPower;
};

BatteryHistoryEntry batteryHistory[BATTERY_HISTORY_SIZE];
int batteryHistoryIndex = 0;
int batteryHistoryCount = 0;
unsigned long lastBatterySample = 0;

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
#define LOG_PUBLISH_INTERVAL 500  // Publish logs every 500ms

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
void loadConfig();
void saveConfig();
String getDeviceId();
void setupWebServer();
void setupDisplay();
void updateDisplay();
void drawDebugScreen();
void drawKayciScreen();
void drawBatteryDebugScreen();
void drawBatteryViewV2();
int getBatteryPercentFromCurve(float voltage);
void handleButtons();
void switchToDebugView();
void switchToKayciView();
void switchToBatteryDebugView();
void startSendAnimation();

// Heart animation functions
void setupHeartSprite();
void startHeartAnimation(int type, const char* message = nullptr);
void stopHeartAnimation();
void updateHeartAnimation();
void drawHeart(TFT_eSprite &spr, int cx, int cy, int size, uint16_t color);
void initParticles();
void spawnParticle(float x, float y, float vx, float vy, float scale, uint16_t color);
void updateParticles();
void renderPulsingHeart();
void renderHeartShower();
void renderHeartBurst();
void renderSendingHeart();

// Battery functions
void setupBattery();
void readBattery();
void sampleBatteryHistory();
float getVoltageChangeRate();
int getEstimatedMinutes();
void drawBatteryIcon(TFT_eSprite &spr, int x, int y);

// TinyBee communication
void pingTinyBee();

// Time and sun functions
void setupNTP();
void calculateSunTimes();
void updateBrightness();
void drawChef(TFT_eSprite &spr, int x, int y, int frame, bool facingRight);

// ============== Display Functions ==============

void setupDisplay() {
    // Enable LCD power first (GPIO 15 must be HIGH)
    pinMode(TFT_POWER, OUTPUT);
    digitalWrite(TFT_POWER, HIGH);
    delay(10);  // Let power stabilize

    // Initialize display in landscape mode (both views use landscape)
    tft.init();
    tft.invertDisplay(true);  // T-Display S3 needs color inversion
    tft.setRotation(1);  // Landscape mode (320x170)
    isLandscape = true;

    // Enable backlight using PWM
    ledcAttach(TFT_BACKLIGHT, 5000, 8);  // 5kHz, 8-bit
    ledcWrite(TFT_BACKLIGHT, 255);  // Full brightness

    tft.fillScreen(COLOR_DEBUG_BG);

    // Show boot screen (landscape)
    tft.setTextColor(COLOR_DEBUG_TEXT);
    tft.setTextDatum(MC_DATUM);
    tft.drawString("Homecontrol", SCREEN_WIDTH_LANDSCAPE/2, SCREEN_HEIGHT_LANDSCAPE/2 - 20, 4);
    tft.setTextColor(COLOR_DEBUG_DIM);
    tft.drawString("SimBee", SCREEN_WIDTH_LANDSCAPE/2, SCREEN_HEIGHT_LANDSCAPE/2 + 10, 2);
    tft.drawString(FIRMWARE_VERSION, SCREEN_WIDTH_LANDSCAPE/2, SCREEN_HEIGHT_LANDSCAPE/2 + 30, 2);
}

void drawStatusDot(int x, int y, bool active) {
    uint16_t color = active ? COLOR_SUCCESS : COLOR_ERROR;
    tft.fillCircle(x, y, 6, color);
}

// ============== Screen Switching Functions ==============

void switchToDebugView() {
    // No rotation change - both views use portrait mode now
    currentScreen = 0;
    displayNeedsUpdate = true;
    Serial.println("[Display] Switched to debug view");
}

void switchToKayciView() {
    // No rotation change - both views use portrait mode now
    currentScreen = 1;
    displayNeedsUpdate = true;
    Serial.println("[Display] Switched to Kayci view");
}

void switchToBatteryDebugView() {
    currentScreen = 4;
    displayNeedsUpdate = true;
    Serial.println("[Display] Switched to battery debug view");
}

// ============== Debug Screen (Landscape 320x170) ==============

void drawDebugScreen() {
    debugDrawCount++;  // Track successful draws

    // Guard: if sprite wasn't created, show error
    if (!spriteValid) {
        tft.fillScreen(TFT_RED);
        tft.setTextColor(TFT_WHITE);
        tft.setTextDatum(MC_DATUM);
        tft.drawString("Sprite Error!", 160, 85, 4);
        return;
    }

    // Use sprite for flicker-free drawing
    heartSprite.fillSprite(TFT_BLACK);

    struct tm timeinfo;
    bool hasTime = getLocalTime(&timeinfo, 0);

    const int W = SCREEN_WIDTH_LANDSCAPE;  // 320
    const int H = SCREEN_HEIGHT_LANDSCAPE; // 170

    // ============ TOP ROW: Time + Date ============
    if (hasTime) {
        char timeStr[16];
        int hour12 = timeinfo.tm_hour % 12;
        if (hour12 == 0) hour12 = 12;
        sprintf(timeStr, "%d:%02d:%02d %s", hour12, timeinfo.tm_min, timeinfo.tm_sec,
                timeinfo.tm_hour >= 12 ? "PM" : "AM");
        heartSprite.setTextDatum(TL_DATUM);
        heartSprite.setTextColor(COLOR_DEBUG_TEXT, TFT_BLACK);
        heartSprite.drawString(timeStr, 10, 8, 4);

        char dateStr[20];
        const char* monthNames[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
        sprintf(dateStr, "%s %d, %d", monthNames[timeinfo.tm_mon],
                timeinfo.tm_mday, timeinfo.tm_year + 1900);
        heartSprite.setTextDatum(TR_DATUM);
        heartSprite.setTextColor(COLOR_DEBUG_DIM, TFT_BLACK);
        heartSprite.drawString(dateStr, W - 10, 12, 2);
    } else {
        heartSprite.setTextDatum(TL_DATUM);
        heartSprite.setTextColor(COLOR_DEBUG_TEXT, TFT_BLACK);
        heartSprite.drawString("--:--:-- --", 10, 8, 4);
    }

    heartSprite.drawFastHLine(10, 38, W - 20, COLOR_DEBUG_DIM);

    int leftCol = 10;
    int rightCol = W / 2 + 10;
    int y1 = 48, y2 = 78, y3 = 108;

    // WiFi
    heartSprite.setTextDatum(TL_DATUM);
    heartSprite.setTextColor(COLOR_DEBUG_DIM, TFT_BLACK);
    heartSprite.drawString("WiFi", leftCol, y1, 1);
    if (WiFi.isConnected()) {
        heartSprite.setTextColor(COLOR_DEBUG_TEXT, TFT_BLACK);
        String ssid = WiFi.SSID();
        if (ssid.length() > 12) ssid = ssid.substring(0, 12) + "..";
        heartSprite.drawString(ssid, leftCol + 30, y1, 2);
        char rssiStr[12];
        sprintf(rssiStr, "%ddBm", state.rssi);
        heartSprite.setTextColor(COLOR_SUCCESS, TFT_BLACK);
        heartSprite.drawString(rssiStr, leftCol + 30, y1 + 16, 1);
    } else {
        heartSprite.setTextColor(COLOR_ERROR, TFT_BLACK);
        heartSprite.drawString("DISCONNECTED", leftCol + 30, y1, 2);
    }

    // Companion Bee
    heartSprite.setTextColor(COLOR_DEBUG_DIM, TFT_BLACK);
    heartSprite.drawString("Bee", leftCol, y2, 1);
    if (strlen(config.companionBeeId) > 0) {
        String beeId = String(config.companionBeeId);
        if (beeId.length() > 6) beeId = beeId.substring(beeId.length() - 6);
        heartSprite.setTextColor(COLOR_DEBUG_TEXT, TFT_BLACK);
        heartSprite.drawString(beeId, leftCol + 30, y2, 2);
        heartSprite.setTextColor(state.tinyBeeOnline ? COLOR_SUCCESS : COLOR_DEBUG_DIM, TFT_BLACK);
        heartSprite.drawString(state.tinyBeeOnline ? "ONLINE" : "offline", leftCol + 30, y2 + 16, 1);
    } else {
        heartSprite.setTextColor(COLOR_DEBUG_DIM, TFT_BLACK);
        heartSprite.drawString("(not set)", leftCol + 30, y2, 2);
    }

    // Device ID + IP
    heartSprite.setTextColor(COLOR_DEBUG_DIM, TFT_BLACK);
    heartSprite.drawString("ID", leftCol, y3, 1);
    heartSprite.setTextColor(COLOR_DEBUG_TEXT, TFT_BLACK);
    heartSprite.drawString(deviceId, leftCol + 30, y3, 1);
    heartSprite.setTextColor(COLOR_DEBUG_DIM, TFT_BLACK);
    heartSprite.drawString("IP", leftCol, y3 + 12, 1);
    heartSprite.setTextColor(COLOR_DEBUG_TEXT, TFT_BLACK);
    heartSprite.drawString(WiFi.localIP().toString(), leftCol + 30, y3 + 12, 1);

    // MQTT
    heartSprite.setTextColor(COLOR_DEBUG_DIM, TFT_BLACK);
    heartSprite.drawString("MQTT", rightCol, y1, 1);
    heartSprite.setTextColor(COLOR_DEBUG_TEXT, TFT_BLACK);
    heartSprite.drawString(config.mqttServer, rightCol + 35, y1, 2);
    heartSprite.setTextColor(state.mqttConnected ? COLOR_SUCCESS : COLOR_ERROR, TFT_BLACK);
    heartSprite.drawString(state.mqttConnected ? "CONNECTED" : "OFFLINE", rightCol + 35, y1 + 16, 1);

    // Uptime
    heartSprite.setTextColor(COLOR_DEBUG_DIM, TFT_BLACK);
    heartSprite.drawString("Up", rightCol, y2, 1);
    char uptimeStr[20];
    sprintf(uptimeStr, "%luh %lum %lus", state.uptime / 3600, (state.uptime % 3600) / 60, state.uptime % 60);
    heartSprite.setTextColor(COLOR_DEBUG_TEXT, TFT_BLACK);
    heartSprite.drawString(uptimeStr, rightCol + 35, y2, 2);

    // Memory
    heartSprite.setTextColor(COLOR_DEBUG_DIM, TFT_BLACK);
    heartSprite.drawString("Heap", rightCol, y3, 1);
    char heapStr[24];
    uint32_t freeHeap = ESP.getFreeHeap() / 1024;
    sprintf(heapStr, "%luK / PSRAM %luK", freeHeap, ESP.getFreePsram() / 1024);
    heartSprite.setTextColor(freeHeap > 100 ? COLOR_DEBUG_TEXT : COLOR_DEBUG_WARN, TFT_BLACK);
    heartSprite.drawString(heapStr, rightCol + 35, y3, 1);
    heartSprite.setTextColor(COLOR_DEBUG_DIM, TFT_BLACK);
    heartSprite.drawString("Ver", rightCol, y3 + 12, 1);
    heartSprite.setTextColor(COLOR_DEBUG_TEXT, TFT_BLACK);
    heartSprite.drawString(FIRMWARE_VERSION, rightCol + 35, y3 + 12, 1);

    // Thin two-color button bar on right edge
    heartSprite.fillRect(W - 4, 0, 4, H / 2, COLOR_ACCENT);
    heartSprite.fillRect(W - 4, H / 2, 4, H / 2, COLOR_DEBUG_DIM);

    // Push sprite to display
    heartSprite.pushSprite(0, 0);
}

void drawProgressBar(int x, int y, int w, int h, int percent, uint16_t color) {
    tft.drawRect(x, y, w, h, COLOR_MUTED);
    int fillWidth = (w - 2) * percent / 100;
    tft.fillRect(x + 1, y + 1, fillWidth, h - 2, color);
}

void drawKayciScreen() {
    // Guard: if sprite wasn't created, show error and return
    if (!spriteValid) {
        tft.fillScreen(TFT_RED);
        tft.setTextColor(TFT_WHITE);
        tft.setTextDatum(MC_DATUM);
        tft.drawString("Sprite Error!", 160, 85, 4);
        Serial.println("[Display] ERROR: drawKayciScreen called with invalid sprite!");
        return;
    }

    // Use sprite for flicker-free animation (landscape 320x170)
    heartSprite.fillSprite(COLOR_BG);

    const int W = SCREEN_WIDTH_LANDSCAPE;  // 320
    const int H = SCREEN_HEIGHT_LANDSCAPE; // 170

    struct tm timeinfo;
    bool hasTime = getLocalTime(&timeinfo, 0);

    // Get current time
    char timeStr[10] = "--:--";
    char dateStr[32] = "";
    char ampm[3] = "";

    if (hasTime) {
        ntpSynced = true;
        int hour12 = timeinfo.tm_hour % 12;
        if (hour12 == 0) hour12 = 12;
        sprintf(timeStr, "%d:%02d", hour12, timeinfo.tm_min);
        strcpy(ampm, timeinfo.tm_hour >= 12 ? "PM" : "AM");

        // Format date: "Thu, Jan 9"
        const char* dayNames[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
        const char* monthNames[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
        sprintf(dateStr, "%s, %s %d", dayNames[timeinfo.tm_wday],
                monthNames[timeinfo.tm_mon], timeinfo.tm_mday);

        // Recalculate sun times once per day (or if never calculated)
        if (lastSunCalc == 0 || (millis() - lastSunCalc > 3600000)) {
            calculateSunTimes();
        }
    }

    // ============ LEFT: Dancing Chef ============
    int chefX = 55;
    int chefY = 65;
    drawChef(heartSprite, chefX, chefY, chefAnim.frame, chefAnim.facingRight);

    // ============ CENTER: Time Display ============
    heartSprite.setTextDatum(MC_DATUM);
    heartSprite.setTextColor(COLOR_TEXT);

    int timeX = W / 2 + 10;
    int timeY = 50;
    heartSprite.drawString(timeStr, timeX, timeY, 7);

    // AM/PM indicator (to the right of time)
    heartSprite.setTextDatum(TL_DATUM);
    heartSprite.setTextColor(COLOR_MUTED);
    heartSprite.drawString(ampm, timeX + 60, timeY - 20, 2);

    // Date below time
    heartSprite.setTextDatum(MC_DATUM);
    heartSprite.setTextColor(COLOR_ACCENT);
    heartSprite.drawString(dateStr, timeX, timeY + 40, 4);

    // ============ TOP-RIGHT: Battery (nudged left ~3mm from edge) ============
    drawBatteryIcon(heartSprite, W - 100, 4);

    // ============ BOTTOM: Sun Times (simple text) ============
    int sunY = H - 30;
    heartSprite.setTextDatum(TL_DATUM);

    // Sunrise
    char sunriseStr[16];
    int srHour12 = sunriseHour % 12;
    if (srHour12 == 0) srHour12 = 12;
    sprintf(sunriseStr, "Rise %d:%02d", srHour12, sunriseMin);
    heartSprite.setTextColor(COLOR_ORANGE, COLOR_BG);
    heartSprite.drawString(sunriseStr, 10, sunY, 2);

    // Sunset
    char sunsetStr[16];
    int ssHour12 = sunsetHour % 12;
    if (ssHour12 == 0) ssHour12 = 12;
    sprintf(sunsetStr, "Set %d:%02d", ssHour12, sunsetMin);
    heartSprite.setTextColor(COLOR_HEART, COLOR_BG);
    heartSprite.drawString(sunsetStr, 90, sunY, 2);

    // ============ RIGHT EDGE: Thin two-color button indicator bar ============
    heartSprite.fillRect(W - 4, 0, 4, H / 2, COLOR_HEART);
    heartSprite.fillRect(W - 4, H / 2, 4, H / 2, COLOR_ACCENT);

    // Push sprite to display
    heartSprite.pushSprite(0, 0);
}

// ============== Battery Debug Screen (Landscape 320x170) ==============

void drawBatteryDebugScreen() {
    if (!spriteValid) {
        tft.fillScreen(TFT_RED);
        tft.setTextColor(TFT_WHITE);
        tft.setTextDatum(MC_DATUM);
        tft.drawString("Sprite Error!", 160, 85, 4);
        return;
    }

    heartSprite.fillSprite(TFT_BLACK);

    const int W = 320;
    const int H = 170;
    char buf[32];

    // ============ TOP ROW: Title + USB Status ============
    heartSprite.setTextColor(TFT_WHITE, TFT_BLACK);
    heartSprite.setTextDatum(TL_DATUM);
    heartSprite.drawString("BATTERY DEBUG", 8, 4, 2);

    // USB status on right
    heartSprite.setTextDatum(TR_DATUM);
    if (state.batteryUsbPower) {
        heartSprite.setTextColor(COLOR_SUCCESS, TFT_BLACK);
        heartSprite.drawString("USB CONNECTED", W - 8, 4, 2);
    } else {
        heartSprite.setTextColor(COLOR_ORANGE, TFT_BLACK);
        heartSprite.drawString("BATTERY ONLY", W - 8, 4, 2);
    }

    // Divider line
    heartSprite.drawFastHLine(0, 22, W, COLOR_MUTED);

    // ============ LEFT COLUMN: Voltage Box ============
    int boxX = 8;
    int boxY = 28;
    int boxW = 90;
    int boxH = 55;
    uint16_t boxColor = state.batteryUsbPower ? COLOR_SUCCESS : COLOR_ORANGE;
    heartSprite.drawRect(boxX, boxY, boxW, boxH, boxColor);

    // Large voltage
    heartSprite.setTextColor(TFT_WHITE, TFT_BLACK);
    heartSprite.setTextDatum(TC_DATUM);
    sprintf(buf, "%.2fV", state.batteryVoltage);
    heartSprite.drawString(buf, boxX + boxW/2, boxY + 8, 4);

    // Percentage
    if (state.batteryPercent >= 0) {
        sprintf(buf, "%d%%", state.batteryPercent);
    } else {
        strcpy(buf, "N/A");
    }
    heartSprite.setTextColor(COLOR_MUTED, TFT_BLACK);
    heartSprite.drawString(buf, boxX + boxW/2, boxY + 38, 2);

    // ============ MIDDLE COLUMN: Raw Data ============
    int col2X = 108;
    int dataY = 28;

    heartSprite.setTextColor(COLOR_MUTED, TFT_BLACK);
    heartSprite.setTextDatum(TL_DATUM);

    sprintf(buf, "RAW: %d", state.batteryRawAdc);
    heartSprite.drawString(buf, col2X, dataY, 1);
    dataY += 12;

    sprintf(buf, "ADC: %.2fV", state.batteryAdcVoltage);
    heartSprite.drawString(buf, col2X, dataY, 1);
    dataY += 12;

    sprintf(buf, "x2.0: %.2fV", state.batteryVoltage);
    heartSprite.drawString(buf, col2X, dataY, 1);
    dataY += 12;

    // Trend
    float diff = state.batteryVoltage - state.batteryPrevVoltage;
    if (diff > 0.01f) {
        heartSprite.setTextColor(COLOR_SUCCESS, TFT_BLACK);
        heartSprite.drawString("^ RISING", col2X, dataY, 1);
    } else if (diff < -0.01f) {
        heartSprite.setTextColor(COLOR_ERROR, TFT_BLACK);
        heartSprite.drawString("v FALLING", col2X, dataY, 1);
    } else {
        heartSprite.setTextColor(COLOR_MUTED, TFT_BLACK);
        heartSprite.drawString("- STABLE", col2X, dataY, 1);
    }

    // ============ RIGHT COLUMN: Min/Max + Predictions ============
    int col3X = 190;
    dataY = 28;

    sprintf(buf, "MIN: %.2fV", state.batteryMinVoltage < 50 ? state.batteryMinVoltage : 0);
    heartSprite.setTextColor(COLOR_ACCENT, TFT_BLACK);
    heartSprite.drawString(buf, col3X, dataY, 1);
    dataY += 12;

    sprintf(buf, "MAX: %.2fV", state.batteryMaxVoltage);
    heartSprite.setTextColor(COLOR_SUCCESS, TFT_BLACK);
    heartSprite.drawString(buf, col3X, dataY, 1);
    dataY += 16;

    // Rate
    float rate = getVoltageChangeRate();
    sprintf(buf, "RATE: %.2f V/h", rate);
    heartSprite.setTextColor(rate > 0 ? COLOR_SUCCESS : (rate < 0 ? COLOR_ORANGE : COLOR_MUTED), TFT_BLACK);
    heartSprite.drawString(buf, col3X, dataY, 1);
    dataY += 12;

    // Time estimate
    int estMins = getEstimatedMinutes();
    if (estMins >= 0) {
        int hrs = estMins / 60;
        int mins = estMins % 60;
        if (rate > 0) {
            sprintf(buf, "FULL: %dh%dm", hrs, mins);
        } else {
            sprintf(buf, "EMPTY: %dh%dm", hrs, mins);
        }
        heartSprite.setTextColor(TFT_WHITE, TFT_BLACK);
    } else {
        strcpy(buf, "EST: ...");
        heartSprite.setTextColor(COLOR_MUTED, TFT_BLACK);
    }
    heartSprite.drawString(buf, col3X, dataY, 1);

    // ============ BOTTOM: History Graph ============
    heartSprite.drawFastHLine(0, 88, W, COLOR_MUTED);

    heartSprite.setTextColor(COLOR_MUTED, TFT_BLACK);
    heartSprite.setTextDatum(TL_DATUM);
    heartSprite.drawString("HISTORY (1hr):", 8, 92, 1);

    // Draw bar graph - wider in landscape
    int graphX = 8;
    int graphY = 105;
    int graphW = W - 16;
    int graphH = 45;
    int barW = graphW / BATTERY_HISTORY_SIZE;
    if (barW < 2) barW = 2;

    // Find min/max in history for scaling
    float histMin = 3.0f, histMax = 4.2f;
    for (int i = 0; i < batteryHistoryCount; i++) {
        if (batteryHistory[i].voltage < histMin) histMin = batteryHistory[i].voltage;
        if (batteryHistory[i].voltage > histMax) histMax = batteryHistory[i].voltage;
    }
    float histRange = histMax - histMin;
    if (histRange < 0.1f) histRange = 0.1f;

    // Draw bars from oldest to newest
    for (int i = 0; i < batteryHistoryCount; i++) {
        int idx = (batteryHistoryIndex - batteryHistoryCount + i + BATTERY_HISTORY_SIZE) % BATTERY_HISTORY_SIZE;
        float v = batteryHistory[idx].voltage;
        int barH = (int)(((v - histMin) / histRange) * graphH);
        if (barH < 1) barH = 1;

        uint16_t barColor = batteryHistory[idx].usbPower ? COLOR_SUCCESS : COLOR_ORANGE;
        int bx = graphX + (i * barW);
        heartSprite.fillRect(bx, graphY + graphH - barH, barW - 1, barH, barColor);
    }

    // Draw outline
    heartSprite.drawRect(graphX - 1, graphY - 1, graphW + 2, graphH + 2, COLOR_MUTED);

    // ============ RIGHT EDGE: Thin two-color button indicator bar ============
    heartSprite.fillRect(W - 4, 0, 4, H / 2, COLOR_HEART);
    heartSprite.fillRect(W - 4, H / 2, 4, H / 2, COLOR_ACCENT);

    // Push to display
    heartSprite.pushSprite(0, 0);
}

// ============== Battery V2 - Improved Logic ==============

// Get battery percentage using lookup table (interpolated)
int getBatteryPercentFromCurve(float voltage) {
    // Above max = 100%
    if (voltage >= BATTERY_CURVE[0].voltage) return 100;
    // Below min = 0%
    if (voltage <= BATTERY_CURVE[BATTERY_CURVE_SIZE - 1].voltage) return 0;

    // Find the two points to interpolate between
    for (int i = 0; i < BATTERY_CURVE_SIZE - 1; i++) {
        if (voltage <= BATTERY_CURVE[i].voltage && voltage > BATTERY_CURVE[i + 1].voltage) {
            // Linear interpolation between these two points
            float vHigh = BATTERY_CURVE[i].voltage;
            float vLow = BATTERY_CURVE[i + 1].voltage;
            int pHigh = BATTERY_CURVE[i].percent;
            int pLow = BATTERY_CURVE[i + 1].percent;

            float ratio = (voltage - vLow) / (vHigh - vLow);
            return pLow + (int)(ratio * (pHigh - pLow));
        }
    }
    return 0;
}

void drawBatteryViewV2() {
    if (!spriteValid) {
        tft.fillScreen(TFT_RED);
        tft.setTextColor(TFT_WHITE);
        tft.setTextDatum(MC_DATUM);
        tft.drawString("Sprite Error!", 160, 85, 4);
        return;
    }

    heartSprite.fillSprite(TFT_BLACK);

    const int W = 320;
    const int H = 170;
    char buf[64];

    // Detect charging state (USB connected = can't read true battery)
    bool isCharging = state.batteryVoltage > BATTERY_USB_THRESHOLD;

    // Calculate "real" percentage only when on battery
    int realPercent = isCharging ? -1 : getBatteryPercentFromCurve(state.batteryVoltage);

    // ============ HEADER ============
    heartSprite.setTextColor(TFT_WHITE, TFT_BLACK);
    heartSprite.setTextDatum(TL_DATUM);
    heartSprite.drawString("BATTERY V2", 8, 4, 2);

    heartSprite.setTextDatum(TR_DATUM);
    if (isCharging) {
        heartSprite.setTextColor(COLOR_SUCCESS, TFT_BLACK);
        heartSprite.drawString("CHARGING", W - 8, 4, 2);
    } else {
        heartSprite.setTextColor(COLOR_ORANGE, TFT_BLACK);
        heartSprite.drawString("ON BATTERY", W - 8, 4, 2);
    }
    heartSprite.drawFastHLine(0, 22, W, COLOR_MUTED);

    // ============ MAIN DISPLAY ============

    if (isCharging) {
        // CHARGING STATE: Show big charging indicator
        heartSprite.setTextDatum(MC_DATUM);
        heartSprite.setTextColor(COLOR_SUCCESS, TFT_BLACK);
        heartSprite.drawString("USB POWER", W/2, 50, 4);

        heartSprite.setTextColor(COLOR_MUTED, TFT_BLACK);
        heartSprite.drawString("Battery % unknown while charging", W/2, 80, 2);

        sprintf(buf, "Reading: %.2fV (charge voltage)", state.batteryVoltage);
        heartSprite.drawString(buf, W/2, 100, 2);

        // Show what the voltage WILL be when unplugged (estimate)
        heartSprite.setTextColor(TFT_WHITE, TFT_BLACK);
        heartSprite.drawString("Unplug to see true battery level", W/2, 125, 2);

    } else {
        // ON BATTERY: Show real percentage with big display

        // Large percentage
        heartSprite.setTextDatum(MC_DATUM);
        uint16_t pctColor = realPercent > 50 ? COLOR_SUCCESS :
                           (realPercent > 20 ? COLOR_ORANGE : COLOR_ERROR);
        heartSprite.setTextColor(pctColor, TFT_BLACK);
        sprintf(buf, "%d%%", realPercent);
        heartSprite.drawString(buf, W/2 - 60, 60, 7);  // Big font

        // Voltage next to it
        heartSprite.setTextColor(COLOR_MUTED, TFT_BLACK);
        heartSprite.setTextDatum(TL_DATUM);
        sprintf(buf, "%.2fV", state.batteryVoltage);
        heartSprite.drawString(buf, W/2 + 40, 45, 4);

        // Battery bar
        int barX = 30;
        int barY = 100;
        int barW = W - 60;
        int barH = 25;
        heartSprite.drawRect(barX, barY, barW, barH, COLOR_MUTED);
        int fillW = (barW - 4) * realPercent / 100;
        heartSprite.fillRect(barX + 2, barY + 2, fillW, barH - 4, pctColor);

        // Time estimate (only show if we have enough history and draining)
        float rate = getVoltageChangeRate();
        int estMins = getEstimatedMinutes();

        heartSprite.setTextDatum(MC_DATUM);
        if (rate < -0.01f && estMins > 0 && estMins < 1440) {  // Draining, valid estimate
            int hrs = estMins / 60;
            int mins = estMins % 60;
            heartSprite.setTextColor(TFT_WHITE, TFT_BLACK);
            sprintf(buf, "~%dh %dm remaining", hrs, mins);
            heartSprite.drawString(buf, W/2, 140, 2);
        } else if (rate > 0.01f) {
            heartSprite.setTextColor(COLOR_MUTED, TFT_BLACK);
            heartSprite.drawString("Voltage rising (settling)", W/2, 140, 2);
        } else {
            heartSprite.setTextColor(COLOR_MUTED, TFT_BLACK);
            heartSprite.drawString("Collecting data...", W/2, 140, 2);
        }
    }

    // ============ COMPARISON: Old vs New % ============
    heartSprite.drawFastHLine(0, 155, W, COLOR_MUTED);
    heartSprite.setTextDatum(TL_DATUM);
    heartSprite.setTextColor(COLOR_MUTED, TFT_BLACK);

    // Show both calculations for comparison
    sprintf(buf, "V1(linear):%d%%  V2(curve):%d%%",
            state.batteryPercent, isCharging ? -1 : realPercent);
    heartSprite.drawString(buf, 8, 158, 1);

    // Button indicator
    heartSprite.fillRect(W - 4, 0, 4, H / 2, COLOR_HEART);
    heartSprite.fillRect(W - 4, H / 2, 4, H / 2, COLOR_ACCENT);

    heartSprite.pushSprite(0, 0);
}

// Obsolete screen functions removed - now using drawDebugScreen() and drawKayciScreen()

// ============== Heart Animation Functions ==============

void setupHeartSprite() {
    heartSprite.setColorDepth(16);
    // Create in landscape dimensions (both views use landscape)
    void* ptr = heartSprite.createSprite(SCREEN_WIDTH_LANDSCAPE, SCREEN_HEIGHT_LANDSCAPE);
    if (ptr != nullptr) {
        spriteValid = true;
        heartSprite.fillSprite(TFT_BLACK);
        Serial.printf("[Display] Sprite created OK (320x170, ptr=%p)\n", ptr);
    } else {
        spriteValid = false;
        Serial.println("[Display] ERROR: Failed to create sprite!");
    }
}

void drawHeart(TFT_eSprite &spr, int cx, int cy, int size, uint16_t color) {
    // Heart shape: 2 circles for lobes + triangle for bottom
    int r = size;
    int lobeOffset = r * 0.7;
    int lobeY = cy - r * 0.2;
    int bottomY = cy + r * 1.5;
    int edgeY = cy + r * 0.3;

    // Fill the two upper lobes
    spr.fillCircle(cx - lobeOffset, lobeY, r, color);
    spr.fillCircle(cx + lobeOffset, lobeY, r, color);

    // Fill the bottom triangle
    spr.fillTriangle(cx - r * 1.4, edgeY, cx + r * 1.4, edgeY, cx, bottomY, color);

    // Fill gap between circles and triangle
    spr.fillRect(cx - lobeOffset, lobeY, lobeOffset * 2, edgeY - lobeY + 2, color);
}

void initParticles() {
    for (int i = 0; i < MAX_PARTICLES; i++) {
        particles[i].life = 0;
    }
}

void spawnParticle(float x, float y, float vx, float vy, float scale, uint16_t color) {
    for (int i = 0; i < MAX_PARTICLES; i++) {
        if (particles[i].life == 0) {
            particles[i].x = x;
            particles[i].y = y;
            particles[i].vx = vx;
            particles[i].vy = vy;
            particles[i].scale = scale;
            particles[i].color = color;
            particles[i].life = 100 + random(50);
            return;
        }
    }
}

void updateParticles() {
    // Use correct dimensions based on orientation
    int maxX = isLandscape ? SCREEN_WIDTH_LANDSCAPE : SCREEN_WIDTH;
    int maxY = isLandscape ? SCREEN_HEIGHT_LANDSCAPE : SCREEN_HEIGHT;

    for (int i = 0; i < MAX_PARTICLES; i++) {
        if (particles[i].life > 0) {
            particles[i].x += particles[i].vx;
            particles[i].y += particles[i].vy;
            particles[i].vy += 0.15;  // Gravity
            particles[i].life--;

            // Remove if off screen
            if (particles[i].y > maxY + 30 ||
                particles[i].x < -30 || particles[i].x > maxX + 30) {
                particles[i].life = 0;
            }
        }
    }
}

void startHeartAnimation(int type, const char* message) {
    if (!spriteValid) {
        Serial.println("[Animation] Cannot start - sprite invalid");
        return;
    }
    heartAnim.type = (AnimationType)type;
    heartAnim.endTime = millis() + 10000;  // 10 second duration
    heartAnim.lastFrame = 0;
    heartAnim.phase = 0;
    heartAnim.burstScale = 0.1;
    heartAnim.burstExploded = false;
    heartAnim.previousScreen = currentScreen;

    // Set custom message or use default
    if (message && strlen(message) > 0) {
        strncpy(heartAnim.message, message, sizeof(heartAnim.message) - 1);
        heartAnim.message[sizeof(heartAnim.message) - 1] = '\0';
    } else {
        strncpy(heartAnim.message, defaultMessages[type], sizeof(heartAnim.message) - 1);
        heartAnim.message[sizeof(heartAnim.message) - 1] = '\0';
    }

    initParticles();
    currentScreen = 3;  // Heart animation screen

    Serial.printf("[Animation] Started type %d with message: %s\n", type, heartAnim.message);
}

void stopHeartAnimation() {
    heartAnim.type = ANIM_NONE;
    currentScreen = heartAnim.previousScreen;
    displayNeedsUpdate = true;
    Serial.println("[Animation] Stopped");
}

void renderPulsingHeart() {
    if (!spriteValid) return;

    int w = isLandscape ? SCREEN_WIDTH_LANDSCAPE : SCREEN_WIDTH;
    int h = isLandscape ? SCREEN_HEIGHT_LANDSCAPE : SCREEN_HEIGHT;

    heartSprite.fillSprite(TFT_BLACK);

    // Calculate scale using sine wave for smooth pulse
    float scale = 1.0 + 0.25 * sin(heartAnim.phase);
    int baseSize = 35;
    int size = (int)(baseSize * scale);

    int cx = w / 2;
    int cy = h / 2 - 20;

    // Outer glow (slightly larger, lighter)
    drawHeart(heartSprite, cx, cy, size + 6, COLOR_HEART_GLOW);

    // Main heart
    drawHeart(heartSprite, cx, cy, size, COLOR_HEART);

    // Inner highlight
    drawHeart(heartSprite, cx - 4, cy - 4, size / 3, COLOR_LIGHT_PINK);

    // Text below (custom or default)
    heartSprite.setTextColor(COLOR_PINK);
    heartSprite.setTextDatum(MC_DATUM);
    heartSprite.drawString(heartAnim.message, cx, cy + 60, 2);

    heartSprite.pushSprite(0, 0);
}

void renderHeartShower() {
    if (!spriteValid) return;

    int w = isLandscape ? SCREEN_WIDTH_LANDSCAPE : SCREEN_WIDTH;
    int h = isLandscape ? SCREEN_HEIGHT_LANDSCAPE : SCREEN_HEIGHT;

    heartSprite.fillSprite(COLOR_BG);

    // Spawn new hearts at random positions at top
    if (random(100) < 35) {  // 35% chance each frame
        float x = random(w);
        float vx = (random(100) - 50) * 0.015;  // Slight drift
        float vy = 1.5 + random(100) * 0.02;     // Downward speed
        float scale = 0.4 + random(60) * 0.01;   // Size variation

        uint16_t colors[] = { COLOR_HEART, COLOR_PINK, COLOR_LIGHT_PINK, COLOR_HEART_GLOW };
        uint16_t color = colors[random(4)];

        spawnParticle(x, -20, vx, vy, scale, color);
    }

    updateParticles();

    // Draw all particles
    for (int i = 0; i < MAX_PARTICLES; i++) {
        if (particles[i].life > 0) {
            int size = (int)(12 * particles[i].scale);

            // Fade based on remaining life
            uint16_t color = particles[i].color;
            if (particles[i].life < 30) {
                // Dim by using darker shade
                uint8_t r = ((color >> 11) & 0x1F) * particles[i].life / 30;
                uint8_t g = ((color >> 5) & 0x3F) * particles[i].life / 30;
                uint8_t b = (color & 0x1F) * particles[i].life / 30;
                color = (r << 11) | (g << 5) | b;
            }

            drawHeart(heartSprite, (int)particles[i].x, (int)particles[i].y, size, color);
        }
    }

    // Title at bottom (custom or default)
    heartSprite.setTextColor(COLOR_TEXT);
    heartSprite.setTextDatum(MC_DATUM);
    heartSprite.drawString(heartAnim.message, w / 2, h - 30, 2);

    heartSprite.pushSprite(0, 0);
}

void renderHeartBurst() {
    if (!spriteValid) return;

    int w = isLandscape ? SCREEN_WIDTH_LANDSCAPE : SCREEN_WIDTH;
    int h = isLandscape ? SCREEN_HEIGHT_LANDSCAPE : SCREEN_HEIGHT;

    heartSprite.fillSprite(COLOR_BG);

    int cx = w / 2;
    int cy = h / 2 - 20;

    if (!heartAnim.burstExploded) {
        // Growing phase
        heartAnim.burstScale += 0.08;
        int size = (int)(15 * heartAnim.burstScale);

        // Shake effect near explosion
        int shake = 0;
        if (heartAnim.burstScale > 2.5) {
            shake = random(6) - 3;
        }

        // Draw growing heart with glow
        drawHeart(heartSprite, cx + shake, cy + shake, size + 4, COLOR_HEART_GLOW);
        drawHeart(heartSprite, cx + shake, cy + shake, size, COLOR_HEART);

        // Explode when max size reached
        if (heartAnim.burstScale >= 3.0) {
            heartAnim.burstExploded = true;

            // Spawn explosion particles in all directions
            int numParticles = 20;
            for (int i = 0; i < numParticles; i++) {
                float angle = (2.0 * PI * i) / numParticles + (random(100) * 0.005);
                float speed = 3.0 + random(100) * 0.03;
                float vx = cos(angle) * speed;
                float vy = sin(angle) * speed;
                float scale = 0.25 + random(40) * 0.01;

                uint16_t colors[] = { COLOR_HEART, COLOR_PINK, COLOR_LIGHT_PINK, COLOR_HEART_GLOW };
                spawnParticle(cx, cy, vx, vy, scale, colors[random(4)]);
            }
        }
    } else {
        // Explosion phase - update and draw particles
        updateParticles();

        for (int i = 0; i < MAX_PARTICLES; i++) {
            if (particles[i].life > 0) {
                int size = (int)(10 * particles[i].scale);
                drawHeart(heartSprite, (int)particles[i].x, (int)particles[i].y, size, particles[i].color);
            }
        }
    }

    // Text (custom or default)
    heartSprite.setTextColor(COLOR_PINK);
    heartSprite.setTextDatum(MC_DATUM);
    heartSprite.drawString(heartAnim.message, cx, h - 30, 4);

    heartSprite.pushSprite(0, 0);
}

void renderSendingHeart() {
    // Safety check
    if (!spriteValid) return;

    int w = SCREEN_WIDTH_LANDSCAPE;
    int h = SCREEN_HEIGHT_LANDSCAPE;

    heartSprite.fillSprite(TFT_BLACK);

    int cy = h / 2;

    // Calculate progress (0 to 1 over 1.2 seconds)
    unsigned long elapsed = millis() - (heartAnim.endTime - 1200);
    float progress = constrain((float)elapsed / 1200.0f, 0.0f, 1.0f);

    // Easing function - starts slow, accelerates
    float eased = progress * progress;

    // Heart starts at center, flies to right edge
    float startX = w / 3;
    float endX = w + 40;
    float heartX = startX + (endX - startX) * eased;

    // Pulsing effect while flying
    float pulse = 1.0 + 0.15 * sin(progress * 12);

    // Scale shrinks as it exits (1.0 -> 0.4)
    float scale = (1.0 - (0.6 * eased)) * pulse;
    int heartSize = (int)(28 * scale);

    // Draw trailing mini hearts (fade behind main heart)
    if (progress < 0.8) {
        for (int i = 3; i >= 1; i--) {
            float trailX = heartX - (i * 35);
            if (trailX > 0 && trailX < w) {
                int trailSize = heartSize / (i + 1);
                uint8_t fade = 255 / (i + 1);
                // Dimmed pink trails
                uint16_t trailColor = ((fade >> 3) << 11) | ((fade >> 4) << 5) | (fade >> 3);
                drawHeart(heartSprite, (int)trailX, cy, trailSize, trailColor);
            }
        }
    }

    // Draw main heart if still on screen
    if (heartX < w + 30) {
        drawHeart(heartSprite, (int)heartX, cy, heartSize + 3, COLOR_PINK);
        drawHeart(heartSprite, (int)heartX, cy, heartSize, COLOR_HEART);
    }

    // Show "love sent" text as heart exits
    if (progress > 0.6) {
        float textFade = (progress - 0.6) / 0.4;  // 0 to 1
        heartSprite.setTextColor(COLOR_PINK, TFT_BLACK);
        heartSprite.setTextDatum(MC_DATUM);
        heartSprite.drawString("love sent", w / 2, h / 2, 4);
    }

    heartSprite.pushSprite(0, 0);
}

void startSendAnimation() {
    if (!spriteValid) {
        Serial.println("[Animation] Cannot start send - sprite invalid");
        return;
    }
    heartAnim.type = ANIM_SEND;
    heartAnim.endTime = millis() + 1200;  // 1.2 second duration
    heartAnim.lastFrame = 0;
    heartAnim.previousScreen = currentScreen;
    heartAnim.sendX = isLandscape ? SCREEN_WIDTH_LANDSCAPE / 2 : SCREEN_WIDTH / 2;
    heartAnim.sendScale = 1.0;
    strncpy(heartAnim.message, "Sent!", sizeof(heartAnim.message) - 1);

    // No particle initialization - simple heart animation only
    currentScreen = 2;  // Animation screen

    Serial.println("[Animation] Started send animation");
}

void updateHeartAnimation() {
    // Guard: if sprite isn't valid, stop animation
    if (!spriteValid) {
        stopHeartAnimation();
        return;
    }

    // Check if animation should end
    if (millis() >= heartAnim.endTime) {
        stopHeartAnimation();
        return;
    }

    // Frame rate control (~30 FPS)
    if (millis() - heartAnim.lastFrame < 33) {
        return;
    }
    heartAnim.lastFrame = millis();

    // Update phase for animations
    heartAnim.phase += 0.15;

    // Render based on animation type
    switch (heartAnim.type) {
        case ANIM_PULSE:
            renderPulsingHeart();
            break;
        case ANIM_SHOWER:
            renderHeartShower();
            break;
        case ANIM_BURST:
            renderHeartBurst();
            break;
        case ANIM_SEND:
            renderSendingHeart();
            break;
        default:
            break;
    }
}

// ============== NTP and Sun Calculation Functions ==============

void setupNTP() {
    configTzTime(TZ_INFO, NTP_SERVER1, NTP_SERVER2);
    Serial.println("[NTP] Time sync configured for Central Time");

    // Wait up to 5 seconds for time sync
    struct tm timeinfo;
    for (int i = 0; i < 10; i++) {
        if (getLocalTime(&timeinfo, 500)) {
            ntpSynced = true;
            Serial.printf("[NTP] Time synced: %02d:%02d:%02d\n",
                          timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
            break;
        }
        delay(500);
    }

    if (!ntpSynced) {
        Serial.println("[NTP] Initial sync failed, will retry in background");
    }

    // Calculate sunrise/sunset times
    calculateSunTimes();
}

// Sunrise/Sunset calculation using simplified NOAA algorithm
void calculateSunTimes() {
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo)) {
        return;  // Can't calculate without current date
    }

    // Day of year (1-365)
    int N = timeinfo.tm_yday + 1;

    // Fractional year (radians)
    double gamma = 2.0 * PI * (N - 1 + (12.0 - 12.0) / 24.0) / 365.0;

    // Equation of time (minutes)
    double eqtime = 229.18 * (0.000075 + 0.001868 * cos(gamma) - 0.032077 * sin(gamma)
                             - 0.014615 * cos(2.0 * gamma) - 0.040849 * sin(2.0 * gamma));

    // Solar declination (radians)
    double decl = 0.006918 - 0.399912 * cos(gamma) + 0.070257 * sin(gamma)
                  - 0.006758 * cos(2.0 * gamma) + 0.000907 * sin(2.0 * gamma)
                  - 0.002697 * cos(3.0 * gamma) + 0.00148 * sin(3.0 * gamma);

    // Hour angle (degrees)
    double latRad = LOCATION_LAT * PI / 180.0;
    double ha = acos(cos(90.833 * PI / 180.0) / (cos(latRad) * cos(decl))
                     - tan(latRad) * tan(decl)) * 180.0 / PI;

    // Sunrise and sunset in minutes from midnight UTC
    double sunriseUTC = 720 - 4 * (LOCATION_LON + ha) - eqtime;
    double sunsetUTC = 720 - 4 * (LOCATION_LON - ha) - eqtime;

    // Convert to local time (CST = UTC-6, CDT = UTC-5)
    // We use the DST offset from the current time struct
    int tzOffset = (timeinfo.tm_isdst > 0) ? -5 * 60 : -6 * 60;  // minutes

    double sunriseLocal = sunriseUTC + tzOffset;
    double sunsetLocal = sunsetUTC + tzOffset;

    // Handle day boundary
    if (sunriseLocal < 0) sunriseLocal += 1440;
    if (sunsetLocal < 0) sunsetLocal += 1440;
    if (sunriseLocal >= 1440) sunriseLocal -= 1440;
    if (sunsetLocal >= 1440) sunsetLocal -= 1440;

    sunriseHour = (int)(sunriseLocal / 60);
    sunriseMin = (int)sunriseLocal % 60;
    sunsetHour = (int)(sunsetLocal / 60);
    sunsetMin = (int)sunsetLocal % 60;

    lastSunCalc = millis();

    Serial.printf("[Sun] Sunrise: %02d:%02d, Sunset: %02d:%02d (day %d)\n",
                  sunriseHour, sunriseMin, sunsetHour, sunsetMin, N);
}

// Update screen brightness based on time schedule
void updateBrightness() {
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo, 0)) {
        return;  // Can't check without time
    }

    int hour = timeinfo.tm_hour;
    bool shouldDim = false;

    // Check if current hour is within dim period
    // Handle wrap-around (e.g., 1 AM to 7 AM, or 22:00 to 6:00)
    if (config.dimStartHour < config.dimEndHour) {
        // Simple case: e.g., 1 to 7
        shouldDim = (hour >= config.dimStartHour && hour < config.dimEndHour);
    } else {
        // Wrap-around case: e.g., 22 to 6
        shouldDim = (hour >= config.dimStartHour || hour < config.dimEndHour);
    }

    int targetBrightness = shouldDim ? config.dimBrightness : config.normalBrightness;

    // Only update if brightness changed
    if (currentBrightness != targetBrightness) {
        currentBrightness = targetBrightness;
        isDimmed = shouldDim;
        ledcWrite(TFT_BACKLIGHT, currentBrightness);
        Serial.printf("[Display] Brightness %s: %d (hour=%d)\n",
                      shouldDim ? "dimmed" : "normal", currentBrightness, hour);
    }
}

// Draw the Tiny Chef at given position (based on the real character)
// Tiny Chef is a green bean/pickle shaped creature with chef hat, rainbow apron, blue oven mitts
void drawChef(TFT_eSprite &spr, int x, int y, int frame, bool facingRight) {
    // Sprite is about 40x50 pixels
    int bobY = (frame % 2 == 0) ? 0 : -2;  // Bounce on even frames

    // Mitt wave offset based on frame
    int leftMittY = 0, rightMittY = 0;
    switch (frame) {
        case 0: leftMittY = 0; rightMittY = 2; break;
        case 1: leftMittY = -3; rightMittY = -1; break;
        case 2: leftMittY = 2; rightMittY = 0; break;
        case 3: leftMittY = -1; rightMittY = -3; break;
    }

    int cx = x;
    int cy = y + bobY;

    // === Chef Hat (tall puffy toque) ===
    // Hat puff (top part - rounded)
    spr.fillCircle(cx, cy - 38, 10, COLOR_CHEF_HAT);
    spr.fillCircle(cx - 6, cy - 34, 8, COLOR_CHEF_HAT);
    spr.fillCircle(cx + 6, cy - 34, 8, COLOR_CHEF_HAT);
    spr.fillRect(cx - 10, cy - 34, 20, 10, COLOR_CHEF_HAT);
    // Hat band (slightly darker line)
    spr.drawFastHLine(cx - 8, cy - 24, 16, COLOR_MUTED);

    // === Green Body (pickle/bean shape) ===
    // Head area (rounded top)
    spr.fillCircle(cx, cy - 16, 9, COLOR_CHEF_BODY);
    // Body (oval/bean shape going down)
    spr.fillEllipse(cx, cy, 10, 16, COLOR_CHEF_BODY);

    // === Face ===
    // Eyes (simple black dots)
    spr.fillCircle(cx - 3, cy - 18, 2, 0x0000);
    spr.fillCircle(cx + 3, cy - 18, 2, 0x0000);
    // Smile (small curved line)
    spr.drawPixel(cx - 2, cy - 13, 0x0000);
    spr.drawPixel(cx - 1, cy - 12, 0x0000);
    spr.drawPixel(cx, cy - 12, 0x0000);
    spr.drawPixel(cx + 1, cy - 12, 0x0000);
    spr.drawPixel(cx + 2, cy - 13, 0x0000);

    // === Rainbow Striped Apron ===
    int apronTop = cy - 6;
    int apronWidth = 14;
    int stripeH = 3;
    // Draw vertical stripes on the apron
    uint16_t stripeColors[] = {COLOR_APRON_RED, COLOR_APRON_ORG, COLOR_APRON_YEL,
                                COLOR_APRON_GRN, COLOR_APRON_BLU, COLOR_APRON_RED, COLOR_APRON_ORG};
    for (int i = 0; i < 7; i++) {
        spr.fillRect(cx - 7 + (i * 2), apronTop, 2, 14, stripeColors[i]);
    }
    // Apron strings at top (small ties)
    spr.drawFastHLine(cx - 9, apronTop, 3, COLOR_APRON_RED);
    spr.drawFastHLine(cx + 6, apronTop, 3, COLOR_APRON_RED);

    // === Blue Oven Mitts ===
    // Left mitt
    int lmx = cx - 16;
    int lmy = cy - 4 + leftMittY;
    spr.fillRoundRect(lmx, lmy, 10, 14, 3, COLOR_CHEF_MITT);
    // Dots on mitt
    spr.fillCircle(lmx + 3, lmy + 4, 1, COLOR_MITT_DOT);
    spr.fillCircle(lmx + 6, lmy + 7, 1, COLOR_MITT_DOT);
    spr.fillCircle(lmx + 3, lmy + 10, 1, COLOR_MITT_DOT);

    // Right mitt
    int rmx = cx + 6;
    int rmy = cy - 4 + rightMittY;
    spr.fillRoundRect(rmx, rmy, 10, 14, 3, COLOR_CHEF_MITT);
    // Dots on mitt
    spr.fillCircle(rmx + 3, rmy + 4, 1, COLOR_MITT_DOT);
    spr.fillCircle(rmx + 6, rmy + 7, 1, COLOR_MITT_DOT);
    spr.fillCircle(rmx + 3, rmy + 10, 1, COLOR_MITT_DOT);

    // === Legs (small green stubs at bottom) ===
    int legSpread = (frame % 2 == 0) ? 2 : 0;
    spr.fillRect(cx - 5 - legSpread, cy + 14, 4, 6, COLOR_CHEF_BODY);
    spr.fillRect(cx + 1 + legSpread, cy + 14, 4, 6, COLOR_CHEF_BODY);
}

void updateDisplay() {
    // Debug screen (0) updates every second for status
    // Kayci screen (1) needs frequent updates for chef animation (~6 FPS = 166ms)
    unsigned long updateInterval;
    if (currentScreen == 0) {
        updateInterval = 1000;  // Debug view - 1 second refresh
    } else if (currentScreen == 1) {
        updateInterval = 166;   // Kayci view - 6 FPS for chef animation
    } else {
        updateInterval = 5000;  // Fallback
    }

    if (!displayNeedsUpdate && millis() - lastDisplayUpdate < updateInterval) {
        return;
    }

    static unsigned long lastDebugPrint = 0;
    if (millis() - lastDebugPrint > 5000) {
        Serial.printf("[Display] screen=%d, interval=%lu, spriteValid=%d\n",
                      currentScreen, updateInterval, spriteValid);
        lastDebugPrint = millis();
    }

    lastDisplayUpdate = millis();
    displayNeedsUpdate = false;

    switch (currentScreen) {
        case 0:
            drawDebugScreen();
            break;
        case 1:
            drawKayciScreen();
            break;
        case 4:
            drawBatteryDebugScreen();
            break;
        case 5:
            drawBatteryViewV2();
            break;
        // Screen 2, 3 are animations - handled by updateHeartAnimation()
    }
}

void handleButtons() {
    bool buttonLeft = digitalRead(BUTTON_LEFT);
    bool buttonRight = digitalRead(BUTTON_RIGHT);

    // If animation is playing, any button press dismisses it
    if ((currentScreen == 2 || currentScreen == 3) && heartAnim.type != ANIM_NONE) {
        if ((buttonLeft == HIGH && lastButtonLeft == LOW) ||
            (buttonRight == HIGH && lastButtonRight == LOW)) {
            stopHeartAnimation();
        }
        lastButtonLeft = buttonLeft;
        lastButtonRight = buttonRight;
        return;
    }

    // Left button - cycle through views: Debug (0) -> Battery V1 (4) -> Battery V2 (5) -> Kayci (1) -> Debug (0)
    if (buttonLeft == LOW && lastButtonLeft == HIGH) {
        buttonLeftPressTime = millis();
        Serial.println("[Button] Left pressed");
    }
    if (buttonLeft == HIGH && lastButtonLeft == LOW) {
        Serial.printf("[Button] Left released, duration=%lums, screen=%d, spriteValid=%d\n",
                      millis() - buttonLeftPressTime, currentScreen, spriteValid);
        if (millis() - buttonLeftPressTime < 500) {  // Short press
            if (currentScreen == 0) {
                Serial.println("[Button] Switching to Battery V1 view");
                switchToBatteryDebugView();  // Debug -> Battery V1
            } else if (currentScreen == 4) {
                Serial.println("[Button] Switching to Battery V2 view");
                currentScreen = 5;  // Battery V1 -> Battery V2
                displayNeedsUpdate = true;
            } else if (currentScreen == 5) {
                Serial.println("[Button] Switching to Kayci view");
                switchToKayciView();  // Battery V2 -> Kayci
            } else if (currentScreen == 1) {
                Serial.println("[Button] Switching to Debug view");
                switchToDebugView();  // Kayci -> Debug
            }
        }
    }

    // Right button - always ping companion bee + show send animation
    if (buttonRight == LOW && lastButtonRight == HIGH) {
        buttonRightPressTime = millis();
    }
    if (buttonRight == HIGH && lastButtonRight == LOW) {
        if (millis() - buttonRightPressTime < 500) {  // Short press
            // Ping companion bee and show animation
            pingTinyBee();
            startSendAnimation();
        }
    }

    // Long press right button (3 sec) - reset WiFi
    if (buttonRight == LOW && millis() - buttonRightPressTime > 3000) {
        // Use current dimensions based on orientation
        int w = isLandscape ? SCREEN_WIDTH_LANDSCAPE : SCREEN_WIDTH;
        int h = isLandscape ? SCREEN_HEIGHT_LANDSCAPE : SCREEN_HEIGHT;
        tft.fillScreen(COLOR_ERROR);
        tft.setTextColor(COLOR_TEXT);
        tft.setTextDatum(MC_DATUM);
        tft.drawString("Resetting WiFi...", w/2, h/2, 4);
        delay(1000);
        wifiManager.resetSettings();
        ESP.restart();
    }

    lastButtonLeft = buttonLeft;
    lastButtonRight = buttonRight;
}

// ============== MQTT Callbacks (Async) ==============

void onMqttConnect(bool sessionPresent) {
    mqttLog("[MQTT] Connected! Session present: %d\n", sessionPresent);
    state.mqttConnected = true;
    lastConnectTime = millis();
    lastError = "none";
    displayNeedsUpdate = true;

    // Subscribe to command topic
    uint16_t packetId = mqttClient.subscribe(commandTopic.c_str(), 1);
    Serial.printf("[MQTT] Subscribed to %s (packet %d)\n", commandTopic.c_str(), packetId);

    // Subscribe to TinyBee1's availability to track its status
    String tinyBeeAvailTopic = String(config.topicPrefix) + "/devices/" + config.companionBeeId + "/availability";
    mqttClient.subscribe(tinyBeeAvailTopic.c_str(), 1);
    Serial.printf("[MQTT] Subscribed to TinyBee1 availability\n");

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
    displayNeedsUpdate = true;

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

    // Check for TinyBee1 availability (plain text, not JSON)
    String tinyBeeAvailTopic = String(config.topicPrefix) + "/devices/" + config.companionBeeId + "/availability";
    if (strcmp(topic, tinyBeeAvailTopic.c_str()) == 0) {
        char statusBuf[16];
        size_t copyLen = len < sizeof(statusBuf) - 1 ? len : sizeof(statusBuf) - 1;
        memcpy(statusBuf, payload, copyLen);
        statusBuf[copyLen] = '\0';

        bool wasOnline = state.tinyBeeOnline;
        state.tinyBeeOnline = (strcmp(statusBuf, "online") == 0);
        state.lastTinyBeeSeen = millis();

        if (state.tinyBeeOnline != wasOnline) {
            mqttLog("[TINYBEE] Status: %s\n", state.tinyBeeOnline ? "online" : "offline");
            displayNeedsUpdate = true;
        }
        return;
    }

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
            mqttLog("[LED] Set to: %s\n", state.ledOn ? "ON" : "OFF");
            displayNeedsUpdate = true;

            // Publish updated state immediately
            publishState();
        }

        // Handle love message animation trigger
        if (instance && strcmp(instance, "loveMessage") == 0) {
            // Ignore if animation already playing (prevents reset loop)
            if (heartAnim.type != ANIM_NONE) {
                mqttLog("[MQTT] Love message ignored - animation already playing\n");
                return;
            }
            int animType = capability["type"] | 1;  // Default to pulse (1)
            const char* message = capability["message"] | (const char*)nullptr;
            mqttLog("[MQTT] Love message received! Type: %d, Message: %s\n",
                          animType, message ? message : "(default)");
            startHeartAnimation(animType, message);
        }
    }
}

void onMqttPublish(uint16_t packetId) {
    // Optional: track successful publishes
}

// ============== Setup ==============

void setup() {
    Serial.begin(115200);

    // Wait for USB CDC serial connection (up to 3 seconds)
    unsigned long serialStart = millis();
    while (!Serial && millis() - serialStart < 3000) {
        delay(10);
    }
    delay(100);

    Serial.println("\n\n=== ESP32 Homecontrol (T-Display S3) ===");
    Serial.printf("Firmware: %s\n", FIRMWARE_VERSION);

    // Initialize buttons
    pinMode(BUTTON_LEFT, INPUT_PULLUP);
    pinMode(BUTTON_RIGHT, INPUT_PULLUP);

    // Initialize display
    setupDisplay();

    // Initialize heart animation sprite (uses PSRAM)
    setupHeartSprite();

    // Initialize battery ADC
    setupBattery();

    // Generate device ID
    deviceId = getDeviceId();
    Serial.printf("Device ID: %s\n", deviceId.c_str());

    // Update display with WiFi connecting message (landscape)
    tft.fillScreen(COLOR_DEBUG_BG);
    tft.setTextColor(COLOR_DEBUG_TEXT);
    tft.setTextDatum(MC_DATUM);
    tft.drawString("Connecting...", SCREEN_WIDTH_LANDSCAPE/2, SCREEN_HEIGHT_LANDSCAPE/2, 4);

    // Load configuration
    loadConfig();

    // Setup WiFi Manager
    WiFiManagerParameter mqttServerParam("mqtt_server", "MQTT Server", config.mqttServer, 64);
    WiFiManagerParameter mqttPortParam("mqtt_port", "MQTT Port", String(config.mqttPort).c_str(), 6);
    WiFiManagerParameter deviceNameParam("device_name", "Device Name", config.deviceName, 32);

    wifiManager.addParameter(&mqttServerParam);
    wifiManager.addParameter(&mqttPortParam);
    wifiManager.addParameter(&deviceNameParam);

    String apName = "SimBee-Setup-" + deviceId.substring(deviceId.length() - 4);
    wifiManager.setConfigPortalTimeout(180);

    // Show AP name on display if config portal opens (landscape)
    wifiManager.setAPCallback([](WiFiManager* wm) {
        tft.fillScreen(COLOR_DEBUG_BG);
        tft.setTextColor(COLOR_DEBUG_TEXT);
        tft.setTextDatum(MC_DATUM);
        tft.drawString("WiFi Setup", SCREEN_WIDTH_LANDSCAPE/2, 30, 4);
        tft.setTextColor(COLOR_TEXT);
        tft.drawString("Connect to:", SCREEN_WIDTH_LANDSCAPE/2, 70, 2);
        tft.setTextColor(COLOR_SUCCESS);
        tft.drawString(wm->getConfigPortalSSID(), SCREEN_WIDTH_LANDSCAPE/2, 95, 4);
        tft.setTextColor(COLOR_MUTED);
        tft.drawString("Then open 192.168.4.1", SCREEN_WIDTH_LANDSCAPE/2, 140, 2);
    });

    Serial.println("Starting WiFi Manager...");
    if (!wifiManager.autoConnect(apName.c_str())) {
        Serial.println("Failed to connect, restarting...");
        tft.fillScreen(COLOR_ERROR);
        tft.setTextColor(COLOR_TEXT);
        tft.setTextDatum(MC_DATUM);
        tft.drawString("WiFi Failed", SCREEN_WIDTH_LANDSCAPE/2, SCREEN_HEIGHT_LANDSCAPE/2, 4);
        delay(3000);
        ESP.restart();
    }

    // Save any updated parameters
    strcpy(config.mqttServer, mqttServerParam.getValue());
    config.mqttPort = atoi(mqttPortParam.getValue());
    strcpy(config.deviceName, deviceNameParam.getValue());
    saveConfig();

    mqttLog("WiFi connected! IP: %s\n", WiFi.localIP().toString().c_str());

    // Show connected status (landscape)
    tft.fillScreen(COLOR_DEBUG_BG);
    tft.setTextColor(COLOR_SUCCESS);
    tft.setTextDatum(MC_DATUM);
    tft.drawString("Connected!", SCREEN_WIDTH_LANDSCAPE/2, SCREEN_HEIGHT_LANDSCAPE/2 - 20, 4);
    tft.setTextColor(COLOR_DEBUG_TEXT);
    tft.drawString(WiFi.localIP().toString(), SCREEN_WIDTH_LANDSCAPE/2, SCREEN_HEIGHT_LANDSCAPE/2 + 15, 2);

    // Setup NTP time sync
    tft.setTextColor(COLOR_DEBUG_DIM);
    tft.drawString("Syncing time...", SCREEN_WIDTH_LANDSCAPE/2, SCREEN_HEIGHT_LANDSCAPE/2 + 40, 2);
    setupNTP();

    delay(500);

    // Setup mDNS
    String mdnsName = "simbee-" + deviceId.substring(deviceId.length() - 6);
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

    // BLE Keyboard disabled - ESP32-BLE-Keyboard library causes WiFi/display issues
    // TODO: Try raw NimBLE advertising instead
    Serial.println("[BLE] Disabled - investigating alternatives");

    // Initial display
    displayNeedsUpdate = true;

    // Set initial brightness based on time
    updateBrightness();

    Serial.println("Setup complete!\n");
}

// ============== Main Loop ==============

void loop() {
    // Update state
    state.uptime = millis() / 1000;
    state.rssi = WiFi.RSSI();

    // Read battery every 2 seconds (has internal averaging with delays)
    static unsigned long lastBatteryRead = 0;
    if (millis() - lastBatteryRead > 2000) {
        lastBatteryRead = millis();
        readBattery();
    }

    // Sample battery history for predictions (every 60 seconds)
    sampleBatteryHistory();

    // Check brightness schedule every minute
    static unsigned long lastBrightnessCheck = 0;
    if (millis() - lastBrightnessCheck > 60000) {
        lastBrightnessCheck = millis();
        updateBrightness();
    }

    // Update chef animation independently (~6 FPS) - only on Kayci screen (1)
    if (currentScreen == 1 && millis() - chefAnim.lastFrame > 166) {
        chefAnim.frame = (chefAnim.frame + 1) % 4;
        chefAnim.lastFrame = millis();

        // Occasionally change direction
        if (random(100) < 5) {
            chefAnim.facingRight = !chefAnim.facingRight;
        }
    }

    // Handle button input
    handleButtons();

    // Handle heart animation (runs at 30 FPS when active) - screen 2 or 3
    if ((currentScreen == 2 || currentScreen == 3) && heartAnim.type != ANIM_NONE) {
        updateHeartAnimation();
    } else {
        // Update normal display (debug or kayci)
        updateDisplay();
    }

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

    // Publish buffered logs to MQTT
    publishBufferedLogs();

    // Partial updates handle value changes without full redraw
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
    currentClientId = "simbee-" + deviceId + "-" + String(millis());

    mqttLog("[MQTT] Connecting... (attempt #%d)\n", reconnectCount);
    mqttClient.setClientId(currentClientId.c_str());
    mqttClient.connect();
}

void publishDiscovery() {
    JsonDocument doc;

    doc["device_id"] = deviceId;
    doc["name"] = config.deviceName;
    doc["type"] = DEVICE_TYPE;
    doc["model"] = "LILYGO T-Display S3";
    doc["firmware_version"] = FIRMWARE_VERSION;
    doc["ip_address"] = WiFi.localIP().toString();
    doc["has_display"] = true;
    doc["display_size"] = "170x320";

    JsonArray capabilities = doc["capabilities"].to<JsonArray>();
    JsonObject cap = capabilities.add<JsonObject>();
    cap["type"] = "devices.capabilities.on_off";
    cap["instance"] = "powerSwitch";

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
    doc["psram_size"] = ESP.getPsramSize();
    doc["psram_free"] = ESP.getFreePsram();
    doc["ip"] = WiFi.localIP().toString();
    doc["reconnect_count"] = reconnectCount;
    doc["last_error"] = lastError;
    doc["led_state"] = state.ledOn;
    doc["timestamp"] = millis();
    doc["client_id"] = currentClientId;
    doc["firmware"] = FIRMWARE_VERSION;
    doc["display_screen"] = currentScreen;
    // Display debug info
    doc["display_sprite_valid"] = spriteValid;
    doc["display_needs_update"] = displayNeedsUpdate;
    doc["display_last_update"] = lastDisplayUpdate;
    doc["display_draw_count"] = debugDrawCount;
    // Battery info
    doc["battery_voltage"] = state.batteryVoltage;
    doc["battery_percent"] = state.batteryPercent;
    doc["battery_charging"] = state.batteryCharging;

    String payload;
    serializeJson(doc, payload);

    mqttClient.publish(healthTopic.c_str(), 0, false, payload.c_str());
}

// ============== Battery Functions ==============

void setupBattery() {
    // Configure ADC for battery reading
    analogSetAttenuation(ADC_11db);  // Full range 0-3.3V
    analogReadResolution(12);         // 12-bit (0-4095)
    pinMode(BATTERY_ADC_PIN, INPUT);
    Serial.println("[Battery] ADC configured on GPIO4");
}

void readBattery() {
    // Read multiple samples and average for stability
    const int samples = 10;
    long total = 0;
    for (int i = 0; i < samples; i++) {
        total += analogRead(BATTERY_ADC_PIN);
        delay(2);
    }
    state.batteryRawAdc = total / samples;

    // Convert to voltage (ESP32-S3 ADC with 11db attenuation: ~0-3.3V range)
    // ADC reference is 3.3V, 12-bit resolution = 4095 steps
    state.batteryAdcVoltage = (state.batteryRawAdc / 4095.0f) * 3.3f;

    // Save previous voltage for trend detection
    state.batteryPrevVoltage = state.batteryVoltage;

    // Account for voltage divider (2:1 ratio on T-Display S3)
    state.batteryVoltage = state.batteryAdcVoltage * BATTERY_DIVIDER_RATIO;

    // Track min/max voltages (only when valid reading)
    if (state.batteryVoltage > 2.0f) {
        if (state.batteryVoltage < state.batteryMinVoltage)
            state.batteryMinVoltage = state.batteryVoltage;
        if (state.batteryVoltage > state.batteryMaxVoltage)
            state.batteryMaxVoltage = state.batteryVoltage;
    }

    // USB power detection (voltage > 4.25V indicates charging voltage passing through)
    state.batteryUsbPower = (state.batteryVoltage > 4.25f);

    // Calculate percentage (linear approximation)
    if (state.batteryVoltage < 2.0f) {
        // No battery connected or very low - USB powered only
        state.batteryPercent = -1;
    } else if (state.batteryUsbPower) {
        // On USB power - show 100% (can't determine actual battery level)
        state.batteryPercent = 100;
        state.batteryCharging = true;
    } else if (state.batteryVoltage >= BATTERY_MAX_VOLTAGE) {
        state.batteryPercent = 100;
        state.batteryCharging = false;
    } else if (state.batteryVoltage <= BATTERY_MIN_VOLTAGE) {
        state.batteryPercent = 0;
        state.batteryCharging = false;
    } else {
        state.batteryPercent = (int)((state.batteryVoltage - BATTERY_MIN_VOLTAGE) /
                                      (BATTERY_MAX_VOLTAGE - BATTERY_MIN_VOLTAGE) * 100.0f);
        state.batteryCharging = false;
    }
}

// Sample battery voltage to history buffer
void sampleBatteryHistory() {
    if (millis() - lastBatterySample < BATTERY_SAMPLE_INTERVAL) return;
    if (state.batteryVoltage < 2.0f) return;  // Skip invalid readings

    lastBatterySample = millis();

    batteryHistory[batteryHistoryIndex] = {
        millis(),
        state.batteryVoltage,
        state.batteryUsbPower
    };
    batteryHistoryIndex = (batteryHistoryIndex + 1) % BATTERY_HISTORY_SIZE;
    if (batteryHistoryCount < BATTERY_HISTORY_SIZE) batteryHistoryCount++;
}

// Calculate voltage change rate (V per hour)
float getVoltageChangeRate() {
    if (batteryHistoryCount < 5) return 0;  // Need at least 5 samples

    // Compare oldest to newest
    int oldest = (batteryHistoryIndex - batteryHistoryCount + BATTERY_HISTORY_SIZE) % BATTERY_HISTORY_SIZE;
    int newest = (batteryHistoryIndex - 1 + BATTERY_HISTORY_SIZE) % BATTERY_HISTORY_SIZE;

    float voltDiff = batteryHistory[newest].voltage - batteryHistory[oldest].voltage;
    float timeDiffHours = (batteryHistory[newest].timestamp - batteryHistory[oldest].timestamp) / 3600000.0f;

    if (timeDiffHours < 0.001f) return 0;  // Avoid division by zero
    return voltDiff / timeDiffHours;  // V/hour
}

// Estimate time to full (when charging) or empty (when discharging)
// Returns minutes, or -1 if can't estimate
int getEstimatedMinutes() {
    float rate = getVoltageChangeRate();
    if (fabsf(rate) < 0.01f) return -1;  // Too stable to predict

    if (rate > 0) {
        // Charging: time to 4.2V
        float remaining = BATTERY_MAX_VOLTAGE - state.batteryVoltage;
        if (remaining <= 0) return 0;
        return (int)((remaining / rate) * 60);
    } else {
        // Discharging: time to 3.0V
        float remaining = state.batteryVoltage - BATTERY_MIN_VOLTAGE;
        if (remaining <= 0) return 0;
        return (int)((remaining / -rate) * 60);
    }
}

void drawBatteryIcon(TFT_eSprite &spr, int x, int y) {
    // Battery dimensions
    const int w = 24;
    const int h = 12;
    const int tipW = 3;
    const int tipH = 6;

    // No battery detected
    if (state.batteryPercent < 0) {
        // Draw USB plug icon instead
        spr.setTextColor(COLOR_MUTED);
        spr.setTextDatum(TL_DATUM);
        spr.drawString("USB", x, y + 1, 1);
        return;
    }

    // Choose color based on level
    uint16_t fillColor;
    if (state.batteryPercent > 60) {
        fillColor = COLOR_SUCCESS;  // Green
    } else if (state.batteryPercent > 20) {
        fillColor = COLOR_ORANGE;   // Orange/yellow
    } else {
        fillColor = COLOR_ERROR;    // Red
    }

    // Battery outline
    spr.drawRect(x, y, w, h, COLOR_MUTED);
    // Battery tip (positive terminal)
    spr.fillRect(x + w, y + (h - tipH) / 2, tipW, tipH, COLOR_MUTED);

    // Fill based on percentage
    int fillW = ((w - 4) * state.batteryPercent) / 100;
    if (fillW > 0) {
        spr.fillRect(x + 2, y + 2, fillW, h - 4, fillColor);
    }

    // Charging indicator (lightning bolt or just show %)
    if (state.batteryCharging) {
        spr.setTextColor(COLOR_BG);
        spr.setTextDatum(MC_DATUM);
        spr.drawString("+", x + w/2, y + h/2, 1);
    }

    // Show percentage text next to battery
    char pctStr[8];
    sprintf(pctStr, "%d%%", state.batteryPercent);
    spr.setTextColor(COLOR_MUTED);
    spr.setTextDatum(TL_DATUM);
    spr.drawString(pctStr, x + w + tipW + 4, y + 1, 1);
}

// ============== TinyBee Communication ==============

void pingTinyBee() {
    if (!mqttClient.connected()) {
        mqttLog("[PING] Can't ping - MQTT not connected\n");
        return;
    }

    // Build the command topic for TinyBee1
    String tinyBeeTopic = String(config.topicPrefix) + "/devices/" + config.companionBeeId + "/set";

    // Send a ping command (TinyBee can handle this as a heartbeat/acknowledgment)
    JsonDocument doc;
    doc["capability"]["instance"] = "ping";
    doc["capability"]["value"] = "from_kayciBee";
    doc["sender"] = deviceId;
    doc["timestamp"] = state.uptime;

    String payload;
    serializeJson(doc, payload);

    mqttClient.publish(tinyBeeTopic.c_str(), 0, false, payload.c_str());
    mqttLog("[PING] Sent ping to TinyBee1\n");
}

// ============== Logging Functions ==============

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

// ============== Async Web Server ==============

void setupWebServer() {
    // Root page - device status (same as original but updated for T-Display)
    webServer.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
        String html = R"rawhtml(
<!DOCTYPE html>
<html>
<head>
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <title>T-Display S3 Homecontrol</title>
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
        .badge { background: #8b5cf6; color: white; padding: 2px 8px; border-radius: 4px; font-size: 12px; margin-left: 8px; }
    </style>
</head>
<body>
    <div class="container">
        <h1>)rawhtml";
        html += config.deviceName;
        html += R"rawhtml(<span class="badge">T-Display</span></h1>
        <p class="subtitle">LILYGO T-Display S3</p>
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
                <span class="label">PSRAM</span>
                <span class="value">)rawhtml";
        html += String(ESP.getPsramSize() / (1024*1024)) + " MB";
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

    // Config page (simplified - reuse from main_async.cpp pattern)
    webServer.on("/config", HTTP_GET, [](AsyncWebServerRequest *request) {
        String html = R"rawhtml(
<!DOCTYPE html>
<html>
<head>
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <title>Settings - T-Display S3</title>
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
            <div class="card">
                <h3 style="margin-top:0;color:#888;">Companion Bee</h3>
                <label>Device ID</label>
                <input type="text" name="companion_bee" value=")rawhtml";
        html += config.companionBeeId;
        html += R"rawhtml(" placeholder="e.g., 503035d4db1c">
                <p style="color:#666;font-size:12px;margin-top:8px;">Device ID of the bee to communicate with (ping button)</p>
            </div>
            <div class="card">
                <h3 style="margin-top:0;color:#888;">Display Dimming</h3>
                <p style="color:#666;font-size:12px;margin-bottom:12px;">Dim the display during sleep hours to save power</p>
                <div style="display:flex;gap:10px;">
                    <div style="flex:1;">
                        <label>Dim Start (hour)</label>
                        <input type="number" name="dim_start" min="0" max="23" value=")rawhtml";
        html += String(config.dimStartHour);
        html += R"rawhtml(">
                    </div>
                    <div style="flex:1;">
                        <label>Dim End (hour)</label>
                        <input type="number" name="dim_end" min="0" max="23" value=")rawhtml";
        html += String(config.dimEndHour);
        html += R"rawhtml(">
                    </div>
                </div>
                <div style="display:flex;gap:10px;margin-top:10px;">
                    <div style="flex:1;">
                        <label>Dim Brightness (0-255)</label>
                        <input type="number" name="dim_bright" min="0" max="255" value=")rawhtml";
        html += String(config.dimBrightness);
        html += R"rawhtml(">
                    </div>
                    <div style="flex:1;">
                        <label>Normal Brightness</label>
                        <input type="number" name="norm_bright" min="0" max="255" value=")rawhtml";
        html += String(config.normalBrightness);
        html += R"rawhtml(">
                    </div>
                </div>
                <p style="color:#666;font-size:12px;margin-top:8px;">Hours in 24h format. E.g., 1-7 = 1AM to 7AM</p>
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
        if (request->hasParam("companion_bee", true)) {
            strncpy(config.companionBeeId, request->getParam("companion_bee", true)->value().c_str(), sizeof(config.companionBeeId) - 1);
        }
        // Dimming settings
        if (request->hasParam("dim_start", true)) {
            config.dimStartHour = request->getParam("dim_start", true)->value().toInt();
        }
        if (request->hasParam("dim_end", true)) {
            config.dimEndHour = request->getParam("dim_end", true)->value().toInt();
        }
        if (request->hasParam("dim_bright", true)) {
            config.dimBrightness = request->getParam("dim_bright", true)->value().toInt();
        }
        if (request->hasParam("norm_bright", true)) {
            config.normalBrightness = request->getParam("norm_bright", true)->value().toInt();
        }

        saveConfig();

        // Apply brightness immediately
        currentBrightness = -1;  // Force update
        updateBrightness();

        request->redirect("/");
        delay(500);
        ESP.restart();
    });

    // Toggle LED
    webServer.on("/toggle", HTTP_GET, [](AsyncWebServerRequest *request) {
        state.ledOn = !state.ledOn;
        displayNeedsUpdate = true;
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
        doc["psram_size"] = ESP.getPsramSize();
        doc["firmware"] = FIRMWARE_VERSION;
        doc["ip"] = WiFi.localIP().toString();
        doc["display_screen"] = currentScreen;

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
        strcpy(config.deviceName, "SimBee");
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

    preferences.getString("companionBee", config.companionBeeId, sizeof(config.companionBeeId));
    // Default to TinyBee1 if not set
    if (strlen(config.companionBeeId) == 0) {
        strcpy(config.companionBeeId, "503035d4db1c");
    }

    // Dimming schedule settings
    config.dimStartHour = preferences.getInt("dimStart", 1);
    config.dimEndHour = preferences.getInt("dimEnd", 7);
    config.dimBrightness = preferences.getInt("dimBright", 20);
    config.normalBrightness = preferences.getInt("normBright", 255);

    preferences.end();

    mqttLog("[Config] Loaded: name=%s, mqtt=%s:%d, companion=%s, dim=%d-%d\n",
                  config.deviceName, config.mqttServer, config.mqttPort, config.companionBeeId,
                  config.dimStartHour, config.dimEndHour);
}

void saveConfig() {
    preferences.begin("homecontrol", false);
    preferences.putString("deviceName", config.deviceName);
    preferences.putString("mqttServer", config.mqttServer);
    preferences.putInt("mqttPort", config.mqttPort);
    preferences.putString("mqttUser", config.mqttUser);
    preferences.putString("mqttPass", config.mqttPass);
    preferences.putString("topicPrefix", config.topicPrefix);
    preferences.putString("companionBee", config.companionBeeId);
    // Dimming schedule
    preferences.putInt("dimStart", config.dimStartHour);
    preferences.putInt("dimEnd", config.dimEndHour);
    preferences.putInt("dimBright", config.dimBrightness);
    preferences.putInt("normBright", config.normalBrightness);
    preferences.end();

    Serial.println("[Config] Saved");
}

String getDeviceId() {
    uint64_t chipid = ESP.getEfuseMac();
    char id[13];
    sprintf(id, "%04x%08x", (uint16_t)(chipid >> 32), (uint32_t)chipid);
    return String(id);
}
