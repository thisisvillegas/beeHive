/**
 * MQTT Service for ESP32 device communication
 */

import mqtt, { MqttClient } from 'mqtt';
import { ESP32Device, ESP32State, ESP32Health } from './types';

interface ESP32DeviceEntry {
  config: ESP32Device;
  state: ESP32State;
  lastSeen: number;
}

interface LogEntry {
  ts: number;
  msg: string;
  received: number;
}

// Raw MQTT message for broker console
interface RawMQTTMessage {
  topic: string;
  payload: string;
  timestamp: number;
}

type DeviceUpdateListener = (devices: ESP32Device[]) => void;

class MQTTService {
  private client: MqttClient | null = null;
  private devices: Map<string, ESP32DeviceEntry> = new Map();
  private deviceHealth: Map<string, ESP32Health> = new Map();
  private deviceLogs: Map<string, LogEntry[]> = new Map();
  private listeners: Set<DeviceUpdateListener> = new Set();
  private connected: boolean = false;
  private topicPrefix: string = 'homecontrol';
  private maxLogsPerDevice: number = 100;

  // Broker console - raw message log
  private rawMessages: RawMQTTMessage[] = [];
  private maxRawMessages: number = 200;
  private connectionTime: number | null = null;
  private disconnectCount: number = 0;
  private currentBrokerUrl: string = 'mqtt://localhost:1883';

  connect(brokerUrl: string = 'mqtt://localhost:1883') {
    if (this.client) {
      console.log('[MQTT] Already connected');
      return;
    }
    this.currentBrokerUrl = brokerUrl;

    console.log(`[MQTT] Connecting to ${brokerUrl}...`);

    this.client = mqtt.connect(brokerUrl, {
      clientId: `homecontrol-server-${Date.now()}`,
      clean: true,
      reconnectPeriod: 5000,
    });

    this.client.on('connect', () => {
      console.log('[MQTT] Connected to broker');
      this.connected = true;
      this.connectionTime = Date.now();

      const topics = [
        `${this.topicPrefix}/discovery/+/config`,
        `${this.topicPrefix}/devices/+/state`,
        `${this.topicPrefix}/devices/+/availability`,
        `${this.topicPrefix}/devices/+/health`,
        `${this.topicPrefix}/devices/+/logs`,
      ];

      topics.forEach(topic => {
        this.client?.subscribe(topic, (err) => {
          if (err) {
            console.error(`[MQTT] Subscribe error for ${topic}:`, err);
          } else {
            console.log(`[MQTT] Subscribed to ${topic}`);
          }
        });
      });
    });

    this.client.on('message', (topic, payload) => {
      const payloadStr = payload.toString();

      // Store raw message for broker console
      this.addRawMessage(topic, payloadStr);

      // Process message
      this.handleMessage(topic, payloadStr);
    });

    this.client.on('error', (error) => {
      console.error('[MQTT] Error:', error);
    });

    this.client.on('close', () => {
      console.log('[MQTT] Disconnected');
      this.connected = false;
      this.disconnectCount++;
    });

    this.client.on('reconnect', () => {
      console.log('[MQTT] Reconnecting...');
    });
  }

  private addRawMessage(topic: string, payload: string) {
    this.rawMessages.push({
      topic,
      payload,
      timestamp: Date.now(),
    });

    // Trim to max size
    if (this.rawMessages.length > this.maxRawMessages) {
      this.rawMessages.splice(0, this.rawMessages.length - this.maxRawMessages);
    }
  }

  private handleMessage(topic: string, payload: string) {
    const parts = topic.split('/');

    try {
      if (topic.includes('/discovery/') && topic.endsWith('/config')) {
        const config = JSON.parse(payload) as ESP32Device;
        config.online = true;
        config.lastSeen = Date.now();
        console.log(`[MQTT] Device discovered: ${config.name} (${config.device_id})`);
        this.updateDevice(config.device_id, { config });

      } else if (topic.includes('/devices/') && topic.endsWith('/state')) {
        const deviceId = parts[2];
        const state = JSON.parse(payload) as ESP32State;
        console.log(`[MQTT] State update for ${deviceId}:`, state);
        this.updateDevice(deviceId, { state, lastSeen: Date.now() });

      } else if (topic.includes('/devices/') && topic.endsWith('/health')) {
        const deviceId = parts[2];
        const health = JSON.parse(payload) as ESP32Health;
        health.last_seen = Date.now();
        health.stale = false;
        console.log(`[MQTT] Health update for ${deviceId}: mqtt=${health.mqtt_state} heap=${health.free_heap}`);
        this.deviceHealth.set(deviceId, health);

        // Health messages are authoritative - if we're receiving health, device is online
        const existing = this.devices.get(deviceId);
        if (existing) {
          existing.lastSeen = Date.now();
          existing.config.online = true;
        }
        this.notifyListeners();

      } else if (topic.includes('/devices/') && topic.endsWith('/logs')) {
        const deviceId = parts[2];
        const logEntry = JSON.parse(payload) as { ts: number; msg: string };

        let logs = this.deviceLogs.get(deviceId);
        if (!logs) {
          logs = [];
          this.deviceLogs.set(deviceId, logs);
        }

        logs.push({
          ts: logEntry.ts,
          msg: logEntry.msg,
          received: Date.now(),
        });

        if (logs.length > this.maxLogsPerDevice) {
          logs.splice(0, logs.length - this.maxLogsPerDevice);
        }

        console.log(`[MQTT] Log from ${deviceId}: ${logEntry.msg}`);

      } else if (topic.endsWith('/availability')) {
        const deviceId = parts[2];
        const online = payload === 'online';
        console.log(`[MQTT] Device ${deviceId} is ${online ? 'online' : 'offline'}`);

        const existing = this.devices.get(deviceId);
        if (existing) {
          existing.config.online = online;
          existing.lastSeen = Date.now();
          this.notifyListeners();
        }
      }
    } catch (error) {
      console.error('[MQTT] Error parsing message:', error);
    }
  }

