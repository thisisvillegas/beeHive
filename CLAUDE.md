# ESP32 Homecontrol Project

## Overview
ESP32-based home automation device that communicates via MQTT with the homecontrol Next.js web app running on remusPi.

## Architecture
```
┌─────────────┐      WiFi       ┌──────────────┐      MQTT      ┌─────────────┐
│   ESP32     │ ──────────────► │   Router     │                │  mosquitto  │
│ 192.168.0.63│                 │              │                │  (broker)   │
│             │ ── MQTT ──────────────────────────────────────► │  port 1883  │
└─────────────┘                 └──────────────┘                └─────────────┘
                                                                       ▲
                                                                       │ MQTT
                                                               ┌───────────────┐
                                                               │  homecontrol  │
                                                               │  (Next.js)    │
                                                               │  port 3003    │
                                                               └───────────────┘
```

## Server Details (remusPi)
- **IP**: 192.168.0.95
- **SSH**: `ssh remus@192.168.0.95`
- **Username**: remus (not pi)
- **homecontrol path**: `/home/remus/apps/homecontrol/`
- **Process manager**: pm2 (use `pm2 restart homecontrol`)
- **MQTT broker**: mosquitto on port 1883

## ESP32 Details
- **IP**: 192.168.0.63
- **Device ID**: 84d20c1f8a3c (from eFuse MAC)
- **Device Name**: SwitchRemusESP1
- **Web UI**: http://192.168.0.63/ (fast with async firmware)
- **Config UI**: http://192.168.0.63/config (change MQTT broker here)
- **API**: http://192.168.0.63/api/status (JSON status endpoint)
- **Framework**: Arduino via PlatformIO
- **Firmware**: 2.0.0-async (async MQTT + async WebServer)
- **MQTT Library**: espMqttClient (async, non-blocking)
- **Web Server**: ESPAsyncWebServer (non-blocking)
- **Keep-alive**: 60 seconds

## MQTT Topics
All topics use prefix `homecontrol/`:
- `homecontrol/discovery/{deviceId}/config` - Device discovery (retained)
- `homecontrol/devices/{deviceId}/state` - Device state (LED, uptime, etc.) (retained)
- `homecontrol/devices/{deviceId}/set` - Commands TO the device
- `homecontrol/devices/{deviceId}/availability` - online/offline status (retained)
- `homecontrol/devices/{deviceId}/health` - Health data every 5 seconds
- `homecontrol/devices/{deviceId}/logs` - Serial debug logs via MQTT

## Key Files

### ESP32 Firmware (local - /Users/andres/dev/esp32-homecontrol)
- `src/main_async.cpp` - **SwitchRemusESP1** (2.0.0-async) with async MQTT + async WebServer
- `src/main_c3.cpp` - **TinyBee1** (1.2.0-c3) 4-switch panel + Engine Start
- `src/main_tdisplay.cpp` - **kayciBee1** (3.2.0-tdisplay) T-Display S3 with battery + companion bee
- `src/main_bedlift.cpp` - **BedLiftBee** (1.2.0-bedlift) bed lift controller + safety sensors
- `src/main.cpp` - Legacy firmware with blocking PubSubClient + WebServer
- `src/main_minimal.cpp` - Stripped-down MQTT test firmware
- `tools/esp32-simulator.js` - **Full ESP32 simulator** with Web UI, MQTT, power toggle
- `tools/fake-esp32.js` - Simple MQTT-only fake ESP32 (deprecated, use simulator)
- `tools/mosquitto-local.conf` - Config for local Mac mosquitto broker

### homecontrol (remusPi - /home/remus/apps/homecontrol)
- `lib/mqtt.ts` - **Multi-broker MQTT service** (connects to Pi + Mac simultaneously)
- `lib/types.ts` - TypeScript types including ESP32Health, ESP32DeviceWithBroker
- `app/esp32/page.tsx` - ESP32 dashboard with multi-broker status, console, device cards
- `app/api/esp32/broker/route.ts` - Broker API (GET all brokers, POST to switch console view)
- `app/api/esp32/devices/route.ts` - List ESP32 devices (uses `force-dynamic`)
- `app/api/esp32/devices/[id]/route.ts` - Device detail API
- `app/api/esp32/control/route.ts` - Send commands to ESP32 (auto-routes to correct broker)
- `app/api/esp32/network/route.ts` - Network ping API for ESP32 devices

