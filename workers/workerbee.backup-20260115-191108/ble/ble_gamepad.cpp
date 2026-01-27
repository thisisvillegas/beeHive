#include "ble_gamepad.h"

// Global instance
BleGamepad bleGamepad;

// HID Report Descriptor - defines our gamepad to the host
// This tells the OS: "I'm a gamepad with 3 axes and 16 buttons"
const uint8_t BLE_HID_REPORT_DESCRIPTOR[] = {
    // Gamepad
    0x05, 0x01,        // Usage Page (Generic Desktop)
    0x09, 0x05,        // Usage (Game Pad)
    0xA1, 0x01,        // Collection (Application)
    0x85, 0x01,        //   Report ID (1)

    // 16 Buttons
    0x05, 0x09,        //   Usage Page (Button)
    0x19, 0x01,        //   Usage Minimum (Button 1)
    0x29, 0x10,        //   Usage Maximum (Button 16)
    0x15, 0x00,        //   Logical Minimum (0)
    0x25, 0x01,        //   Logical Maximum (1)
    0x75, 0x01,        //   Report Size (1)
    0x95, 0x10,        //   Report Count (16)
    0x81, 0x02,        //   Input (Data,Var,Abs)

    // 3 Axes (8-bit each: Throttle, Brake, Volume)
    0x05, 0x01,        //   Usage Page (Generic Desktop)
    0x09, 0x30,        //   Usage (X) - Throttle
    0x09, 0x31,        //   Usage (Y) - Brake
    0x09, 0x32,        //   Usage (Z) - Volume
    0x15, 0x00,        //   Logical Minimum (0)
    0x26, 0xFF, 0x00,  //   Logical Maximum (255)
    0x75, 0x08,        //   Report Size (8)
    0x95, 0x03,        //   Report Count (3)
    0x81, 0x02,        //   Input (Data,Var,Abs)

    0xC0               // End Collection
};

const size_t BLE_HID_REPORT_DESCRIPTOR_SIZE = sizeof(BLE_HID_REPORT_DESCRIPTOR);

BleGamepad::BleGamepad() {
    pServer = nullptr;
    pHid = nullptr;
    pInputChar = nullptr;

    connected = false;
    batteryLevel = 100;

    axisThrottle = 0;
    axisBrake = 0;
    axisVolume = 0;
    buttons = 0;

    memset(reportData, 0, sizeof(reportData));
}

void BleGamepad::begin() {
    Serial.println("[BLE] Initializing gamepad...");

    // Initialize NimBLE
    NimBLEDevice::init(BLE_DEVICE_NAME);

    // Set power level (maximize range)
    NimBLEDevice::setPower(ESP_PWR_LVL_P9);

    // Create server
    pServer = NimBLEDevice::createServer();
    pServer->setCallbacks(this);

    // Create HID device
    pHid = new NimBLEHIDDevice(pServer);

    // Set manufacturer
    pHid->manufacturer()->setValue(BLE_MANUFACTURER);

    // Set PnP info (generic gamepad)
    pHid->pnp(0x01,   // Vendor ID source (Bluetooth SIG)
              0x02E5, // Vendor ID (ESP32)
              0xABCD, // Product ID
              0x0110  // Product version
    );

    // Set HID info
    pHid->hidInfo(0x00, 0x01);  // Country: not localized, flags: normally connectable

    // Set security (works without pairing but can pair)
    NimBLEDevice::setSecurityAuth(false, false, false);

    // Set report map (descriptor)
    pHid->reportMap((uint8_t*)BLE_HID_REPORT_DESCRIPTOR, BLE_HID_REPORT_DESCRIPTOR_SIZE);

    // Get input characteristic for sending reports
    pInputChar = pHid->inputReport(1);  // Report ID 1

    // Set battery level
    pHid->setBatteryLevel(batteryLevel);

    // Start HID service
    pHid->startServices();

    // Start advertising
    NimBLEAdvertising* pAdvertising = pServer->getAdvertising();
    pAdvertising->setAppearance(HID_GAMEPAD);  // 0x03C4
    pAdvertising->addServiceUUID(pHid->hidService()->getUUID());
    pAdvertising->start();

    Serial.println("[BLE] Gamepad ready - advertising as: " BLE_DEVICE_NAME);
}

void BleGamepad::end() {
    if (pServer != nullptr) {
        pServer->getAdvertising()->stop();
    }
    NimBLEDevice::deinit(true);
    connected = false;
    Serial.println("[BLE] Gamepad stopped");
}

bool BleGamepad::isConnected() const {
    return connected;
}

void BleGamepad::setThrottle(uint8_t value) {
    axisThrottle = value;
}

void BleGamepad::setBrake(uint8_t value) {
    axisBrake = value;
}

void BleGamepad::setVolume(uint8_t value) {
    axisVolume = value;
}

void BleGamepad::setAxes(uint8_t throttle, uint8_t brake, uint8_t volume) {
    axisThrottle = throttle;
    axisBrake = brake;
    axisVolume = volume;
}

void BleGamepad::pressButton(uint8_t button) {
    if (button < 16) {
        buttons |= (1 << button);
    }
}

void BleGamepad::releaseButton(uint8_t button) {
    if (button < 16) {
        buttons &= ~(1 << button);
    }
}

void BleGamepad::buildReport() {
    // Report format: [buttons_low, buttons_high, throttle, brake, volume]
    reportData[0] = buttons & 0xFF;         // Buttons 1-8
    reportData[1] = (buttons >> 8) & 0xFF;  // Buttons 9-16
    reportData[2] = axisThrottle;
    reportData[3] = axisBrake;
    reportData[4] = axisVolume;
}

void BleGamepad::sendReport() {
    if (!connected || pInputChar == nullptr) return;

    buildReport();
    pInputChar->setValue(reportData, 5);
    pInputChar->notify();
}

uint8_t BleGamepad::getBatteryLevel() const {
    return batteryLevel;
}

// Connection callbacks
void BleGamepad::onConnect(NimBLEServer* pServer) {
    connected = true;
    Serial.println("[BLE] Host connected!");

    // Update connection parameters for low latency
    // Min interval: 7.5ms, Max interval: 15ms, Latency: 0, Timeout: 500ms
    // Get the connection handle from the peer info
    NimBLEConnInfo connInfo = pServer->getPeerInfo(0);
    pServer->updateConnParams(
        connInfo.getConnHandle(),
        6,    // Min interval (6 * 1.25ms = 7.5ms)
        12,   // Max interval (12 * 1.25ms = 15ms)
        0,    // Latency
        400   // Timeout (400 * 10ms = 4s)
    );
}

void BleGamepad::onDisconnect(NimBLEServer* pServer) {
    connected = false;
    Serial.println("[BLE] Host disconnected");

    // Restart advertising
    pServer->getAdvertising()->start();
    Serial.println("[BLE] Advertising restarted");
}
