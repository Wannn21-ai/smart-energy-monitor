#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <WiFiManager.h>
#include <WebServer.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <PZEM004Tv30.h>
#include <DNSServer.h>

// ================================================================
// PIN DEFINITIONS
// ================================================================
#define PIN_LED_BLUE    2    // LED Biru  — WiFi status
#define PIN_LED_GREEN   25   // LED Hijau — device connected (V>0, I>0)
#define PIN_LED_RED     26   // LED Merah — overload (sinkron buzzer)
#define PIN_BUZZER      5    // Buzzer    — overload (sinkron LED merah)
#define PIN_RELAY       27   // Relay     — kontrol stopkontak (Active LOW)
#define PIN_RESET_WIFI  0    // Tombol BOOT — hold 3 detik reset WiFi

#define RELAY_ON   LOW
#define RELAY_OFF  HIGH

// ================================================================
// LOCAL AP & WEBSERVER
// AP selalu aktif (WIFI_AP_STA), webserver di 192.168.4.1
// ================================================================
DNSServer dnsServer;
const byte DNS_PORT = 53;

#define AP_SSID      "SEM-Config"
#define AP_PASS      "12345678"
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
// KONSTANTA
// ================================================================
const float  TARIF_DEFAULT     = 1444.70f;
const float  THRESHOLD_DEFAULT = 2000.0f;

const unsigned long LOOP_INTERVAL           = 5000;
const unsigned long RECONNECT_INTERVAL      = 60000;
const unsigned long THRESHOLD_SYNC_INTERVAL = 30000;
const unsigned long COMMAND_POLL_INTERVAL   = 2000;
const unsigned long OVERLOAD_BLINK_MS       = 200;
// LED/Buzzer tetap blink N detik setelah relay mati karena overload
const unsigned long OVERLOAD_ALERT_LINGER   = 10000;

// Device dianggap dicabut setelah N pembacaan berturutan I<0.01 & P<0.5
const int DISCONNECT_THRESHOLD = 2;

// ================================================================
// STATE — SISTEM
// ================================================================
bool wifiConnected = false;
bool ntpSynced     = false;
bool modeOffline   = false;
unsigned long offlineStartMs = 0;

// ================================================================
// STATE — SENSOR & RELAY
// ================================================================
bool  relayOn         = false;
bool  deviceConnected = false;
bool  prevDevConn     = false;
bool  isOverload      = false;
int   disconnectCount = 0;

// Overload alert linger: LED+Buzzer tetap berkedip setelah relay mati
bool          overloadAlertLinger = false;
unsigned long overloadLingerStart = 0;

// ================================================================
// STATE — AKUMULASI ENERGI
// ================================================================
float overloadThreshold = THRESHOLD_DEFAULT;
float tarif             = TARIF_DEFAULT;
float sessionEnergyWh   = 0.0f;
float sessionKwh        = 0.0f;
float sessionCost       = 0.0f;
bool  sessionActive     = false;

// Snapshot sensor terakhir yang valid (untuk OLED offline)
float lastV = 0, lastI = 0, lastP = 0, lastPF = 0, lastHz = 0;
bool  hadDataOnce = false;

// ================================================================
// TIMING
// ================================================================
unsigned long lastLoopMs          = 0;
unsigned long lastReconnectMs     = 0;
unsigned long lastThresholdSyncMs = 0;
unsigned long lastCommandPollMs   = 0;

// LED timing
unsigned long lastBlinkMs         = 0;
bool          blinkState          = false;
unsigned long lastOverloadBlinkMs = 0;
bool          overloadBlinkState  = false;

// ================================================================
// FORWARD DECLARATIONS
// ================================================================
void loadPrefs(); void savePrefs();
void saveSessionToPrefs(); void loadSessionFromPrefs(); void clearSessionPrefs();
bool tryConnectWiFi(int sec = 20); bool tryNTPSync();
void startLocalAP(); void setupWebServer(); void startWiFiManager();
void setRelay(bool on, const char* reason = "");
void handleDeviceDisconnect(); void handleOverload(float power);
bool sendToFirebase(float v, float i, float p, float pf, float freq,
                    float kwh, float cost, bool dev, bool ovl,
                    bool relay, unsigned long ts);
