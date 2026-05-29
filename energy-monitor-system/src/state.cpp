// ================================================================
// state.cpp — Smart Energy Monitor v3.1
// Definisi semua global variable yang di-extern di state.h
// ================================================================

#include "state.h"
#include "config.h"

AppConfig appConfig = {
    THRESHOLD_DEFAULT,
    TARIF_DEFAULT
};

SessionData sessionData = {
    0.0f,
    0.0f,
    0.0f,
    0.0f,
    0.0f,
    0,
    false,
    "",
    SESSION_MODE_ONLINE,
    false,
    SESSION_END_NONE
};

SystemMode systemMode = SystemMode::ONLINE;
SessionState sessionState = SessionState::IDLE;

// ── Mode ─────────────────────────────────────────────────────────
bool wifiConnected = false;
bool ntpSynced     = false;
bool modeOffline   = false;
unsigned long offlineStartMs       = 0;
unsigned long lastModeTransitionMs = 0;

// ── Sensor & Relay ───────────────────────────────────────────────
bool  relayOn         = false;
bool  deviceConnected = false;
bool  prevDevConn     = false;
bool  isOverload      = false;
bool  overloadWarning = false;
int   disconnectCount = 0;
bool  overloadAlertLinger = false;
unsigned long overloadLingerStart = 0;

// ── Session ──────────────────────────────────────────────────────
float sessionEnergyWh    = 0.0f;
float sessionKwh         = 0.0f;
float sessionCost        = 0.0f;
bool  sessionActive      = false;
char  sessionDeviceName[32] = "";
unsigned long sessionStartTs = 0;
int   offlineDeviceCounter   = 0;
bool  recoveredSessionPending = false;
int   recoveredNoDeviceCount  = 0;
bool  loadCheckPending        = false;
unsigned long loadCheckStartedMs = 0;
int   loadDetectStableCount   = 0;

// ── Last sensor readings ─────────────────────────────────────────
float lastV = 0, lastI = 0, lastP = 0, lastPF = 0, lastHz = 0;
bool  hadDataOnce = false;

// ── Persisted identifiers ────────────────────────────────────────
char currentUid[64]       = "";
char currentSessionId[48] = "";
char lastCommandId[48]    = "";

// ── Timing ───────────────────────────────────────────────────────
unsigned long lastLoopMs              = 0;
unsigned long lastReconnectMs         = 0;
unsigned long lastThresholdSyncMs     = 0;
unsigned long lastCommandPollMs       = 0;
unsigned long lastCheckpointMs        = 0;
unsigned long lastOfflineSyncRetryMs  = 0;

const char* systemModeToString(SystemMode mode) {
    switch (mode) {
        case SystemMode::ONLINE:     return "ONLINE";
        case SystemMode::OFFLINE:    return "OFFLINE";
        case SystemMode::TRANSITION: return "TRANSITION";
    }
    return "ONLINE";
}

const char* sessionStateToString(SessionState state) {
    switch (state) {
        case SessionState::IDLE:         return "IDLE";
        case SessionState::WAITING_LOAD: return "WAITING_LOAD";
        case SessionState::MONITORING:   return "MONITORING";
        case SessionState::OVERLOAD:     return "OVERLOAD";
        case SessionState::FINISHED:     return "FINISHED";
    }
    return "IDLE";
}

void setSystemMode(SystemMode next, const char* reason) {
    if (systemMode == next) return;
    Serial.printf("[State] SystemMode %s -> %s (%s)\n",
                  systemModeToString(systemMode),
                  systemModeToString(next),
                  reason ? reason : "");
    systemMode = next;
}

void setSessionState(SessionState next, const char* reason) {
    if (sessionState == next) return;
    Serial.printf("[State] SessionState %s -> %s (%s)\n",
                  sessionStateToString(sessionState),
                  sessionStateToString(next),
                  reason ? reason : "");
    sessionState = next;
}

void syncStateMachineFromLegacy() {
    systemMode = modeOffline ? SystemMode::OFFLINE : SystemMode::ONLINE;

    if (isOverload || overloadAlertLinger) {
        sessionState = SessionState::OVERLOAD;
    } else if (relayOn && sessionActive && deviceConnected) {
        sessionState = SessionState::MONITORING;
    } else if (relayOn && (loadCheckPending || !deviceConnected)) {
        sessionState = SessionState::WAITING_LOAD;
    } else if (sessionState != SessionState::FINISHED) {
        sessionState = SessionState::IDLE;
    }
}

void syncSessionDataFromLegacy(unsigned long nowTs) {
    syncStateMachineFromLegacy();
    sessionData.voltage = lastV;
    sessionData.current = lastI;
    sessionData.power = lastP;
    sessionData.energy = sessionKwh;
    sessionData.cost = sessionCost;
    sessionData.duration = (sessionActive && sessionStartTs > 0 && nowTs > sessionStartTs)
        ? nowTs - sessionStartTs
        : 0;
    sessionData.sessionActive = sessionActive;
    strlcpy(sessionData.deviceName, sessionDeviceName, sizeof(sessionData.deviceName));
    sessionData.mode = modeOffline ? SESSION_MODE_OFFLINE : SESSION_MODE_ONLINE;
    sessionData.overload = isOverload;
}
