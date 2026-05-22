#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <WiFiManager.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <PZEM004Tv30.h>

// ================================================================
// PIN DEFINITIONS
// ================================================================
#define PIN_LED_WIFI     2    // LED biru — WiFi status (onboard LED ESP32)
#define PIN_LED_OVERLOAD 4    // LED merah — overload
#define PIN_BUZZER       5    // Buzzer — overload alert
#define PIN_RESET_WIFI   0    // Tombol BOOT — hold 3 detik untuk reset WiFi

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
PZEM004Tv30 pzem(pzemSerial, 16, 17); // RX=16, TX=17

// ================================================================
// FIREBASE
// ================================================================
const char* FIREBASE_HOST = "https://smart-energy-monitor-v2-de79d-default-rtdb.asia-southeast1.firebasedatabase.app";
const char* FIREBASE_PATH = "/live.json";

// ================================================================
// PREFERENCES (NVS) — simpan threshold overload
// ================================================================
Preferences prefs;

// ================================================================
// KONSTANTA & STATE
// ================================================================
const float  TARIF_DEFAULT      = 1444.70f;
const float  THRESHOLD_DEFAULT  = 2000.0f;  // Watt
const unsigned long LOOP_INTERVAL         = 5000;   // ms — interval baca sensor
const unsigned long RECONNECT_INTERVAL    = 60000;  // ms — reconnect WiFi
const unsigned long THRESHOLD_SYNC_INTERVAL = 30000; // ms — sync threshold dari Firebase
const unsigned long BUZZER_ON_MS          = 200;    // ms — durasi buzzer beep
const unsigned long BUZZER_OFF_MS         = 300;    // ms — jeda antar beep
const int    BUZZER_BEEPS       = 3;

// ================================================================
// GLOBAL STATE
// ================================================================
bool  wifiConnected     = false;
bool  ntpSynced         = false;
bool  deviceConnected   = false;
bool  isOverload        = false;
bool  prevDevConn       = false;

float overloadThreshold = THRESHOLD_DEFAULT;
float tarif             = TARIF_DEFAULT;

// Energy akumulasi mandiri
float sessionEnergyWh   = 0.0f;
float sessionKwh        = 0.0f;
float sessionCost       = 0.0f;

unsigned long lastLoopMs          = 0;
unsigned long lastReconnectMs     = 0;
unsigned long lastThresholdSyncMs = 0;  // FIX #4: track kapan terakhir sync threshold
unsigned long lastBuzzerMs        = 0;
int           buzzerBeepCount     = 0;
bool          buzzerActive        = false;
bool          buzzerState         = false;

// ================================================================
// WiFi BLINK STATE (non-blocking)
// ================================================================
unsigned long lastBlinkMs  = 0;
bool          blinkState   = false;

// ================================================================
// FORWARD DECLARATIONS
// ================================================================
bool  tryConnectWiFi(int timeoutSeconds = 20);
bool  tryNTPSync();
bool  sendToFirebase(float v, float i, float p, float pf, float freq,
                     float kwh, float cost, bool devConn, bool overload,
                     unsigned long ts);
void  syncThresholdFromFirebase();  // FIX #4
void  oledSplash();
void  oledStatus(const char* l1, const char* l2 = "");
void  oledData(float v, float i, float p, float pf, float freq,
               float kwh, float cost, bool devConn, bool online, bool ovld);
