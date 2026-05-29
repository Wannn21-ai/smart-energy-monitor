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

static const char* FS_SESSION_TMP_PATH = "/session_active.tmp";
static const char* FS_SESSION_BAK_PATH = "/session_active.bak";

static SessionState sessionStateFromString(const char* value) {
    if (!value) return SessionState::IDLE;
    if (strcmp(value, "WAITING_LOAD") == 0) return SessionState::WAITING_LOAD;
    if (strcmp(value, "MONITORING") == 0) return SessionState::MONITORING;
    if (strcmp(value, "OVERLOAD") == 0) return SessionState::OVERLOAD;
    if (strcmp(value, "FINISHED") == 0) return SessionState::FINISHED;
    return SessionState::IDLE;
}

static SystemMode systemModeFromString(const char* value) {
    if (!value) return SystemMode::ONLINE;
    if (strcmp(value, "OFFLINE") == 0) return SystemMode::OFFLINE;
    if (strcmp(value, "TRANSITION") == 0) return SystemMode::TRANSITION;
    return SystemMode::ONLINE;
}

static bool validateJsonFile(const char* path) {
    File f = LittleFS.open(path, "r");
    if (!f) return false;
    StaticJsonDocument<1024> doc;
    DeserializationError err = deserializeJson(doc, f);
    f.close();
    return !err && (doc["committed"] | false);
}

template <size_t N>
static bool commitSessionCheckpoint(StaticJsonDocument<N> &doc) {
    if (LittleFS.exists(FS_SESSION_TMP_PATH)) LittleFS.remove(FS_SESSION_TMP_PATH);

    File f = LittleFS.open(FS_SESSION_TMP_PATH, "w");
    if (!f) {
        Serial.println("[FS] ✗ Gagal buka temp checkpoint");
        return false;
    }

    size_t n = serializeJson(doc, f);
    f.flush();
    f.close();
    if (n == 0 || !validateJsonFile(FS_SESSION_TMP_PATH)) {
        LittleFS.remove(FS_SESSION_TMP_PATH);
        Serial.println("[FS] ✗ Temp checkpoint invalid");
        return false;
    }

    if (LittleFS.exists(FS_SESSION_BAK_PATH)) LittleFS.remove(FS_SESSION_BAK_PATH);
    if (LittleFS.exists(FS_SESSION_PATH) &&
        !LittleFS.rename(FS_SESSION_PATH, FS_SESSION_BAK_PATH)) {
        LittleFS.remove(FS_SESSION_TMP_PATH);
        Serial.println("[FS] ✗ Gagal backup checkpoint lama");
        return false;
    }

    if (!LittleFS.rename(FS_SESSION_TMP_PATH, FS_SESSION_PATH)) {
        if (LittleFS.exists(FS_SESSION_BAK_PATH)) {
            LittleFS.rename(FS_SESSION_BAK_PATH, FS_SESSION_PATH);
        }
        Serial.println("[FS] ✗ Gagal commit checkpoint");
        return false;
    }

    return true;
}

// ================================================================
// PREFERENCES
// ================================================================
static bool validOverloadThreshold(float threshold) {
    return threshold > 0.0f && threshold <= 10000.0f;
}

static bool validElectricityCost(float costPerKwh) {
    return costPerKwh > 0.0f;
}

void loadPrefs() {
    prefs.begin("sem", true);
    appConfig.overloadThreshold     = prefs.getFloat("threshold", THRESHOLD_DEFAULT);
    appConfig.electricityCostPerKwh = prefs.getFloat("tarif",     TARIF_DEFAULT);
    prefs.getString("uid",       currentUid,       sizeof(currentUid));
    prefs.getString("sessionId", currentSessionId, sizeof(currentSessionId));
    prefs.end();

    if (!validOverloadThreshold(appConfig.overloadThreshold)) {
        appConfig.overloadThreshold = THRESHOLD_DEFAULT;
    }
    if (!validElectricityCost(appConfig.electricityCostPerKwh)) {
        appConfig.electricityCostPerKwh = TARIF_DEFAULT;
    }
}

void savePrefs() {
    prefs.begin("sem", false);
    prefs.putFloat("threshold", appConfig.overloadThreshold);
    prefs.putFloat("tarif",     appConfig.electricityCostPerKwh);
    prefs.end();
}

