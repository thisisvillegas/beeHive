#!/usr/bin/env node
/**
 * ESP32 Simulator - Functional clone with Web UI and MQTT
 *
 * Usage: node esp32-simulator.js [broker-ip]
 * Default broker: 192.168.0.95 (Pi)
 *
 * Web UI: http://localhost:8032
 */

const http = require('http');
const mqtt = require('mqtt');
const fs = require('fs');
const path = require('path');

// Config file for persistence
const CONFIG_FILE = path.join(__dirname, '.esp32-simulator-config.json');

// Default config
let config = {
  deviceId: 'sim-esp32-001',
  deviceName: 'SimulatedESP32',
  mqttServer: process.argv[2] || '192.168.0.95',
  mqttPort: 1883,
  webPort: 8032,
};

// Device state
let state = {
  ledOn: false,
  uptime: 0,
  startTime: Date.now(),
  reconnectCount: 0,
  lastError: 'none',
  powered: true, // Simulated power state
};

// MQTT client
let mqttClient = null;
let mqttConnected = false;

// Load config if exists
function loadConfig() {
  try {
    if (fs.existsSync(CONFIG_FILE)) {
      const saved = JSON.parse(fs.readFileSync(CONFIG_FILE, 'utf8'));
      config = { ...config, ...saved };
      // Allow command line to override saved config
      if (process.argv[2]) {
        config.mqttServer = process.argv[2];
      }
    }
  } catch (e) {
    console.log('Using default config');
  }
}

function saveConfig() {
  try {
    fs.writeFileSync(CONFIG_FILE, JSON.stringify(config, null, 2));
  } catch (e) {
    console.error('Failed to save config:', e.message);
  }
}

// Random helpers for realistic values
function randomRSSI() {
  return -30 - Math.floor(Math.random() * 40); // -30 to -70
}

function randomHeap() {
  return 200000 + Math.floor(Math.random() * 20000); // 200k-220k
}

function getUptime() {
  return Math.floor((Date.now() - state.startTime) / 1000);
}

// MQTT Functions
function getTopicPrefix() {
  return 'homecontrol';
}

function publishDiscovery() {
  if (!mqttClient || !mqttConnected) return;

  const discovery = {
    device_id: config.deviceId,
    name: config.deviceName,
    model: 'ESP32-Simulator',
    firmware_version: '1.0.0-sim',
    ip_address: `127.0.0.1:${config.webPort}`,
    capabilities: [
      { type: 'devices.capabilities.on_off', instance: 'powerSwitch' }
    ],
  };

  mqttClient.publish(
    `${getTopicPrefix()}/discovery/${config.deviceId}/config`,
    JSON.stringify(discovery),
    { retain: true, qos: 1 }
  );
  console.log('📡 Published discovery');
}

function publishState() {
  if (!mqttClient || !mqttConnected) return;

  const stateMsg = {
    led: state.ledOn ? 1 : 0,
    uptime: getUptime(),
    ip: `127.0.0.1:${config.webPort}`,
  };

  mqttClient.publish(
    `${getTopicPrefix()}/devices/${config.deviceId}/state`,
    JSON.stringify(stateMsg),
    { retain: true, qos: 1 }
  );
  console.log(`📊 State: LED=${state.ledOn ? 'ON' : 'OFF'}, uptime=${getUptime()}s`);
}

function publishHealth() {
  if (!mqttClient || !mqttConnected) return;

  const health = {
    uptime: getUptime(),
    wifi_rssi: randomRSSI(),
    wifi_connected: true,
    mqtt_connected: mqttConnected,
    mqtt_state: mqttConnected ? 'CONNECTED' : 'DISCONNECTED',
    free_heap: randomHeap(),
    ip: `127.0.0.1:${config.webPort}`,
    reconnect_count: state.reconnectCount,
    last_error: state.lastError,
    led_state: state.ledOn,
    timestamp: Date.now(),
  };

  mqttClient.publish(
    `${getTopicPrefix()}/devices/${config.deviceId}/health`,
    JSON.stringify(health),
    { qos: 0 }
  );
  console.log(`💓 Health: LED=${state.ledOn ? 'ON' : 'OFF'}, heap=${health.free_heap}, rssi=${health.wifi_rssi}`);
}

function publishAvailability(online) {
  if (!mqttClient) return;

  mqttClient.publish(
    `${getTopicPrefix()}/devices/${config.deviceId}/availability`,
    online ? 'online' : 'offline',
    { retain: true, qos: 1 }
  );
  console.log(`🔔 Availability: ${online ? 'ONLINE' : 'OFFLINE'}`);
}

function publishLog(msg) {
  if (!mqttClient || !mqttConnected) return;

  const logEntry = {
    ts: getUptime(),
    msg: `[${getUptime()}] ${msg}`,
  };

  mqttClient.publish(
    `${getTopicPrefix()}/devices/${config.deviceId}/logs`,
    JSON.stringify(logEntry),
    { qos: 0 }
  );
}

