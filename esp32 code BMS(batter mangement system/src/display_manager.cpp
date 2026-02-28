#include "display_manager.h"
#include "config.h"
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// ── Private state ──
static TwoWire          I2C_OLED(0);
static Adafruit_SSD1306 oled(OLED_WIDTH, OLED_HEIGHT, &I2C_OLED, -1);
static bool             present = false;

// ── Public API ──
bool display_init() {
    I2C_OLED.begin(OLED_SDA_PIN, OLED_SCL_PIN, 400000);

    // Quick scan
    Serial.println("[OLED] Scanning I2C bus 0...");
    for (uint8_t a = 1; a < 127; ++a) {
        I2C_OLED.beginTransmission(a);
        if (I2C_OLED.endTransmission() == 0)
            Serial.printf("[OLED]   Found 0x%02X\n", a);
    }

    present = oled.begin(SSD1306_SWITCHCAPVCC, OLED_ADDRESS);
    if (present) {
        oled.clearDisplay();
        oled.setTextSize(1);
        oled.setTextColor(SSD1306_WHITE);
        oled.setCursor(0, 0);
        oled.println("OLED OK");
        oled.display();
    }
    Serial.printf("[OLED] %s\n", present ? "OK" : "NOT FOUND");
    return present;
}

bool display_isPresent() { return present; }

void display_status(const char* line1, const char* line2) {
    if (!present) return;
    oled.clearDisplay();
    oled.setTextSize(1);
    oled.setTextColor(SSD1306_WHITE);
    oled.setCursor(0, 0);
    oled.println(line1);
    if (line2) oled.println(line2);
    oled.display();
}

void display_sensorPage(const SampleRecord& rec, float tempC) {
    if (!present) return;
    oled.clearDisplay();

    // V / I / P  –  large text
    oled.setTextSize(2);
    oled.setCursor(0, 0);
    oled.print("V:");  oled.print(String(rec.bus_V, 2));      oled.print("V");
    oled.setCursor(0, 20);
    oled.print("I:");  oled.print(String(rec.current_A, 2));  oled.print("A");
    oled.setCursor(0, 40);
    oled.print("P:");  oled.print(String(rec.power_W, 2));    oled.print("W");

    // T / SoC / SoH  –  small text
    oled.setTextSize(1);
    oled.setCursor(0, 50);
    oled.print("T:");  oled.print(String(tempC, 1));  oled.print("C");
    oled.setCursor(0, 57);
    oled.print("SoC:"); oled.print(String(rec.soc_percent, 1)); oled.print("%");
    oled.setCursor(80, 57);
    oled.print("SoH:"); oled.print(String(rec.soh_percent, 1)); oled.print("%");

    oled.display();
}

void display_tempOnlyPage(float tempC) {
    if (!present) return;
    oled.clearDisplay();
    oled.setTextSize(2);
    oled.setCursor(0, 0);
    oled.print("Temp:");
    oled.setCursor(0, 22);
    oled.print(String(tempC, 1));
    oled.print(" C");
    oled.setTextSize(1);
    oled.setCursor(0, 56);
    oled.print("INA219 not found");
    oled.display();
}
