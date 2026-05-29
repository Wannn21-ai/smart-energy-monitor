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
    syncSessionDataFromLegacy(ts);

    WiFiClientSecure c; c.setInsecure(); c.setTimeout(10000);
    HTTPClient h;
    h.begin(c, String(FIREBASE_HOST) + String(FIREBASE_PATH));
    h.addHeader("Content-Type", "application/json");

    unsigned long elapsedSec = sessionData.duration;
    const char* systemModeStr = systemModeToString(systemMode);
    const char* sessionStateStr = sessionStateToString(sessionState);
    const char* modeStr = modeOffline
        ? (relay ? "OFFLINE_MONITORING" : "OFFLINE_IDLE")
        : (relay ? "ONLINE_MONITORING"  : "ONLINE_IDLE");

    int pendingSync = fsCountOfflineHistory();

    String j = "{";
    j += "\"system\":{";
    j += "\"timestamp\":"      + String(ts);
    j += ",\"internet\":true";
    j += ",\"threshold\":"     + String(appConfig.overloadThreshold, 0);
    j += ",\"tarif\":"         + String(appConfig.electricityCostPerKwh, 2);
    j += ",\"relay\":"         + String(relay ? "true" : "false");
    j += ",\"offline\":"       + String(modeOffline ? "true" : "false");
    j += ",\"sessionActive\":" + String(sessionActive ? "true" : "false");
    j += ",\"sessionStartTs\":" + String(sessionStartTs);
    j += ",\"elapsedSec\":"    + String(elapsedSec);
    j += ",\"mode\":\"";        j += modeStr; j += "\"";
    j += ",\"systemMode\":\"";  j += systemModeStr; j += "\"";
    j += ",\"sessionState\":\""; j += sessionStateStr; j += "\"";
    j += ",\"endReason\":\"";    j += sessionEndReasonToString(sessionData.endReason); j += "\"";
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
    Serial.printf("[FB] %d P=%.1fW E=%.4fkWh Dev=%s Mode=%s Session=%s Pending=%d\n",
                  statusCode, p, kwh, sessionDeviceName,
                  systemModeStr, sessionStateStr, pendingSync);
    return (statusCode == 200 || statusCode == 204);
}

// ================================================================
// PUSH HISTORY
// ================================================================
bool pushHistoryToFirebase(const char* name, const char* duration,
                           float avgPower, float energyKwh, float cost,
                           unsigned long ts, bool recovered, bool wasOverload,
                           const char* endReason) {
    if (!wifiConnected || WiFi.status() != WL_CONNECTED) return false;
    if (strlen(currentUid) == 0) {
        Serial.println("[History] Push ditahan: uid kosong, history tetap pending lokal");
        return false;
    }

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

    if (wasOverload) {
        snprintf(displayName, sizeof(displayName), "%s [OVERLOAD]%s",
                 name, recovered ? " Recovered" : "");
    }

    unsigned long long historyKey = (unsigned long long)ts * 1000ULL;
    char historyKeyStr[24];
    snprintf(historyKeyStr, sizeof(historyKeyStr), "%llu", historyKey);
    char path[192];
    snprintf(path, sizeof(path), "/users/%s/history/%s.json", currentUid, historyKeyStr);

    WiFiClientSecure c; c.setInsecure(); c.setTimeout(10000);
    HTTPClient h;
    h.begin(c, String(FIREBASE_HOST) + String(path));
    h.addHeader("Content-Type", "application/json");

    String safeName = jsonEscape(displayName);
    String safeDuration = jsonEscape(duration);
    String safeSessionId = jsonEscape(currentSessionId);
    String safeEndReason = jsonEscape(endReason && strlen(endReason) > 0 ? endReason : "NORMAL_STOP");

    char body[640];
    snprintf(body, sizeof(body),
        "{\"name\":\"%s\",\"duration\":\"%s\",\"power\":%.1f,"
        "\"energy\":%.4f,\"cost\":\"%s\",\"date\":\"%s\","
        "\"timestamp\":%s,\"isOverload\":%s,\"recovered\":%s,"
        "\"sessionId\":\"%s\",\"endReason\":\"%s\"}",
        safeName.c_str(), safeDuration.c_str(), avgPower,
        energyKwh, costStr, dateStr,
        historyKeyStr,
        wasOverload ? "true" : "false",
        recovered   ? "true" : "false",
        safeSessionId.c_str(),
        safeEndReason.c_str());

    int code = h.PUT(body);
    h.end();
    Serial.printf("[History] Firebase target /users/%s/history/%s endReason=%s\n",
                  currentUid, historyKeyStr, safeEndReason.c_str());
    Serial.printf("[History] Push '%s' → %d\n", displayName, code);
    return (code == 200 || code == 204);
}

