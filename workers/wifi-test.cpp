/**
 * Minimal WiFi Test - Focus on MAC address
 */

#include <Arduino.h>
#include <WiFi.h>

#define WIFI_SSID "Remus2.0"
#define WIFI_PASS "Z3k3Z3k3"

// WiFi event callback
void WiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
    Serial.printf("[EVENT] %d: ", event);
    switch(event) {
        case ARDUINO_EVENT_WIFI_STA_START:
            Serial.println("STA_START");
            break;
        case ARDUINO_EVENT_WIFI_STA_CONNECTED:
            Serial.println("STA_CONNECTED");
            break;
        case ARDUINO_EVENT_WIFI_STA_GOT_IP:
            Serial.printf("GOT_IP: %s\n", WiFi.localIP().toString().c_str());
            break;
        case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
            Serial.printf("DISCONNECTED reason=%d\n", info.wifi_sta_disconnected.reason);
            break;
        default:
            Serial.println("other");
            break;
    }
}

void setup() {
    Serial.begin(115200);
    delay(2000);

    Serial.println("\n\n=============================");
    Serial.println("=== WiFi Test v3 - MAC Check ===");
    Serial.println("=============================\n");

    WiFi.onEvent(WiFiEvent);
    WiFi.mode(WIFI_STA);

    // IMPORTANT: Print this board's MAC address
    Serial.println("*************************************");
    Serial.printf("** THIS BOARD'S MAC: %s **\n", WiFi.macAddress().c_str());
    Serial.println("*************************************");
    Serial.println("Check if this MAC is whitelisted on your router!\n");

    // Disconnect and clear old credentials
    WiFi.disconnect(true, true);  // disconnect and erase credentials
    delay(1000);

    // Scan
    Serial.println("Scanning...");
    int n = WiFi.scanNetworks();
    Serial.printf("Found %d networks\n", n);

    for (int i = 0; i < n; i++) {
        String marker = (WiFi.SSID(i) == WIFI_SSID) ? " <-- TARGET" : "";
        Serial.printf("  %d: %-20s %d dBm ch%d%s\n",
            i, WiFi.SSID(i).c_str(), WiFi.RSSI(i), WiFi.channel(i), marker.c_str());
    }
    WiFi.scanDelete();

    // Connect
    Serial.printf("\nConnecting to: %s\n", WIFI_SSID);
    Serial.printf("Password: %s\n", WIFI_PASS);

    WiFi.begin(WIFI_SSID, WIFI_PASS);

    Serial.print("Waiting");
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 30) {
        delay(500);
        Serial.printf(" [%d]", WiFi.status());
        attempts++;
    }

    Serial.println();
    if (WiFi.status() == WL_CONNECTED) {
        Serial.println("\n=== CONNECTED! ===");
        Serial.printf("IP: %s\n", WiFi.localIP().toString().c_str());
    } else {
        Serial.println("\n=== CONNECTION FAILED ===");
        Serial.printf("Status: %d (4=WL_CONNECT_FAILED)\n", WiFi.status());
        Serial.println("\nPossible causes:");
        Serial.println("1. MAC filtering - add this MAC to your router");
        Serial.println("2. WPA3 mode - router may need WPA2 fallback");
        Serial.println("3. Wrong password");
    }
}

void loop() {
    delay(10000);
    Serial.printf("Status: %s (RSSI: %d)\n",
        WiFi.status() == WL_CONNECTED ? "connected" : "disconnected",
        WiFi.RSSI());
}
