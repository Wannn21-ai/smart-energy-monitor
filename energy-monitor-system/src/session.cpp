// ================================================================
// session.cpp — Smart Energy Monitor v3.1
// Logika bisnis: relay, overload, disconnect, mode transition,
// session recovery, dan tombol BOOT (GPIO 0)
// ================================================================

#include "session.h"
#include "config.h"
#include "state.h"
#include "storage.h"
#include "firebase.h"
#include "display.h"

#include <WiFi.h>
#include <time.h>

// ================================================================
// UTILITY HELPERS
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
// RELAY — single entry point, atomic state update
// ================================================================
void setRelay(bool on, const char* reason) {
    relayOn = on;
    digitalWrite(PIN_RELAY, on ? RELAY_ON : RELAY_OFF);
    sessionActive = on;
    if (!on) {
        disconnectCount = 0;
        recoveredSessionPending = false;
        recoveredNoDeviceCount = 0;
        fsClearSession();
    }
    Serial.printf("[Relay] %s — %s\n", on ? "ON" : "OFF", reason);
}

// ================================================================
// MODE TRANSITION → ONLINE
// ================================================================
static void archiveCurrentSession(unsigned long nowTs, float power, bool recovered, bool wasOverload) {
    if (strlen(sessionDeviceName) == 0) strlcpy(sessionDeviceName, "Device", sizeof(sessionDeviceName));
    String dur = buildDuration(sessionStartTs, nowTs);

    if (wifiConnected) {
        bool ok = pushHistoryToFirebase(sessionDeviceName, dur.c_str(),
                                       power, sessionKwh, sessionCost,
                                       nowTs, recovered, wasOverload);
        Serial.printf("[Session] History push: %s\n", ok ? "OK" : "FAIL");
        if (!ok) {
            fsAppendOfflineHistory(sessionDeviceName, sessionStartTs, nowTs,
                                  sessionKwh, sessionCost, power, wasOverload);
        }
    } else {
        fsAppendOfflineHistory(sessionDeviceName, sessionStartTs, nowTs,
                              sessionKwh, sessionCost, power, wasOverload);
        Serial.println("[Session] History queued offline");
    }
}

void transitionToOnlineMode() {
    if (!modeOffline) return;

    unsigned long now = millis();
    if (now - lastModeTransitionMs < MODE_TRANSITION_DEBOUNCE) {
        Serial.println("[Mode] Debounced rapid transition (→ Online)");
        return;
    }
    lastModeTransitionMs = now;

    Serial.println("[Mode] ▶ → ONLINE MODE");

    if (sessionActive && hadDataOnce) {
        if (fsWriteSession()) {
            Serial.println("[Mode→Online] Checkpoint disimpan");
        }
    }

    modeOffline   = false;
    wifiConnected = true;
    offlineStartMs = 0;

    Serial.println("[Mode→Online] State set, akan sync history...");
    fsSyncOfflineHistoryToFirebase();
    clearFirebaseCommand();
}

// ================================================================
// MODE TRANSITION → OFFLINE
// ================================================================
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

    if (relayOn) {
        if (strlen(sessionDeviceName) == 0) generateOfflineDeviceName();
        Serial.printf("[Mode→Offline] Relay tetap ON — device: %s\n", sessionDeviceName);
    } else {
        Serial.println("[Mode→Offline] Idle, relay tetap OFF");
    }
}

// ================================================================
// OFFLINE SESSION — start new
// ================================================================
void startOfflineSession(const char* reason) {
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
    if (strlen(sessionDeviceName) > 0) return;
    int count = fsCountOfflineHistory();
    if (offlineDeviceCounter <= count) offlineDeviceCounter = count + 1;
    else offlineDeviceCounter++;
    snprintf(sessionDeviceName, sizeof(sessionDeviceName),
             "Device %d", offlineDeviceCounter);
    Serial.printf("[Offline] Nama device: %s\n", sessionDeviceName);
}

