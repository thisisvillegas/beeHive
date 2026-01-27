#ifndef MQTT_CLIENT_H
#define MQTT_CLIENT_H

#include <Arduino.h>
#include <espMqttClient.h>
#include "../config.h"

/**
 * MQTT Client for Worker Bee (using espMqttClient - async)
 *
 * Features:
 *   - Async, non-blocking MQTT
 *   - Companion bee status tracking
 *   - Ping companion bee functionality
 *   - Health/state publishing
 */

// Initialize MQTT (call after WiFi connected)
void setupMqtt(const char* broker, uint16_t port, const char* myDeviceId);

// Call in network loop
void mqttLoop();

// Check connection
bool isMqttConnected();

// Companion bee
void setCompanionBee(const char* companionId);
bool isCompanionOnline();
void pingCompanionBee();

// Publishing
void publishHealth(int rssi, uint32_t heap);
void publishAvailability(bool online);

// Get status for display
const char* getMqttStatus();

#endif // MQTT_CLIENT_H
