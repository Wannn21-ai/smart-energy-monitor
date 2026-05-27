#pragma once

// ================================================================
// display.h — Smart Energy Monitor v3.1
// OLED (SSD1306), LED, dan Buzzer
// ================================================================

#include <Adafruit_SSD1306.h>

// Objek display di-expose supaya modul lain bisa pakai (misal session.cpp)
extern Adafruit_SSD1306 display;

// ── Init ─────────────────────────────────────────────────────────
bool displayInit();

// ── OLED Screens ─────────────────────────────────────────────────
void oledSplash();
void oledStatus(const char* l1, const char* l2 = "");
void oledData(float v, float i, float p, float pf, float hz,
              float kwh, float cost, bool dev, bool online,
              bool ovl, bool relay, bool offline, unsigned long offMs);

// ── LED & Buzzer handlers (dipanggil tiap loop) ──────────────────
void handleBlueLed();
void handleGreenLed();
void handleOverloadAlert();
