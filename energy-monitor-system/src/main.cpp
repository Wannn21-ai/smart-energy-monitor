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
#include <LittleFS.h>          // ① LittleFS
#include <ArduinoJson.h>       // ① JSON baca/tulis file

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

const unsigned long LOOP_INTERVAL             = 5000;
const unsigned long RECONNECT_INTERVAL        = 60000;
const unsigned long THRESHOLD_SYNC_INTERVAL   = 30000;
const unsigned long COMMAND_POLL_INTERVAL      = 2000;
const unsigned long OVERLOAD_BLINK_MS          = 200;
const unsigned long OVERLOAD_ALERT_LINGER      = 10000;
const unsigned long CHECKPOINT_INTERVAL        = 30000;  // ① auto-save tiap 30 detik
const unsigned long OFFLINE_SYNC_RETRY_INTERVAL = 15000; // ④ retry sync offline history

const int DISCONNECT_THRESHOLD = 2;

// ① LittleFS paths
#define FS_SESSION_PATH  "/session_active.json"
#define FS_HISTORY_PATH  "/history_offline.json"

// ================================================================
// STATE — SYSTEM & MODE MANAGEMENT
// ================================================================
bool wifiConnected = false;
bool ntpSynced     = false;
bool modeOffline   = false;           // TRUE = offline, FALSE = online
unsigned long offlineStartMs = 0;     // waktu WiFi putus
unsigned long lastModeTransitionMs = 0; // debounce rapid mode transitions
const unsigned long MODE_TRANSITION_DEBOUNCE = 2000; // min 2s antar transition

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
// STATE — ENERGY ACCUMULATION
// ================================================================
float overloadThreshold = THRESHOLD_DEFAULT;
float tarif             = TARIF_DEFAULT;
float sessionEnergyWh   = 0.0f;
float sessionKwh        = 0.0f;
float sessionCost       = 0.0f;
bool  sessionActive     = false;

// ③ Nama device & durasi untuk session recovery
char  sessionDeviceName[32] = "";
unsigned long sessionStartTs = 0;   // Unix timestamp atau millis/1000

float lastV = 0, lastI = 0, lastP = 0, lastPF = 0, lastHz = 0;
bool  hadDataOnce = false;

// ================================================================
// TIMING
// ================================================================
unsigned long lastLoopMs              = 0;
unsigned long lastReconnectMs         = 0;
unsigned long lastThresholdSyncMs     = 0;
unsigned long lastCommandPollMs       = 0;
unsigned long lastCheckpointMs        = 0;   // ① checkpoint timer
unsigned long lastOfflineSyncRetryMs  = 0;   // ④ sync retry timer

unsigned long lastBlinkMs         = 0;
bool          blinkState          = false;
unsigned long lastOverloadBlinkMs = 0;
bool          overloadBlinkState  = false;

// ================================================================
// FORWARD DECLARATIONS
// ================================================================
void loadPrefs(); void savePrefs();

// ① LittleFS helpers
bool   fsInit();
void   fsWriteSession();
void   fsClearSession();
bool   fsReadSession(float &outEnergyWh, float &outKwh, float &outCost,
                     char *outName, unsigned long &outStartTs);
void   fsAppendOfflineHistory(const char* name, unsigned long startTs,
                              unsigned long endTs, float energyKwh,
                              float cost, float avgPower, bool wasOverload);
bool   fsSyncOfflineHistoryToFirebase();
int    fsCountOfflineHistory();

bool tryConnectWiFi(int sec = 20); bool tryNTPSync();
void startLocalAP(); void setupWebServer();
void setRelay(bool on, const char* reason = "");
void handleDeviceDisconnect(); void handleOverload(float power);
bool sendToFirebase(float v, float i, float p, float pf, float freq,
                    float kwh, float cost, bool dev, bool ovl,
                    bool relay, unsigned long ts);
bool pushHistoryToFirebase(const char* name, const char* duration,
                           float avgPower, float energyKwh, float cost,
                           unsigned long ts, bool recovered, bool wasOverload);
void syncThresholdFromFirebase(); void pollCommandFromFirebase();
void clearFirebaseCommand();
void handleBlueLed(); void handleGreenLed(); void handleOverloadAlert();
void checkResetButton();
void oledSplash(); void oledStatus(const char* l1, const char* l2 = "");
void oledData(float v, float i, float p, float pf, float hz, float kwh,
              float cost, bool dev, bool online, bool ovl, bool relay,
              bool offline, unsigned long offMs);
String buildDuration(unsigned long startTs, unsigned long nowTs);

// ================================================================
// ① LittleFS INIT
// ================================================================
bool fsInit() {
  if (!LittleFS.begin(true)) {
    Serial.println("[FS] LittleFS mount gagal — format ulang");
    if (!LittleFS.begin(true)) {
      Serial.println("[FS] Format gagal");
      return false;
    }
  }
  Serial.println("[FS] LittleFS OK");
  return true;
}

// ================================================================
// ① WRITE SESSION CHECKPOINT → session_active.json
// Format: {"name":"...","start":ts,"energy_wh":x,"kwh":x,"cost":x}
// Return: true jika berhasil disimpan
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

  File f = LittleFS.open(FS_SESSION_PATH, "w");
  if (!f) { 
    Serial.println("[FS] ✗ Gagal tulis session (file not opened)");
    return false;
  }
  
  size_t bytesWritten = serializeJson(doc, f);
  f.close();
  
  if (bytesWritten == 0) {
    Serial.println("[FS] ✗ Gagal tulis session (serialize failed)");
    return false;
  }
  
  Serial.printf("[FS] ✓ Checkpoint: %s %.4f kWh\n", sessionDeviceName, sessionKwh);
  return true;
}

// ================================================================
// ① CLEAR SESSION FILE
// ================================================================
void fsClearSession() {
  if (LittleFS.exists(FS_SESSION_PATH)) {
    LittleFS.remove(FS_SESSION_PATH);
    Serial.println("[FS] session_active.json dihapus");
  }
}

// ================================================================
// ① READ SESSION FILE → return true jika ada data valid
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

  if (outKwh <= 0 && outEnergyWh <= 0) {
    LittleFS.remove(FS_SESSION_PATH);
    return false;
  }
  return true;
}

// ================================================================
// ① APPEND SESI SELESAI → history_offline.json
// File berisi JSON array dari sesi-sesi yang perlu di-push ke Firebase
// ================================================================
void fsAppendOfflineHistory(const char* name, unsigned long startTs,
                            unsigned long endTs, float energyKwh,
                            float cost, float avgPower, bool wasOverload) {
  // Baca array yang sudah ada
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

  // Buat entri baru
  JsonObject entry = arr.createNestedObject();
  entry["name"]      = name;
  entry["start_ts"]  = startTs;
  entry["end_ts"]    = endTs;
  entry["kwh"]       = energyKwh;
  entry["cost"]      = cost;
  entry["power"]     = avgPower;
  entry["overload"]  = wasOverload;

  // Tulis kembali
  File f = LittleFS.open(FS_HISTORY_PATH, "w");
  if (!f) { Serial.println("[FS] Gagal tulis offline history"); return; }
  serializeJson(doc, f);
  f.close();
  Serial.printf("[FS] Offline history: '%s' %.4f kWh disimpan\n", name, energyKwh);
}