function handleCommand(payload) {
  try {
    const cmd = JSON.parse(payload);
    console.log('📥 Command received:', JSON.stringify(cmd));

    if (cmd.capability) {
      const { type, instance, value } = cmd.capability;

      if (type === 'devices.capabilities.on_off' && instance === 'powerSwitch') {
        const newState = value === 1 || value === true || value === 'on';
        state.ledOn = newState;
        console.log(`💡 LED toggled: ${state.ledOn ? 'ON' : 'OFF'}`);
        publishLog(`LED set to ${state.ledOn ? 'ON' : 'OFF'} via MQTT command`);
        publishState();
        publishHealth();
      }
    }
  } catch (e) {
    console.error('Failed to parse command:', e.message);
  }
}

function powerOff() {
  if (!state.powered) return;
  state.powered = false;
  console.log('\n⚡ POWER OFF - Simulating device shutdown');

  if (mqttClient && mqttConnected) {
    // Publish offline before disconnecting (like real device losing power wouldn't do)
    // Actually, real power loss = no goodbye, so just disconnect abruptly
    mqttClient.end(true); // force close
    mqttClient = null;
    mqttConnected = false;
  }
}

function powerOn() {
  if (state.powered) return;
  state.powered = true;
  state.startTime = Date.now(); // Reset uptime
  state.reconnectCount = 0;
  console.log('\n⚡ POWER ON - Simulating device boot');
  connectMQTT();
}

function connectMQTT() {
  if (!state.powered) {
    console.log('⚡ Cannot connect - device is powered off');
    return;
  }

  const brokerUrl = `mqtt://${config.mqttServer}:${config.mqttPort}`;
  console.log(`\n🔌 Connecting to MQTT broker at ${brokerUrl}...`);

  if (mqttClient) {
    mqttClient.end(true);
  }

  mqttClient = mqtt.connect(brokerUrl, {
    clientId: `${config.deviceId}-${Date.now()}`,
    clean: true,
    reconnectPeriod: 5000,
    will: {
      topic: `${getTopicPrefix()}/devices/${config.deviceId}/availability`,
      payload: 'offline',
      retain: true,
      qos: 1,
    },
  });

  mqttClient.on('connect', () => {
    console.log('✅ Connected to MQTT broker!');
    mqttConnected = true;

    // Subscribe to commands
    const cmdTopic = `${getTopicPrefix()}/devices/${config.deviceId}/set`;
    mqttClient.subscribe(cmdTopic, (err) => {
      if (err) {
        console.error('Subscribe error:', err);
      } else {
        console.log(`📥 Subscribed to ${cmdTopic}`);
      }
    });

    // Publish initial state
    publishAvailability(true);
    publishDiscovery();
    publishState();
    publishHealth();
  });

  mqttClient.on('message', (topic, payload) => {
    if (topic.endsWith('/set')) {
      handleCommand(payload.toString());
    }
  });

  mqttClient.on('error', (err) => {
    console.error('❌ MQTT Error:', err.message);
    state.lastError = err.message;
    mqttConnected = false;
  });

  mqttClient.on('close', () => {
    console.log('🔌 MQTT Disconnected');
    mqttConnected = false;
    state.reconnectCount++;
  });

  mqttClient.on('reconnect', () => {
    console.log('🔄 MQTT Reconnecting...');
  });
}

// Web Server
function createWebServer() {
  const server = http.createServer((req, res) => {
    const url = new URL(req.url, `http://localhost:${config.webPort}`);

    // Parse form data for POST
    let body = '';
    req.on('data', chunk => { body += chunk; });

    req.on('end', () => {
      // Route handling
      if (url.pathname === '/' && req.method === 'GET') {
        serveHomePage(res);
      } else if (url.pathname === '/config' && req.method === 'GET') {
        serveConfigPage(res);
      } else if (url.pathname === '/config' && req.method === 'POST') {
        handleConfigPost(body, res);
      } else if (url.pathname === '/toggle' && req.method === 'POST') {
        handleToggle(res);
      } else if (url.pathname === '/power' && req.method === 'POST') {
        handlePower(res);
      } else if (url.pathname === '/api/state' && req.method === 'GET') {
        serveState(res);
      } else {
        res.writeHead(404);
        res.end('Not Found');
      }
    });
  });

  server.listen(config.webPort, () => {
    console.log(`\n🌐 Web UI: http://localhost:${config.webPort}`);
    console.log(`   Config: http://localhost:${config.webPort}/config`);
  });
}

