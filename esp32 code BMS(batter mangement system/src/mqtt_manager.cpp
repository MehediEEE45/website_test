#include "mqtt_manager.h"
#include "config.h"
#include "wifi_manager.h"
#include <WiFiClientSecure.h>

// ── Internals ──
static WiFiClientSecure secureClient;
static PubSubClient     client(secureClient);

static void onMessage(char* topic, byte* payload, unsigned int len) {
    Serial.printf("[MQTT] Msg [%s]: ", topic);
    for (unsigned int i = 0; i < len; i++) Serial.print((char)payload[i]);
    Serial.println();

    if (len == 6 && strncmp((char*)payload, "TOGGLE", 6) == 0) {
        digitalWrite(LED_BUILTIN, !digitalRead(LED_BUILTIN));
    }
}

// ── Public API ──
void mqtt_init() {
    secureClient.setInsecure();              // skip CA check (dev)
    client.setServer(MQTT_BROKER, MQTT_PORT);
    client.setCallback(onMessage);
    client.setBufferSize(512);               // room for larger JSON
}

bool mqtt_isConnected() {
    return client.connected();
}

bool mqtt_connect() {
    if (client.connected()) return true;
    if (!wifi_isConnected()) return false;

    String id = "ESP32-" + String((uint32_t)ESP.getEfuseMac(), HEX);
    Serial.print("[MQTT] Connecting... ");

    if (client.connect(id.c_str(), MQTT_USER, MQTT_PASSWORD)) {
        Serial.println("OK");
        client.subscribe(SUB_TOPIC);
        mqtt_flushBuffer();                  // drain offline cache
        return true;
    }
    Serial.printf("FAIL rc=%d\n", client.state());
    return false;
}

void mqtt_loop() {
    client.loop();
}

bool mqtt_publishSample(const SampleRecord& rec, float temperature) {
    if (!client.connected()) return false;
    char buf[300];
    snprintf(buf, sizeof(buf),
        "{\"uptime_ms\":%u,"
         "\"bus_V\":%.3f,"
         "\"shunt_mV\":%.3f,"
         "\"current_A\":%.3f,"
         "\"power_W\":%.3f,"
         "\"temperature\":%.2f,"
         "\"soc_percent\":%.2f,"
         "\"soh_percent\":%.2f}",
        (unsigned)rec.uptime_ms,
        rec.bus_V, rec.shunt_mV,
        rec.current_A, rec.power_W,
        temperature,
        rec.soc_percent, rec.soh_percent);
    return client.publish(PUB_TOPIC, buf);
}

void mqtt_flushBuffer() {
    SampleRecord r;
    while (!buffer_is_empty()) {
        if (!client.connected()) return;
        if (!buffer_pop(r)) return;
        // Publish without temperature (historical record)
        char buf[256];
        snprintf(buf, sizeof(buf),
            "{\"uptime_ms\":%u,"
             "\"bus_V\":%.3f,"
             "\"shunt_mV\":%.3f,"
             "\"current_A\":%.3f,"
             "\"power_W\":%.3f,"
             "\"soc_percent\":%.2f,"
             "\"soh_percent\":%.2f,"
             "\"offline\":true}",
            (unsigned)r.uptime_ms,
            r.bus_V, r.shunt_mV,
            r.current_A, r.power_W,
            r.soc_percent, r.soh_percent);
        if (!client.publish(PUB_TOPIC, buf)) return;
        delay(20);
    }
}