void syncThresholdFromFirebase(); void pollCommandFromFirebase();
void clearFirebaseCommand();
void handleBlueLed(); void handleGreenLed(); void handleOverloadAlert();
void checkResetButton();
void oledSplash(); void oledStatus(const char* l1, const char* l2 = "");
void oledData(float v, float i, float p, float pf, float hz, float kwh,
              float cost, bool dev, bool online, bool ovl, bool relay,
              bool offline, unsigned long offMs);

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
void saveSessionToPrefs() {
  prefs.begin("session", false);
  prefs.putFloat("energyWh", sessionEnergyWh);
  prefs.putFloat("kwh",      sessionKwh);
  prefs.putFloat("cost",     sessionCost);
  prefs.putFloat("lastV",    lastV);
  prefs.putFloat("lastI",    lastI);
  prefs.putFloat("lastP",    lastP);
  prefs.putBool ("hadData",  hadDataOnce);
  prefs.putBool ("relayOn",  relayOn);
  prefs.putBool ("active",   sessionActive);
  prefs.end();
}
void loadSessionFromPrefs() {
  prefs.begin("session", true);
  if (prefs.getBool("active", false) && prefs.getBool("relayOn", false)) {
    sessionEnergyWh = prefs.getFloat("energyWh", 0);
    sessionKwh      = prefs.getFloat("kwh",      0);
    sessionCost     = prefs.getFloat("cost",     0);
    lastV           = prefs.getFloat("lastV",    0);
    lastI           = prefs.getFloat("lastI",    0);
    lastP           = prefs.getFloat("lastP",    0);
    hadDataOnce     = prefs.getBool ("hadData",  false);
    sessionActive   = true;
    Serial.printf("[Session] Restored: %.4f kWh Rp%.0f\n", sessionKwh, sessionCost);
  }
  prefs.end();
}
void clearSessionPrefs() {
  prefs.begin("session", false);
  prefs.putFloat("energyWh", 0); prefs.putFloat("kwh", 0); prefs.putFloat("cost", 0);
  prefs.putBool("hadData", false); prefs.putBool("relayOn", false); prefs.putBool("active", false);
  prefs.end();
}

// ================================================================
// RELAY
// ================================================================
void setRelay(bool on, const char* reason) {
  relayOn = on;
  digitalWrite(PIN_RELAY, on ? RELAY_ON : RELAY_OFF);
  if (!on) { sessionActive = false; disconnectCount = 0; clearSessionPrefs(); }
  else      { sessionActive = true; }
  Serial.printf("[Relay] %s — %s\n", on ? "ON" : "OFF", reason);
}

// ================================================================
// DEVICE DISCONNECT — 2 pembacaan berturutan → relay OFF
// ================================================================
void handleDeviceDisconnect() {
  if (!relayOn || !sessionActive) return;
  if (!deviceConnected && prevDevConn) {
    disconnectCount++;
    Serial.printf("[Disconnect] %d/%d\n", disconnectCount, DISCONNECT_THRESHOLD);
    if (disconnectCount >= DISCONNECT_THRESHOLD) {
      Serial.println("[Disconnect] Device dicabut — relay OFF");
      if (wifiConnected) {
        unsigned long ts = ntpSynced ? (unsigned long)time(nullptr) : millis()/1000;
        sendToFirebase(0, 0, 0, 0, 0, sessionKwh, sessionCost, false, false, true, ts);
      }
      setRelay(false, "device dicabut");
      sessionEnergyWh = 0; sessionKwh = 0; sessionCost = 0; hadDataOnce = false;
    }
  } else if (deviceConnected) {
    disconnectCount = 0;
  }
}

// ================================================================
// OVERLOAD — relay OFF + alert linger
// ================================================================
void handleOverload(float power) {
  bool newOvl = deviceConnected && (power >= overloadThreshold);
  if (newOvl && !isOverload) {
    isOverload = true;
    Serial.printf("[Overload] %.1fW >= %.0fW — relay OFF\n", power, overloadThreshold);
    setRelay(false, "overload");
    overloadAlertLinger = true;
    overloadLingerStart = millis();
    sessionEnergyWh = 0; sessionKwh = 0; sessionCost = 0;
  } else if (!newOvl && isOverload) {
    isOverload = false;
    Serial.println("[Overload] Teratasi");
  }
}

