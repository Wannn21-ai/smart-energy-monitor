// ================================================================
// network.cpp - Smart Energy Monitor v3.1
// WiFi, NTP, Local AP, WebServer / Captive Portal
// ================================================================

#include "network.h"
#include "config.h"
#include "state.h"
#include "storage.h"
#include "firebase.h"
#include "session.h"
#include "indicators.h"

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>

WebServer localServer(80);
DNSServer dnsServer;

// ================================================================
// HELPERS
// ================================================================
static String htmlEscape(const String& value) {
    String out;
    out.reserve(value.length());
    for (size_t i = 0; i < value.length(); i++) {
        char c = value[i];
        if (c == '&') out += F("&amp;");
        else if (c == '<') out += F("&lt;");
        else if (c == '>') out += F("&gt;");
        else if (c == '"') out += F("&quot;");
        else if (c == '\'') out += F("&#39;");
        else out += c;
    }
    return out;
}

static void beginWifiScan() {
    if (WiFi.scanComplete() == WIFI_SCAN_RUNNING) return;
    WiFi.scanDelete();
    WiFi.scanNetworks(true, true);
}

static void syncPortalSettingsToFirebase() {
    if (!wifiConnected || WiFi.status() != WL_CONNECTED) return;

    WiFiClientSecure c; c.setInsecure(); c.setTimeout(5000);
    HTTPClient h;

    if (h.begin(c, String(FIREBASE_HOST) + "/config/threshold.json")) {
        h.addHeader("Content-Type", "application/json");
        h.PUT(String(appConfig.overloadThreshold, 0));
        h.end();
    }

    WiFiClientSecure cCfg; cCfg.setInsecure(); cCfg.setTimeout(5000);
    HTTPClient hCfg;
    if (hCfg.begin(cCfg, String(FIREBASE_HOST) + "/config/app.json")) {
        hCfg.addHeader("Content-Type", "application/json");
        String body = "{\"overloadThreshold\":" + String(appConfig.overloadThreshold, 0) +
                      ",\"electricityCostPerKwh\":" + String(appConfig.electricityCostPerKwh, 2) +
                      ",\"tariff\":" + String(appConfig.electricityCostPerKwh, 2) + "}";
        hCfg.PUT(body);
        hCfg.end();
    }

    if (strlen(currentUid) == 0) return;

    WiFiClientSecure c2; c2.setInsecure(); c2.setTimeout(5000);
    HTTPClient h2;
    String path = String("/users/") + currentUid + "/settings.json";
    if (h2.begin(c2, String(FIREBASE_HOST) + path)) {
        h2.addHeader("Content-Type", "application/json");
        String body = "{\"overloadThreshold\":" + String(appConfig.overloadThreshold, 0) +
                      ",\"tariff\":" + String(appConfig.electricityCostPerKwh, 2) + "}";
        h2.PATCH(body);
        h2.end();
    }
}

// ================================================================
// WiFi CONNECT
// ================================================================
bool tryConnectWiFi(int sec) {
    WiFi.begin();
    Serial.print("[WiFi] Connecting");
    indicatorsSetWifiSearching(true);
    unsigned long start = millis();
    unsigned long timeout = (unsigned long)sec * 1000UL;

    while (millis() - start < timeout && WiFi.status() != WL_CONNECTED) {
        dnsServer.processNextRequest();
        localServer.handleClient();
        if (manualOfflineRequested) break;
        indicatorsUpdate();

        delay(100);
    }
    indicatorsSetWifiSearching(false);
    indicatorsUpdate();

    if (manualOfflineRequested) {
        manualOfflineRequested = false;
        Serial.println("\n[WiFi] Manual offline requested");
        return false;
    }

    if (WiFi.status() == WL_CONNECTED) {
        Serial.println("\n[WiFi] OK: " + WiFi.localIP().toString());
        return true;
    }
    Serial.println("\n[WiFi] Gagal");
    return false;
}