void saveSessionId() {
    prefs.begin("sem", false);
    prefs.putString("uid",       currentUid);
    prefs.putString("sessionId", currentSessionId);
    prefs.end();
    Serial.printf("[Prefs] Saved uid=%s sessionId=%s\n", currentUid, currentSessionId);
}

bool setOverloadThreshold(float threshold, const char* source) {
    AppConfig next = appConfig;
    next.overloadThreshold = threshold;
    return setAppConfig(next, source);
}

bool setElectricityCostPerKwh(float costPerKwh, const char* source) {
    AppConfig next = appConfig;
    next.electricityCostPerKwh = costPerKwh;
    return setAppConfig(next, source);
}

bool setAppConfig(const AppConfig& next, const char* source) {
    if (!validOverloadThreshold(next.overloadThreshold)) return false;
    if (!validElectricityCost(next.electricityCostPerKwh)) return false;

    bool changed = next.overloadThreshold != appConfig.overloadThreshold ||
                   next.electricityCostPerKwh != appConfig.electricityCostPerKwh;
    if (!changed) return true;

    appConfig = next;
    savePrefs();
    Serial.printf("[Prefs] Config threshold=%.0fW cost=%.2f/kWh (%s)\n",
                  appConfig.overloadThreshold,
                  appConfig.electricityCostPerKwh,
                  source && strlen(source) > 0 ? source : "update");
    return true;
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
    if (LittleFS.exists(FS_SESSION_TMP_PATH)) {
        LittleFS.remove(FS_SESSION_TMP_PATH);
        Serial.println("[FS] Temp checkpoint dibersihkan");
    }
    if (!LittleFS.exists(FS_SESSION_PATH) && LittleFS.exists(FS_SESSION_BAK_PATH)) {
        LittleFS.rename(FS_SESSION_BAK_PATH, FS_SESSION_PATH);
        Serial.println("[FS] Checkpoint backup dipulihkan");
    }
    return true;
}

// ================================================================
// SESSION FILE — write checkpoint
// ================================================================
bool fsWriteSession() {
    unsigned long nowTs = ntpSynced ? (unsigned long)time(nullptr) : millis() / 1000;
    syncSessionDataFromLegacy(nowTs);

    if (!sessionData.sessionActive || strlen(sessionData.deviceName) == 0) return false;

    StaticJsonDocument<1024> doc;
    doc["schema"]        = 2;
    doc["committed"]     = true;
    doc["saved_ms"]      = millis();
    doc["name"]          = sessionData.deviceName;
    doc["start"]         = sessionStartTs;
    doc["elapsed"]       = sessionData.duration;
    doc["energy_wh"]     = sessionData.energy * 1000.0f;
    doc["kwh"]           = sessionData.energy;
    doc["cost"]          = sessionData.cost;
    doc["sessionActive"] = sessionData.sessionActive;
    doc["relay"]         = relayOn;
    doc["overload"]      = sessionData.overload;
    doc["offline"]       = (sessionData.mode == SESSION_MODE_OFFLINE);
    doc["mode"]          = (sessionData.mode == SESSION_MODE_OFFLINE) ? "OFFLINE" : "ONLINE";
    doc["systemMode"]    = systemModeToString(systemMode);
    doc["sessionState"]  = sessionStateToString(sessionState);
    doc["endReason"]     = sessionEndReasonToString(sessionData.endReason);
    doc["uid"]           = currentUid;
    doc["sessionId"]     = currentSessionId;

    if (doc.overflowed()) {
        Serial.println("[FS] Checkpoint JSON overflow");
        return false;
    }

    if (!commitSessionCheckpoint(doc)) return false;

    Serial.printf("[FS] Checkpoint: %s %.4f kWh %lus %s/%s\n",
                  sessionData.deviceName, sessionData.energy,
                  sessionData.duration,
                  systemModeToString(systemMode),
                  sessionStateToString(sessionState));
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
    if (LittleFS.exists(FS_SESSION_TMP_PATH)) LittleFS.remove(FS_SESSION_TMP_PATH);
    if (LittleFS.exists(FS_SESSION_BAK_PATH)) LittleFS.remove(FS_SESSION_BAK_PATH);
}

