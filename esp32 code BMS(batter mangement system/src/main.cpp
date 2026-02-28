// ================================================================
//  main.cpp  –  Slim orchestrator for BMS firmware
//  All logic lives in separate modules; see src/*.h for details.
// ================================================================
#include <Arduino.h>
#include "config.h"
#include "wifi_manager.h"
#include "mqtt_manager.h"
#include "temperature.h"
#include "ina219_sensor.h"
#include "display_manager.h"
#include "eeprom_buffer.h"

static unsigned long lastPublish = 0;

// ─────────────────── SETUP ───────────────────
void setup() {
    pinMode(LED_BUILTIN, OUTPUT);
    pinMode(BUTTON_PIN, INPUT_PULLUP);
    Serial.begin(115200);
    delay(1000);

    // 1. Offline buffer
    eeprom_buffer_init();

    // 2. Sensors
    temp_init();
    display_init();
    ina_init();

    // Show INA219 status on OLED
    display_status(ina_isPresent() ? "INA219: OK" : "INA219: NOT FOUND");
    delay(1000);

    // 3. Connectivity
    display_status("WiFi: connecting...");
    wifi_connect();
    if (wifi_isConnected()) {
        display_status("WiFi: connected", WiFi.localIP().toString().c_str());
    } else {
        display_status("WiFi: FAILED");
    }
    delay(800);

    display_status("MQTT: connecting...");
    mqtt_init();
    mqtt_connect();
    display_status(mqtt_isConnected() ? "MQTT: connected" : "MQTT: FAILED");
    delay(800);

    // 4. Initial display page
    display_status("V   I    P");
}

// ─────────────────── LOOP ────────────────────
void loop() {
    // Keep connections alive
    if (!wifi_isConnected())  wifi_connect();
    if (!mqtt_isConnected())  mqtt_connect();
    mqtt_loop();

    unsigned long now = millis();
    if (now - lastPublish < PUBLISH_INTERVAL_MS) return;
    lastPublish = now;

    // Temperature (always available; uses ADC1 so no WiFi conflict)
    float tempC = temp_readCelsius();
    Serial.printf("[Loop] Temp: %.2f C\n", tempC);

    // Build sample record
    SampleRecord rec = {};
    if (ina_isPresent()) {
        ina_readSample(now, rec);
    } else {
        rec.uptime_ms = (uint32_t)now;
    }

    // Publish or buffer
    bool online = wifi_isConnected() && mqtt_isConnected();
    if (online && mqtt_publishSample(rec, tempC)) {
        if (ina_isPresent()) {
            Serial.printf("[Loop] Published V=%.3f I=%.3f T=%.2f\n",
                          rec.bus_V, rec.current_A, tempC);
        } else {
            Serial.printf("[Loop] Published temp-only T=%.2f\n", tempC);
        }
        mqtt_flushBuffer();   // drain any offline records
    } else {
        Serial.println("[Loop] Offline – buffering");
        buffer_push(rec);
    }

    // Update OLED
    if (ina_isPresent()) {
        display_sensorPage(rec, tempC);
    } else {
        display_tempOnlyPage(tempC);
    }
}

