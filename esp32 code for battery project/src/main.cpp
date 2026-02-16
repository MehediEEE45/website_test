#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_INA219.h>
// WiFi + MQTT configuration

#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <Preferences.h>
#define WIFI_SSID     "MiM"
#define WIFI_PASSWORD "Ha20202021"

// Configuration Constants
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
#define OLED_ADDRESS 0x3C

// Pin Definitions
#define OLED_SDA_PIN 21
#define OLED_SCL_PIN 22
#define INA_SDA_PIN 5
#define INA_SCL_PIN 4
#define BUTTON_PIN 25

// Timing Constants
#define UPDATE_INTERVAL 1000
#define BUTTON_DEBOUNCE_MS 250
#define I2C_TIMEOUT_MS 100
#define I2C_FREQUENCY 400000
#define SERIAL_BAUD_RATE 115200

// Display Constants
#define VOLTAGE_ROW 0
#define CURRENT_ROW 22
#define POWER_ROW 44
#define STATUS_ROW 56
#define TEXT_SIZE_LARGE 2
#define TEXT_SIZE_SMALL 1

// Global Objects
TwoWire I2C_OLED = TwoWire(0);
TwoWire I2C_INA = TwoWire(1);
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &I2C_OLED, OLED_RESET);
Adafruit_INA219 ina219(INA219_ADDRESS);

// State Variables
struct SystemState {
  bool inaFound = false;
  bool displayReady = false;
  bool useCustomBus = true;
  unsigned long lastUpdate = 0;
  unsigned long lastButtonPress = 0;
  float voltage = 0.0;
  float current = 0.0;
  float power = 0.0;
  uint32_t errorCount = 0;
  bool wifiConnected = false;
  String ipAddress = "";
} state;


// === MQTT (HiveMQ) ===
const char* mqtt_server   = "0d34f5789e1e4a669367abfe5bd45b15.s1.eu.hivemq.cloud";
const int   mqtt_port     = 8883;
const char* mqtt_user     = "battery";
const char* mqtt_pass     = "Batterybsm80";
String clientIdStr;
// Device identifier used by dashboard
const char* deviceId = "battery_1";

// Topics (will be built at runtime to include deviceId)
String pubTopic;
String subTopic;

WiFiClientSecure espClient;
PubSubClient mqttClient(espClient);

// Persistent settings
Preferences prefs;
// Flip sign if your INA219 wiring makes current sign inverted
bool invertCurrent = false;

// Function Declarations
void initializeSerial();
bool initializeI2C();
bool initializeDisplay();
bool initializeINA219();
void scanI2CBus(TwoWire &wire, const char* busName);
void handleButton();
void updateReadings();
void updateDisplay();
void printDiagnostics();
void connectWiFi();
void reconnectMQTT();
void mqttCallback(char* topic, byte* payload, unsigned int length);
void publishMetricsMQTT();