// ================================================================
// SESSION FILE — read
// ================================================================
static bool readSessionFromPath(const char* path, PersistedSession &out) {
    if (!LittleFS.exists(path)) return false;

    File f = LittleFS.open(path, "r");
    if (!f) return false;

    StaticJsonDocument<1024> doc;
    DeserializationError err = deserializeJson(doc, f);
    f.close();

    if (err) {
        Serial.printf("[FS] JSON error %s: %s\n", path, err.c_str());
        return false;
    }

    bool committed = doc["committed"] | true;
    if (!committed) return false;

    out.energyWh      = doc["energy_wh"]     | 0.0f;
    out.kwh           = doc["kwh"]           | 0.0f;
    out.cost          = doc["cost"]          | 0.0f;
    out.startTs       = doc["start"]         | (unsigned long)0;
    out.elapsedSec    = doc["elapsed"]       | (unsigned long)0;
    out.sessionActive = doc["sessionActive"] | true;
    out.relay         = doc["relay"]         | true;
    out.overload      = doc["overload"]      | false;
    out.offlineMode   = doc["offline"]       | false;
    out.sessionState  = sessionStateFromString(doc["sessionState"] | "MONITORING");
    out.systemMode    = systemModeFromString(doc["systemMode"] | (out.offlineMode ? "OFFLINE" : "ONLINE"));
    strlcpy(out.name,      doc["name"]      | "Recovered", sizeof(out.name));
    strlcpy(out.uid,       doc["uid"]       | "",          sizeof(out.uid));
    strlcpy(out.sessionId, doc["sessionId"] | "",          sizeof(out.sessionId));

    return out.sessionActive && strlen(out.name) > 0;
}

bool fsReadSession(PersistedSession &out) {
    memset(&out, 0, sizeof(out));
    out.sessionState = SessionState::IDLE;
    out.systemMode = SystemMode::ONLINE;

    if (readSessionFromPath(FS_SESSION_PATH, out)) {
        if (strlen(out.uid) > 0) strlcpy(currentUid, out.uid, sizeof(currentUid));
        if (strlen(out.sessionId) > 0) strlcpy(currentSessionId, out.sessionId, sizeof(currentSessionId));
        return true;
    }

    if (LittleFS.exists(FS_SESSION_PATH)) {
        LittleFS.remove(FS_SESSION_PATH);
    }

    if (readSessionFromPath(FS_SESSION_BAK_PATH, out)) {
        Serial.println("[FS] Checkpoint utama invalid, pakai backup");
        if (strlen(out.uid) > 0) strlcpy(currentUid, out.uid, sizeof(currentUid));
        if (strlen(out.sessionId) > 0) strlcpy(currentSessionId, out.sessionId, sizeof(currentSessionId));
        return true;
    }

    if (LittleFS.exists(FS_SESSION_BAK_PATH)) {
        LittleFS.remove(FS_SESSION_BAK_PATH);
    }
    return false;
}

// ================================================================
// OFFLINE HISTORY — append entry
// ================================================================
static size_t fsHistoryFileSize() {
    if (!LittleFS.exists(FS_HISTORY_PATH)) return 0;
    File f = LittleFS.open(FS_HISTORY_PATH, "r");
    if (!f) return 0;
    size_t size = f.size();
    f.close();
    return size;
}

static size_t fsHistoryJsonCapacity() {
    size_t size = fsHistoryFileSize();
    size_t capacity = size * 2 + 1024;
    return capacity < 4096 ? 4096 : capacity;
}

static int countPendingHistory(JsonArray arr) {
    int pending = 0;
    for (JsonObject entry : arr) {
        if (entry["pendingSync"] | true) pending++;
    }
    return pending;
}