  private updateDevice(deviceId: string, update: Partial<ESP32DeviceEntry>) {
    const existing = this.devices.get(deviceId) || {
      config: { device_id: deviceId, online: true } as ESP32Device,
      state: {},
      lastSeen: Date.now(),
    };

    const updated = {
      ...existing,
      ...update,
      config: { ...existing.config, ...update.config },
      state: { ...existing.state, ...update.state },
    };

    // Debug: log online status changes
    if (existing.config?.online !== updated.config?.online) {
      console.log(`[MQTT] Device ${deviceId} online: ${existing.config?.online} -> ${updated.config?.online}`);
    }

    this.devices.set(deviceId, updated);
    this.notifyListeners();
  }

  // ===== Public API =====

  getDevices(): ESP32Device[] {
    return Array.from(this.devices.values()).map(entry => ({
      ...entry.config,
      lastSeen: entry.lastSeen,
    }));
  }

  getDeviceState(deviceId: string): ESP32State | undefined {
    return this.devices.get(deviceId)?.state;
  }

  getDevice(deviceId: string): ESP32DeviceEntry | undefined {
    return this.devices.get(deviceId);
  }

  getDeviceHealth(deviceId: string): ESP32Health | undefined {
    const health = this.deviceHealth.get(deviceId);
    if (health) {
      health.stale = Date.now() - (health.last_seen || 0) > 15000;
    }
    return health;
  }

  getDeviceLogs(deviceId: string): LogEntry[] {
    return this.deviceLogs.get(deviceId) || [];
  }

  clearDeviceLogs(deviceId: string): void {
    this.deviceLogs.set(deviceId, []);
  }

  getAllDeviceHealth(): Map<string, ESP32Health> {
    return this.deviceHealth;
  }

  // Broker console methods
  getBrokerStatus() {
    return {
      connected: this.connected,
      connectionTime: this.connectionTime,
      disconnectCount: this.disconnectCount,
      deviceCount: this.devices.size,
      messageCount: this.rawMessages.length,
      brokerUrl: this.currentBrokerUrl,
    };
  }

  getRawMessages(): RawMQTTMessage[] {
    return this.rawMessages;
  }

  clearRawMessages(): void {
    this.rawMessages = [];
  }

  sendCommand(deviceId: string, capability: { type: string; instance: string; value: unknown }) {
    if (!this.client || !this.connected) {
      console.error('[MQTT] Cannot send command - not connected');
      return false;
    }

    const topic = `${this.topicPrefix}/devices/${deviceId}/set`;
    const payload = JSON.stringify({
      capability,
      request_id: `${Date.now()}-${Math.random().toString(36).substr(2, 9)}`,
    });

    console.log(`[MQTT] Sending command to ${topic}:`, payload);

    this.client.publish(topic, payload, { qos: 1 }, (err) => {
      if (err) {
        console.error('[MQTT] Publish error:', err);
      }
    });

    return true;
  }

  subscribe(listener: DeviceUpdateListener): () => void {
    this.listeners.add(listener);
    return () => this.listeners.delete(listener);
  }

  private notifyListeners() {
    const devices = this.getDevices();
    this.listeners.forEach(listener => {
      try {
        listener(devices);
      } catch (error) {
        console.error('[MQTT] Listener error:', error);
      }
    });
  }

  isConnected(): boolean {
    return this.connected;
  }

  getBrokerUrl(): string {
    return this.currentBrokerUrl;
  }

  disconnect() {
    if (this.client) {
      this.client.end();
      this.client = null;
      this.connected = false;
    }
  }

  reconnect(newBrokerUrl: string) {
    console.log(`[MQTT] Reconnecting to ${newBrokerUrl}...`);
    this.disconnect();
    this.connect(newBrokerUrl);
  }
}

export const mqttService = new MQTTService();

let initialized = false;
export function initMQTT() {
  if (initialized) return;
  initialized = true;
  const brokerUrl = process.env.MQTT_BROKER_URL || 'mqtt://localhost:1883';
  mqttService.connect(brokerUrl);
}