// ================================================================
// NTP SYNC
// ================================================================
bool tryNTPSync() {
    configTime(7 * 3600, 0, "pool.ntp.org", "time.nist.gov");
    struct tm ti;
    int r = 0;
    while (!getLocalTime(&ti) && r < 20) { delay(500); r++; }
    return r < 20;
}

// ================================================================
// LOCAL AP
// ================================================================
void startLocalAP() {
    IPAddress ip(192, 168, 4, 1), sub(255, 255, 255, 0);
    WiFi.softAPConfig(ip, ip, sub);
    WiFi.softAP(AP_SSID, AP_PASS);
    indicatorsSetCaptivePortalActive(true);
    dnsServer.stop();
    dnsServer.start(DNS_PORT, "*", ip);
    Serial.printf("[AP] '%s' aktif - 192.168.4.1\n", AP_SSID);
}

// ================================================================
// NETWORK HANDLE CLIENTS (dipanggil dari session.cpp)
// ================================================================
void networkHandleClients() {
    localServer.handleClient();
    dnsServer.processNextRequest();
}

// ================================================================
// WEBSERVER HANDLERS
// ================================================================
static void handleRoot() {
    int n = WiFi.scanComplete();
    String ssidOptions;
    String scanStatus;
    bool autoRefreshScan = false;

    if (n == WIFI_SCAN_RUNNING) {
        autoRefreshScan = true;
        ssidOptions = F("<option value='' selected>Scan WiFi sedang berjalan...</option>");
        scanStatus = F("<div class='notice warn'>Scan WiFi sedang berjalan. Daftar akan diperbarui otomatis.</div>");
    } else if (n < 0) {
        beginWifiScan();
        autoRefreshScan = true;
        ssidOptions = F("<option value='' selected>Memulai scan WiFi...</option>");
        scanStatus = F("<div class='notice warn'>Memulai scan WiFi. Tunggu beberapa detik.</div>");
    } else if (n == 0) {
        beginWifiScan();
        autoRefreshScan = true;
        ssidOptions = F("<option value='' selected>Tidak ada jaringan terdeteksi</option>");
        scanStatus = F("<div class='notice err'>Belum ada WiFi terdeteksi. Pastikan router dekat, lalu scan ulang.</div>");
    } else {
        for (int i = 0; i < n; i++) {
            String ssid = WiFi.SSID(i);
            if (ssid.length() == 0) continue;

            int rssi = WiFi.RSSI(i);
            bool locked = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN);
            int bars = rssi > -50 ? 4 : rssi > -65 ? 3 : rssi > -75 ? 2 : 1;
            String signal = bars == 4 ? "||||" : bars == 3 ? "|||." : bars == 2 ? "||.." : "|...";
            String safeSsid = htmlEscape(ssid);

            ssidOptions += F("<option value=\"");
            ssidOptions += safeSsid;
            ssidOptions += F("\">");
            ssidOptions += signal + " " + safeSsid;
            if (locked) ssidOptions += F(" [secured]");
            ssidOptions += F("</option>");
        }
        if (ssidOptions.length() == 0) {
            beginWifiScan();
            autoRefreshScan = true;
            ssidOptions = F("<option value='' selected>SSID tersembunyi - ketik manual</option>");
        }
        scanStatus = "<div class='notice ok'>" + String(n) + " jaringan ditemukan.</div>";
    }

    int pendingSync = fsCountOfflineHistory();
    String syncInfo = "";
    if (pendingSync > 0) {
        syncInfo = "<div class='notice warn'>" + String(pendingSync) +
                   " sesi offline menunggu sync ke Firebase.</div>";
    }

    String html = "<!DOCTYPE html><html lang='id'><head>"
        "<meta charset='UTF-8'/>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'/>"
        "<title>SEM Setup</title>"
        "<style>"
        "*{box-sizing:border-box}body{margin:0;background:#0a0a0a;color:#f0f0f0;font-family:Arial,sans-serif;"
        "display:flex;flex-direction:column;align-items:center;padding:24px 16px}"
        "h2{color:#00e5ff;font-size:18px;margin:0 0 20px}.card{background:#111;border:1px solid #242424;"
        "border-radius:8px;padding:20px;width:100%;max-width:430px;margin-bottom:14px}"
        ".title{color:#aaa;font-size:11px;font-weight:700;text-transform:uppercase;letter-spacing:.08em}"
        "label{display:block;font-size:11px;color:#888;text-transform:uppercase;letter-spacing:.06em;margin:14px 0 7px}"
        "select,input{width:100%;padding:12px 14px;background:#1a1a1a;border:1px solid #333;border-radius:8px;"
        "color:#f0f0f0;font-size:15px;outline:none}select:focus,input:focus{border-color:#00e5ff}"
        ".btn{display:block;width:100%;padding:14px;border:none;border-radius:8px;font-size:15px;font-weight:700;"
        "cursor:pointer;margin-top:16px}.btn-p{background:#00e5ff;color:#000}.btn-g{background:transparent;color:#bbb;"
        "border:1px solid #333;margin-top:8px}.btn-o{background:#ffab00;color:#000;margin-top:8px}"
        ".notice{border-radius:8px;padding:10px 12px;font-size:12px;margin:12px 0}"
        ".ok{background:rgba(0,230,118,.1);border:1px solid rgba(0,230,118,.25);color:#00e676}"
        ".warn{background:rgba(255,171,0,.1);border:1px solid rgba(255,171,0,.25);color:#ffab00}"
        ".err{background:rgba(255,23,68,.1);border:1px solid rgba(255,23,68,.25);color:#ff6b82}"
        "#toast{position:fixed;bottom:24px;left:50%;transform:translateX(-50%) translateY(80px);"
        "background:#1a1a1a;border:1px solid #333;color:#f0f0f0;padding:12px 18px;border-radius:8px;"
        "font-size:13px;transition:transform .3s;z-index:999;width:calc(100% - 32px);max-width:420px;text-align:center}"
        "#toast.show{transform:translateX(-50%) translateY(0)}#toast.ok{color:#00e676}"
        "#toast.err{color:#ff6b82}#toast.info{color:#00e5ff}"
        "</style></head><body>"
        "<h2>Smart Energy Monitor</h2>"
        "<div class='card'><div class='title'>Koneksi WiFi</div>"
        + syncInfo + scanStatus +
        "<label>Pilih Jaringan</label>"
        "<select id='ssid' onchange=\"document.getElementById('ssid_manual').value=this.value\">"
        + ssidOptions +
        "</select>"
        "<label>Atau ketik SSID manual</label>"
        "<input type='text' id='ssid_manual' placeholder='Ketik nama WiFi...'/>"
        "<label>Password</label>"
        "<input type='password' id='wpass' placeholder='Password WiFi'/>"
        "<button class='btn btn-p' onclick='connect()'>Hubungkan</button>"
        "<button class='btn btn-o' onclick='offlineMode()'>Lanjutkan ke Mode Offline</button>"
        "<button class='btn btn-g' onclick='rescan()'>Scan Ulang</button></div>"
        "<div class='card'><div class='title'>Pengaturan</div>"
        "<label>Tarif / kWh (IDR)</label>"
        "<input type='number' id='trf' value='" + String(appConfig.electricityCostPerKwh, 2) + "' step='0.01'/>"
        "<label>Batas Overload (Watt)</label>"
        "<input type='number' id='thr' value='" + String(appConfig.overloadThreshold, 0) + "' step='100'/>"
        "<button class='btn btn-p' onclick='save()'>Simpan Pengaturan</button>"
        "<button class='btn btn-g' onclick='resetWifi()'>Reset WiFi</button></div>"
        "<div id='toast'></div>"
        R"JS(<script>