// ================================================================
// SESSION RECOVERY (dipanggil saat boot)
// ================================================================
void doSessionRecovery() {
    float recoveredEnergyWh = 0, recoveredKwh = 0, recoveredCost = 0;
    char  recoveredName[32] = "";
    unsigned long recoveredStartTs = 0;
    unsigned long recoveredElapsedSec = 0;

    if (!fsReadSession(recoveredEnergyWh, recoveredKwh, recoveredCost,
                       recoveredName, recoveredStartTs, recoveredElapsedSec)) return;

    Serial.printf("[Recovery] Ditemukan: '%s' %.4f kWh\n", recoveredName, recoveredKwh);
    oledStatus("Recovery sesi", recoveredName);
    delay(1500);

    unsigned long nowTs = ntpSynced ? (unsigned long)time(nullptr) : millis() / 1000;
    if ((recoveredStartTs == 0 || recoveredStartTs > nowTs) && recoveredElapsedSec > 0) {
        recoveredStartTs = nowTs > recoveredElapsedSec ? nowTs - recoveredElapsedSec : nowTs;
    }

    strlcpy(sessionDeviceName, recoveredName, sizeof(sessionDeviceName));
    sessionStartTs      = recoveredStartTs;
    sessionEnergyWh     = recoveredEnergyWh;
    sessionKwh          = recoveredKwh;
    sessionCost         = recoveredCost;
    hadDataOnce         = true;
    disconnectCount     = 0;
    isOverload          = false;
    overloadAlertLinger = false;
    recoveredSessionPending = true;
    recoveredNoDeviceCount  = 0;

    setRelay(true, "resume recovered session");
    fsWriteSession();

    oledStatus("Recovery lanjut", recoveredName);
    delay(1000);
}

void handleRecoveredSessionCheck() {
    if (!recoveredSessionPending || !relayOn || !sessionActive) return;

    if (deviceConnected) {
        recoveredSessionPending = false;
        recoveredNoDeviceCount = 0;
        prevDevConn = true;
        Serial.println("[Recovery] Device masih terhubung, sesi dilanjutkan");
        return;
    }

    recoveredNoDeviceCount++;
    Serial.printf("[Recovery] Menunggu device setelah boot %d/%d\n",
                  recoveredNoDeviceCount, DISCONNECT_THRESHOLD);

    if (recoveredNoDeviceCount < DISCONNECT_THRESHOLD) return;

    unsigned long nowTs = ntpSynced ? (unsigned long)time(nullptr) : millis() / 1000;
    Serial.println("[Recovery] Device tidak ada, arsipkan checkpoint terakhir");
    archiveCurrentSession(nowTs, lastP, true, false);
    if (wifiConnected) {
        sendToFirebase(0, 0, 0, 0, 0, sessionKwh, sessionCost, false, false, false, nowTs);
    }

    setRelay(false, "recovered device missing");
    sessionEnergyWh = 0; sessionKwh = 0; sessionCost = 0;
    hadDataOnce = false; prevDevConn = false;
    sessionDeviceName[0] = '\0';
    currentSessionId[0] = '\0';
    saveSessionId();
}

// ================================================================
// HANDLE DEVICE DISCONNECT
// ================================================================
void handleDeviceDisconnect() {
    if (!relayOn || !sessionActive) return;
    if (!deviceConnected && prevDevConn) {
        disconnectCount++;
        Serial.printf("[Disconnect] %d/%d\n", disconnectCount, DISCONNECT_THRESHOLD);
        if (disconnectCount >= DISCONNECT_THRESHOLD) {
            Serial.println("[Disconnect] ✓ Device dicabut — simpan sesi, relay OFF");
            unsigned long nowTs = ntpSynced ? (unsigned long)time(nullptr) : millis() / 1000;
            archiveCurrentSession(nowTs, lastP, false, false);
            if (wifiConnected) {
                sendToFirebase(0, 0, 0, 0, 0, sessionKwh, sessionCost, false, false, false, nowTs);
            }

            setRelay(false, "device dicabut");
            sessionEnergyWh = 0; sessionKwh = 0; sessionCost = 0;
            hadDataOnce = false; sessionDeviceName[0] = '\0';
            currentSessionId[0] = '\0';
            saveSessionId();
        }
    } else if (deviceConnected) {
        disconnectCount = 0;
    }
}