## ESP32 Simulator
Full-featured simulator for testing without real hardware:

```bash
cd /Users/andres/dev/esp32-homecontrol/tools
node esp32-simulator.js [broker-ip]  # Default: 192.168.0.95 (Pi)
```

**Web UI**: http://localhost:8032
- Power ON/OFF button - simulates device power cycling (abrupt disconnect)
- LED toggle - responds to MQTT commands from homecontrol
- Device status - shows MQTT connection, uptime, reconnects
- Config page - change broker, device ID, device name

**Features**:
- Publishes: discovery, state, health, availability, logs
- Subscribes to `/set` topic and responds to LED commands
- Config persists to `.esp32-simulator-config.json`
- Power OFF = abrupt disconnect (no goodbye message, like real power loss)
- Power ON = fresh boot with uptime reset

## Multi-Broker Architecture (DEPLOYED)
homecontrol connects to ALL configured brokers simultaneously:

**Brokers** (defined in `lib/mqtt.ts`):
- `pi` - mqtt://localhost:1883 (Pi's local mosquitto)
- `mac` - mqtt://192.168.0.77:1883 (Mac's local mosquitto)

**How it works**:
- Devices aggregated from all brokers with `broker` and `brokerUrl` fields
- Console dropdown switches which broker's messages to view (independent of device list)
- Commands auto-route to correct broker based on `device.broker`
- Device cards show which broker each device is connected to

**Important**: API routes must use `export const dynamic = 'force-dynamic'` to avoid static prerendering capturing empty device state at build time.

## ESP32 Dashboard Features (https://homecontrol.thisisvillegas.com/esp32)
- **Broker Status** - Shows connection status for all brokers (Pi, Mac)
- **Console Dropdown** - Switch which broker's messages to view
- **Device Cards** with:
  - Two status dots: blue (ping) + green/red (MQTT)
  - Health stats: RSSI, heap, uptime
  - Broker indicator (shows which broker device is on)
  - LED toggle button
  - "Details" button → links to /devices/{id}
  - "Web UI" button → links to ESP32 web interface
  - Web Serial support (Chrome/Edge) for USB debugging

## Known Issues

### ESP32 Pi Broker Connection - FIXED (Jan 2026)

**Root Cause:** Asymmetric routing on the Pi.

The Pi has two interfaces on the same subnet:
- eth0: 192.168.0.94 (metric 100 - preferred)
- wlan0: 192.168.0.95 (metric 600)

ESP32 connects to wlan0 (192.168.0.95), but Pi's responses were routed out eth0 due to lower metric. The ESP32 never received the MQTT CONNACK, kept retransmitting for ~15 seconds, then gave up.

**Symptoms:**
- ESP32 connected/disconnected every 15 seconds exactly
- mosquitto log showed "Client closed its connection"
- tcpdump showed: packets IN on wlan0, responses OUT on eth0

**Fix:** Add host route for ESP32 via wlan0:
```bash
sudo ip route add 192.168.0.63 dev wlan0
```

**Persistent fix:** Created `/lib/dhcpcd/dhcpcd-hooks/60-esp32-route` to add route on boot.

**Verify:**
```bash
ip route get 192.168.0.63  # Should show "dev wlan0"
```

## Local MQTT Testing Setup
For isolated testing without the Pi:

```bash
# Start local broker on Mac (192.168.0.77)
mosquitto -c /Users/andres/dev/esp32-homecontrol/tools/mosquitto-local.conf -v

# Run ESP32 simulator against Pi broker
cd /Users/andres/dev/esp32-homecontrol/tools
node esp32-simulator.js 192.168.0.95

# Or against local Mac broker
node esp32-simulator.js 192.168.0.77

# Point real ESP32 to different broker
# Go to http://192.168.0.63/config and change MQTT broker address
```

## PlatformIO Environments
Firmware environments defined in `platformio.ini`:

| Environment | Firmware | Board | Use Case |
|-------------|----------|-------|----------|
| `async` | main_async.cpp | ESP32-WROOM | **SwitchRemusESP1** - Production firmware |
| `esp32dev` | main.cpp | ESP32-WROOM | Legacy blocking firmware |
| `minimal` | main_minimal.cpp | ESP32-WROOM | MQTT connectivity testing |
| `c3` | main_c3.cpp | ESP32-C3 Mini | **TinyBee1** - 4-switch panel |
| `tdisplay-s3` | main_tdisplay.cpp | T-Display S3 | **kayciBee1** - Display + battery |
| `bedlift` | main_bedlift.cpp | ESP32-C3 Mini | **BedLiftBee** - Bed lift controller |

