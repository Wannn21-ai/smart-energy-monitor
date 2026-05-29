// ================================================================
// display.cpp — Smart Energy Monitor v3.1
// Implementasi OLED SSD1306, LED, dan Buzzer
// ================================================================

#include "display.h"
#include "config.h"
#include "state.h"

#include <Wire.h>
#include <WiFi.h>
#include <Adafruit_GFX.h>

#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT 64

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

// ================================================================
// INIT
// ================================================================
bool displayInit() {
    if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
        Serial.println("[OLED] Init gagal");
        return false;
    }
    return true;
}

// ================================================================
// SPLASH SCREEN
// ================================================================
void oledSplash() {
    display.clearDisplay();
    display.setTextColor(WHITE);
    display.setTextSize(1);
    display.setCursor(0, 0);  display.println("SMART ENERGY");
    display.setCursor(0, 10); display.println("MONITOR v3.1");
    display.drawLine(0, 20, 127, 20, WHITE);
    display.setCursor(0, 26); display.println("Initializing...");
    display.display();
}

// ================================================================
// STATUS SCREEN — 2 baris teks
// ================================================================
void oledStatus(const char* l1, const char* l2) {
    display.clearDisplay();
    display.setTextColor(WHITE);
    display.setTextSize(1);
    display.setCursor(0, 20); display.println(l1);
    if (l2 && strlen(l2) > 0) {
        display.setCursor(0, 36); display.println(l2);
    }
    display.display();
}

// ================================================================
// DATA SCREEN — layout lengkap saat monitoring
//
// Row 0  (y=0):  [mode badge]          [relay badge]
// Row 1  (y=9):  separator
// Row 2  (y=13): device name (max 16 char)
// Row 3  (y=23): V: xxx.xV  I: x.xxA
// Row 4  (y=33): P: xxxx.xW PF: x.xx
// Row 5  (y=43): separator
// Row 6  (y=47): E: x.xxxx kWh
// Row 7  (y=57): Cost: Rp xxxxx
// ================================================================
void oledData(float v, float i, float p, float pf, float hz,
              float kwh, float cost, bool dev, bool online,
              bool ovl, bool relay, bool offline, unsigned long offMs) {
    display.clearDisplay();
    display.setTextColor(WHITE);
    display.setTextSize(1);

    // ── Row 0: mode + relay badges ──────────────────────────────
    display.setCursor(0, 0);
    if (offline) {
        unsigned long s = offMs / 1000;
        if (s < 60)  display.printf("[OFF %lus]",        s);
        else         display.printf("[OFF %lum%02lus]",  s / 60, s % 60);
    } else {
        display.print(online ? "[ONLINE]" : "[NONET]");
    }
    const char* relayBadge = relay ? "[RLY:ON]" : "[RLY:OFF]";
    int badgeX = 128 - (int)strlen(relayBadge) * 6;
    if (badgeX < 64) badgeX = 64;
    display.setCursor(badgeX, 0);
    display.print(relayBadge);

    // ── Separator ────────────────────────────────────────────────
    display.drawLine(0, 9, 127, 9, WHITE);

    // ── Overload state ───────────────────────────────────────────
    if (ovl || overloadAlertLinger) {
        display.setCursor(10, 13); display.println("!! OVERLOAD !!");
        display.setCursor(0,  23); display.printf("%.1fW >= %.0fW", lastP, appConfig.overloadThreshold);
        display.setCursor(0,  33); display.println("Relay OFF");
        display.setCursor(0,  43); display.println("192.168.4.1");
        if (offline) {
            display.setCursor(0, 53); display.println("Relay ON otomatis...");
        }
        display.display();
        return;
    }

    if (overloadWarning) {
        display.setCursor(0, 13); display.println("WARN: Near overload");
        display.setCursor(0, 23); display.printf("P:%.1fW / %.0fW", p, appConfig.overloadThreshold);
        display.setCursor(0, 33); display.println("Kurangi beban");
        display.drawLine(0, 43, 127, 43, WHITE);
        display.setCursor(0, 47); display.printf("E:%.4fkWh", kwh);
        display.setCursor(0, 57);
        if (cost >= 1000) display.printf("Rp %lu",  (unsigned long)cost);
        else              display.printf("Rp %.1f", cost);
        display.display();
        return;
    }

    // ── Relay OFF state ──────────────────────────────────────────
    if (!relay) {
        display.setCursor(10, 20); display.println("Relay OFF");
        if (offline) {
            display.setCursor(0, 32); display.println("Tahan 1s: Sesi Baru");
            display.setCursor(0, 44); display.println("Tahan 5s: Reset WiFi");
            display.setCursor(0, 56); display.println("192.168.4.1");
        } else {
            display.setCursor(0, 32); display.println("Web: klik + utk mulai");
            if (!online) {
                display.setCursor(0, 44); display.println("Menunggu WiFi...");
            }
        }
        display.display();
        return;
    }

    // ── Relay ON, no device ──────────────────────────────────────
    if (!dev) {
        char shortName[17];
        strlcpy(shortName, sessionDeviceName, sizeof(shortName));
        display.setCursor(0, 13); display.printf("%-16s", shortName);
        display.setCursor(5, 25); display.println("No Device");
        display.setCursor(0, 37); display.println("Colokkan beban...");
        if (offline) {
            display.setCursor(0, 49); display.println("Mode: Offline");
        }
        display.display();
        return;
    }

    // ── Normal data display ──────────────────────────────────────
    char shortName[17];
    strlcpy(shortName, sessionDeviceName, sizeof(shortName));
    display.setCursor(0, 13); display.printf("%-16s", shortName);

    display.setCursor(0, 23); display.printf("V:%.1fV  I:%.2fA", v, i);
    display.setCursor(0, 33); display.printf("P:%.1fW PF:%.2f",  p, pf);

    display.drawLine(0, 43, 127, 43, WHITE);

    display.setCursor(0, 47); display.printf("E:%.4fkWh", kwh);
    display.setCursor(0, 57);
    if (cost >= 1000) display.printf("Rp %lu",  (unsigned long)cost);
    else              display.printf("Rp %.1f", cost);

    display.display();
}