void setup() {
  initializeSerial();
  
  // Initialize hardware components
  if (!initializeI2C()) {
    Serial.println("FATAL: I2C initialization failed");
    while(1) { delay(1000); }
  }
  
  if (!initializeDisplay()) {
    Serial.println("FATAL: Display initialization failed");
    while(1) { delay(1000); }
  }
  
  // Initialize button
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  Serial.println("Button initialized on GPIO" + String(BUTTON_PIN));
  
  // Scan I2C buses for diagnostics
  scanI2CBus(I2C_OLED, "OLED");
  scanI2CBus(I2C_INA, "INA219");
  
  // Initialize INA219
  initializeINA219();
  
  // Show startup message
  display.clearDisplay();
  display.setTextSize(TEXT_SIZE_SMALL);
  display.setCursor(0, 0);
  display.println("ESP32 Power Monitor");
  display.println("INA219 + SSD1306");
  display.println("");
  display.print("INA219: ");
  display.println(state.inaFound ? "OK" : "FAIL");
  display.print("Bus: ");
  display.println(state.useCustomBus ? "Custom" : "Default");
  display.display();
  
  delay(2000); // Show startup screen
  
  Serial.println("=== Setup Complete ===");
  printDiagnostics();

  // Connect to WiFi
  display.clearDisplay();
  display.setTextSize(TEXT_SIZE_SMALL);
  display.setCursor(0, 0);
  display.println("Connecting WiFi...");
  display.display();
  Serial.println("Connecting to WiFi...");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  unsigned long wifiStart = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - wifiStart < 15000) {
    delay(500);
    display.setCursor(0, 16);
    display.print(".");
    display.display();
    Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED) {
    state.wifiConnected = true;
    state.ipAddress = WiFi.localIP().toString();
    Serial.println("\nWiFi connected!");
    Serial.print("IP Address: ");
    Serial.println(state.ipAddress);

    // Load persisted settings
    prefs.begin("monitor", false);
    invertCurrent = prefs.getBool("invert_current", false);
    Serial.printf("Invert current setting (from prefs): %s\n", invertCurrent ? "ENABLED" : "disabled");

    // Quick auto-check: if readings are consistently negative and invert is not enabled, enable it automatically
    float sum = 0;
    int samples = 6;
    for (int i = 0; i < samples; i++) {
      updateReadings();
      delay(200);
      sum += state.current;
    }
    float avg = sum / samples;
    Serial.printf("Auto-check avg current: %.3f A\n", avg);
    if (avg < -0.05 && !invertCurrent) {
      invertCurrent = true;
      prefs.putBool("invert_current", invertCurrent);
      Serial.println("Auto-correct: invert_current ENABLED due to negative average readings");
    }

    // Setup MQTT client
    espClient.setInsecure(); // NOTE: skips certificate validation (quick testing)
    mqttClient.setServer(mqtt_server, mqtt_port);
    mqttClient.setCallback(mqttCallback);

    // Build unique client ID from MAC address and topics including deviceId
    clientIdStr = String("ESP32_") + WiFi.macAddress();
    clientIdStr.replace(":", "_");
    pubTopic = String("energy/battery/") + deviceId + "/telemetry"; // dashboard subscribes to this
    subTopic = String("energy/battery/") + deviceId + "/command";    // optional command topic
    Serial.printf("MQTT clientId: %s\n", clientIdStr.c_str());
    Serial.printf("MQTT pub topic: %s\n", pubTopic.c_str());
    Serial.printf("MQTT sub topic: %s\n", subTopic.c_str());

    display.clearDisplay();
    display.setCursor(0, 0);
    display.println("WiFi Connected!");
    display.setCursor(0, 16);
    display.print("IP: ");
    display.println(state.ipAddress);
    display.display();
    delay(2000);
  } else {
    state.wifiConnected = false;
    state.ipAddress = "";
    Serial.println("\nWiFi connection failed!");
    display.clearDisplay();
    display.setCursor(0, 0);
    display.println("WiFi Failed!");
    display.display();
    delay(2000);
  }
}

void loop() {
  // Handle MQTT when WiFi connected
  if (state.wifiConnected) {
    if (!mqttClient.connected()) {
      reconnectMQTT();
    }
    mqttClient.loop();
  }

  // Serial commands: send 'i' or 'invert' to toggle invert setting for testing
  if (Serial.available()) {
    String cmd = Serial.readStringUntil('\n');
    cmd.trim();
    if (cmd.equalsIgnoreCase("i") || cmd.equalsIgnoreCase("invert")) {
      invertCurrent = !invertCurrent;
      prefs.putBool("invert_current", invertCurrent);
      Serial.printf("Invert current now %s\n", invertCurrent ? "ENABLED" : "disabled");
    }
  }
  
  handleButton();
  
  unsigned long now = millis();
  if (now - state.lastUpdate >= UPDATE_INTERVAL) {
    state.lastUpdate = now;
    updateReadings();
    updateDisplay();
    
    // Print to serial and publish to MQTT every 5 seconds
    static unsigned long lastSerialPrint = 0;
    if (now - lastSerialPrint >= 5000) {
      lastSerialPrint = now;
      Serial.printf("[%lu] V:%.2f I:%.3f P:%.3f Bus:%s INA:%s\n", 
                    now/1000, state.voltage, state.current, state.power,
                    state.useCustomBus ? "Custom" : "Default",
                    state.inaFound ? "OK" : "FAIL");
      if (state.wifiConnected && mqttClient.connected()) {
        publishMetricsMQTT();
      }
    }
  }
  
  // Small delay to prevent excessive CPU usage
  delay(10);
}

