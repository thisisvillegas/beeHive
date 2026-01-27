#!/usr/bin/env node
/**
 * Fake ESP32 MQTT Publisher
 * Tests the MQTT broker and homecontrol app without a real ESP32
 *
 * Usage: node fake-esp32.js [broker-ip]
 * Default broker: 192.168.0.95
 */

const mqtt = require('mqtt');

const BROKER_IP = process.argv[2] || '192.168.0.95';
const BROKER_URL = `mqtt://${BROKER_IP}:1883`;
const DEVICE_ID = 'fake-esp32-001';
const DEVICE_NAME = 'FakeESP32';

console.log(`\n🔌 Connecting to MQTT broker at ${BROKER_URL}...`);

const client = mqtt.connect(BROKER_URL, {
  clientId: `fake-esp32-${Date.now()}`,
  clean: true,
  connectTimeout: 10000,
  reconnectPeriod: 5000,
});

let uptime = 0;
let ledState = false;

// Topics
const topics = {
  discovery: `homecontrol/discovery/${DEVICE_ID}/config`,
  state: `homecontrol/devices/${DEVICE_ID}/state`,
  health: `homecontrol/devices/${DEVICE_ID}/health`,
  availability: `homecontrol/devices/${DEVICE_ID}/availability`,
  command: `homecontrol/devices/${DEVICE_ID}/set`,
  logs: `homecontrol/devices/${DEVICE_ID}/logs`,
};

// Discovery payload (like real ESP32)
const discoveryPayload = {
  device_id: DEVICE_ID,
  name: DEVICE_NAME,
  type: 'esp32',
  model: 'FakeESP32-Test',
  firmware_version: '1.0.0-fake',
  ip_address: '127.0.0.1',
  capabilities: [{ type: 'devices.capabilities.on_off', instance: 'powerSwitch' }],
  sensors: [{ type: 'led', instance: 'builtin', gpio: 2 }],
  timestamp: 0,
};

function publishDiscovery() {
  discoveryPayload.timestamp = uptime;
  client.publish(topics.discovery, JSON.stringify(discoveryPayload), { retain: true });
  console.log(`📡 Published discovery`);
}

function publishState() {
  const state = {
    powerSwitch: ledState ? 1 : 0,
    rssi: -50 + Math.floor(Math.random() * 20), // Simulate varying signal
    uptime: uptime,
    ip: '127.0.0.1',
    timestamp: uptime,
  };
  client.publish(topics.state, JSON.stringify(state), { retain: true });
  console.log(`📊 State: LED=${ledState ? 'ON' : 'OFF'}, uptime=${uptime}s`);
}

function publishHealth() {
  const health = {
    uptime: uptime,
    wifi_rssi: -50 + Math.floor(Math.random() * 20),
    wifi_connected: true,
    mqtt_connected: true,
    mqtt_state: 'CONNECTED',
    free_heap: 200000 + Math.floor(Math.random() * 20000),
    ip: '127.0.0.1',
    reconnect_count: 0,
    last_error: 'none',
    led_state: ledState,
    timestamp: uptime,
  };
  client.publish(topics.health, JSON.stringify(health));
  console.log(`💓 Health: heap=${health.free_heap}, rssi=${health.wifi_rssi}`);
}

function publishLog(msg) {
  const log = { ts: uptime * 1000, msg: `[${uptime}] ${msg}` };
  client.publish(topics.logs, JSON.stringify(log));
}

function publishAvailability(online) {
  client.publish(topics.availability, online ? 'online' : 'offline', { retain: true });
  console.log(`🔔 Availability: ${online ? 'ONLINE' : 'OFFLINE'}`);
}

client.on('connect', () => {
  console.log(`✅ Connected to broker!\n`);

  // Subscribe to command topic
  client.subscribe(topics.command, (err) => {
    if (!err) {
      console.log(`📥 Subscribed to ${topics.command}`);
    }
  });

  // Publish initial messages
  publishAvailability(true);
  publishDiscovery();
  publishState();
  publishHealth();
  publishLog('Fake ESP32 connected and running');

  // Publish state every 5 seconds
  setInterval(() => {
    uptime += 5;
    publishHealth();
  }, 5000);

  // Publish state every 30 seconds
  setInterval(() => {
    publishState();
  }, 30000);

  console.log(`\n🎮 Fake ESP32 is running. Press Ctrl+C to stop.`);
  console.log(`   Health publishes every 5s, state every 30s\n`);
});

client.on('message', (topic, message) => {
  console.log(`\n📨 Received command: ${message.toString()}`);
  try {
    const cmd = JSON.parse(message.toString());
    if (cmd.instance === 'powerSwitch') {
      ledState = cmd.value === 1;
      console.log(`💡 LED toggled to: ${ledState ? 'ON' : 'OFF'}`);
      publishState();
      publishHealth();
      publishLog(`LED set to ${ledState ? 'ON' : 'OFF'}`);
    }
  } catch (e) {
    console.error('Failed to parse command:', e.message);
  }
});

client.on('error', (err) => {
  console.error(`❌ MQTT Error: ${err.message}`);
});

client.on('close', () => {
  console.log('🔌 Connection closed');
});

client.on('reconnect', () => {
  console.log('🔄 Reconnecting...');
});

// Clean shutdown
process.on('SIGINT', () => {
  console.log('\n\n👋 Shutting down fake ESP32...');
  publishAvailability(false);
  publishLog('Fake ESP32 disconnecting');
  setTimeout(() => {
    client.end();
    process.exit(0);
  }, 500);
});
