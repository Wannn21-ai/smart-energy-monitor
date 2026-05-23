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
#define PIN_LED_WIFI     2    // LED biru  — WiFi status
#define PIN_LED_GREEN    25   // RGB Green — sistem online
#define PIN_LED_RED      26   // RGB Red   — overload
#define PIN_BUZZER       5    // Buzzer    — overload alert
#define PIN_RELAY        27   // Relay     — kontrol stopkontak (Active LOW)
#define PIN_RESET_WIFI   0    // Tombol BOOT — hold 3 detik reset WiFi

// Active LOW: LOW=ON (listrik mengalir), HIGH=OFF (terputus)
#define RELAY_ON  LOW
#define RELAY_OFF HIGH

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
const float  TARIF_DEFAULT            = 1444.70f;
const float  THRESHOLD_DEFAULT        = 2000.0f;
const unsigned long LOOP_INTERVAL           = 5000;
const unsigned long RECONNECT_INTERVAL      = 60000;
const unsigned long THRESHOLD_SYNC_INTERVAL = 30000;
const unsigned long COMMAND_POLL_INTERVAL   = 2000;
const unsigned long BUZZER_ON_MS            = 200;
const unsigned long BUZZER_OFF_MS           = 300;
const int    BUZZER_BEEPS             = 3;

// ================================================================
// GLOBAL STATE
// ================================================================
bool  wifiConnected   = false;
bool  ntpSynced       = false;
bool  deviceConnected = false;
bool  isOverload      = false;
bool  prevDevConn     = false;
bool  relayOn         = false; // BOOT: relay OFF, tunggu perintah web

float overloadThreshold = THRESHOLD_DEFAULT;
float tarif             = TARIF_DEFAULT;
float sessionEnergyWh   = 0.0f;
float sessionKwh        = 0.0f;
float sessionCost       = 0.0f;

unsigned long lastLoopMs          = 0;
unsigned long lastReconnectMs     = 0;
unsigned long lastThresholdSyncMs = 0;
unsigned long lastCommandPollMs   = 0;
unsigned long lastBuzzerMs        = 0;
int           buzzerBeepCount     = 0;
bool          buzzerActive        = false;
bool          buzzerState         = false;
unsigned long lastBlinkMs         = 0;
bool          blinkState          = false;

// ================================================================
// FORWARD DECLARATIONS
// ================================================================
bool tryConnectWiFi(int timeoutSeconds = 20);
bool tryNTPSync();
bool sendToFirebase(float v, float i, float p, float pf, float freq,
                    float kwh, float cost, bool devConn, bool overload,
                    bool relay, unsigned long ts);
void syncThresholdFromFirebase();
void pollCommandFromFirebase();
void clearFirebaseCommand();
void setRelay(bool on);
void oledSplash();
void oledStatus(const char* l1, const char* l2 = "");
void oledData(float v, float i, float p, float pf, float freq,
              float kwh, float cost, bool devConn, bool online, bool ovld, bool relay);
void handleBuzzer();
void handleWifiLed();
void handleRgbLed();
void checkResetButton();
void loadPrefs();
void savePrefs();
void startWiFiManager();

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
void setRelay(bool on) {
  relayOn = on;
  digitalWrite(PIN_RELAY, on ? RELAY_ON : RELAY_OFF);
  Serial.printf("[Relay] %s\n", on ? "ON - listrik mengalir" : "OFF - listrik terputus");
}

// ================================================================
// CLEAR FIREBASE COMMAND
// ================================================================
void clearFirebaseCommand() {
  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(5000);
  HTTPClient http;
  if (http.begin(client, String(FIREBASE_HOST) + "/command/relay.json")) {
    http.addHeader("Content-Type", "application/json");
    http.PUT("null");
    http.end();
  }
}

// ================================================================
// POLL COMMAND DARI FIREBASE
// Web tulis /command/relay: true atau false
// ESP32 polling tiap 2 detik, eksekusi lalu clear
// ================================================================
void pollCommandFromFirebase() {
  if (!wifiConnected || WiFi.status() != WL_CONNECTED) return;
  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(5000);
  HTTPClient http;
  if (!http.begin(client, String(FIREBASE_HOST) + "/command/relay.json")) return;
  int code = http.GET();
  if (code == 200) {
    String payload = http.getString();
    payload.trim();
    http.end();
    if (payload == "true") {
      Serial.println("[Command] Relay ON dari web");
      setRelay(true);
      clearFirebaseCommand();
    } else if (payload == "false") {
      Serial.println("[Command] Relay OFF dari web");
      setRelay(false);
      // Reset akumulasi sesi lokal saat relay dimatikan
      sessionEnergyWh = 0.0f;
      sessionKwh      = 0.0f;
      sessionCost     = 0.0f;
      prevDevConn     = false;
      clearFirebaseCommand();
    }
    // "null" = tidak ada command baru
  } else {
    http.end();
  }
}