// ================================================================
// ④ SYNC OFFLINE HISTORY → Firebase, return true jika semua berhasil
// ================================================================
bool fsSyncOfflineHistoryToFirebase() {
  if (!wifiConnected || WiFi.status() != WL_CONNECTED) return false;
  if (!LittleFS.exists(FS_HISTORY_PATH)) return true; // tidak ada yang perlu di-sync

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

  Serial.printf("[Sync] Mulai sync %d sesi offline ke Firebase\n", arr.size());
  oledStatus("Sync offline", "history...");

  int pushed = 0;
  for (JsonObject entry : arr) {
    const char*   name      = entry["name"]     | "Offline Device";
    unsigned long startTs   = entry["start_ts"] | (unsigned long)0;
    unsigned long endTs     = entry["end_ts"]   | (unsigned long)0;
    float         kwh       = entry["kwh"]      | 0.0f;
    float         cost      = entry["cost"]     | 0.0f;
    float         avgPower  = entry["power"]    | 0.0f;
    bool          wasOvl    = entry["overload"] | false;

    // Buat string durasi dari startTs - endTs
    String dur = buildDuration(startTs, endTs);

    bool ok = pushHistoryToFirebase(name, dur.c_str(), avgPower, kwh, cost,
                                    endTs > 0 ? endTs : startTs,
                                    true,   // recovered = true → badge "⚡ Recovered"
                                    wasOvl);
    if (!ok) {
      Serial.printf("[Sync] Push '%s' gagal — henti, coba lagi nanti\n", name);
      break;
    }
    pushed++;
    delay(300); // jeda kecil antar push
  }

  if (pushed == (int)arr.size()) {
    LittleFS.remove(FS_HISTORY_PATH);
    Serial.printf("[Sync] Semua %d sesi offline berhasil di-sync\n", pushed);
    return true;
  }

  // Jika sebagian berhasil, hapus entri yang sudah di-push dari array
  if (pushed > 0) {
    DynamicJsonDocument newDoc(4096);
    JsonArray newArr = newDoc.to<JsonArray>();
    int idx = 0;
    for (JsonObject entry : arr) {
      if (idx >= pushed) {
        newArr.add(entry);
      }
      idx++;
    }
    File fw = LittleFS.open(FS_HISTORY_PATH, "w");
    if (fw) { serializeJson(newDoc, fw); fw.close(); }
    Serial.printf("[Sync] %d/%d sesi berhasil, sisanya disimpan\n", pushed, (int)arr.size());
  }
  return false;
}

// ================================================================
// Count entri history offline
// ================================================================
int fsCountOfflineHistory() {
  if (!LittleFS.exists(FS_HISTORY_PATH)) return 0;
  File f = LittleFS.open(FS_HISTORY_PATH, "r");
  if (!f) return 0;
  DynamicJsonDocument doc(4096);
  if (deserializeJson(doc, f) || !doc.is<JsonArray>()) { f.close(); return 0; }
  f.close();
  return doc.as<JsonArray>().size();
}

// ================================================================
// HELPER — build duration string dari dua Unix timestamp
// ================================================================
String buildDuration(unsigned long startTs, unsigned long endTs) {
  if (endTs <= startTs) return "00:00:00";
  unsigned long secs = endTs - startTs;
  unsigned long h = secs / 3600;
  unsigned long m = (secs % 3600) / 60;
  unsigned long s = secs % 60;
  char buf[16];
  snprintf(buf, sizeof(buf), "%02lu:%02lu:%02lu", h, m, s);
  return String(buf);
}

// ================================================================
// PREFERENCES
// ================================================================
void loadPrefs() {
  prefs.begin("sem", true);
  overloadThreshold = prefs.getFloat("threshold", THRESHOLD_DEFAULT);
  tarif             = prefs.getFloat("tarif",     TARIF_DEFAULT);
  prefs.end();
}
void savePrefs() {
  prefs.begin("sem", false);
  prefs.putFloat("threshold", overloadThreshold);
  prefs.putFloat("tarif",     tarif);
  prefs.end();
}

// ================================================================
// RELAY
// ================================================================
void setRelay(bool on, const char* reason) {
  relayOn = on;
  digitalWrite(PIN_RELAY, on ? RELAY_ON : RELAY_OFF);
  if (!on) {
    sessionActive = false;
    disconnectCount = 0;
    // Hapus session_active.json saat relay OFF (sesi selesai)
    fsClearSession();
  } else {
    sessionActive = true;
  }
  Serial.printf("[Relay] %s — %s\n", on ? "ON" : "OFF", reason);
}

// ================================================================
// ONLINE/OFFLINE MODE MANAGEMENT
// ================================================================
// Fungsi untuk transition ke ONLINE MODE
// - Relay OFF (menunggu command dari web)
// - Siap untuk sync history offline
// - Protected dengan debounce untuk avoid rapid mode flapping
void transitionToOnlineMode() {
  if (modeOffline == false) return; // Sudah online
  
  // Debounce: prevent rapid mode transitions
  unsigned long now = millis();
  if (now - lastModeTransitionMs < MODE_TRANSITION_DEBOUNCE) {
    Serial.printf("[Mode] ⚠ Rapid transition ignored (debounce)\n");
    return;
  }
  lastModeTransitionMs = now;
  
  modeOffline = false;
  wifiConnected = true;
  offlineStartMs = 0;
  
  Serial.println("[Mode] → ONLINE (WiFi tersedia)");
  Serial.println("[Mode-Online] Relay OFF — menunggu command web");
  Serial.println("[Mode-Online] Sync offline history...");
  
  // Relay OFF untuk online mode (menunggu command web)
  // Tapi cek dulu apakah session aktif perlu disimpan
  if (relayOn && sessionActive && hadDataOnce) {
    fsWriteSession();
    Serial.println("[Mode-Online] Checkpoint sesi sebelum switch mode");
  }
  
  // Clear command jika ada
  clearFirebaseCommand();
  
  // Sync offline history jika ada
  fsSyncOfflineHistoryToFirebase();
}

// Fungsi untuk transition ke OFFLINE MODE
// - Relay ON otomatis
// - Auto-monitor device
// - Protected dengan debounce
void transitionToOfflineMode() {
  if (modeOffline == true) return; // Sudah offline
  
  // Debounce: prevent rapid mode transitions
  unsigned long now = millis();
  if (now - lastModeTransitionMs < MODE_TRANSITION_DEBOUNCE) {
    Serial.printf("[Mode] ⚠ Rapid transition ignored (debounce)\n");
    return;
  }
  lastModeTransitionMs = now;
  
  modeOffline = true;
  wifiConnected = false;
  offlineStartMs = now;
  
  Serial.println("[Mode] → OFFLINE (WiFi tidak tersedia / terputus)");
  
  // Jika relay belum ON, nyalakan untuk offline mode
  if (!relayOn) {
    if (strlen(sessionDeviceName) == 0)
      generateOfflineDeviceName();
    sessionStartTs = now / 1000;
    setRelay(true, "mode offline");
    Serial.printf("[Mode-Offline] Relay ON — device: %s\n", sessionDeviceName);
  } else {
    // Relay sudah ON, generate nama device baru jika belum ada
    if (strlen(sessionDeviceName) == 0)
      generateOfflineDeviceName();
    Serial.printf("[Mode-Offline] Relay tetap ON — device: %s\n", sessionDeviceName);
  }
}

