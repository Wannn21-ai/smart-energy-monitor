// ================================================================
// storage.cpp — Smart Energy Monitor v3.1
// Implementasi LittleFS (session + history) dan Preferences
// ================================================================

#include "storage.h"
#include "config.h"
#include "state.h"
#include "firebase.h"   // fsSyncOfflineHistoryToFirebase butuh pushHistoryToFirebase
#include "session.h"    // fsWriteSession butuh sessionEnergyWh, sessionKwh, sessionCost

#include <Preferences.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <ArduinoJson.h>
#include <time.h>

static Preferences prefs;

// ================================================================
// PREFERENCES
// ================================================================
void loadPrefs() {
    prefs.begin("sem", true);
    overloadThreshold = prefs.getFloat("threshold", THRESHOLD_DEFAULT);
    tarif             = prefs.getFloat("tarif",     TARIF_DEFAULT);
    prefs.getString("uid",       currentUid,       sizeof(currentUid));
    prefs.getString("sessionId", currentSessionId, sizeof(currentSessionId));
    prefs.end();
}

void savePrefs() {
    prefs.begin("sem", false);
    prefs.putFloat("threshold", overloadThreshold);
    prefs.putFloat("tarif",     tarif);
    prefs.end();
}

void saveSessionId() {
    prefs.begin("sem", false);
    prefs.putString("uid",       currentUid);
    prefs.putString("sessionId", currentSessionId);
    prefs.end();
    Serial.printf("[Prefs] Saved uid=%s sessionId=%s\n", currentUid, currentSessionId);
}

// ================================================================
// LittleFS INIT
// ================================================================
bool fsInit() {
    if (!LittleFS.begin(true)) {
        Serial.println("[FS] Mount gagal — format ulang");
        if (!LittleFS.begin(true)) {
            Serial.println("[FS] Format gagal");
            return false;
        }
    }
    Serial.println("[FS] LittleFS OK");
    return true;
}

// ================================================================
// SESSION FILE — write checkpoint
// ================================================================
bool fsWriteSession() {
    unsigned long nowTs = ntpSynced ? (unsigned long)time(nullptr) : millis() / 1000;
    syncSessionDataFromLegacy(nowTs);

    if (!sessionData.sessionActive || strlen(sessionData.deviceName) == 0) return false;

    StaticJsonDocument<384> doc;
    doc["name"]      = sessionData.deviceName;
    doc["start"]     = sessionStartTs;
    doc["elapsed"]   = sessionData.duration;
    doc["energy_wh"] = sessionData.energy * 1000.0f;
    doc["kwh"]       = sessionData.energy;
    doc["cost"]      = sessionData.cost;
    doc["relay"]     = relayOn;
    doc["uid"]       = currentUid;
    doc["sessionId"] = currentSessionId;

    File f = LittleFS.open(FS_SESSION_PATH, "w");
    if (!f) {
        Serial.println("[FS] ✗ Gagal buka session file");
        return false;
    }
    size_t n = serializeJson(doc, f);
    f.close();
    if (n == 0) {
        Serial.println("[FS] ✗ Gagal serialize session");
        return false;
    }
    Serial.printf("[FS] ✓ Checkpoint: %s %.4f kWh\n", sessionData.deviceName, sessionData.energy);
    return true;
}

// ================================================================
// SESSION FILE — clear
// ================================================================
void fsClearSession() {
    if (LittleFS.exists(FS_SESSION_PATH)) {
        LittleFS.remove(FS_SESSION_PATH);
        Serial.println("[FS] session_active.json dihapus");
    }
}

// ================================================================
// SESSION FILE — read
// ================================================================
bool fsReadSession(float &outEnergyWh, float &outKwh, float &outCost,
                   char *outName, unsigned long &outStartTs,
                   unsigned long &outElapsedSec) {
    if (!LittleFS.exists(FS_SESSION_PATH)) return false;

    File f = LittleFS.open(FS_SESSION_PATH, "r");
    if (!f) return false;

    StaticJsonDocument<256> doc;
    DeserializationError err = deserializeJson(doc, f);
    f.close();

    if (err) {
        Serial.printf("[FS] JSON error: %s\n", err.c_str());
        LittleFS.remove(FS_SESSION_PATH);
        return false;
    }

    outEnergyWh = doc["energy_wh"] | 0.0f;
    outKwh      = doc["kwh"]       | 0.0f;
    outCost     = doc["cost"]      | 0.0f;
    outStartTs  = doc["start"]     | (unsigned long)0;
    outElapsedSec = doc["elapsed"] | (unsigned long)0;
    strlcpy(outName, doc["name"] | "Recovered", 32);

    const char* savedUid  = doc["uid"]       | "";
    const char* savedSess = doc["sessionId"] | "";
    if (strlen(savedUid)  > 0) strlcpy(currentUid,       savedUid,  sizeof(currentUid));
    if (strlen(savedSess) > 0) strlcpy(currentSessionId, savedSess, sizeof(currentSessionId));

    if (outKwh <= 0 && outEnergyWh <= 0) {
        LittleFS.remove(FS_SESSION_PATH);
        return false;
    }
    return true;
}

