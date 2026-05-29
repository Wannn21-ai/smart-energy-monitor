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
#include <esp_system.h>

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

static const char* resetReasonToString(int reason) {
    switch ((esp_reset_reason_t)reason) {
        case ESP_RST_POWERON:   return "POWERON";
        case ESP_RST_EXT:       return "EXT";
        case ESP_RST_SW:        return "SW";
        case ESP_RST_PANIC:     return "PANIC";
        case ESP_RST_INT_WDT:   return "INT_WDT";
        case ESP_RST_TASK_WDT:  return "TASK_WDT";
        case ESP_RST_WDT:       return "WDT";
        case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
        case ESP_RST_BROWNOUT:  return "BROWNOUT";
        case ESP_RST_SDIO:      return "SDIO";
        default:                return "UNKNOWN";
    }
}

static bool isBlackoutReset(int reason) {
    esp_reset_reason_t reset = (esp_reset_reason_t)reason;
    return reset == ESP_RST_POWERON || reset == ESP_RST_BROWNOUT;
}

// ================================================================
// RELAY — single entry point, atomic state update
// ================================================================
void setRelay(bool on, const char* reason) {
    bool wasActive = sessionActive;
    relayOn = on;
    digitalWrite(PIN_RELAY, on ? RELAY_ON : RELAY_OFF);

    // State migration point: relay ON means waiting for measurable load,
    // then the sensor loop promotes WAITING_LOAD to MONITORING.
    if (on) {
        setSessionState(SessionState::WAITING_LOAD, reason);
    } else if (isOverload || overloadAlertLinger) {
        setSessionState(SessionState::OVERLOAD, reason);
    } else {
        setSessionState(wasActive ? SessionState::FINISHED : SessionState::IDLE, reason);
    }

    if (!on) {
        overloadWarning = false;
        sessionActive = false;
        loadCheckPending = false;
        loadCheckStartedMs = 0;
        loadDetectStableCount = 0;
        disconnectCount = 0;
        recoveredSessionPending = false;
        recoveredNoDeviceCount = 0;
        fsClearSession();
    }
    Serial.printf("[Relay] %s — %s\n", on ? "ON" : "OFF", reason);
}

// ================================================================
// LOAD CHECK
// ================================================================
void beginLoadCheck(const char* reason) {
    sessionActive = false;
    loadCheckPending = true;
    loadCheckStartedMs = millis();
    loadDetectStableCount = 0;
    deviceConnected = false;
    prevDevConn = false;
    setRelay(true, reason);
    lastLoopMs = millis();
    Serial.printf("[LoadCheck] Started, settle=%lums timeout=%lums\n",
                  LOAD_SETTLE_MS, LOAD_DETECT_TIMEOUT_MS);
}

static void cancelLoadCheck(const char* reason) {
    unsigned long nowTs = ntpSynced ? (unsigned long)time(nullptr) : millis() / 1000;
    Serial.printf("[LoadCheck] Cancelled: %s\n", reason ? reason : "no load");
    setRelay(false, reason ? reason : "load check failed");
    sessionEnergyWh = 0; sessionKwh = 0; sessionCost = 0;
    hadDataOnce = false; prevDevConn = false; deviceConnected = false;
    sessionDeviceName[0] = '\0';
    currentSessionId[0] = '\0';
    saveSessionId();
    if (wifiConnected) {
        sendToFirebase(0, 0, 0, 0, 0, 0, 0, false, false, false, nowTs);
    }
}