// ================================================================
// DEVICE DISCONNECT
// ================================================================
void handleDeviceDisconnect() {
  if (!relayOn || !sessionActive) return;
  if (!deviceConnected && prevDevConn) {
    disconnectCount++;
    Serial.printf("[Disconnect] Attempt %d/%d\n", disconnectCount, DISCONNECT_THRESHOLD);
    if (disconnectCount >= DISCONNECT_THRESHOLD) {
      Serial.println("[Disconnect] ✓ Device dicabut — relay OFF, simpan sesi");

      // Simpan sesi ke history
      unsigned long nowTs = ntpSynced ? (unsigned long)time(nullptr) : millis()/1000;
      bool historyOk = false;
      
      if (wifiConnected) {
        String dur = buildDuration(sessionStartTs, nowTs);
        historyOk = pushHistoryToFirebase(
          sessionDeviceName, dur.c_str(),
          lastP, sessionKwh, sessionCost,
          nowTs, false, false
        );
        sendToFirebase(0, 0, 0, 0, 0, sessionKwh, sessionCost, false, false, true, nowTs);
        if (historyOk) Serial.println("[Disconnect] ✓ History pushed to Firebase");
        else Serial.println("[Disconnect] ✗ History push failed");
      } else {
        // Simpan ke LittleFS untuk di-sync nanti
        fsAppendOfflineHistory(
          sessionDeviceName, sessionStartTs, nowTs,
          sessionKwh, sessionCost, lastP, false
        );
        Serial.println("[Disconnect] ✓ History saved to offline queue");
      }

      setRelay(false, "device dicabut");
      sessionEnergyWh = 0; sessionKwh = 0; sessionCost = 0;
      hadDataOnce = false; sessionDeviceName[0] = '\0';
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
    Serial.printf("[Overload] ⚠ ALERT: %.1fW >= %.0fW — relay OFF\n", power, overloadThreshold);

    // Simpan sesi ke history dengan tag overload
    unsigned long nowTs = ntpSynced ? (unsigned long)time(nullptr) : millis()/1000;
    bool historyOk = false;
    
    if (wifiConnected) {
      String dur = buildDuration(sessionStartTs, nowTs);
      historyOk = pushHistoryToFirebase(
        sessionDeviceName, dur.c_str(),
        power, sessionKwh, sessionCost,
        nowTs, false, true   // wasOverload = true
      );
      if (historyOk) Serial.println("[Overload] ✓ Overload history pushed");
      else Serial.println("[Overload] ✗ Overload history push failed");
    } else {
      fsAppendOfflineHistory(
        sessionDeviceName, sessionStartTs, nowTs,
        sessionKwh, sessionCost, power, true
      );
      Serial.println("[Overload] ✓ Overload history saved to offline queue");
    }

    setRelay(false, "overload");
    overloadAlertLinger = true;
    overloadLingerStart = millis();
    sessionEnergyWh = 0; sessionKwh = 0; sessionCost = 0;
    sessionDeviceName[0] = '\0';
  } else if (!newOvl && isOverload) {
    isOverload = false;
    Serial.println("[Overload] ✓ Teratasi");
  }
}

// ================================================================
// FIREBASE — Push satu sesi ke /users/... history
// Key unik = timestamp (ms). Badge "⚡ Recovered" jika recovered=true.
// Badge overload jika wasOverload=true (ditandai di field isOverload).
// ================================================================
bool pushHistoryToFirebase(const char* name, const char* duration,
                           float avgPower, float energyKwh, float cost,
                           unsigned long ts, bool recovered, bool wasOverload) {
  if (!wifiConnected || WiFi.status() != WL_CONNECTED) return false;

  // Format tanggal dari timestamp
  char dateStr[20] = "";
  if (ntpSynced && ts > 1000000) {
    struct tm ti;
    time_t t = (time_t)ts;
    localtime_r(&t, &ti);
    snprintf(dateStr, sizeof(dateStr), "%02d/%02d/%04d",
             ti.tm_mday, ti.tm_mon + 1, ti.tm_year + 1900);
  } else {
    snprintf(dateStr, sizeof(dateStr), "—");
  }

  // Format cost
  char costStr[32];
  if (cost >= 1000)
    snprintf(costStr, sizeof(costStr), "Rp %lu", (unsigned long)cost);
  else
    snprintf(costStr, sizeof(costStr), "Rp %.1f", cost);

  // Buat nama dengan badge jika perlu
  char displayName[64];
  if (recovered && wasOverload)
    snprintf(displayName, sizeof(displayName), "%s ⚡ Recovered ⚠", name);
  else if (recovered)
    snprintf(displayName, sizeof(displayName), "%s ⚡ Recovered", name);
  else
    strlcpy(displayName, name, sizeof(displayName));

  // Path: /users/global_history/<ts_ms>.json
  // Catatan: karena tidak ada uid di firmware, pakai /shared_history/
  // Web harus baca dari path ini untuk history global
  // (atau bisa juga kirim ke /live dan web yang push ke per-user history)
  // Untuk saat ini kirim ke /shared_history/<ts>
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
    recovered   ? "true" : "false"
  );

  int code = h.PUT(body);
  h.end();
  Serial.printf("[History] Push '%s' → %d\n", displayName, code);
  return (code == 200 || code == 204);
}

// ================================================================
// FIREBASE — Send live data
// ================================================================
void clearFirebaseCommand() {
  WiFiClientSecure c; c.setInsecure(); c.setTimeout(5000);
  HTTPClient h;
  if (h.begin(c, String(FIREBASE_HOST) + "/command/relay.json")) {
    h.addHeader("Content-Type","application/json"); h.PUT("null"); h.end();
  }
}

void pollCommandFromFirebase() {
  if (!wifiConnected || WiFi.status() != WL_CONNECTED) return;
  WiFiClientSecure c; c.setInsecure(); c.setTimeout(5000);
  HTTPClient h;
  if (!h.begin(c, String(FIREBASE_HOST) + "/command/relay.json")) {
    Serial.println("[Command] ✗ Gagal koneksi Firebase");
    return;
  }
  
  int code = h.GET();
  if (code == 200) {
    String pl = h.getString(); pl.trim(); h.end();
    if (pl == "true" && !relayOn) {
      // Web command: turn relay ON (untuk online mode)
      sessionEnergyWh=0; sessionKwh=0; sessionCost=0;
      hadDataOnce=false; disconnectCount=0; isOverload=false;
      overloadAlertLinger=false;
      // ③ Offline mode: nama otomatis dari web command juga pakai generate
      if (strlen(sessionDeviceName) == 0)
        strlcpy(sessionDeviceName, "Device", sizeof(sessionDeviceName));
      setRelay(true, "perintah web");
      unsigned long nowTs = ntpSynced ? (unsigned long)time(nullptr) : millis()/1000;
      sessionStartTs = nowTs;
      clearFirebaseCommand();
      Serial.println("[Command] ✓ Relay ON (from web)");
    } else if (pl == "false" && relayOn) {
      // Web command: turn relay OFF (stop session)
      unsigned long nowTs = ntpSynced ? (unsigned long)time(nullptr) : millis()/1000;
      if (sessionActive && sessionKwh > 0) {
        String dur = buildDuration(sessionStartTs, nowTs);
        pushHistoryToFirebase(
          sessionDeviceName, dur.c_str(),
          lastP, sessionKwh, sessionCost,
          nowTs, false, false
        );
      }
      setRelay(false, "stop session web");
      sessionEnergyWh=0; sessionKwh=0; sessionCost=0;
      hadDataOnce=false; prevDevConn=false; disconnectCount=0;
      sessionDeviceName[0] = '\0';
      clearFirebaseCommand();
      Serial.println("[Command] ✓ Relay OFF (from web)");
    }
  } else if (code != 0) {
    Serial.printf("[Command] ✗ Firebase error: %d\n", code);
    h.end();
  } else {
    h.end();
  }
}

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
        overloadThreshold = v; savePrefs();
        Serial.printf("[Threshold] %.0f W\n", v);
      }
    }
  }
  h.end();
}

bool sendToFirebase(float v, float i, float p, float pf, float hz,
                    float kwh, float cost, bool dev, bool ovl,
                    bool relay, unsigned long ts) {
  if (!wifiConnected || WiFi.status() != WL_CONNECTED) return false;
  WiFiClientSecure c; c.setInsecure(); c.setTimeout(10000);
  HTTPClient h;
  h.begin(c, String(FIREBASE_HOST) + FIREBASE_PATH);
  h.addHeader("Content-Type","application/json");
  String j = "{";
  j += "\"system\":{\"timestamp\":" + String(ts) + ",\"internet\":true";
  j += ",\"threshold\":" + String(overloadThreshold,0);
  j += ",\"tarif\":"     + String(tarif,2);
  j += ",\"relay\":"     + String(relay ? "true":"false");
  j += ",\"offline\":"   + String(modeOffline ? "true":"false");
  j += ",\"deviceName\":\"" + String(sessionDeviceName) + "\"},";
  j += "\"connected\":"  + String(dev  ? "true":"false") + ",";
  j += "\"overload\":"   + String(ovl  ? "true":"false") + ",";
  j += "\"device\":{";
  j += "\"voltage\":"    + String(v,1)   + ",";
  j += "\"current\":"    + String(i,2)   + ",";
  j += "\"power\":"      + String(p,1)   + ",";
  j += "\"apparent\":"   + String(v*i,1) + ",";
  j += "\"pf\":"         + String(pf,2)  + ",";
  j += "\"frequency\":"  + String(hz,1)  + ",";
  j += "\"energy\":"     + String(kwh,4) + ",";
  j += "\"cost\":"       + String(cost,0)+ ",";
  j += "\"overload\":"   + String(ovl ? "true":"false") + "}}";
  int code = h.PUT(j); h.end();
  Serial.printf("[FB] %d P=%.1fW E=%.4fkWh Dev=%s\n", code, p, kwh, sessionDeviceName);
  return (code == 200 || code == 204);
}