// ================================================================
// SYNC THRESHOLD
// ================================================================
void syncThresholdFromFirebase() {
  if (!wifiConnected || WiFi.status() != WL_CONNECTED) return;
  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(8000);
  HTTPClient http;
  if (!http.begin(client, String(FIREBASE_HOST) + "/config/threshold.json")) return;
  http.addHeader("Content-Type", "application/json");
  int code = http.GET();
  if (code == 200) {
    String payload = http.getString();
    payload.trim();
    if (payload != "null" && payload.length() > 0) {
      float newThr = payload.toFloat();
      if (newThr > 0 && newThr != overloadThreshold) {
        overloadThreshold = newThr;
        savePrefs();
        Serial.printf("[Threshold] Updated: %.0f W\n", newThr);
      }
    }
  }
  http.end();
}

// ================================================================
// WIFI MANAGER
// ================================================================
const char CUSTOM_HTML_HEAD[] PROGMEM = R"rawliteral(
<style>
  *{margin:0;padding:0;box-sizing:border-box}
  body{background:#0a0a0a;color:#f0f0f0;font-family:sans-serif;
       min-height:100vh;display:flex;align-items:center;justify-content:center;padding:20px}
  .card{background:#111;border:1px solid rgba(255,255,255,0.07);
        border-radius:18px;padding:32px;width:100%;max-width:380px}
  h1{font-size:16px;font-weight:700;color:#f0f0f0;margin-bottom:6px}
  p{font-size:13px;color:#666;margin-bottom:24px}
  label{display:block;font-size:11px;font-weight:600;color:#888;
        letter-spacing:0.05em;text-transform:uppercase;margin-bottom:6px}
  input[type=text],input[type=password]{
    width:100%;padding:10px 14px;background:#1a1a1a;
    border:1px solid rgba(255,255,255,0.07);border-radius:6px;
    color:#f0f0f0;font-size:14px;outline:none;margin-bottom:16px}
  input[type=submit]{width:100%;padding:11px;background:#00e5ff;color:#000;
    font-weight:700;font-size:14px;border:none;border-radius:6px;cursor:pointer}
</style>
)rawliteral";

const char CUSTOM_HTML_BODY[] PROGMEM = R"rawliteral(
<div class="card">
  <h1>Smart Energy Monitor</h1>
  <p>Pilih WiFi dan masukkan password untuk menghubungkan perangkat.</p>
)rawliteral";

void startWiFiManager() {
  oledStatus("WiFi Setup", "AP: SEM-Setup");
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
    if (thr > 0) overloadThreshold = thr;
    if (trf > 0) tarif = trf;
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
    delay(500); Serial.print(".");
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
                    bool relay, unsigned long ts) {
  if (!wifiConnected || WiFi.status() != WL_CONNECTED) return false;
  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(10000);
  HTTPClient http;
  http.begin(client, String(FIREBASE_HOST) + FIREBASE_PATH);
  http.addHeader("Content-Type", "application/json");
  String json = "{";
  json += "\"system\":{";
  json += "\"timestamp\":"  + String(ts)                   + ",";
  json += "\"internet\":true,";
  json += "\"threshold\":"  + String(overloadThreshold, 0) + ",";
  json += "\"tarif\":"      + String(tarif, 2)             + ",";
  json += "\"relay\":"      + (relay ? "true" : "false")   + "},";
  json += "\"device\":{";
  json += "\"connected\":"  + (devConn  ? "true" : "false") + ",";
  json += "\"voltage\":"    + String(v,    1)               + ",";
  json += "\"current\":"    + String(i,    2)               + ",";
  json += "\"power\":"      + String(p,    1)               + ",";
  json += "\"apparent\":"   + String(v * i, 1)              + ",";
  json += "\"pf\":"         + String(pf,   2)               + ",";
  json += "\"frequency\":"  + String(freq, 1)               + ",";
  json += "\"energy\":"     + String(kwh,  4)               + ",";
  json += "\"cost\":"       + String(cost, 0)               + ",";
  json += "\"overload\":"   + (overload ? "true" : "false") + "}}";
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
    buzzerActive = false; buzzerBeepCount = 0; buzzerState = false;
    return;
  }
  unsigned long now = millis();
  if (!buzzerActive) {
    buzzerActive = true; buzzerBeepCount = 0; buzzerState = false; lastBuzzerMs = now;
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
    if (now - lastBuzzerMs >= 2000) { buzzerBeepCount = 0; lastBuzzerMs = now; }
  }
}

