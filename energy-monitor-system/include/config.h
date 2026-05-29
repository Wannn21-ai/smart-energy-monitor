#pragma once

// ================================================================
// config.h — Smart Energy Monitor v3.1
// Semua konstanta, pin definition, dan path LittleFS
// ================================================================

// ── Pin Definitions ─────────────────────────────────────────────
#define PIN_LED_BLUE    2
#define PIN_LED_GREEN   25
#define PIN_LED_RED     26
#define PIN_BUZZER      5
#define PIN_RELAY       27
#define PIN_RESET_WIFI  0

#define RELAY_ON   LOW
#define RELAY_OFF  HIGH

// ── AP & Firebase ────────────────────────────────────────────────
#define AP_SSID  "SEM-Setup"
#define AP_PASS  "12345678"

#define FIREBASE_HOST "https://smart-energy-monitor-v2-de79d-default-rtdb.asia-southeast1.firebasedatabase.app"
#define FIREBASE_PATH "/live.json"

// ── LittleFS Paths ───────────────────────────────────────────────
#define FS_SESSION_PATH  "/session_active.json"
#define FS_HISTORY_PATH  "/history_offline.json"

// ── Default Values ───────────────────────────────────────────────
#define TARIF_DEFAULT     1444.70f
#define THRESHOLD_DEFAULT 2000.0f

// ── Timing Constants (ms) ────────────────────────────────────────
#define LOOP_INTERVAL              5000UL
#define RECONNECT_INTERVAL         60000UL
#define WIFI_OFFLINE_GRACE_MS      300000UL
#define THRESHOLD_SYNC_INTERVAL    30000UL
#define COMMAND_POLL_INTERVAL      2000UL
#define WIFI_LED_BLINK_MS          500UL
#define OVERLOAD_BLINK_MS          200UL
#define OVERLOAD_WARNING_BLINK_MS  500UL
#define OVERLOAD_WARNING_MARGIN_W  1.0f
#define OVERLOAD_ALERT_LINGER      10000UL
#define CHECKPOINT_INTERVAL        30000UL
#define OFFLINE_SYNC_RETRY_INTERVAL 15000UL
#define MODE_TRANSITION_DEBOUNCE   2000UL

// Load validation after relay ON
#define LOAD_SETTLE_MS             2000UL
#define LOAD_CHECK_INTERVAL        1000UL
#define LOAD_DETECT_TIMEOUT_MS     12000UL
#define LOAD_DETECT_STABLE_SAMPLES 2
#define LOAD_MIN_CURRENT           0.02f
#define LOAD_MIN_POWER             1.0f
#define LOAD_REMOVED_DEBOUNCE_MS   2500UL

// ── Button Hold Thresholds (ms) ──────────────────────────────────
#define BTN_NEW_SESSION_HOLD 1000UL
#define BTN_RESET_WIFI_HOLD  5000UL
#define BTN_OFFLINE_MODE_HOLD 10000UL

// ── Misc ─────────────────────────────────────────────────────────
#define DISCONNECT_THRESHOLD 2

// ── DNS ─────────────────────────────────────────────────────────
#define DNS_PORT 53
