#!/usr/bin/env python3
"""
mqtt_to_mongo.py
Subscribe to MQTT telemetry and insert into MongoDB (Atlas or local).

Usage:
  pip install paho-mqtt pymongo python-dotenv
  SUPABASE not required. Provide MONGO_URI env or --mongo-uri.

Example:
  MONGO_URI="mongodb+srv://user:pass@cluster0.abcd.mongodb.net/battery_monitor" python server/mqtt_to_mongo.py \
    --broker "wss://...:8884/mqtt" --topic battery/data --username battery --password Batterybms80

"""
from __future__ import annotations
import os
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
    raise SystemExit('Please install paho-mqtt: pip install paho-mqtt')

try:
    from pymongo import MongoClient
except Exception:
    raise SystemExit('Please install pymongo: pip install pymongo')

try:
    from dotenv import load_dotenv
    load_dotenv(dotenv_path=Path(__file__).parent / '.env')
except Exception:
    pass

stop_requested = False

def signal_handler(sig, frame):
    global stop_requested
    stop_requested = True


def parse_broker_url(url: str):
    if not url:
        return None, None, None, None
    p = urlparse(url)
    return p.hostname, p.port, p.scheme, p.path or ''


class MQTTToMongo:
    def __init__(self, mongo_uri: str, db_name: str, collection: str,
                 broker: str, port: Optional[int], scheme: Optional[str], path: Optional[str],
                 username: Optional[str], password: Optional[str], topic: str):
        self.mongo_uri = mongo_uri
        self.db_name = db_name
        self.collection_name = collection
        self.client = MongoClient(self.mongo_uri, serverSelectionTimeoutMS=5000)
        self.db = self.client[self.db_name]
        self.col = self.db[self.collection_name]

        self.broker = broker
        self.port = port
        self.scheme = scheme
        self.path = path or ''
        self.username = username
        self.password = password
        self.topic = topic

        transport = 'websockets' if (scheme in ('ws', 'wss')) else 'tcp'
        self.mqtt = mqtt.Client(transport=transport)
        self.mqtt.on_connect = self.on_connect
        self.mqtt.on_message = self.on_message
        if username:
            self.mqtt.username_pw_set(username, password)
        if scheme in ('mqtts', 'wss') or (self.port in (8883, 8884)):
            try:
                self.mqtt.tls_set()
                self.mqtt.tls_insecure_set(True)
            except Exception:
                pass
        if scheme in ('ws', 'wss') and self.path:
            try:
                self.mqtt.ws_set_options(path=self.path)
            except Exception:
                pass

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
                payload = {}

            ts = int(time.time() * 1000)
            ts_iso = time.strftime('%Y-%m-%dT%H:%M:%S', time.localtime(ts / 1000.0))

            parts = msg.topic.split('/')
            device_type = ''
            device_id = ''
            if len(parts) >= 4 and parts[0] == 'energy':
                device_type = parts[1]
                device_id = parts[2]
            elif parts and parts[0] == 'battery':
                device_type = 'esp32'
                device_id = 'esp32_1'

            doc = {
                'ts': ts,
                'ts_iso': ts_iso,
                'topic': msg.topic,
                'device_type': device_type,
                'device_id': device_id,
                'voltage': payload.get('bus_V') or payload.get('voltage'),
                'current': payload.get('current_A') or payload.get('current'),
                'power': payload.get('power_W') or payload.get('power'),
                'soc': payload.get('soc_percent') or payload.get('soc'),
                'soh': payload.get('soh_percent') or payload.get('soh'),
                'uptime_ms': payload.get('uptime_ms') or payload.get('uptime'),
                'raw_payload': payload_raw,
                'received_at': ts_iso
            }

            # Insert into MongoDB
            try:
                self.col.insert_one(doc)
                print('Inserted telemetry:', doc['ts_iso'], doc['device_id'])
            except Exception as e:
                print('Mongo insert failed:', e)

        except Exception as e:
            print('Error processing message:', e)

    def run(self):
        # verify mongo connection
        try:
            self.client.admin.command('ping')
            print('Connected to MongoDB')
        except Exception as e:
            print('Cannot connect to MongoDB:', e)
            raise

        host = self.broker
        port = self.port or (8883 if self.scheme in ('mqtts', 'wss') else 1883)
        print(f'Connecting to MQTT broker {host}:{port} (scheme={self.scheme}, path={self.path})')
        self.mqtt.connect(host, port, keepalive=60)
        try:
            while not stop_requested:
                self.mqtt.loop(timeout=1.0)
        except KeyboardInterrupt:
            pass
        finally:
            try:
                self.mqtt.disconnect()
            except Exception:
                pass


def main():
    parser = argparse.ArgumentParser(description='MQTT -> MongoDB logger')
    parser.add_argument('--broker', help='MQTT broker URL or host')
    parser.add_argument('--port', type=int, help='MQTT broker port')
    parser.add_argument('--topic', default=os.environ.get('MQTT_TOPIC_FILTER', 'battery/data'), help='MQTT topic to subscribe')
    parser.add_argument('--username', help='MQTT username')
    parser.add_argument('--password', help='MQTT password')
    parser.add_argument('--mongo-uri', help='MongoDB connection URI')
    parser.add_argument('--db', default='battery_monitor')
    parser.add_argument('--collection', default='telemetry')
    args = parser.parse_args()

    mqtt_url = os.environ.get('MQTT_URL')
    env_user = os.environ.get('MQTT_USERNAME')
    env_pass = os.environ.get('MQTT_PASSWORD')
    mongo_uri = args.mongo_uri or os.environ.get('MONGO_URI')

    if args.broker:
        if '://' in args.broker:
            host, p, scheme, path = parse_broker_url(args.broker)
        else:
            host = args.broker
            p = args.port
            scheme = None
            path = ''
    elif mqtt_url:
        host, p, scheme, path = parse_broker_url(mqtt_url)
    else:
        host = 'localhost'
        p = args.port
        scheme = None
        path = ''

    username = args.username or env_user
    password = args.password or env_pass

    if not mongo_uri:
        raise SystemExit('Please provide MongoDB URI via --mongo-uri or MONGO_URI env var')

    signal.signal(signal.SIGINT, signal_handler)
    signal.signal(signal.SIGTERM, signal_handler)

    svc = MQTTToMongo(mongo_uri, args.db, args.collection, host, p, scheme, path, username, password, args.topic)
    svc.run()


if __name__ == '__main__':
    main()