// ================================================================
// SYNC CONFIG FROM FIREBASE
// ================================================================
void syncConfigFromFirebase() {
    if (!wifiConnected || WiFi.status() != WL_CONNECTED) return;

    WiFiClientSecure c; c.setInsecure(); c.setTimeout(8000);
    HTTPClient h;
    if (h.begin(c, String(FIREBASE_HOST) + "/config/app.json")) {
        if (h.GET() == 200) {
            String pl = h.getString(); pl.trim();
            if (pl != "null" && pl.length() > 0) {
                StaticJsonDocument<256> doc;
                if (!deserializeJson(doc, pl) && doc.is<JsonObject>()) {
                    AppConfig next = appConfig;
                    float threshold = doc["overloadThreshold"] | 0.0f;
                    if (threshold <= 0) threshold = doc["threshold"] | 0.0f;
                    float cost = doc["electricityCostPerKwh"] | 0.0f;
                    if (cost <= 0) cost = doc["tariff"] | 0.0f;
                    if (cost <= 0) cost = doc["tarif"] | 0.0f;
                    if (threshold > 0) next.overloadThreshold = threshold;
                    if (cost > 0) next.electricityCostPerKwh = cost;
                    if (setAppConfig(next, "firebase config")) {
                        Serial.printf("[Config] Updated: %.0fW %.2f/kWh\n",
                                      appConfig.overloadThreshold,
                                      appConfig.electricityCostPerKwh);
                    }
                    h.end();
                    return;
                }
            }
        }
        h.end();
    }

    WiFiClientSecure c2; c2.setInsecure(); c2.setTimeout(8000);
    HTTPClient h2;
    if (!h2.begin(c2, String(FIREBASE_HOST) + "/config/threshold.json")) return;
    if (h2.GET() == 200) {
        String pl = h2.getString(); pl.trim();
        if (pl != "null" && pl.length() > 0) {
            float v = pl.toFloat();
            if (v > 0 && v != appConfig.overloadThreshold) {
                setOverloadThreshold(v, "firebase config");
                Serial.printf("[Threshold] Updated: %.0fW\n", v);
            }
        }
    }
    h2.end();
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
        isOverload = false; overloadWarning = false; overloadAlertLinger = false;

        if (strlen(cmdUid)       > 0) strlcpy(currentUid,       cmdUid,       sizeof(currentUid));
        if (strlen(cmdSessionId) > 0) strlcpy(currentSessionId, cmdSessionId, sizeof(currentSessionId));
        saveSessionId();

        if (strlen(cmdDeviceName) > 0) strlcpy(sessionDeviceName, cmdDeviceName, sizeof(sessionDeviceName));
        else if (strlen(sessionDeviceName) == 0) strlcpy(sessionDeviceName, "Device", sizeof(sessionDeviceName));

        bool prefsChanged = false;
        AppConfig nextConfig = appConfig;
        if (cmdTarif     > 0 && cmdTarif     != appConfig.electricityCostPerKwh) { nextConfig.electricityCostPerKwh = cmdTarif; prefsChanged = true; }
        if (cmdThreshold > 0 && cmdThreshold != appConfig.overloadThreshold)     { nextConfig.overloadThreshold = cmdThreshold; prefsChanged = true; }
        if (prefsChanged) setAppConfig(nextConfig, "web command");

        sessionStartTs = 0;
        beginLoadCheck("web command START");
        clearFirebaseCommand();
        Serial.println("[Command] ✓ Relay ON (web)");

    } else if (cmdStop && relayOn) {
        if (sessionActive && hadDataOnce) {
            finalizeSession(SESSION_END_USER_STOP, "web command STOP");
        } else {
            setRelay(false, "web command STOP");
            sessionEnergyWh = 0; sessionKwh = 0; sessionCost = 0;
            hadDataOnce = false; prevDevConn = false; disconnectCount = 0;
            sessionDeviceName[0] = '\0';
            currentSessionId[0] = '\0';
            saveSessionId();
        }
        clearFirebaseCommand();
        Serial.println("[Command] ✓ Relay OFF (web)");
    }
}
