// ================================================================
// network.cpp — Smart Energy Monitor v3.1
// WiFi, NTP, Local AP, WebServer / Captive Portal
// ================================================================

#include "network.h"
#include "config.h"
#include "state.h"
#include "storage.h"
#include "firebase.h"
#include "session.h"

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>

WebServer localServer(80);
DNSServer dnsServer;

// ================================================================
// WiFi CONNECT
// ================================================================
// Di network.cpp, ganti implementasi tryConnectWiFi
bool tryConnectWiFi(int sec) {
    WiFi.begin();
    Serial.print("[WiFi] Connecting");
    bool blink = false;
    unsigned long start = millis();
    unsigned long timeout = (unsigned long)sec * 1000UL;
    
    while (millis() - start < timeout && WiFi.status() != WL_CONNECTED) {
        // Tetap melayani DNS dan WebServer selama menunggu
        dnsServer.processNextRequest();
        localServer.handleClient();
        
        delay(100);
        
        // Blink setiap 500ms
        if ((millis() - start) % 500 < 100) {
            blink = !blink;
            digitalWrite(PIN_LED_BLUE, blink);
        }
    }
    
    if (WiFi.status() == WL_CONNECTED) {
        Serial.println("\n[WiFi] OK: " + WiFi.localIP().toString());
        digitalWrite(PIN_LED_BLUE, HIGH);
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
    dnsServer.stop();
    dnsServer.start(DNS_PORT, "*", ip);
    Serial.printf("[AP] '%s' aktif — 192.168.4.1\n", AP_SSID);
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
    
    String ssidOptions = "";
    String scanStatus  = "";
    
    if (n == WIFI_SCAN_RUNNING) {
        ssidOptions = "<option value=''>⏳ Sedang scan... refresh sebentar lagi</option>";
        scanStatus  = "<div style='color:#ffab00;font-size:12px;margin-bottom:8px;'>"
                      "⏳ Scan berjalan — <a href='/' style='color:#00e5ff;'>Refresh</a></div>";
    } else if (n <= 0) {
        WiFi.scanNetworks(true);  // async scan, results will be available on next page load
        ssidOptions = "<option value=''>Tidak ada jaringan — <a href='/'>Refresh</a></option>";
        scanStatus  = "<div style='color:#ff1744;font-size:12px;margin-bottom:8px;'>"
                      "Tidak ada jaringan ditemukan. "
                      "<a href='/' style='color:#00e5ff;'>Scan ulang</a></div>";
    } else {

        for (int i = 0; i < n; i++) {
            String ssid = WiFi.SSID(i);
            int rssi    = WiFi.RSSI(i);
            bool locked = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN);
            int bars = rssi > -50 ? 4 : rssi > -65 ? 3 : rssi > -75 ? 2 : 1;
            String bar = bars == 4 ? "████" : bars == 3 ? "███░" : bars == 2 ? "██░░" : "█░░░";
            ssidOptions += "<option value='" + ssid + "'>"
                        + bar + " " + ssid + (locked ? " 🔒" : "") + "</option>";
        }

    if (n == 0) ssidOptions = "<option value=''>Tidak ada jaringan ditemukan</option>";
    }
    int pendingSync = fsCountOfflineHistory();
    String syncInfo = "";
    if (pendingSync > 0) {
        syncInfo = "<div style='background:rgba(255,171,0,0.1);border:1px solid rgba(255,171,0,0.3);"
                   "border-radius:10px;padding:10px 14px;font-size:12px;color:#ffab00;margin-bottom:14px;'>"
                   "⚡ " + String(pendingSync) + " sesi offline menunggu sync ke Firebase</div>";
    }

    String html = "<!DOCTYPE html><html lang='id'><head>"
        "<meta charset='UTF-8'/><meta name='viewport' content='width=device-width,initial-scale=1'/>"
        "<title>SEM Config</title>"
        "<style>body{background:#0a0a0a;color:#f0f0f0;font-family:sans-serif;"
        "display:flex;flex-direction:column;align-items:center;padding:24px 16px}"
        ".card{background:#111;border:1px solid #222;border-radius:14px;padding:22px;"
        "width:100%;max-width:420px;margin-bottom:14px}"
        "label{display:block;font-size:11px;color:#666;text-transform:uppercase;"
        "letter-spacing:.06em;margin-bottom:7px;margin-top:14px}"
        "select,input{width:100%;padding:12px 14px;background:#1a1a1a;"
        "border:1px solid #2a2a2a;border-radius:10px;color:#f0f0f0;font-size:15px;outline:none}"
        "select:focus,input:focus{border-color:#00e5ff}"
        ".btn{display:block;width:100%;padding:14px;border:none;border-radius:12px;"
        "font-size:15px;font-weight:700;cursor:pointer;margin-top:16px}"
        ".btn-p{background:#00e5ff;color:#000}.btn-g{background:transparent;color:#555;"
        "border:1px solid #2a2a2a;margin-top:8px}"
        "#toast{position:fixed;bottom:24px;left:50%;transform:translateX(-50%) translateY(80px);"
        "background:#1a1a1a;border:1px solid #2a2a2a;color:#f0f0f0;padding:12px 20px;"
        "border-radius:12px;font-size:13px;transition:transform .3s;z-index:999}"
        "#toast.show{transform:translateX(-50%) translateY(0)}"
        "#toast.ok{color:#00e676}#toast.err{color:#ff1744}#toast.info{color:#00e5ff}"
        "</style></head><body>"
        "<h2 style='color:#00e5ff;font-size:16px;margin-bottom:20px'>⚡ Smart Energy Monitor</h2>"
        "<div class='card'><b style='color:#888;font-size:11px;text-transform:uppercase;"
        "letter-spacing:.1em'>Koneksi WiFi</b>"
        + syncInfo +
   // Di dalam handleRoot(), bagian HTML — ganti bagian form WiFi
String wifiForm = 
    "<label>Pilih Jaringan</label>"
    "<select id='ssid' onchange=\"document.getElementById('ssid_manual').value=this.value\">"
    + ssidOptions +
    "</select>"
    "<label>Atau ketik SSID manual</label>"
    "<input type='text' id='ssid_manual' placeholder='Ketik nama WiFi...' "
    "style='margin-bottom:4px;'/>"
    "<label>Password</label>"
    "<input type='password' id='wpass' placeholder='••••••••'/>"
    "<button class='btn btn-p' onclick='connect()'>Hubungkan</button>"
    "<button class='btn btn-g' onclick='rescan()'>🔄 Scan Ulang</button>";
        "<div class='card'><b style='color:#888;font-size:11px;text-transform:uppercase;"
        "letter-spacing:.1em'>Pengaturan</b>"
        "<label>Tarif / kWh (IDR)</label>"
        "<input type='number' id='trf' value='" + String(tarif, 2) + "' step='0.01'/>"
        "<label>Batas Overload (Watt)</label>"
        "<input type='number' id='thr' value='" + String(overloadThreshold, 0) + "' step='100'/>"
        "<button class='btn btn-p' onclick='save()'>Simpan Pengaturan</button>"
        "<button class='btn btn-g' onclick='resetWifi()'>Reset WiFi</button></div>"
        "<div id='toast'></div>"
        R"JS(<script>
var _tt;
function toast(msg,type){var el=document.getElementById('toast');
el.textContent=msg;el.className=type||'';el.classList.add('show');
clearTimeout(_tt);_tt=setTimeout(function(){el.classList.remove('show');},4000);}
function connect() {
    // Prioritas: input manual jika diisi, fallback ke dropdown
    var ssid = document.getElementById('ssid_manual').value.trim() 
               || document.getElementById('ssid').value;
    var pass = document.getElementById('wpass').value;
    if (!ssid) { toast('Pilih atau ketik nama jaringan WiFi', 'err'); return; }
    toast('Menghubungkan ke "' + ssid + '"...', 'info');
    fetch('/connectwifi?ssid=' + encodeURIComponent(ssid) 
          + '&pass=' + encodeURIComponent(pass))
        .then(function(r) { return r.text(); })
        .then(function(t) {
            if (t.indexOf('Berhasil') >= 0) toast('✓ ' + t, 'ok');
            else toast('✗ ' + t, 'err');
        }).catch(function() { toast('Timeout — coba lagi', 'err'); });
}

function rescan() {
    toast('Scanning...', 'info');
    fetch('/rescan').then(function() {
        setTimeout(function() { window.location.reload(); }, 3000);
    });
}
function save(){var thr=parseFloat(document.getElementById('thr').value);
var trf=parseFloat(document.getElementById('trf').value);
if(isNaN(thr)||thr<100||thr>10000){toast('Threshold 100–10000 W','err');return;}
if(isNaN(trf)||trf<=0){toast('Tarif tidak valid','err');return;}
fetch('/save?thr='+thr+'&trf='+trf).then(function(r){return r.text();})
.then(function(t){if(t.indexOf('Tersimpan')>=0)toast('✓ '+t,'ok');
else toast('✗ '+t,'err');});}
function resetWifi(){if(!confirm('Reset WiFi?'))return;
fetch('/resetwifi').then(function(){toast('Mereset...','info');});}
document.addEventListener('keydown',function(e){if(e.key==='Enter')connect();});
</script>)JS"
        "</body></html>";

    localServer.send(200, "text/html; charset=utf-8", html);
}