// === Implementation Functions ===

void initializeSerial() {
  Serial.begin(SERIAL_BAUD_RATE);
  delay(100);
  Serial.println();
  Serial.println("==============================");
  Serial.println("ESP32 Smart Power Monitor v2.0");
  Serial.println("INA219 Current Sensor + OLED");
  Serial.println("==============================");
}

// ---------------- MQTT Helpers ----------------
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  Serial.print("MQTT Message arrived [");
  Serial.print(topic);
  Serial.print("] : ");

  String msg;
  for (unsigned int i = 0; i < length; i++) {
    msg += (char)payload[i];
  }
  Serial.println(msg);

  // Simple command handling: toggle invert if payload contains "invert"
  if (msg.equalsIgnoreCase("invert") || msg.equalsIgnoreCase("toggle_invert")) {
    invertCurrent = !invertCurrent;
    prefs.putBool("invert_current", invertCurrent);
    Serial.printf("Invert current now %s\n", invertCurrent ? "ENABLED" : "disabled");
  }
}

void reconnectMQTT() {
  // Loop until reconnected
  while (!mqttClient.connected()) {
    Serial.print("Connecting to MQTT...");
    if (mqttClient.connect(clientIdStr.c_str(), mqtt_user, mqtt_pass)) {
      Serial.println("Connected to MQTT");
      if (subTopic.length() > 0) mqttClient.subscribe(subTopic.c_str());
    } else {
      int8_t st = mqttClient.state();
      Serial.print("Failed to connect, rc=");
      Serial.print(st);
      // Helpful hint for common failures
      switch (st) {
        case -4: Serial.println(" (connection timeout)"); break;
        case -3: Serial.println(" (connection lost)"); break;
        case -2: Serial.println(" (network unavailable)"); break;
        case -1: Serial.println(" (invalid state)"); break;
        case 0: Serial.println(" (success)"); break;
        case 1: Serial.println(" (refused, unacceptable protocol)"); break;
        case 2: Serial.println(" (refused, bad client id)"); break;
        case 3: Serial.println(" (refused, server unavailable)"); break;
        case 4: Serial.println(" (refused, bad credentials)"); break;
        case 5: Serial.println(" (refused, not authorised)"); break;
        default: Serial.println(); break;
      }
      Serial.println("Retrying in 2s...");
      delay(2000);
    }
  }
}

void publishMetricsMQTT() {
  float signedC = state.current * (invertCurrent ? -1.0 : 1.0);
  float absC = fabs(signedC);
  float absP = fabs(state.power);
  float signedP = absP * (signedC >= 0 ? 1.0 : -1.0);
  String dir = "idle";
  if (absC >= 0.005) dir = (signedC > 0) ? "charging" : "discharging";

  String json = "{";
  json += "\"voltage\": "; json += String(state.voltage, 2); json += ",";
  json += "\"current_signed\": "; json += String(signedC, 3); json += ",";
  json += "\"current\": "; json += String(absC, 3); json += ",";
  json += "\"power_signed\": "; json += String(signedP, 3); json += ",";
  json += "\"power\": "; json += String(absP, 3); json += ",";
  json += "\"direction\": \"" + dir + "\",";
  json += "\"invert_current\": "; json += (invertCurrent ? "true" : "false"); json += ",";
  json += "\"inaFound\": "; json += (state.inaFound ? "true" : "false"); json += ",";
  json += "\"wifiConnected\": "; json += (state.wifiConnected ? "true" : "false"); json += ",";
  json += "\"ip\": \"" + state.ipAddress + "\",";
  json += "\"uptime\": "; json += String(millis() / 1000); json += ",";
  json += "\"errorCount\": "; json += String(state.errorCount);
  json += "}";

  // Publish using the runtime-built topic
  if (pubTopic.length() > 0) {
    mqttClient.publish(pubTopic.c_str(), json.c_str());
  } else {
    // fallback to the old topic if not initialized (shouldn't happen)
    mqttClient.publish("esp32/battery/data", json.c_str());
  }
}

