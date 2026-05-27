// ================================================================
// firebase.cpp — Smart Energy Monitor v3.1
// Implementasi komunikasi ke Firebase Realtime Database
// ================================================================

#include "firebase.h"
#include "config.h"
#include "state.h"
#include "storage.h"
#include "session.h"

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <time.h>

// ================================================================
// SEND LIVE DATA
// ================================================================
bool sendToFirebase(float v, float i, float p, float pf, float freq,
                    float kwh, float cost, bool dev, bool ovl,
                    bool relay, unsigned long ts) {
    if (!wifiConnected || WiFi.status() != WL_CONNECTED) return false;

    WiFiClientSecure c; c.setInsecure(); c.setTimeout(10000);
    HTTPClient h;
    h.begin(c, String(FIREBASE_HOST) + String(FIREBASE_PATH));
    h.addHeader("Content-Type", "application/json");

    unsigned long elapsedSec = getSessionElapsedSec(ts);
    const char* modeStr = modeOffline
        ? (relay ? "OFFLINE_MONITORING" : "OFFLINE_IDLE")
        : (relay ? "ONLINE_MONITORING"  : "ONLINE_IDLE");

    int pendingSync = fsCountOfflineHistory();

    String j = "{";
    j += "\"system\":{";
    j += "\"timestamp\":"      + String(ts);
    j += ",\"internet\":true";
    j += ",\"threshold\":"     + String(overloadThreshold, 0);
    j += ",\"tarif\":"         + String(tarif, 2);
    j += ",\"relay\":"         + String(relay ? "true" : "false");
    j += ",\"offline\":"       + String(modeOffline ? "true" : "false");
    j += ",\"sessionActive\":" + String(sessionActive ? "true" : "false");
    j += ",\"sessionStartTs\":" + String(sessionStartTs);
    j += ",\"elapsedSec\":"    + String(elapsedSec);
    j += ",\"mode\":\"";        j += modeStr; j += "\"";
    j += ",\"uid\":\"";         j += jsonEscape(currentUid); j += "\"";
    j += ",\"sessionId\":\"";   j += jsonEscape(currentSessionId); j += "\"";
    j += ",\"deviceName\":\"";  j += jsonEscape(sessionDeviceName); j += "\"";
    j += ",\"pendingSync\":";  j += String(pendingSync);
    j += "},";
    j += "\"connected\":"  + String(dev ? "true" : "false") + ",";
    j += "\"overload\":"   + String(ovl ? "true" : "false") + ",";
    j += "\"device\":{";
    j += "\"voltage\":"    + String(v, 1) + ",";
    j += "\"current\":"    + String(i, 2) + ",";
    j += "\"power\":"      + String(p, 1) + ",";
    j += "\"apparent\":"   + String(v * i, 1) + ",";
    j += "\"pf\":"         + String(pf, 2) + ",";
    j += "\"frequency\":"  + String(freq, 1) + ",";
    j += "\"energy\":"     + String(kwh, 4) + ",";
    j += "\"cost\":"       + String(cost, 0) + ",";
    j += "\"overload\":"   + String(ovl ? "true" : "false") + "}}";

    int statusCode = h.PUT(j);
    h.end();
    Serial.printf("[FB] %d P=%.1fW E=%.4fkWh Dev=%s Mode=%s Pending=%d\n",
                  statusCode, p, kwh, sessionDeviceName, modeStr, pendingSync);
    return (statusCode == 200 || statusCode == 204);
}