bool fsAppendOfflineHistory(const char* name, unsigned long startTs,
                            unsigned long endTs, float energyKwh,
                            float cost, float avgPower, bool wasOverload,
                            const char* endReason, bool recovered) {
    DynamicJsonDocument doc(fsHistoryJsonCapacity());
    JsonArray arr = doc.to<JsonArray>();

    if (LittleFS.exists(FS_HISTORY_PATH)) {
        File f = LittleFS.open(FS_HISTORY_PATH, "r");
        if (f) {
            DeserializationError err = deserializeJson(doc, f);
            f.close();
            if (!err && doc.is<JsonArray>()) {
                arr = doc.as<JsonArray>();
            } else {
                Serial.printf("[FS] %s invalid, rebuilding history queue\n", FS_HISTORY_PATH);
                doc.clear();
                arr = doc.to<JsonArray>();
            }
        }
    }

    const char* reasonText = endReason && strlen(endReason) > 0 ? endReason : "NORMAL_STOP";
    for (JsonObject existing : arr) {
        const char* existingSessionId = existing["sessionId"] | "";
        unsigned long existingStart = existing["start_ts"] | (unsigned long)0;
        const char* existingReason = existing["endReason"] | "";
        bool sameSessionId = strlen(currentSessionId) > 0 &&
                             strcmp(existingSessionId, currentSessionId) == 0;
        bool sameFallback = strlen(currentSessionId) == 0 &&
                            existingStart == startTs &&
                            strcmp(existingReason, reasonText) == 0;
        if (sameSessionId || sameFallback) {
            Serial.printf("[FS] History duplicate skipped: '%s' reason=%s pending=%d size=%u\n",
                          name, reasonText, countPendingHistory(arr),
                          (unsigned)fsHistoryFileSize());
            return true;
        }
    }

    JsonObject entry = arr.createNestedObject();
    entry["name"]     = name;
    entry["start_ts"] = startTs;
    entry["end_ts"]   = endTs;
    entry["duration"] = buildDuration(startTs, endTs);
    entry["kwh"]      = energyKwh;
    entry["energy"]   = energyKwh;
    entry["cost"]     = cost;
    entry["power"]    = avgPower;
    entry["overload"] = wasOverload;
    entry["recovered"] = recovered;
    entry["pendingSync"] = true;
    entry["endReason"] = reasonText;
    entry["uid"]      = currentUid;
    entry["sessionId"] = currentSessionId;

    File f = LittleFS.open(FS_HISTORY_PATH, "w");
    if (!f) {
        Serial.println("[FS] Gagal tulis offline history");
        return false;
    }
    size_t written = serializeJson(doc, f);
    f.flush();
    f.close();
    bool ok = written > 0;
    Serial.printf("[FS] History write %s: '%s' %.4f kWh reason=%s pending=%d size=%u\n",
                  ok ? "OK" : "FAIL", name, energyKwh, reasonText,
                  countPendingHistory(arr), (unsigned)fsHistoryFileSize());
    Serial.printf("[FS] Local history count=%d pending=%d\n",
                  (int)arr.size(), countPendingHistory(arr));
    return ok;
}

