// ================================================================
// indicators.cpp - Non-blocking LED and buzzer indicator manager
// ================================================================

#include "indicators.h"
#include "config.h"
#include "state.h"

#include <Arduino.h>
#include <WiFi.h>

namespace {

enum class BinaryPattern {
    OFF,
    SOLID,
    BLINK
};

enum class AlertPattern {
    OFF,
    WARNING,
    ALARM
};

bool wifiSearching = false;
bool captivePortalActive = false;

BinaryPattern lastBluePattern = BinaryPattern::OFF;
bool blueBlinkState = false;
unsigned long lastBlueBlinkMs = 0;

AlertPattern lastAlertPattern = AlertPattern::OFF;
bool alertBlinkState = false;
unsigned long lastAlertBlinkMs = 0;

void writeAlert(bool on) {
    digitalWrite(PIN_LED_RED, on ? HIGH : LOW);
    digitalWrite(PIN_BUZZER, on ? HIGH : LOW);
}

void applyBluePattern(BinaryPattern pattern, unsigned long now) {
    if (pattern != lastBluePattern) {
        lastBluePattern = pattern;
        blueBlinkState = false;
        lastBlueBlinkMs = now;
    }

    switch (pattern) {
        case BinaryPattern::SOLID:
            digitalWrite(PIN_LED_BLUE, HIGH);
            break;
        case BinaryPattern::BLINK:
            if (now - lastBlueBlinkMs >= WIFI_LED_BLINK_MS) {
                blueBlinkState = !blueBlinkState;
                lastBlueBlinkMs = now;
            }
            digitalWrite(PIN_LED_BLUE, blueBlinkState ? HIGH : LOW);
            break;
        case BinaryPattern::OFF:
        default:
            digitalWrite(PIN_LED_BLUE, LOW);
            break;
    }
}

void applyAlertPattern(AlertPattern pattern, unsigned long intervalMs, unsigned long now) {
    if (pattern != lastAlertPattern) {
        lastAlertPattern = pattern;
        alertBlinkState = false;
        lastAlertBlinkMs = now;
        writeAlert(false);
    }

    if (pattern == AlertPattern::OFF) {
        writeAlert(false);
        return;
    }

    if (now - lastAlertBlinkMs >= intervalMs) {
        alertBlinkState = !alertBlinkState;
        lastAlertBlinkMs = now;
        writeAlert(alertBlinkState);
    }
}

void updateBlue(unsigned long now) {
    bool connected = wifiConnected && WiFi.status() == WL_CONNECTED;
    if (connected) {
        applyBluePattern(BinaryPattern::SOLID, now);
    } else if (wifiSearching || captivePortalActive) {
        applyBluePattern(BinaryPattern::BLINK, now);
    } else {
        applyBluePattern(BinaryPattern::OFF, now);
    }
}

void updateGreen() {
    bool monitoringLoad = sessionActive && deviceConnected;
    digitalWrite(PIN_LED_GREEN, monitoringLoad ? HIGH : LOW);
}

void updateAlert(unsigned long now) {
    if (overloadAlertLinger && !isOverload &&
        now - overloadLingerStart >= OVERLOAD_ALERT_LINGER) {
        overloadAlertLinger = false;
    }

    if (isOverload || overloadAlertLinger) {
        applyAlertPattern(AlertPattern::ALARM, OVERLOAD_BLINK_MS, now);
    } else if (overloadWarning) {
        applyAlertPattern(AlertPattern::WARNING, OVERLOAD_WARNING_BLINK_MS, now);
    } else {
        applyAlertPattern(AlertPattern::OFF, 0, now);
    }
}

}  // namespace

void indicatorsBegin() {
    pinMode(PIN_LED_BLUE, OUTPUT);
    pinMode(PIN_LED_GREEN, OUTPUT);
    pinMode(PIN_LED_RED, OUTPUT);
    pinMode(PIN_BUZZER, OUTPUT);

    digitalWrite(PIN_LED_BLUE, LOW);
    digitalWrite(PIN_LED_GREEN, LOW);
    writeAlert(false);

    unsigned long now = millis();
    lastBlueBlinkMs = now;
    lastAlertBlinkMs = now;
}

void indicatorsUpdate() {
    unsigned long now = millis();
    updateBlue(now);
    updateGreen();
    updateAlert(now);
}

void indicatorsSetWifiSearching(bool searching) {
    wifiSearching = searching;
}

void indicatorsSetCaptivePortalActive(bool active) {
    captivePortalActive = active;
}

void indicatorsResetAlertPattern() {
    lastAlertPattern = AlertPattern::OFF;
    alertBlinkState = false;
    lastAlertBlinkMs = millis();
    writeAlert(false);
}

