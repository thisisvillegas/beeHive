/**
 * Multi-Broker MQTT Service for ESP32 device communication
 * Connects to multiple brokers simultaneously, aggregates devices
 */

import mqtt, { MqttClient } from 'mqtt';
import { ESP32Device, ESP32State, ESP32Health } from './types';

// Extended device with broker info
interface ESP32DeviceWithBroker extends ESP32Device {
  broker: string; // broker ID (e.g., 'pi', 'mac')
  brokerUrl: string;
}

interface ESP32DeviceEntry {
  config: ESP32DeviceWithBroker;
  state: ESP32State;
  lastSeen: number;
}

interface LogEntry {
  ts: number;
  msg: string;
  received: number;
}

interface RawMQTTMessage {
  topic: string;
  payload: string;
  timestamp: number;
  broker: string;
}

interface BrokerConfig {
  id: string;
  label: string;
  url: string;
}

type DeviceUpdateListener = (devices: ESP32DeviceWithBroker[]) => void;

// Individual broker connection
class BrokerConnection {
  public id: string;
  public label: string;
  public url: string;
  public client: MqttClient | null = null;
  public connected: boolean = false;
  public connectionTime: number | null = null;
  public disconnectCount: number = 0;
  public rawMessages: RawMQTTMessage[] = [];
  public maxRawMessages: number = 200;

  private topicPrefix: string = 'homecontrol';
  private onMessage: (broker: string, topic: string, payload: string) => void;
  private onConnectionChange: () => void;

  constructor(
    config: BrokerConfig,
    onMessage: (broker: string, topic: string, payload: string) => void,
    onConnectionChange: () => void
  ) {
    this.id = config.id;
    this.label = config.label;
    this.url = config.url;
    this.onMessage = onMessage;
    this.onConnectionChange = onConnectionChange;
  }

  connect() {
    if (this.client) {
      console.log(`[MQTT:${this.id}] Already connected`);
      return;
    }

    console.log(`[MQTT:${this.id}] Connecting to ${this.url}...`);

    this.client = mqtt.connect(this.url, {
      clientId: `homecontrol-${this.id}-${Date.now()}`,
      clean: true,
      reconnectPeriod: 5000,
    });

    this.client.on('connect', () => {
      console.log(`[MQTT:${this.id}] Connected`);
      this.connected = true;
      this.connectionTime = Date.now();
      this.onConnectionChange();

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
            console.error(`[MQTT:${this.id}] Subscribe error for ${topic}:`, err);
          } else {
            console.log(`[MQTT:${this.id}] Subscribed to ${topic}`);
          }
        });
      });
    });

    this.client.on('message', (topic, payload) => {
      const payloadStr = payload.toString();
      this.addRawMessage(topic, payloadStr);
      this.onMessage(this.id, topic, payloadStr);
    });

    this.client.on('error', (error) => {
      console.error(`[MQTT:${this.id}] Error:`, error);
    });

    this.client.on('close', () => {
      console.log(`[MQTT:${this.id}] Disconnected`);
      this.connected = false;
      this.disconnectCount++;
      this.onConnectionChange();
    });

    this.client.on('reconnect', () => {
      console.log(`[MQTT:${this.id}] Reconnecting...`);
    });
  }

  private addRawMessage(topic: string, payload: string) {
    this.rawMessages.push({
      topic,
      payload,
      timestamp: Date.now(),
      broker: this.id,
    });

    if (this.rawMessages.length > this.maxRawMessages) {
      this.rawMessages.splice(0, this.rawMessages.length - this.maxRawMessages);
    }
  }

  publish(topic: string, payload: string, options?: { qos?: 0 | 1 | 2 }) {
    if (!this.client || !this.connected) {
      console.error(`[MQTT:${this.id}] Cannot publish - not connected`);
      return false;
    }
    this.client.publish(topic, payload, { qos: options?.qos || 1 });
    return true;
  }

  disconnect() {
    if (this.client) {
      this.client.end();
      this.client = null;
      this.connected = false;
    }
  }

  getStatus() {
    return {
      id: this.id,
      label: this.label,
      url: this.url,
      connected: this.connected,
      connectionTime: this.connectionTime,
      disconnectCount: this.disconnectCount,
      messageCount: this.rawMessages.length,
    };
  }

  clearRawMessages() {
    this.rawMessages = [];
  }
}

