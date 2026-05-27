#pragma once

// ================================================================
// network.h — Smart Energy Monitor v3.1
// WiFi, NTP, Local AP, WebServer (captive portal)
// ================================================================

#include <Arduino.h>
#include <WebServer.h>
#include <DNSServer.h>

// Objek di-expose agar session.cpp bisa memanggil handleClient
extern WebServer localServer;
extern DNSServer dnsServer;

// ── WiFi & NTP ───────────────────────────────────────────────────
bool tryConnectWiFi(int sec = 20);
bool tryNTPSync();

// ── Local AP + WebServer ─────────────────────────────────────────
void startLocalAP();
void setupWebServer();

// ── Dipanggil tiap loop dari session.cpp (checkResetButton) ─────
void networkHandleClients();
