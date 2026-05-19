#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <PZEM004Tv30.h>


#include "credentials.h" // File ini berisi WIFI_SSID dan WIFI_PASSWORD, pastikan sudah dibuat dan diisi

// ================= KONFIGURASI =================
// #define WIFI_SSID     ""
// #define WIFI_PASSWORD ""

// Firebase — ganti dengan project baru kamu
const char* FIREBASE_HOST = "https://smart-energy-monitor-v2-de79d-default-rtdb.asia-southeast1.firebasedatabase.app";
const char* FIREBASE_PATH = "/live.json";
//const char* FIREBASE_AUTH = ""; // Firebase → Project Settings → Service Accounts → Database secret

// ================= OLED =================
#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT 64
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

// ================= PZEM =================
HardwareSerial pzemSerial(2);
PZEM004Tv30 pzem(pzemSerial, 16, 17); // RX=16, TX=17

// ================= TARIF =================
const float TARIF_PLN = 1444.70;

// ================= STATUS =================
bool wifiConnected = false;
bool ntpSynced     = false;

// Reconnect timer — coba reconnect setiap 60 detik saat offline
unsigned long lastReconnectAttempt = 0;
const unsigned long RECONNECT_INTERVAL = 60000; // 60 detik

// ================= WIFI CONNECT =================
bool tryConnectWiFi(int timeoutSeconds = 15) {
  WiFi.disconnect(true);
  delay(200);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting WiFi");
  int elapsed = 0;
  while (WiFi.status() != WL_CONNECTED && elapsed < timeoutSeconds * 2) {
    delay(500);
    Serial.print(".");
    elapsed++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi Connected: " + WiFi.localIP().toString());
    return true;
  }
  Serial.println("\nWiFi gagal");
  WiFi.disconnect(true);
  return false;
}

// ================= NTP SYNC =================
bool tryNTPSync() {
  configTime(7 * 3600, 0, "pool.ntp.org", "time.nist.gov");
  struct tm timeinfo;
  int retry = 0;
  while (!getLocalTime(&timeinfo) && retry < 20) {
    delay(500);
    retry++;
  }
  return (retry < 20);
}

// ================= FIREBASE SEND =================
bool sendToFirebase(
  float voltage, float current, float power,
  float energy,  float cost,
  bool  deviceConnected,
  unsigned long timestamp
) {
  if (!wifiConnected || WiFi.status() != WL_CONNECTED) return false;

  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(10000);

  HTTPClient http;
  String url = String(FIREBASE_HOST) + FIREBASE_PATH;
  http.begin(client, url);
  http.addHeader("Content-Type", "application/json");

  String json = "{"
    "\"system\":{"
      "\"timestamp\":"  + String(timestamp)                          + ","
      "\"internet\":true"
    "},"
    "\"device\":{"
      "\"connected\":"  + String(deviceConnected ? "true" : "false") + ","
      "\"voltage\":"    + String(voltage, 1)                         + ","
      "\"current\":"    + String(current, 2)                         + ","
      "\"power\":"      + String(power,   1)                         + ","
      "\"energy\":"     + String(energy,  3)                         + ","
      "\"cost\":"       + String(cost,    0)                         +
    "}"
  "}";

  int code = http.PUT(json);
  Serial.printf("Firebase: %d\n", code);
  http.end();
  return (code == 200 || code == 204);
}

// ================= OLED =================
void oledSplash() {
  display.clearDisplay();
  display.setTextColor(WHITE);
  display.setTextSize(2);
  display.setCursor(10, 10); display.println("SMART");
  display.setCursor(10, 35); display.println("METER");
  display.display();
}

void oledStatus(const char* line1, const char* line2 = "") {
  display.clearDisplay();
  display.setTextColor(WHITE);
  display.setTextSize(1);
  display.setCursor(0, 20); display.println(line1);
  if (strlen(line2) > 0) {
    display.setCursor(0, 36); display.println(line2);
  }
  display.display();
}

void oledData(float v, float i, float p, float e, bool devConnected, bool online) {
  display.clearDisplay();
  display.setTextColor(WHITE);
  display.setTextSize(1);

  // Status koneksi pojok kanan atas
  if (online) {
    display.setCursor(92, 0); display.print("[WiFi]");
  } else {
    display.setCursor(74, 0); display.print("[OFFLINE]");
  }

  if (!devConnected) {
    display.setCursor(20, 28); display.println("No Device");
    display.setCursor(10, 44); display.println("Plug in device");
    display.display();
    return;
  }

  display.setCursor(0, 10); display.printf("V: %.1f V", v);
  display.setCursor(0, 22); display.printf("I: %.2f A", i);
  display.setCursor(0, 34); display.printf("P: %.1f W", p);
  display.setCursor(0, 46); display.printf("E: %.3f kWh", e);
  display.display();
}

