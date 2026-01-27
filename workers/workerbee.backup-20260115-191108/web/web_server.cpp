// Include order matters! WiFiManager pulls in WebServer.h which has http_parser
// conflicts with ESPAsyncWebServer. WiFiManager must come FIRST.
#include <WiFi.h>
#include <Preferences.h>
#include "../config.h"
#include "../wifi/wifi_manager.h"

// Now safe to include ESPAsyncWebServer after WiFiManager's headers loaded
#include <ESPAsyncWebServer.h>
#include "web_server.h"

AsyncWebServer webServer(80);
static WifiManagerWrapper* pWifiManager = nullptr;
static Preferences prefs;

extern char deviceId[];

// Persistent device configuration
struct WebConfig {
    char deviceName[32];
    char mqttBroker[64];
    uint16_t mqttPort;
    char companionBeeId[16];  // Device ID of companion bee (e.g., "503035d4db1c")
} webConfig;

void loadWebConfig() {
    prefs.begin("workerbee", true);  // read-only
    strlcpy(webConfig.deviceName, prefs.getString("name", DEVICE_NAME).c_str(), sizeof(webConfig.deviceName));
    strlcpy(webConfig.mqttBroker, prefs.getString("mqtt_host", MQTT_BROKER_HOST).c_str(), sizeof(webConfig.mqttBroker));
    webConfig.mqttPort = prefs.getUShort("mqtt_port", MQTT_BROKER_PORT);
    strlcpy(webConfig.companionBeeId, prefs.getString("companion", "").c_str(), sizeof(webConfig.companionBeeId));
    prefs.end();
}

void saveWebConfig() {
    prefs.begin("workerbee", false);  // read-write
    prefs.putString("name", webConfig.deviceName);
    prefs.putString("mqtt_host", webConfig.mqttBroker);
    prefs.putUShort("mqtt_port", webConfig.mqttPort);
    prefs.putString("companion", webConfig.companionBeeId);
    prefs.end();
}

void setWebServerWifiManager(void* wm) {
    pWifiManager = (WifiManagerWrapper*)wm;
}

