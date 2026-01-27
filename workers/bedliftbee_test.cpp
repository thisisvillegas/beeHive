/**
 * BedLiftBee Minimal Test
 * Just blinks LED and prints to serial - no MQTT
 */

#include <Arduino.h>
#include <WiFi.h>

#define LED_PIN 8

void setup() {
    Serial.begin(115200);
    delay(1000);

    Serial.println("\n\n=== BedLiftBee TEST ===");
    Serial.println("Minimal firmware - no MQTT");
    Serial.printf("Free heap: %d bytes\n", ESP.getFreeHeap());

    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, HIGH); // LED off (active-low)

    // Get device ID
    uint64_t chipid = ESP.getEfuseMac();
    Serial.printf("MAC: %012llx\n", chipid);

    Serial.println("Setup complete!\n");
}

void loop() {
    static uint32_t lastBlink = 0;
    static uint32_t lastPrint = 0;
    static bool ledState = false;

    // Blink every 500ms
    if (millis() - lastBlink > 500) {
        lastBlink = millis();
        ledState = !ledState;
        digitalWrite(LED_PIN, ledState ? LOW : HIGH);
    }

    // Print status every 2 seconds
    if (millis() - lastPrint > 2000) {
        lastPrint = millis();
        Serial.printf("[%lu] Alive! Heap: %d bytes\n", millis()/1000, ESP.getFreeHeap());
    }

    delay(10);
}