static void handleSave() {
    bool ok = false;
    if (localServer.hasArg("thr")) {
        float v = localServer.arg("thr").toFloat();
        if (v >= 100 && v <= 10000) { overloadThreshold = v; ok = true; }
    }
    if (localServer.hasArg("trf")) {
        float v = localServer.arg("trf").toFloat();
        if (v > 0) { tarif = v; ok = true; }
    }
    if (ok) {
        savePrefs();
        if (wifiConnected) {
            WiFiClientSecure c; c.setInsecure(); c.setTimeout(5000);
            HTTPClient h;
            if (h.begin(c, String(FIREBASE_HOST) + "/config/threshold.json")) {
                h.addHeader("Content-Type", "application/json");
                h.PUT(String(overloadThreshold, 0));
                h.end();
            }
        }
        localServer.send(200, "text/plain",
            "✓ Tersimpan! Threshold=" + String(overloadThreshold, 0) +
            "W | Tarif=Rp" + String(tarif, 2));
    } else {
        localServer.send(400, "text/plain", "Nilai tidak valid");
    }
}

static void handleConnectWifi() {
    if (!localServer.hasArg("ssid")) {
        localServer.send(400, "text/plain", "SSID diperlukan"); return;
    }
    String ssid = localServer.arg("ssid");
    String pass = localServer.hasArg("pass") ? localServer.arg("pass") : "";

    WiFi.begin(ssid.c_str(), pass.c_str());
    int waited = 0;
    while (WiFi.status() != WL_CONNECTED && waited < 20) {
        delay(500); waited++;
        localServer.handleClient();
    }

    if (WiFi.status() == WL_CONNECTED) {
        if (modeOffline) transitionToOnlineMode();
        else { wifiConnected = true; }

        WiFi.setSleep(false);
        if (!ntpSynced) ntpSynced = tryNTPSync();
        syncThresholdFromFirebase();
        clearFirebaseCommand();
        fsSyncOfflineHistoryToFirebase();

        String ip = WiFi.localIP().toString();
        localServer.send(200, "text/plain",
            "✓ Berhasil! IP: " + ip + " — Dashboard web aktif.");
    } else {
        localServer.send(200, "text/plain",
            "✗ Gagal terhubung ke \"" + ssid + "\". Cek SSID & password.");
    }
}

