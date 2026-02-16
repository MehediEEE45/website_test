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
    from dotenv import load_dotenv
    load_dotenv(dotenv_path=Path(__file__).parent / '.env')
except Exception:
    # dotenv optional; env vars can still be used
    pass

DEFAULT_TOPIC = os.environ.get('MQTT_TOPIC_FILTER', 'energy/+/+/telemetry')

FIELDNAMES = [
    'ts', 'ts_iso', 'topic', 'device_type', 'device_id',
    'voltage', 'current_signed', 'current', 'power_signed', 'power',
    'direction', 'ip', 'raw_payload'
]

stop_requested = False

def signal_handler(sig, frame):
    global stop_requested
    stop_requested = True


def parse_broker_url(url: str):
    """Return (host, port, scheme)"""
    if not url:
        return None, None, None
    p = urlparse(url)
    scheme = p.scheme
    host = p.hostname
    port = p.port
    return host, port, scheme


class MQTTToCSV:
    def __init__(self, broker: Optional[str], port: Optional[int], scheme: Optional[str],
                 username: Optional[str], password: Optional[str], topic: str, outfile: str):
        self.broker = broker
        self.port = port
        self.scheme = scheme
        self.username = username
        self.password = password
        self.topic = topic
        self.outfile = Path(outfile)
        self.client = mqtt.Client()
        self.client.on_connect = self.on_connect
        self.client.on_message = self.on_message
        if username:
            self.client.username_pw_set(username, password)
        # TLS for mqtts
        if scheme == 'mqtts' or (self.port == 8883):
            try:
                self.client.tls_set()
                # For quick testing allow insecure; remove for production
                self.client.tls_insecure_set(True)
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

            # Extract device_type and device_id from topic if it matches energy/{type}/{device}/telemetry
            parts = msg.topic.split('/')
            device_type = ''
            device_id = ''
            if len(parts) >= 4 and parts[0] == 'energy':
                device_type = parts[1]
                device_id = parts[2]

            row = {
                'ts': ts,
                'ts_iso': ts_iso,
                'topic': msg.topic,
                'device_type': device_type,
                'device_id': device_id,
                'voltage': payload.get('voltage') if isinstance(payload, dict) else '',
                'current_signed': payload.get('current_signed') if isinstance(payload, dict) else '',
                'current': payload.get('current') if isinstance(payload, dict) else '',
                'power_signed': payload.get('power_signed') if isinstance(payload, dict) else '',
                'power': payload.get('power') if isinstance(payload, dict) else '',
                'direction': payload.get('direction') if isinstance(payload, dict) else '',
                'ip': payload.get('ip') if isinstance(payload, dict) else '',
                'raw_payload': payload_raw
            }

            # Append row to CSV
            with self.outfile.open('a', newline='', encoding='utf-8') as f:
                writer = csv.DictWriter(f, fieldnames=FIELDNAMES)
                writer.writerow(row)

            print('Saved:', device_type, device_id, '->', self.outfile)
        except Exception as e:
            print('Error processing message:', e)

    def run(self):
        if not self.broker:
            raise RuntimeError('No broker host provided')
        host = self.broker
        port = self.port or (8883 if self.scheme == 'mqtts' else 1883)
        print(f'Connecting to MQTT broker {host}:{port} (scheme={self.scheme})')
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
    if args.broker:
        broker = args.broker
        port = args.port
        scheme = None
    elif mqtt_url:
        host, p, scheme = parse_broker_url(mqtt_url)
        broker = host
        port = p
    else:
        broker = 'localhost'
        port = args.port

    username = args.username or env_user
    password = args.password or env_pass

    signal.signal(signal.SIGINT, signal_handler)
    signal.signal(signal.SIGTERM, signal_handler)

    svc = MQTTToCSV(broker, port, scheme, username, password, args.topic, args.outfile)
    svc.run()


if __name__ == '__main__':
    main()