// ================================================================
// PUSH HISTORY
// ================================================================
bool pushHistoryToFirebase(const char* name, const char* duration,
                           float avgPower, float energyKwh, float cost,
                           unsigned long ts, bool recovered, bool wasOverload) {
    if (!wifiConnected || WiFi.status() != WL_CONNECTED) return false;

    char dateStr[20] = "—";
    if (ntpSynced && ts > 1000000) {
        struct tm ti;
        time_t t = (time_t)ts;
        localtime_r(&t, &ti);
        snprintf(dateStr, sizeof(dateStr), "%02d/%02d/%04d",
                 ti.tm_mday, ti.tm_mon + 1, ti.tm_year + 1900);
    }

    char costStr[32];
    if (cost >= 1000)
        snprintf(costStr, sizeof(costStr), "Rp %lu", (unsigned long)cost);
    else
        snprintf(costStr, sizeof(costStr), "Rp %.1f", cost);

    char displayName[64];
    if (recovered && wasOverload)
        snprintf(displayName, sizeof(displayName), "%s ⚡ Recovered ⚠", name);
    else if (recovered)
        snprintf(displayName, sizeof(displayName), "%s ⚡ Recovered", name);
    else
        strlcpy(displayName, name, sizeof(displayName));

    char path[128];
    snprintf(path, sizeof(path), "/shared_history/%lu.json", (unsigned long)(ts * 1000UL));

    WiFiClientSecure c; c.setInsecure(); c.setTimeout(10000);
    HTTPClient h;
    h.begin(c, String(FIREBASE_HOST) + String(path));
    h.addHeader("Content-Type", "application/json");

    char body[512];
    snprintf(body, sizeof(body),
        "{\"name\":\"%s\",\"duration\":\"%s\",\"power\":%.1f,"
        "\"energy\":%.4f,\"cost\":\"%s\",\"date\":\"%s\","
        "\"timestamp\":%lu,\"isOverload\":%s,\"recovered\":%s}",
        displayName, duration, avgPower,
        energyKwh, costStr, dateStr,
        (unsigned long)(ts * 1000UL),
        wasOverload ? "true" : "false",
        recovered   ? "true" : "false");

    int code = h.PUT(body);
    h.end();
    Serial.printf("[History] Push '%s' → %d\n", displayName, code);
    return (code == 200 || code == 204);
}

// ================================================================
// SYNC THRESHOLD FROM FIREBASE
// ================================================================
void syncThresholdFromFirebase() {
    if (!wifiConnected || WiFi.status() != WL_CONNECTED) return;
    WiFiClientSecure c; c.setInsecure(); c.setTimeout(8000);
    HTTPClient h;
    if (!h.begin(c, String(FIREBASE_HOST) + "/config/threshold.json")) return;
    if (h.GET() == 200) {
        String pl = h.getString(); pl.trim();
        if (pl != "null" && pl.length() > 0) {
            float v = pl.toFloat();
            if (v > 0 && v != overloadThreshold) {
                overloadThreshold = v;
                savePrefs();
                Serial.printf("[Threshold] Updated: %.0fW\n", v);
            }
        }
    }
    h.end();
}

// ================================================================
// CLEAR RELAY COMMAND
// ================================================================
void clearFirebaseCommand() {
    if (!wifiConnected || WiFi.status() != WL_CONNECTED) return;
    WiFiClientSecure c; c.setInsecure(); c.setTimeout(5000);
    HTTPClient h;
    if (h.begin(c, String(FIREBASE_HOST) + "/command/relay.json")) {
        h.addHeader("Content-Type", "application/json");
        h.PUT("null");
        h.end();
    }
}