var _tt;
function toast(msg,type){var el=document.getElementById('toast');
el.textContent=msg;el.className=type||'';el.classList.add('show');
clearTimeout(_tt);_tt=setTimeout(function(){el.classList.remove('show');},4000);}
function connect(){
    var manual=document.getElementById('ssid_manual').value.trim();
    var selected=document.getElementById('ssid').value;
    var ssid=manual||selected;
    var pass=document.getElementById('wpass').value;
    if(!ssid){toast('Pilih atau ketik nama jaringan WiFi','err');return;}
    toast('Menghubungkan ke "'+ssid+'"...','info');
    fetch('/connectwifi?ssid='+encodeURIComponent(ssid)+'&pass='+encodeURIComponent(pass))
        .then(function(r){return r.text();})
        .then(function(t){toast(t,(t.indexOf('Berhasil')>=0)?'ok':'err');})
        .catch(function(){toast('Timeout - coba lagi','err');});
}
function rescan(){
    toast('Scanning WiFi...','info');
    fetch('/rescan').then(function(){setTimeout(function(){location.reload();},3500);})
        .catch(function(){toast('Scan gagal dimulai','err');});
}
function save(){
    var thr=parseFloat(document.getElementById('thr').value);
    var trf=parseFloat(document.getElementById('trf').value);
    if(isNaN(thr)||thr<=0||thr>10000){toast('Threshold harus > 0 dan <= 10000 W','err');return;}
    if(isNaN(trf)||trf<=0){toast('Tarif tidak valid','err');return;}
    fetch('/save?thr='+thr+'&trf='+trf).then(function(r){return r.text();})
        .then(function(t){toast(t,(t.indexOf('Tersimpan')>=0)?'ok':'err');});
}
function resetWifi(){if(!confirm('Reset WiFi?'))return;
fetch('/resetwifi').then(function(){toast('Mereset...','info');});}
function offlineMode(){
    toast('Masuk Mode Offline...','info');
    fetch('/offline').then(function(r){return r.text();})
        .then(function(t){toast(t,'ok');setTimeout(function(){location.reload();},1500);})
        .catch(function(){toast('Gagal masuk Mode Offline','err');});
}
document.addEventListener('keydown',function(e){if(e.key==='Enter')connect();});
)JS";

    if (autoRefreshScan) {
        html += "setTimeout(function(){location.reload();},3500);";
    }

    html += "</script></body></html>";

    localServer.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
    localServer.send(200, "text/html; charset=utf-8", html);
}

