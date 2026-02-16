/*
  ESP32 Energy Monitor - MQTT Publisher
  
  This sketch reads sensor data and publishes to MQTT broker.
  Works with the Energy Monitoring Dashboard.
  
  Wiring (example for Solar):
  - Voltage sensor: A0 (with voltage divider for >3.3V)
  - Current sensor (ACS712): A1
  - Temperature sensor (NTC/LM35): A2
  
  Dependencies:
  - PubSubClient library
  - ArduinoJson library
  - WiFi library (built-in for ESP32)
  
  Install via Arduino Library Manager:
  - PubSubClient by Nick O'Leary
  - ArduinoJson by Benoit Blanchon
*/

#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>

// =====================================================
// CONFIGURATION - Edit these values
// =====================================================

// WiFi credentials
const char* WIFI_SSID = "YOUR_WIFI_SSID";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";

// MQTT Broker settings
const char* MQTT_BROKER = "192.168.1.100";  // Your MQTT broker IP
const int MQTT_PORT = 1883;                  // Standard MQTT port
const char* MQTT_USER = "";                  // Leave empty if no auth
const char* MQTT_PASSWORD = "";              // Leave empty if no auth

// Device settings
const char* DEVICE_TYPE = "solar";           // "solar", "wind", or "battery"
const char* DEVICE_ID = "1";                 // Unique device identifier

// Sensor pins (adjust based on your wiring)
const int PIN_VOLTAGE = 34;      // ADC pin for voltage
const int PIN_CURRENT = 35;      // ADC pin for current
const int PIN_TEMPERATURE = 32;  // ADC pin for temperature

// Calibration values (adjust based on your sensors)
const float VOLTAGE_MULTIPLIER = 0.0165;    // For voltage divider (12V max -> 3.3V)
const float CURRENT_MULTIPLIER = 0.066;     // For ACS712-30A
const float CURRENT_OFFSET = 2.5;           // ACS712 zero-current voltage

// Publish interval (milliseconds)
const unsigned long PUBLISH_INTERVAL = 5000;  // 5 seconds

// =====================================================
// INTERNAL VARIABLES - Don't edit
// =====================================================

WiFiClient wifiClient;
PubSubClient mqttClient(wifiClient);

char mqttTopic[64];
unsigned long lastPublish = 0;
float totalEnergy = 0.0;  // Accumulated energy in Wh
unsigned long lastEnergyCalc = 0;

// =====================================================
// SETUP
// =====================================================

void setup() {
    Serial.begin(115200);
    Serial.println("\n\n=== ESP32 Energy Monitor ===");
    Serial.printf("Device: %s/%s\n", DEVICE_TYPE, DEVICE_ID);
    
    // Configure ADC
    analogReadResolution(12);  // 12-bit ADC (0-4095)
    analogSetAttenuation(ADC_11db);  // Full range 0-3.3V
    
    // Build MQTT topic
    snprintf(mqttTopic, sizeof(mqttTopic), "energy/%s/%s/telemetry", DEVICE_TYPE, DEVICE_ID);
    Serial.printf("MQTT Topic: %s\n", mqttTopic);
    
    // Connect to WiFi
    connectWiFi();
    
    // Setup MQTT
    mqttClient.setServer(MQTT_BROKER, MQTT_PORT);
    mqttClient.setCallback(mqttCallback);
    mqttClient.setBufferSize(512);
    
    lastEnergyCalc = millis();
}

// =====================================================
// MAIN LOOP
// =====================================================

void loop() {
    // Ensure WiFi is connected
    if (WiFi.status() != WL_CONNECTED) {
        connectWiFi();
    }
    
    // Ensure MQTT is connected
    if (!mqttClient.connected()) {
        connectMQTT();
    }
    mqttClient.loop();
    
    // Publish telemetry at interval
    unsigned long now = millis();
    if (now - lastPublish >= PUBLISH_INTERVAL) {
        lastPublish = now;
        publishTelemetry();
    }
}

// =====================================================
// WIFI CONNECTION
// =====================================================

void connectWiFi() {
    Serial.printf("Connecting to WiFi: %s", WIFI_SSID);
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 30) {
        delay(500);
        Serial.print(".");
        attempts++;
    }
    
    if (WiFi.status() == WL_CONNECTED) {
        Serial.println(" Connected!");
        Serial.printf("IP Address: %s\n", WiFi.localIP().toString().c_str());
    } else {
        Serial.println(" Failed!");
        Serial.println("Restarting in 5 seconds...");
        delay(5000);
        ESP.restart();
    }
}

// =====================================================
// MQTT CONNECTION
// =====================================================

void connectMQTT() {
    Serial.printf("Connecting to MQTT broker: %s:%d", MQTT_BROKER, MQTT_PORT);
    
    String clientId = "esp32-" + String(DEVICE_TYPE) + "-" + String(DEVICE_ID);
    
    int attempts = 0;
    while (!mqttClient.connected() && attempts < 5) {
        Serial.print(".");
        
        bool connected;
        if (strlen(MQTT_USER) > 0) {
            connected = mqttClient.connect(clientId.c_str(), MQTT_USER, MQTT_PASSWORD);
        } else {
            connected = mqttClient.connect(clientId.c_str());
        }
        
        if (connected) {
            Serial.println(" Connected!");
            
            // Subscribe to command topic
            char cmdTopic[64];
            snprintf(cmdTopic, sizeof(cmdTopic), "energy/%s/%s/command", DEVICE_TYPE, DEVICE_ID);
            mqttClient.subscribe(cmdTopic);
            Serial.printf("Subscribed to: %s\n", cmdTopic);
            
            return;
        }
        
        Serial.printf(" Failed (rc=%d), retrying...\n", mqttClient.state());
        delay(2000);
        attempts++;
    }
    
    Serial.println("MQTT connection failed after 5 attempts");
}