// ================================================================
// FIREBASE
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
  if (!h.begin(c, String(FIREBASE_HOST) + "/command/relay.json")) return;
  int code = h.GET();
  if (code == 200) {
    String pl = h.getString(); pl.trim(); h.end();
    if (pl == "true" && !relayOn) {
      sessionEnergyWh=0; sessionKwh=0; sessionCost=0;
      hadDataOnce=false; disconnectCount=0; isOverload=false;
      overloadAlertLinger=false;
      setRelay(true, "perintah web");
      clearFirebaseCommand();
    } else if (pl == "false" && relayOn) {
      setRelay(false, "stop session web");
      sessionEnergyWh=0; sessionKwh=0; sessionCost=0;
      hadDataOnce=false; prevDevConn=false; disconnectCount=0;
      clearFirebaseCommand();
    }
  } else h.end();
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
  j += ",\"offline\":"   + String(modeOffline ? "true":"false") + "},";
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
  Serial.printf("[FB] %d P=%.1fW E=%.4fkWh\n", code, p, kwh);
  return (code == 200 || code == 204);
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
// LOCAL WEBSERVER — HTML
// Hanya form konfigurasi: WiFi, threshold, tarif + reset
// Tidak ada status sensor, tidak ada autentikasi
// ================================================================
void handleRoot() {
  // Scan WiFi networks untuk dropdown
  int n = WiFi.scanNetworks();
  String ssidOptions = "";
  for (int i = 0; i < n; i++) {
    String ssid = WiFi.SSID(i);
    int rssi    = WiFi.RSSI(i);
    bool locked = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN);
    // Signal bar: 4=kuat, 1=lemah
    int bars = rssi > -50 ? 4 : rssi > -65 ? 3 : rssi > -75 ? 2 : 1;
    String bar = bars == 4 ? "████" : bars == 3 ? "███░" : bars == 2 ? "██░░" : "█░░░";
    ssidOptions += "<option value='" + ssid + "'>"
                + bar + " " + ssid + (locked ? " 🔒" : "") + "</option>";
  }
  if (n == 0) ssidOptions = "<option value=''>Tidak ada jaringan ditemukan</option>";

  String html = "<!DOCTYPE html><html lang='id'><head>"
    "<meta charset='UTF-8'/>"
    "<meta name='viewport' content='width=device-width,initial-scale=1,maximum-scale=1'/>"
    "<title>Smart Energy Monitor · Setup</title>"
    "<style>"
    // ── Reset ──
    "*{margin:0;padding:0;box-sizing:border-box}"
    "body{background:#0a0a0a;color:#f0f0f0;"
      "font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif;"
      "min-height:100vh;display:flex;flex-direction:column;"
      "align-items:center;justify-content:flex-start;padding:24px 16px}"
    // ── Header ──
    ".hdr{width:100%;max-width:420px;display:flex;align-items:center;gap:14px;"
      "margin-bottom:28px}"
    ".hdr-icon{width:44px;height:44px;flex-shrink:0;"
      "background:rgba(0,229,255,0.08);border:1.5px solid #00e5ff;"
      "border-radius:12px;display:flex;align-items:center;"
      "justify-content:center;font-size:22px}"
    ".hdr-title{font-size:17px;font-weight:700;color:#00e5ff;letter-spacing:.04em}"
    ".hdr-sub{font-size:12px;color:#444;margin-top:3px}"
    // ── Step indicator ──
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
    // ── Card ──
    ".card{background:#111;border:1px solid rgba(255,255,255,0.07);"
      "border-radius:18px;padding:22px;width:100%;max-width:420px;"
      "margin-bottom:14px}"
    ".card-title{font-size:10px;font-weight:700;color:#444;"
      "letter-spacing:.1em;text-transform:uppercase;"
      "margin-bottom:18px;padding-bottom:12px;"
      "border-bottom:1px solid rgba(255,255,255,0.04);"
      "display:flex;align-items:center;gap:8px}"
    // ── Form ──
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
    // ── Input row (value + unit) ──
    ".input-row{display:flex;gap:10px;align-items:flex-end}"
    ".input-row input{flex:1}"
    ".input-unit{font-size:13px;color:#555;padding-bottom:14px;white-space:nowrap}"
    // ── Converted preview ──
    ".converted{font-size:12px;color:#555;margin-top:6px;min-height:18px;transition:color .2s}"
    ".converted.active{color:#00e5ff}"
    // ── Buttons ──
    ".btn{display:block;width:100%;padding:14px;"
      "border:none;border-radius:12px;"
      "font-size:15px;font-weight:700;cursor:pointer;"
      "transition:opacity .15s,transform .1s;letter-spacing:.02em}"
    ".btn:active{opacity:.8;transform:scale(.98)}"
    ".btn-primary{background:#00e5ff;color:#000}"
    ".btn-ghost{background:transparent;color:#555;"
      "border:1px solid rgba(255,255,255,0.08);margin-top:10px}"
    ".btn:disabled{opacity:.4;cursor:not-allowed}"
    // ── Status badge ──
    ".badge{display:inline-flex;align-items:center;gap:6px;"
      "padding:4px 12px;border-radius:20px;font-size:11px;font-weight:600}"
    ".badge-online{background:rgba(0,230,118,0.12);"
      "border:1px solid rgba(0,230,118,0.3);color:#00e676}"
    ".badge-offline{background:rgba(255,171,0,0.10);"
      "border:1px solid rgba(255,171,0,0.3);color:#ffab00}"
    // ── Toast ──
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
    // ── Spinner ──
    ".spin{display:inline-block;width:16px;height:16px;"
      "border:2px solid rgba(0,0,0,.2);border-top-color:#000;"
      "border-radius:50%;animation:spin .7s linear infinite;vertical-align:middle}"
    "@keyframes spin{to{transform:rotate(360deg)}}"
    // ── Footer ──
    ".footer{width:100%;max-width:420px;text-align:center;"
      "font-size:11px;color:#2a2a2a;margin-top:16px;line-height:2}"
    // ── Current values chip ──
    ".chip{display:inline-flex;align-items:center;gap:6px;"
      "background:#1a1a1a;border:1px solid rgba(255,255,255,0.06);"
      "border-radius:20px;padding:4px 12px;font-size:11px;color:#555;"
      "font-family:monospace;margin-top:6px}"
    ".chip span{color:#00e5ff}"
    "</style></head><body>";

  // ── Header ─────────────────────────────────────────────────
  html += "<div class='hdr'>"
    "<div class='hdr-icon'>⚡</div>"
    "<div>"
      "<div class='hdr-title'>Smart Energy Monitor</div>"
      "<div class='hdr-sub'>Konfigurasi perangkat · SEM v3.0</div>"
    "</div>"
    "</div>";

  // ── Step indicator ─────────────────────────────────────────
  html += "<div class='steps'>"
    "<div class='step'>"
      "<div class='step-dot active'>1</div>"
      "<div class='step-label'>WiFi</div>"
    "</div>"
    "<div class='step-line'></div>"
    "<div class='step'>"
      "<div class='step-dot pending'>2</div>"
      "<div class='step-label'>Tarif</div>"
    "</div>"
    "<div class='step-line'></div>"
    "<div class='step'>"
      "<div class='step-dot pending'>3</div>"
      "<div class='step-label'>Selesai</div>"
    "</div>"
    "</div>";

  // ── Card 1: WiFi ───────────────────────────────────────────
  html += "<div class='card' id='card-wifi'>"
    "<div class='card-title'>📶 &nbsp;Koneksi WiFi</div>"
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

  // ── Card 2: Pengaturan (hidden until WiFi OK) ──────────────
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

  // ── Card 3: Done (hidden) ──────────────────────────────────
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

  // ── Footer ─────────────────────────────────────────────────
  html += "<div class='footer'>"
    "SEM-Config &nbsp;·&nbsp; 192.168.4.1<br>"
    "Hubungkan ke WiFi <b>SEM-Config</b> &nbsp;·&nbsp; pw: <b>12345678</b>"
    "</div>";

  // ── Toast ──────────────────────────────────────────────────
  html += "<div id='toast'></div>";

  // ── JavaScript ─────────────────────────────────────────────
  html += R"JS(<script>
