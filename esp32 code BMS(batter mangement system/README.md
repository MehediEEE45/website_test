ESP32 MQTT example (HiveMQ Cloud)

Prerequisites
- ESP32 board package installed in Arduino IDE or PlatformIO.
- Library: `PubSubClient` (install via Library Manager).

Files
- `esp32_mqtt.ino`: ESP32 Arduino sketch that connects to HiveMQ Cloud using TLS (port 8883), publishes to `esp32/test` and subscribes to `esp32/commands`.
 - `web_mqtt_example.html`: simple browser example using Eclipse Paho JS client to connect via WSS to `wss://0d34f5789e1e4a669367abfe5bd45b15.s1.eu.hivemq.cloud:8884/mqtt`.

Setup
1. Open `esp32_mqtt.ino` and fill `WIFI_SSID`, `WIFI_PASSWORD`, `MQTT_USER`, and `MQTT_PASSWORD`.
2. (Optional) Replace `secureClient.setInsecure()` with proper CA verification in production.
3. Select your ESP32 board and upload the sketch.
4. Open Serial Monitor at `115200` baud to see connection and messages.

Test
- Publish to `esp32/commands` (e.g., payload `TOGGLE`) to toggle the onboard LED.
- Subscribe to `esp32/test` to receive periodic JSON messages published by the ESP32.

WebSocket (WSS) example
- Open `web_mqtt_example.html` in a browser (or serve it with a simple HTTP server).
- Default connection in the example: `wss://0d34f5789e1e4a669367abfe5bd45b15.s1.eu.hivemq.cloud:8884/mqtt` with username `Mehedi` and password `Me107645`.
- Use the UI to connect, subscribe to `esp32/commands`, and publish to `esp32/test`.

Notes
- This example uses `setInsecure()` for simplicity; in production provide the broker CA certificate and verify it.
- HiveMQ Cloud may require specific username/password or token—use credentials from your HiveMQ Cloud instance.
 - HiveMQ Cloud supports MQTT/TLS on port `8883` and MQTT over WebSocket TLS on port `8884` (path `/mqtt`).
 - The repository includes both a native ESP32 TLS example (`esp32_mqtt.ino`) and a browser WebSocket example (`web_mqtt_example.html`).
