#include "potentiometer.h"

// Global instance
PotentiometerManager potManager;

// ═══════════════════════════════════════════════════════════════════════════
// Potentiometer Class
// ═══════════════════════════════════════════════════════════════════════════

Potentiometer::Potentiometer(uint8_t pin, const char* name)
    : pin(pin), name(name) {

    // Default calibration (full range)
    calMin = 0;
    calMax = 4095;

    // Smoothing (alpha = 0.2 means 80% old value, 20% new value)
    smoothedValue = 0;
    alpha = 0.2;

    rawValue = 0;
    mappedValue = 0;
    lastReportedValue = 0;
}

void Potentiometer::begin() {
    pinMode(pin, INPUT);

    // Set ADC resolution to 12 bits
    analogReadResolution(ADC_RESOLUTION);

    // Take initial reading
    rawValue = analogRead(pin);
    smoothedValue = rawValue;

    Serial.printf("[POT] %s initialized on GPIO%d\n", name, pin);
}

void Potentiometer::update() {
    // Read raw ADC
    uint16_t newRaw = analogRead(pin);

    // Apply exponential moving average
    smoothedValue = (alpha * newRaw) + ((1.0 - alpha) * smoothedValue);
    rawValue = (uint16_t)smoothedValue;

    // Map to 0-255 with calibration
    int32_t constrained = constrain(rawValue, calMin, calMax);
    mappedValue = map(constrained, calMin, calMax, 0, 255);

    // Apply deadzone
    mappedValue = applyDeadzone(mappedValue);
}

uint8_t Potentiometer::getValue() const {
    return mappedValue;
}

uint16_t Potentiometer::getRawValue() const {
    return rawValue;
}

void Potentiometer::calibrate(uint16_t min, uint16_t max) {
    calMin = min;
    calMax = max;
    Serial.printf("[POT] %s calibrated: %d - %d\n", name, calMin, calMax);
}

void Potentiometer::autoCalibrate() {
    // Use current value as center, assume ~10% range for now
    uint16_t current = rawValue;
    calMin = current > 200 ? current - 200 : 0;
    calMax = current < 3895 ? current + 200 : 4095;
    Serial.printf("[POT] %s auto-cal around %d\n", name, current);
}

bool Potentiometer::hasChanged() const {
    // Check if difference is significant (more than deadzone)
    int diff = abs((int)mappedValue - (int)lastReportedValue);
    return diff > (POT_DEADZONE / 16);  // Scale deadzone to 0-255 range
}

uint8_t Potentiometer::applyDeadzone(uint8_t value) const {
    // Apply deadzone at min and max (prevents jitter at edges)
    const uint8_t edgeDead = 5;  // Dead at 0-5 and 250-255

    if (value <= edgeDead) return 0;
    if (value >= (255 - edgeDead)) return 255;

    // Map remaining range (5-250) to full (0-255)
    return map(value, edgeDead, 255 - edgeDead, 0, 255);
}

// ═══════════════════════════════════════════════════════════════════════════
// PotentiometerManager Class
// ═══════════════════════════════════════════════════════════════════════════

PotentiometerManager::PotentiometerManager()
    : throttle(POT_THROTTLE_PIN, "Throttle"),
      brake(POT_BRAKE_PIN, "Brake"),
      volume(POT_VOLUME_PIN, "Volume") {
}

void PotentiometerManager::begin() {
    Serial.println("[POT] Initializing potentiometers...");

    throttle.begin();
    brake.begin();
    volume.begin();

    Serial.println("[POT] Ready");
}

void PotentiometerManager::update() {
    throttle.update();
    brake.update();
    volume.update();
}

void PotentiometerManager::getAxes(uint8_t& throttleVal, uint8_t& brakeVal, uint8_t& volumeVal) {
    throttleVal = throttle.getValue();
    brakeVal = brake.getValue();
    volumeVal = volume.getValue();
}

bool PotentiometerManager::hasAnyChanged() const {
    return throttle.hasChanged() || brake.hasChanged() || volume.hasChanged();
}