var _tt, wifiOk = false;

// ── Toast ──
function toast(msg, type) {
  var el = document.getElementById('toast');
  el.textContent = msg;
  el.className = type || '';
  el.classList.add('show');
  clearTimeout(_tt);
  _tt = setTimeout(function() { el.classList.remove('show'); }, 4000);
}

// ── Converted preview (IDR → USD) ──
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

// ── Manual SSID toggle ──
function manualSSID() {
  var w = document.getElementById('manual-wrap');
  w.style.display = w.style.display === 'none' ? 'block' : 'none';
  if (w.style.display !== 'none')
    document.getElementById('ssid-manual').focus();
}

// ── Step 1: Connect WiFi ──
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

  // Step dots
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

// ── Step 2: Save settings ──
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
      if (t.indexOf('Tersimpan') >= 0) {
        showDone();
      } else {
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

function skipSettings() {
  showDone();
}

// ── Show done card ──
function showDone() {
  document.getElementById('card-settings').style.display = 'none';
  document.getElementById('card-done').style.display = 'block';
  document.querySelectorAll('.step-dot')[1].className = 'step-dot done';
  document.querySelectorAll('.step-dot')[1].textContent = '✓';
  document.querySelectorAll('.step-dot')[2].className = 'step-dot done';
  document.querySelectorAll('.step-dot')[2].textContent = '✓';
  document.getElementById('card-done').scrollIntoView({ behavior: 'smooth' });
  // Poll IP
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

// ── Reset WiFi ──
function resetWifi() {
  if (!confirm('Reset semua konfigurasi WiFi dan mulai ulang?')) return;
  fetch('/resetwifi').then(function() {
    toast('Mereset... ESP32 akan restart', 'info');
  });
}

// ── Enter key support ──
document.addEventListener('keydown', function(e) {
  if (e.key !== 'Enter') return;
  if (!wifiOk) step1(); else step2();
});
</script>)JS";

  html += "</body></html>";
  localServer.send(200, "text/html; charset=utf-8", html);
}

