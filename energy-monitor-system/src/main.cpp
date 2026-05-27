/*
 * Smart Energy Monitor v3.1 — main.cpp
 *
 * File ini HANYA berisi setup() dan loop().
 * Semua logika dipecah ke modul terpisah:
 *
 *   config.h      — konstanta, pin define, path
 *   state.h/.cpp  — global variable
 *   storage.h/.cpp — LittleFS + Preferences
 *   display.h/.cpp — OLED, LED, buzzer
 *   firebase.h/.cpp — komunikasi Firebase
 *   session.h/.cpp  — relay, overload, mode transition
 *   network.h/.cpp  — WiFi, NTP, WebServer
 */

#include "config.h"
#include "state.h"
#include "storage.h"
#include "display.h"
#include "firebase.h"
#include "session.h"
#include "network.h"

#include <WiFi.h>
#include <LittleFS.h>
#include <PZEM004Tv30.h>

// ── Hardware objects (hanya didefinisikan di sini) ───────────────
HardwareSerial pzemSerial(2);
PZEM004Tv30 pzem(pzemSerial, 16, 17);

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

    // Load persisted values
    loadPrefs();
    Serial.printf("[Prefs] threshold=%.0fW tarif=%.2f uid=%s sessionId=%s\n",
                  overloadThreshold, tarif, currentUid, currentSessionId);

    // LittleFS
    fsInit();

    // OLED
    if (displayInit()) {
        oledSplash();
        delay(2000);
    }

    // Cek tombol reset sebelum WiFi
    checkResetButton();

    // WiFi AP + webserver
    WiFi.mode(WIFI_AP_STA);
    startLocalAP();
    setupWebServer();

    oledStatus("AP: SEM-Setup", "pw: 12345678");
    delay(1500);

    // ── Detect startup mode ──────────────────────────────────────
    oledStatus("Checking WiFi...", "");
    wifiConnected = tryConnectWiFi(WIFI_OFFLINE_GRACE_MS / 1000UL);

    if (wifiConnected) {
        // ★ ONLINE MODE
        Serial.println("[Boot] ★ ONLINE MODE");
        modeOffline = false;
        WiFi.setSleep(false);
        oledStatus("WiFi OK ✓", "Sync NTP...");
        ntpSynced = tryNTPSync();
        delay(500);
        syncThresholdFromFirebase();
        clearFirebaseCommand();
        doSessionRecovery();
        fsSyncOfflineHistoryToFirebase();

        unsigned long ts = ntpSynced ? (unsigned long)time(nullptr) : millis() / 1000;
        sendToFirebase(0, 0, 0, 0, 0, sessionKwh, sessionCost,
                       false, false, relayOn, ts);

        oledStatus("Online Ready ✓", relayOn ? sessionDeviceName : "Waiting for web...");
        delay(1000);
        Serial.printf("[Boot-Online] Relay %s\n", relayOn ? "ON (recovered)" : "OFF");

    } else {
        // ★ OFFLINE MODE
        Serial.println("[Boot] ★ OFFLINE MODE");
        modeOffline    = true;
        offlineStartMs = millis();
        lastModeTransitionMs = millis();

        doSessionRecovery();

        lastReconnectMs = millis();
        oledStatus("Offline Mode ✓", relayOn ? sessionDeviceName : "Idle");
        delay(1500);
        Serial.printf("[Boot-Offline] Relay %s\n", relayOn ? "ON (recovered)" : "OFF");
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
    static unsigned long wifiLostSinceMs = 0;

    // ── Serve web & button ───────────────────────────────────────
    networkHandleClients();
    checkResetButton();

    // ── LED & buzzer ─────────────────────────────────────────────
    handleBlueLed();
    handleGreenLed();
    handleOverloadAlert();

    // ── WiFi disconnect → offline mode ───────────────────────────
    if (wifiConnected && WiFi.status() != WL_CONNECTED) {
        Serial.println("[WiFi] ✗ Disconnected");
        wifiConnected = false;
        digitalWrite(PIN_LED_BLUE, LOW);
        lastReconnectMs = now;
        wifiLostSinceMs = now;
        if (sessionActive && hadDataOnce) fsWriteSession();
        Serial.println("[WiFi] Waiting grace period before offline mode");
    }

    if (!wifiConnected && !modeOffline && wifiLostSinceMs > 0 &&
        (now - wifiLostSinceMs >= WIFI_OFFLINE_GRACE_MS)) {
        transitionToOfflineMode("wifi lost for 5 minutes");
    }

    // ── Auto-reconnect setiap 60s ─────────────────────────────────
    if (!wifiConnected && (now - lastReconnectMs >= RECONNECT_INTERVAL)) {
        lastReconnectMs = now;
        Serial.println("[WiFi] Trying reconnect...");
        if (tryConnectWiFi(15)) {
            if (modeOffline) transitionToOnlineMode();
            else wifiConnected = true;
            wifiLostSinceMs = 0;
            WiFi.setSleep(false);
            if (!ntpSynced) ntpSynced = tryNTPSync();
            syncThresholdFromFirebase();
            int pending = fsCountOfflineHistory();
            if (pending > 0) {
                Serial.printf("[Reconnect] Syncing %d pending sessions\n", pending);
                fsSyncOfflineHistoryToFirebase();
            }
            unsigned long ts = ntpSynced ? (unsigned long)time(nullptr) : now / 1000;
            sendToFirebase(lastV, lastI, lastP, lastPF, lastHz,
                           sessionKwh, sessionCost,
                           deviceConnected, isOverload, relayOn, ts);
        }
    }

    // ── Threshold sync (online) ───────────────────────────────────
    if (wifiConnected && (now - lastThresholdSyncMs >= THRESHOLD_SYNC_INTERVAL)) {
        lastThresholdSyncMs = now;
        syncThresholdFromFirebase();
    }

    // ── Command poll (online, non-offline mode) ───────────────────
    if (wifiConnected && !modeOffline && (now - lastCommandPollMs >= COMMAND_POLL_INTERVAL)) {
        lastCommandPollMs = now;
        pollCommandFromFirebase();
    }

    // ── Offline history retry sync ────────────────────────────────
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

    // ── Sensor loop setiap 5s ────────────────────────────────────
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

    // Recovery, disconnect & overload
    handleRecoveredSessionCheck();
    handleDeviceDisconnect();
    handleOverload(power);

    // Energy accumulation
    if (deviceConnected && relayOn && !isOverload) {
        sessionEnergyWh += power * dT;
        sessionKwh       = sessionEnergyWh / 1000.0f;
        sessionCost      = sessionKwh * tarif;
    }

    // Checkpoint
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

    // OLED
    unsigned long offMs = modeOffline ? (now - offlineStartMs) : 0;
    oledData(voltage, current, power, pf, frequency,
             sessionKwh, sessionCost,
             deviceConnected, wifiConnected,
             isOverload, relayOn, modeOffline, offMs);

    // Send to Firebase
    if (wifiConnected) {
        unsigned long ts = ntpSynced ? (unsigned long)time(nullptr) : now / 1000;
        sendToFirebase(voltage, current, power, pf, frequency,
                       sessionKwh, sessionCost,
                       deviceConnected, isOverload, relayOn, ts);
    }
}
