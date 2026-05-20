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
const unsigned long LOOP_INTERVAL      = 5000;   // ms — interval baca sensor
const unsigned long RECONNECT_INTERVAL = 60000;  // ms — reconnect WiFi
const unsigned long BUZZER_ON_MS       = 200;    // ms — durasi buzzer beep
const unsigned long BUZZER_OFF_MS      = 300;    // ms — jeda antar beep
const int    BUZZER_BEEPS       = 3;

// ================================================================
// GLOBAL STATE
// ================================================================
bool  wifiConnected     = false;
bool  ntpSynced         = false;
bool  deviceConnected   = false;
bool  isOverload        = false;
bool  prevDevConn       = false;    // deteksi edge connect/disconnect

float overloadThreshold = THRESHOLD_DEFAULT;
float tarif             = TARIF_DEFAULT;

// Energy akumulasi mandiri (tidak pakai pzem.energy() yang akumulatif)
float sessionEnergyWh   = 0.0f;    // Wh sejak device terakhir connect
float sessionKwh        = 0.0f;
float sessionCost       = 0.0f;

unsigned long lastLoopMs        = 0;
unsigned long lastReconnectMs   = 0;
unsigned long lastBuzzerMs      = 0;
int           buzzerBeepCount   = 0;
bool          buzzerActive      = false;
bool          buzzerState       = false;

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
// PREFERENCES — load threshold & tarif dari NVS
// ================================================================
void loadPrefs() {
  prefs.begin("sem", true); // read-only
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
// WiFiManager — custom HTML page (dark theme)
// ================================================================

// CSS & HTML untuk halaman AP WiFiManager
// Diinject ke WiFiManager sebagai custom page
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

// Halaman custom "Info" di WiFiManager — tampilkan threshold & tarif
const char CUSTOM_PAGE_INFO[] PROGMEM = R"rawliteral(
{"title":"Device Info","uri":"/info","menu":true,"first":false}
<div class="card" style="max-width:400px;margin:20px auto">
  <div class="logo">
    <div class="logo-icon">⚡</div>
    <div><div class="logo-text">S·E·M</div><div class="logo-sub">Device Info</div></div>
  </div>
  <div class="divider"></div>
  <div class="info-row"><span>Firmware</span><span class="info-val">v3.0.0</span></div>
  <div class="info-row"><span>Hardware</span><span class="info-val">ESP32 + PZEM-004T</span></div>
  <div class="info-row"><span>Overload Threshold</span><span class="info-val" id="thr">--</span></div>
  <div class="info-row"><span>Tariff</span><span class="info-val" id="trf">--</span></div>
  <div class="divider"></div>
  <a href="/reset" class="reset-btn" onclick="return confirm('Reset WiFi credentials? Device will restart in AP mode.')">
    ✕ Reset WiFi Credentials
  </a>
</div>
<script>
  fetch('/sem-config').then(r=>r.json()).then(d=>{
    document.getElementById('thr').textContent = d.threshold + ' W';
    document.getElementById('trf').textContent = 'Rp ' + d.tarif.toLocaleString('id-ID');
  });
</script>
)rawliteral";