// Handler: simpan threshold & tarif
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
    // Jika online, push threshold ke Firebase supaya web sync
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

// Handler: connect ke WiFi baru
// ESP32 coba connect ke SSID yang diberikan selama 10 detik
void handleConnectWifi() {
  if (!localServer.hasArg("ssid")) {
    localServer.send(400, "text/plain", "SSID diperlukan");
    return;
  }
  String ssid = localServer.arg("ssid");
  String pass = localServer.hasArg("pass") ? localServer.arg("pass") : "";

  Serial.printf("[LocalWeb] Coba connect WiFi: %s\n", ssid.c_str());

  // Simpan kredensial ke flash WiFi
  WiFi.begin(ssid.c_str(), pass.c_str());

  // Tunggu 10 detik
  int waited = 0;
  while (WiFi.status() != WL_CONNECTED && waited < 20) {
    delay(500); waited++;
    localServer.handleClient(); // agar webserver tidak timeout
  }

  if (WiFi.status() == WL_CONNECTED) {
    wifiConnected = true;
    modeOffline   = false;
    digitalWrite(PIN_LED_BLUE, HIGH);
    String ip = WiFi.localIP().toString();
    Serial.println("[LocalWeb] WiFi OK: " + ip);
    // Kirim status awal ke Firebase
    if (!ntpSynced) ntpSynced = tryNTPSync();
    syncThresholdFromFirebase();
    clearFirebaseCommand();
    localServer.send(200, "text/plain",
      "✓ Berhasil! IP: " + ip + " — Dashboard web aktif kembali.");
  } else {
    Serial.println("[LocalWeb] Gagal connect WiFi");
    localServer.send(200, "text/plain",
      "✗ Gagal terhubung ke \"" + ssid + "\". Cek SSID & password, lalu coba lagi.");
  }
}

// Handler: reset WiFi & restart
void handleResetWifi() {
  localServer.send(200, "text/plain", "Mereset konfigurasi WiFi — ESP32 restart...");
  delay(500);
  WiFiManager wm; wm.resetSettings();
  ESP.restart();
}

void handleNotFound() {
  localServer.send(404, "text/plain", "404 Not Found");
}

// Handler: status JSON (untuk halaman done)
void handleStatus() {
  String ip = wifiConnected ? WiFi.localIP().toString() : "—";
  String json = "{\"wifi\":" + String(wifiConnected ? "true":"false") +
                ",\"ip\":\"" + ip + "\""
                ",\"threshold\":" + String(overloadThreshold,0) +
                ",\"tarif\":" + String(tarif,2) + "}";
  localServer.send(200, "application/json", json);
}