```bash
# SwitchRemusESP1 (OG device)
pio run -e async --target upload

# TinyBee1 (4-switch panel)
pio run -e c3 --target upload

# kayciBee1 (T-Display S3 with battery)
pio run -e tdisplay-s3 --target upload

# BedLiftBee (bed lift controller)
pio run -e bedlift --target upload

# Legacy/testing
pio run -e esp32dev --target upload
pio run -e minimal --target upload
```

## Build Commands
```bash
# Flash ESP32 (uses default environment)
pio run --target upload

# Monitor serial (requires interactive terminal)
pio device monitor --baud 115200

# Restart homecontrol
ssh remus@192.168.0.95 "pm2 restart homecontrol"

# Check homecontrol logs
ssh remus@192.168.0.95 "pm2 logs homecontrol --lines 30"

# Subscribe to MQTT topics for debugging
ssh remus@192.168.0.95 "mosquitto_sub -h localhost -t 'homecontrol/#' -v"

# Rebuild homecontrol after changes
ssh remus@192.168.0.95 "cd /home/remus/apps/homecontrol && npm run build && pm2 restart homecontrol"
```

## Editing Files on Pi
Don't use sed for complex edits on Pi - it gets messy with escaping. Instead:
```bash
# Download
scp remus@192.168.0.95:/home/remus/apps/homecontrol/path/to/file.ts /tmp/file.ts

# Edit locally
# ... make changes ...

# Upload
scp /tmp/file.ts remus@192.168.0.95:/home/remus/apps/homecontrol/path/to/file.ts

# Rebuild
ssh remus@192.168.0.95 "cd /home/remus/apps/homecontrol && npm run build && pm2 restart homecontrol"
```

## Future Improvements (Noted)
- **ESP32 Web UI optimization** - Further improvements possible:
  - Minify HTML/CSS
  - ~~Use ESPAsyncWebServer instead of WebServer~~ (DONE - 2.0.0-async)
  - Add AJAX updates instead of full page reloads
  - Enable gzip compression
- ~~**Investigate Pi broker disconnect**~~ (FIXED - asymmetric routing, see Known Issues)
- **Add more simulators** - Support multiple simulated devices for testing

## Debug Features
1. **debugLog()** - Timestamped logging to Serial AND MQTT
2. **getMqttStateString()** - Human-readable MQTT state codes
3. **Log buffer** - Stores last 20 logs, publishes on reconnect
4. **Health topic** - Publishes every 5 seconds with full device status
5. **reconnectCount** - Tracks MQTT reconnection attempts
6. **lastError** - Stores last MQTT error message
7. **Broker console** - Raw MQTT message log in homecontrol UI (per-broker)
8. **Web Serial** - USB serial logging in browser (Chrome/Edge)
9. **ESP32 Simulator** - Full device simulation with power toggle for testing
10. **MQTT Logging** - All Bees publish logs via MQTT (see below)

### MQTT Logging (v1.2.0 Bees)

All three Bee devices (TinyBee1, kayciBee1, BedLiftBee) now support buffered MQTT logging for remote debugging.

**How it works:**
- Circular buffer holds up to 20 log entries
- Logs published in batch every 500ms to reduce MQTT traffic
- Each log entry includes timestamp (uptime in ms)
- Publishes to: `homecontrol/devices/{deviceId}/logs`

**Key functions:**
```cpp
void mqttLog(const char* format, ...);       // Printf-style logging
void publishLog(const char* msg);            // Add to buffer
void publishBufferedLogs();                  // Flush buffer (called in loop)
```

**Log message format:**
```json
{
  "logs": [
    {"ts": 12345, "msg": "WiFi connected"},
    {"ts": 12500, "msg": "MQTT connected to broker"}
  ]
}
```

**Viewing logs:**
- homecontrol dashboard DeviceLogs component shows real-time logs
- Or subscribe directly: `mosquitto_sub -h 192.168.0.95 -t 'homecontrol/devices/+/logs' -v`