static void handleSave() {
    bool ok = true;
    bool hasConfig = false;
    AppConfig next = appConfig;
    if (localServer.hasArg("thr")) {
        float v = localServer.arg("thr").toFloat();
        next.overloadThreshold = v;
        hasConfig = true;
    }
    if (localServer.hasArg("trf")) {
        float v = localServer.arg("trf").toFloat();
        next.electricityCostPerKwh = v;
        hasConfig = true;
    }
    if (hasConfig) ok = setAppConfig(next, "captive portal");
    else ok = false;
    if (ok) {
        syncPortalSettingsToFirebase();
        localServer.send(200, "text/plain",
            "Tersimpan! Threshold=" + String(appConfig.overloadThreshold, 0) +
            "W | Tarif=Rp" + String(appConfig.electricityCostPerKwh, 2));
    } else {
        localServer.send(400, "text/plain", "Nilai tidak valid");
    }
}

static void handleConnectWifi() {
    if (!localServer.hasArg("ssid")) {
        localServer.send(400, "text/plain", "SSID diperlukan");
        return;
    }
    String ssid = localServer.arg("ssid");
    String pass = localServer.hasArg("pass") ? localServer.arg("pass") : "";

    WiFi.scanDelete();
    WiFi.begin(ssid.c_str(), pass.c_str());
    indicatorsSetWifiSearching(true);
    int waited = 0;
    while (WiFi.status() != WL_CONNECTED && waited < 20) {
        indicatorsUpdate();
        delay(500);
        waited++;
        dnsServer.processNextRequest();
        localServer.handleClient();
    }
    indicatorsSetWifiSearching(false);
    indicatorsUpdate();

    if (WiFi.status() == WL_CONNECTED) {
        if (modeOffline) transitionToOnlineMode();
        else wifiConnected = true;

        WiFi.setSleep(false);
        if (!ntpSynced) ntpSynced = tryNTPSync();
        syncConfigFromFirebase();
        clearFirebaseCommand();
        fsSyncOfflineHistoryToFirebase();
        beginWifiScan();

        String ip = WiFi.localIP().toString();
        localServer.send(200, "text/plain",
            "Berhasil! IP: " + ip + " - Dashboard web aktif.");
    } else {
        beginWifiScan();
        localServer.send(200, "text/plain",
            "Gagal terhubung ke \"" + ssid + "\". Cek SSID & password.");
    }
}

