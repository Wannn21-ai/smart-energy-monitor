// ================================================================
// state.cpp — Smart Energy Monitor v3.1
// Definisi semua global variable yang di-extern di state.h
// ================================================================

#include "state.h"
#include "config.h"

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
int   disconnectCount = 0;
bool  overloadAlertLinger = false;
unsigned long overloadLingerStart = 0;

// ── Session ──────────────────────────────────────────────────────
float overloadThreshold  = THRESHOLD_DEFAULT;
float tarif              = TARIF_DEFAULT;
float sessionEnergyWh    = 0.0f;
float sessionKwh         = 0.0f;
float sessionCost        = 0.0f;
bool  sessionActive      = false;
char  sessionDeviceName[32] = "";
unsigned long sessionStartTs = 0;
int   offlineDeviceCounter   = 0;
bool  recoveredSessionPending = false;
int   recoveredNoDeviceCount  = 0;

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
unsigned long lastBlinkMs             = 0;
bool          blinkState              = false;
unsigned long lastOverloadBlinkMs     = 0;
bool          overloadBlinkState      = false;

void syncSessionDataFromLegacy(unsigned long nowTs) {
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