// =====================================================
// MQTT CALLBACK (for commands)
// =====================================================

void mqttCallback(char* topic, byte* payload, unsigned int length) {
    Serial.printf("Message received on topic: %s\n", topic);
    
    // Parse JSON command
    StaticJsonDocument<256> doc;
    DeserializationError error = deserializeJson(doc, payload, length);
    
    if (error) {
        Serial.printf("JSON parse error: %s\n", error.c_str());
        return;
    }
    
    // Handle commands
    const char* command = doc["command"];
    if (command) {
        Serial.printf("Command: %s\n", command);
        
        if (strcmp(command, "reset_energy") == 0) {
            totalEnergy = 0;
            Serial.println("Energy counter reset");
        }
        else if (strcmp(command, "status") == 0) {
            // Publish immediate status
            publishTelemetry();
        }
        else if (strcmp(command, "restart") == 0) {
            Serial.println("Restarting...");
            delay(1000);
            ESP.restart();
        }
    }
}

// =====================================================
// SENSOR READING
// =====================================================

float readVoltage() {
    int raw = analogRead(PIN_VOLTAGE);
    float voltage = (raw / 4095.0) * 3.3 / VOLTAGE_MULTIPLIER;
    return voltage;
}

float readCurrent() {
    int raw = analogRead(PIN_CURRENT);
    float voltage = (raw / 4095.0) * 3.3;
    float current = (voltage - CURRENT_OFFSET) / CURRENT_MULTIPLIER;
    if (current < 0) current = 0;  // No negative current for solar
    return current;
}

float readTemperature() {
    int raw = analogRead(PIN_TEMPERATURE);
    // Simple linear conversion for LM35 (10mV per degree C)
    float voltage = (raw / 4095.0) * 3.3;
    float temperature = voltage * 100;  // LM35: 10mV/°C
    return temperature;
}

// =====================================================
// TELEMETRY PUBLISHING
// =====================================================

void publishTelemetry() {
    // Read sensors
    float voltage = readVoltage();
    float current = readCurrent();
    float power = voltage * current;
    float temperature = readTemperature();
    
    // Calculate energy (Wh) based on time elapsed
    unsigned long now = millis();
    float hours = (now - lastEnergyCalc) / 3600000.0;  // ms to hours
    totalEnergy += power * hours;
    lastEnergyCalc = now;
    
    // Calculate efficiency (simplified - based on temperature)
    // Real calculation would need irradiance data
    float efficiency = 100.0 - (temperature - 25.0) * 0.4;  // -0.4%/°C above 25°C
    if (efficiency > 100) efficiency = 100;
    if (efficiency < 0) efficiency = 0;
    
    // Build JSON payload
    StaticJsonDocument<256> doc;
    doc["voltage"] = round(voltage * 100) / 100.0;
    doc["current"] = round(current * 100) / 100.0;
    doc["power"] = round(power * 100) / 100.0;
    doc["energy"] = round(totalEnergy * 100) / 100.0;
    doc["temperature"] = round(temperature * 10) / 10.0;
    doc["efficiency"] = round(efficiency * 10) / 10.0;
    doc["timestamp"] = now;
    doc["rssi"] = WiFi.RSSI();
    
    // Add device-type specific fields
    if (strcmp(DEVICE_TYPE, "wind") == 0) {
        // Simulated wind-specific data
        doc["rpm"] = random(500, 1500);
        doc["windSpeed"] = random(5, 20);
    }
    else if (strcmp(DEVICE_TYPE, "battery") == 0) {
        // Simulated battery-specific data
        doc["soc"] = random(20, 95);
        doc["capacity"] = 95.5;
        doc["cycles"] = 250;
    }
    
    // Serialize and publish
    char buffer[256];
    size_t len = serializeJson(doc, buffer);
    
    if (mqttClient.publish(mqttTopic, buffer, len)) {
        Serial.printf("Published to %s:\n", mqttTopic);
        Serial.println(buffer);
    } else {
        Serial.println("Publish failed!");
    }
}

// =====================================================
// OPTIONAL: HTTP Server for direct access
// =====================================================

#ifdef ENABLE_HTTP_SERVER
#include <WebServer.h>
WebServer httpServer(80);

void setupHttpServer() {
    httpServer.on("/", HTTP_GET, []() {
        httpServer.send(200, "text/plain", "ESP32 Energy Monitor");
    });
    
    httpServer.on("/metrics", HTTP_GET, []() {
        StaticJsonDocument<256> doc;
        doc["voltage"] = readVoltage();
        doc["current"] = readCurrent();
        doc["power"] = readVoltage() * readCurrent();
        doc["energy"] = totalEnergy;
        doc["temperature"] = readTemperature();
        
        String response;
        serializeJson(doc, response);
        httpServer.send(200, "application/json", response);
    });
    
    httpServer.begin();
    Serial.printf("HTTP Server started at http://%s/\n", WiFi.localIP().toString().c_str());
}
#endif
