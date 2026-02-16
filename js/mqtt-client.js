/*
  MQTT Client for Energy Monitoring Dashboard
  - Connects to MQTT broker via WebSocket
  - Subscribes to device telemetry topics
  - Integrates with DATA module for persistence
  - Supports QoS 1 for reliable delivery

  Usage:
    MQTT_CLIENT.init({ brokerUrl: 'ws://broker.local:9001', ... });
    MQTT_CLIENT.connect();
    MQTT_CLIENT.subscribe('energy/solar/+/telemetry');
*/

const MQTT_CLIENT = {
    client: null,
    connected: false,
    config: {
        brokerUrl: 'ws://localhost:9001/mqtt',  // WebSocket URL (ws:// or wss://)
        username: '',
        password: '',
        clientId: 'energy-dashboard-' + Math.random().toString(16).slice(2, 10),
        keepalive: 60,
        reconnectPeriod: 5000,
        connectTimeout: 30000,
        clean: true
    },
    subscriptions: new Map(),  // topic -> callback[]
    messageHandlers: [],       // global message handlers
    statusCallbacks: [],       // connection status callbacks

    // Storage keys
    STORAGE_KEY: 'mqtt_config',

    // Initialize with optional config overrides
    init(overrides = {}) {
        // Load saved config from localStorage
        const saved = this.loadConfig();
        this.config = { ...this.config, ...saved, ...overrides };
        console.log('[MQTT] Initialized with config:', { ...this.config, password: '***' });
        return this;
    },

    // Save config to localStorage
    saveConfig(cfg = null) {
        const toSave = cfg || this.config;
        // Don't save password in clear text for production - this is demo only
        localStorage.setItem(this.STORAGE_KEY, JSON.stringify({
            brokerUrl: toSave.brokerUrl,
            username: toSave.username,
            password: toSave.password, // For demo; in production use secure storage
            clientId: toSave.clientId
        }));
    },

    // Load config from localStorage
    loadConfig() {
        try {
            const raw = localStorage.getItem(this.STORAGE_KEY);
            return raw ? JSON.parse(raw) : {};
        } catch (e) {
            console.warn('[MQTT] Failed to load config:', e);
            return {};
        }
    },

    // Connect to MQTT broker
    async connect() {
        return new Promise((resolve, reject) => {
            if (this.connected && this.client) {
                console.log('[MQTT] Already connected');
                resolve(true);
                return;
            }

            // Load mqtt.js from CDN if not available
            if (typeof mqtt === 'undefined') {
                const script = document.createElement('script');
                script.src = 'https://unpkg.com/mqtt@5.3.4/dist/mqtt.min.js';
                script.onload = () => this._doConnect(resolve, reject);
                script.onerror = () => reject(new Error('Failed to load MQTT library'));
                document.head.appendChild(script);
            } else {
                this._doConnect(resolve, reject);
            }
        });
    },

    _doConnect(resolve, reject) {
        try {
            console.log('[MQTT] Connecting to', this.config.brokerUrl);
            
            const options = {
                clientId: this.config.clientId,
                keepalive: this.config.keepalive,
                reconnectPeriod: this.config.reconnectPeriod,
                connectTimeout: this.config.connectTimeout,
                clean: this.config.clean
            };

            if (this.config.username) {
                options.username = this.config.username;
                options.password = this.config.password;
            }

            this.client = mqtt.connect(this.config.brokerUrl, options);

            this.client.on('connect', () => {
                console.log('[MQTT] Connected successfully');
                this.connected = true;
                this._notifyStatus('connected');
                
                // Resubscribe to all topics
                this.subscriptions.forEach((callbacks, topic) => {
                    this.client.subscribe(topic, { qos: 1 }, (err) => {
                        if (err) console.error('[MQTT] Resubscribe error:', topic, err);
                        else console.log('[MQTT] Resubscribed to:', topic);
                    });
                });
                
                resolve(true);
            });

            this.client.on('reconnect', () => {
                console.log('[MQTT] Reconnecting...');
                this._notifyStatus('reconnecting');
            });

            this.client.on('close', () => {
                console.log('[MQTT] Connection closed');
                this.connected = false;
                this._notifyStatus('disconnected');
            });

            this.client.on('offline', () => {
                console.log('[MQTT] Client offline');
                this.connected = false;
                this._notifyStatus('offline');
            });

            this.client.on('error', (err) => {
                console.error('[MQTT] Error:', err);
                this._notifyStatus('error', err.message);
                if (!this.connected) reject(err);
            });

            this.client.on('message', (topic, message, packet) => {
                this._handleMessage(topic, message, packet);
            });

        } catch (err) {
            console.error('[MQTT] Connection error:', err);
            reject(err);
        }
    },

    // Disconnect from broker
    disconnect() {
        if (this.client) {
            this.client.end(true);
            this.client = null;
            this.connected = false;
            this._notifyStatus('disconnected');
        }
    },

    // Subscribe to a topic
    subscribe(topic, callback = null, qos = 1) {
        if (!this.subscriptions.has(topic)) {
            this.subscriptions.set(topic, []);
        }
        if (callback) {
            this.subscriptions.get(topic).push(callback);
        }

        if (this.connected && this.client) {
            this.client.subscribe(topic, { qos }, (err, granted) => {
                if (err) {
                    console.error('[MQTT] Subscribe error:', topic, err);
                } else {
                    console.log('[MQTT] Subscribed to:', topic, granted);
                }
            });
        }
        return this;
    },

    // Unsubscribe from a topic
    unsubscribe(topic) {
        this.subscriptions.delete(topic);
        if (this.connected && this.client) {
            this.client.unsubscribe(topic, (err) => {
                if (err) console.error('[MQTT] Unsubscribe error:', topic, err);
                else console.log('[MQTT] Unsubscribed from:', topic);
            });
        }
        return this;
    },

    // Publish a message
    publish(topic, message, options = { qos: 1, retain: false }) {
        if (!this.connected || !this.client) {
            console.error('[MQTT] Not connected, cannot publish');
            return false;
        }

        const payload = typeof message === 'object' ? JSON.stringify(message) : String(message);
        this.client.publish(topic, payload, options, (err) => {
            if (err) console.error('[MQTT] Publish error:', topic, err);
            else console.log('[MQTT] Published to:', topic);
        });
        return true;
    },

    // Handle incoming messages
    _handleMessage(topic, message, packet) {
        let payload;
        try {
            const str = message.toString();
            try {
                payload = JSON.parse(str);
            } catch {
                payload = str;
            }
        } catch (e) {
            payload = message;
        }

        console.log('[MQTT] Message received:', topic, payload);

        // Call global handlers
        this.messageHandlers.forEach(handler => {
            try {
                handler(topic, payload, packet);
            } catch (e) {
                console.error('[MQTT] Handler error:', e);
            }
        });

        // Call topic-specific callbacks (support wildcards)
        this.subscriptions.forEach((callbacks, subTopic) => {
            if (this._topicMatch(subTopic, topic)) {
                callbacks.forEach(cb => {
                    try {
                        cb(topic, payload, packet);
                    } catch (e) {
                        console.error('[MQTT] Callback error:', e);
                    }
                });
            }
        });

        // Auto-save to DATA module if topic matches telemetry pattern
        this._autoSaveIfTelemetry(topic, payload);
    },

    // Check if topic matches subscription (with wildcards)
    _topicMatch(subscription, topic) {
        if (subscription === topic) return true;
        
        const subParts = subscription.split('/');
        const topicParts = topic.split('/');
        
        for (let i = 0; i < subParts.length; i++) {
            if (subParts[i] === '#') return true;  // Multi-level wildcard
            if (subParts[i] === '+') continue;     // Single-level wildcard
            if (subParts[i] !== topicParts[i]) return false;
        }
        
        return subParts.length === topicParts.length;
    },

    // Auto-save telemetry to DATA module
    _autoSaveIfTelemetry(topic, payload) {
        // Expected topic format: energy/{type}/{deviceId}/telemetry
        // Example: energy/solar/solar_1/telemetry
        const match = topic.match(/^energy\/(\w+)\/([^\/]+)\/telemetry$/);
        if (match && typeof DATA !== 'undefined') {
            const [, type, deviceId] = match;
            const fullDeviceId = `${type}_${deviceId}`.replace(/_+/g, '_');
            
            // Validate telemetry
            if (this._isTelemetry(payload)) {
                const user = typeof AUTH !== 'undefined' ? AUTH.getCurrentUser() : null;
                DATA.saveReading(fullDeviceId, {
                    payload,
                    userId: user ? user.id : null,
                    meta: { source: 'mqtt', topic }
                });
                
                // Dispatch event for UI updates
                window.dispatchEvent(new CustomEvent('mqtt-telemetry', {
                    detail: { topic, deviceId: fullDeviceId, type, payload }
                }));
                
                console.log('[MQTT] Auto-saved telemetry for:', fullDeviceId);
            }
        }
    },

    // Check if payload looks like telemetry
    _isTelemetry(payload) {
        if (!payload) return false;
        if (typeof payload === 'object') {
            const keys = Object.keys(payload).join(' ');
            if (/(volt|voltage|power|current|soc|energy|w|v|a|temp|humidity)/i.test(keys)) return true;
            return Object.values(payload).some(v => typeof v === 'number');
        }
        return false;
    },

    // Add global message handler
    onMessage(handler) {
        this.messageHandlers.push(handler);
        return this;
    },

    // Add connection status callback
    onStatus(callback) {
        this.statusCallbacks.push(callback);
        return this;
    },

    _notifyStatus(status, error = null) {
        this.statusCallbacks.forEach(cb => {
            try {
                cb(status, error);
            } catch (e) {
                console.error('[MQTT] Status callback error:', e);
            }
        });

        // Dispatch global event
        window.dispatchEvent(new CustomEvent('mqtt-status', {
            detail: { status, error, connected: this.connected }
        }));
    },

    // Get connection status
    getStatus() {
        return {
            connected: this.connected,
            brokerUrl: this.config.brokerUrl,
            clientId: this.config.clientId,
            subscriptions: Array.from(this.subscriptions.keys())
        };
    },

    // Update config and reconnect
    async updateConfig(newConfig) {
        const wasConnected = this.connected;
        if (wasConnected) {
            this.disconnect();
        }
        
        this.config = { ...this.config, ...newConfig };
        this.saveConfig();
        
        if (wasConnected) {
            await this.connect();
        }
    },

    // =====================================================
    // Convenience methods for Energy Monitoring Dashboard
    // =====================================================

    // Subscribe to solar telemetry
    subscribeSolar(callback) {
        return this.subscribe('energy/solar/+/telemetry', callback);
    },

    // Subscribe to wind telemetry
    subscribeWind(callback) {
        return this.subscribe('energy/wind/+/telemetry', callback);
    },

    // Subscribe to battery telemetry
    subscribeBattery(callback) {
        return this.subscribe('energy/battery/+/telemetry', callback);
    },

    // Subscribe to all energy telemetry
    subscribeAll(callback) {
        return this.subscribe('energy/+/+/telemetry', callback);
    },

    // Publish command to device
    sendCommand(type, deviceId, command) {
        const topic = `energy/${type}/${deviceId}/command`;
        return this.publish(topic, command);
    }
};

// Auto-init on load
if (typeof window !== 'undefined') {
    MQTT_CLIENT.init();
}
