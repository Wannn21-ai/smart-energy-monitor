#pragma once

// ================================================================
// firebase.h — Smart Energy Monitor v3.1
// Semua komunikasi ke Firebase Realtime Database
// ================================================================

#include <Arduino.h>

// ── Live Data ────────────────────────────────────────────────────
bool sendToFirebase(float v, float i, float p, float pf, float freq,
                    float kwh, float cost, bool dev, bool ovl,
                    bool relay, unsigned long ts);

// ── History Push ─────────────────────────────────────────────────
bool pushHistoryToFirebase(const char* name, const char* duration,
                           float avgPower, float energyKwh, float cost,
                           unsigned long ts, bool recovered, bool wasOverload);

// ── Config Sync ──────────────────────────────────────────────────
void syncThresholdFromFirebase();

// ── Relay Command Poll ───────────────────────────────────────────
void pollCommandFromFirebase();
void clearFirebaseCommand();