// Handler captive portal — redirect semua URL asing ke root
// Browser (Android/iOS/Windows) mendeteksi redirect ini sebagai captive portal
void handleCaptivePortal() {
  localServer.sendHeader("Location", "http://192.168.4.1/", true);
  localServer.send(302, "text/plain", "");
}

// Handler: Apple CNA (Captive Network Assistant)
void handleAppleCNA() {
  // iOS/macOS mengecek URL ini — jika bukan "Success" maka buka portal
  localServer.send(200, "text/html",
    "<HTML><HEAD><TITLE>Success</TITLE></HEAD>"
    "<BODY>Success</BODY></HTML>");
}

void setupWebServer() {
  // Halaman utama
  localServer.on("/",                HTTP_GET, handleRoot);
  localServer.on("/index.html",      HTTP_GET, handleRoot);

  // Apple Captive Network Assistant endpoints
  localServer.on("/hotspot-detect.html",   HTTP_GET, handleAppleCNA);
  localServer.on("/library/test/success.html", HTTP_GET, handleAppleCNA);
  localServer.on("/success.txt",           HTTP_GET, handleAppleCNA);

  // Android / Windows captive portal check endpoints
  localServer.on("/generate_204",     HTTP_GET, handleCaptivePortal);
  localServer.on("/gen_204",          HTTP_GET, handleCaptivePortal);
  localServer.on("/redirect",         HTTP_GET, handleCaptivePortal);
  localServer.on("/hotspot",          HTTP_GET, handleCaptivePortal);
  localServer.on("/ncsi.txt",         HTTP_GET, handleCaptivePortal);
  localServer.on("/connecttest.txt",  HTTP_GET, handleCaptivePortal);

  // Fungsional
  localServer.on("/save",        HTTP_GET, handleSave);
  localServer.on("/connectwifi", HTTP_GET, handleConnectWifi);
  localServer.on("/resetwifi",   HTTP_GET, handleResetWifi);
  localServer.on("/status",      HTTP_GET, handleStatus);

  // Catch-all: redirect semua URL yang tidak dikenal ke root (captive portal)
  localServer.onNotFound(handleCaptivePortal);

  localServer.begin();
  Serial.println("[LocalWeb] Captive portal aktif di http://192.168.4.1");
}

// ================================================================
// WIFI MANAGER (portal setup awal)
// ================================================================
const char WIFI_MGR_HEAD[] PROGMEM = R"html(
<style>
*{margin:0;padding:0;box-sizing:border-box}
body{background:#0a0a0a;color:#f0f0f0;font-family:sans-serif;
     min-height:100vh;display:flex;align-items:center;justify-content:center;padding:20px}
.card{background:#111;border:1px solid rgba(255,255,255,0.07);
      border-radius:18px;padding:32px;width:100%;max-width:380px}
h1{font-size:16px;font-weight:700;margin-bottom:6px}
p{font-size:13px;color:#666;margin-bottom:24px}
label{display:block;font-size:11px;font-weight:600;color:#888;
      letter-spacing:.05em;text-transform:uppercase;margin-bottom:6px}
input[type=text],input[type=password]{width:100%;padding:10px 14px;background:#1a1a1a;
  border:1px solid rgba(255,255,255,0.07);border-radius:6px;
  color:#f0f0f0;font-size:14px;outline:none;margin-bottom:16px}
input[type=submit]{width:100%;padding:11px;background:#00e5ff;color:#000;
  font-weight:700;font-size:14px;border:none;border-radius:6px;cursor:pointer}
</style>
)html";

const char WIFI_MGR_BODY[] PROGMEM = R"html(
<div class="card">
<h1>Smart Energy Monitor</h1>
<p>Pilih WiFi dan masukkan password untuk menghubungkan perangkat.</p>
)html";

