/*
  Lightweight client-side Data Saver for demo/testing.
  - Stores time-series readings per device in localStorage
  - Keeps a cap per device (maxRecords) or retentionDays
  - Exposes: init, saveReading, getRecent, purgeOld, setAutoPoll
  - Polling tries to fetch from ESP32 endpoint and only saves on success (no fake/autodata)
*/

const DATA = {
    STORAGE_PREFIX: 'device_data_',
    DEFAULT_MAX_RECORDS: 500, // default cap per device

    init() {
        // noop for now - placeholder for future migration
        return true;
    },

    _key(deviceId) {
        return `${this.STORAGE_PREFIX}${deviceId}`;
    },

    // Save a reading object { timestamp, payload, userId?, meta? }
    saveReading(deviceId, reading, maxRecords = this.DEFAULT_MAX_RECORDS) {
        if (!deviceId || !reading) return { success: false, error: 'Missing params' };
        const key = this._key(deviceId);
        const arr = JSON.parse(localStorage.getItem(key) || '[]');
        arr.push({ timestamp: (new Date()).toISOString(), ...reading });
        // enforce cap
        if (arr.length > maxRecords) arr.splice(0, arr.length - maxRecords);
        localStorage.setItem(key, JSON.stringify(arr));
        return { success: true };
    },

    // Get recent readings for a device
    getRecent(deviceId, limit = 100) {
        const key = this._key(deviceId);
        const arr = JSON.parse(localStorage.getItem(key) || '[]');
        if (!Array.isArray(arr)) return [];
        return arr.slice(-limit);
    },

    // Purge old readings by retentionDays or keep maxRecords
    purgeOld(deviceId, { retentionDays = null, maxRecords = this.DEFAULT_MAX_RECORDS } = {}) {
        const key = this._key(deviceId);
        let arr = JSON.parse(localStorage.getItem(key) || '[]');
        if (!Array.isArray(arr)) arr = [];
        if (retentionDays) {
            const cutoff = Date.now() - retentionDays * 24 * 3600 * 1000;
            arr = arr.filter(r => new Date(r.timestamp).getTime() >= cutoff);
        }
        if (arr.length > maxRecords) arr = arr.slice(-maxRecords);
        localStorage.setItem(key, JSON.stringify(arr));
        return { success: true, retained: arr.length };
    },

    // Try to poll ESP32 for data at http://{ip}/metrics or /data
    async pollEsp32AndSave(espIp, deviceId, opts = {}) {
        if (!espIp) return { success: false, error: 'No IP' };
        const urls = [`http://${espIp}/metrics`, `http://${espIp}/data`, `http://${espIp}/`];
        let lastErr = null;

        function isTelemetry(payload) {
            if (!payload) return false;
            // If object, check for numeric telemetry keys or numeric values
            if (typeof payload === 'object') {
                const keys = Object.keys(payload).join(' ');
                if (/(volt|voltage|power|current|soc|energy|w|v|a)/i.test(keys)) return true;
                return Object.values(payload).some(v => typeof v === 'number' || (!isNaN(parseFloat(v)) && isFinite(v)));
            }
            // If string, look for units or numeric patterns
            if (typeof payload === 'string') {
                if (/(?:\d+\.?\d*\s*(?:v|volt|w|kw|a|amp|amps|kwh))/i.test(payload)) return true;
                if (/esp32|esp-.?32|adc|sensor|voltage|power|current/i.test(payload)) return true;
            }
            return false;
        }

        for (const url of urls) {
            try {
                const res = await fetch(url, { cache: 'no-store', mode: 'cors' });
                if (!res.ok) { lastErr = `HTTP ${res.status}`; continue; }
                const contentType = res.headers.get('content-type') || '';
                let payload;
                if (contentType.includes('application/json')) {
                    try {
                        payload = await res.json();
                    } catch (e) {
                        lastErr = 'Invalid JSON';
                        continue;
                    }
                } else {
                    const text = await res.text();
                    // try JSON parse, else keep text
                    try { payload = JSON.parse(text); } catch { payload = text; }
                }

                // Validate that the payload looks like telemetry from an ESP32
                if (!isTelemetry(payload)) {
                    lastErr = 'Invalid payload (not telemetry)';
                    continue; // try next URL
                }

                // attach user id if available
                const user = AUTH.getCurrentUser();
                const userId = user ? user.id : null;
                const saveRes = this.saveReading(deviceId, { payload, userId, meta: { source: url } }, opts.maxRecords);
                return { success: true, url, saved: saveRes.success, payloadSample: (typeof payload === 'object' ? Object.keys(payload).slice(0,4) : (String(payload).slice(0,120))) };
            } catch (err) {
                lastErr = err.message || String(err);
                continue;
            }
        }
        return { success: false, error: lastErr || 'No valid response' };
    },

    // Probe ESP32 without saving (checks telemetry validity) with abort timeout
    async probeEsp32(espIp, timeoutMs = 3000) {
        if (!espIp) return { success: false, error: 'No IP' };
        const urls = [`http://${espIp}/metrics`, `http://${espIp}/data`, `http://${espIp}/`];
        let lastErr = null;
        for (const url of urls) {
            try {
                const controller = new AbortController();
                const id = setTimeout(() => controller.abort(), timeoutMs);
                const res = await fetch(url, { cache: 'no-store', mode: 'cors', signal: controller.signal });
                clearTimeout(id);
                if (!res.ok) { lastErr = `HTTP ${res.status}`; continue; }
                const contentType = res.headers.get('content-type') || '';
                let payload;
                if (contentType.includes('application/json')) {
                    try {
                        payload = await res.json();
                    } catch (e) { lastErr = 'Invalid JSON'; continue; }
                } else {
                    const text = await res.text();
                    try { payload = JSON.parse(text); } catch { payload = text; }
                }

                function isTelemetryLocal(payload) {
                    if (!payload) return false;
                    if (typeof payload === 'object') {
                        const keys = Object.keys(payload).join(' ');
                        if (/(volt|voltage|power|current|soc|energy|w|v|a)/i.test(keys)) return true;
                        return Object.values(payload).some(v => typeof v === 'number' || (!isNaN(parseFloat(v)) && isFinite(v)));
                    }
                    if (typeof payload === 'string') {
                        if (/(?:\d+\.?\d*\s*(?:v|volt|w|kw|a|amp|amps|kwh))/i.test(payload)) return true;
                        if (/esp32|esp-.?32|adc|sensor|voltage|power|current/i.test(payload)) return true;
                    }
                    return false;
                }

                if (!isTelemetryLocal(payload)) { lastErr = 'Invalid payload (not telemetry)'; continue; }
                return { success: true, url, payloadSample: (typeof payload === 'object' ? Object.keys(payload).slice(0,4) : (String(payload).slice(0,120))) };
            } catch (err) {
                lastErr = err.name === 'AbortError' ? 'Timeout' : (err.message || String(err));
                continue;
            }
        }
        return { success: false, error: lastErr || 'No valid response' };
    },

    // Start auto polling at intervalMs; returns an object with stop()
    setAutoPoll(espIp, deviceId, intervalMs = 15000, opts = {}) {
        let stopped = false;
        const tick = async () => {
            if (stopped) return;
            const res = await this.pollEsp32AndSave(espIp, deviceId, opts);
            // optional: emit a custom event for UI to pick up
            window.dispatchEvent(new CustomEvent('device-data-saved', { detail: { deviceId, res } }));
            setTimeout(tick, intervalMs);
        };
        // start
        setTimeout(tick, 0);
        return {
            stop() { stopped = true; }
        };
    },

    // Stub for server sync; in production call backend API
    async syncToServer(deviceId, options = {}) {
        // options: url, apiKey, batchSize
        return { success: false, error: 'Not implemented' };
    }
};

// initialize small module
DATA.init();