// ================================================================
// ② SESSION RECOVERY — dipanggil di setup() sebelum monitoring dimulai
// Jika ada session_active.json:
//   - Push ke Firebase History dengan badge "⚡ Recovered"
//   - Hapus file
//   - Fresh start
// ================================================================
void doSessionRecovery() {
  float recoveredEnergyWh = 0, recoveredKwh = 0, recoveredCost = 0;
  char  recoveredName[32] = "";
  unsigned long recoveredStartTs = 0;

  if (!fsReadSession(recoveredEnergyWh, recoveredKwh, recoveredCost,
                     recoveredName, recoveredStartTs)) {
    return; // tidak ada sesi yang perlu di-recover
  }

  Serial.printf("[Recovery] Ditemukan sesi: '%s' %.4f kWh\n",
                recoveredName, recoveredKwh);
  oledStatus("Recovery sesi", recoveredName);
  delay(1500);

  unsigned long nowTs = ntpSynced ? (unsigned long)time(nullptr) : millis()/1000;
  String dur = buildDuration(recoveredStartTs, nowTs);

  bool pushed = false;
  if (wifiConnected) {
    pushed = pushHistoryToFirebase(
      recoveredName, dur.c_str(),
      0.0f,             // avgPower tidak tahu (tidak disimpan di checkpoint sederhana)
      recoveredKwh, recoveredCost,
      nowTs, true, false
    );
    if (pushed) {
      Serial.println("[Recovery] Push ke Firebase OK");
    } else {
      Serial.println("[Recovery] Push gagal — simpan ke offline history");
      fsAppendOfflineHistory(
        recoveredName, recoveredStartTs, nowTs,
        recoveredKwh, recoveredCost, 0.0f, false
      );
    }
  } else {
    // Belum online — simpan ke history offline, nanti di-sync
    fsAppendOfflineHistory(
      recoveredName, recoveredStartTs, nowTs,
      recoveredKwh, recoveredCost, 0.0f, false
    );
    Serial.println("[Recovery] Simpan ke offline history (belum online)");
  }

  fsClearSession();

  oledStatus("Recovery selesai", pushed ? "Tersimpan ✓" : "Antri sync");
  delay(1000);
}

// ================================================================
// ③ GENERATE OFFLINE DEVICE NAME — "Offline Device N"
// ================================================================
void generateOfflineDeviceName() {
  // Guard: jangan generate jika sudah ada nama yang valid
  if (strlen(sessionDeviceName) > 0) {
    return;
  }
  
  // Cek berapa sesi offline yang sudah ada untuk buat nama unik
  int count = fsCountOfflineHistory();
  snprintf(sessionDeviceName, sizeof(sessionDeviceName),
           "Offline Device %d", count + 1);
  Serial.printf("[Offline] Nama device: %s\n", sessionDeviceName);
}

// ================================================================
// LOCAL AP
// ================================================================
void startLocalAP() {
  IPAddress ip(192,168,4,1), sub(255,255,255,0);
  WiFi.softAPConfig(ip, ip, sub);
  WiFi.softAP(AP_SSID, AP_PASS);
  dnsServer.stop();
  dnsServer.start(DNS_PORT, "*", ip);
  Serial.printf("[AP] '%s' aktif — 192.168.4.1\n", AP_SSID);
}

