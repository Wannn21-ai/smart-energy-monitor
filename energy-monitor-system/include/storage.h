#pragma once

// ================================================================
// storage.h — Smart Energy Monitor v3.1
// LittleFS session/history dan Preferences (tarif, threshold, uid)
// ================================================================

#include <Arduino.h>

// ── Preferences ──────────────────────────────────────────────────
void loadPrefs();
void savePrefs();
void saveSessionId();

// ── LittleFS Init ────────────────────────────────────────────────
bool fsInit();

// ── Session File ─────────────────────────────────────────────────
bool fsWriteSession();
void fsClearSession();
bool fsReadSession(float &outEnergyWh, float &outKwh, float &outCost,
                   char *outName, unsigned long &outStartTs,
                   unsigned long &outElapsedSec);

// ── Offline History Queue ────────────────────────────────────────
void fsAppendOfflineHistory(const char* name, unsigned long startTs,
                            unsigned long endTs, float energyKwh,
                            float cost, float avgPower, bool wasOverload);
bool fsSyncOfflineHistoryToFirebase();
int  fsCountOfflineHistory();