// Main service managing multiple brokers
class MultiBrokerMQTTService {
  private brokers: Map<string, BrokerConnection> = new Map();
  private devices: Map<string, ESP32DeviceEntry> = new Map(); // key: deviceId
  private deviceHealth: Map<string, ESP32Health> = new Map();
  private deviceLogs: Map<string, LogEntry[]> = new Map();
  private listeners: Set<DeviceUpdateListener> = new Set();
  private topicPrefix: string = 'homecontrol';
  private maxLogsPerDevice: number = 100;
  private selectedBrokerForConsole: string = '';

  // Add a broker connection
  addBroker(config: BrokerConfig) {
    if (this.brokers.has(config.id)) {
      console.log(`[MQTT] Broker ${config.id} already exists`);
      return;
    }

    const connection = new BrokerConnection(
      config,
      this.handleMessage.bind(this),
      this.notifyListeners.bind(this)
    );

    this.brokers.set(config.id, connection);
    connection.connect();

    if (!this.selectedBrokerForConsole) {
      this.selectedBrokerForConsole = config.id;
    }

    console.log(`[MQTT] Added broker: ${config.id} (${config.url})`);
  }

  removeBroker(brokerId: string) {
    const broker = this.brokers.get(brokerId);
    if (broker) {
      broker.disconnect();
      this.brokers.delete(brokerId);

      // Remove devices from this broker
      for (const [deviceId, entry] of this.devices) {
        if (entry.config.broker === brokerId) {
          this.devices.delete(deviceId);
        }
      }

      this.notifyListeners();
    }
  }

