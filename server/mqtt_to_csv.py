#!/usr/bin/env python3
"""
mqtt_to_csv.py
Subscribe to MQTT telemetry topics and append readings to a CSV file.

Usage examples:
    pip install paho-mqtt python-dotenv
    python server/mqtt_to_csv.py --outfile battery_data.csv
    # Or use server/.env values automatically

The script will read MQTT_URL, MQTT_USERNAME, MQTT_PASSWORD from environment (or .env).
"""
from __future__ import annotations
import os
import csv
import json
import time
import signal
import argparse
from pathlib import Path
from typing import Optional
from urllib.parse import urlparse

try:
    import paho.mqtt.client as mqtt
except Exception:
    raise SystemExit("Please install required package: pip install paho-mqtt")

try:
    import requests
except Exception:
    requests = None

try:
    from dotenv import load_dotenv
    load_dotenv(dotenv_path=Path(__file__).parent / '.env')
except Exception:
    # dotenv optional; env vars can still be used
    pass

DEFAULT_TOPIC = os.environ.get('MQTT_TOPIC_FILTER', 'energy/+/+/telemetry')

FIELDNAMES = [
    'ts', 'ts_iso', 'topic', 'device_type', 'device_id',
    'voltage', 'shunt_mV', 'current', 'power',
    'soc_percent', 'soh_percent', 'uptime_ms', 'raw_payload'
]

stop_requested = False

def signal_handler(sig, frame):
    global stop_requested
    stop_requested = True


def parse_broker_url(url: str):
    """Return (host, port, scheme, path)"""
    if not url:
        return None, None, None, None
    p = urlparse(url)
    scheme = p.scheme
    host = p.hostname
    port = p.port
    path = p.path or ''
    return host, port, scheme, path


class MQTTToCSV:
    def __init__(self, broker: Optional[str], port: Optional[int], scheme: Optional[str], path: Optional[str],
                 username: Optional[str], password: Optional[str], topic: str, outfile: str,
                 supabase_url: Optional[str] = None, supabase_key: Optional[str] = None):
        self.broker = broker
        self.port = port
        self.scheme = scheme
        self.path = path or ''
        self.username = username
        self.password = password
        self.topic = topic
        self.outfile = Path(outfile)
        self.supabase_url = supabase_url
        self.supabase_key = supabase_key
        # Choose transport: websockets for ws/wss, otherwise tcp
        transport = 'websockets' if (scheme in ('ws', 'wss')) else 'tcp'
        self.client = mqtt.Client(transport=transport)
        self.client.on_connect = self.on_connect
        self.client.on_message = self.on_message
        if username:
            self.client.username_pw_set(username, password)
        # TLS for mqtts
        if scheme in ('mqtts', 'wss') or (self.port == 8883) or (self.port == 8884):
            try:
                self.client.tls_set()
                # For quick testing allow insecure; remove for production
                self.client.tls_insecure_set(True)
            except Exception:
                pass

        # If using websockets and a path (e.g. /mqtt), set ws options
        if scheme in ('ws', 'wss') and self.path:
            try:
                # paho expects path without host
                self.client.ws_set_options(path=self.path)
            except Exception:
                pass

        # Ensure CSV exists and header written
        exists = self.outfile.exists()
        if not exists:
            with self.outfile.open('w', newline='', encoding='utf-8') as f:
                writer = csv.DictWriter(f, fieldnames=FIELDNAMES)
                writer.writeheader()

    def on_connect(self, client, userdata, flags, rc):
        print('MQTT connected, rc=', rc)
        client.subscribe(self.topic)
        print('Subscribed to', self.topic)

    def on_message(self, client, userdata, msg):
        try:
            payload_raw = msg.payload.decode('utf-8')
            try:
                payload = json.loads(payload_raw)
            except Exception:
                payload = payload_raw

            ts = int(time.time() * 1000)
            ts_iso = time.strftime('%Y-%m-%dT%H:%M:%S', time.localtime(ts / 1000.0))

            # Extract device_type and device_id from topic
            parts = msg.topic.split('/')
            device_type = ''
            device_id = ''
            if len(parts) >= 4 and parts[0] == 'energy':
                device_type = parts[1]
                device_id = parts[2]
            elif len(parts) >= 1 and parts[0] == 'battery':
                # ESP32 BMS publishes to battery/data
                device_type = 'battery'
                device_id = 'esp32_bms'

            p = payload if isinstance(payload, dict) else {}
            row = {
                'ts': ts,
                'ts_iso': ts_iso,
                'topic': msg.topic,
                'device_type': device_type,
                'device_id': device_id,
                # ESP32 BMS fields: bus_V / voltage, shunt_mV, current_A / current, power_W / power
                'voltage': p.get('bus_V') or p.get('voltage', ''),
                'shunt_mV': p.get('shunt_mV', ''),
                'current': p.get('current_A') or p.get('current', ''),
                'power': p.get('power_W') or p.get('power', ''),
                'soc_percent': p.get('soc_percent', ''),
                'soh_percent': p.get('soh_percent', ''),
                'uptime_ms': p.get('uptime_ms', ''),
                'raw_payload': payload_raw
            }

            # Append row to CSV
            with self.outfile.open('a', newline='', encoding='utf-8') as f:
                writer = csv.DictWriter(f, fieldnames=FIELDNAMES)
                writer.writerow(row)

            print('Saved:', device_type, device_id, '->', self.outfile)
            # Optionally forward to Supabase
            if self.supabase_url and self.supabase_key:
                try:
                    self._send_to_supabase(payload, row, msg.topic)
                except Exception as e:
                    print('Supabase upload failed:', e)
        except Exception as e:
            print('Error processing message:', e)

    def run(self):
        if not self.broker:
            raise RuntimeError('No broker host provided')
        host = self.broker
        port = self.port or (8883 if self.scheme in ('mqtts', 'wss') else 1883)
        print(f'Connecting to MQTT broker {host}:{port} (scheme={self.scheme}, path={self.path})')
        # For websockets transport paho will use ws_set_options if provided above
        self.client.connect(host, port, keepalive=60)
        # Blocking loop
        try:
            while not stop_requested:
                self.client.loop(timeout=1.0)
        except KeyboardInterrupt:
            pass
        finally:
            try:
                self.client.disconnect()
            except Exception:
                pass

    def _send_to_supabase(self, payload, row, topic):
        if requests is None:
            raise RuntimeError('requests is required for Supabase uploads (pip install requests)')

        # Normalize payload
        if isinstance(payload, dict):
            p = payload
        else:
            try:
                p = json.loads(row.get('raw_payload') or '{}')
            except Exception:
                p = {}

        data = {
            'ts': row.get('ts'),
            'ts_iso': row.get('ts_iso'),
            'topic': topic,
            'device_type': row.get('device_type'),
            'device_id': row.get('device_id'),
            'voltage': p.get('bus_V') or p.get('voltage'),
            'current': p.get('current_A') or p.get('current'),
            'power': p.get('power_W') or p.get('power'),
            'soc': p.get('soc_percent') or p.get('soc'),
            'soh': p.get('soh_percent') or p.get('soh'),
            'uptime_ms': p.get('uptime_ms') or p.get('uptime'),
            'raw_payload': row.get('raw_payload')
        }

        url = self.supabase_url.rstrip('/') + '/rest/v1/telemetry'
        headers = {
            'apikey': self.supabase_key,
            'Authorization': f'Bearer {self.supabase_key}',
            'Content-Type': 'application/json',
            'Prefer': 'return=representation'
        }

        resp = requests.post(url, headers=headers, json=[data], timeout=10)
        if resp.status_code not in (200, 201):
            raise RuntimeError(f'Supabase insert failed: {resp.status_code} {resp.text}')