void  handleBuzzer();
void  handleWifiLed();
void  checkResetButton();
void  loadPrefs();
void  startWiFiManager();

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
// FIX #4: syncThresholdFromFirebase
// Baca threshold dari Firebase path yang ditulis oleh web settings.
// Web menulis ke: users/{uid}/settings/overloadThreshold
// Tapi untuk ESP32 yang tidak tahu UID, kita sediakan path publik:
// /config/threshold.json yang bisa dibaca tanpa auth.
//
// CARA SETUP DI FIREBASE RULES:
//   "config": { ".read": true, ".write": "auth != null" }
//
// Web perlu menulis ke /config/threshold setiap kali settings disimpan.
// Lihat catatan di bawah untuk patch settings.js.
// ================================================================
void syncThresholdFromFirebase() {
  if (!wifiConnected || WiFi.status() != WL_CONNECTED) return;

  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(8000);
  HTTPClient http;

  // Baca threshold dari path publik
  String url = String(FIREBASE_HOST) + "/config/threshold.json";
  if (!http.begin(client, url)) {
    Serial.println("[Threshold Sync] http.begin() gagal");
    return;
  }
  http.addHeader("Content-Type", "application/json");
  int code = http.GET();

  if (code == 200) {
    String payload = http.getString();
    payload.trim();
    // Payload bisa berupa angka langsung: 2000 atau "null"
    if (payload != "null" && payload.length() > 0) {
      float newThreshold = payload.toFloat();
      if (newThreshold > 0 && newThreshold != overloadThreshold) {
        Serial.printf("[Threshold Sync] Updated: %.0f → %.0f W\n",
                      overloadThreshold, newThreshold);
        overloadThreshold = newThreshold;
        savePrefs();
      } else {
        Serial.printf("[Threshold Sync] No change: %.0f W\n", overloadThreshold);
      }
    } else {
      Serial.println("[Threshold Sync] Payload null/empty — gunakan nilai lokal");
    }
  } else {
    Serial.printf("[Threshold Sync] HTTP %d\n", code);
  }
  http.end();
}