// ================================================================
// LOCAL WEBSERVER
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

  // Hitung jumlah sesi offline yang antri
  int pendingSync = fsCountOfflineHistory();
  String syncInfo = "";
  if (pendingSync > 0) {
    syncInfo = "<div style='background:rgba(255,171,0,0.1);border:1px solid rgba(255,171,0,0.3);"
               "border-radius:10px;padding:10px 14px;font-size:12px;color:#ffab00;margin-bottom:14px;'>"
               "⚡ " + String(pendingSync) + " sesi offline menunggu sync ke Firebase</div>";
  }

  String html = "<!DOCTYPE html><html lang='id'><head>"
    "<meta charset='UTF-8'/>"
    "<meta name='viewport' content='width=device-width,initial-scale=1,maximum-scale=1'/>"
    "<title>Smart Energy Monitor · Setup</title>"
    "<style>"
    "*{margin:0;padding:0;box-sizing:border-box}"
    "body{background:#0a0a0a;color:#f0f0f0;"
      "font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif;"
      "min-height:100vh;display:flex;flex-direction:column;"
      "align-items:center;justify-content:flex-start;padding:24px 16px}"
    ".hdr{width:100%;max-width:420px;display:flex;align-items:center;gap:14px;"
      "margin-bottom:28px}"
    ".hdr-icon{width:44px;height:44px;flex-shrink:0;"
      "background:rgba(0,229,255,0.08);border:1.5px solid #00e5ff;"
      "border-radius:12px;display:flex;align-items:center;"
      "justify-content:center;font-size:22px}"
    ".hdr-title{font-size:17px;font-weight:700;color:#00e5ff;letter-spacing:.04em}"
    ".hdr-sub{font-size:12px;color:#444;margin-top:3px}"
    ".steps{width:100%;max-width:420px;display:flex;"
      "align-items:center;margin-bottom:28px;gap:0}"
    ".step{display:flex;flex-direction:column;align-items:center;flex:1}"
    ".step-dot{width:28px;height:28px;border-radius:50%;"
      "display:flex;align-items:center;justify-content:center;"
      "font-size:11px;font-weight:700;transition:all .3s}"
    ".step-dot.done{background:#00e5ff;color:#000}"
    ".step-dot.active{background:rgba(0,229,255,0.15);"
      "border:1.5px solid #00e5ff;color:#00e5ff}"
    ".step-dot.pending{background:#1a1a1a;border:1px solid #2a2a2a;color:#444}"
    ".step-label{font-size:9px;color:#444;margin-top:4px;"
      "text-transform:uppercase;letter-spacing:.06em;text-align:center}"
    ".step-line{flex:1;height:1px;background:#1a1a1a;margin-top:-14px}"
    ".card{background:#111;border:1px solid rgba(255,255,255,0.07);"
      "border-radius:18px;padding:22px;width:100%;max-width:420px;"
      "margin-bottom:14px}"
    ".card-title{font-size:10px;font-weight:700;color:#444;"
      "letter-spacing:.1em;text-transform:uppercase;"
      "margin-bottom:18px;padding-bottom:12px;"
      "border-bottom:1px solid rgba(255,255,255,0.04);"
      "display:flex;align-items:center;gap:8px}"
    ".fg{margin-bottom:18px}"
    ".fg:last-of-type{margin-bottom:0}"
    "label{display:block;font-size:11px;font-weight:600;color:#666;"
      "text-transform:uppercase;letter-spacing:.06em;margin-bottom:7px}"
    ".hint{font-size:11px;color:#333;margin-top:5px;line-height:1.6}"
    "select,input[type=text],input[type=password],input[type=number]{"
      "width:100%;padding:12px 14px;"
      "background:#1a1a1a;border:1px solid rgba(255,255,255,0.08);"
      "border-radius:10px;color:#f0f0f0;font-size:15px;outline:none;"
      "-webkit-appearance:none;appearance:none;"
      "transition:border-color .2s,box-shadow .2s}"
    "select:focus,input:focus{"
      "border-color:#00e5ff;box-shadow:0 0 0 3px rgba(0,229,255,0.08)}"
    "select option{background:#1a1a1a;color:#f0f0f0}"
    "input::placeholder{color:#2a2a2a}"
    ".input-row{display:flex;gap:10px;align-items:flex-end}"
    ".input-row input{flex:1}"
    ".input-unit{font-size:13px;color:#555;padding-bottom:14px;white-space:nowrap}"
    ".converted{font-size:12px;color:#555;margin-top:6px;min-height:18px;transition:color .2s}"
    ".converted.active{color:#00e5ff}"
    ".btn{display:block;width:100%;padding:14px;"
      "border:none;border-radius:12px;"
      "font-size:15px;font-weight:700;cursor:pointer;"
      "transition:opacity .15s,transform .1s;letter-spacing:.02em}"
    ".btn:active{opacity:.8;transform:scale(.98)}"
    ".btn-primary{background:#00e5ff;color:#000}"
    ".btn-ghost{background:transparent;color:#555;"
      "border:1px solid rgba(255,255,255,0.08);margin-top:10px}"
    ".btn:disabled{opacity:.4;cursor:not-allowed}"
    "#toast{position:fixed;bottom:24px;left:50%;"
      "transform:translateX(-50%) translateY(80px);opacity:0;"
      "background:#1a1a1a;border:1px solid rgba(255,255,255,0.1);"
      "color:#f0f0f0;padding:12px 20px;border-radius:12px;"
      "font-size:13px;font-weight:500;white-space:nowrap;"
      "transition:transform .3s ease,opacity .3s ease;z-index:999}"
    "#toast.show{transform:translateX(-50%) translateY(0);opacity:1}"
    "#toast.ok{border-color:rgba(0,230,118,.4);color:#00e676}"
    "#toast.err{border-color:rgba(255,23,68,.4);color:#ff1744}"
    "#toast.info{border-color:rgba(0,229,255,.4);color:#00e5ff}"
    ".spin{display:inline-block;width:16px;height:16px;"
      "border:2px solid rgba(0,0,0,.2);border-top-color:#000;"
      "border-radius:50%;animation:spin .7s linear infinite;vertical-align:middle}"
    "@keyframes spin{to{transform:rotate(360deg)}}"
    ".footer{width:100%;max-width:420px;text-align:center;"
      "font-size:11px;color:#2a2a2a;margin-top:16px;line-height:2}"
    ".chip{display:inline-flex;align-items:center;gap:6px;"
      "background:#1a1a1a;border:1px solid rgba(255,255,255,0.06);"
      "border-radius:20px;padding:4px 12px;font-size:11px;color:#555;"
      "font-family:monospace;margin-top:6px}"
    ".chip span{color:#00e5ff}"
    "</style></head><body>";

  html += "<div class='hdr'>"
    "<div class='hdr-icon'>⚡</div>"
    "<div>"
      "<div class='hdr-title'>Smart Energy Monitor</div>"
      "<div class='hdr-sub'>Konfigurasi perangkat · SEM v3.0</div>"
    "</div>"
    "</div>";

  html += "<div class='steps'>"
    "<div class='step'><div class='step-dot active'>1</div>"
      "<div class='step-label'>WiFi</div></div>"
    "<div class='step-line'></div>"
    "<div class='step'><div class='step-dot pending'>2</div>"
      "<div class='step-label'>Tarif</div></div>"
    "<div class='step-line'></div>"
    "<div class='step'><div class='step-dot pending'>3</div>"
      "<div class='step-label'>Selesai</div></div>"
    "</div>";

  html += "<div class='card' id='card-wifi'>"
    "<div class='card-title'>📶 &nbsp;Koneksi WiFi</div>"
    + syncInfo +
    "<div class='fg'>"
      "<label>Pilih Jaringan WiFi</label>"
      "<select id='ssid'>" + ssidOptions + "</select>"
      "<div class='hint'>Tidak ada? &nbsp;"
        "<a href='#' onclick='manualSSID()' "
          "style='color:#00e5ff;text-decoration:none;'>Ketik manual ↓</a></div>"
    "</div>"
    "<div class='fg' id='manual-wrap' style='display:none'>"
      "<label>Nama WiFi (Manual)</label>"
      "<input type='text' id='ssid-manual' placeholder='Ketik nama WiFi' "
        "autocomplete='off' spellcheck='false'/>"
    "</div>"
    "<div class='fg'>"
      "<label>Password WiFi</label>"
      "<input type='password' id='wpass' placeholder='••••••••' "
        "autocomplete='new-password'/>"
    "</div>"
    "<button class='btn btn-primary' id='btn-wifi' onclick='step1()'>Hubungkan &rarr;</button>"
    "</div>";

  html += "<div class='card' id='card-settings' style='display:none'>"
    "<div class='card-title'>⚙ &nbsp;Pengaturan Energi</div>"
    "<div class='fg'>"
      "<label>Tarif per kWh</label>"
      "<div class='input-row'>"
        "<input type='number' id='trf' min='0' step='0.01' "
          "placeholder='1444.70' value='" + String(tarif, 2) + "'/>"
        "<span class='input-unit'>IDR</span>"
      "</div>"
      "<div class='converted' id='trf-preview'></div>"
      "<div class='chip'>Tersimpan: <span>" + String(tarif, 2) + " IDR/kWh</span></div>"
    "</div>"
    "<div class='fg'>"
      "<label>Batas Overload</label>"
      "<div class='input-row'>"
        "<input type='number' id='thr' min='100' max='10000' step='100' "
          "placeholder='2000' value='" + String(overloadThreshold, 0) + "'/>"
        "<span class='input-unit'>Watt</span>"
      "</div>"
      "<div class='hint'>Relay mati otomatis jika daya perangkat melebihi nilai ini.</div>"
      "<div class='chip'>Tersimpan: <span>" + String(overloadThreshold, 0) + " W</span></div>"
    "</div>"
    "<button class='btn btn-primary' id='btn-save' onclick='step2()'>"
      "Simpan &amp; Selesai ✓"
    "</button>"
    "<button class='btn btn-ghost' onclick='skipSettings()'>"
      "Lewati (gunakan nilai sekarang)"
    "</button>"
    "</div>";

  html += "<div class='card' id='card-done' style='display:none;text-align:center'>"
    "<div style='font-size:48px;margin-bottom:16px'>✅</div>"
    "<div style='font-size:16px;font-weight:700;color:#00e676;margin-bottom:8px'>"
      "Konfigurasi Selesai!</div>"
    "<div style='font-size:13px;color:#555;line-height:1.8;margin-bottom:20px'>"
      "ESP32 berhasil terhubung ke WiFi.<br>"
      "Pengaturan tarif &amp; threshold tersimpan.<br>"
      "Dashboard web kini aktif.</div>"
    "<div id='done-ip' style='font-family:monospace;font-size:13px;"
      "background:#1a1a1a;border:1px solid rgba(0,229,255,.2);"
      "border-radius:8px;padding:12px;color:#00e5ff;margin-bottom:20px'>"
      "Menghubungkan...</div>"
    "<button class='btn btn-ghost' onclick='resetWifi()' "
      "style='max-width:200px;margin:0 auto'>"
      "Reset &amp; Ulangi</button>"
    "</div>";

  html += "<div class='footer'>"
    "SEM-Config &nbsp;·&nbsp; 192.168.4.1<br>"
    "Hubungkan ke WiFi <b>SEM-Config</b> &nbsp;·&nbsp; pw: <b>12345678</b>"
    "</div>";

  html += "<div id='toast'></div>";

  html += R"JS(<script>
