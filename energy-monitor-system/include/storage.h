#pragma once

// ================================================================
// storage.h — Smart Energy Monitor v3.1
// LittleFS session/history dan Preferences (config, uid)
// ================================================================

#include <Arduino.h>
#include "state.h"

struct PersistedSession {
    float energyWh;
    float kwh;
    float cost;
    unsigned long startTs;
    unsigned long elapsedSec;
    bool sessionActive;
    bool relay;
    bool overload;
    bool offlineMode;
    SessionState sessionState;
    SystemMode systemMode;
    char name[32];
    char uid[64];
    char sessionId[48];
};

// ── Preferences ──────────────────────────────────────────────────
void loadPrefs();
void savePrefs();
void saveSessionId();
bool setAppConfig(const AppConfig& next, const char* source = "");
bool setOverloadThreshold(float threshold, const char* source = "");
bool setElectricityCostPerKwh(float costPerKwh, const char* source = "");

// ── LittleFS Init ────────────────────────────────────────────────
bool fsInit();

// ── Session File ─────────────────────────────────────────────────
bool fsWriteSession();
void fsClearSession();
bool fsReadSession(PersistedSession &out);

// ── Offline History Queue ────────────────────────────────────────
bool fsAppendOfflineHistory(const char* name, unsigned long startTs,
                            unsigned long endTs, float energyKwh,
                            float cost, float avgPower, bool wasOverload,
                            const char* endReason = "NORMAL_STOP",
                            bool recovered = false);
bool fsSyncOfflineHistoryToFirebase();
int  fsCountOfflineHistory();
