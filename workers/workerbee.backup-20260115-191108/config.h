#ifndef CONFIG_H
#define CONFIG_H

// ═══════════════════════════════════════════════════════════════════════════
// Worker Bee Configuration - Hosyond ESP32-S3 2.8" Touchscreen
// ═══════════════════════════════════════════════════════════════════════════

// ─────────────────────────────────────────────────────────────────────────────
// Device Identity
// ─────────────────────────────────────────────────────────────────────────────
#define DEVICE_NAME "WorkerBee"
#define DEVICE_TYPE "monitor"
#define FIRMWARE_VERSION "0.1.0"

// ─────────────────────────────────────────────────────────────────────────────
// Display Pins (ILI9341V) - Hosyond ESP32-S3 2.8" (from lcdwiki.com)
// ─────────────────────────────────────────────────────────────────────────────
#define TFT_MISO 13
#define TFT_MOSI 11
#define TFT_SCLK 12
#define TFT_CS   10
#define TFT_DC   46
#define TFT_RST  -1   // Shared with ESP32-S3 reset
#define TFT_BL   45   // Backlight control

// ─────────────────────────────────────────────────────────────────────────────
// Touch Pins (FT6336G Capacitive - I2C) - Hosyond ESP32-S3 2.8"
// ─────────────────────────────────────────────────────────────────────────────
#define TOUCH_SDA 16
#define TOUCH_SCL 15
#define TOUCH_INT 17
#define TOUCH_RST 18

// ─────────────────────────────────────────────────────────────────────────────
// Free GPIO Pins (Available for potentiometers/buttons)
// ─────────────────────────────────────────────────────────────────────────────
// Note: Limited GPIOs due to display/touch using most pins
// IO3 may conflict with backlight on some board revisions - test carefully

#define POT_THROTTLE_PIN 2   // ADC1_CH1 - Throttle/Gas (racing game)
#define POT_BRAKE_PIN    3   // ADC1_CH2 - Brake
#define POT_VOLUME_PIN   14  // ADC2_CH3 - Volume control

#define SPARE_BUTTON_PIN 21  // Digital input (if not using I2C)

// ─────────────────────────────────────────────────────────────────────────────
// ADC Configuration
// ─────────────────────────────────────────────────────────────────────────────
#define ADC_RESOLUTION   12       // 12-bit ADC (0-4095)
#define ADC_SAMPLES      8        // Samples for averaging
#define POT_SAMPLE_RATE  100      // Hz - how often to read pots
#define POT_DEADZONE     50       // Ignore changes smaller than this

// ─────────────────────────────────────────────────────────────────────────────
// BLE Gamepad Configuration
// ─────────────────────────────────────────────────────────────────────────────
#define BLE_DEVICE_NAME    "WorkerBee Gamepad"
#define BLE_MANUFACTURER   "Homecontrol"
#define BLE_REPORT_RATE_HZ 50     // 50Hz = 20ms between reports
#define BLE_AXIS_MIN       0
#define BLE_AXIS_MAX       255

// ─────────────────────────────────────────────────────────────────────────────
// WiFi Configuration
// ─────────────────────────────────────────────────────────────────────────────
#define WIFI_AP_NAME       "WorkerBee-Setup"
#define WIFI_AP_PASSWORD   ""     // Empty for open AP
#define WIFI_CONFIG_PORTAL_TIMEOUT 180  // 3 minutes

// ─────────────────────────────────────────────────────────────────────────────
// MQTT Configuration
// ─────────────────────────────────────────────────────────────────────────────
#define MQTT_BROKER_HOST   "192.168.0.95"  // Pi broker
#define MQTT_BROKER_PORT   1883
#define MQTT_KEEPALIVE     30
#define MQTT_BUFFER_SIZE   1024

// Topic patterns (+ is replaced with device_id)
#define MQTT_TOPIC_PREFIX       "homecontrol"
#define MQTT_TOPIC_DISCOVERY    "homecontrol/discovery/%s/config"
#define MQTT_TOPIC_STATE        "homecontrol/devices/%s/state"
#define MQTT_TOPIC_HEALTH       "homecontrol/devices/%s/health"
#define MQTT_TOPIC_AVAILABILITY "homecontrol/devices/%s/availability"
#define MQTT_TOPIC_LOGS         "homecontrol/devices/%s/logs"

// Subscription wildcards (to monitor all bees)
#define MQTT_SUB_ALL_DISCOVERY  "homecontrol/discovery/+/config"
#define MQTT_SUB_ALL_STATE      "homecontrol/devices/+/state"
#define MQTT_SUB_ALL_HEALTH     "homecontrol/devices/+/health"
#define MQTT_SUB_ALL_LOGS       "homecontrol/devices/+/logs"
#define MQTT_SUB_ALL_AVAIL      "homecontrol/devices/+/availability"
#define MQTT_SUB_SHELLY         "shelly#"

// ─────────────────────────────────────────────────────────────────────────────
// Health Reporting
// ─────────────────────────────────────────────────────────────────────────────
#define HEALTH_REPORT_INTERVAL_MS 5000   // Report every 5 seconds

// ─────────────────────────────────────────────────────────────────────────────
// UI Configuration
// ─────────────────────────────────────────────────────────────────────────────
#define UI_MAX_MESSAGES     50    // MQTT console message buffer
#define UI_MAX_DEVICES      16    // Max devices to track in grid
#define UI_REFRESH_RATE_MS  100   // UI refresh interval

// ─────────────────────────────────────────────────────────────────────────────
// Display Settings
// ─────────────────────────────────────────────────────────────────────────────
#define DISPLAY_WIDTH      320
#define DISPLAY_HEIGHT     240
#define DISPLAY_ROTATION   0      // 0, 1, 2, or 3 (90° increments)
#define BACKLIGHT_DEFAULT  200    // 0-255 brightness

// ─────────────────────────────────────────────────────────────────────────────
// Task Priorities (FreeRTOS)
// ─────────────────────────────────────────────────────────────────────────────
// Higher number = higher priority (0-24 on ESP32)
#define TASK_PRIORITY_MQTT     5   // Core 0 - Network tasks
#define TASK_PRIORITY_WIFI     4
#define TASK_PRIORITY_BLE      6   // Core 1 - App tasks
#define TASK_PRIORITY_UI       5
#define TASK_PRIORITY_INPUT    7   // Highest - pot sampling

// Task stack sizes (words, not bytes - multiply by 4)
#define TASK_STACK_MQTT   4096
#define TASK_STACK_BLE    4096
#define TASK_STACK_UI     8192
#define TASK_STACK_INPUT  2048

// Core assignment
#define CORE_NETWORK  0    // WiFi, MQTT run on Core 0
#define CORE_APP      1    // BLE, UI, Input run on Core 1

// ─────────────────────────────────────────────────────────────────────────────
// Debug Configuration
// ─────────────────────────────────────────────────────────────────────────────
#define DEBUG_SERIAL     true
#define DEBUG_BAUD_RATE  115200

#endif // CONFIG_H
