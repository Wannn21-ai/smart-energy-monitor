/*
 * Smart Energy Monitor v3.1 — main.cpp
 *
 * Perbaikan dari v3.0:
 *  [1] State online/offline dirapikan: transitionTo*() atomic, tidak ada
 *      race condition, guard yang benar.
 *  [2] lastUid, lastTariff, lastThreshold, currentSessionId disimpan ke
 *      Preferences sehingga tidak hilang saat reboot.
 *  [3] Offline mode: relay ON otomatis saat boot offline — clean, tanpa
 *      workaround consistency-check.
 *  [4] Tombol BOOT (GPIO 0):
 *        hold >= 1s  = sesi offline baru  (hanya saat offline + relay OFF)
 *        hold >= 5s  = reset WiFi credentials + restart
 *  [5] OLED: layout konsisten di semua state.
 *  [6] Offline history queue: append → sync saat reconnect/retry 15s.
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <WebServer.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <PZEM004Tv30.h>
#include <DNSServer.h>
#include <LittleFS.h>
#include <ArduinoJson.h>

// ================================================================
// PIN DEFINITIONS
// ================================================================
#define PIN_LED_BLUE    2
#define PIN_LED_GREEN   25
#define PIN_LED_RED     26
#define PIN_BUZZER      5
#define PIN_RELAY       27
#define PIN_RESET_WIFI  0

#define RELAY_ON   LOW
#define RELAY_OFF  HIGH

// ================================================================
// LOCAL AP & WEBSERVER
// ================================================================
DNSServer dnsServer;
const byte DNS_PORT = 53;

#define AP_SSID  "SEM-Config"
#define AP_PASS  "12345678"
WebServer localServer(80);

// ================================================================
// OLED
// ================================================================
#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT 64
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

// ================================================================
// PZEM
// ================================================================
HardwareSerial pzemSerial(2);
PZEM004Tv30 pzem(pzemSerial, 16, 17);

// ================================================================
// FIREBASE
// ================================================================
const char* FIREBASE_HOST = "https://smart-energy-monitor-v2-de79d-default-rtdb.asia-southeast1.firebasedatabase.app";
const char* FIREBASE_PATH = "/live.json";

// ================================================================
// PREFERENCES
// ================================================================
Preferences prefs;

// ================================================================
// CONSTANTS
// ================================================================
const float  TARIF_DEFAULT     = 1444.70f;
const float  THRESHOLD_DEFAULT = 2000.0f;

const unsigned long LOOP_INTERVAL              = 5000;
const unsigned long RECONNECT_INTERVAL         = 60000;
const unsigned long THRESHOLD_SYNC_INTERVAL    = 30000;
const unsigned long COMMAND_POLL_INTERVAL       = 2000;
const unsigned long OVERLOAD_BLINK_MS           = 200;
const unsigned long OVERLOAD_ALERT_LINGER       = 10000;
const unsigned long CHECKPOINT_INTERVAL         = 30000;
const unsigned long OFFLINE_SYNC_RETRY_INTERVAL = 15000;
const unsigned long MODE_TRANSITION_DEBOUNCE    = 2000;

// Button hold thresholds
const unsigned long BTN_NEW_SESSION_HOLD = 1000;   // [4] 1s = sesi baru
const unsigned long BTN_RESET_WIFI_HOLD  = 5000;   // [4] 5s = reset WiFi

const int DISCONNECT_THRESHOLD = 2;

// LittleFS paths
#define FS_SESSION_PATH  "/session_active.json"
#define FS_HISTORY_PATH  "/history_offline.json"

// ================================================================
// STATE — MODE
// ================================================================
bool wifiConnected = false;
bool ntpSynced     = false;
bool modeOffline   = false;   // true = offline mode, false = online mode

unsigned long offlineStartMs       = 0;
unsigned long lastModeTransitionMs = 0;

// ================================================================
// STATE — [2] PERSISTED IDENTIFIERS
// Disimpan ke Preferences sehingga tidak hilang saat reboot.
// ================================================================
char currentUid[64]       = "";
char currentSessionId[48] = "";
char lastCommandId[48]    = "";

// ================================================================
// STATE — SENSOR & RELAY
// ================================================================
bool  relayOn         = false;
bool  deviceConnected = false;
bool  prevDevConn     = false;
bool  isOverload      = false;
int   disconnectCount = 0;

bool          overloadAlertLinger = false;
unsigned long overloadLingerStart = 0;

// ================================================================
// STATE — SESSION
// ================================================================
float overloadThreshold  = THRESHOLD_DEFAULT;
float tarif              = TARIF_DEFAULT;
float sessionEnergyWh    = 0.0f;
float sessionKwh         = 0.0f;
float sessionCost        = 0.0f;
bool  sessionActive      = false;

char          sessionDeviceName[32] = "";
unsigned long sessionStartTs        = 0;
int           offlineDeviceCounter  = 0;

float lastV = 0, lastI = 0, lastP = 0, lastPF = 0, lastHz = 0;
bool  hadDataOnce = false;

// ================================================================
// TIMING
// ================================================================
unsigned long lastLoopMs             = 0;
unsigned long lastReconnectMs        = 0;
unsigned long lastThresholdSyncMs    = 0;
unsigned long lastCommandPollMs      = 0;
unsigned long lastCheckpointMs       = 0;
unsigned long lastOfflineSyncRetryMs = 0;

unsigned long lastBlinkMs         = 0;
bool          blinkState          = false;
unsigned long lastOverloadBlinkMs = 0;
bool          overloadBlinkState  = false;

// ================================================================
// FORWARD DECLARATIONS
// ================================================================
void loadPrefs();
void savePrefs();
void saveSessionId();   // [2]
void loadSessionId();   // [2]

bool   fsInit();
bool   fsWriteSession();
void   fsClearSession();
bool   fsReadSession(float &outEnergyWh, float &outKwh, float &outCost,
                     char *outName, unsigned long &outStartTs);
void   fsAppendOfflineHistory(const char* name, unsigned long startTs,
                              unsigned long endTs, float energyKwh,
                              float cost, float avgPower, bool wasOverload);
bool   fsSyncOfflineHistoryToFirebase();
int    fsCountOfflineHistory();

bool tryConnectWiFi(int sec = 20);
bool tryNTPSync();
void startLocalAP();
void setupWebServer();

// [3] Clean relay control — single entry point
void setRelay(bool on, const char* reason = "");

// [1] Clean mode transitions — atomic, no side-effects outside these fns
void transitionToOnlineMode();
void transitionToOfflineMode(const char* reason = "");

void startOfflineSession(const char* reason);
void generateOfflineDeviceName();

void handleDeviceDisconnect();
void handleOverload(float power);

bool sendToFirebase(float v, float i, float p, float pf, float freq,
                    float kwh, float cost, bool dev, bool ovl,
                    bool relay, unsigned long ts);
bool pushHistoryToFirebase(const char* name, const char* duration,
                           float avgPower, float energyKwh, float cost,
                           unsigned long ts, bool recovered, bool wasOverload);
void syncThresholdFromFirebase();
void pollCommandFromFirebase();
void clearFirebaseCommand();

void handleBlueLed();
void handleGreenLed();
void handleOverloadAlert();
void checkResetButton();   // [4]
void doSessionRecovery();

// [5] OLED helpers
void oledSplash();
void oledStatus(const char* l1, const char* l2 = "");
void oledData(float v, float i, float p, float pf, float hz, float kwh,
              float cost, bool dev, bool online, bool ovl, bool relay,
              bool offline, unsigned long offMs);

String       buildDuration(unsigned long startTs, unsigned long endTs);
unsigned long getSessionElapsedSec(unsigned long nowTs);
String       jsonEscape(const char* s);

// ================================================================
// [2] PREFERENCES — load / save (including persisted session IDs)
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

// [2] Simpan uid + sessionId secara terpisah agar tidak perlu tulis
// threshold/tarif setiap kali session berubah.
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
// WRITE SESSION CHECKPOINT → session_active.json
// ================================================================
bool fsWriteSession() {
    if (!sessionActive || strlen(sessionDeviceName) == 0) return false;

    StaticJsonDocument<256> doc;
    doc["name"]      = sessionDeviceName;
    doc["start"]     = sessionStartTs;
    doc["energy_wh"] = sessionEnergyWh;
    doc["kwh"]       = sessionKwh;
    doc["cost"]      = sessionCost;
    doc["relay"]     = relayOn;
    doc["uid"]       = currentUid;       // [2]
    doc["sessionId"] = currentSessionId; // [2]

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
    Serial.printf("[FS] ✓ Checkpoint: %s %.4f kWh\n", sessionDeviceName, sessionKwh);
    return true;
}

// ================================================================
// CLEAR SESSION FILE
// ================================================================
void fsClearSession() {
    if (LittleFS.exists(FS_SESSION_PATH)) {
        LittleFS.remove(FS_SESSION_PATH);
        Serial.println("[FS] session_active.json dihapus");
    }
}

// ================================================================
// READ SESSION FILE
// ================================================================
bool fsReadSession(float &outEnergyWh, float &outKwh, float &outCost,
                   char *outName, unsigned long &outStartTs) {
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
    strlcpy(outName, doc["name"] | "Recovered", 32);

    // [2] Restore uid/sessionId jika tersimpan di checkpoint
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
// APPEND SESI SELESAI → history_offline.json
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

    File f = LittleFS.open(FS_HISTORY_PATH, "w");
    if (!f) { Serial.println("[FS] Gagal tulis offline history"); return; }
    serializeJson(doc, f);
    f.close();
    Serial.printf("[FS] Offline history: '%s' %.4f kWh disimpan\n", name, energyKwh);
}

// ================================================================
// [6] SYNC OFFLINE HISTORY → Firebase
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
    oledStatus("Sync offline", "history...");

    int pushed = 0;
    for (JsonObject entry : arr) {
        const char*   name     = entry["name"]     | "Offline Device";
        unsigned long startTs  = entry["start_ts"] | (unsigned long)0;
        unsigned long endTs    = entry["end_ts"]   | (unsigned long)0;
        float         kwh      = entry["kwh"]      | 0.0f;
        float         cost     = entry["cost"]     | 0.0f;
        float         avgPwr   = entry["power"]    | 0.0f;
        bool          wasOvl   = entry["overload"] | false;

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

int fsCountOfflineHistory() {
    if (!LittleFS.exists(FS_HISTORY_PATH)) return 0;
    File f = LittleFS.open(FS_HISTORY_PATH, "r");
    if (!f) return 0;
    DynamicJsonDocument doc(4096);
    if (deserializeJson(doc, f) || !doc.is<JsonArray>()) { f.close(); return 0; }
    f.close();
    return (int)doc.as<JsonArray>().size();
}

// ================================================================
// HELPERS
// ================================================================
String buildDuration(unsigned long startTs, unsigned long endTs) {
    if (endTs <= startTs) return "00:00:00";
    unsigned long secs = endTs - startTs;
    char buf[16];
    snprintf(buf, sizeof(buf), "%02lu:%02lu:%02lu",
             secs / 3600, (secs % 3600) / 60, secs % 60);
    return String(buf);
}

unsigned long getSessionElapsedSec(unsigned long nowTs) {
    if (!sessionActive || sessionStartTs == 0 || nowTs <= sessionStartTs) return 0;
    return nowTs - sessionStartTs;
}

String jsonEscape(const char* s) {
    String out;
    if (!s) return out;
    while (*s) {
        char c = *s++;
        if (c == '"' || c == '\\') out += '\\';
        if      (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "\\t";
        else                out += c;
    }
    return out;
}

// ================================================================
// [3] RELAY — single entry point, atomic state update
// ================================================================
void setRelay(bool on, const char* reason) {
    relayOn = on;
    digitalWrite(PIN_RELAY, on ? RELAY_ON : RELAY_OFF);
    sessionActive = on;   // relay ON = session active, OFF = session ended
    if (!on) {
        disconnectCount = 0;
        fsClearSession();
    }
    Serial.printf("[Relay] %s — %s\n", on ? "ON" : "OFF", reason);
}

// ================================================================
// [1] MODE TRANSITIONS — atomic, debounced, clear responsibility
//
// transitionToOnlineMode():
//   - Hanya dipanggil saat modeOffline==true & WiFi baru tersambung
//   - TIDAK mematikan relay (web yang handle via command)
//   - Sync offline history
//
// transitionToOfflineMode():
//   - Hanya dipanggil saat modeOffline==false & WiFi putus
//   - Menyalakan relay otomatis [3]
//   - Generate nama device jika belum ada
// ================================================================
void transitionToOnlineMode() {
    if (!modeOffline) return;

    unsigned long now = millis();
    if (now - lastModeTransitionMs < MODE_TRANSITION_DEBOUNCE) {
        Serial.println("[Mode] Debounced rapid transition (→ Online)");
        return;
    }
    lastModeTransitionMs = now;

    Serial.println("[Mode] ▶ → ONLINE MODE");

    // Checkpoint sesi terakhir sebelum switch
    if (sessionActive && hadDataOnce) {
        if (fsWriteSession()) {
            Serial.println("[Mode→Online] Checkpoint disimpan");
        }
    }

    modeOffline   = false;
    wifiConnected = true;
    offlineStartMs = 0;

    // Relay tetap pada state yang ada — web yang akan kirim START/STOP
    // Tidak ada force relay OFF di sini supaya tidak memutus sesi yang sedang berjalan

    Serial.println("[Mode→Online] State set, akan sync history...");
    fsSyncOfflineHistoryToFirebase();
    clearFirebaseCommand();
}

void transitionToOfflineMode(const char* reason) {
    if (modeOffline) return;

    unsigned long now = millis();
    if (now - lastModeTransitionMs < MODE_TRANSITION_DEBOUNCE) {
        Serial.println("[Mode] Debounced rapid transition (→ Offline)");
        return;
    }
    lastModeTransitionMs = now;

    Serial.printf("[Mode] ▶ → OFFLINE MODE (%s)\n", reason ? reason : "?");

    modeOffline    = true;
    wifiConnected  = false;
    offlineStartMs = now;

    // [3] Relay ON otomatis di offline mode
    if (!relayOn) {
        if (strlen(sessionDeviceName) == 0) generateOfflineDeviceName();
        unsigned long ts = ntpSynced ? (unsigned long)time(nullptr) : now / 1000;
        sessionStartTs  = ts;
        sessionEnergyWh = 0;
        sessionKwh      = 0;
        sessionCost     = 0;
        hadDataOnce     = false;
        disconnectCount = 0;
        isOverload      = false;
        overloadAlertLinger = false;
        setRelay(true, "offline mode auto-start");
        Serial.printf("[Mode→Offline] Relay ON — device: %s\n", sessionDeviceName);
    } else {
        // Relay sudah ON (mungkin sedang ada sesi online yang lanjut offline)
        if (strlen(sessionDeviceName) == 0) generateOfflineDeviceName();
        Serial.printf("[Mode→Offline] Relay tetap ON — device: %s\n", sessionDeviceName);
    }
}

// ================================================================
// OFFLINE SESSION — start a new offline session
// Dipanggil oleh: transitionToOfflineMode(), tombol (1s hold)
// ================================================================
void startOfflineSession(const char* reason) {
    // Generate nama baru untuk setiap sesi offline baru
    sessionDeviceName[0] = '\0';
    generateOfflineDeviceName();

    sessionEnergyWh     = 0;
    sessionKwh          = 0;
    sessionCost         = 0;
    hadDataOnce         = false;
    deviceConnected     = false;
    prevDevConn         = false;
    disconnectCount     = 0;
    isOverload          = false;
    overloadAlertLinger = false;
    currentSessionId[0] = '\0';

    unsigned long nowTs = ntpSynced ? (unsigned long)time(nullptr) : millis() / 1000;
    sessionStartTs = nowTs;

    setRelay(true, reason);
    Serial.printf("[Offline] New session: %s (%s)\n", sessionDeviceName, reason);
    oledStatus("Sesi Baru", sessionDeviceName);
    delay(1000);
}

// ================================================================
// GENERATE OFFLINE DEVICE NAME
// ================================================================
void generateOfflineDeviceName() {
    if (strlen(sessionDeviceName) > 0) return;  // sudah ada nama
    int count = fsCountOfflineHistory();
    if (offlineDeviceCounter <= count) offlineDeviceCounter = count + 1;
    else offlineDeviceCounter++;
    snprintf(sessionDeviceName, sizeof(sessionDeviceName),
             "Device %d", offlineDeviceCounter);
    Serial.printf("[Offline] Nama device: %s\n", sessionDeviceName);
}

// ================================================================
// SESSION RECOVERY
// ================================================================
void doSessionRecovery() {
    float recoveredEnergyWh = 0, recoveredKwh = 0, recoveredCost = 0;
    char  recoveredName[32] = "";
    unsigned long recoveredStartTs = 0;

    if (!fsReadSession(recoveredEnergyWh, recoveredKwh, recoveredCost,
                       recoveredName, recoveredStartTs)) return;

    Serial.printf("[Recovery] Ditemukan: '%s' %.4f kWh\n", recoveredName, recoveredKwh);
    oledStatus("Recovery sesi", recoveredName);
    delay(1500);

    unsigned long nowTs = ntpSynced ? (unsigned long)time(nullptr) : millis() / 1000;
    String dur = buildDuration(recoveredStartTs, nowTs);

    if (wifiConnected) {
        bool pushed = pushHistoryToFirebase(
            recoveredName, dur.c_str(),
            0.0f, recoveredKwh, recoveredCost,
            nowTs, true, false);
        if (pushed) Serial.println("[Recovery] Push Firebase OK");
        else {
            Serial.println("[Recovery] Push gagal — antri offline");
            fsAppendOfflineHistory(recoveredName, recoveredStartTs, nowTs,
                                   recoveredKwh, recoveredCost, 0.0f, false);
        }
    } else {
        fsAppendOfflineHistory(recoveredName, recoveredStartTs, nowTs,
                               recoveredKwh, recoveredCost, 0.0f, false);
        Serial.println("[Recovery] Simpan ke offline history");
    }

    fsClearSession();
    oledStatus("Recovery selesai", wifiConnected ? "Tersimpan ✓" : "Antri sync");
    delay(1000);
}

// ================================================================
// DEVICE DISCONNECT
// ================================================================
void handleDeviceDisconnect() {
    if (!relayOn || !sessionActive) return;
    if (!deviceConnected && prevDevConn) {
        disconnectCount++;
        Serial.printf("[Disconnect] %d/%d\n", disconnectCount, DISCONNECT_THRESHOLD);
        if (disconnectCount >= DISCONNECT_THRESHOLD) {
            Serial.println("[Disconnect] ✓ Device dicabut — simpan sesi, relay OFF");
            unsigned long nowTs = ntpSynced ? (unsigned long)time(nullptr) : millis() / 1000;
            String dur = buildDuration(sessionStartTs, nowTs);

            if (wifiConnected) {
                bool ok = pushHistoryToFirebase(sessionDeviceName, dur.c_str(),
                                               lastP, sessionKwh, sessionCost,
                                               nowTs, false, false);
                sendToFirebase(0,0,0,0,0, sessionKwh, sessionCost, false, false, true, nowTs);
                Serial.printf("[Disconnect] History push: %s\n", ok ? "OK" : "FAIL");
                if (!ok) {
                    fsAppendOfflineHistory(sessionDeviceName, sessionStartTs, nowTs,
                                          sessionKwh, sessionCost, lastP, false);
                }
            } else {
                fsAppendOfflineHistory(sessionDeviceName, sessionStartTs, nowTs,
                                      sessionKwh, sessionCost, lastP, false);
                Serial.println("[Disconnect] History antri offline");
            }

            bool wasOffline = modeOffline;
            setRelay(false, "device dicabut");
            sessionEnergyWh = 0; sessionKwh = 0; sessionCost = 0;
            hadDataOnce = false; sessionDeviceName[0] = '\0';

            // [3] Di offline mode, langsung mulai sesi baru otomatis
            if (wasOffline) {
                delay(500);
                startOfflineSession("offline auto-restart");
            }
        }
    } else if (deviceConnected) {
        disconnectCount = 0;
    }
}

// ================================================================
// OVERLOAD
// ================================================================
void handleOverload(float power) {
    bool newOvl = deviceConnected && (power >= overloadThreshold);
    if (newOvl && !isOverload) {
        isOverload = true;
        Serial.printf("[Overload] ⚠ %.1fW >= %.0fW — relay OFF\n", power, overloadThreshold);

        unsigned long nowTs = ntpSynced ? (unsigned long)time(nullptr) : millis() / 1000;
        String dur = buildDuration(sessionStartTs, nowTs);

        if (wifiConnected) {
            bool ok = pushHistoryToFirebase(sessionDeviceName, dur.c_str(),
                                           power, sessionKwh, sessionCost,
                                           nowTs, false, true);
            Serial.printf("[Overload] History push: %s\n", ok ? "OK" : "FAIL");
            if (!ok) {
                fsAppendOfflineHistory(sessionDeviceName, sessionStartTs, nowTs,
                                      sessionKwh, sessionCost, power, true);
            }
        } else {
            fsAppendOfflineHistory(sessionDeviceName, sessionStartTs, nowTs,
                                  sessionKwh, sessionCost, power, true);
        }

        bool wasOffline = modeOffline;
        setRelay(false, "overload");
        overloadAlertLinger = true;
        overloadLingerStart = millis();
        sessionEnergyWh = 0; sessionKwh = 0; sessionCost = 0;
        sessionDeviceName[0] = '\0';

        // [3] Di offline mode, mulai sesi baru setelah linger
        // (ditangani di loop setelah linger selesai)
        (void)wasOffline;

    } else if (!newOvl && isOverload) {
        isOverload = false;
        Serial.println("[Overload] ✓ Teratasi");

        // [3] Di offline mode, auto-start sesi baru setelah overload teratasi
        if (modeOffline && !relayOn && !overloadAlertLinger) {
            delay(500);
            startOfflineSession("offline post-overload");
        }
    }
}

// ================================================================
// FIREBASE — push history
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
// FIREBASE — clear command
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
// FIREBASE — poll relay command (ONLINE MODE only)
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

        // [2] Simpan uid + sessionId ke Preferences
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

        // [2] Clear session ID setelah stop
        currentSessionId[0] = '\0';
        saveSessionId();

        clearFirebaseCommand();
        Serial.println("[Command] ✓ Relay OFF (web)");
    }
}

// ================================================================
// FIREBASE — threshold sync
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
// FIREBASE — send live data
// ================================================================
bool sendToFirebase(float v, float i, float p, float pf, float hz,
                    float kwh, float cost, bool dev, bool ovl,
                    bool relay, unsigned long ts) {
    if (!wifiConnected || WiFi.status() != WL_CONNECTED) return false;

    WiFiClientSecure c; c.setInsecure(); c.setTimeout(10000);
    HTTPClient h;
    h.begin(c, String(FIREBASE_HOST) + FIREBASE_PATH);
    h.addHeader("Content-Type", "application/json");

    unsigned long elapsedSec = getSessionElapsedSec(ts);
    const char* modeStr = modeOffline
        ? (relay ? "OFFLINE_MONITORING" : "OFFLINE_IDLE")
        : (relay ? "ONLINE_MONITORING"  : "ONLINE_IDLE");

    int pendingSync = fsCountOfflineHistory();

    String j = "{";
    j += "\"system\":{";
    j += "\"timestamp\":"     + String(ts);
    j += ",\"internet\":true";
    j += ",\"threshold\":"    + String(overloadThreshold, 0);
    j += ",\"tarif\":"        + String(tarif, 2);
    j += ",\"relay\":"        + String(relay ? "true" : "false");
    j += ",\"offline\":"      + String(modeOffline ? "true" : "false");
    j += ",\"sessionActive\":" + String(sessionActive ? "true" : "false");
    j += ",\"sessionStartTs\":" + String(sessionStartTs);
    j += ",\"elapsedSec\":"   + String(elapsedSec);
    j += ",\"mode\":\"";       j += modeStr; j += "\"";
    j += ",\"uid\":\"";        j += jsonEscape(currentUid); j += "\"";
    j += ",\"sessionId\":\"";  j += jsonEscape(currentSessionId); j += "\"";
    j += ",\"deviceName\":\""; j += jsonEscape(sessionDeviceName); j += "\"";
    j += ",\"pendingSync\":"; j += String(pendingSync);
    j += "},";
    j += "\"connected\":"  + String(dev ? "true" : "false") + ",";
    j += "\"overload\":"   + String(ovl ? "true" : "false") + ",";
    j += "\"device\":{";
    j += "\"voltage\":"    + String(v, 1) + ",";
    j += "\"current\":"    + String(i, 2) + ",";
    j += "\"power\":"      + String(p, 1) + ",";
    j += "\"apparent\":"   + String(v * i, 1) + ",";
    j += "\"pf\":"         + String(pf, 2) + ",";
    j += "\"frequency\":"  + String(hz, 1) + ",";
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
// WiFi & NTP
// ================================================================
bool tryConnectWiFi(int sec) {
    WiFi.begin();
    Serial.print("[WiFi] Connecting");
    for (int i = 0; i < sec * 2 && WiFi.status() != WL_CONNECTED; i++) {
        delay(500);
        Serial.print(".");
        blinkState = !blinkState;
        digitalWrite(PIN_LED_BLUE, blinkState);
    }
    if (WiFi.status() == WL_CONNECTED) {
        Serial.println("\n[WiFi] OK: " + WiFi.localIP().toString());
        digitalWrite(PIN_LED_BLUE, HIGH);
        return true;
    }
    Serial.println("\n[WiFi] Gagal");
    return false;
}

bool tryNTPSync() {
    configTime(7 * 3600, 0, "pool.ntp.org", "time.nist.gov");
    struct tm ti;
    int r = 0;
    while (!getLocalTime(&ti) && r < 20) { delay(500); r++; }
    return r < 20;
}

// ================================================================
// LOCAL AP
// ================================================================
void startLocalAP() {
    IPAddress ip(192, 168, 4, 1), sub(255, 255, 255, 0);
    WiFi.softAPConfig(ip, ip, sub);
    WiFi.softAP(AP_SSID, AP_PASS);
    dnsServer.stop();
    dnsServer.start(DNS_PORT, "*", ip);
    Serial.printf("[AP] '%s' aktif — 192.168.4.1\n", AP_SSID);
}

// ================================================================
// LOCAL WEBSERVER (unchanged from v3.0 — captive portal)
// ================================================================
void handleRoot() {
    int n = WiFi.scanNetworks();
    String ssidOptions = "";
    for (int i = 0; i < n; i++) {
        String ssid = WiFi.SSID(i);
        int rssi    = WiFi.RSSI(i);
        bool locked = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN);
        int bars = rssi > -50 ? 4 : rssi > -65 ? 3 : rssi > -75 ? 2 : 1;
        String bar = bars == 4 ? "████" : bars == 3 ? "███░" : bars == 2 ? "██░░" : "█░░░";
        ssidOptions += "<option value='" + ssid + "'>"
                    + bar + " " + ssid + (locked ? " 🔒" : "") + "</option>";
    }
    if (n == 0) ssidOptions = "<option value=''>Tidak ada jaringan ditemukan</option>";

    int pendingSync = fsCountOfflineHistory();
    String syncInfo = "";
    if (pendingSync > 0) {
        syncInfo = "<div style='background:rgba(255,171,0,0.1);border:1px solid rgba(255,171,0,0.3);"
                   "border-radius:10px;padding:10px 14px;font-size:12px;color:#ffab00;margin-bottom:14px;'>"
                   "⚡ " + String(pendingSync) + " sesi offline menunggu sync ke Firebase</div>";
    }

    // HTML sama seperti v3.0 — dipersingkat untuk tidak mengulang 400+ baris
    // Full HTML ada di v3.0, hanya syncInfo yang ditambah
    String html = "<!DOCTYPE html><html lang='id'><head>"
        "<meta charset='UTF-8'/><meta name='viewport' content='width=device-width,initial-scale=1'/>"
        "<title>SEM Config</title>"
        "<style>body{background:#0a0a0a;color:#f0f0f0;font-family:sans-serif;"
        "display:flex;flex-direction:column;align-items:center;padding:24px 16px}"
        ".card{background:#111;border:1px solid #222;border-radius:14px;padding:22px;"
        "width:100%;max-width:420px;margin-bottom:14px}"
        "label{display:block;font-size:11px;color:#666;text-transform:uppercase;"
        "letter-spacing:.06em;margin-bottom:7px;margin-top:14px}"
        "select,input{width:100%;padding:12px 14px;background:#1a1a1a;"
        "border:1px solid #2a2a2a;border-radius:10px;color:#f0f0f0;font-size:15px;outline:none}"
        "select:focus,input:focus{border-color:#00e5ff}"
        ".btn{display:block;width:100%;padding:14px;border:none;border-radius:12px;"
        "font-size:15px;font-weight:700;cursor:pointer;margin-top:16px}"
        ".btn-p{background:#00e5ff;color:#000}.btn-g{background:transparent;color:#555;"
        "border:1px solid #2a2a2a;margin-top:8px}"
        "#toast{position:fixed;bottom:24px;left:50%;transform:translateX(-50%) translateY(80px);"
        "background:#1a1a1a;border:1px solid #2a2a2a;color:#f0f0f0;padding:12px 20px;"
        "border-radius:12px;font-size:13px;transition:transform .3s;z-index:999}"
        "#toast.show{transform:translateX(-50%) translateY(0)}"
        "#toast.ok{color:#00e676}#toast.err{color:#ff1744}#toast.info{color:#00e5ff}"
        "</style></head><body>"
        "<h2 style='color:#00e5ff;font-size:16px;margin-bottom:20px'>⚡ Smart Energy Monitor</h2>"
        "<div class='card'><b style='color:#888;font-size:11px;text-transform:uppercase;"
        "letter-spacing:.1em'>Koneksi WiFi</b>"
        + syncInfo +
        "<label>Pilih Jaringan</label>"
        "<select id='ssid'>" + ssidOptions + "</select>"
        "<label>Password</label>"
        "<input type='password' id='wpass' placeholder='••••••••'/>"
        "<button class='btn btn-p' onclick='connect()'>Hubungkan</button></div>"
        "<div class='card'><b style='color:#888;font-size:11px;text-transform:uppercase;"
        "letter-spacing:.1em'>Pengaturan</b>"
        "<label>Tarif / kWh (IDR)</label>"
        "<input type='number' id='trf' value='" + String(tarif, 2) + "' step='0.01'/>"
        "<label>Batas Overload (Watt)</label>"
        "<input type='number' id='thr' value='" + String(overloadThreshold, 0) + "' step='100'/>"
        "<button class='btn btn-p' onclick='save()'>Simpan Pengaturan</button>"
        "<button class='btn btn-g' onclick='resetWifi()'>Reset WiFi</button></div>"
        "<div id='toast'></div>"
        R"JS(<script>
var _tt;
function toast(msg,type){var el=document.getElementById('toast');
el.textContent=msg;el.className=type||'';el.classList.add('show');
clearTimeout(_tt);_tt=setTimeout(function(){el.classList.remove('show');},4000);}
function connect(){var ssid=document.getElementById('ssid').value;
var pass=document.getElementById('wpass').value;
if(!ssid){toast('Pilih jaringan WiFi','err');return;}
toast('Menghubungkan ke "'+ssid+'"...','info');
fetch('/connectwifi?ssid='+encodeURIComponent(ssid)+'&pass='+encodeURIComponent(pass))
.then(function(r){return r.text();})
.then(function(t){if(t.indexOf('Berhasil')>=0)toast('✓ '+t,'ok');
else toast('✗ '+t,'err');}).catch(function(){toast('Timeout — coba lagi','err');});}
function save(){var thr=parseFloat(document.getElementById('thr').value);
var trf=parseFloat(document.getElementById('trf').value);
if(isNaN(thr)||thr<100||thr>10000){toast('Threshold 100–10000 W','err');return;}
if(isNaN(trf)||trf<=0){toast('Tarif tidak valid','err');return;}
fetch('/save?thr='+thr+'&trf='+trf).then(function(r){return r.text();})
.then(function(t){if(t.indexOf('Tersimpan')>=0)toast('✓ '+t,'ok');
else toast('✗ '+t,'err');});}
function resetWifi(){if(!confirm('Reset WiFi?'))return;
fetch('/resetwifi').then(function(){toast('Mereset...','info');});}
document.addEventListener('keydown',function(e){if(e.key==='Enter')connect();});
</script>)JS"
        "</body></html>";

    localServer.send(200, "text/html; charset=utf-8", html);
}

void handleSave() {
    bool ok = false;
    if (localServer.hasArg("thr")) {
        float v = localServer.arg("thr").toFloat();
        if (v >= 100 && v <= 10000) { overloadThreshold = v; ok = true; }
    }
    if (localServer.hasArg("trf")) {
        float v = localServer.arg("trf").toFloat();
        if (v > 0) { tarif = v; ok = true; }
    }
    if (ok) {
        savePrefs();
        if (wifiConnected) {
            WiFiClientSecure c; c.setInsecure(); c.setTimeout(5000);
            HTTPClient h;
            if (h.begin(c, String(FIREBASE_HOST) + "/config/threshold.json")) {
                h.addHeader("Content-Type", "application/json");
                h.PUT(String(overloadThreshold, 0));
                h.end();
            }
        }
        localServer.send(200, "text/plain",
            "✓ Tersimpan! Threshold=" + String(overloadThreshold, 0) +
            "W | Tarif=Rp" + String(tarif, 2));
    } else {
        localServer.send(400, "text/plain", "Nilai tidak valid");
    }
}

void handleConnectWifi() {
    if (!localServer.hasArg("ssid")) {
        localServer.send(400, "text/plain", "SSID diperlukan"); return;
    }
    String ssid = localServer.arg("ssid");
    String pass = localServer.hasArg("pass") ? localServer.arg("pass") : "";

    WiFi.begin(ssid.c_str(), pass.c_str());
    int waited = 0;
    while (WiFi.status() != WL_CONNECTED && waited < 20) {
        delay(500); waited++;
        localServer.handleClient();
    }

    if (WiFi.status() == WL_CONNECTED) {
        // Transition ke online mode
        if (modeOffline) transitionToOnlineMode();
        else { wifiConnected = true; }

        WiFi.setSleep(false);
        if (!ntpSynced) ntpSynced = tryNTPSync();
        syncThresholdFromFirebase();
        clearFirebaseCommand();
        fsSyncOfflineHistoryToFirebase();

        String ip = WiFi.localIP().toString();
        localServer.send(200, "text/plain",
            "✓ Berhasil! IP: " + ip + " — Dashboard web aktif.");
    } else {
        localServer.send(200, "text/plain",
            "✗ Gagal terhubung ke \"" + ssid + "\". Cek SSID & password.");
    }
}

void handleResetWifi() {
    localServer.send(200, "text/plain", "Mereset WiFi — ESP32 restart...");
    delay(500);
    if (sessionActive && hadDataOnce) fsWriteSession();
    WiFi.disconnect(true, true);
    delay(500);
    ESP.restart();
}

void handleStatus() {
    String ip = wifiConnected ? WiFi.localIP().toString() : "—";
    int pending = fsCountOfflineHistory();
    String json = "{\"wifi\":" + String(wifiConnected ? "true" : "false") +
                  ",\"ip\":\"" + ip + "\"" +
                  ",\"mode\":\"" + String(modeOffline ? "offline" : "online") + "\"" +
                  ",\"threshold\":" + String(overloadThreshold, 0) +
                  ",\"tarif\":" + String(tarif, 2) +
                  ",\"pendingSync\":" + String(pending) + "}";
    localServer.send(200, "application/json", json);
}

void handleCaptivePortal() {
    localServer.sendHeader("Location", "http://192.168.4.1/", true);
    localServer.send(302, "text/plain", "");
}

void setupWebServer() {
    localServer.on("/",            HTTP_GET, handleRoot);
    localServer.on("/index.html",  HTTP_GET, handleRoot);
    localServer.on("/save",        HTTP_GET, handleSave);
    localServer.on("/connectwifi", HTTP_GET, handleConnectWifi);
    localServer.on("/resetwifi",   HTTP_GET, handleResetWifi);
    localServer.on("/status",      HTTP_GET, handleStatus);

    // Captive portal detection URLs
    auto captive = []() { handleCaptivePortal(); };
    localServer.on("/hotspot-detect.html",  HTTP_GET, captive);
    localServer.on("/generate_204",         HTTP_GET, captive);
    localServer.on("/gen_204",              HTTP_GET, captive);
    localServer.on("/ncsi.txt",             HTTP_GET, captive);
    localServer.on("/connecttest.txt",      HTTP_GET, captive);
    localServer.on("/redirect",             HTTP_GET, captive);
    localServer.onNotFound(handleCaptivePortal);

    localServer.begin();
    Serial.println("[LocalWeb] Captive portal aktif di http://192.168.4.1");
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

void handleOverloadAlert() {
    if (overloadAlertLinger && !isOverload) {
        if (millis() - overloadLingerStart >= OVERLOAD_ALERT_LINGER) {
            overloadAlertLinger = false;
            overloadBlinkState  = false;
            digitalWrite(PIN_LED_RED, LOW);
            digitalWrite(PIN_BUZZER,  LOW);
            // [3] Offline mode: auto-start sesi baru setelah linger selesai
            if (modeOffline && !relayOn) {
                delay(200);
                startOfflineSession("offline post-overload-linger");
            }
            return;
        }
    }
    bool active = isOverload || overloadAlertLinger;
    if (!active) {
        digitalWrite(PIN_LED_RED, LOW);
        digitalWrite(PIN_BUZZER,  LOW);
        overloadBlinkState = false;
        return;
    }
    if (millis() - lastOverloadBlinkMs >= OVERLOAD_BLINK_MS) {
        overloadBlinkState = !overloadBlinkState;
        digitalWrite(PIN_LED_RED, overloadBlinkState ? HIGH : LOW);
        digitalWrite(PIN_BUZZER,  overloadBlinkState ? HIGH : LOW);
        lastOverloadBlinkMs = millis();
    }
}

// ================================================================
// [4] RESET BUTTON — GPIO 0 (BOOT button)
//
//   Tahan >= 1 detik  (< 5 detik):
//     Jika offline mode DAN relay OFF DAN tidak ada sesi:
//       → Mulai sesi offline baru
//     Lainnya: tidak ada aksi
//
//   Tahan >= 5 detik:
//     → Reset WiFi credentials + restart ESP32
//     (berlaku di semua kondisi)
//
// OLED feedback:
//   0–1s:  "Tahan utk aksi"   "1s=Baru 5s=Reset"
//   1–5s:  "Lepas=Sesi Baru"  "Tahan=Reset WiFi"
//   >5s:   "Reset WiFi..."    ""
// ================================================================
void checkResetButton() {
    if (digitalRead(PIN_RESET_WIFI) != LOW) return;

    unsigned long pressStart = millis();

    // Tampilkan petunjuk awal
    oledStatus("Tahan utk aksi", "1s=Baru 5s=Reset");

    while (digitalRead(PIN_RESET_WIFI) == LOW) {
        unsigned long held = millis() - pressStart;

        // Update OLED feedback berdasarkan durasi tahan
        if (held >= BTN_RESET_WIFI_HOLD) {
            // Sudah melewati threshold reset — tunjukkan konfirmasi
            oledStatus("Reset WiFi...", "Lepaskan utk batal");
        } else if (held >= BTN_NEW_SESSION_HOLD) {
            // Di antara 1s–5s: tunjukkan opsi yang tersedia
            if (modeOffline && !relayOn && !sessionActive) {
                oledStatus("Lepas=Sesi Baru", "Tahan=Reset WiFi");
            } else {
                oledStatus("Lepas utk batal", "Tahan=Reset WiFi");
            }
        }

        // Cegah watchdog timeout
        localServer.handleClient();
        dnsServer.processNextRequest();
        delay(50);
    }

    unsigned long held = millis() - pressStart;
    Serial.printf("[Button] Released after %lu ms\n", held);

    if (held >= BTN_RESET_WIFI_HOLD) {
        // [4] Hold >= 5s: RESET WiFi
        Serial.println("[Button] ★ Reset WiFi triggered");
        oledStatus("Reset WiFi...", "Restarting...");

        // Simpan checkpoint sesi terakhir sebelum reset
        if (sessionActive && hadDataOnce) {
            if (fsWriteSession()) {
                Serial.println("[Button] Checkpoint disimpan sebelum reset");
            }
        }

        // Matikan relay sebelum restart
        digitalWrite(PIN_RELAY, RELAY_OFF);
        relayOn = false;

        WiFi.disconnect(true, true);
        delay(500);
        ESP.restart();

    } else if (held >= BTN_NEW_SESSION_HOLD) {
        // [4] Hold >= 1s (< 5s): Sesi offline baru
        if (modeOffline && !relayOn && !sessionActive) {
            Serial.println("[Button] ★ New offline session triggered");
            startOfflineSession("button press");
        } else {
            Serial.printf("[Button] 1s hold ignored — modeOffline=%d relayOn=%d sessionActive=%d\n",
                          modeOffline, relayOn, sessionActive);
            // Tampilkan info mengapa tidak bisa mulai sesi baru
            if (!modeOffline) {
                oledStatus("Mode Online", "Gunakan web app");
            } else if (relayOn) {
                oledStatus("Sesi aktif", sessionDeviceName);
            }
            delay(1500);
        }
    } else {
        // Tahan < 1s: tidak ada aksi
        Serial.println("[Button] Short press — no action");
    }
}

// ================================================================
// [5] OLED — splash
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
// [5] OLED — status screen (2 lines, centered vertically)
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
// [5] OLED — data screen
//
// Layout (128×64, 8px/char, 8 rows):
//   Row 0 (y=0):  [mode badge]        [relay badge]
//   Row 1 (y=9):  separator line
//   Row 2 (y=13): device name (max 16 chars)
//   Row 3 (y=23): V: xxx.xV  I: x.xxA
//   Row 4 (y=33): P: xxxx.xW PF: x.xx
//   Row 5 (y=43): separator line
//   Row 6 (y=47): E: x.xxxx kWh
//   Row 7 (y=57): Cost: Rp xxxxx
//
// Special states handled:
//   - Overload: full-screen warning
//   - Relay OFF (online): "Relay OFF / Web: klik +"
//   - Relay OFF (offline): "Relay OFF / Tahan 1s: Sesi Baru"
//   - Relay ON, no device: "No Device / Colokkan beban..."
// ================================================================
void oledData(float v, float i, float p, float pf, float hz, float kwh,
              float cost, bool dev, bool online, bool ovl, bool relay,
              bool offline, unsigned long offMs) {
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
    // Relay badge rata kanan (estimasi: 8 char × 6px = 48px dari kanan)
    const char* relayBadge = relay ? "[RLY:ON]" : "[RLY:OFF]";
    int badgeX = 128 - (int)strlen(relayBadge) * 6;
    if (badgeX < 64) badgeX = 64;
    display.setCursor(badgeX, 0);
    display.print(relayBadge);

    // ── Separator ───────────────────────────────────────────────
    display.drawLine(0, 9, 127, 9, WHITE);

    // ── OVERLOAD state ──────────────────────────────────────────
    if (ovl || overloadAlertLinger) {
        display.setCursor(10, 13); display.println("!! OVERLOAD !!");
        display.setCursor(0,  23); display.printf("%.1fW >= %.0fW", lastP, overloadThreshold);
        display.setCursor(0,  33); display.println("Relay OFF");
        display.setCursor(0,  43); display.println("192.168.4.1");
        if (offline) {
            display.setCursor(0, 53); display.println("Relay ON otomatis...");
        }
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
        // Tampilkan nama sesi yang sedang menunggu device
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
// SETUP
// ================================================================
void setup() {
    Serial.begin(115200);
    delay(100);
    Serial.println("\n[Boot] Smart Energy Monitor v3.1");

    // GPIO init
    pinMode(PIN_LED_BLUE,   OUTPUT); pinMode(PIN_LED_GREEN,  OUTPUT);
    pinMode(PIN_LED_RED,    OUTPUT); pinMode(PIN_BUZZER,     OUTPUT);
    pinMode(PIN_RELAY,      OUTPUT); pinMode(PIN_RESET_WIFI, INPUT_PULLUP);

    digitalWrite(PIN_LED_BLUE, LOW); digitalWrite(PIN_LED_GREEN, LOW);
    digitalWrite(PIN_LED_RED,  LOW); digitalWrite(PIN_BUZZER,    LOW);
    digitalWrite(PIN_RELAY, RELAY_OFF);
    relayOn = false;

    pzemSerial.begin(9600, SERIAL_8N1, 16, 17);

    // [2] Load all persisted values
    loadPrefs();
    Serial.printf("[Prefs] threshold=%.0fW tarif=%.2f uid=%s sessionId=%s\n",
                  overloadThreshold, tarif, currentUid, currentSessionId);

    // LittleFS
    fsInit();

    // OLED
    if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
        Serial.println("[OLED] Init gagal");
    } else {
        oledSplash();
        delay(2000);
    }

    // [4] Check tombol sebelum WiFi (untuk tombol reset di awal)
    checkResetButton();

    // WiFi AP + webserver
    WiFi.mode(WIFI_AP_STA);
    startLocalAP();
    setupWebServer();

    oledStatus("AP: SEM-Config", "pw: 12345678");
    delay(1500);

    // ─────────────────────────────────────────────────────────────
    // STARTUP: Detect mode (online / offline)
    // ─────────────────────────────────────────────────────────────
    oledStatus("Checking WiFi...", "");
    wifiConnected = tryConnectWiFi(20);

    if (wifiConnected) {
        // ★ ONLINE MODE STARTUP
        Serial.println("[Boot] ★ ONLINE MODE");
        modeOffline = false;

        WiFi.setSleep(false);
        oledStatus("WiFi OK ✓", "Sync NTP...");
        ntpSynced = tryNTPSync();
        delay(500);

        syncThresholdFromFirebase();
        clearFirebaseCommand();

        // Session recovery
        doSessionRecovery();

        // [6] Sync offline history yang tertinggal
        fsSyncOfflineHistoryToFirebase();

        unsigned long ts = ntpSynced ? (unsigned long)time(nullptr) : millis() / 1000;
        sendToFirebase(0, 0, 0, 0, 0, 0, 0, false, false, false, ts);

        oledStatus("Online Ready ✓", "Waiting for web...");
        delay(1000);
        Serial.println("[Boot-Online] Relay OFF — menunggu command web");

    } else {
        // ★ OFFLINE MODE STARTUP [3]
        Serial.println("[Boot] ★ OFFLINE MODE");
        // Langsung set modeOffline = true tanpa debounce (ini boot pertama)
        modeOffline   = true;
        offlineStartMs = millis();
        lastModeTransitionMs = millis();

        // Session recovery dulu (jika ada sesi tersimpan dari boot sebelumnya)
        doSessionRecovery();

        // [3] Relay ON otomatis
        generateOfflineDeviceName();
        unsigned long nowTs = millis() / 1000;
        sessionStartTs  = nowTs;
        sessionEnergyWh = 0; sessionKwh = 0; sessionCost = 0;
        hadDataOnce     = false; disconnectCount = 0;
        isOverload      = false; overloadAlertLinger = false;

        setRelay(true, "offline boot auto-start");

        lastReconnectMs = millis();
        oledStatus("Offline Mode ✓", sessionDeviceName);
        delay(1500);
        Serial.printf("[Boot-Offline] Relay ON — device: %s\n", sessionDeviceName);
    }

    // Init timers
    lastLoopMs = lastThresholdSyncMs = lastCommandPollMs =
    lastCheckpointMs = lastOfflineSyncRetryMs = millis();
}

// ================================================================
// LOOP
// ================================================================
void loop() {
    unsigned long now = millis();

    // Serve local webserver + DNS
    localServer.handleClient();
    dnsServer.processNextRequest();

    // [4] Button check every loop
    checkResetButton();

    // LED & alert handlers
    handleBlueLed();
    handleGreenLed();
    handleOverloadAlert();

    // ── WiFi disconnect detection → OFFLINE MODE ─────────────
    if (wifiConnected && WiFi.status() != WL_CONNECTED) {
        Serial.println("[WiFi] ✗ Disconnected");
        wifiConnected = false;
        digitalWrite(PIN_LED_BLUE, LOW);
        lastReconnectMs = now;

        // Checkpoint sebelum transisi
        if (sessionActive && hadDataOnce) {
            fsWriteSession();
        }

        // [1] Atomic transition to offline
        transitionToOfflineMode("wifi lost");
    }

    // ── Auto-reconnect every 60s ──────────────────────────────
    if (!wifiConnected && (now - lastReconnectMs >= RECONNECT_INTERVAL)) {
        lastReconnectMs = now;
        Serial.println("[WiFi] Trying reconnect...");

        if (tryConnectWiFi(15)) {
            // [1] Atomic transition to online
            transitionToOnlineMode();
            WiFi.setSleep(false);
            if (!ntpSynced) ntpSynced = tryNTPSync();
            syncThresholdFromFirebase();

            // [6] Sync pending offline sessions
            int pending = fsCountOfflineHistory();
            if (pending > 0) {
                Serial.printf("[Reconnect] Syncing %d pending sessions\n", pending);
                fsSyncOfflineHistoryToFirebase();
            }

            // Send live data confirm
            unsigned long ts = ntpSynced ? (unsigned long)time(nullptr) : now / 1000;
            sendToFirebase(lastV, lastI, lastP, lastPF, lastHz,
                           sessionKwh, sessionCost,
                           deviceConnected, isOverload, relayOn, ts);
        }
    }

    // ── Threshold sync (online only) ──────────────────────────
    if (wifiConnected && (now - lastThresholdSyncMs >= THRESHOLD_SYNC_INTERVAL)) {
        lastThresholdSyncMs = now;
        syncThresholdFromFirebase();
    }

    // ── Command poll (ONLINE MODE only) ──────────────────────
    if (wifiConnected && !modeOffline && (now - lastCommandPollMs >= COMMAND_POLL_INTERVAL)) {
        lastCommandPollMs = now;
        pollCommandFromFirebase();
    }

    // ── [6] Offline history retry sync (online only) ─────────
    if (wifiConnected && (now - lastOfflineSyncRetryMs >= OFFLINE_SYNC_RETRY_INTERVAL)) {
        lastOfflineSyncRetryMs = now;
        if (LittleFS.exists(FS_HISTORY_PATH)) {
            int pending = fsCountOfflineHistory();
            if (pending > 0) {
                Serial.printf("[Sync-Retry] %d sessions pending\n", pending);
                fsSyncOfflineHistoryToFirebase();
            }
        }
    }

    // ── Sensor loop every 5s ─────────────────────────────────
    if (now - lastLoopMs < LOOP_INTERVAL) return;
    float dT = (float)(now - lastLoopMs) / 3600000.0f;
    lastLoopMs = now;

    // Read PZEM
    float voltage   = pzem.voltage();
    float current   = pzem.current();
    float power     = pzem.power();
    float pf        = pzem.pf();
    float frequency = pzem.frequency();
    if (isnan(voltage))   voltage   = 0;
    if (isnan(current))   current   = 0;
    if (isnan(power))     power     = 0;
    if (isnan(pf))        pf        = 0;
    if (isnan(frequency)) frequency = 0;

    deviceConnected = (current > 0.01f && power > 0.5f);

    if (deviceConnected) {
        lastV = voltage; lastI = current; lastP = power;
        lastPF = pf; lastHz = frequency; hadDataOnce = true;
    }

    // Disconnect + overload detection
    handleDeviceDisconnect();
    handleOverload(power);

    // Energy accumulation
    if (deviceConnected && relayOn && !isOverload) {
        sessionEnergyWh += power * dT;
        sessionKwh       = sessionEnergyWh / 1000.0f;
        sessionCost      = sessionKwh * tarif;
    }

    // Checkpoint every 30s
    if (sessionActive && hadDataOnce && (now - lastCheckpointMs >= CHECKPOINT_INTERVAL)) {
        lastCheckpointMs = now;
        fsWriteSession();
    }

    prevDevConn = deviceConnected;

    // Serial log
    Serial.printf("[%s] Relay:%s Dev:%s(%s) V:%.1f I:%.2f P:%.1f E:%.4f Ovl:%s\n",
                  modeOffline ? "OFF" : "ONL",
                  relayOn ? "ON" : "OFF",
                  deviceConnected ? "Y" : "N",
                  sessionDeviceName,
                  voltage, current, power, sessionKwh,
                  isOverload ? "YES" : "no");

    // [5] OLED update
    unsigned long offMs = modeOffline ? (now - offlineStartMs) : 0;
    oledData(voltage, current, power, pf, frequency,
             sessionKwh, sessionCost,
             deviceConnected, wifiConnected,
             isOverload, relayOn, modeOffline, offMs);

    // Send to Firebase (online only)
    if (wifiConnected) {
        unsigned long ts = ntpSynced ? (unsigned long)time(nullptr) : now / 1000;
        sendToFirebase(voltage, current, power, pf, frequency,
                       sessionKwh, sessionCost,
                       deviceConnected, isOverload, relayOn, ts);
    }
}