var _tt, wifiOk = false;
function toast(msg, type) {
  var el = document.getElementById('toast');
  el.textContent = msg;
  el.className = type || '';
  el.classList.add('show');
  clearTimeout(_tt);
  _tt = setTimeout(function() { el.classList.remove('show'); }, 4000);
}
var USD_RATE = 16500;
document.getElementById('trf').addEventListener('input', function() {
  var v = parseFloat(this.value);
  var el = document.getElementById('trf-preview');
  if (!isNaN(v) && v > 0) {
    el.textContent = '≈ $ ' + (v / USD_RATE).toFixed(4) + ' / kWh';
    el.className = 'converted active';
  } else {
    el.textContent = '';
    el.className = 'converted';
  }
});
function manualSSID() {
  var w = document.getElementById('manual-wrap');
  w.style.display = w.style.display === 'none' ? 'block' : 'none';
  if (w.style.display !== 'none') document.getElementById('ssid-manual').focus();
}
function step1() {
  var ssidSel = document.getElementById('ssid').value;
  var ssidMan = document.getElementById('ssid-manual').value.trim();
  var ssid = (document.getElementById('manual-wrap').style.display !== 'none' && ssidMan)
    ? ssidMan : ssidSel;
  var pass = document.getElementById('wpass').value;
  if (!ssid) { toast('Pilih atau ketik nama WiFi terlebih dahulu', 'err'); return; }
  var btn = document.getElementById('btn-wifi');
  btn.disabled = true;
  btn.innerHTML = '<span class="spin"></span>  Menghubungkan...';
  toast('Mencoba terhubung ke "' + ssid + '"...', 'info');
  document.querySelectorAll('.step-dot')[0].className = 'step-dot done';
  document.querySelectorAll('.step-dot')[0].textContent = '✓';
  document.querySelectorAll('.step-dot')[1].className = 'step-dot active';
  fetch('/connectwifi?ssid=' + encodeURIComponent(ssid) + '&pass=' + encodeURIComponent(pass))
    .then(function(r) { return r.text(); })
    .then(function(t) {
      if (t.indexOf('Berhasil') >= 0) {
        wifiOk = true;
        toast('✓ ' + t, 'ok');
        document.getElementById('card-settings').style.display = 'block';
        document.getElementById('card-settings').scrollIntoView({ behavior: 'smooth' });
        btn.innerHTML = '✓ Terhubung';
        btn.style.background = 'rgba(0,230,118,0.15)';
        btn.style.color = '#00e676';
        btn.style.border = '1px solid rgba(0,230,118,0.3)';
      } else {
        toast('✗ ' + t, 'err');
        btn.disabled = false;
        btn.innerHTML = 'Coba Lagi';
        document.querySelectorAll('.step-dot')[0].className = 'step-dot active';
        document.querySelectorAll('.step-dot')[0].textContent = '1';
        document.querySelectorAll('.step-dot')[1].className = 'step-dot pending';
      }
    })
    .catch(function() {
      toast('Tidak ada respons — ESP32 sedang mencoba connect', 'err');
      btn.disabled = false;
      btn.innerHTML = 'Coba Lagi';
      document.querySelectorAll('.step-dot')[0].className = 'step-dot active';
      document.querySelectorAll('.step-dot')[0].textContent = '1';
      document.querySelectorAll('.step-dot')[1].className = 'step-dot pending';
    });
}
function step2() {
  var thr = parseFloat(document.getElementById('thr').value);
  var trf = parseFloat(document.getElementById('trf').value);
  if (isNaN(thr) || thr < 0 || thr > 10000) {
    toast('Threshold harus antara 0–10000 Watt', 'err'); return;
  }
  if (isNaN(trf) || trf <= 0) {
    toast('Masukkan tarif yang valid', 'err'); return;
  }
  var btn = document.getElementById('btn-save');
  btn.disabled = true;
  btn.innerHTML = '<span class="spin"></span>  Menyimpan...';
  fetch('/save?thr=' + thr + '&trf=' + trf)
    .then(function(r) { return r.text(); })
    .then(function(t) {
      if (t.indexOf('Tersimpan') >= 0) { showDone(); }
      else {
        toast('Gagal menyimpan: ' + t, 'err');
        btn.disabled = false;
        btn.innerHTML = 'Simpan & Selesai ✓';
      }
    })
    .catch(function() {
      toast('Gagal — coba lagi', 'err');
      btn.disabled = false;
      btn.innerHTML = 'Simpan & Selesai ✓';
    });
}
function skipSettings() { showDone(); }
function showDone() {
  document.getElementById('card-settings').style.display = 'none';
  document.getElementById('card-done').style.display = 'block';
  document.querySelectorAll('.step-dot')[1].className = 'step-dot done';
  document.querySelectorAll('.step-dot')[1].textContent = '✓';
  document.querySelectorAll('.step-dot')[2].className = 'step-dot done';
  document.querySelectorAll('.step-dot')[2].textContent = '✓';
  document.getElementById('card-done').scrollIntoView({ behavior: 'smooth' });
  setTimeout(function() {
    fetch('/status').then(function(r) { return r.json(); })
      .then(function(d) {
        document.getElementById('done-ip').innerHTML =
          'IP: <b>' + (d.ip || '—') + '</b><br>' +
          'Dashboard: <a href="https://gridwatch-system.vercel.app/" ' +
          'style="color:#00e5ff">gridwatch-system.vercel.app</a>';
      }).catch(function() {
        document.getElementById('done-ip').textContent = 'Konfigurasi tersimpan ✓';
      });
  }, 1500);
}
function resetWifi() {
  if (!confirm('Reset semua konfigurasi WiFi dan mulai ulang?')) return;
  fetch('/resetwifi').then(function() { toast('Mereset... ESP32 akan restart', 'info'); });
}
document.addEventListener('keydown', function(e) {
  if (e.key !== 'Enter') return;
  if (!wifiOk) step1(); else step2();
});
</script>)JS";

  html += "</body></html>";
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
        h.addHeader("Content-Type","application/json");
        h.PUT(String(overloadThreshold,0));
        h.end();
      }
    }
    Serial.printf("[LocalWeb] Saved: thr=%.0fW trf=%.2f\n", overloadThreshold, tarif);
    localServer.send(200, "text/plain",
      "✓ Tersimpan! Threshold=" + String(overloadThreshold,0) +
      "W | Tarif=Rp" + String(tarif,2));
  } else {
    localServer.send(400, "text/plain", "Nilai tidak valid");
  }
}

void handleConnectWifi() {
  if (!localServer.hasArg("ssid")) {
    localServer.send(400, "text/plain", "SSID diperlukan");
    return;
  }
  String ssid = localServer.arg("ssid");
  String pass = localServer.hasArg("pass") ? localServer.arg("pass") : "";

  Serial.printf("[LocalWeb] Coba connect WiFi: %s\n", ssid.c_str());
  WiFi.begin(ssid.c_str(), pass.c_str());

  int waited = 0;
  while (WiFi.status() != WL_CONNECTED && waited < 20) {
    delay(500); waited++;
    localServer.handleClient();
  }

  if (WiFi.status() == WL_CONNECTED) {
    wifiConnected = true;
    modeOffline   = false;
    digitalWrite(PIN_LED_BLUE, HIGH);
    String ip = WiFi.localIP().toString();
    Serial.println("[LocalWeb] WiFi OK: " + ip);
    if (!ntpSynced) ntpSynced = tryNTPSync();
    syncThresholdFromFirebase();
    clearFirebaseCommand();
    // ④ Coba sync offline history yang antri
    fsSyncOfflineHistoryToFirebase();
    localServer.send(200, "text/plain",
      "✓ Berhasil! IP: " + ip + " — Dashboard web aktif kembali.");
  } else {
    Serial.println("[LocalWeb] Gagal connect WiFi");
    localServer.send(200, "text/plain",
      "✗ Gagal terhubung ke \"" + ssid + "\". Cek SSID & password, lalu coba lagi.");
  }
}

void handleResetWifi() {
  localServer.send(200, "text/plain", "Mereset konfigurasi WiFi — ESP32 restart...");
  delay(500);
  WiFi.disconnect(true, true);
  delay(500);
  ESP.restart();
}

void handleStatus() {
  String ip = wifiConnected ? WiFi.localIP().toString() : "—";
  int pending = fsCountOfflineHistory();
  String json = "{\"wifi\":" + String(wifiConnected ? "true":"false") +
                ",\"ip\":\"" + ip + "\""
                ",\"threshold\":" + String(overloadThreshold,0) +
                ",\"tarif\":" + String(tarif,2) +
                ",\"pendingSync\":" + String(pending) + "}";
  localServer.send(200, "application/json", json);
}

// Captive portal handlers
void handleAppleCNA() {
  localServer.send(200, "text/html",
    "<HTML><HEAD><TITLE>Success</TITLE></HEAD><BODY>Success</BODY></HTML>");
}
void handleAndroid204()       { localServer.send(204, "text/plain", ""); }
void handleWindowsNCsi()      { localServer.send(200, "text/plain", "Microsoft NCSI"); }
void handleWindowsConnectTest(){ localServer.send(200, "text/plain", "Microsoft Connect Test"); }
void handleCaptivePortal() {
  localServer.sendHeader("Location", "http://192.168.4.1/", true);
  localServer.send(302, "text/plain", "");
}

void setupWebServer() {
  localServer.on("/",                HTTP_GET, handleRoot);
  localServer.on("/index.html",      HTTP_GET, handleRoot);
  localServer.on("/hotspot-detect.html", HTTP_GET, []() {
    if (wifiConnected) handleAppleCNA(); else handleCaptivePortal();
  });
  localServer.on("/library/test/success.html", HTTP_GET, []() {
    if (wifiConnected) handleAppleCNA(); else handleCaptivePortal();
  });
  localServer.on("/success.txt", HTTP_GET, []() {
    if (wifiConnected) localServer.send(200, "text/plain", "success");
    else handleCaptivePortal();
  });
  localServer.on("/generate_204", HTTP_GET, []() {
    if (wifiConnected) handleAndroid204(); else handleCaptivePortal();
  });
  localServer.on("/gen_204", HTTP_GET, []() {
    if (wifiConnected) handleAndroid204(); else handleCaptivePortal();
  });
  localServer.on("/connectivitycheck.gstatic.com", HTTP_GET, []() {
    if (wifiConnected) handleAndroid204(); else handleCaptivePortal();
  });
  localServer.on("/ncsi.txt", HTTP_GET, []() {
    if (wifiConnected) handleWindowsNCsi(); else handleCaptivePortal();
  });
  localServer.on("/connecttest.txt", HTTP_GET, []() {
    if (wifiConnected) handleWindowsConnectTest(); else handleCaptivePortal();
  });
  localServer.on("/redirect", HTTP_GET, handleCaptivePortal);
  localServer.on("/hotspot",  HTTP_GET, handleCaptivePortal);
  localServer.on("/save",        HTTP_GET, handleSave);
  localServer.on("/connectwifi", HTTP_GET, handleConnectWifi);
  localServer.on("/resetwifi",   HTTP_GET, handleResetWifi);
  localServer.on("/status",      HTTP_GET, handleStatus);
  localServer.onNotFound(handleCaptivePortal);
  localServer.begin();
  Serial.println("[LocalWeb] Captive portal aktif di http://192.168.4.1");
}