void handleLoadCheck(float current, float power) {
    if (!loadCheckPending) return;
    if (!relayOn) {
        loadCheckPending = false;
        loadDetectStableCount = 0;
        return;
    }

    unsigned long elapsed = millis() - loadCheckStartedMs;
    if (elapsed < LOAD_SETTLE_MS) {
        setSessionState(SessionState::WAITING_LOAD, "relay settling");
        return;
    }

    bool loadDetected = current >= LOAD_MIN_CURRENT && power >= LOAD_MIN_POWER;
    if (loadDetected) {
        loadDetectStableCount++;
    } else {
        loadDetectStableCount = 0;
    }

    if (loadDetectStableCount >= LOAD_DETECT_STABLE_SAMPLES) {
        unsigned long nowTs = ntpSynced ? (unsigned long)time(nullptr) : millis() / 1000;
        loadCheckPending = false;
        loadDetectStableCount = 0;
        sessionActive = true;
        sessionStartTs = nowTs;
        hadDataOnce = true;
        prevDevConn = true;
        setSessionState(SessionState::MONITORING, "load verified");
        fsWriteSession();
        Serial.printf("[LoadCheck] Load verified I=%.2fA P=%.1fW\n", current, power);
        return;
    }

    Serial.printf("[LoadCheck] Waiting load I=%.2fA P=%.1fW stable=%d/%d elapsed=%lums\n",
                  current, power, loadDetectStableCount,
                  LOAD_DETECT_STABLE_SAMPLES, elapsed);

    if (elapsed >= LOAD_DETECT_TIMEOUT_MS) {
        cancelLoadCheck("no load detected");
    }
}

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

static void clearFinishedSession(const char* reason) {
    setRelay(false, reason);
    sessionEnergyWh = 0; sessionKwh = 0; sessionCost = 0;
    hadDataOnce = false; prevDevConn = false; disconnectCount = 0;
    sessionDeviceName[0] = '\0';
    currentSessionId[0] = '\0';
    saveSessionId();
}

static bool finalizeMissingLoadOnReconnect(unsigned long nowTs) {
    if (!relayOn || !sessionActive || !hadDataOnce || deviceConnected) return false;
    if (loadCheckPending || recoveredSessionPending) return false;
    if (!prevDevConn && disconnectCount == 0) return false;

    Serial.println("[Reconnect] Load missing, finalizing active session");
    archiveCurrentSession(nowTs, lastP, false, false);
    clearFinishedSession("load missing after reconnect");
    syncSessionDataFromLegacy(nowTs);
    sendToFirebase(0, 0, 0, 0, 0, 0, 0, false, false, false, nowTs);
    return true;
}