bool initializeI2C() {
  Serial.println("Initializing I2C buses...");
  
  // Initialize OLED I2C bus
  bool oledSuccess = I2C_OLED.begin(OLED_SDA_PIN, OLED_SCL_PIN, I2C_FREQUENCY);
  Serial.printf("OLED I2C (SDA:%d, SCL:%d): %s\n", 
                OLED_SDA_PIN, OLED_SCL_PIN, oledSuccess ? "OK" : "FAIL");
  
  // Initialize INA219 I2C bus
  bool inaSuccess = I2C_INA.begin(INA_SDA_PIN, INA_SCL_PIN, I2C_FREQUENCY);
  Serial.printf("INA I2C (SDA:%d, SCL:%d): %s\n", 
                INA_SDA_PIN, INA_SCL_PIN, inaSuccess ? "OK" : "FAIL");
  
  return oledSuccess; // At minimum, OLED bus must work
}

bool initializeDisplay() {
  Serial.println("Initializing OLED display...");
  
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDRESS)) {
    Serial.println("ERROR: SSD1306 allocation failed!");
    return false;
  }
  
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(TEXT_SIZE_SMALL);
  
  state.displayReady = true;
  Serial.printf("Display initialized at address 0x%02X\n", OLED_ADDRESS);
  return true;
}

bool initializeINA219() {
  Serial.println("Initializing INA219 sensor...");
  Serial.printf("Expected INA219 address: 0x%02X\n", INA219_ADDRESS);
  
  state.inaFound = false;
  
  if (state.useCustomBus) {
    Serial.println("Attempting to init INA219 on custom I2C bus...");
    if (ina219.begin(&I2C_INA)) {
      state.inaFound = true;
      ina219.setCalibration_32V_2A();
      Serial.println("INA219 initialized successfully on custom bus");
    } else {
      Serial.println("WARNING: INA219 not found on custom I2C bus");
    }
  } else {
    Serial.println("Attempting to init INA219 on default I2C bus...");
    if (ina219.begin()) {
      state.inaFound = true;
      ina219.setCalibration_32V_2A();
      Serial.println("INA219 initialized successfully on default bus");
    } else {
      Serial.println("WARNING: INA219 not found on default I2C bus");
    }
  }
  
  return state.inaFound;
}

void scanI2CBus(TwoWire &wire, const char* busName) {
  Serial.printf("Scanning %s I2C bus...\n", busName);
  
  uint8_t devicesFound = 0;
  for (uint8_t addr = 1; addr < 127; addr++) {
    wire.beginTransmission(addr);
    uint8_t error = wire.endTransmission();
    
    if (error == 0) {
      Serial.printf("Device found at address 0x%02X\n", addr);
      devicesFound++;
    }
    delay(1); // Small delay between scans
  }
  
  Serial.printf("%s bus scan complete: %d device(s) found\n", busName, devicesFound);
}

void handleButton() {
  static bool lastButtonState = HIGH;
  bool currentButtonState = digitalRead(BUTTON_PIN);
  unsigned long now = millis();
  
  // Detect button press (HIGH to LOW transition with debouncing)
  if (lastButtonState == HIGH && currentButtonState == LOW && 
      (now - state.lastButtonPress) > BUTTON_DEBOUNCE_MS) {
    
    state.lastButtonPress = now;
    state.useCustomBus = !state.useCustomBus;
    
    Serial.printf("\n=== BUTTON PRESSED ===\n");
    Serial.printf("Switching to %s I2C bus\n", 
                  state.useCustomBus ? "custom" : "default");
    
    // Re-initialize INA219 on new bus
    initializeINA219();
    
    // Show bus switch notification on display
    display.clearDisplay();
    display.setTextSize(TEXT_SIZE_SMALL);
    display.setCursor(0, 0);
    display.println("Bus Switched!");
    display.print("Using: ");
    display.println(state.useCustomBus ? "Custom" : "Default");
    display.print("INA219: ");
    display.println(state.inaFound ? "Found" : "Not Found");
    display.display();
    
    delay(1500); // Show notification
  }
  
  lastButtonState = currentButtonState;
}