// ================================================================
// WiFiManager setup
// ================================================================
void startWiFiManager() {
  oledStatus("WiFi Setup", "Connect to AP:");
  oledStatus("AP: SEM-Setup", "192.168.4.1");

  // Blink LED WiFi saat AP mode
  digitalWrite(PIN_LED_WIFI, HIGH);

  WiFiManager wm;

  // Custom HTML
  wm.setCustomHeadElement(CUSTOM_HTML_HEAD);
  wm.setCustomMenuHTML(CUSTOM_HTML_BODY);

  // Custom parameter — threshold & tarif bisa diisi saat setup
  WiFiManagerParameter param_threshold("threshold", "Overload Threshold (Watt)", 
    String(overloadThreshold, 0).c_str(), 8);
  WiFiManagerParameter param_tarif("tarif", "Tariff per kWh (IDR)", 
    String(tarif, 2).c_str(), 10);
  wm.addParameter(&param_threshold);
  wm.addParameter(&param_tarif);

  // Timeout AP mode 3 menit
  wm.setConfigPortalTimeout(180);
  wm.setAPCallback([](WiFiManager* wm) {
    Serial.println("AP Mode aktif — SSID: SEM-Setup");
  });
  wm.setSaveConfigCallback([]() {
    Serial.println("Config tersimpan");
  });

  // Dark theme title
  wm.setTitle("Smart Energy Monitor");
  wm.setDarkMode(true);

  bool connected = wm.startConfigPortal("SEM-Setup");

  if (connected) {
    // Simpan parameter ke NVS
    float thr = String(param_threshold.getValue()).toFloat();
    float trf  = String(param_tarif.getValue()).toFloat();
    if (thr > 0)  overloadThreshold = thr;
    if (trf > 0)  tarif = trf;
    savePrefs();
    Serial.println("WiFiManager: berhasil konek");
    wifiConnected = true;
  } else {
    Serial.println("WiFiManager: timeout — lanjut offline");
    wifiConnected = false;
  }
}

// ================================================================
// WIFI CONNECT (normal, pakai kredensial tersimpan di NVS oleh WM)
// ================================================================
bool tryConnectWiFi(int timeoutSeconds) {
  WiFi.mode(WIFI_STA);
  WiFi.begin(); // pakai kredensial tersimpan
  Serial.print("Connecting WiFi");
  int elapsed = 0;
  while (WiFi.status() != WL_CONNECTED && elapsed < timeoutSeconds * 2) {
    delay(500);
    Serial.print(".");
    // Blink LED WiFi saat scanning
    blinkState = !blinkState;
    digitalWrite(PIN_LED_WIFI, blinkState);
    elapsed++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi OK: " + WiFi.localIP().toString());
    digitalWrite(PIN_LED_WIFI, HIGH); // nyala stabil = konek
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
// Kirim semua data: V, I, P, PF, Freq, kWh, Cost, connected, overload
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

  // Hitung apparent power & PF untuk dikirim ke web
  float apparentPower = v * i; // VA
  String json = "{";
  json += "\"system\":{";
  json += "\"timestamp\":"  + String(ts)             + ",";
  json += "\"internet\":true,";
  json += "\"threshold\":"  + String(overloadThreshold, 0) + ",";
  json += "\"tarif\":"      + String(tarif, 2)        + "},";
  json += "\"device\":{";
  json += "\"connected\":"  + String(devConn ? "true" : "false") + ",";
  json += "\"voltage\":"    + String(v,    1)          + ",";
  json += "\"current\":"    + String(i,    2)          + ",";
  json += "\"power\":"      + String(p,    1)          + ",";
  json += "\"apparent\":"   + String(apparentPower, 1) + ",";
  json += "\"pf\":"         + String(pf,   2)          + ",";
  json += "\"frequency\":"  + String(freq, 1)          + ",";
  json += "\"energy\":"     + String(kwh,  4)          + ",";
  json += "\"cost\":"       + String(cost, 0)          + ",";
  json += "\"overload\":"   + String(overload ? "true" : "false") + "}}";

  int code = http.PUT(json);
  Serial.printf("Firebase: %d\n", code);
  http.end();
  return (code == 200 || code == 204);
}

// ================================================================
// BUZZER — non-blocking beep pattern saat overload
// ================================================================
void handleBuzzer() {
  if (!isOverload) {
    digitalWrite(PIN_BUZZER, LOW);
    buzzerActive    = false;
    buzzerBeepCount = 0;
    buzzerState     = false;
    return;
  }
  // Sudah overload — beep BUZZER_BEEPS kali lalu diam 2 detik, ulang
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
    // Jeda 2 detik antar siklus beep
    if (now - lastBuzzerMs >= 2000) {
      buzzerBeepCount = 0;
      lastBuzzerMs    = now;
    }
  }
}

// ================================================================
// LED WIFI — blink saat offline, stabil saat online
// ================================================================
void handleWifiLed() {
  if (wifiConnected && WiFi.status() == WL_CONNECTED) {
    digitalWrite(PIN_LED_WIFI, HIGH);
    return;
  }
  // Blink setiap 500ms saat offline / scanning
  unsigned long now = millis();
  if (now - lastBlinkMs >= 500) {
    blinkState = !blinkState;
    digitalWrite(PIN_LED_WIFI, blinkState);
    lastBlinkMs = now;
  }
}