**Safety events logged (BedLiftBee):**
- Motor start/stop with reason (timer, limit, obstacle, manual)
- Obstacle detection alerts
- Reed switch limit triggers

---

## The Bee Colony

All ESP32 devices in the homecontrol ecosystem:

| Device | Board | IP | Device ID | Firmware | Function |
|--------|-------|-----|-----------|----------|----------|
| **SwitchRemusESP1** | ESP32-WROOM | 192.168.0.63 | 84d20c1f8a3c | 2.0.0-async | OG device, LED control |
| **TinyBee1** | ESP32-C3 Mini | 192.168.0.111 | 503035d4db1c | 1.2.0-c3 | 4-switch panel + Engine Start (sends love bombs) |
| **kayciBee1** | T-Display S3 | 192.168.0.243 | c404f416a398 | 3.2.0-tdisplay | 170x320 display with battery, companion bee ping |
| **BedLiftBee** | ESP32-C3 Mini | 192.168.0.87 | 2ce238d4db1c | 1.2.0-bedlift | Happy Jack bed lift controller + safety sensors |

### kayciBee1 Details

Personal display device with battery monitoring, companion bee communication, and animated views.

**Hardware:**
- LILYGO T-Display S3 (ESP32-S3 + 1.9" 170x320 TFT)
- 3000mAh LiPo battery connected via JST
- Built-in buttons (GPIO0, GPIO14)

**GPIO Mapping:**
```
Display (built-in):
- TFT via SPI (configured by TFT_eSPI library)
- Backlight PWM

Buttons:
- GPIO0 → Left button (info view toggle)
- GPIO14 → Right button (ping companion bee)

Battery:
- GPIO4 → ADC input (voltage divider: 2x 100kΩ)
```

**Battery Monitoring (v3.2.0):**
- ADC reads battery voltage via voltage divider (2:1 ratio)
- Calibrated for LiPo range: 3.0V (0%) to 4.2V (100%)
- Charging detection: voltage > 4.25V indicates USB power
- Battery icon shows fill level + percentage text
- "USB" displayed when on USB power without battery

**Display Layout:**
```
┌─────────────────────┐
│   [Battery Icon]    │  ← Top right corner
│                     │
│   🧑‍🍳 Dancing Chef   │  ← y=45
│                     │
│      12:34 PM       │  ← y=120 (centered time)
│                     │
│  WiFi ✓  MQTT ✓     │  ← y=195 (status labels)
│     TinyBee ✓       │
│                     │
│   🌅 Sunset 5:42pm  │  ← y=250 (sun info)
│                     │
│  < Info    Ping >   │  ← Footer buttons
└─────────────────────┘
```

**Companion Bee Feature (v3.2.0):**
- Configurable companion bee ID via web UI at /config
- Default: TinyBee1 (503035d4db1c)
- Right button sends ping to companion bee's /set topic
- Subscribes to companion bee's /availability topic
- Status shown on display: "TinyBee ✓" or "TinyBee ✗"

**Web UI:** http://192.168.0.243
- Main status display
- Settings at /config (WiFi, MQTT, Companion Bee)

**MQTT Topics:**
```
homecontrol/devices/c404f416a398/state      - Device state
homecontrol/devices/c404f416a398/health     - Health data (includes battery)
homecontrol/devices/c404f416a398/logs       - MQTT logging
homecontrol/devices/c404f416a398/set        - Commands TO device
homecontrol/devices/{companionId}/set       - Ping sent here
homecontrol/devices/{companionId}/availability - Subscribes to this
```

**Health Message includes:**
```json
{
  "rssi": -45,
  "heap": 180000,
  "uptime": 3600,
  "batteryVoltage": 3.85,
  "batteryPercent": 70,
  "batteryCharging": false
}
```

**Firmware:** `src/main_tdisplay.cpp`
**PlatformIO env:** `pio run -e tdisplay-s3 --target upload`

---

### BedLiftBee Details

Controls a Happy Jack RV bed lift via 2-channel relay module simulating rocker switch presses. Includes safety sensors for position tracking and obstacle detection.

**Hardware:**
- ESP32-C3 Mini
- 2-channel relay module (SRD-05VDC-SL-C)
- Happy Jack bed lift rocker switch
- HC-SR04 ultrasonic sensor (obstacle detection)
- Reed switches x2 (top/bottom limit detection)

**GPIO Mapping:**
```
Relays:
- GPIO2 → Relay 1 IN (UP - raises bed)
- GPIO3 → Relay 2 IN (DOWN - lowers bed)

Safety Sensors:
- GPIO4 → HC-SR04 TRIG
- GPIO5 → HC-SR04 ECHO (use voltage divider 5V→3.3V)
- GPIO6 → Reed switch TOP limit
- GPIO7 → Reed switch BOTTOM limit
- GPIO8 → Onboard LED
```

**Relay Wiring to Switch:**
```
Relay 1 COM → Switch BOTTOM terminal (UP)
Relay 2 COM → Switch TOP terminal (DOWN)
Relay 1 NO + Relay 2 NO (bridged) → Switch MIDDLE terminal (COM)
```

**Safety Features (v1.1.0):**
- Ultrasonic obstacle detection (stops if < 10cm when lowering)
- Reed switch limit detection (stops at top/bottom)
- Position tracking (0-100% after calibration)
- Auto-stop reasons logged: timer, top_limit, bottom_limit, obstacle, manual

**Web UI:** http://192.168.0.87
- RAISE / LOWER / STOP buttons
- Configurable lift duration (default: 3 seconds)
- Settings at /config
- Calibration at /calibrate?pos=top or /calibrate?pos=bottom

**MQTT Commands:**
```bash
# Raise bed
mosquitto_pub -h 192.168.0.95 -t "homecontrol/devices/2ce238d4db1c/set" \
  -m '{"capability":{"instance":"bedLift","value":"raise"}}'

# Lower bed
mosquitto_pub -h 192.168.0.95 -t "homecontrol/devices/2ce238d4db1c/set" \
  -m '{"capability":{"instance":"bedLift","value":"lower"}}'

# Stop
mosquitto_pub -h 192.168.0.95 -t "homecontrol/devices/2ce238d4db1c/set" \
  -m '{"capability":{"instance":"bedLift","value":"stop"}}'

# Calibrate positions
mosquitto_pub -h 192.168.0.95 -t "homecontrol/devices/2ce238d4db1c/set" \
  -m '{"capability":{"instance":"bedLift","value":"calibrate_top"}}'
mosquitto_pub -h 192.168.0.95 -t "homecontrol/devices/2ce238d4db1c/set" \
  -m '{"capability":{"instance":"bedLift","value":"calibrate_bottom"}}'
```

**Firmware:** `src/main_bedlift.cpp`
**PlatformIO env:** `pio run -e bedlift --target upload`

### BedLiftBee Modular Wiring System

The bed lift uses a modular connector system for clean, safe, and serviceable wiring.

**System Architecture:**
```
                              WALL PANEL
                         ┌─────────────────┐
Happy Jack ══► WAGO ═════╪═► Old Switch    │ (always connected - backup)
               ║         │                 │
               ║         │   [TOGGLE]      │
               ║         │    ╱    ╲       │
               ╠═════════╪══╱      ╲═══════╪═► "FULL SEND" GX16-4 MALE
               ║ UP/DOWN │ (off)   (on)    │      (UP + DOWN + COM x2)
               ║         │          ║      │           ║
               ╚═════════╪══════════╬══════╪═► "COM ONLY" GX16-4 MALE
                  COM    │          ║      │      (just COM doubled)
                         └──────────╬──────┘
                                    ║
                            Long cable to TinyBee1
                            Dashboard (kill switch)
```

**Connector Standard: GX16-4**
- 4-pin aviation connector
- Male on panel/enclosure (pins protected inside)
- Female-to-female cables (safe when disconnected)
- Pin 1: UP, Pin 2: DOWN, Pin 3: COM, Pin 4: COM (doubled for current)

**Wire Specifications:**
| Run | Wire | Gauge | Notes |
|-----|------|-------|-------|
| Motor power (GX16) | 4-conductor stranded | 16 AWG | Handles 10-15A motor current |
| ESP ↔ Relay (JST-XH) | Silicone stranded | 22 AWG | Signal/control only |
| Perfboard traces | Solid core | 22 AWG | Internal board wiring |

**TinyBee1 Dashboard Integration:**
- Physical kill switch wired to "COM ONLY" outlet
- When wall toggle ON + kill switch pressed → breaks COM → bed stops
- Hardware safety that works even if WiFi is down
- Future: LED indicator for kill switch active state

**Shopping List:**
```
CONNECTORS:
□ GX16-4 male panel mount (x4) - wall x2, BedLiftBee x1, TinyBee1 x1
□ GX16-4 female cable connector with 90° boot (x6) - for cables
□ JST-XH 2.54mm connector kit (2/3/4 pin) - internal wiring

WIRE:
□ 16 AWG 4-conductor stranded cable (30ft) - motor power runs
□ 22 AWG solid core hookup wire kit - perfboard traces
□ JST-XH pre-crimped 22 AWG silicone (included in kit)

WALL PANEL:
□ Plastic junction box (weatherproof)
□ SPDT toggle switch (15A) - routes COM between outlets
□ Wago connectors (5-port)

ENCLOSURES:
□ Project box for BedLiftBee
□ SendCutSend laser cut panel for TinyBee1 Dashboard
□ Design tool: Inkscape (free) for panel layout
```

---

## Bee Templates

Two base templates for creating new bees. Located in `/templates/`.

### Basic Bee (`templates/basic-bee.cpp`)

Core functionality for ALL bees:

| Feature | Description |
|---------|-------------|
| WiFiManager | Captive portal for WiFi setup |
| espMqttClient | Async, non-blocking MQTT |
| ESPAsyncWebServer | Status/config web UI |
| Preferences | Persistent config storage |
| MQTT Logging | Buffered log publishing |
| Health/State | Standard publishing patterns |
| mDNS | Local network discovery |
| Battery Monitoring | Optional - set `BATTERY_ADC_PIN` (-1 to disable) |
| Companion Bee | Track another bee's online status, send pings |

**Use for:** Sensor bees, relay controllers, headless devices, any ESP32

### Display Bee (`templates/display-bee.cpp`)

Everything from Basic Bee PLUS display-specific features:

| Feature | Description |
|---------|-------------|
| TFT_eSPI + Sprite | Flicker-free display rendering |
| Auto-Dimming | Time-based brightness schedule (configurable hours) |
| Screen Management | Multiple views with switching |
| spriteValid Guard | Prevents crashes on invalid sprite |
| Battery Icon | Visual battery indicator on screen |
| Companion Status | Show companion online/offline on display |

**Use for:** T-Display S3, any TFT-equipped ESP32

### Creating a New Bee

1. Copy appropriate template
2. Update `DEVICE_TYPE` and `FIRMWARE_VERSION`
3. Add GPIO pins to defines section
4. Extend `DeviceConfig` struct with your settings
5. Extend `DeviceState` struct with your state
6. Implement command handling in `onMqttMessage()`
7. Add your sensor/actuator logic to `setup()` and `loop()`
8. Add PlatformIO environment to `platformio.ini`

### Template Features Quick Reference

```cpp
// MQTT Logging (works offline, publishes when connected)
mqttLog("[Sensor] Reading: %d\n", value);

// Save/Load Config (survives reboots)
config.mySetting = 42;
saveConfig();

// Publish State Change
state.myValue = newValue;
publishState();

// Display Bee: Ping Companion
pingCompanionBee();

// Display Bee: Force Display Update
state.displayNeedsUpdate = true;
```

---

## Electronics Inventory

### Microcontrollers & Dev Boards

| Board | Chip | Status | Notes |
|-------|------|--------|-------|
| ESP32-WROOM-32 | ESP32 (Xtensa dual-core) | In use | SwitchRemusESP1 |
| ESP32-C3 Mini | ESP32-C3 (RISC-V single-core) | In use (x2) | TinyBee1, BedLiftBee |
| LILYGO T-Display S3 | ESP32-S3 + 1.9" TFT | In use | kayciBee1 |
| ESP32-C3 Mini | ESP32-C3 | Spare | Available for projects |

### Sensors - Distance & Motion

| Sensor | Type | Interface | Use Case |
|--------|------|-----------|----------|
| **HC-SR04** | Ultrasonic distance | GPIO (Trig/Echo) | Distance 2-400cm, bed lift safety |
| Accelerometer | Motion/tilt | I2C | Orientation, impact detection |
| Shock sensor | Vibration/impact | Digital | Collision detection |
| Tilt switch | Mercury/ball tilt | Digital | Angle detection |
| Ball switch | Tilt | Digital | Orientation |
| PIR (if in kit) | Motion | Digital | Presence detection |

### Sensors - Environmental

| Sensor | Type | Interface | Use Case |
|--------|------|-----------|----------|
| **GY-BME280** | Pressure/Temp/Humidity | I2C | Weather station, altitude |
| **AHT20** | Temp/Humidity | I2C | Climate monitoring |
| DS18B20 | Temperature (waterproof) | 1-Wire | Precise temp, liquids |
| DHT11/22 style | Temp/Humidity | Digital | Basic climate |
| Analog temp | Thermistor | Analog | Simple temp sensing |
| Flame sensor | IR flame detect | Analog/Digital | Fire detection |

### Sensors - Light & IR

| Sensor | Type | Interface | Use Case |
|--------|------|-----------|----------|
| **VS1838B** | IR receiver | Digital | Remote control input |
| IR emission | IR LED | Digital | Remote control output |
| Photoresistor (LDR) | Light level | Analog | Ambient light sensing |
| Light blocking | Break-beam | Digital | Object detection |
| Light cup | Tilt + light | Digital | Light-activated tilt |

### Sensors - Sound

| Sensor | Type | Interface | Use Case |
|--------|------|-----------|----------|
| **LM393 sound module** | Microphone + comparator | Digital/Analog | Sound detection, clap switch |
| Big sound | Microphone | Digital/Analog | Loud sound detection |
| Small sound | Microphone | Digital/Analog | Quiet sound detection |

### Sensors - Magnetic & Touch

| Sensor | Type | Interface | Use Case |
|--------|------|-----------|----------|
| Reed switch | Magnetic | Digital | Door/window sensor |
| Mini reed | Magnetic (small) | Digital | Compact door sensor |
| Hall effect (linear) | Magnetic field | Analog | Position sensing |
| Hall effect (digital) | Magnetic | Digital | Magnet detection |
| Touch sensor | Capacitive | Digital | Touch buttons |

### Sensors - Other

| Sensor | Type | Interface | Use Case |
|--------|------|-----------|----------|
| Heartbeat | Optical pulse | Analog | Heart rate (finger) |
| Tracking | IR reflective | Digital | Line following |
| Rotary encoder | Rotation | Digital (A/B/SW) | Dials, volume control |
| Joystick | 2-axis + button | Analog + Digital | Game controller |

### Output Modules

| Module | Type | Interface | Use Case |
|--------|------|-----------|----------|
| **2-channel relay** | SRD-05VDC-SL-C | Digital | AC/DC switching (BedLiftBee) |
| **MCP4725** | 12-bit DAC | I2C | Analog voltage output |
| **LM386** | Audio amplifier | Analog | Speaker driver |
| RGB LED | Common cathode | Digital PWM | Color lighting |
| SMD RGB | Surface mount | Digital PWM | Compact color LED |
| Two-color LED | Red/Green | Digital | Status indicator |
| Laser emit | Red laser | Digital | Pointer, alignment |
| 7-color flash | Auto-cycle LED | Digital | Decorative |
| Buzzer (active) | Piezo | Digital | Beeps, alarms |
| Buzzer (passive) | Piezo | PWM | Tones, melodies |
| Relay (from kit) | Single channel | Digital | Switching |

### Smart Home Devices

| Device | Model | Protocol | Status |
|--------|-------|----------|--------|
| **Shelly Plus RGBW PM** | Gen2 | MQTT | Connected (low-side switch - wrong for 12V) |
| Shelly Plus 1 | Gen2 | MQTT | Recommended for 12V switching |

---

## Recommended Sensor for BedLiftBee Safety

**HC-SR04 Ultrasonic** is ideal for preventing bed crush incidents:

```
Mount on bed frame pointing DOWN
        │
        ▼
   ┌─────────┐
   │ HC-SR04 │  ← Measures distance to floor/objects
   └────┬────┘
        │
   Safe zone > 10cm
        │
   ─────┴───── Objects/Floor
```

**Wiring to ESP32-C3:**
- VCC → 5V
- GND → GND
- TRIG → GPIO4
- ECHO → GPIO5 (use voltage divider: 5V → 3.3V)

**Safety Logic:**
- Continuously measure distance while lifting
- If distance < threshold (e.g., 10cm) → emergency STOP
- Prevent lowering if obstacle detected below
