#pragma once

// ================================================================
// state.h — Smart Energy Monitor v3.1
// Deklarasi extern semua global variable yang dipakai lintas modul.
// Definisi ada di state.cpp.
// ================================================================

#include <Arduino.h>

enum SessionMode {
    SESSION_MODE_ONLINE,
    SESSION_MODE_OFFLINE
};

enum SessionEndReason {
    SESSION_END_NONE,
    SESSION_END_USER_STOP,
    SESSION_END_DEVICE_DISCONNECT,
    SESSION_END_OVERLOAD,
    SESSION_END_RECOVERY_MISSING_DEVICE
};

struct SessionData {
    float voltage;
    float current;
    float power;
    float energy;
    float cost;
    unsigned long duration;
    bool sessionActive;
    char deviceName[32];
    SessionMode mode;
    bool overload;
    SessionEndReason endReason;
};

extern SessionData sessionData;
void syncSessionDataFromLegacy(unsigned long nowTs);

// ── Mode ─────────────────────────────────────────────────────────
extern bool wifiConnected;
extern bool ntpSynced;
extern bool modeOffline;
extern unsigned long offlineStartMs;
extern unsigned long lastModeTransitionMs;

// ── Sensor & Relay ───────────────────────────────────────────────
extern bool  relayOn;
extern bool  deviceConnected;
extern bool  prevDevConn;
extern bool  isOverload;
extern int   disconnectCount;
extern bool  overloadAlertLinger;
extern unsigned long overloadLingerStart;

// ── Session ──────────────────────────────────────────────────────
extern float overloadThreshold;
extern float tarif;
extern float sessionEnergyWh;
extern float sessionKwh;
extern float sessionCost;
extern bool  sessionActive;
extern char  sessionDeviceName[32];
extern unsigned long sessionStartTs;
extern int   offlineDeviceCounter;
extern bool  recoveredSessionPending;
extern int   recoveredNoDeviceCount;

// ── Last sensor readings ─────────────────────────────────────────
extern float lastV;
extern float lastI;
extern float lastP;
extern float lastPF;
extern float lastHz;
extern bool  hadDataOnce;

// ── Persisted identifiers ────────────────────────────────────────
extern char currentUid[64];
extern char currentSessionId[48];
extern char lastCommandId[48];

// ── Timing ───────────────────────────────────────────────────────
extern unsigned long lastLoopMs;
extern unsigned long lastReconnectMs;
extern unsigned long lastThresholdSyncMs;
extern unsigned long lastCommandPollMs;
extern unsigned long lastCheckpointMs;
extern unsigned long lastOfflineSyncRetryMs;
extern unsigned long lastBlinkMs;
extern bool          blinkState;
extern unsigned long lastOverloadBlinkMs;
extern bool          overloadBlinkState;
