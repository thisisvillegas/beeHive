/**
 * SimBee MCP23017 Test
 * 
 * Simple test to verify MCP23017 I2C connection and read switches.
 * 
 * Wiring:
 *   MCP23017 VDD   → 3.3V
 *   MCP23017 VSS   → GND
 *   MCP23017 SCL   → GPIO 17
 *   MCP23017 SDA   → GPIO 18
 *   MCP23017 A0-A2 → GND (address 0x20)
 *   MCP23017 RESET → 3.3V
 *   MCP23017 GPA0  → Momentary button → GND
 *   MCP23017 GPA1  → 3-way switch pos 1 → GND
 *   MCP23017 GPA2  → 3-way switch pos 2 → GND
 */

#include <Arduino.h>
#include <Wire.h>
#include <TFT_eSPI.h>

// I2C pins for T-Display S3
#define I2C_SDA 18
#define I2C_SCL 17

// MCP23017 I2C address (A0=A1=A2=GND)
#define MCP23017_ADDR 0x20

// MCP23017 registers
#define MCP_IODIRA   0x00  // I/O direction register A
#define MCP_IODIRB   0x01  // I/O direction register B
#define MCP_GPPUA    0x0C  // Pull-up register A
#define MCP_GPPUB    0x0D  // Pull-up register B
#define MCP_GPIOA    0x12  // GPIO register A
#define MCP_GPIOB    0x13  // GPIO register B

// Display
TFT_eSPI tft = TFT_eSPI();
TFT_eSprite spr = TFT_eSprite(&tft);

#define SCREEN_WIDTH 170
#define SCREEN_HEIGHT 320

// Colors
#define COLOR_BG      TFT_BLACK
#define COLOR_TEXT    TFT_WHITE
#define COLOR_ACCENT  0x07E0  // Green
#define COLOR_WARN    0xFD20  // Orange
#define COLOR_DIM     0x8410  // Gray

bool mcpFound = false;
uint8_t lastGPIOA = 0xFF;
uint8_t lastGPIOB = 0xFF;

// Write to MCP23017 register
void mcpWrite(uint8_t reg, uint8_t value) {
    Wire.beginTransmission(MCP23017_ADDR);
    Wire.write(reg);
    Wire.write(value);
    Wire.endTransmission();
}

// Read from MCP23017 register
uint8_t mcpRead(uint8_t reg) {
    Wire.beginTransmission(MCP23017_ADDR);
    Wire.write(reg);
    Wire.endTransmission();
    Wire.requestFrom(MCP23017_ADDR, (uint8_t)1);
    return Wire.read();
}

// Initialize MCP23017
bool mcpInit() {
    // Check if device responds
    Wire.beginTransmission(MCP23017_ADDR);
    if (Wire.endTransmission() != 0) {
        return false;
    }
    
    // Set all pins as inputs
    mcpWrite(MCP_IODIRA, 0xFF);
    mcpWrite(MCP_IODIRB, 0xFF);
    
    // Enable pull-ups on all pins
    mcpWrite(MCP_GPPUA, 0xFF);
    mcpWrite(MCP_GPPUB, 0xFF);
    
    return true;
}