static void handleOfflineMode() {
    localServer.send(200, "text/plain", "Mode Offline aktif. Relay ON untuk sesi offline pertama.");
    enterManualOfflineMonitoringMode("captive portal offline");
    beginWifiScan();
}

static void handleResetWifi() {
    localServer.send(200, "text/plain", "Mereset WiFi - ESP32 restart...");
    delay(500);
    if (sessionActive && hadDataOnce) fsWriteSession();
    WiFi.disconnect(true, true);
    delay(500);
    ESP.restart();
}

static void handleStatus() {
    String ip = wifiConnected ? WiFi.localIP().toString() : "-";
    int pending = fsCountOfflineHistory();
    String json = "{\"wifi\":" + String(wifiConnected ? "true" : "false") +
                  ",\"ip\":\"" + ip + "\"" +
                  ",\"mode\":\"" + String(modeOffline ? "offline" : "online") + "\"" +
                  ",\"systemMode\":\"" + String(systemModeToString(systemMode)) + "\"" +
                  ",\"sessionState\":\"" + String(sessionStateToString(sessionState)) + "\"" +
                  ",\"threshold\":" + String(appConfig.overloadThreshold, 0) +
                  ",\"tarif\":" + String(appConfig.electricityCostPerKwh, 2) +
                  ",\"pendingSync\":" + String(pending) + "}";
    localServer.send(200, "application/json", json);
}

static void handleRescan() {
    beginWifiScan();
    localServer.send(200, "text/plain", "Scanning...");
}

static void handleCaptivePortal() {
    handleRoot();
}

// ================================================================
// SETUP WEBSERVER
// ================================================================
void setupWebServer() {
    localServer.on("/",            HTTP_GET, handleRoot);
    localServer.on("/index.html",  HTTP_GET, handleRoot);
    localServer.on("/save",        HTTP_GET, handleSave);
    localServer.on("/connectwifi", HTTP_GET, handleConnectWifi);
    localServer.on("/offline",     HTTP_GET, handleOfflineMode);
    localServer.on("/resetwifi",   HTTP_GET, handleResetWifi);
    localServer.on("/status",      HTTP_GET, handleStatus);
    localServer.on("/rescan",      HTTP_GET, handleRescan);
    localServer.on("/scan",        HTTP_GET, handleRescan);

    localServer.on("/hotspot-detect.html", HTTP_GET, handleCaptivePortal);
    localServer.on("/generate_204",        HTTP_GET, handleCaptivePortal);
    localServer.on("/gen_204",             HTTP_GET, handleCaptivePortal);
    localServer.on("/ncsi.txt",            HTTP_GET, handleCaptivePortal);
    localServer.on("/connecttest.txt",     HTTP_GET, handleCaptivePortal);
    localServer.on("/redirect",            HTTP_GET, handleCaptivePortal);
    localServer.on("/fwlink",              HTTP_GET, handleCaptivePortal);
    localServer.onNotFound(handleCaptivePortal);

    localServer.begin();
    Serial.println("[LocalWeb] Captive portal aktif di http://192.168.4.1");
}