// ================================================================
// WiFiManager
// ================================================================
const char CUSTOM_HTML_HEAD[] PROGMEM = R"rawliteral(
<style>
  @import url('https://fonts.googleapis.com/css2?family=Space+Mono:wght@400;700&family=DM+Sans:wght@400;500;600&display=swap');
  *{margin:0;padding:0;box-sizing:border-box}
  body{background:#0a0a0a;color:#f0f0f0;font-family:'DM Sans',sans-serif;
       min-height:100vh;display:flex;align-items:center;justify-content:center;padding:20px}
  .card{background:#111;border:1px solid rgba(255,255,255,0.07);border-radius:18px;
        padding:32px;width:100%;max-width:380px}
  .logo{display:flex;align-items:center;gap:10px;margin-bottom:28px}
  .logo-icon{width:38px;height:38px;background:rgba(0,229,255,0.12);border:1px solid #00e5ff;
             border-radius:8px;display:flex;align-items:center;justify-content:center;font-size:18px}
  .logo-text{font-family:'Space Mono',monospace;font-size:13px;font-weight:700;
             color:#00e5ff;letter-spacing:0.1em}
  .logo-sub{font-size:10px;color:#444;margin-top:2px}
  h1{font-family:'Space Mono',monospace;font-size:16px;font-weight:700;
     color:#f0f0f0;margin-bottom:6px}
  p{font-size:13px;color:#666;margin-bottom:24px}
  label{display:block;font-size:11px;font-weight:600;color:#888;
        letter-spacing:0.05em;text-transform:uppercase;margin-bottom:6px}
  input[type=text],input[type=password],select{
    width:100%;padding:10px 14px;background:#1a1a1a;border:1px solid rgba(255,255,255,0.07);
    border-radius:6px;color:#f0f0f0;font-family:'DM Sans',sans-serif;font-size:14px;
    outline:none;margin-bottom:16px;transition:border-color 0.2s}
  input:focus,select:focus{border-color:#00e5ff;box-shadow:0 0 0 3px rgba(0,229,255,0.12)}
  input[type=submit]{width:100%;padding:11px;background:#00e5ff;color:#000;
    font-weight:700;font-size:14px;border:none;border-radius:6px;cursor:pointer;
    font-family:'DM Sans',sans-serif;transition:background 0.2s;margin-top:4px}
  input[type=submit]:hover{background:#33eaff}
  .divider{height:1px;background:rgba(255,255,255,0.07);margin:20px 0}
  .info-row{display:flex;justify-content:space-between;font-size:12px;
            color:#666;padding:4px 0}
  .info-val{font-family:'Space Mono',monospace;color:#888}
  .badge{display:inline-flex;align-items:center;gap:5px;font-size:11px;
         padding:3px 10px;border-radius:20px;font-family:'Space Mono',monospace;
         background:rgba(0,229,255,0.12);color:#00e5ff;
         border:1px solid rgba(0,229,255,0.3);margin-bottom:20px}
  .reset-btn{display:block;width:100%;padding:10px;margin-top:12px;
    background:rgba(255,23,68,0.1);color:#ff1744;border:1px solid rgba(255,23,68,0.3);
    border-radius:6px;cursor:pointer;font-size:13px;font-family:'DM Sans',sans-serif;
    font-weight:500;text-align:center;text-decoration:none;transition:background 0.2s}
  .reset-btn:hover{background:rgba(255,23,68,0.2)}
</style>
)rawliteral";

const char CUSTOM_HTML_BODY[] PROGMEM = R"rawliteral(
<div class="card">
  <div class="logo">
    <div class="logo-icon">⚡</div>
    <div>
      <div class="logo-text">S·E·M</div>
      <div class="logo-sub">Smart Energy Monitor</div>
    </div>
  </div>
  <div class="badge">● Setup Mode</div>
  <h1>Connect to WiFi</h1>
  <p>Select your WiFi network and enter the password to connect the device.</p>
)rawliteral";

void startWiFiManager() {
  oledStatus("WiFi Setup", "Connect to AP:");
  oledStatus("AP: SEM-Setup", "192.168.4.1");
  digitalWrite(PIN_LED_WIFI, HIGH);

  WiFiManager wm;
  wm.setCustomHeadElement(CUSTOM_HTML_HEAD);
  wm.setCustomMenuHTML(CUSTOM_HTML_BODY);

  WiFiManagerParameter param_threshold("threshold", "Overload Threshold (Watt)",
    String(overloadThreshold, 0).c_str(), 8);
  WiFiManagerParameter param_tarif("tarif", "Tariff per kWh (IDR)",
    String(tarif, 2).c_str(), 10);
  wm.addParameter(&param_threshold);
  wm.addParameter(&param_tarif);

  wm.setConfigPortalTimeout(180);
  wm.setTitle("Smart Energy Monitor");
  wm.setDarkMode(true);

  bool connected = wm.startConfigPortal("SEM-Setup");

  if (connected) {
    float thr = String(param_threshold.getValue()).toFloat();
    float trf  = String(param_tarif.getValue()).toFloat();
    if (thr > 0)  overloadThreshold = thr;
    if (trf > 0)  tarif = trf;
    savePrefs();
    wifiConnected = true;
  } else {
    wifiConnected = false;
  }
}

// ================================================================
// WIFI CONNECT
// ================================================================
bool tryConnectWiFi(int timeoutSeconds) {
  WiFi.mode(WIFI_STA);
  WiFi.begin();
  Serial.print("Connecting WiFi");
  int elapsed = 0;
  while (WiFi.status() != WL_CONNECTED && elapsed < timeoutSeconds * 2) {
    delay(500);
    Serial.print(".");
    blinkState = !blinkState;
    digitalWrite(PIN_LED_WIFI, blinkState);
    elapsed++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi OK: " + WiFi.localIP().toString());
    digitalWrite(PIN_LED_WIFI, HIGH);
    return true;
  }
  Serial.println("\nWiFi gagal");
  digitalWrite(PIN_LED_WIFI, LOW);
  return false;
}

// ================================================================
// NTP
// ================================================================
bool tryNTPSync() {
  configTime(7 * 3600, 0, "pool.ntp.org", "time.nist.gov");
  struct tm ti;
  int retry = 0;
  while (!getLocalTime(&ti) && retry < 20) { delay(500); retry++; }
  return retry < 20;
}

// ================================================================
// FIREBASE SEND
// ================================================================
bool sendToFirebase(float v, float i, float p, float pf, float freq,
                    float kwh, float cost, bool devConn, bool overload,
                    unsigned long ts) {
  if (!wifiConnected || WiFi.status() != WL_CONNECTED) return false;
  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(10000);
  HTTPClient http;
  String url = String(FIREBASE_HOST) + FIREBASE_PATH;
  http.begin(client, url);
  http.addHeader("Content-Type", "application/json");

  float apparentPower = v * i;
  String json = "{";
  json += "\"system\":{";
  json += "\"timestamp\":"  + String(ts)                  + ",";
  json += "\"internet\":true,";
  json += "\"threshold\":"  + String(overloadThreshold, 0) + ",";
  json += "\"tarif\":"      + String(tarif, 2)             + "},";
  json += "\"device\":{";
  json += "\"connected\":"  + String(devConn ? "true" : "false") + ",";
  json += "\"voltage\":"    + String(v,    1)               + ",";
  json += "\"current\":"    + String(i,    2)               + ",";
  json += "\"power\":"      + String(p,    1)               + ",";
  json += "\"apparent\":"   + String(apparentPower, 1)      + ",";
  json += "\"pf\":"         + String(pf,   2)               + ",";
  json += "\"frequency\":"  + String(freq, 1)               + ",";
  json += "\"energy\":"     + String(kwh,  4)               + ",";
  json += "\"cost\":"       + String(cost, 0)               + ",";
  json += "\"overload\":"   + String(overload ? "true" : "false") + "}}";

  int code = http.PUT(json);
  Serial.printf("Firebase: %d\n", code);
  http.end();
  return (code == 200 || code == 204);
}

// ================================================================
// BUZZER
// ================================================================
void handleBuzzer() {
  if (!isOverload) {
    digitalWrite(PIN_BUZZER, LOW);
    buzzerActive    = false;
    buzzerBeepCount = 0;
    buzzerState     = false;
    return;
  }
  unsigned long now = millis();
  if (!buzzerActive) {
    buzzerActive    = true;
    buzzerBeepCount = 0;
    buzzerState     = false;
    lastBuzzerMs    = now;
  }
  if (buzzerBeepCount < BUZZER_BEEPS) {
    unsigned long interval = buzzerState ? BUZZER_ON_MS : BUZZER_OFF_MS;
    if (now - lastBuzzerMs >= interval) {
      buzzerState = !buzzerState;
      digitalWrite(PIN_BUZZER, buzzerState ? HIGH : LOW);
      if (!buzzerState) buzzerBeepCount++;
      lastBuzzerMs = now;
    }
  } else {
    if (now - lastBuzzerMs >= 2000) {
      buzzerBeepCount = 0;
      lastBuzzerMs    = now;
    }
  }
}

// ================================================================
// LED WIFI
// ================================================================
void handleWifiLed() {
  if (wifiConnected && WiFi.status() == WL_CONNECTED) {
    digitalWrite(PIN_LED_WIFI, HIGH);
    return;
  }
  unsigned long now = millis();
  if (now - lastBlinkMs >= 500) {
    blinkState = !blinkState;
    digitalWrite(PIN_LED_WIFI, blinkState);
    lastBlinkMs = now;
  }
}

// ================================================================
// RESET BUTTON
// ================================================================
void checkResetButton() {
  if (digitalRead(PIN_RESET_WIFI) == LOW) {
    unsigned long pressStart = millis();
    oledStatus("Hold to reset", "WiFi credentials");
    while (digitalRead(PIN_RESET_WIFI) == LOW) {
      delay(100);
      if (millis() - pressStart >= 3000) {
        oledStatus("Resetting WiFi...", "");
        WiFiManager wm;
        wm.resetSettings();
        delay(1000);
        ESP.restart();
      }
    }
  }
}

// ================================================================
// OLED
// ================================================================
void oledSplash() {
  display.clearDisplay();
  display.setTextColor(WHITE);
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println("SMART ENERGY");
  display.println("MONITOR v3.0");
  display.drawLine(0, 18, 127, 18, WHITE);
  display.setCursor(0, 24);
  display.println("Initializing...");
  display.display();
}

void oledStatus(const char* l1, const char* l2) {
  display.clearDisplay();
  display.setTextColor(WHITE);
  display.setTextSize(1);
  display.setCursor(0, 20);
  display.println(l1);
  if (strlen(l2) > 0) { display.setCursor(0, 36); display.println(l2); }
  display.display();
}

void oledData(float v, float i, float p, float pf, float freq,
              float kwh, float cost, bool devConn, bool online, bool ovld) {
  display.clearDisplay();
  display.setTextColor(WHITE);
  display.setTextSize(1);

  display.setCursor(0, 0);
  display.print(online ? "[WiFi]" : "[OFFLINE]");
  if (ovld) {
    display.setCursor(80, 0);
    display.print("OVERLOAD!");
  }
  display.drawLine(0, 9, 127, 9, WHITE);

  if (!devConn) {
    display.setCursor(20, 28);
    display.println("No Device");
    display.setCursor(10, 44);
    display.println("Plug in device...");
    display.display();
    return;
  }

  display.setCursor(0, 13);
  display.printf("V:%.1fV  I:%.2fA", v, i);
  display.setCursor(0, 23);
  display.printf("P:%.1fW  PF:%.2f", p, pf);
  display.setCursor(0, 33);
  // FIX #4: Tampilkan threshold saat ini di OLED untuk debug
  display.printf("Hz:%.1f Thr:%.0fW", freq, overloadThreshold);

  display.drawLine(0, 43, 127, 43, WHITE);

  display.setCursor(0, 47);
  display.printf("E:%.4f kWh", kwh);
  display.setCursor(0, 57);
  if (cost >= 1000) {
    display.printf("Rp %.0f", cost);
  } else {
    display.printf("Rp %.1f", cost);
  }
  display.display();
}

// ================================================================
// SETUP
// ================================================================
void setup() {
  Serial.begin(115200);

  pinMode(PIN_LED_WIFI,     OUTPUT);
  pinMode(PIN_LED_OVERLOAD, OUTPUT);
  pinMode(PIN_BUZZER,       OUTPUT);
  pinMode(PIN_RESET_WIFI,   INPUT_PULLUP);
  digitalWrite(PIN_LED_WIFI,     LOW);
  digitalWrite(PIN_LED_OVERLOAD, LOW);
  digitalWrite(PIN_BUZZER,       LOW);

  pzemSerial.begin(9600, SERIAL_8N1, 16, 17);
  loadPrefs();

  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("OLED gagal");
  } else {
    oledSplash();
    delay(2000);
  }

  checkResetButton();

  oledStatus("Connecting WiFi...", "");
  wifiConnected = tryConnectWiFi(20);

  if (!wifiConnected) {
    startWiFiManager();
  }

  if (wifiConnected) {
    WiFi.setSleep(false);
    oledStatus("WiFi OK", "Sync NTP...");
    ntpSynced = tryNTPSync();
    oledStatus("WiFi OK", ntpSynced ? "NTP Synced" : "NTP Gagal");
    delay(1500);

    // FIX #4: Sync threshold dari Firebase segera setelah konek
    Serial.println("Initial threshold sync...");
    syncThresholdFromFirebase();
    Serial.printf("Threshold setelah sync: %.0f W\n", overloadThreshold);
  } else {
    oledStatus("Mode OFFLINE", "Retry in 60s");
    delay(2000);
    lastReconnectMs = millis();
  }

  lastLoopMs          = millis();
  lastThresholdSyncMs = millis();
}

// ================================================================
// LOOP
// ================================================================
void loop() {
  unsigned long now = millis();

  checkResetButton();
  handleWifiLed();
  handleBuzzer();

  // ── Reconnect logic ──
  if (!wifiConnected && now - lastReconnectMs >= RECONNECT_INTERVAL) {
    lastReconnectMs = now;
    oledStatus("Reconnecting...", "");
    wifiConnected = tryConnectWiFi(15);
    if (wifiConnected) {
      WiFi.setSleep(false);
      if (!ntpSynced) ntpSynced = tryNTPSync();
      // FIX #4: Sync threshold setelah reconnect
      syncThresholdFromFirebase();
      oledStatus("WiFi Kembali!", "");
      delay(1000);
    } else {
      oledStatus("Mode OFFLINE", "Retry in 60s");
      delay(1000);
    }
  }

  // ── Deteksi WiFi putus ──
  if (wifiConnected && WiFi.status() != WL_CONNECTED) {
    wifiConnected   = false;
    lastReconnectMs = now;
    digitalWrite(PIN_LED_WIFI, LOW);
  }

  // ── FIX #4: Sync threshold dari Firebase setiap 30 detik ──
  // Ini memungkinkan user ubah threshold di web settings dan
  // ESP32 akan mengambilnya tanpa perlu restart.
  if (wifiConnected && (now - lastThresholdSyncMs >= THRESHOLD_SYNC_INTERVAL)) {
    lastThresholdSyncMs = now;
    syncThresholdFromFirebase();
  }

  // ── Sensor loop ──
  if (now - lastLoopMs < LOOP_INTERVAL) return;
  float deltaT_hours = (float)(now - lastLoopMs) / 3600000.0f;
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

  if (deviceConnected && !prevDevConn) {
    sessionEnergyWh = 0.0f;
    sessionKwh      = 0.0f;
    sessionCost     = 0.0f;
  }
  if (!deviceConnected && prevDevConn) {
    sessionEnergyWh = 0.0f;
    sessionKwh      = 0.0f;
    sessionCost     = 0.0f;
  }
  prevDevConn = deviceConnected;

  if (deviceConnected) {
    sessionEnergyWh += power * deltaT_hours;
    sessionKwh       = sessionEnergyWh / 1000.0f;
    sessionCost      = sessionKwh * tarif;
  }

  // ── FIX #4: Gunakan overloadThreshold yang sudah di-sync dari Firebase ──
  isOverload = deviceConnected && (power >= overloadThreshold);
  digitalWrite(PIN_LED_OVERLOAD, isOverload ? HIGH : LOW);

  Serial.println("===========================");
  Serial.printf("WiFi     : %s\n", wifiConnected ? "Online" : "Offline");
  Serial.printf("Device   : %s\n", deviceConnected ? "Connected" : "Not Connected");
  Serial.printf("V:%.1fV  I:%.2fA\n", voltage, current);
  Serial.printf("P:%.1fW  PF:%.2f  Hz:%.1f\n", power, pf, frequency);
  Serial.printf("E:%.4f kWh  Cost:Rp%.0f\n", sessionKwh, sessionCost);
  // FIX #4: Log threshold yang sedang digunakan
  Serial.printf("Overload : %s (threshold:%.0fW, power:%.1fW)\n",
    isOverload ? "YES ⚠" : "no", overloadThreshold, power);

  oledData(voltage, current, power, pf, frequency,
           sessionKwh, sessionCost, deviceConnected, wifiConnected, isOverload);

  if (wifiConnected) {
    unsigned long ts = ntpSynced ? (unsigned long)time(nullptr) : now / 1000;
    sendToFirebase(voltage, current, power, pf, frequency,
                   sessionKwh, sessionCost, deviceConnected, isOverload, ts);
  }
}