// ================================================================
// RESET BUTTON — hold PIN_RESET_WIFI selama 3 detik
// ================================================================
void checkResetButton() {
  if (digitalRead(PIN_RESET_WIFI) == LOW) {
    unsigned long pressStart = millis();
    oledStatus("Hold to reset", "WiFi credentials");
    while (digitalRead(PIN_RESET_WIFI) == LOW) {
      delay(100);
      if (millis() - pressStart >= 3000) {
        oledStatus("Resetting WiFi...", "");
        Serial.println("Reset WiFi credentials");
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

  // Status bar atas
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

  // Data utama
  display.setCursor(0, 13);
  display.printf("V:%.1fV  I:%.2fA", v, i);
  display.setCursor(0, 23);
  display.printf("P:%.1fW  PF:%.2f", p, pf);
  display.setCursor(0, 33);
  display.printf("Hz:%.1f", freq);

  // Garis pemisah
  display.drawLine(0, 43, 127, 43, WHITE);

  // Energy & Cost
  display.setCursor(0, 47);
  display.printf("E:%.4f kWh", kwh);
  display.setCursor(0, 57);
  // Format cost — tampilkan Rp + ribuan
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

  // GPIO
  pinMode(PIN_LED_WIFI,     OUTPUT);
  pinMode(PIN_LED_OVERLOAD, OUTPUT);
  pinMode(PIN_BUZZER,       OUTPUT);
  pinMode(PIN_RESET_WIFI,   INPUT_PULLUP);
  digitalWrite(PIN_LED_WIFI,     LOW);
  digitalWrite(PIN_LED_OVERLOAD, LOW);
  digitalWrite(PIN_BUZZER,       LOW);

  // PZEM
  pzemSerial.begin(9600, SERIAL_8N1, 16, 17);

  // Load preferensi dari NVS
  loadPrefs();

  // OLED
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("OLED gagal");
  } else {
    oledSplash();
    delay(2000);
  }

  // Cek tombol reset saat boot
  checkResetButton();

  // Coba konek WiFi tersimpan
  oledStatus("Connecting WiFi...", "");
  wifiConnected = tryConnectWiFi(20);

  if (!wifiConnected) {
    // Tidak ada kredensial atau gagal → buka WiFiManager AP
    Serial.println("Konek gagal — buka WiFiManager");
    startWiFiManager();
  }

  if (wifiConnected) {
    WiFi.setSleep(false);
    oledStatus("WiFi OK", "Sync NTP...");
    ntpSynced = tryNTPSync();
    oledStatus("WiFi OK", ntpSynced ? "NTP Synced" : "NTP Gagal");
    delay(1500);
  } else {
    oledStatus("Mode OFFLINE", "Retry in 60s");
    delay(2000);
    lastReconnectMs = millis();
  }

  lastLoopMs = millis();
}

// ================================================================
// LOOP
// ================================================================
void loop() {
  unsigned long now = millis();

  // ── Tombol reset (non-blocking check) ──────────────────────
  checkResetButton();

  // ── LED WiFi (non-blocking blink) ──────────────────────────
  handleWifiLed();

  // ── Buzzer overload (non-blocking) ─────────────────────────
  handleBuzzer();

  // ── Reconnect logic ────────────────────────────────────────
  if (!wifiConnected && now - lastReconnectMs >= RECONNECT_INTERVAL) {
    lastReconnectMs = now;
    Serial.println("Reconnect attempt...");
    oledStatus("Reconnecting...", "");
    wifiConnected = tryConnectWiFi(15);
    if (wifiConnected) {
      WiFi.setSleep(false);
      if (!ntpSynced) ntpSynced = tryNTPSync();
      oledStatus("WiFi Kembali!", "");
      delay(1000);
    } else {
      oledStatus("Mode OFFLINE", "Retry in 60s");
      delay(1000);
    }
  }

  // ── Deteksi WiFi putus mendadak ────────────────────────────
  if (wifiConnected && WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi putus mendadak");
    wifiConnected   = false;
    lastReconnectMs = now;
    digitalWrite(PIN_LED_WIFI, LOW);
  }

  // ── Sensor loop (setiap LOOP_INTERVAL) ─────────────────────
  if (now - lastLoopMs < LOOP_INTERVAL) return;
  float deltaT_hours = (float)(now - lastLoopMs) / 3600000.0f; // jam
  lastLoopMs = now;

  // Baca PZEM
  float voltage   = pzem.voltage();
  float current   = pzem.current();
  float power     = pzem.power();     // Watt aktif (sudah include PF)
  float pf        = pzem.pf();
  float frequency = pzem.frequency();

  if (isnan(voltage))   voltage   = 0;
  if (isnan(current))   current   = 0;
  if (isnan(power))     power     = 0;
  if (isnan(pf))        pf        = 0;
  if (isnan(frequency)) frequency = 0;

  // ── Deteksi device connect/disconnect ──────────────────────
  // Device dianggap terhubung jika ada arus DAN daya (bukan cuma tegangan PLN)
  deviceConnected = (current > 0.01f && power > 0.5f);

  // Edge: device baru connect → reset energy sesi
  if (deviceConnected && !prevDevConn) {
    Serial.println("Device baru terhubung — reset energy sesi");
    sessionEnergyWh = 0.0f;
    sessionKwh      = 0.0f;
    sessionCost     = 0.0f;
  }
  // Edge: device disconnect → biarkan nilai terakhir (tidak perlu reset, akan 0 sendiri di OLED)
  if (!deviceConnected && prevDevConn) {
    Serial.println("Device dicabut");
    sessionEnergyWh = 0.0f;
    sessionKwh      = 0.0f;
    sessionCost     = 0.0f;
  }
  prevDevConn = deviceConnected;

  // ── Akumulasi energy (hanya saat device terhubung) ─────────
  // Rumus: E(Wh) += P(W) × ΔT(jam)
  // Konversi: kWh = Wh ÷ 1000
  // Cost: Cost = kWh × tarif → dibulatkan
  if (deviceConnected) {
    sessionEnergyWh += power * deltaT_hours;
    sessionKwh       = sessionEnergyWh / 1000.0f;
    sessionCost      = sessionKwh * tarif;
  }

  // ── Cek threshold overload dari Firebase ───────────────────
  // (threshold juga bisa di-update dari web settings,
  //  dibaca dari Firebase node system.threshold setiap loop)
  // Untuk sekarang pakai nilai lokal dari NVS
  isOverload = deviceConnected && (power > overloadThreshold);
  digitalWrite(PIN_LED_OVERLOAD, isOverload ? HIGH : LOW);

  // Serial debug
  Serial.println("===========================");
  Serial.printf("WiFi     : %s\n", wifiConnected ? "Online" : "Offline");
  Serial.printf("Device   : %s\n", deviceConnected ? "Connected" : "Not Connected");
  Serial.printf("V:%.1fV  I:%.2fA\n", voltage, current);
  Serial.printf("P:%.1fW  PF:%.2f  Hz:%.1f\n", power, pf, frequency);
  Serial.printf("E:%.4f kWh  Cost:Rp%.0f\n", sessionKwh, sessionCost);
  Serial.printf("Overload : %s (threshold:%.0fW)\n",
    isOverload ? "YES" : "no", overloadThreshold);

  // ── Update OLED ─────────────────────────────────────────────
  oledData(voltage, current, power, pf, frequency,
           sessionKwh, sessionCost, deviceConnected, wifiConnected, isOverload);

  // ── Kirim ke Firebase ───────────────────────────────────────
  if (wifiConnected) {
    unsigned long ts = ntpSynced ? (unsigned long)time(nullptr) : now / 1000;
    sendToFirebase(voltage, current, power, pf, frequency,
                   sessionKwh, sessionCost, deviceConnected, isOverload, ts);
  } else {
    Serial.println("Offline — data hanya OLED");
  }
}