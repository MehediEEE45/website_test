#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <Wire.h>
#include <Adafruit_INA219.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

#ifndef LED_BUILTIN
#define LED_BUILTIN 2
#endif

// --- CONFIG: fill these in ---
const char* WIFI_SSID = "MiM";
const char* WIFI_PASSWORD = "Ha20202021";
const char* MQTT_BROKER = "0d34f5789e1e4a669367abfe5bd45b15.s1.eu.hivemq.cloud"; // provided
const int MQTT_PORT = 8883; // TLS port (use 8884 + /mqtt path for WebSockets browser)
const char* MQTT_USER = "battery"; // HiveMQ Cloud user (if any)
const char* MQTT_PASSWORD = "Batterybms80"; // HiveMQ Cloud password (if any)

// Topics
const char* PUB_TOPIC = "battery/data";
const char* SUB_TOPIC = "battery/recieve";

// Pin / I2C configuration (user-provided)
static const int OLED_SDA_PIN = 21; // OLED SDA
static const int OLED_SCL_PIN = 22; // OLED SCL
static const int INA_SDA_PIN  = 5;  // INA219 SDA
static const int INA_SCL_PIN  = 4;  // INA219 SCL
static const int BUTTON_PIN   = 25; // Button input

// I2C addresses
static const uint8_t OLED_ADDRESS = 0x3C;
// OLED dimensions
static const int OLED_WIDTH = 128;
static const int OLED_HEIGHT = 64;

// Create two separate I2C buses
TwoWire I2C_OLED = TwoWire(0);
TwoWire I2C_INA  = TwoWire(1);

// INA219 object (use I2C_INA bus)
Adafruit_INA219 ina219 = Adafruit_INA219();
bool inaPresent = false;

// OLED display (use I2C_OLED bus)
Adafruit_SSD1306 display(OLED_WIDTH, OLED_HEIGHT, &I2C_OLED, -1);
bool oledPresent = false;

WiFiClientSecure secureClient;
PubSubClient mqttClient(secureClient);

unsigned long lastPublish = 0;
const unsigned long PUBLISH_INTERVAL = 5000;

// Battery / SoC configuration
static const float BATTERY_CAPACITY_mAh = 4200.0f; // user provided
static const float INITIAL_SOC_PERCENT = 100.0f;  // change if known
// Optional: if you know a measured full capacity, set MEASURED_CAPACITY_mAh > 0 to compute SoH
// Measured capacity converted from 3000 mWh @ 3.7V -> ~810.81 mAh
static const float MEASURED_CAPACITY_mAh = 810.81f;

// Coulomb counting state
float consumed_mAh = 0.0f;
float remaining_mAh = 0.0f;
float soc_percent = INITIAL_SOC_PERCENT;
float soh_percent = 100.0f;
unsigned long lastIntegrationMillis = 0;

void callback(char* topic, byte* payload, unsigned int length) {
  Serial.print("Message arrived [");
  Serial.print(topic);
  Serial.print("] ");
  for (unsigned int i = 0; i < length; i++) {
    Serial.print((char)payload[i]);
  }
  Serial.println();
  if (length == 6 && strncmp((char*)payload, "TOGGLE", 6) == 0) {
    digitalWrite(LED_BUILTIN, !digitalRead(LED_BUILTIN));
  }
}

void connectWiFi() {
  Serial.printf("Connecting to %s\n", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print('.');
  }
  Serial.println("\nWiFi connected");
  Serial.print("IP: ");
  Serial.println(WiFi.localIP());
}

