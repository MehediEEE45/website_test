require('dotenv').config();
const fs = require('fs');
const path = require('path');
const mqtt = require('mqtt');
const express = require('express');
const bodyParser = require('body-parser');
const sqlite3 = require('sqlite3').verbose();

const MQTT_URL = process.env.MQTT_URL || 'mqtt://localhost:1883';
const MQTT_USERNAME = process.env.MQTT_USERNAME || '';
const MQTT_PASSWORD = process.env.MQTT_PASSWORD || '';
const MQTT_TOPIC_FILTER = process.env.MQTT_TOPIC_FILTER || 'energy/+/+/telemetry';
const PORT = parseInt(process.env.PORT || '3000', 10);
const DB_FILE = process.env.DB_FILE || 'telemetry.db';

// Ensure DB exists
const dbPath = path.resolve(__dirname, DB_FILE);
const db = new sqlite3.Database(dbPath);

db.serialize(() => {
  db.run(`CREATE TABLE IF NOT EXISTS readings (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    device_id TEXT NOT NULL,
    topic TEXT NOT NULL,
    payload TEXT NOT NULL,
    ts INTEGER NOT NULL
  )`);
});

function saveReading(deviceId, topic, payload) {
  const ts = Date.now();
  const stmt = db.prepare('INSERT INTO readings (device_id, topic, payload, ts) VALUES (?, ?, ?, ?)');
  stmt.run(deviceId, topic, JSON.stringify(payload), ts, function (err) {
    if (err) console.error('DB insert error', err);
    stmt.finalize();
  });
}

// Connect to MQTT
const mqttOptions = {};
if (MQTT_USERNAME) mqttOptions.username = MQTT_USERNAME;
if (MQTT_PASSWORD) mqttOptions.password = MQTT_PASSWORD;

console.log('Connecting to MQTT broker:', MQTT_URL);
const client = mqtt.connect(MQTT_URL, mqttOptions);

client.on('connect', () => {
  console.log('Connected to MQTT broker');
  client.subscribe(MQTT_TOPIC_FILTER, { qos: 1 }, (err) => {
    if (err) console.error('Subscribe error', err);
    else console.log('Subscribed to', MQTT_TOPIC_FILTER);
  });
});

client.on('error', (err) => {
  console.error('MQTT error', err);
});

client.on('message', (topic, message) => {
  let payload = null;
  try { payload = JSON.parse(message.toString()); } catch { payload = message.toString(); }
  // Extract device id from topic (energy/{type}/{deviceId}/telemetry)
  const parts = topic.split('/');
  let deviceId = topic;
  if (parts.length >= 3) deviceId = parts[1] + '_' + parts[2];
  saveReading(deviceId, topic, payload);
  console.log(`Saved reading for ${deviceId} topic=${topic}`);
});

// Simple HTTP API
const app = express();
app.use(bodyParser.json());

// Get recent readings for a device
app.get('/api/readings/:deviceId', (req, res) => {
  const deviceId = req.params.deviceId;
  const limit = parseInt(req.query.limit || '100', 10);
  db.all('SELECT id, device_id, topic, payload, ts FROM readings WHERE device_id = ? ORDER BY ts DESC LIMIT ?', [deviceId, limit], (err, rows) => {
    if (err) return res.status(500).json({ error: 'DB error' });
    const parsed = rows.map(r => ({ id: r.id, device_id: r.device_id, topic: r.topic, payload: (() => { try { return JSON.parse(r.payload); } catch { return r.payload; } })(), ts: r.ts }));
    res.json(parsed);
  });
});

// Health
app.get('/api/health', (req, res) => res.json({ ok: true, mqtt: client.connected }));

// Post a reading (optional - allows direct HTTP ingestion)
app.post('/api/readings', (req, res) => {
  const { device_id, topic, payload } = req.body || {};
  if (!device_id || !topic || !payload) return res.status(400).json({ error: 'Missing fields' });
  saveReading(device_id, topic, payload);
  res.json({ success: true });
});

// Add WebSocket bridge so browsers can receive MQTT messages via ws://<host>:<port>/ws
const http = require('http');
const WebSocket = require('ws');
const server = http.createServer(app);
const wss = new WebSocket.Server({ noServer: true, path: '/ws' });

// Upgrade HTTP connections to WebSocket for the WS bridge
server.on('upgrade', function upgrade(request, socket, head) {
  if (request.url === '/ws') {
    wss.handleUpgrade(request, socket, head, function done(ws) {
      wss.emit('connection', ws, request);
    });
  } else {
    socket.destroy();
  }
});

client.on('message', (topic, message) => {
  let payload = null;
  try { payload = JSON.parse(message.toString()); } catch { payload = message.toString(); }
  // Extract device id from topic (energy/{type}/{deviceId}/telemetry)
  const parts = topic.split('/');
  let deviceId = topic;
  if (parts.length >= 3) deviceId = parts[1] + '_' + parts[2];
  saveReading(deviceId, topic, payload);
  console.log(`Saved reading for ${deviceId} topic=${topic}`);

  // Broadcast to connected WebSocket clients
  try {
    const wsMsg = JSON.stringify({ topic, deviceId, payload, ts: Date.now() });
    wss.clients.forEach((c) => {
      if (c.readyState === WebSocket.OPEN) c.send(wsMsg);
    });
  } catch (e) {
    console.error('WS broadcast error', e);
  }
});

const os = require('os');

// Start the HTTP server (which also serves the WS bridge) and bind to all interfaces
server.listen(PORT, '0.0.0.0', () => {
  console.log(`Telemetry bridge listening on http://0.0.0.0:${PORT} (WS at /ws)`);

  // Print network interfaces to help connect from other devices
  const nets = os.networkInterfaces();
  Object.keys(nets).forEach((name) => {
    for (const net of nets[name]) {
      if (net.family === 'IPv4' && !net.internal) {
        console.log(`Interface ${name}: ${net.address}`);
      }
    }
  });
});

// Simple ping on new WS connections
wss.on('connection', (ws) => {
  console.log('WebSocket client connected');
  ws.send(JSON.stringify({ hello: 'ws-bridge', ts: Date.now() }));
  ws.on('close', () => console.log('WebSocket client disconnected'));
});