// ================= SETUP =================
void setup() {
  Serial.begin(115200);
  pzemSerial.begin(9600, SERIAL_8N1, 16, 17);

  // OLED init
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("OLED gagal — cek wiring I2C");
  } else {
    oledSplash();
    delay(2000);
  }

  // Coba connect WiFi saat startup
  oledStatus("Connecting WiFi...");
  wifiConnected = tryConnectWiFi(15);

  if (wifiConnected) {
    WiFi.setSleep(false);
    oledStatus("WiFi OK", "Sync NTP...");
    ntpSynced = tryNTPSync();
    Serial.println(ntpSynced ? "NTP synced (WIB)" : "NTP gagal");
    oledStatus("WiFi OK", ntpSynced ? "NTP Synced" : "NTP Gagal");
    delay(1500);
  } else {
    // Mode offline — OLED tetap berjalan
    // Reconnect akan dicoba secara berkala di loop()
    oledStatus("Mode OFFLINE", "Retry in 60s...");
    delay(2000);
    lastReconnectAttempt = millis(); // mulai hitung mundur reconnect
  }
}

// ================= LOOP =================
void loop() {

  // ===== OFFLINE RECONNECT LOGIC =====
  // Jika belum terhubung, coba reconnect setiap RECONNECT_INTERVAL ms
  if (!wifiConnected) {
    unsigned long now = millis();
    if (now - lastReconnectAttempt >= RECONNECT_INTERVAL) {
      lastReconnectAttempt = now;
      Serial.println("Mencoba reconnect WiFi...");
      oledStatus("Reconnecting...", "Please wait");

      wifiConnected = tryConnectWiFi(10);

      if (wifiConnected) {
        WiFi.setSleep(false);
        Serial.println("Reconnect berhasil!");

        // Sync NTP setelah berhasil connect
        if (!ntpSynced) {
          ntpSynced = tryNTPSync();
          Serial.println(ntpSynced ? "NTP synced" : "NTP gagal");
        }

        oledStatus("WiFi Kembali!", "Online mode");
        delay(1500);
      } else {
        Serial.println("Reconnect gagal — lanjut offline");
        oledStatus("Mode OFFLINE", "Retry in 60s...");
        delay(1500);
      }
    }
  }

  // ===== JIKA TERHUBUNG: cek apakah WiFi tiba-tiba putus =====
  if (wifiConnected && WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi terputus mendadak — switch ke offline");
    wifiConnected        = false;
    lastReconnectAttempt = millis(); // mulai hitung countdown reconnect
    oledStatus("WiFi Putus", "Mode OFFLINE");
    delay(1500);
  }

  // ===== BACA SENSOR PZEM =====
  float voltage = pzem.voltage();
  float current = pzem.current();
  float power   = pzem.power();
  float energy  = pzem.energy();

  if (isnan(voltage)) voltage = 0;
  if (isnan(current)) current = 0;
  if (isnan(power))   power   = 0;
  if (isnan(energy))  energy  = 0;

  float cost           = energy * TARIF_PLN;
  // Device dianggap terhubung hanya jika ada arus DAN daya
  // Voltage saja tidak cukup — PLN tetap ada di terminal PZEM meski tidak ada beban
  // Threshold: current > 0.01A dan power > 0.5W untuk hindari noise sensor
  bool deviceConnected = (current > 0.01 && power > 0.5);

  // ===== SERIAL DEBUG =====
  Serial.println("===========================");
  Serial.printf("WiFi    : %s\n", wifiConnected ? "Online" : "Offline");
  Serial.printf("Device  : %s\n", deviceConnected ? "Connected" : "Not Connected");
  Serial.printf("V: %.1f V | I: %.2f A\n", voltage, current);
  Serial.printf("P: %.1f W | E: %.3f kWh\n", power, energy);
  Serial.printf("Cost    : Rp %.0f\n", cost);

  // ===== OLED selalu update (online & offline) =====
  oledData(voltage, current, power, energy, deviceConnected, wifiConnected);

  // ===== KIRIM KE FIREBASE (hanya jika online) =====
  if (wifiConnected) {
    unsigned long timestamp = ntpSynced ? (unsigned long)time(nullptr) : millis() / 1000;
    bool sent = sendToFirebase(voltage, current, power, energy, cost, deviceConnected, timestamp);
    if (!sent) {
      Serial.println("Gagal kirim Firebase — mungkin WiFi baru putus");
      // Akan terdeteksi sebagai putus di iterasi berikutnya
    }
  } else {
    Serial.println("Mode offline — data hanya di OLED");
  }

  delay(5000);
}