// ================================================================
// WIFI CONNECT
// ================================================================
bool tryConnectWiFi(int sec) {
  WiFi.begin();
  Serial.print("[WiFi] Connecting");
  for (int i = 0; i < sec*2 && WiFi.status() != WL_CONNECTED; i++) {
    delay(500); Serial.print(".");
    blinkState = !blinkState;
    digitalWrite(PIN_LED_BLUE, blinkState);
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n[WiFi] OK: " + WiFi.localIP().toString());
    digitalWrite(PIN_LED_BLUE, HIGH);
    modeOffline = false; return true;
  }
  Serial.println("\n[WiFi] Gagal");
  return false;
}

// ================================================================
// NTP
// ================================================================
bool tryNTPSync() {
  configTime(7*3600, 0, "pool.ntp.org", "time.nist.gov");
  struct tm ti; int r = 0;
  while (!getLocalTime(&ti) && r < 20) { delay(500); r++; }
  return r < 20;
}

// ================================================================
// LED HANDLERS
// ================================================================
void handleBlueLed() {
  if (wifiConnected && WiFi.status() == WL_CONNECTED) {
    digitalWrite(PIN_LED_BLUE, HIGH); return;
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
// RESET BUTTON
// ================================================================
void checkResetButton() {
  if (digitalRead(PIN_RESET_WIFI) != LOW) return;
  unsigned long t = millis();
  oledStatus("Hold to reset", "WiFi credentials");
  while (digitalRead(PIN_RESET_WIFI) == LOW) {
    delay(100);
    if (millis() - t >= 3000) {
      oledStatus("Resetting WiFi...", "");
      WiFi.disconnect(true, true);
      delay(500); ESP.restart();
    }
  }
}

// ================================================================
// OLED
// ================================================================
void oledSplash() {
  display.clearDisplay(); display.setTextColor(WHITE); display.setTextSize(1);
  display.setCursor(0,0);  display.println("SMART ENERGY");
  display.setCursor(0,10); display.println("MONITOR v3.0");
  display.drawLine(0,20,127,20,WHITE);
  display.setCursor(0,26); display.println("Initializing...");
  display.display();
}

void oledStatus(const char* l1, const char* l2) {
  display.clearDisplay(); display.setTextColor(WHITE); display.setTextSize(1);
  display.setCursor(0,20); display.println(l1);
  if (l2 && strlen(l2)) { display.setCursor(0,36); display.println(l2); }
  display.display();
}

void oledData(float v, float i, float p, float pf, float hz, float kwh,
              float cost, bool dev, bool online, bool ovl, bool relay,
              bool offline, unsigned long offMs) {
  display.clearDisplay(); display.setTextColor(WHITE); display.setTextSize(1);

  display.setCursor(0,0);
  if (offline) {
    unsigned long s = offMs/1000;
    if (s < 60) display.printf("[OFF %lus]", s);
    else        display.printf("[OFF %lum%lus]", s/60, s%60);
  } else {
    display.print(online ? "[WiFi]" : "[NoWiFi]");
  }
  display.setCursor(72,0);
  display.print(relay ? "[RLY:ON]" : "[RLY:OFF]");
  display.drawLine(0,9,127,9,WHITE);

  if (ovl || overloadAlertLinger) {
    display.setCursor(0,13); display.println("!! OVERLOAD !!");
    display.setCursor(0,23); display.printf("%.1fW >= %.0fW", lastP, overloadThreshold);
    display.setCursor(0,33); display.println("Relay OFF-Proteksi");
    display.setCursor(0,43); display.println("192.168.4.1");
    display.display(); return;
  }

  if (!relay) {
    display.setCursor(10,20); display.println("Relay OFF");
    if (offline) {
      display.setCursor(0,32); display.println("SEM-Config:12345678");
      display.setCursor(0,44); display.println("-> 192.168.4.1");
    } else {
      display.setCursor(0,32); display.println("Web: klik + utk mulai");
    }
    display.display(); return;
  }

  if (!dev) {
    display.setCursor(15,20); display.println("No Device");
    display.setCursor(5,32);  display.println("Colokkan beban...");
    // ③ Tampilkan nama device yang sedang ditunggu (jika ada)
    if (strlen(sessionDeviceName) > 0) {
      display.setCursor(0,44);
      display.printf("Dev: %s", sessionDeviceName);
    }
    display.display(); return;
  }

  // ③ Tampilkan nama device di baris atas data
  display.setCursor(0,13);
  char shortName[17];
  strlcpy(shortName, sessionDeviceName, sizeof(shortName));
  display.printf("%-16s", shortName);

  display.setCursor(0,23); display.printf("V:%.1fV  I:%.2fA", v, i);
  display.setCursor(0,33); display.printf("P:%.1fW  PF:%.2f", p, pf);
  display.drawLine(0,43,127,43,WHITE);
  display.setCursor(0,47); display.printf("E:%.4f kWh", kwh);
  display.setCursor(0,57);
  if (cost >= 1000) display.printf("Rp %.0f", cost);
  else              display.printf("Rp %.1f", cost);
  display.display();
}

// ================================================================
// SETUP
// ================================================================
void setup() {
  Serial.begin(115200); delay(100);
  Serial.println("\n[Boot] Smart Energy Monitor v3.0");

  pinMode(PIN_LED_BLUE,   OUTPUT); pinMode(PIN_LED_GREEN, OUTPUT);
  pinMode(PIN_LED_RED,    OUTPUT); pinMode(PIN_BUZZER,    OUTPUT);
  pinMode(PIN_RELAY,      OUTPUT); pinMode(PIN_RESET_WIFI, INPUT_PULLUP);

  digitalWrite(PIN_LED_BLUE, LOW); digitalWrite(PIN_LED_GREEN, LOW);
  digitalWrite(PIN_LED_RED,  LOW); digitalWrite(PIN_BUZZER,    LOW);
  digitalWrite(PIN_RELAY, RELAY_OFF); relayOn = false;

  pzemSerial.begin(9600, SERIAL_8N1, 16, 17);
  loadPrefs();

  // ① Init LittleFS sebelum apapun
  fsInit();

  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C))
    Serial.println("[OLED] Init gagal");
  else { oledSplash(); delay(2000); }

  checkResetButton();

  WiFi.mode(WIFI_AP_STA);
  startLocalAP();
  setupWebServer();

  oledStatus("AP: SEM-Config", "pw: 12345678");
  delay(1500);

  // ═══════════════════════════════════════════════════════════════
  // ONLINE/OFFLINE MODE DETECTION & SETUP
  // ═══════════════════════════════════════════════════════════════
  oledStatus("Checking WiFi...", "");
  wifiConnected = tryConnectWiFi(20);

  if (wifiConnected) {
    // ─────────────────────────────────────────────────────────────
    // ★ ONLINE MODE STARTUP
    // ─────────────────────────────────────────────────────────────
    Serial.println("[Boot] ★ STARTUP: WiFi FOUND → ONLINE MODE");
    transitionToOnlineMode();
    
    WiFi.setSleep(false);
    oledStatus("WiFi OK ✓", "Sync NTP...");
    ntpSynced = tryNTPSync();
    delay(800);
    syncThresholdFromFirebase();
    clearFirebaseCommand();

    // ② Session recovery — harus setelah NTP sync agar timestamp akurat
    doSessionRecovery();

    // ④ Sync offline history yang mungkin tertinggal dari mode offline sebelumnya
    fsSyncOfflineHistoryToFirebase();

    unsigned long ts = ntpSynced ? (unsigned long)time(nullptr) : millis()/1000;
    sendToFirebase(0,0,0,0,0,0,0,false,false,false,ts);
    
    oledStatus("Online Ready ✓", "Waiting for web");
    delay(1000);
    Serial.println("[Boot-Online] Relay OFF — menunggu command web");
  } else {
    // ─────────────────────────────────────────────────────────────
    // ★ OFFLINE MODE STARTUP
    // ─────────────────────────────────────────────────────────────
    Serial.println("[Boot] ★ STARTUP: WiFi NOT FOUND → OFFLINE MODE");
    transitionToOfflineMode();
    
    // Consistency check: pastikan state sudah konsisten
    if (!relayOn) {
      Serial.println("[Boot] ⚠ Relay should be ON in offline mode, forcing ON");
      digitalWrite(PIN_RELAY, RELAY_ON);
      relayOn = true;
      sessionActive = true;
    }
    
    sessionEnergyWh=0; sessionKwh=0; sessionCost=0;
    hadDataOnce=false; disconnectCount=0; isOverload=false;
    overloadAlertLinger=false;

    // ② Session recovery juga dicoba saat offline
    doSessionRecovery();

    lastReconnectMs = millis();
    
    oledStatus("Offline Mode ✓", sessionDeviceName);
    delay(1500);
    Serial.printf("[Boot-Offline] Relay ON — waiting for device (%s)\n", sessionDeviceName);
  }

  lastLoopMs = lastThresholdSyncMs = lastCommandPollMs = lastCheckpointMs
             = lastOfflineSyncRetryMs = lastModeTransitionMs = millis();
}