function serveHomePage(res) {
  const html = `<!DOCTYPE html>
<html>
<head>
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>${config.deviceName} - Simulator</title>
  <style>
    body { font-family: -apple-system, sans-serif; background: #1a1a1a; color: #fff; margin: 0; padding: 20px; }
    .container { max-width: 400px; margin: 0 auto; }
    h1 { color: #3b82f6; margin-bottom: 5px; }
    .subtitle { color: #666; margin-bottom: 20px; font-size: 14px; }
    .sim-badge { background: #f59e0b; color: #000; padding: 2px 8px; border-radius: 4px; font-size: 12px; font-weight: bold; }
    .card { background: #2a2a2a; border-radius: 12px; padding: 20px; margin-bottom: 15px; }
    .stat { display: flex; justify-content: space-between; padding: 8px 0; border-bottom: 1px solid #333; }
    .stat:last-child { border-bottom: none; }
    .stat-label { color: #888; }
    .stat-value { font-family: monospace; }
    .online { color: #22c55e; }
    .offline { color: #ef4444; }
    .led-indicator { width: 20px; height: 20px; border-radius: 50%; display: inline-block; margin-right: 10px; }
    .led-on { background: #22c55e; box-shadow: 0 0 10px #22c55e; }
    .led-off { background: #333; }
    .btn { display: block; width: 100%; padding: 15px; border: none; border-radius: 8px; font-size: 16px; cursor: pointer; margin-top: 10px; box-sizing: border-box; }
    .btn-primary { background: #3b82f6; color: white; }
    .btn-success { background: #22c55e; color: white; }
    .btn-secondary { background: #333; color: #fff; text-decoration: none; text-align: center; }
    .btn-danger { background: #dc2626; color: white; }
    .power-section { display: flex; gap: 10px; margin-bottom: 15px; }
    .power-btn { flex: 1; }
    .device-off .card { opacity: 0.5; }
    .device-off .btn-success, .device-off .btn-primary { pointer-events: none; opacity: 0.5; }
  </style>
</head>
<body>
  <div class="container ${state.powered ? '' : 'device-off'}">
    <h1>${config.deviceName} <span class="sim-badge">SIM</span></h1>
    <p class="subtitle">ESP32 Simulator</p>

    <div class="power-section">
      <button class="btn power-btn ${state.powered ? 'btn-danger' : 'btn-success'}" onclick="togglePower()">
        ⚡ ${state.powered ? 'Power OFF' : 'Power ON'}
      </button>
    </div>

    <div class="card">
      <div class="stat">
        <span class="stat-label">Power</span>
        <span class="stat-value ${state.powered ? 'online' : 'offline'}">${state.powered ? '⚡ ON' : '⚫ OFF'}</span>
      </div>
      <div class="stat">
        <span class="stat-label">Device ID</span>
        <span class="stat-value">${config.deviceId}</span>
      </div>
      <div class="stat">
        <span class="stat-label">MQTT Broker</span>
        <span class="stat-value">${config.mqttServer}:${config.mqttPort}</span>
      </div>
      <div class="stat">
        <span class="stat-label">MQTT Status</span>
        <span class="stat-value ${mqttConnected ? 'online' : 'offline'}">${mqttConnected ? 'Connected' : 'Disconnected'}</span>
      </div>
      <div class="stat">
        <span class="stat-label">Uptime</span>
        <span class="stat-value">${getUptime()}s</span>
      </div>
      <div class="stat">
        <span class="stat-label">Reconnects</span>
        <span class="stat-value">${state.reconnectCount}</span>
      </div>
    </div>

    <div class="card">
      <div style="display: flex; align-items: center; margin-bottom: 15px;">
        <div class="led-indicator ${state.ledOn ? 'led-on' : 'led-off'}"></div>
        <span style="font-size: 18px;">LED is ${state.ledOn ? 'ON' : 'OFF'}</span>
      </div>
      <button class="btn ${state.ledOn ? 'btn-secondary' : 'btn-success'}" onclick="toggleLED()">
        Turn ${state.ledOn ? 'OFF' : 'ON'}
      </button>
    </div>

    <a href="/config" class="btn btn-secondary">Settings</a>
  </div>

  <script>
    function toggleLED() {
      fetch('/toggle', { method: 'POST' })
        .then(() => location.reload());
    }
    function togglePower() {
      fetch('/power', { method: 'POST' })
        .then(() => location.reload());
    }
    // Auto-refresh every 5s
    setTimeout(() => location.reload(), 5000);
  </script>
</body>
</html>`;

  res.writeHead(200, { 'Content-Type': 'text/html' });
  res.end(html);
}