// ================================================================
// POLL RELAY COMMAND (ONLINE MODE only)
// ================================================================
void pollCommandFromFirebase() {
    if (!wifiConnected || WiFi.status() != WL_CONNECTED) return;

    WiFiClientSecure c; c.setInsecure(); c.setTimeout(5000);
    HTTPClient h;
    if (!h.begin(c, String(FIREBASE_HOST) + "/command/relay.json")) return;

    int code = h.GET();
    if (code != 200) { h.end(); return; }

    String pl = h.getString(); pl.trim(); h.end();

    bool hasCommand = false, cmdStart = false, cmdStop = false;
    char cmdId[48]="", cmdUid[64]="", cmdSessionId[48]="", cmdDeviceName[32]="";
    float cmdTarif = 0, cmdThreshold = 0;

    if (pl == "true" || pl == "false") {
        hasCommand = true;
        cmdStart = (pl == "true");
        cmdStop  = (pl == "false");
    } else if (pl != "null" && pl.length() > 0) {
        StaticJsonDocument<512> doc;
        if (deserializeJson(doc, pl)) return;
        const char* type = doc["type"] | "";
        hasCommand = true;
        cmdStart = (strcmp(type, "START") == 0) || (doc["on"] | false);
        cmdStop  = (strcmp(type, "STOP")  == 0) || (doc.containsKey("on") && !(doc["on"] | false));
        strlcpy(cmdId,         doc["id"]         | "", sizeof(cmdId));
        strlcpy(cmdUid,        doc["uid"]        | "", sizeof(cmdUid));
        strlcpy(cmdSessionId,  doc["sessionId"]  | "", sizeof(cmdSessionId));
        strlcpy(cmdDeviceName, doc["deviceName"] | "", sizeof(cmdDeviceName));
        cmdTarif     = doc["tariff"]    | 0.0f;
        cmdThreshold = doc["threshold"] | 0.0f;
    }

    if (!hasCommand || (!cmdStart && !cmdStop)) return;

    // Duplicate guard
    if (strlen(cmdId) > 0 && strcmp(cmdId, lastCommandId) == 0) {
        clearFirebaseCommand();
        return;
    }
    if (strlen(cmdId) > 0) strlcpy(lastCommandId, cmdId, sizeof(lastCommandId));

    if (cmdStart && !relayOn) {
        sessionEnergyWh = 0; sessionKwh = 0; sessionCost = 0;
        hadDataOnce = false; disconnectCount = 0;
        isOverload = false; overloadAlertLinger = false;

        if (strlen(cmdUid)       > 0) strlcpy(currentUid,       cmdUid,       sizeof(currentUid));
        if (strlen(cmdSessionId) > 0) strlcpy(currentSessionId, cmdSessionId, sizeof(currentSessionId));
        saveSessionId();

        if (strlen(cmdDeviceName) > 0) strlcpy(sessionDeviceName, cmdDeviceName, sizeof(sessionDeviceName));
        else if (strlen(sessionDeviceName) == 0) strlcpy(sessionDeviceName, "Device", sizeof(sessionDeviceName));

        bool prefsChanged = false;
        if (cmdTarif     > 0 && cmdTarif     != tarif)             { tarif             = cmdTarif;     prefsChanged = true; }
        if (cmdThreshold > 0 && cmdThreshold != overloadThreshold) { overloadThreshold = cmdThreshold; prefsChanged = true; }
        if (prefsChanged) savePrefs();

        unsigned long nowTs = ntpSynced ? (unsigned long)time(nullptr) : millis() / 1000;
        sessionStartTs = nowTs;
        setRelay(true, "web command START");
        clearFirebaseCommand();
        Serial.println("[Command] ✓ Relay ON (web)");

    } else if (cmdStop && relayOn) {
        unsigned long nowTs = ntpSynced ? (unsigned long)time(nullptr) : millis() / 1000;
        if (sessionActive && sessionKwh > 0) {
            String dur = buildDuration(sessionStartTs, nowTs);
            pushHistoryToFirebase(sessionDeviceName, dur.c_str(),
                                  lastP, sessionKwh, sessionCost,
                                  nowTs, false, false);
        }
        setRelay(false, "web command STOP");
        sessionEnergyWh = 0; sessionKwh = 0; sessionCost = 0;
        hadDataOnce = false; prevDevConn = false; disconnectCount = 0;
        sessionDeviceName[0] = '\0';
        currentSessionId[0] = '\0';
        saveSessionId();
        clearFirebaseCommand();
        Serial.println("[Command] ✓ Relay OFF (web)");
    }
}