// ================================================================
// OFFLINE HISTORY — sync to Firebase
// ================================================================
bool fsSyncOfflineHistoryToFirebase() {
    if (!wifiConnected || WiFi.status() != WL_CONNECTED) return false;
    if (!LittleFS.exists(FS_HISTORY_PATH)) return true;
    if (!firebaseHasAuthToken()) {
        Serial.printf("[Sync] Firebase sync skipped: no auth token pending=%d local=%d\n",
                      fsCountOfflineHistory(), fsCountLocalHistory());
        return false;
    }

    File f = LittleFS.open(FS_HISTORY_PATH, "r");
    if (!f) return false;

    DynamicJsonDocument doc(fsHistoryJsonCapacity());
    DeserializationError err = deserializeJson(doc, f);
    f.close();

    if (err || !doc.is<JsonArray>()) {
        Serial.printf("[FS] history_offline.json corrupt, sync skipped: %s\n",
                      err ? err.c_str() : "not array");
        return false;
        Serial.println("[FS] history_offline.json corrupt — hapus");
        LittleFS.remove(FS_HISTORY_PATH);
        Serial.printf("[Sync] Pending cleared, pending=0 size=0\n");
        return true;
    }

    JsonArray arr = doc.as<JsonArray>();
    if (arr.size() == 0) {
        LittleFS.remove(FS_HISTORY_PATH);
        return true;
    }

    int pendingBefore = countPendingHistory(arr);
    if (pendingBefore == 0) {
        Serial.printf("[Sync] No pending history, local=%d size=%u\n",
                      (int)arr.size(), (unsigned)fsHistoryFileSize());
        return true;
    }

    Serial.printf("[Sync] Mulai sync %d sesi offline ke Firebase local=%d size=%u\n",
                  pendingBefore, (int)arr.size(), (unsigned)fsHistoryFileSize());

    int pushed = 0;
    for (JsonObject entry : arr) {
        if (!(entry["pendingSync"] | true)) continue;

        const char*   name    = entry["name"]     | "Offline Device";
        unsigned long startTs = entry["start_ts"] | (unsigned long)0;
        unsigned long endTs   = entry["end_ts"]   | (unsigned long)0;
        float         kwh     = entry["kwh"]      | 0.0f;
        float         cost    = entry["cost"]     | 0.0f;
        float         avgPwr  = entry["power"]    | 0.0f;
        bool          wasOvl  = entry["overload"] | false;
        bool          recovered = entry["recovered"] | false;
        const char*   reason  = entry["endReason"] | (wasOvl ? "OVERLOAD" : "NORMAL_STOP");
        const char*   uid     = entry["uid"]      | "";
        const char*   sessId  = entry["sessionId"] | "";

        if (strlen(uid) > 0) strlcpy(currentUid, uid, sizeof(currentUid));
        if (strlen(sessId) > 0) strlcpy(currentSessionId, sessId, sizeof(currentSessionId));
        if (strlen(currentUid) == 0) {
            Serial.printf("[Sync] '%s' belum punya uid, tetap pending agar tidak hilang dari web history\n",
                          name);
            break;
        }

        String dur = buildDuration(startTs, endTs);
        bool ok = pushHistoryToFirebase(name, dur.c_str(), avgPwr, kwh, cost,
                                        endTs > 0 ? endTs : startTs,
                                        recovered, wasOvl, reason);
        if (!ok) {
            Serial.printf("[Sync] Push '%s' gagal — berhenti\n", name);
            break;
        }
        entry["pendingSync"] = false;
        entry["syncedAt"] = ntpSynced ? (unsigned long)time(nullptr) : millis() / 1000;
        pushed++;
        delay(300);
    }

    File fw = LittleFS.open(FS_HISTORY_PATH, "w");
    if (fw) {
        serializeJson(doc, fw);
        fw.flush();
        fw.close();
    } else {
        Serial.println("[Sync] Gagal simpan status sync lokal");
        return false;
    }

    int pendingAfter = countPendingHistory(arr);
    Serial.printf("[Sync] Pending before=%d after=%d local=%d size=%u\n",
                  pendingBefore, pendingAfter, (int)arr.size(),
                  (unsigned)fsHistoryFileSize());
    if (pendingAfter == 0) {
        Serial.printf("[Sync] Semua %d pending berhasil di-sync, local history tetap disimpan\n",
                      pushed);
        return true;
    }
    Serial.printf("[Sync] %d/%d berhasil, sisanya tetap pending\n",
                  pushed, pendingBefore);
    return false;

    if (pushed == (int)arr.size()) {
        LittleFS.remove(FS_HISTORY_PATH);
        Serial.printf("[Sync] Pending cleared, pending=0 size=0\n");
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
        if (fw) { serializeJson(newDoc, fw); fw.flush(); fw.close(); }
        Serial.printf("[Sync] Pending setelah sync=%d size=%u\n",
                      (int)newArr.size(), (unsigned)fsHistoryFileSize());
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
    DynamicJsonDocument doc(fsHistoryJsonCapacity());
    if (deserializeJson(doc, f) || !doc.is<JsonArray>()) { f.close(); return 0; }
    f.close();
    return countPendingHistory(doc.as<JsonArray>());
}

int fsCountLocalHistory() {
    if (!LittleFS.exists(FS_HISTORY_PATH)) return 0;
    File f = LittleFS.open(FS_HISTORY_PATH, "r");
    if (!f) return 0;
    DynamicJsonDocument doc(fsHistoryJsonCapacity());
    if (deserializeJson(doc, f) || !doc.is<JsonArray>()) { f.close(); return 0; }
    f.close();
    return (int)doc.as<JsonArray>().size();
}

bool fsReadHistoryJson(String &out) {
    out = "[]";
    if (!LittleFS.exists(FS_HISTORY_PATH)) return true;

    File f = LittleFS.open(FS_HISTORY_PATH, "r");
    if (!f) return false;
    DynamicJsonDocument doc(fsHistoryJsonCapacity());
    DeserializationError err = deserializeJson(doc, f);
    f.close();
    if (err || !doc.is<JsonArray>()) {
        Serial.printf("[FS] Local history read failed: %s\n",
                      err ? err.c_str() : "not array");
        return false;
    }

    out = "";
    serializeJson(doc.as<JsonArray>(), out);
    return true;
}
