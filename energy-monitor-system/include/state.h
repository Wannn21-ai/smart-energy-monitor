#pragma once

// ================================================================
// state.h — Smart Energy Monitor v3.1
// Deklarasi extern semua global variable yang dipakai lintas modul.
// Definisi ada di state.cpp.
// ================================================================

#include <Arduino.h>

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