static void handleResetWifi() {
    localServer.send(200, "text/plain", "Mereset WiFi — ESP32 restart...");
    delay(500);
    if (sessionActive && hadDataOnce) fsWriteSession();
    WiFi.disconnect(true, true);
    delay(500);
    ESP.restart();
}

static void handleStatus() {
    String ip = wifiConnected ? WiFi.localIP().toString() : "—";
    int pending = fsCountOfflineHistory();
    String json = "{\"wifi\":" + String(wifiConnected ? "true" : "false") +
                  ",\"ip\":\"" + ip + "\"" +
                  ",\"mode\":\"" + String(modeOffline ? "offline" : "online") + "\"" +
                  ",\"threshold\":" + String(overloadThreshold, 0) +
                  ",\"tarif\":" + String(tarif, 2) +
                  ",\"pendingSync\":" + String(pending) + "}";
    localServer.send(200, "application/json", json);
}

static void handleCaptivePortal() {
    localServer.sendHeader("Location", "http://192.168.4.1/", true);
    localServer.send(302, "text/plain", "");
}

// ================================================================
// SETUP WEBSERVER
// ================================================================
void setupWebServer() {

    localServer.on(".rescan", HTTP_GET, []() {
        WiFi.scanDelete();
        WiFi.scanNetworks(true);
        localServer.send(200, "text/plain", "Scanning...");
    });
    localServer.on("/",            HTTP_GET, handleRoot);
    localServer.on("/index.html",  HTTP_GET, handleRoot);
    localServer.on("/save",        HTTP_GET, handleSave);
    localServer.on("/connectwifi", HTTP_GET, handleConnectWifi);
    localServer.on("/resetwifi",   HTTP_GET, handleResetWifi);
    localServer.on("/status",      HTTP_GET, handleStatus);

    auto captive = []() { handleCaptivePortal(); };
    localServer.on("/hotspot-detect.html", HTTP_GET, captive);
    localServer.on("/generate_204",        HTTP_GET, captive);
    localServer.on("/gen_204",             HTTP_GET, captive);
    localServer.on("/ncsi.txt",            HTTP_GET, captive);
    localServer.on("/connecttest.txt",     HTTP_GET, captive);
    localServer.on("/redirect",            HTTP_GET, captive);
    localServer.onNotFound(handleCaptivePortal);

    localServer.begin();
    Serial.println("[LocalWeb] Captive portal aktif di http://192.168.4.1");
}
