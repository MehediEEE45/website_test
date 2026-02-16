#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>

// --- CONFIG: fill these in ---
const char* WIFI_SSID = "YOUR_WIFI_SSID";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";
const char* MQTT_BROKER = "0d34f5789e1e4a669367abfe5bd45b15.s1.eu.hivemq.cloud"; // provided
const int MQTT_PORT = 8883; // TLS port
const char* MQTT_USER = "YOUR_MQTT_USER"; // HiveMQ Cloud user (if any)
const char* MQTT_PASSWORD = "YOUR_MQTT_PASSWORD"; // HiveMQ Cloud password (if any)

// Topics
// Publishing structured telemetry to: smartpower/{device_id}/data
const char* DEVICE_ID = "device01";
const char* PUB_TOPIC = "smartpower/device01/data"; // change device01 if needed
const char* SUB_TOPIC = "smartpower/device01/commands";

WiFiClientSecure secureClient;
PubSubClient mqttClient(secureClient);

unsigned long lastPublish = 0;
const unsigned long PUBLISH_INTERVAL = 5000;

void callback(char* topic, byte* payload, unsigned int length) {
  Serial.print("Message arrived [");
  Serial.print(topic);
  Serial.print("] ");
  for (unsigned int i = 0; i < length; i++) {
    Serial.print((char)payload[i]);
  }
  Serial.println();
  // Example: toggle LED if payload == "TOGGLE"
  if (length == 6 && strncmp((char*)payload, "TOGGLE", 6) == 0) {
    digitalWrite(LED_BUILTIN, !digitalRead(LED_BUILTIN));
  }
}

void connectWiFi() {
  Serial.printf("Connecting to %s\n", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi connected");
  Serial.print("IP: ");
  Serial.println(WiFi.localIP());
}

bool mqttConnect() {
  if (mqttClient.connected()) return true;
  Serial.print("Connecting to MQTT...");
  // Use chip ID for client id
  String clientId = "ESP32-" + String((uint32_t)ESP.getEfuseMac());
  // NOTE: for a secure connection we should verify certificates. For quick testing we use setInsecure().
  secureClient.setInsecure();
  mqttClient.setServer(MQTT_BROKER, MQTT_PORT);
  mqttClient.setCallback(callback);
  if (mqttClient.connect(clientId.c_str(), MQTT_USER, MQTT_PASSWORD)) {
    Serial.println("connected");
    mqttClient.subscribe(SUB_TOPIC);
    return true;
  } else {
    Serial.print("failed, rc=");
    Serial.println(mqttClient.state());
    return false;
  }
}

void setup() {
  pinMode(LED_BUILTIN, OUTPUT);
  Serial.begin(115200);
  delay(1000);
  connectWiFi();
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    connectWiFi();
  }
  if (!mqttClient.connected()) {
    mqttConnect();
  }
  mqttClient.loop();

  unsigned long now = millis();
  if (now - lastPublish > PUBLISH_INTERVAL) {
    lastPublish = now;
    // structured telemetry payload: device_id, voltage, current, power, timestamp
    float voltage = random(480, 540) / 10.0; // simulated 48.0-54.0 V
    float current = random(0, 500) / 10.0; // simulated 0-50.0 A
    float power = voltage * current; // W
    unsigned long ts = (unsigned long)(WiFi.getTime() * 1000UL);
    String payload = "{";
    payload += "\"device_id\":\"" + String(DEVICE_ID) + "\",";
    payload += "\"voltage\":" + String(voltage, 2) + ",";
    payload += "\"current\":" + String(current, 2) + ",";
    payload += "\"power\":" + String(power, 2) + ",";
    payload += "\"timestamp\":" + String(ts) + ",";
    payload += "\"uptime_ms\":" + String(now);
    payload += "}";
    if (mqttClient.publish(PUB_TOPIC, payload.c_str())) {
      Serial.print("Published: ");
      Serial.println(payload);
    } else {
      Serial.println("Publish failed");
    }
  }
}