// ================================================================
// HANDLE OVERLOAD
// ================================================================
void handleOverload(float power) {
    bool newOvl = deviceConnected && (power >= overloadThreshold);
    if (newOvl && !isOverload) {
        isOverload = true;
        Serial.printf("[Overload] ⚠ %.1fW >= %.0fW — relay OFF\n", power, overloadThreshold);

        unsigned long nowTs = ntpSynced ? (unsigned long)time(nullptr) : millis() / 1000;
        archiveCurrentSession(nowTs, power, false, true);

        setRelay(false, "overload");
        overloadAlertLinger = true;
        overloadLingerStart = millis();
        sessionEnergyWh = 0; sessionKwh = 0; sessionCost = 0;
        sessionDeviceName[0] = '\0';
        currentSessionId[0] = '\0';
        saveSessionId();

    } else if (!newOvl && isOverload) {
        isOverload = false;
        Serial.println("[Overload] ✓ Teratasi");

        // Tetap idle setelah overload; user bisa mulai sesi baru manual.
    }
}

// ================================================================
// CHECK RESET BUTTON (GPIO 0 / BOOT)
//   >= 1s (< 5s) : mulai sesi offline baru (jika kondisi memungkinkan)
//   >= 5s        : reset WiFi credentials + restart
// ================================================================
void checkResetButton() {
    if (digitalRead(PIN_RESET_WIFI) != LOW) return;

    unsigned long pressStart = millis();
    oledStatus("Tahan utk aksi", "1s=Baru 5s=Reset");

    // Perlu akses WebServer & DNS — include forward declarations di sini
    // via extern. Deklarasi dilakukan di network.h.
    extern void networkHandleClients();

    while (digitalRead(PIN_RESET_WIFI) == LOW) {
        unsigned long held = millis() - pressStart;
        if (held >= BTN_RESET_WIFI_HOLD) {
            oledStatus("Reset WiFi...", "Lepaskan utk batal");
        } else if (held >= BTN_NEW_SESSION_HOLD) {
            if (modeOffline && !relayOn && !sessionActive) {
                oledStatus("Lepas=Sesi Baru", "Tahan=Reset WiFi");
            } else {
                oledStatus("Lepas utk batal", "Tahan=Reset WiFi");
            }
        }
        networkHandleClients();
        delay(50);
    }

    unsigned long held = millis() - pressStart;
    Serial.printf("[Button] Released after %lu ms\n", held);

    if (held >= BTN_RESET_WIFI_HOLD) {
        Serial.println("[Button] ★ Reset WiFi triggered");
        oledStatus("Reset WiFi...", "Restarting...");
        if (sessionActive && hadDataOnce) fsWriteSession();
        digitalWrite(PIN_RELAY, RELAY_OFF);
        relayOn = false;
        WiFi.disconnect(true, true);
        delay(500);
        ESP.restart();

    } else if (held >= BTN_NEW_SESSION_HOLD) {
        if (modeOffline && !relayOn && !sessionActive) {
            Serial.println("[Button] ★ New offline session triggered");
            startOfflineSession("button press");
        } else {
            Serial.printf("[Button] 1s hold ignored — modeOffline=%d relayOn=%d sessionActive=%d\n",
                          modeOffline, relayOn, sessionActive);
            if (!modeOffline) {
                oledStatus("Mode Online", "Gunakan web app");
            } else if (relayOn) {
                oledStatus("Sesi aktif", sessionDeviceName);
            }
            delay(1500);
        }
    } else {
        Serial.println("[Button] Short press — no action");
    }
}