void setupWebServer() {
    // Load saved config
    loadWebConfig();

    // Helper to format uptime
    auto formatUptime = []() -> String {
        uint32_t secs = millis() / 1000;
        uint32_t mins = secs / 60; secs %= 60;
        uint32_t hrs = mins / 60; mins %= 60;
        uint32_t days = hrs / 24; hrs %= 24;
        if (days > 0) return String(days) + "d " + String(hrs) + "h";
        if (hrs > 0) return String(hrs) + "h " + String(mins) + "m";
        return String(mins) + "m " + String(secs) + "s";
    };

    // Root page - device status
    webServer.on("/", HTTP_GET, [formatUptime](AsyncWebServerRequest *request) {
        String html = R"rawhtml(
<!DOCTYPE html>
<html>
<head>
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <title>)rawhtml";
        html += webConfig.deviceName;
        html += R"rawhtml(</title>
    <style>
        body { font-family: -apple-system, sans-serif; background: #1a1a1a; color: #fff; margin: 0; padding: 20px; }
        .container { max-width: 400px; margin: 0 auto; }
        h1 { color: #f59e0b; margin-bottom: 5px; }
        .subtitle { color: #666; margin-bottom: 20px; }
        .card { background: #2a2a2a; border-radius: 12px; padding: 20px; margin-bottom: 15px; }
        .status-row { display: flex; justify-content: space-between; padding: 8px 0; border-bottom: 1px solid #333; }
        .status-row:last-child { border: none; }
        .label { color: #888; }
        .value { color: #fff; font-weight: 500; }
        .online { color: #22c55e; }
        .offline { color: #ef4444; }
        .btn { display: block; width: 100%; padding: 15px; border: none; border-radius: 8px; font-size: 16px; cursor: pointer; margin-top: 10px; box-sizing: border-box; text-align: center; text-decoration: none; }
        .btn-primary { background: #f59e0b; color: black; }
        .btn-secondary { background: #333; color: #fff; }
        .btn-danger { background: #dc2626; color: white; }
    </style>
</head>
<body>
    <div class="container">
        <h1>)rawhtml";
        html += webConfig.deviceName;
        html += R"rawhtml(</h1>
        <p class="subtitle">Racing Sim Dashboard</p>
        <div class="card">
            <div class="status-row">
                <span class="label">Device ID</span>
                <span class="value">)rawhtml";
        html += deviceId;
        html += R"rawhtml(</span>
            </div>
            <div class="status-row">
                <span class="label">IP Address</span>
                <span class="value">)rawhtml";
        html += WiFi.localIP().toString();
        html += R"rawhtml(</span>
            </div>
            <div class="status-row">
                <span class="label">WiFi Signal</span>
                <span class="value">)rawhtml";
        html += String(WiFi.RSSI()) + " dBm";
        html += R"rawhtml(</span>
            </div>
            <div class="status-row">
                <span class="label">Free Heap</span>
                <span class="value">)rawhtml";
        html += String(ESP.getFreeHeap() / 1024) + " KB";
        html += R"rawhtml(</span>
            </div>
            <div class="status-row">
                <span class="label">Uptime</span>
                <span class="value">)rawhtml";
        html += formatUptime();
        html += R"rawhtml(</span>
            </div>
            <div class="status-row">
                <span class="label">Firmware</span>
                <span class="value">)rawhtml";
        html += FIRMWARE_VERSION;
        html += R"rawhtml(</span>
            </div>
        </div>
        <a href="/config" class="btn btn-secondary">Settings</a>
        <a href="/reboot" class="btn btn-danger" onclick="return confirm('Reboot device?')">Reboot</a>
    </div>
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
    <title>Settings - Worker Bee</title>
    <style>
        body { font-family: -apple-system, sans-serif; background: #1a1a1a; color: #fff; margin: 0; padding: 20px; }
        .container { max-width: 400px; margin: 0 auto; }
        h1 { color: #f59e0b; }
        .card { background: #2a2a2a; border-radius: 12px; padding: 20px; margin-bottom: 15px; }
        h3 { margin-top: 0; color: #888; }
        label { display: block; color: #888; margin-bottom: 5px; margin-top: 15px; }
        label:first-of-type { margin-top: 0; }
        input { width: 100%; padding: 12px; border: 1px solid #444; border-radius: 8px; background: #1a1a1a; color: #fff; box-sizing: border-box; }
        .btn { display: block; width: 100%; padding: 15px; border: none; border-radius: 8px; font-size: 16px; cursor: pointer; margin-top: 15px; box-sizing: border-box; text-align: center; text-decoration: none; }
        .btn-primary { background: #f59e0b; color: black; }
        .btn-secondary { background: #333; color: #fff; }
        .btn-danger { background: #dc2626; color: white; }
    </style>
</head>
<body>
    <div class="container">
        <h1>Settings</h1>
        <form method="POST" action="/config">
            <div class="card">
                <h3>Device</h3>
                <label>Device Name</label>
                <input type="text" name="device_name" value=")rawhtml";
        html += webConfig.deviceName;
        html += R"rawhtml(">
            </div>
            <div class="card">
                <h3>MQTT</h3>
                <label>Server Address</label>
                <input type="text" name="mqtt_broker" value=")rawhtml";
        html += webConfig.mqttBroker;
        html += R"rawhtml(">
                <label>Port</label>
                <input type="number" name="mqtt_port" value=")rawhtml";
        html += String(webConfig.mqttPort);
        html += R"rawhtml(">
            </div>
            <div class="card">
                <h3>Companion Bee</h3>
                <label>Device ID</label>
                <input type="text" name="companion_bee" value=")rawhtml";
        html += webConfig.companionBeeId;
        html += R"rawhtml(" placeholder="e.g., 503035d4db1c">
                <p style="color:#666;font-size:12px;margin-top:8px;">Device ID of the bee to communicate with</p>
            </div>
            <button type="submit" class="btn btn-primary">Save Settings</button>
        </form>
        <a href="/" class="btn btn-secondary">Back</a>
        <button class="btn btn-danger" onclick="if(confirm('Reset WiFi settings and reboot?')){fetch('/reboot?reset=1').then(()=>alert('Rebooting...'))}">Reset WiFi & Reboot</button>
    </div>
</body>
</html>)rawhtml";
        request->send(200, "text/html", html);
    });

    // Config POST handler
    webServer.on("/config", HTTP_POST, [](AsyncWebServerRequest *request) {
        if (request->hasParam("device_name", true)) {
            strlcpy(webConfig.deviceName, request->getParam("device_name", true)->value().c_str(), sizeof(webConfig.deviceName));
        }
        if (request->hasParam("mqtt_broker", true)) {
            strlcpy(webConfig.mqttBroker, request->getParam("mqtt_broker", true)->value().c_str(), sizeof(webConfig.mqttBroker));
        }
        if (request->hasParam("mqtt_port", true)) {
            webConfig.mqttPort = request->getParam("mqtt_port", true)->value().toInt();
        }
        if (request->hasParam("companion_bee", true)) {
            strlcpy(webConfig.companionBeeId, request->getParam("companion_bee", true)->value().c_str(), sizeof(webConfig.companionBeeId));
        }
        saveWebConfig();
        Serial.printf("[Web] Config saved: name=%s, mqtt=%s:%d\n",
                      webConfig.deviceName, webConfig.mqttBroker, webConfig.mqttPort);
        request->redirect("/");
        delay(500);
        ESP.restart();  // Restart to apply new settings
    });

    // Reboot handler (with optional WiFi reset)
    webServer.on("/reboot", HTTP_GET, [](AsyncWebServerRequest *request) {
        bool resetWifi = request->hasParam("reset") && request->getParam("reset")->value() == "1";
        if (resetWifi && pWifiManager) {
            pWifiManager->resetSettings();
            Serial.println("[Web] WiFi settings reset");
        }
        request->send(200, "text/html", "<html><body style='background:#1a1a1a;color:#fff;font-family:sans-serif;text-align:center;padding-top:100px;'><h1>Rebooting...</h1><script>setTimeout(()=>location='/',5000)</script></body></html>");
        delay(500);
        ESP.restart();
    });

    // API status endpoint
    webServer.on("/api/status", HTTP_GET, [](AsyncWebServerRequest *request) {
        String json = "{";
        json += "\"device_id\":\"" + String(deviceId) + "\",";
        json += "\"device_name\":\"" + String(webConfig.deviceName) + "\",";
        json += "\"ip\":\"" + WiFi.localIP().toString() + "\",";
        json += "\"rssi\":" + String(WiFi.RSSI()) + ",";
        json += "\"heap\":" + String(ESP.getFreeHeap()) + ",";
        json += "\"uptime\":" + String(millis() / 1000) + ",";
        json += "\"firmware\":\"" + String(FIRMWARE_VERSION) + "\",";
        json += "\"mqtt_broker\":\"" + String(webConfig.mqttBroker) + "\",";
        json += "\"mqtt_port\":" + String(webConfig.mqttPort) + ",";
        json += "\"companion_bee\":\"" + String(webConfig.companionBeeId) + "\"";
        json += "}";
        request->send(200, "application/json", json);
    });

    webServer.begin();
    Serial.printf("[Web] Server started at http://%s/\n", WiFi.localIP().toString().c_str());
}

// Config getters for MQTT
const char* getConfigDeviceName() { return webConfig.deviceName; }
const char* getConfigMqttBroker() { return webConfig.mqttBroker; }
uint16_t getConfigMqttPort() { return webConfig.mqttPort; }
const char* getConfigCompanionId() { return webConfig.companionBeeId; }
