/**
 * SimBee-BLE - BLE Keyboard (ESP32-C3)
 *
 * ESP32-C3 Mini USB Serial JTAG cannot receive serial input.
 * Commands must come via hardware UART on GPIO20 (RX) / GPIO21 (TX).
 *
 * To test: Connect a USB-to-UART adapter:
 *   Adapter TX  --> C3 GPIO20 (RX)
 *   Adapter GND --> C3 GND
 *
 * Commands: KEY:x, TEXT:xxx
 */

#include <Arduino.h>
#include <BleKeyboard.h>
#include "esp_log.h"

static const char* TAG = "SBK";

#define UART_RX 20
#define UART_TX 21

BleKeyboard bleKeyboard("SimBee-BLE", "Beehive", 100);
String inputBuffer = "";

void setup() {
    // USB for debug output only
    Serial.begin(115200);

    // Hardware UART for receiving commands
    Serial1.begin(115200, SERIAL_8N1, UART_RX, UART_TX);

    delay(1000);

    ESP_LOGI(TAG, "=== SimBee-BLE v2.5 ===");
    ESP_LOGI(TAG, "UART RX on GPIO%d", UART_RX);

    bleKeyboard.begin();

    ESP_LOGI(TAG, "BLE keyboard ready");
    ESP_LOGI(TAG, "Send commands via UART: KEY:x or TEXT:xxx");
}

void loop() {
    // Status every 5 seconds
    static unsigned long lastStatus = 0;
    if (millis() - lastStatus > 5000) {
        lastStatus = millis();
        ESP_LOGI(TAG, "BLE=%s UART_avail=%d",
            bleKeyboard.isConnected() ? "YES" : "NO",
            Serial1.available());
    }

    // Read from hardware UART
    while (Serial1.available()) {
        char c = Serial1.read();
        ESP_LOGI(TAG, "RX: 0x%02X", c);

        if (c == '\n' || c == '\r') {
            if (inputBuffer.length() > 0) {
                ESP_LOGI(TAG, "CMD: %s", inputBuffer.c_str());

                if (bleKeyboard.isConnected()) {
                    if (inputBuffer.startsWith("KEY:") && inputBuffer.length() > 4) {
                        char key = inputBuffer.charAt(4);
                        bleKeyboard.write(key);
                        ESP_LOGI(TAG, "Sent: %c", key);
                    } else if (inputBuffer.startsWith("TEXT:")) {
                        bleKeyboard.print(inputBuffer.substring(5));
                        ESP_LOGI(TAG, "Sent text");
                    }
                } else {
                    ESP_LOGW(TAG, "BLE not connected");
                }
                inputBuffer = "";
            }
        } else {
            inputBuffer += c;
        }
    }

    delay(1);
}