// ================================================================
// LED WiFi — blink saat connecting, solid saat online
// ================================================================
void handleWifiLed() {
  if (wifiConnected && WiFi.status() == WL_CONNECTED) {
    digitalWrite(PIN_LED_WIFI, HIGH); return;
  }
  unsigned long now = millis();
  if (now - lastBlinkMs >= 500) {
    blinkState = !blinkState;
    digitalWrite(PIN_LED_WIFI, blinkState);
    lastBlinkMs = now;
  }
}

// ================================================================
// RGB LED
// ================================================================
void handleRgbLed() {
  digitalWrite(PIN_LED_GREEN, (wifiConnected && WiFi.status() == WL_CONNECTED) ? HIGH : LOW);
  digitalWrite(PIN_LED_RED,   isOverload ? HIGH : LOW);
}

// ================================================================
// RESET BUTTON
// ================================================================
void checkResetButton() {
  if (digitalRead(PIN_RESET_WIFI) != LOW) return;
  unsigned long pressStart = millis();
  oledStatus("Hold to reset", "WiFi credentials");
  while (digitalRead(PIN_RESET_WIFI) == LOW) {
    delay(100);
    if (millis() - pressStart >= 3000) {
      oledStatus("Resetting...", "");
      WiFiManager wm; wm.resetSettings();
      delay(1000); ESP.restart();
    }
  }
}

// ================================================================
// OLED
// ================================================================
void oledSplash() {
  display.clearDisplay();
  display.setTextColor(WHITE); display.setTextSize(1);
  display.setCursor(0, 0);  display.println("SMART ENERGY");
  display.setCursor(0, 10); display.println("MONITOR v3.0");
  display.drawLine(0, 20, 127, 20, WHITE);
  display.setCursor(0, 26); display.println("Initializing...");
  display.display();
}

void oledStatus(const char* l1, const char* l2) {
  display.clearDisplay();
  display.setTextColor(WHITE); display.setTextSize(1);
  display.setCursor(0, 20); display.println(l1);
  if (strlen(l2) > 0) { display.setCursor(0, 36); display.println(l2); }
  display.display();
}

void oledData(float v, float i, float p, float pf, float freq,
              float kwh, float cost, bool devConn, bool online, bool ovld, bool relay) {
  display.clearDisplay();
  display.setTextColor(WHITE); display.setTextSize(1);
  display.setCursor(0, 0);
  display.print(online ? "[WiFi]" : "[OFFLINE]");
  display.setCursor(72, 0);
  display.print(relay ? "[RLY:ON]" : "[RLY:OFF]");
  display.drawLine(0, 9, 127, 9, WHITE);
  if (ovld) { display.setCursor(0, 0); display.print("!! OVERLOAD !!"); }
  if (!relay) {
    display.setCursor(15, 26); display.println("Relay OFF");
    display.setCursor(0, 42);  display.println("Klik web utk mulai");
    display.display(); return;
  }
  if (!devConn) {
    display.setCursor(15, 26); display.println("No Device");
    display.setCursor(5, 42);  display.println("Colokkan beban...");
    display.display(); return;
  }
  display.setCursor(0, 13); display.printf("V:%.1fV  I:%.2fA", v, i);
  display.setCursor(0, 23); display.printf("P:%.1fW  PF:%.2f", p, pf);
  display.setCursor(0, 33); display.printf("Hz:%.1f Thr:%.0fW", freq, overloadThreshold);
  display.drawLine(0, 43, 127, 43, WHITE);
  display.setCursor(0, 47); display.printf("E:%.4f kWh", kwh);
  display.setCursor(0, 57);
  if (cost >= 1000) display.printf("Rp %.0f", cost);
  else              display.printf("Rp %.1f", cost);
  display.display();
}

// ================================================================
// SETUP
// ================================================================
void setup() {
  Serial.begin(115200);
  pinMode(PIN_LED_WIFI,   OUTPUT);
  pinMode(PIN_LED_GREEN,  OUTPUT);
  pinMode(PIN_LED_RED,    OUTPUT);
  pinMode(PIN_BUZZER,     OUTPUT);
  pinMode(PIN_RELAY,      OUTPUT);
  pinMode(PIN_RESET_WIFI, INPUT_PULLUP);
  digitalWrite(PIN_LED_WIFI,  LOW);
  digitalWrite(PIN_LED_GREEN, LOW);
  digitalWrite(PIN_LED_RED,   LOW);
  digitalWrite(PIN_BUZZER,    LOW);

  // BOOT: relay OFF - tunggu "Siapkan Pengukuran Baru" dari web
  setRelay(false);
  // Green nyala segera - tanda ESP32 hidup
  digitalWrite(PIN_LED_GREEN, HIGH);

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
    digitalWrite(PIN_LED_GREEN, LOW);
    startWiFiManager();
  }

  if (wifiConnected) {
    WiFi.setSleep(false);
    digitalWrite(PIN_LED_GREEN, HIGH);
    oledStatus("WiFi OK", "Sync NTP...");
    ntpSynced = tryNTPSync();
    oledStatus("WiFi OK", ntpSynced ? "NTP Synced" : "NTP Gagal");
    delay(1500);
    syncThresholdFromFirebase();
    // Clear command lama supaya relay tidak bereaksi terhadap command sisa
    clearFirebaseCommand();
    Serial.println("[Boot] Firebase command di-clear, relay OFF, siap terima perintah");
    // Kirim status awal ke Firebase
    unsigned long ts = ntpSynced ? (unsigned long)time(nullptr) : millis() / 1000;
    sendToFirebase(0, 0, 0, 0, 0, 0, 0, false, false, false, ts);
  } else {
    digitalWrite(PIN_LED_GREEN, LOW);
    oledStatus("Mode OFFLINE", "Retry in 60s");
    delay(2000);
    lastReconnectMs = millis();
  }

  lastLoopMs          = millis();
  lastThresholdSyncMs = millis();
  lastCommandPollMs   = millis();
}

