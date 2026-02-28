#pragma once
// ===============================================================
//  config.h  –  All project-wide constants, pins & credentials
// ===============================================================

#include <Arduino.h>

// ───── LED ─────
#ifndef LED_BUILTIN
#define LED_BUILTIN 2
#endif

// ───── LM35 Temperature Sensor ─────
// ** GPIO 32 is on ADC1 – safe to use while WiFi is active **
#define LM35_PIN        32
#define DEFAULT_VREF    1100   // mV (eFuse fallback)
#define ADC_SAMPLES     10     // averaging window

// LM35 2-point calibration: temp = SLOPE * raw + OFFSET
#define LM35_SLOPE_DEFAULT   1.03f
#define LM35_OFFSET_DEFAULT -10.0f   // °C

// ───── WiFi (tried in order) ─────
struct WifiCred { const char* ssid; const char* password; };
static const WifiCred WIFI_CREDENTIALS[] = {
    { "MiM",                "Ha20202021"  },
    { "Teachers_WiFi_SUST", "SUST11s34"   },
    { "SUST WiFi",          "SUST10s10"   },
};
static const size_t WIFI_CRED_COUNT =
    sizeof(WIFI_CREDENTIALS) / sizeof(WIFI_CREDENTIALS[0]);
static const unsigned long WIFI_TIMEOUT_MS = 15000;  // per SSID

// ───── MQTT (HiveMQ Cloud, TLS) ─────
#define MQTT_BROKER   "0d34f5789e1e4a669367abfe5bd45b15.s1.eu.hivemq.cloud"
#define MQTT_PORT     8883
#define MQTT_USER     "battery"
#define MQTT_PASSWORD "Batterybms80"
#define PUB_TOPIC     "battery/data"
#define SUB_TOPIC     "battery/recieve"

// ───── I2C Pins ─────
#define OLED_SDA_PIN  21
#define OLED_SCL_PIN  22
#define INA_SDA_PIN    5
#define INA_SCL_PIN    4

// ───── OLED ─────
#define OLED_ADDRESS  0x3C
#define OLED_WIDTH    128
#define OLED_HEIGHT   64

// ───── Misc Pins ─────
#define BUTTON_PIN    25

// ───── Battery / SoC ─────
#define BATTERY_CAPACITY_mAh    4200.0f
#define INITIAL_SOC_PERCENT     100.0f
// Measured capacity from 3000 mWh @ 3.7 V → ~810.81 mAh
#define MEASURED_CAPACITY_mAh   810.81f

// ───── Timing ─────
#define PUBLISH_INTERVAL_MS     5000

// ───── EEPROM Buffering ─────
#define EEPROM_TOTAL_SIZE       4096