void startWiFiManager() {
  oledStatus("WiFi Setup", "AP: SEM-Setup");
  WiFiManager wm;
  wm.setCustomHeadElement(WIFI_MGR_HEAD);
  wm.setCustomMenuHTML(WIFI_MGR_BODY);
  WiFiManagerParameter p_thr("thr", "Overload Threshold (Watt)",
    String(overloadThreshold,0).c_str(), 8);
  WiFiManagerParameter p_trf("trf", "Tariff per kWh (IDR)",
    String(tarif,2).c_str(), 10);
  wm.addParameter(&p_thr);
  wm.addParameter(&p_trf);
  wm.setConfigPortalTimeout(180);
  wm.setTitle("Smart Energy Monitor");
  wm.setDarkMode(true);
  bool ok = wm.startConfigPortal("SEM-Setup");
  if (ok) {
    float t1 = String(p_thr.getValue()).toFloat();
    float t2 = String(p_trf.getValue()).toFloat();
    if (t1 > 0) overloadThreshold = t1;
    if (t2 > 0) tarif = t2;
    savePrefs();
    wifiConnected = true; modeOffline = false;
  } else {
    wifiConnected = false; modeOffline = true;
  }
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
  // Nyala saat V>0 dan I>0 (device terhubung), mati jika tidak ada
  digitalWrite(PIN_LED_GREEN, (sessionActive && relayOn) ? HIGH : LOW);
}

// LED Merah + Buzzer sinkron:
// - Aktif saat isOverload = true
// - Tetap aktif OVERLOAD_ALERT_LINGER ms setelah overload selesai (linger)
void handleOverloadAlert() {
  // Cek apakah linger sudah habis
  if (overloadAlertLinger && !isOverload) {
    if (millis() - overloadLingerStart >= OVERLOAD_ALERT_LINGER) {
      overloadAlertLinger = false;
      overloadBlinkState  = false;
      digitalWrite(PIN_LED_RED, LOW);
      digitalWrite(PIN_BUZZER,  LOW);
      Serial.println("[Alert] Linger selesai");
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

  // Blink sinkron 200ms
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
      WiFiManager wm; wm.resetSettings();
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

  // Baris 0: status
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

  // Overload/linger
  if (ovl || overloadAlertLinger) {
    display.setCursor(0,13); display.println("!! OVERLOAD !!");
    display.setCursor(0,23); display.printf("%.1fW >= %.0fW", lastP, overloadThreshold);
    display.setCursor(0,33); display.println("Relay OFF-Proteksi");
    display.setCursor(0,43); display.println("192.168.4.1");
    display.display(); return;
  }

  // Relay OFF
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

  // No device
  if (!dev) {
    display.setCursor(15,20); display.println("No Device");
    display.setCursor(5,32);  display.println("Colokkan beban...");
    display.display(); return;
  }

  // Data sensor
  display.setCursor(0,13); display.printf("V:%.1fV  I:%.2fA", v, i);
  display.setCursor(0,23); display.printf("P:%.1fW  PF:%.2f", p, pf);
  display.setCursor(0,33); display.printf("Hz:%.1f Thr:%.0fW", hz, overloadThreshold);
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
  loadSessionFromPrefs();

  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C))
    Serial.println("[OLED] Init gagal");
  else { oledSplash(); delay(2000); }

  checkResetButton();

  // ── Mode AP+STA: AP selalu aktif, STA untuk internet ──────
  WiFi.mode(WIFI_AP_STA);
  startLocalAP();
  setupWebServer();

  // Tampilkan info AP sebentar di OLED
  oledStatus("AP: SEM-Config", "pw: 12345678");
  delay(1500);

  // Coba connect WiFi STA
  oledStatus("Connecting WiFi...", "");
  wifiConnected = tryConnectWiFi(20);

  if (!wifiConnected) {
    // Tidak ada kredensial tersimpan → buka portal WiFiManager
    // (sementara AP SEM-Config digantikan oleh portal "SEM-Setup")
    startWiFiManager();
    // Setelah portal selesai, restart AP SEM-Config kembali
    startLocalAP();
  }

  if (wifiConnected) {
    modeOffline = false;
    WiFi.setSleep(false);
    oledStatus("WiFi OK", "Sync NTP...");
    ntpSynced = tryNTPSync();
    delay(800);
    syncThresholdFromFirebase();
    clearFirebaseCommand();
    unsigned long ts = ntpSynced ? (unsigned long)time(nullptr) : millis()/1000;
    sendToFirebase(0,0,0,0,0,0,0,false,false,false,ts);
    Serial.println("[Boot] Online — relay OFF, tunggu perintah web");
  } else {
    modeOffline = true; offlineStartMs = millis();
    sessionEnergyWh=0; sessionKwh=0; sessionCost=0;
    hadDataOnce=false; disconnectCount=0; isOverload=false;
    overloadAlertLinger=false;
    setRelay(true, "mode offline otomatis");
    oledStatus("Mode OFFLINE", "Relay ON - Siap ukur");
    delay(1500);
    lastReconnectMs = millis();
    Serial.println("[Boot] Offline — relay ON, AP: 192.168.4.1");
  }

  lastLoopMs = lastThresholdSyncMs = lastCommandPollMs = millis();
}