void drawScreen() {
    spr.fillSprite(COLOR_BG);
    
    // Title
    spr.setTextColor(COLOR_TEXT);
    spr.setTextDatum(TC_DATUM);
    spr.drawString("MCP23017 Test", SCREEN_WIDTH/2, 10, 4);
    
    // I2C Status
    int y = 60;
    spr.setTextDatum(TL_DATUM);
    spr.setTextColor(COLOR_DIM);
    spr.drawString("I2C:", 10, y, 2);
    spr.setTextColor(mcpFound ? COLOR_ACCENT : COLOR_WARN);
    spr.drawString(mcpFound ? "Found 0x20" : "NOT FOUND!", 60, y, 2);
    
    if (!mcpFound) {
        spr.setTextColor(COLOR_WARN);
        spr.setTextDatum(TC_DATUM);
        spr.drawString("Check wiring!", SCREEN_WIDTH/2, 120, 2);
        spr.drawString("SDA=GPIO18", SCREEN_WIDTH/2, 150, 2);
        spr.drawString("SCL=GPIO17", SCREEN_WIDTH/2, 180, 2);
        spr.pushSprite(0, 0);
        return;
    }
    
    // Read GPIO
    uint8_t gpioA = mcpRead(MCP_GPIOA);
    uint8_t gpioB = mcpRead(MCP_GPIOB);
    
    // Port A status
    y = 100;
    spr.setTextColor(COLOR_TEXT);
    spr.drawString("Port A:", 10, y, 2);
    
    // Draw individual pins
    y += 25;
    for (int i = 0; i < 8; i++) {
        int x = 10 + (i * 20);
        bool pinState = (gpioA >> i) & 1;
        spr.fillCircle(x + 8, y + 8, 7, pinState ? COLOR_DIM : COLOR_ACCENT);
        spr.setTextColor(COLOR_TEXT);
        spr.setTextDatum(TC_DATUM);
        spr.drawString(String(i), x + 8, y + 20, 1);
    }
    
    // Switch interpretations
    y = 180;
    spr.setTextDatum(TL_DATUM);
    
    // Momentary button (GPA0) - active LOW
    bool btnPressed = !((gpioA >> 0) & 1);
    spr.setTextColor(COLOR_DIM);
    spr.drawString("Button:", 10, y, 2);
    spr.setTextColor(btnPressed ? COLOR_ACCENT : COLOR_TEXT);
    spr.drawString(btnPressed ? "PRESSED" : "released", 80, y, 2);
    
    // 3-way switch (GPA1, GPA2)
    y += 30;
    bool sw1 = !((gpioA >> 1) & 1);  // Position 1
    bool sw2 = !((gpioA >> 2) & 1);  // Position 2
    
    spr.setTextColor(COLOR_DIM);
    spr.drawString("3-Way:", 10, y, 2);
    
    String swPos;
    if (sw1 && !sw2) swPos = "POS 1";
    else if (!sw1 && sw2) swPos = "POS 2";
    else if (!sw1 && !sw2) swPos = "CENTER";
    else swPos = "???";
    
    spr.setTextColor(COLOR_ACCENT);
    spr.drawString(swPos, 80, y, 2);
    
    // Raw hex values
    y = 260;
    spr.setTextColor(COLOR_DIM);
    char buf[32];
    sprintf(buf, "Raw A: 0x%02X", gpioA);
    spr.drawString(buf, 10, y, 2);
    sprintf(buf, "Raw B: 0x%02X", gpioB);
    spr.drawString(buf, 10, y + 20, 2);
    
    // Footer
    spr.setTextDatum(TC_DATUM);
    spr.setTextColor(COLOR_DIM);
    spr.drawString("LOW = pressed/active", SCREEN_WIDTH/2, SCREEN_HEIGHT - 20, 1);
    
    spr.pushSprite(0, 0);
}

void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println("\n=== MCP23017 Test ===");
    
    // Display power (T-Display S3)
    pinMode(15, OUTPUT);
    digitalWrite(15, HIGH);
    
    // Display init
    tft.init();
    tft.setRotation(0);
    tft.fillScreen(COLOR_BG);
    tft.invertDisplay(true);
    
    // Backlight
    pinMode(38, OUTPUT);
    digitalWrite(38, HIGH);
    
    // Sprite
    spr.createSprite(SCREEN_WIDTH, SCREEN_HEIGHT);
    spr.setSwapBytes(true);
    
    // I2C init
    Serial.printf("I2C init: SDA=%d, SCL=%d\n", I2C_SDA, I2C_SCL);
    Wire.begin(I2C_SDA, I2C_SCL);
    Wire.setClock(100000);  // 100kHz
    
    // Scan for devices
    Serial.println("Scanning I2C bus...");
    for (uint8_t addr = 1; addr < 127; addr++) {
        Wire.beginTransmission(addr);
        if (Wire.endTransmission() == 0) {
            Serial.printf("  Found device at 0x%02X\n", addr);
        }
    }
    
    // Init MCP23017
    mcpFound = mcpInit();
    Serial.printf("MCP23017: %s\n", mcpFound ? "OK" : "NOT FOUND");
    
    drawScreen();
}

void loop() {
    static unsigned long lastUpdate = 0;
    
    if (millis() - lastUpdate > 50) {  // 20Hz update
        lastUpdate = millis();
        drawScreen();
        
        // Log changes to serial
        if (mcpFound) {
            uint8_t gpioA = mcpRead(MCP_GPIOA);
            if (gpioA != lastGPIOA) {
                Serial.printf("GPIOA changed: 0x%02X\n", gpioA);
                lastGPIOA = gpioA;
            }
        }
    }
    
    delay(10);
}