// ================================================================
// OFFLINE HISTORY — append entry
// ================================================================
void fsAppendOfflineHistory(const char* name, unsigned long startTs,
                            unsigned long endTs, float energyKwh,
                            float cost, float avgPower, bool wasOverload) {
    DynamicJsonDocument doc(4096);
    JsonArray arr;

    if (LittleFS.exists(FS_HISTORY_PATH)) {
        File f = LittleFS.open(FS_HISTORY_PATH, "r");
        if (f) {
            DeserializationError err = deserializeJson(doc, f);
            f.close();
            if (!err && doc.is<JsonArray>()) {
                arr = doc.as<JsonArray>();
            } else {
                doc.clear();
                arr = doc.to<JsonArray>();
            }
        }
    } else {
        arr = doc.to<JsonArray>();
    }

    JsonObject entry = arr.createNestedObject();
    entry["name"]     = name;
    entry["start_ts"] = startTs;
    entry["end_ts"]   = endTs;
    entry["kwh"]      = energyKwh;
    entry["cost"]     = cost;
    entry["power"]    = avgPower;
    entry["overload"] = wasOverload;
    entry["uid"]      = currentUid;
    entry["sessionId"] = currentSessionId;

    File f = LittleFS.open(FS_HISTORY_PATH, "w");
    if (!f) { Serial.println("[FS] Gagal tulis offline history"); return; }
    serializeJson(doc, f);
    f.close();
    Serial.printf("[FS] Offline history: '%s' %.4f kWh disimpan\n", name, energyKwh);
}

// ================================================================
// OFFLINE HISTORY — sync to Firebase
// ================================================================
bool fsSyncOfflineHistoryToFirebase() {
    if (!wifiConnected || WiFi.status() != WL_CONNECTED) return false;
    if (!LittleFS.exists(FS_HISTORY_PATH)) return true;

    File f = LittleFS.open(FS_HISTORY_PATH, "r");
    if (!f) return false;

    DynamicJsonDocument doc(4096);
    DeserializationError err = deserializeJson(doc, f);
    f.close();

    if (err || !doc.is<JsonArray>()) {
        Serial.println("[FS] history_offline.json corrupt — hapus");
        LittleFS.remove(FS_HISTORY_PATH);
        return true;
    }

    JsonArray arr = doc.as<JsonArray>();
    if (arr.size() == 0) {
        LittleFS.remove(FS_HISTORY_PATH);
        return true;
    }

    Serial.printf("[Sync] Mulai sync %d sesi offline ke Firebase\n", (int)arr.size());

    int pushed = 0;
    for (JsonObject entry : arr) {
        const char*   name    = entry["name"]     | "Offline Device";
        unsigned long startTs = entry["start_ts"] | (unsigned long)0;
        unsigned long endTs   = entry["end_ts"]   | (unsigned long)0;
        float         kwh     = entry["kwh"]      | 0.0f;
        float         cost    = entry["cost"]     | 0.0f;
        float         avgPwr  = entry["power"]    | 0.0f;
        bool          wasOvl  = entry["overload"] | false;
        const char*   uid     = entry["uid"]      | "";
        const char*   sessId  = entry["sessionId"] | "";

        if (strlen(uid) > 0) strlcpy(currentUid, uid, sizeof(currentUid));
        if (strlen(sessId) > 0) strlcpy(currentSessionId, sessId, sizeof(currentSessionId));

        String dur = buildDuration(startTs, endTs);
        bool ok = pushHistoryToFirebase(name, dur.c_str(), avgPwr, kwh, cost,
                                        endTs > 0 ? endTs : startTs,
                                        true, wasOvl);
        if (!ok) {
            Serial.printf("[Sync] Push '%s' gagal — berhenti\n", name);
            break;
        }
        pushed++;
        delay(300);
    }

    if (pushed == (int)arr.size()) {
        LittleFS.remove(FS_HISTORY_PATH);
        Serial.printf("[Sync] ✓ Semua %d sesi berhasil di-sync\n", pushed);
        return true;
    }

    if (pushed > 0) {
        DynamicJsonDocument newDoc(4096);
        JsonArray newArr = newDoc.to<JsonArray>();
        int idx = 0;
        for (JsonObject entry : arr) {
            if (idx >= pushed) newArr.add(entry);
            idx++;
        }
        File fw = LittleFS.open(FS_HISTORY_PATH, "w");
        if (fw) { serializeJson(newDoc, fw); fw.close(); }
        Serial.printf("[Sync] %d/%d berhasil, sisanya disimpan\n", pushed, (int)arr.size());
    }
    return false;
}

// ================================================================
// OFFLINE HISTORY — count pending entries
// ================================================================
int fsCountOfflineHistory() {
    if (!LittleFS.exists(FS_HISTORY_PATH)) return 0;
    File f = LittleFS.open(FS_HISTORY_PATH, "r");
    if (!f) return 0;
    DynamicJsonDocument doc(4096);
    if (deserializeJson(doc, f) || !doc.is<JsonArray>()) { f.close(); return 0; }
    f.close();
    return (int)doc.as<JsonArray>().size();
}
