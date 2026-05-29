#pragma once

// ================================================================
// session.h — Smart Energy Monitor v3.1
// Logika bisnis: relay, overload, disconnect, mode transition,
// session recovery, dan tombol reset
// ================================================================

#include <Arduino.h>

// ── Relay (single entry point) ───────────────────────────────────
void setRelay(bool on, const char* reason = "");

// ── Mode Transitions ─────────────────────────────────────────────
void transitionToOnlineMode();
void transitionToOfflineMode(const char* reason = "");
void enterManualOfflineMonitoringMode(const char* reason = "");
void handleReconnectResync();

// ── Offline Session ──────────────────────────────────────────────
void startOfflineSession(const char* reason);
void generateOfflineDeviceName();

// ── Session Recovery (dipanggil saat boot) ──────────────────────
void doSessionRecovery(int resetReason);
void handleRecoveredSessionCheck();

// ── Device Disconnect & Overload ─────────────────────────────────
void beginLoadCheck(const char* reason = "");
void handleLoadCheck(float current, float power);
void handleLoadRemovedDuringMonitoring(float current, float power);
void handleDeviceDisconnect();
void handleOverload(float power);

// ── Button (GPIO 0 / BOOT) ───────────────────────────────────────
void checkResetButton();

// ── Utility Helpers ──────────────────────────────────────────────
String        buildDuration(unsigned long startTs, unsigned long endTs);
unsigned long getSessionElapsedSec(unsigned long nowTs);
String        jsonEscape(const char* s);