bool mqttConnect() {
  if (mqttClient.connected()) return true;
  Serial.print("Connecting to MQTT...");
  String clientId = "ESP32-" + String((uint32_t)ESP.getEfuseMac());
  secureClient.setInsecure(); // replace with CA verification in production
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
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  Serial.begin(115200);
  delay(1000);
  // Note: WiFi will be connected after sensors and OLED are initialized

  // Initialize the two I2C buses with provided pins
  // OLED on I2C_OLED (bus 0) using 400kHz
  I2C_OLED.begin(OLED_SDA_PIN, OLED_SCL_PIN, 400000);
  // INA219 on I2C_INA (bus 1) using 100kHz
  I2C_INA.begin(INA_SDA_PIN, INA_SCL_PIN, 100000);

  // Simple scanner to report devices found on each bus
  Serial.println("Scanning I2C_OLED (bus 0) ...");
  for (uint8_t addr = 1; addr < 127; ++addr) {
    I2C_OLED.beginTransmission(addr);
    uint8_t err = I2C_OLED.endTransmission();
    if (err == 0) {
      Serial.printf("  Found device at 0x%02X on I2C_OLED\n", addr);
    }
  }

  Serial.println("Scanning I2C_INA (bus 1) ...");
  for (uint8_t addr = 1; addr < 127; ++addr) {
    I2C_INA.beginTransmission(addr);
    uint8_t err = I2C_INA.endTransmission();
    if (err == 0) {
      Serial.printf("  Found device at 0x%02X on I2C_INA\n", addr);
    }
  }

  // Initialize INA219 on I2C_INA bus
  Serial.println("Initializing INA219 on I2C_INA...");
  // Adafruit_INA219::begin accepts an optional TwoWire* parameter
  if (ina219.begin(&I2C_INA)) {
    inaPresent = true;
    Serial.println("INA219 initialized");
  } else if (ina219.begin()) {
    inaPresent = true;
    Serial.println("INA219 initialized (fallback)");
  } else {
    inaPresent = false;
    Serial.println("INA219 not found");
  }

  // Initialize OLED
  Serial.println("Initializing OLED on I2C_OLED...");
  // try to initialize display using the separate I2C bus
  if (display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDRESS)) {
    oledPresent = true;
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 0);
    display.println("OLED initialized");
    display.display();
    Serial.println("OLED initialized");
  } else {
    oledPresent = false;
    Serial.println("OLED not found");
  }

  // Show INA219 status first
  if (oledPresent) {
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 0);
    if (inaPresent) display.println("INA219: OK");
    else display.println("INA219: NOT FOUND");
    display.display();
  }
  delay(1000);

  // Connect WiFi and show status on OLED
  if (oledPresent) {
    display.clearDisplay();
    display.setCursor(0, 0);
    display.println("WiFi: connecting...");
    display.display();
  }
  connectWiFi();
  if (oledPresent) {
    display.clearDisplay();
    display.setTextSize(1);
    display.setCursor(0, 0);
    display.println("WiFi: connected");
    display.println(WiFi.localIP());
    display.display();
  }
  delay(800);

  // Connect MQTT and show status
  if (oledPresent) {
    display.clearDisplay();
    display.setCursor(0, 0);
    display.println("MQTT: connecting...");
    display.display();
  }
  mqttConnect();
  if (oledPresent) {
    display.clearDisplay();
    if (mqttClient.connected()) display.println("MQTT: connected");
    else display.println("MQTT: failed");
    display.display();
  }
  delay(800);

  // Show main V / I / P page (initial values)
  if (oledPresent) {
    display.clearDisplay();
    display.setTextSize(1);
    display.setCursor(0, 0);
    display.println("V   I    P");
    display.display();
  }

  // Initialize SoC state
  remaining_mAh = BATTERY_CAPACITY_mAh * (INITIAL_SOC_PERCENT / 100.0f);
  if (MEASURED_CAPACITY_mAh > 0.0f) soh_percent = (MEASURED_CAPACITY_mAh / BATTERY_CAPACITY_mAh) * 100.0f;
  else soh_percent = 100.0f; // unknown
  lastIntegrationMillis = millis();
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
    if (inaPresent) {
      float shunt_mV = ina219.getShuntVoltage_mV();
      float bus_V = ina219.getBusVoltage_V();
      float current_mA = ina219.getCurrent_mA();
      float power_mW = ina219.getPower_mW();

      // Coulomb counting integration for SoC
      unsigned long now_ms = now;
      unsigned long dt_ms = now_ms - lastIntegrationMillis;
      if (dt_ms > 0) {
        float dt_hours = (float)dt_ms / 3600000.0f;
        // current_mA: positive = discharge (consuming), negative = charging
        float delta_mAh = current_mA * dt_hours;
        consumed_mAh += delta_mAh;
        remaining_mAh -= delta_mAh;
        // keep remaining within [0, capacity]
        if (remaining_mAh < 0.0f) remaining_mAh = 0.0f;
        if (remaining_mAh > BATTERY_CAPACITY_mAh) remaining_mAh = BATTERY_CAPACITY_mAh;
        lastIntegrationMillis = now_ms;
      }

      // Compute SoC and SoH
      soc_percent = (remaining_mAh / BATTERY_CAPACITY_mAh) * 100.0f;
      if (soc_percent < 0.0f) soc_percent = 0.0f;
      if (soc_percent > 100.0f) soc_percent = 100.0f;
      if (MEASURED_CAPACITY_mAh > 0.0f) soh_percent = (MEASURED_CAPACITY_mAh / BATTERY_CAPACITY_mAh) * 100.0f;
      float current_A = current_mA / 1000.0f;
      float power_W = power_mW / 1000.0f;
      String payload = "{\"uptime_ms\":" + String(now) +
           ",\"bus_V\":" + String(bus_V, 3) +
           ",\"shunt_mV\":" + String(shunt_mV, 3) +
           ",\"current_A\":" + String(current_A, 3) +
           ",\"power_W\":" + String(power_W, 3) +
           ",\"soc_percent\":" + String(soc_percent, 2) +
           ",\"soh_percent\":" + String(soh_percent, 2) + "}";
      if (mqttClient.publish(PUB_TOPIC, payload.c_str())) {
        Serial.print("Published INA219: ");
        Serial.println(payload);
      } else {
        Serial.println("Publish failed");
      }

      // Update OLED with concise V / I / P page
      if (oledPresent) {
        display.clearDisplay();
          display.setTextSize(2);
        display.setCursor(0, 0);
        display.print("V: ");
        display.print(String(bus_V, 2));
        display.print(" V");
        float current_A = current_mA / 1000.0f;
        float power_W = power_mW / 1000.0f;
        display.setCursor(0, 20);
        display.print("I: ");
        display.print(String(current_A, 2));
        display.print(" A");
        display.setCursor(0, 40);
        display.print("P: ");
        display.print(String(power_W, 2));
        display.print(" W");
        // show SoC/SoH on bottom line
          display.setTextSize(1);
          display.setCursor(0, 57);
          display.print("SoC:");
          display.print(String(soc_percent, 1));
          display.print("%");
          display.setCursor(80, 57);
          display.print("SoH:");
          display.print(String(soh_percent, 1));
          display.print("%");
        display.display();
      }
    } else {
      String payload = "{\"uptime_ms\":" + String(now) + ",\"value\":" + String(random(20,30)) + "}";
      if (mqttClient.publish(PUB_TOPIC, payload.c_str())) {
        Serial.print("Published: ");
        Serial.println(payload);
      } else {
        Serial.println("Publish failed");
      }
    }
  }
}