// ================================================================
// LED HANDLERS
// ================================================================
void handleBlueLed() {
    if (wifiConnected && WiFi.status() == WL_CONNECTED) {
        digitalWrite(PIN_LED_BLUE, HIGH);
        return;
    }
    if (millis() - lastBlinkMs >= 500) {
        blinkState = !blinkState;
        digitalWrite(PIN_LED_BLUE, blinkState);
        lastBlinkMs = millis();
    }
}

void handleGreenLed() {
    digitalWrite(PIN_LED_GREEN, (sessionActive && relayOn) ? HIGH : LOW);
}

// ================================================================
// OVERLOAD ALERT — LED merah + buzzer, dengan linger
// ================================================================
void handleOverloadAlert() {
    if (overloadAlertLinger && !isOverload) {
        if (millis() - overloadLingerStart >= OVERLOAD_ALERT_LINGER) {
            overloadAlertLinger = false;
            overloadBlinkState  = false;
            digitalWrite(PIN_LED_RED, LOW);
            digitalWrite(PIN_BUZZER,  LOW);
            // Catatan: restart sesi offline setelah linger ditangani di session.cpp
            return;
        }
    }
    bool active = isOverload || overloadAlertLinger;
    if (!active) {
        if (overloadWarning) {
            if (millis() - lastWarningBlinkMs >= OVERLOAD_WARNING_BLINK_MS) {
                warningBlinkState = !warningBlinkState;
                digitalWrite(PIN_LED_RED, warningBlinkState ? HIGH : LOW);
                digitalWrite(PIN_BUZZER,  warningBlinkState ? HIGH : LOW);
                lastWarningBlinkMs = millis();
            }
            return;
        }
        digitalWrite(PIN_LED_RED, LOW);
        digitalWrite(PIN_BUZZER,  LOW);
        overloadBlinkState = false;
        warningBlinkState = false;
        return;
    }
    if (millis() - lastOverloadBlinkMs >= OVERLOAD_BLINK_MS) {
        overloadBlinkState = !overloadBlinkState;
        digitalWrite(PIN_LED_RED, overloadBlinkState ? HIGH : LOW);
        digitalWrite(PIN_BUZZER,  overloadBlinkState ? HIGH : LOW);
        lastOverloadBlinkMs = millis();
    }
}