// ================================================================
// LOOP
// ================================================================
void loop() {
  unsigned long now = millis();

  // ── Webserver handle client ────────────────────────────────
  localServer.handleClient();
  dnsServer.processNextRequest();

  checkResetButton();
  handleBlueLed();
  handleGreenLed();
  handleOverloadAlert();

  // ── Deteksi WiFi putus (runtime) ──────────────────────────
  if (wifiConnected && WiFi.status() != WL_CONNECTED) {
    wifiConnected = false;
    if (!modeOffline) offlineStartMs = now;
    modeOffline = true;
    digitalWrite(PIN_LED_BLUE, LOW);
    lastReconnectMs = now;
    Serial.println("[WiFi] Terputus");
    if (relayOn && hadDataOnce) { saveSessionToPrefs(); Serial.println("[Session] → flash"); }
    if (!relayOn) {
      sessionEnergyWh=0; sessionKwh=0; sessionCost=0;
      hadDataOnce=false; disconnectCount=0; isOverload=false; overloadAlertLinger=false;
      setRelay(true, "mode offline (WiFi putus)");
    }
  }

  // ── Reconnect otomatis setiap 60 detik ────────────────────
  if (!wifiConnected && now - lastReconnectMs >= RECONNECT_INTERVAL) {
    lastReconnectMs = now;
    wifiConnected = tryConnectWiFi(15);
    if (wifiConnected) {
      modeOffline = false; WiFi.setSleep(false);
      if (!ntpSynced) ntpSynced = tryNTPSync();
      syncThresholdFromFirebase();
      unsigned long ts = ntpSynced ? (unsigned long)time(nullptr) : now/1000;
      sendToFirebase(lastV, lastI, lastP, lastPF, lastHz,
                     sessionKwh, sessionCost,
                     deviceConnected, isOverload, relayOn, ts);
      Serial.printf("[WiFi] Online! Offline %.0fs\n", (float)(now-offlineStartMs)/1000);
      oledStatus("WiFi Kembali!", "Data dikirim...");
      delay(800);
    }
  }

  // ── Sync threshold dari Firebase (online) ─────────────────
  if (wifiConnected && now - lastThresholdSyncMs >= THRESHOLD_SYNC_INTERVAL) {
    lastThresholdSyncMs = now;
    syncThresholdFromFirebase();
  }

  // ── Poll command relay dari Firebase (online saja) ─────────
  if (wifiConnected && !modeOffline && now - lastCommandPollMs >= COMMAND_POLL_INTERVAL) {
    lastCommandPollMs = now;
    pollCommandFromFirebase();
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

  // Simpan ke flash saat offline
  if (!wifiConnected && relayOn && hadDataOnce) saveSessionToPrefs();

  prevDevConn = deviceConnected;

  // Serial debug
  Serial.printf("[%s] Relay:%s Dev:%s V:%.1f I:%.2f P:%.1f E:%.4f Ovl:%s Thr:%.0f\n",
    modeOffline?"OFF":"ONL", relayOn?"ON":"OFF", deviceConnected?"Y":"N",
    voltage, current, power, sessionKwh, isOverload?"YES":"no", overloadThreshold);

  // OLED
  unsigned long offMs = modeOffline ? (now - offlineStartMs) : 0;
  oledData(voltage, current, power, pf, frequency,
           sessionKwh, sessionCost,
           deviceConnected, wifiConnected,
           isOverload, relayOn, modeOffline, offMs);

  // Firebase
  if (wifiConnected) {
    unsigned long ts = ntpSynced ? (unsigned long)time(nullptr) : now/1000;
    sendToFirebase(voltage, current, power, pf, frequency,
                   sessionKwh, sessionCost,
                   deviceConnected, isOverload, relayOn, ts);
  }
}