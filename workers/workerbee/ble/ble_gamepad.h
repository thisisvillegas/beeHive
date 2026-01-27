#ifndef BLE_GAMEPAD_H
#define BLE_GAMEPAD_H

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <NimBLEHIDDevice.h>
#include <NimBLECharacteristic.h>
#include "../config.h"

/**
 * BLE HID Gamepad for Worker Bee
 *
 * Presents as a standard HID gamepad with:
 *   - 3 analog axes (throttle, brake, volume)
 *   - 16 buttons (reserved for future touch buttons)
 *
 * Uses NimBLE for lower memory footprint than Bluedroid.
 * Target latency: ~7.5-20ms at 50Hz report rate.
 */

class BleGamepad : public NimBLEServerCallbacks,
                   public NimBLECharacteristicCallbacks {
public:
    BleGamepad();

    // Lifecycle
    void begin();
    void end();

    // Check connection status
    bool isConnected() const;

    // Set axis values (0-255)
    void setThrottle(uint8_t value);
    void setBrake(uint8_t value);
    void setVolume(uint8_t value);

    // Set all axes at once
    void setAxes(uint8_t throttle, uint8_t brake, uint8_t volume);

    // Button control (for future touch buttons)
    void pressButton(uint8_t button);
    void releaseButton(uint8_t button);

    // Send current state to host
    void sendReport();

    // Get battery level for display
    uint8_t getBatteryLevel() const;

private:
    NimBLEServer* pServer;
    NimBLEHIDDevice* pHid;
    NimBLECharacteristic* pInputChar;

    bool connected;
    uint8_t batteryLevel;

    // Gamepad state
    uint8_t axisThrottle;
    uint8_t axisBrake;
    uint8_t axisVolume;
    uint16_t buttons;

    // Report data
    uint8_t reportData[7];  // 2 bytes buttons + 3 bytes axes + padding

    void buildReport();

    // Callbacks
    void onConnect(NimBLEServer* pServer) override;
    void onDisconnect(NimBLEServer* pServer) override;
};

// HID Report Descriptor for gamepad
extern const uint8_t BLE_HID_REPORT_DESCRIPTOR[];
extern const size_t BLE_HID_REPORT_DESCRIPTOR_SIZE;

// Global instance
extern BleGamepad bleGamepad;

#endif // BLE_GAMEPAD_H