void handleReconnectResync() {
    if (!wifiConnected || WiFi.status() != WL_CONNECTED) return;

    unsigned long nowTs = ntpSynced ? (unsigned long)time(nullptr) : millis() / 1000;

    if (finalizeMissingLoadOnReconnect(nowTs)) {
        return;
    }

    if (sessionActive && hadDataOnce) {
        fsWriteSession();
    }

    syncSessionDataFromLegacy(nowTs);
    bool ok = sendToFirebase(lastV, lastI, lastP, lastPF, lastHz,
                             sessionKwh, sessionCost,
                             deviceConnected, isOverload, relayOn, nowTs);
    Serial.printf("[Reconnect] Live mirror resync: %s\n", ok ? "OK" : "FAIL");
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
    setSystemMode(SystemMode::TRANSITION, "offline to online");

    if (sessionActive && hadDataOnce) {
        if (fsWriteSession()) {
            Serial.println("[Mode→Online] Checkpoint disimpan");
        }
    }

    modeOffline   = false;
    wifiConnected = true;
    offlineStartMs = 0;
    setSystemMode(SystemMode::ONLINE, "wifi restored");

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
    setSystemMode(SystemMode::TRANSITION, reason);

    modeOffline    = true;
    wifiConnected  = false;
    offlineStartMs = now;
    setSystemMode(SystemMode::OFFLINE, reason);

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
    overloadWarning     = false;
    overloadAlertLinger = false;
    currentSessionId[0] = '\0';

    sessionStartTs = 0;

    beginLoadCheck(reason);
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
void doSessionRecovery(int resetReason) {
    PersistedSession recovered;

    if (!fsReadSession(recovered)) return;

    Serial.printf("[Recovery] Reset=%s blackout=%s checkpoint='%s' %.4f kWh state=%s mode=%s\n",
                  resetReasonToString(resetReason),
                  isBlackoutReset(resetReason) ? "yes" : "no",
                  recovered.name,
                  recovered.kwh,
                  sessionStateToString(recovered.sessionState),
                  recovered.offlineMode ? "OFFLINE" : "ONLINE");
    oledStatus("Recovery sesi", recovered.name);
    delay(1500);

    unsigned long nowTs = ntpSynced ? (unsigned long)time(nullptr) : millis() / 1000;
    if ((recovered.startTs == 0 || recovered.startTs > nowTs) && recovered.elapsedSec > 0) {
        recovered.startTs = nowTs > recovered.elapsedSec ? nowTs - recovered.elapsedSec : nowTs;
    }

    strlcpy(sessionDeviceName, recovered.name, sizeof(sessionDeviceName));
    sessionStartTs      = recovered.startTs;
    sessionEnergyWh     = recovered.energyWh;
    sessionKwh          = recovered.kwh;
    sessionCost         = recovered.cost;
    hadDataOnce         = true;
    disconnectCount     = 0;
    isOverload          = recovered.overload;
    overloadWarning     = false;
    overloadAlertLinger = false;
    recoveredSessionPending = true;
    recoveredNoDeviceCount  = 0;
    prevDevConn         = false;

    if (strlen(recovered.uid) > 0) strlcpy(currentUid, recovered.uid, sizeof(currentUid));
    if (strlen(recovered.sessionId) > 0) strlcpy(currentSessionId, recovered.sessionId, sizeof(currentSessionId));
    sessionData.endReason = SESSION_END_NONE;
    setSystemMode(modeOffline ? SystemMode::OFFLINE : SystemMode::ONLINE, "boot recovery mode");
    setSessionState(recovered.sessionState, "checkpoint restored");

    setRelay(true, "resume recovered session");
    sessionActive = true;
    loadCheckPending = false;
    fsWriteSession();

    oledStatus("Recovery lanjut", recovered.name);
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
    bool canEvaluate = relayOn && sessionActive && deviceConnected && !overloadAlertLinger;
    float warningAt = overloadThreshold > OVERLOAD_WARNING_MARGIN_W
        ? overloadThreshold - OVERLOAD_WARNING_MARGIN_W
        : overloadThreshold;

    bool warning = canEvaluate && power >= warningAt && power < overloadThreshold;
    if (warning != overloadWarning) {
        overloadWarning = warning;
        warningBlinkState = false;
        digitalWrite(PIN_LED_RED, LOW);
        digitalWrite(PIN_BUZZER, LOW);
        Serial.printf("[Overload] Warning %s P=%.1fW threshold=%.0fW\n",
                      overloadWarning ? "ON" : "OFF", power, overloadThreshold);
    }

    bool newOvl = canEvaluate && (power >= overloadThreshold);
    if (!newOvl) {
        if (!canEvaluate) overloadWarning = false;
        return;
    }

    isOverload = true;
    overloadWarning = false;
    sessionData.endReason = SESSION_END_OVERLOAD;
    setSessionState(SessionState::OVERLOAD, "power threshold exceeded");
    Serial.printf("[Overload] %.1fW >= %.0fW - relay OFF immediately\n", power, overloadThreshold);

    unsigned long nowTs = ntpSynced ? (unsigned long)time(nullptr) : millis() / 1000;
    fsWriteSession();

    relayOn = false;
    digitalWrite(PIN_RELAY, RELAY_OFF);
    loadCheckPending = false;
    loadCheckStartedMs = 0;
    loadDetectStableCount = 0;
    disconnectCount = 0;
    recoveredSessionPending = false;
    recoveredNoDeviceCount = 0;

    overloadAlertLinger = true;
    overloadLingerStart = millis();
    overloadBlinkState = false;
    lastOverloadBlinkMs = 0;

    archiveCurrentSession(nowTs, power, false, true);
    sessionActive = false;
    if (wifiConnected) {
        sendToFirebase(0, 0, 0, 0, 0, sessionKwh, sessionCost, false, true, false, nowTs);
    }

    isOverload = false;
    sessionEnergyWh = 0; sessionKwh = 0; sessionCost = 0;
    hadDataOnce = false; prevDevConn = false;
    sessionDeviceName[0] = '\0';
    currentSessionId[0] = '\0';
    saveSessionId();
    fsClearSession();
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