  private handleMessage(brokerId: string, topic: string, payload: string) {
    const parts = topic.split('/');
    const broker = this.brokers.get(brokerId);
    if (!broker) return;

    try {
      if (topic.includes('/discovery/') && topic.endsWith('/config')) {
        const config = JSON.parse(payload) as ESP32Device;
        const deviceWithBroker: ESP32DeviceWithBroker = {
          ...config,
          online: true,
          lastSeen: Date.now(),
          broker: brokerId,
          brokerUrl: broker.url,
        };
        console.log(`[MQTT:${brokerId}] Device discovered: ${config.name} (${config.device_id})`);
        this.updateDevice(config.device_id, { config: deviceWithBroker });

      } else if (topic.includes('/devices/') && topic.endsWith('/state')) {
        const deviceId = parts[2];
        const state = JSON.parse(payload) as ESP32State;
        console.log(`[MQTT:${brokerId}] State update for ${deviceId}`);

        // Only update if this device belongs to this broker
        const existing = this.devices.get(deviceId);
        if (!existing || existing.config.broker === brokerId) {
          this.updateDevice(deviceId, {
            state,
            lastSeen: Date.now(),
            config: { broker: brokerId, brokerUrl: broker.url } as any
          });
        }

      } else if (topic.includes('/devices/') && topic.endsWith('/health')) {
        const deviceId = parts[2];
        const health = JSON.parse(payload) as ESP32Health;
        health.last_seen = Date.now();
        health.stale = false;
        console.log(`[MQTT:${brokerId}] Health update for ${deviceId}: mqtt=${health.mqtt_state}`);
        this.deviceHealth.set(deviceId, health);

        // Health = device is online on this broker
        const existing = this.devices.get(deviceId);
        if (existing && existing.config.broker === brokerId) {
          existing.lastSeen = Date.now();
          existing.config.online = true;
        } else if (!existing) {
          // Create device entry if we get health before discovery
          this.updateDevice(deviceId, {
            config: {
              device_id: deviceId,
              online: true,
              broker: brokerId,
              brokerUrl: broker.url
            } as ESP32DeviceWithBroker,
            lastSeen: Date.now()
          });
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

        console.log(`[MQTT:${brokerId}] Log from ${deviceId}: ${logEntry.msg}`);

      } else if (topic.endsWith('/availability')) {
        const deviceId = parts[2];
        const online = payload === 'online';
        console.log(`[MQTT:${brokerId}] Device ${deviceId} is ${online ? 'online' : 'offline'}`);

        const existing = this.devices.get(deviceId);
        if (existing && existing.config.broker === brokerId) {
          existing.config.online = online;
          existing.lastSeen = Date.now();
          this.notifyListeners();
        }
      }
    } catch (error) {
      console.error(`[MQTT:${brokerId}] Error parsing message:`, error);
    }
  }

  private updateDevice(deviceId: string, update: Partial<ESP32DeviceEntry>) {
    const existing = this.devices.get(deviceId) || {
      config: { device_id: deviceId, online: true, broker: '', brokerUrl: '' } as ESP32DeviceWithBroker,
      state: {},
      lastSeen: Date.now(),
    };

    const updated = {
      ...existing,
      ...update,
      config: { ...existing.config, ...update.config },
      state: { ...existing.state, ...update.state },
    };

    this.devices.set(deviceId, updated);
    this.notifyListeners();
  }

  // ===== Public API =====

  getDevices(): ESP32DeviceWithBroker[] {
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

  // Broker management
  getBrokers(): BrokerConfig[] {
    return Array.from(this.brokers.values()).map(b => ({
      id: b.id,
      label: b.label,
      url: b.url,
    }));
  }

  getBrokerStatus(brokerId?: string) {
    if (brokerId) {
      const broker = this.brokers.get(brokerId);
      return broker?.getStatus() || null;
    }
    // Return all broker statuses
    return Array.from(this.brokers.values()).map(b => b.getStatus());
  }

  getAllBrokersStatus() {
    return {
      brokers: Array.from(this.brokers.values()).map(b => b.getStatus()),
      totalDevices: this.devices.size,
      selectedConsole: this.selectedBrokerForConsole,
    };
  }

  // Console broker selection
  setConsoleBroker(brokerId: string) {
    if (this.brokers.has(brokerId)) {
      this.selectedBrokerForConsole = brokerId;
    }
  }

  getConsoleBroker(): string {
    return this.selectedBrokerForConsole;
  }

  getRawMessages(brokerId?: string): RawMQTTMessage[] {
    const id = brokerId || this.selectedBrokerForConsole;
    const broker = this.brokers.get(id);
    return broker?.rawMessages || [];
  }

  clearRawMessages(brokerId?: string): void {
    const id = brokerId || this.selectedBrokerForConsole;
    const broker = this.brokers.get(id);
    broker?.clearRawMessages();
  }

  // Send command to correct broker
  sendCommand(deviceId: string, capability: { type: string; instance: string; value: unknown }) {
    const device = this.devices.get(deviceId);
    if (!device) {
      console.error(`[MQTT] Cannot send command - device ${deviceId} not found`);
      return false;
    }

    const broker = this.brokers.get(device.config.broker);
    if (!broker || !broker.connected) {
      console.error(`[MQTT] Cannot send command - broker ${device.config.broker} not connected`);
      return false;
    }

    const topic = `${this.topicPrefix}/devices/${deviceId}/set`;
    const payload = JSON.stringify({
      capability,
      request_id: `${Date.now()}-${Math.random().toString(36).substr(2, 9)}`,
    });

    console.log(`[MQTT:${broker.id}] Sending command to ${topic}:`, payload);
    return broker.publish(topic, payload, { qos: 1 });
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

  isConnected(brokerId?: string): boolean {
    if (brokerId) {
      return this.brokers.get(brokerId)?.connected || false;
    }
    // Return true if any broker is connected
    return Array.from(this.brokers.values()).some(b => b.connected);
  }

  disconnectAll() {
    for (const broker of this.brokers.values()) {
      broker.disconnect();
    }
  }
}

export const mqttService = new MultiBrokerMQTTService();

// Known brokers - add yours here
const KNOWN_BROKERS: BrokerConfig[] = [
  { id: 'pi', label: 'Pi (localhost)', url: 'mqtt://localhost:1883' },
  { id: 'mac', label: 'Mac', url: 'mqtt://192.168.0.77:1883' },
];

let initialized = false;
export function initMQTT() {
  if (initialized) return;
  initialized = true;

  // Connect to all known brokers
  for (const config of KNOWN_BROKERS) {
    mqttService.addBroker(config);
  }
}

// Export types
export type { ESP32DeviceWithBroker, BrokerConfig };