// ================================================================
// LOOP
// ================================================================
void loop() {
  unsigned long now = millis();

  checkResetButton();
  handleWifiLed();
  handleBuzzer();
  handleRgbLed();

  // ── Reconnect ──
  if (!wifiConnected && now - lastReconnectMs >= RECONNECT_INTERVAL) {
    lastReconnectMs = now;
    oledStatus("Reconnecting...", "");
    wifiConnected = tryConnectWiFi(15);
    if (wifiConnected) {
      WiFi.setSleep(false);
      if (!ntpSynced) ntpSynced = tryNTPSync();
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
    wifiConnected = false;
    lastReconnectMs = now;
    digitalWrite(PIN_LED_WIFI, LOW);
  }

  // ── Sync threshold setiap 30 detik ──
  if (wifiConnected && now - lastThresholdSyncMs >= THRESHOLD_SYNC_INTERVAL) {
    lastThresholdSyncMs = now;
    syncThresholdFromFirebase();
  }

  // ── Poll command relay dari Firebase setiap 2 detik ──
  if (wifiConnected && now - lastCommandPollMs >= COMMAND_POLL_INTERVAL) {
    lastCommandPollMs = now;
    pollCommandFromFirebase();
  }

  // ── Sensor loop setiap 5 detik ──
  if (now - lastLoopMs < LOOP_INTERVAL) return;
  float deltaT_hours = (float)(now - lastLoopMs) / 3600000.0f;
  lastLoopMs = now;

  // Relay OFF: kirim status ke Firebase lalu skip baca sensor
  if (!relayOn) {
    if (wifiConnected) {
      unsigned long ts = ntpSynced ? (unsigned long)time(nullptr) : now / 1000;
      sendToFirebase(0, 0, 0, 0, 0, 0, 0, false, false, false, ts);
    }
    oledData(0, 0, 0, 0, 0, 0, 0, false, wifiConnected, false, false);
    return;
  }

  // ── Baca sensor PZEM ──
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
    sessionEnergyWh = 0.0f; sessionKwh = 0.0f; sessionCost = 0.0f;
  }
  if (!deviceConnected && prevDevConn) {
    sessionEnergyWh = 0.0f; sessionKwh = 0.0f; sessionCost = 0.0f;
  }
  prevDevConn = deviceConnected;

  if (deviceConnected) {
    sessionEnergyWh += power * deltaT_hours;
    sessionKwh       = sessionEnergyWh / 1000.0f;
    sessionCost      = sessionKwh * tarif;
  }

  isOverload = deviceConnected && (power >= overloadThreshold);

  Serial.println("===========================");
  Serial.printf("WiFi    : %s\n",  wifiConnected   ? "Online"     : "Offline");
  Serial.printf("Relay   : %s\n",  relayOn         ? "ON"         : "OFF");
  Serial.printf("Device  : %s\n",  deviceConnected ? "Connected"  : "Not Connected");
  Serial.printf("V:%.1fV  I:%.2fA  P:%.1fW\n", voltage, current, power);
  Serial.printf("PF:%.2f  Hz:%.1f\n", pf, frequency);
  Serial.printf("E:%.4f kWh  Cost:Rp%.0f\n", sessionKwh, sessionCost);
  Serial.printf("Overload: %s (%.0fW thr / %.1fW aktual)\n",
    isOverload ? "YES" : "no", overloadThreshold, power);

  oledData(voltage, current, power, pf, frequency,
           sessionKwh, sessionCost, deviceConnected, wifiConnected, isOverload, relayOn);

  if (wifiConnected) {
    unsigned long ts = ntpSynced ? (unsigned long)time(nullptr) : now / 1000;
    sendToFirebase(voltage, current, power, pf, frequency,
                   sessionKwh, sessionCost, deviceConnected, isOverload, relayOn, ts);
  }
}