// ================================================================
// LOOP
// ================================================================
void loop() {
  unsigned long now = millis();

  localServer.handleClient();
  dnsServer.processNextRequest();

  checkResetButton();
  handleBlueLed();
  handleGreenLed();
  handleOverloadAlert();

  // ── Session consistency check ────────────────────────────────
  // Jika relayOn tapi !sessionActive, atau sebaliknya, fix state
  if (relayOn && !sessionActive) {
    sessionActive = true;
    Serial.println("[State] ⚠ Fixed: relayOn=true but sessionActive=false, correcting...");
  }
  if (!relayOn && sessionActive && !modeOffline) {
    sessionActive = false;
    Serial.println("[State] ⚠ Fixed: relayOff but sessionActive=true, correcting...");
  }

  // ── WiFi disconnect detection & mode transition ────────────
  if (wifiConnected && WiFi.status() != WL_CONNECTED) {
    // ★ ONLINE MODE → OFFLINE MODE TRANSITION
    wifiConnected = false;
    digitalWrite(PIN_LED_BLUE, LOW);
    lastReconnectMs = now;
    
    Serial.println("[WiFi] ✗ DISCONNECTED — Mode transition starting...");
    transitionToOfflineMode();
    
    // Simpan checkpoint sesi terakhir sebelum masuk offline
    if (sessionActive && hadDataOnce) {
      bool saved = fsWriteSession();
      if (saved) {
        Serial.println("[WiFi-Disconnect] ✓ Checkpoint sesi disimpan ke LittleFS");
      } else {
        Serial.println("[WiFi-Disconnect] ✗ Checkpoint gagal, data mungkin hilang");
      }
    }
    oledStatus("WiFi Putus!", "Offline Mode ON");
    delay(1000);
  }

  // ── Auto-reconnect WiFi setiap 60 detik ───────────────────
  if (!wifiConnected && now - lastReconnectMs >= RECONNECT_INTERVAL) {
    lastReconnectMs = now;
    Serial.println("[WiFi] Trying to reconnect...");
    
    wifiConnected = tryConnectWiFi(15);
    
    if (wifiConnected) {
      // ★ OFFLINE MODE → ONLINE MODE TRANSITION
      Serial.println("[WiFi] ✓ RECONNECTED — Mode transition starting...");
      transitionToOnlineMode();
      
      WiFi.setSleep(false);
      if (!ntpSynced) ntpSynced = tryNTPSync();
      syncThresholdFromFirebase();
      
      // Sync offline history yang tertinggal saat offline
      int pendingCount = fsCountOfflineHistory();
      if (pendingCount > 0) {
        Serial.printf("[WiFi-Reconnect] Syncing %d offline sessions...\n", pendingCount);
        fsSyncOfflineHistoryToFirebase();
        oledStatus("Sync history...", "Please wait");
        delay(1000);
      }
      
      // Kirim live data ke Firebase untuk confirm online
      unsigned long ts = ntpSynced ? (unsigned long)time(nullptr) : now/1000;
      sendToFirebase(lastV, lastI, lastP, lastPF, lastHz,
                     sessionKwh, sessionCost,
                     deviceConnected, isOverload, relayOn, ts);
      
      unsigned long offlineDuration = (now - offlineStartMs) / 1000;
      Serial.printf("[WiFi-Reconnect] Back Online! Offline duration: %lu seconds\n", offlineDuration);
      oledStatus("Back Online ✓", "Synced!");
      delay(1000);
    }
  }

  // ── Sync threshold dari Firebase ─────────────────────────
  if (wifiConnected && now - lastThresholdSyncMs >= THRESHOLD_SYNC_INTERVAL) {
    lastThresholdSyncMs = now;
    syncThresholdFromFirebase();
  }

  // ── Poll relay command (ONLINE MODE ONLY) ────────────────
  // Dalam online mode, relay dikendalikan oleh web command
  // Tidak ada relay ON otomatis di online mode
  if (wifiConnected && !modeOffline && now - lastCommandPollMs >= COMMAND_POLL_INTERVAL) {
    lastCommandPollMs = now;
    pollCommandFromFirebase();
  }

  // ── Retry sync offline history (ONLINE MODE) ────────────
  // Coba sync ulang history offline yang belum ter-push
  if (wifiConnected && now - lastOfflineSyncRetryMs >= OFFLINE_SYNC_RETRY_INTERVAL) {
    lastOfflineSyncRetryMs = now;
    if (LittleFS.exists(FS_HISTORY_PATH)) {
      int pendingCount = fsCountOfflineHistory();
      if (pendingCount > 0) {
        Serial.printf("[Sync-Retry] Trying to sync %d pending offline sessions...\n", pendingCount);
        fsSyncOfflineHistoryToFirebase();
      }
    }
  }

  // ── Sensor loop setiap 5 detik ────────────────────────────
  if (now - lastLoopMs < LOOP_INTERVAL) return;
  float dT = (float)(now - lastLoopMs) / 3600000.0f;
  lastLoopMs = now;

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

  handleDeviceDisconnect();
  handleOverload(power);

  if (deviceConnected && relayOn && !isOverload) {
    sessionEnergyWh += power * dT;
    sessionKwh       = sessionEnergyWh / 1000.0f;
    sessionCost      = sessionKwh * tarif;
  }

  // ① Checkpoint ke LittleFS setiap 30 detik
  if (sessionActive && hadDataOnce && (now - lastCheckpointMs >= CHECKPOINT_INTERVAL)) {
    lastCheckpointMs = now;
    fsWriteSession();
  }

  prevDevConn = deviceConnected;

  // Log status saat ini
  const char* modeStr = modeOffline ? "OFF" : "ONL";
  const char* relayStr = relayOn ? "ON" : "OFF";
  const char* devStr = deviceConnected ? "Y" : "N";
  const char* ovlStr = isOverload ? "YES" : "no";
  
  Serial.printf("[%s-Mode] Relay:%s Dev:%s(%s) V:%.1f I:%.2f P:%.1f E:%.4f Ovl:%s\n",
    modeStr, relayStr, devStr, sessionDeviceName,
    voltage, current, power, sessionKwh, ovlStr);

  unsigned long offMs = modeOffline ? (now - offlineStartMs) : 0;
  oledData(voltage, current, power, pf, frequency,
           sessionKwh, sessionCost,
           deviceConnected, wifiConnected,
           isOverload, relayOn, modeOffline, offMs);

  // ── Send live data to Firebase (ONLINE MODE ONLY) ────────
  if (wifiConnected) {
    unsigned long ts = ntpSynced ? (unsigned long)time(nullptr) : now/1000;
    sendToFirebase(voltage, current, power, pf, frequency,
                   sessionKwh, sessionCost,
                   deviceConnected, isOverload, relayOn, ts);
  }
}