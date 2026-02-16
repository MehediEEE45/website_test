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

// --- Server sync utilities ---
DATA.SERVER_CONFIG_KEY = 'device_data_servercfg';

// sync local readings for a device to server (batch POST)
DATA.syncToServer = async function(deviceId, options = {}) {
    const cfgRaw = localStorage.getItem(this.SERVER_CONFIG_KEY) || '{}';
    const cfg = Object.assign({ baseUrl: '/api' }, JSON.parse(cfgRaw));
    const url = (options.url || cfg.baseUrl).replace(/\/$/, '');
    const batchSize = options.batchSize || 50;

    const readings = this.getRecent(deviceId, batchSize);
    if (!readings || readings.length === 0) return { success: false, error: 'No readings to sync' };

    try {
        // send each reading (could be batched in future)
        for (const r of readings) {
            const body = { device_id: deviceId, topic: r.meta?.topic || '', payload: r.payload || r };
            await fetch(url + '/readings', {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify(body)
            });
        }
        return { success: true, sent: readings.length };
    } catch (err) {
        return { success: false, error: err.message || String(err) };
    }
};

// fetch recent readings from server for a device
DATA.fetchFromServer = async function(deviceId, options = {}) {
    const cfgRaw = localStorage.getItem(this.SERVER_CONFIG_KEY) || '{}';
    const cfg = Object.assign({ baseUrl: '/api' }, JSON.parse(cfgRaw));
    const url = (options.url || cfg.baseUrl).replace(/\/$/, '');
    const limit = options.limit || 100;
    try {
        const res = await fetch(`${url}/readings/${encodeURIComponent(deviceId)}?limit=${limit}`, { cache: 'no-store' });
        if (!res.ok) return { success: false, error: `HTTP ${res.status}` };
        const data = await res.json();
        return { success: true, data };
    } catch (err) {
        return { success: false, error: err.message || String(err) };
    }
};

// Simple UI panel for server sync
DATA.renderServerPanel = function() {
    if (document.getElementById('data-server-panel')) return;
    const panel = document.createElement('div');
    panel.id = 'data-server-panel';
    panel.style.position = 'fixed';
    panel.style.left = '12px';
    panel.style.bottom = '12px';
    panel.style.width = '420px';
    panel.style.maxHeight = '60vh';
    panel.style.overflow = 'auto';
    panel.style.background = 'rgba(12,12,12,0.95)';
    panel.style.border = '1px solid #333';
    panel.style.borderRadius = '8px';
    panel.style.padding = '12px';
    panel.style.zIndex = 9999;
    panel.style.color = '#ddd';
    panel.style.fontSize = '13px';
    panel.innerHTML = `
        <div style="display:flex;justify-content:space-between;align-items:center;margin-bottom:8px;">
            <strong>Data Server</strong>
            <button id="data-server-close" style="background:transparent;border:0;color:#999;cursor:pointer">✕</button>
        </div>
        <label style="font-size:12px">Server Base URL</label>
        <input id="data-server-url" style="width:100%;padding:8px;margin-bottom:8px;border-radius:6px;border:1px solid #444;background:#0b0b0b;color:#eee" placeholder="/api or https://host:3000/api" />
        <label style="font-size:12px">Device ID</label>
        <input id="data-device-id" style="width:100%;padding:8px;margin-bottom:8px;border-radius:6px;border:1px solid #444;background:#0b0b0b;color:#eee" placeholder="battery_battery_1" />
        <div style="display:flex;gap:8px;justify-content:flex-end;margin-bottom:8px">
            <button id="data-fetch" style="padding:8px 12px;border-radius:6px;border:0;background:#2563eb;color:#fff;cursor:pointer">Fetch</button>
            <button id="data-sync" style="padding:8px 12px;border-radius:6px;border:0;background:#16a34a;color:#fff;cursor:pointer">Upload</button>
        </div>
        <div style="font-size:12px;color:#9ca3af">Status: <span id="data-server-status">idle</span></div>
        <div id="data-server-output" style="margin-top:8px;font-size:13px;color:#ddd;white-space:pre-wrap"></div>
    `;

    document.body.appendChild(panel);

    const urlEl = document.getElementById('data-server-url');
    const devEl = document.getElementById('data-device-id');
    const fetchBtn = document.getElementById('data-fetch');
    const syncBtn = document.getElementById('data-sync');
    const closeBtn = document.getElementById('data-server-close');
    const statusEl = document.getElementById('data-server-status');
    const outEl = document.getElementById('data-server-output');

    // load saved config
    try {
        const saved = JSON.parse(localStorage.getItem(this.SERVER_CONFIG_KEY) || '{}');
        urlEl.value = saved.baseUrl || '/api';
        devEl.value = saved.lastDeviceId || '';
    } catch (e) { urlEl.value = '/api'; }

    closeBtn.addEventListener('click', () => panel.remove());

    fetchBtn.addEventListener('click', async () => {
        const base = urlEl.value.trim() || '/api';
        const deviceId = devEl.value.trim();
        if (!deviceId) return alert('Enter device id');
        localStorage.setItem(DATA.SERVER_CONFIG_KEY, JSON.stringify({ baseUrl: base, lastDeviceId: deviceId }));
        statusEl.textContent = 'fetching...';
        outEl.textContent = '';
        const res = await DATA.fetchFromServer(deviceId, { url: base, limit: 200 });
        if (!res.success) { statusEl.textContent = 'error'; outEl.textContent = res.error; return; }
        statusEl.textContent = `fetched ${res.data.length}`;
        outEl.textContent = JSON.stringify(res.data.slice(0,200), null, 2);
    });

    syncBtn.addEventListener('click', async () => {
        const base = urlEl.value.trim() || '/api';
        const deviceId = devEl.value.trim();
        if (!deviceId) return alert('Enter device id');
        localStorage.setItem(DATA.SERVER_CONFIG_KEY, JSON.stringify({ baseUrl: base, lastDeviceId: deviceId }));
        statusEl.textContent = 'uploading...';
        outEl.textContent = '';
        const res = await DATA.syncToServer(deviceId, { url: base, batchSize: 200 });
        if (!res.success) { statusEl.textContent = 'error'; outEl.textContent = res.error; return; }
        statusEl.textContent = `uploaded ${res.sent}`;
        outEl.textContent = `Uploaded ${res.sent} readings to ${base}`;
    });
};

// Auto-render server panel (user can close it)
if (typeof window !== 'undefined') {
    window.addEventListener('load', () => {
        try { DATA.renderServerPanel(); } catch (e) { console.warn('Failed to render server panel', e); }
    });
}
