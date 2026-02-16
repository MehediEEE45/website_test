require('dotenv').config();
const fs = require('fs');
const path = require('path');
const mqtt = require('mqtt');
const express = require('express');
const bodyParser = require('body-parser');
const sqlite3 = require('sqlite3').verbose();
const { MongoClient, ServerApiVersion } = require('mongodb');

const MQTT_URL = process.env.MQTT_URL || 'mqtt://localhost:1883';
const MQTT_USERNAME = process.env.MQTT_USERNAME || '';
const MQTT_PASSWORD = process.env.MQTT_PASSWORD || '';
const MQTT_TOPIC_FILTER = process.env.MQTT_TOPIC_FILTER || 'energy/+/+/telemetry';
const PORT = parseInt(process.env.PORT || '3000', 10);
const DB_FILE = process.env.DB_FILE || 'telemetry.db';
const MONGO_URI = process.env.MONGO_URI || '';
const MONGO_DB = process.env.MONGO_DB || 'battery_monitor';
const MONGO_COLLECTION = process.env.MONGO_COLLECTION || 'telemetry';
const MONGO_TTL_DAYS = process.env.MONGO_TTL_DAYS ? parseInt(process.env.MONGO_TTL_DAYS, 10) : 0;

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

// Optional: MongoDB client and collection
let mongoClient = null;
let mongoCol = null;
async function initMongo() {
  if (!MONGO_URI) return;
  try {
    mongoClient = new MongoClient(MONGO_URI, {
      serverApi: {
        version: ServerApiVersion.v1,
        strict: true,
        deprecationErrors: true,
      },
      connectTimeoutMS: 5000,
    });
    await mongoClient.connect();
    // ping to verify connection
    await mongoClient.db('admin').command({ ping: 1 });
    const db = mongoClient.db(MONGO_DB);
    mongoCol = db.collection(MONGO_COLLECTION);
    console.log('Connected to MongoDB', MONGO_DB, MONGO_COLLECTION);

    // Ensure indexes: device_id + ts descending for queries
    await mongoCol.createIndex({ device_id: 1, ts: -1 });

    // Optional TTL index on `ts` (convert days to seconds)
    if (MONGO_TTL_DAYS > 0) {
      const seconds = MONGO_TTL_DAYS * 24 * 60 * 60;
      // create a TTL index on 'ts_date' which will store a proper Date
      await mongoCol.createIndex({ ts_date: 1 }, { expireAfterSeconds: seconds });
      console.log('Created TTL index to expire documents after', MONGO_TTL_DAYS, 'days');
    }
  } catch (e) {
    console.error('MongoDB init error', e);
    try { await mongoClient.close(); } catch (e2) {}
    mongoClient = null;
    mongoCol = null;
  }
}

// Connect to MQTT
// MQTT connection options with reconnection logic and clientId
const mqttOptions = {
  reconnectPeriod: 5000,
  clientId: process.env.MQTT_CLIENT_ID || `node-bridge-${Math.random().toString(16).slice(2, 8)}`,
  clean: true,
};
if (MQTT_USERNAME) mqttOptions.username = MQTT_USERNAME;
if (MQTT_PASSWORD) mqttOptions.password = MQTT_PASSWORD;

console.log('Connecting to MQTT broker:', MQTT_URL, 'clientId=', mqttOptions.clientId);
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

client.on('message', async (topic, message) => {
  let payload = null;
  const raw = message.toString();
  try { payload = JSON.parse(raw); } catch { payload = raw; }

  // Extract device id from topic if present (energy/{type}/{deviceId}/telemetry)
  const parts = topic.split('/');
  let deviceId = topic;
  if (parts.length >= 3) deviceId = `${parts[1]}_${parts[2]}`;

  // Basic validation and normalization
  const doc = {
    device_id: deviceId,
    topic,
    raw: raw,
    ts: Date.now(),
  };

  if (payload && typeof payload === 'object') {
    doc.device_id = payload.device_id || deviceId;
    doc.voltage = Number(payload.voltage || payload.bus_V || payload.v || null) || null;
    doc.current = Number(payload.current || payload.current_A || payload.i || null) || null;
    doc.power = Number(payload.power || payload.power_W || null) || null;
    doc.timestamp = payload.timestamp || payload.ts || null;
    doc.uptime_ms = payload.uptime_ms || null;
    doc.payload = payload;
  } else {
    doc.payload = raw;
  }

  // Save to sqlite for compatibility
  saveReading(doc.device_id, topic, doc.payload);

  // Save to MongoDB if enabled
  if (mongoCol) {
    try {
      // add a Date field for TTL index if configured
      doc.ts_date = new Date(doc.ts);
      await mongoCol.insertOne(doc);
    } catch (e) {
      console.error('Mongo insert failed', e);
    }
  }

  console.log(`Saved reading for ${doc.device_id} topic=${topic}`);
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