void updateReadings() {
  if (state.inaFound) {
    try {
      state.voltage = ina219.getBusVoltage_V();
      state.current = ina219.getCurrent_mA() / 1000.0; // Signed Amps (may be negative depending on flow)
      state.power = ina219.getPower_mW() / 1000.0;     // Signed Watts

      // Small noise around zero is common; treat tiny values as zero
      if (fabs(state.current) < 0.005) state.current = 0.0; // <5 mA noise

      // Validate readings (basic sanity checks)
      if (state.voltage < 0 || state.voltage > 50) state.voltage = 0;
      if (fabs(state.current) > 100) state.current = 0; // >100A unrealistic - clamp
      if (fabs(state.power) > 500) state.power = 0; // >500W unrealistic for setup

    } catch (...) {
      Serial.println("ERROR: Exception during INA219 reading");
      state.errorCount++;
      state.voltage = state.current = state.power = 0;
    }
  } else {
    state.voltage = state.current = state.power = 0;
  }
}

void updateDisplay() {
  if (!state.displayReady) return;
  
  display.clearDisplay();
  display.setTextWrap(false);

  // Top: Voltage (large)
  display.setTextSize(2);
  display.setCursor(0, 0);
  display.print("V: ");
  display.printf("%.2f", state.voltage);

  // Middle: Current and Power
  display.setTextSize(1);
  float signedCurrentDisplay = state.current * (invertCurrent ? -1.0f : 1.0f);
  float absCurrent = fabs(signedCurrentDisplay);
  String dir = "IDL";
  if (absCurrent >= 0.005f) dir = (signedCurrentDisplay > 0) ? "CHG" : "DSG";

  display.setCursor(0, 26);
  display.print(dir);
  display.print(" ");
  display.printf("%.3fA", absCurrent);

  // Power on the right side
  display.setCursor(80, 26);
  display.print("P:");
  display.printf("%.2fW", fabs(state.power));

  // Bottom: compact status row
  display.setTextSize(1);
  display.setCursor(0, 48);
  display.print(state.inaFound ? "INA:OK " : "INA:ERR ");
  display.print(state.useCustomBus ? "Bus:C " : "Bus:D ");
  display.print(invertCurrent ? "Inv:Y" : "Inv:N");

  // IP / WiFi indicator (small, right)
  display.setCursor(80, 48);
  if (state.wifiConnected) display.print(state.ipAddress);
  else display.print("WiFi:--");

  // Error indicator
  if (state.errorCount > 0) {
    display.setCursor(0, 56);
    display.print("E:");
    display.print(state.errorCount);
  }

  display.display();
}

// (HTTP server handlers removed — MQTT is used instead)

void printDiagnostics() {
  Serial.println("\n=== DIAGNOSTIC INFO ===");
  Serial.printf("Firmware: ESP32 Power Monitor v2.0\n");
  Serial.printf("Chip Model: %s Rev %d\n", ESP.getChipModel(), ESP.getChipRevision());
  Serial.printf("CPU Frequency: %d MHz\n", ESP.getCpuFreqMHz());
  Serial.printf("Free Heap: %d bytes\n", ESP.getFreeHeap());
  Serial.printf("Flash Size: %d bytes\n", ESP.getFlashChipSize());
  Serial.println("");
  Serial.printf("OLED I2C: SDA=%d, SCL=%d\n", OLED_SDA_PIN, OLED_SCL_PIN);
  Serial.printf("INA I2C: SDA=%d, SCL=%d\n", INA_SDA_PIN, INA_SCL_PIN);
  Serial.printf("Button Pin: %d\n", BUTTON_PIN);
  Serial.printf("Current Bus: %s\n", state.useCustomBus ? "Custom" : "Default");
  Serial.printf("INA219 Status: %s\n", state.inaFound ? "Connected" : "Not Found");
  Serial.printf("Display Status: %s\n", state.displayReady ? "Ready" : "Failed");
  Serial.println("========================\n");
}