function serveConfigPage(res) {
  const html = `<!DOCTYPE html>
<html>
<head>
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Settings - ${config.deviceName}</title>
  <style>
    body { font-family: -apple-system, sans-serif; background: #1a1a1a; color: #fff; margin: 0; padding: 20px; }
    .container { max-width: 400px; margin: 0 auto; }
    h1 { color: #3b82f6; }
    .card { background: #2a2a2a; border-radius: 12px; padding: 20px; margin-bottom: 15px; }
    label { display: block; color: #888; margin-bottom: 5px; margin-top: 15px; }
    label:first-child { margin-top: 0; }
    input { width: 100%; padding: 12px; border: 1px solid #444; border-radius: 8px; background: #1a1a1a; color: #fff; box-sizing: border-box; }
    .btn { display: block; width: 100%; padding: 15px; border: none; border-radius: 8px; font-size: 16px; cursor: pointer; margin-top: 15px; box-sizing: border-box; }
    .btn-primary { background: #3b82f6; color: white; }
    .btn-secondary { background: #333; color: #fff; text-align: center; text-decoration: none; }
  </style>
</head>
<body>
  <div class="container">
    <h1>Settings</h1>

    <form method="POST" action="/config">
      <div class="card">
        <h3 style="margin-top:0;color:#888;">Device</h3>
        <label>Device ID</label>
        <input type="text" name="device_id" value="${config.deviceId}">
        <label>Device Name</label>
        <input type="text" name="device_name" value="${config.deviceName}">
      </div>

      <div class="card">
        <h3 style="margin-top:0;color:#888;">MQTT</h3>
        <label>Server Address</label>
        <input type="text" name="mqtt_server" value="${config.mqttServer}">
        <label>Port</label>
        <input type="number" name="mqtt_port" value="${config.mqttPort}">
      </div>

      <div class="card">
        <h3 style="margin-top:0;color:#888;">Web Server</h3>
        <label>Port</label>
        <input type="number" name="web_port" value="${config.webPort}">
      </div>

      <button type="submit" class="btn btn-primary">Save & Reconnect</button>
    </form>

    <a href="/" class="btn btn-secondary">Back</a>
  </div>
</body>
</html>`;

  res.writeHead(200, { 'Content-Type': 'text/html' });
  res.end(html);
}

function handleConfigPost(body, res) {
  const params = new URLSearchParams(body);

  const newConfig = {
    deviceId: params.get('device_id') || config.deviceId,
    deviceName: params.get('device_name') || config.deviceName,
    mqttServer: params.get('mqtt_server') || config.mqttServer,
    mqttPort: parseInt(params.get('mqtt_port')) || config.mqttPort,
    webPort: parseInt(params.get('web_port')) || config.webPort,
  };

  const needReconnect = newConfig.mqttServer !== config.mqttServer ||
                        newConfig.mqttPort !== config.mqttPort ||
                        newConfig.deviceId !== config.deviceId;

  config = newConfig;
  saveConfig();

  console.log('\n⚙️  Config updated:', config);

  if (needReconnect) {
    console.log('🔄 Reconnecting MQTT with new settings...');
    connectMQTT();
  }

  // Redirect back to home
  res.writeHead(302, { 'Location': '/' });
  res.end();
}

function handleToggle(res) {
  if (!state.powered) {
    res.writeHead(503, { 'Content-Type': 'application/json' });
    res.end(JSON.stringify({ error: 'Device is powered off' }));
    return;
  }
  state.ledOn = !state.ledOn;
  console.log(`💡 LED toggled via Web UI: ${state.ledOn ? 'ON' : 'OFF'}`);
  publishLog(`LED set to ${state.ledOn ? 'ON' : 'OFF'} via Web UI`);
  publishState();
  publishHealth();

  res.writeHead(200, { 'Content-Type': 'application/json' });
  res.end(JSON.stringify({ led: state.ledOn }));
}

function handlePower(res) {
  if (state.powered) {
    powerOff();
  } else {
    powerOn();
  }
  res.writeHead(200, { 'Content-Type': 'application/json' });
  res.end(JSON.stringify({ powered: state.powered }));
}

function serveState(res) {
  res.writeHead(200, { 'Content-Type': 'application/json' });
  res.end(JSON.stringify({
    deviceId: config.deviceId,
    deviceName: config.deviceName,
    ledOn: state.ledOn,
    uptime: getUptime(),
    mqttConnected,
    reconnectCount: state.reconnectCount,
  }));
}

// Main
function main() {
  console.log('╔════════════════════════════════════════╗');
  console.log('║       ESP32 Simulator v1.0.0           ║');
  console.log('╚════════════════════════════════════════╝');

  loadConfig();

  console.log(`\n📋 Device: ${config.deviceName} (${config.deviceId})`);

  createWebServer();
  connectMQTT();

  // Publish health every 5 seconds
  setInterval(() => {
    publishHealth();
    publishLog(`Health: heap=${randomHeap()} rssi=${randomRSSI()}`);
  }, 5000);

  // Publish state every 30 seconds
  setInterval(publishState, 30000);

  console.log('\n🎮 Simulator running. Press Ctrl+C to stop.\n');
}

main();