def main():
    parser = argparse.ArgumentParser(description='MQTT -> CSV logger')
    parser.add_argument('--broker', help='MQTT broker host (overrides env MQTT_URL)')
    parser.add_argument('--port', type=int, help='MQTT broker port')
    parser.add_argument('--topic', default=DEFAULT_TOPIC, help='MQTT topic to subscribe')
    parser.add_argument('--username', help='MQTT username')
    parser.add_argument('--password', help='MQTT password')
    parser.add_argument('--outfile', default='battery_data.csv', help='CSV output file')
    args = parser.parse_args()

    # Environment fallbacks
    mqtt_url = os.environ.get('MQTT_URL')
    env_user = os.environ.get('MQTT_USERNAME')
    env_pass = os.environ.get('MQTT_PASSWORD')

    broker = None
    port = None
    scheme = None
    path = ''
    if args.broker:
        # allow passing full URL (scheme://host:port/path) or just host
        if '://' in args.broker:
            host, p, scheme, path = parse_broker_url(args.broker)
            broker = host
            port = p
        else:
            broker = args.broker
            port = args.port
    elif mqtt_url:
        host, p, scheme, path = parse_broker_url(mqtt_url)
        broker = host
        port = p
    else:
        broker = 'localhost'
        port = args.port

    username = args.username or env_user
    password = args.password or env_pass
    supabase_url = os.environ.get('SUPABASE_URL') or os.environ.get('DB_SUPABASE_URL')
    supabase_key = os.environ.get('SUPABASE_KEY') or os.environ.get('DB_SUPABASE_KEY')

    signal.signal(signal.SIGINT, signal_handler)
    signal.signal(signal.SIGTERM, signal_handler)

    svc = MQTTToCSV(broker, port, scheme, path, username, password, args.topic, args.outfile, supabase_url, supabase_key)
    svc.run()


if __name__ == '__main__':
    main()
