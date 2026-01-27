#ifndef POTENTIOMETER_H
#define POTENTIOMETER_H

#include <Arduino.h>
#include "config.h"

/**
 * Potentiometer Handler with Smoothing
 *
 * Features:
 *   - Exponential moving average for smooth readings
 *   - Deadzone to prevent jitter near center/edges
 *   - Configurable min/max calibration
 *   - Maps ADC range to 0-255 for gamepad
 */

class Potentiometer {
public:
    Potentiometer(uint8_t pin, const char* name);

    // Initialize ADC
    void begin();

    // Read and update (call regularly)
    void update();

    // Get smoothed value (0-255)
    uint8_t getValue() const;

    // Get raw ADC value (0-4095)
    uint16_t getRawValue() const;

    // Calibration
    void calibrate(uint16_t min, uint16_t max);
    void autoCalibrate();  // Uses current value as center

    // Check if value changed significantly
    bool hasChanged() const;

    // Debug
    const char* getName() const { return name; }

private:
    uint8_t pin;
    const char* name;

    // Calibration
    uint16_t calMin;
    uint16_t calMax;

    // Smoothing
    float smoothedValue;
    float alpha;  // Smoothing factor (0-1, higher = less smoothing)

    // State
    uint16_t rawValue;
    uint8_t mappedValue;
    uint8_t lastReportedValue;

    // Helper
    uint8_t applyDeadzone(uint8_t value) const;
};

/**
 * Manager for multiple potentiometers
 */
class PotentiometerManager {
public:
    PotentiometerManager();

    void begin();
    void update();

    // Access individual pots
    Potentiometer& getThrottle() { return throttle; }
    Potentiometer& getBrake() { return brake; }
    Potentiometer& getVolume() { return volume; }

    // Get all values at once
    void getAxes(uint8_t& throttleVal, uint8_t& brakeVal, uint8_t& volumeVal);

    // Check if any pot changed
    bool hasAnyChanged() const;

private:
    Potentiometer throttle;
    Potentiometer brake;
    Potentiometer volume;
};

// Global instance
extern PotentiometerManager potManager;

#endif // POTENTIOMETER_H
