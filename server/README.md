# MQTT -> SQLite Bridge

This small service subscribes to an MQTT topic filter and stores telemetry into a local SQLite database. It also exposes a simple HTTP API to query recent readings.

Usage

1. Copy `.env.example` to `.env` and edit values (broker URL, credentials, port, DB file).

```bash
cd server
cp .env.example .env
# edit .env
npm install
npm start
```

Endpoints

- `GET /api/health` — check service & MQTT connection
- `GET /api/readings/:deviceId?limit=100` — get recent readings for a device
- `POST /api/readings` — add a reading manually (json: `{device_id, topic, payload}`)

Notes

- The bridge expects telemetry topics like `energy/{type}/{deviceId}/telemetry`. It derives `device_id` as `{type}_{deviceId}`.
- Payloads are stored as JSON text. Consider rotating DB and backups for production.
