import {
  requireAuth, renderShell, fillUserInfo, setSystemStatus,
  showToast, applyTheme, updateChartColors, startStatusWatcher,
  loadAndApplySettings
} from "./auth-guard.js";
import { db, ref, onValue, set, push, get } from "./firebase-config.js";

// ================= INIT =================
const user = await requireAuth();
renderShell("dashboard", "DASHBOARD");
fillUserInfo(user);
startStatusWatcher();
const uid = user.uid;

// ================= FIREBASE PATHS =================
const historyRef  = ref(db, `users/${uid}/history`);
const settingsRef = ref(db, `users/${uid}/settings`);
const activeRef   = ref(db, `users/${uid}/activeSession`);
const commandRef  = ref(db, "command/relay");

// ================= SETTINGS =================
const SETTING_DEFAULTS = {
  currency: "IDR", tariff: 1444.70, overloadThreshold: 2000,
  notifDevice: true, notifDisconnect: true, notifSession: true,
  notifOverload: true, refreshInterval: 3000, theme: "dark", language: "en"
};
let settings = { ...SETTING_DEFAULTS };
settings = await loadAndApplySettings(uid);

// Refresh settings tiap 10 detik
setInterval(async () => {
  try {
    const snap = await get(settingsRef);
    if (snap.exists()) {
      const remote = { ...SETTING_DEFAULTS, ...snap.val() };
      if (JSON.stringify(remote) !== JSON.stringify(settings)) {
        settings = remote;
        localStorage.setItem(`sem_settings_${uid}`, JSON.stringify(settings));
        applyTheme(settings.theme);
        startMetersInterval();
      }
    }
  } catch {}
}, 10000);

// ================= CONSTANTS =================
const STALE_THRESHOLD = 15;
const AUTO_SAVE_AFTER_OFFLINE_SEC = 60;

// ================= STATE — FIREBASE DATA =================
let voltage = 0, current = 0, firebasePower = 0, firebaseTimestamp = 0;
let firebasePF = 0, firebaseFreq = 0, firebaseApparent = 0;
let firebaseEnergy = 0, firebaseCost = 0;
let firebaseOverload = false;
let firebaseRelay    = false;
let firebaseOffline  = false;
let systemInternet   = false;
let deviceNameFromEsp = "—";  // Device name dari ESP32 (untuk offline mode)

// ================= STATE — WEB =================
let systemOnline = false;
let deviceOnline = false;
let prevDeviceConnected = false;
let prevOverload        = false;
let prevRelayState      = false;
let prevSystemOnline    = false;
let isRunning    = false;
let startTime    = null;
let timerInterval = null;
let sessionSaved = false;
let activeDevice = null;
let waitingForName = false;
let metersInterval = null;
let energyBaseline  = 0;
let sessionCount    = 0;
let lastknownEnergy = 0;
let offlineSessionStartEnergy = null;

// ── BUG 1 FIX: pendingDeviceName moved to MODULE scope ──────────
// Was declared inside updateMeters() (local), so btnSaveDev handler
// could never read/write the same variable. Now shared correctly.
let pendingDeviceName = null;
let pendingSessionId = null;

// ================= STATE — OFFLINE TRACKING =================
let offlineDetectedAt       = null;
let offlineBannerShown      = false;
let reconnectToastShown     = false;
let autoSaveTriggered       = false;

// ================= RELAY COMMAND =================
function makeId(prefix) {
  return `${prefix}_${Date.now()}_${Math.random().toString(36).slice(2, 8)}`;
}

async function sendRelayCommand(type, payload = {}) {
  try {
    const on = type === "START";
    const command = {
      id: makeId("cmd"),
      type,
      uid,
      createdAt: Date.now(),
      ...payload
    };
    await set(commandRef, command);
    console.log(`[Relay] Command ${on ? "ON" : "OFF"} → Firebase`);
  } catch (e) {
    console.error("[Relay] Gagal:", e);
    showToast("Gagal kirim perintah relay", "error");
  }
}

// ================= STORAGE =================
async function getHistory() {
  try {
    const snap = await get(historyRef);
    if (!snap.exists()) return [];
    return Object.entries(snap.val())
      .map(([key, val]) => ({ ...val, _key: key }))
      .sort((a, b) => b.timestamp - a.timestamp);
  } catch { return []; }
}

async function pushHistory(entry) {
  try { await push(historyRef, entry); }
  catch (e) { console.error("[History] pushHistory gagal:", e); }
}

async function getSessionCount() {
  try {
    const snap = await get(historyRef);
    return snap.exists() ? Object.keys(snap.val()).length : 0;
  } catch { return 0; }
}

async function saveActiveSession(data) {
  try { await set(activeRef, data); } catch {}
}
async function loadActiveSession() {
  try {
    const snap = await get(activeRef);
    return snap.exists() ? snap.val() : null;
  } catch { return null; }
}

// ================= ELEMENTS =================
const valVoltage      = document.getElementById("val-voltage");
const valCurrent      = document.getElementById("val-current");
const valPower        = document.getElementById("val-power");
const valEnergy       = document.getElementById("val-energy");
const valCost         = document.getElementById("val-cost");
const valDeviceName   = document.getElementById("val-device-name");
const subDuration     = document.getElementById("sub-duration");
const subTariff       = document.getElementById("sub-tariff");
const subWebStatus    = document.getElementById("sub-web-status");
const valSessionCount = document.getElementById("val-session-count");
const activeDevLabel  = document.getElementById("active-device-label");
const badgeStatus     = document.getElementById("badge-device-status");
const deviceTabsEl    = document.getElementById("device-tabs");
const btnStop         = document.getElementById("btn-stop");
const fab             = document.getElementById("fab-add");
const modalAdd        = document.getElementById("modal-add-device");
const inputDevName    = document.getElementById("input-device-name");
const btnCancelDev    = document.getElementById("btn-cancel-device");
const btnSaveDev      = document.getElementById("btn-save-device");
const gaugeVoltage    = document.getElementById("gauge-voltage");
const gaugeCurrent    = document.getElementById("gauge-current");
const overloadBanner  = document.getElementById("overload-banner");

// ================================================================
// BANNER: OFFLINE ESP32
// ================================================================
let offlineBannerEl = document.getElementById("offline-banner");
if (!offlineBannerEl) {
  offlineBannerEl = document.createElement("div");
  offlineBannerEl.id = "offline-banner";
  offlineBannerEl.style.cssText = `
    display:none; align-items:center; gap:12px;
    background:rgba(255,171,0,0.08); border:1px solid rgba(255,171,0,0.35);
    border-radius:var(--radius-md); padding:14px 20px; margin-bottom:20px;
    color:var(--amber); font-size:13px; font-weight:500;`;
  offlineBannerEl.innerHTML = `
    <span style="font-size:18px;">⚠</span>
    <div style="flex:1;">
      <div style="font-weight:600;margin-bottom:2px;" id="offline-banner-title">
        ESP32 tidak merespons
      </div>
      <div style="font-size:12px;color:var(--text-muted);" id="offline-banner-sub">
        Menunggu koneksi...
      </div>
    </div>
    <span id="offline-duration-badge" style="
      font-family:var(--font-display);font-size:11px;font-weight:700;
      background:rgba(255,171,0,0.15);border:1px solid rgba(255,171,0,0.3);
      padding:3px 10px;border-radius:20px;white-space:nowrap;"></span>`;

  const ob = document.getElementById("overload-banner");
  if (ob?.parentNode) ob.parentNode.insertBefore(offlineBannerEl, ob.nextSibling);
  else document.querySelector(".page-content")?.prepend(offlineBannerEl);
}

let offlineDurationInterval = null;

function showOfflineBanner(firstDetectedAt) {
  offlineBannerEl.style.display = "flex";
  offlineBannerShown = true;

  const titleEl    = document.getElementById("offline-banner-title");
  const subEl      = document.getElementById("offline-banner-sub");
  const durationEl = document.getElementById("offline-duration-badge");

  const lastSeen = firebaseTimestamp > 0
    ? new Date(firebaseTimestamp * 1000).toLocaleTimeString("id-ID")
    : "—";
  
  // Update banner text berdasarkan kondisi ESP32
  if (!systemInternet && firebaseTimestamp > 0) {
    if (titleEl) titleEl.textContent = "⚠ ESP32 tidak terhubung ke internet";
    if (firebaseOffline && firebaseRelay) {
      // ESP32 dalam mode offline dengan relay ON
      if (subEl) subEl.textContent = `Mode: Offline • Device: ${deviceNameFromEsp} • Data terakhir: ${lastSeen}`;
    } else {
      if (subEl) subEl.textContent = `Data terakhir: ${lastSeen}`;
    }
  } else if (firebaseOffline && firebaseTimestamp > 0) {
    if (titleEl) titleEl.textContent = "📡 ESP32 Mode: OFFLINE";
    if (subEl) subEl.textContent = `Device: ${deviceNameFromEsp} • Relay: ON • Mengukur lokal • Data terakhir: ${lastSeen}`;
  } else {
    if (titleEl) titleEl.textContent = "⚠ ESP32 tidak merespons";
    if (subEl) subEl.textContent = `Data terakhir: ${lastSeen}`;
  }

  if (offlineDurationInterval) clearInterval(offlineDurationInterval);
  offlineDurationInterval = setInterval(() => {
    if (!firstDetectedAt || !durationEl) return;
    const secs = Math.floor((Date.now() - firstDetectedAt) / 1000);
    durationEl.textContent = secs < 60
      ? `Offline ${secs}s`
      : `Offline ${Math.floor(secs / 60)}m ${secs % 60}s`;
  }, 1000);
}

function hideOfflineBanner() {
  offlineBannerEl.style.display = "none";
  offlineBannerShown = false;
  if (offlineDurationInterval) {
    clearInterval(offlineDurationInterval);
    offlineDurationInterval = null;
  }
}

// ================================================================
// BANNER: MODE STATUS & PENDING OFFLINE SESSIONS
// ================================================================
let modeStatusBanner = document.getElementById("mode-status-banner");
if (!modeStatusBanner) {
  modeStatusBanner = document.createElement("div");
  modeStatusBanner.id = "mode-status-banner";
  modeStatusBanner.style.cssText = `
    display:none; align-items:center; justify-content:space-between; gap:12px;
    background:rgba(255,171,0,0.08); border:1px solid rgba(255,171,0,0.3);
    border-radius:var(--radius-md); padding:12px 16px; margin-bottom:14px;
    color:var(--amber); font-size:12px; font-weight:600;`;
  modeStatusBanner.innerHTML = `
    <span id="mode-status-text">⚠ Mode: OFFLINE • Relay: ON</span>
    <span id="pending-sync-badge" style="display:none;
      background:rgba(255,171,0,0.2);border:1px solid rgba(255,171,0,0.4);
      padding:2px 8px;border-radius:10px;font-size:11px;font-weight:700;">
      0 Pending
    </span>`;

  const ob = document.getElementById("overload-banner");
  if (ob?.parentNode) ob.parentNode.insertBefore(modeStatusBanner, ob);
  else document.querySelector(".page-content")?.prepend(modeStatusBanner);
}

function updateModeStatusBanner(isOnline, relayOn, pendingCount = 0) {
  if (!isOnline && relayOn) {
    modeStatusBanner.style.display = "flex";
    const textEl = document.getElementById("mode-status-text");
    const badgeEl = document.getElementById("pending-sync-badge");
    if (textEl) textEl.textContent = `📡 Mode: OFFLINE • Relay: ON`;
    if (badgeEl) {
      badgeEl.style.display = pendingCount > 0 ? "inline-block" : "none";
      badgeEl.textContent = `${pendingCount} Pending Sync`;
    }
  } else {
    modeStatusBanner.style.display = "none";
  }
}

// ================================================================
// BANNER: RELAY READY
// ================================================================
let relayBanner = document.getElementById("relay-banner");
if (!relayBanner) {
  relayBanner = document.createElement("div");
  relayBanner.id = "relay-banner";
  relayBanner.style.cssText = `
    display:none; align-items:center; justify-content:space-between; gap:12px;
    background:rgba(0,229,255,0.08); border:1px solid rgba(0,229,255,0.3);
    border-radius:var(--radius-md); padding:14px 20px; margin-bottom:20px;
    color:var(--cyan); font-size:13px; font-weight:500;`;
  relayBanner.innerHTML = `
    <span>⚡ Klik <strong>+</strong> untuk memulai sesi pengukuran baru</span>`;

  const ob = document.getElementById("overload-banner");
  if (ob?.parentNode) ob.parentNode.insertBefore(relayBanner, ob.nextSibling);
  else document.querySelector(".page-content")?.prepend(relayBanner);
}

function setRelayBanner(show) {
  relayBanner.style.display = show ? "flex" : "none";
}

// ================================================================
// BANNER: OFFLINE SESSION ACTIVE
// ================================================================
let offlineSessionBanner = document.getElementById("offline-session-banner");
if (!offlineSessionBanner) {
  offlineSessionBanner = document.createElement("div");
  offlineSessionBanner.id = "offline-session-banner";
  offlineSessionBanner.style.cssText = `
    display:none; align-items:center; gap:12px;
    background:rgba(0,229,255,0.05); border:1px solid rgba(0,229,255,0.2);
    border-radius:var(--radius-md); padding:14px 20px; margin-bottom:20px;
    color:var(--cyan); font-size:13px;`;
  offlineSessionBanner.innerHTML = `
    <span style="font-size:18px;">📡</span>
    <div>
      <div style="font-weight:600;margin-bottom:2px;" id="offline-session-title">
        ESP32 mengukur dalam mode offline
      </div>
      <div style="font-size:12px;color:var(--text-muted);" id="offline-session-sub">
        Data akan ditampilkan saat ESP32 kembali terhubung ke internet
      </div>
    </div>`;

  const ob = document.getElementById("overload-banner");
  if (ob?.parentNode) ob.parentNode.insertBefore(offlineSessionBanner, ob.nextSibling);
  else document.querySelector(".page-content")?.prepend(offlineSessionBanner);
}

function setOfflineSessionBanner(show, deviceName = "—") {
  offlineSessionBanner.style.display = show ? "flex" : "none";
  if (show && deviceName && deviceName !== "—") {
    const titleEl = document.getElementById("offline-session-title");
    const subEl = document.getElementById("offline-session-sub");
    if (titleEl) titleEl.textContent = `📡 ${deviceName} (Mode Offline)`;
    if (subEl) subEl.textContent = `Relay ON • Data disimpan lokal • Akan tersinkron saat online`;
  }
}

// ================= CHART OPTIONS =================
function chartTickColor() {
  return getComputedStyle(document.documentElement).getPropertyValue("--chart-tick").trim() || "#666";
}
function chartGridColor() {
  return getComputedStyle(document.documentElement).getPropertyValue("--chart-grid").trim() || "rgba(255,255,255,0.04)";
}
function makeChartOpts(extra = {}) {
  return {
    responsive: true, maintainAspectRatio: false,
    plugins: { legend: { display: false } },
    scales: {
      x: { ticks: { color: chartTickColor(), font: { size: 10 } }, grid: { color: chartGridColor() }, ...extra.x },
      y: { ticks: { color: chartTickColor(), font: { size: 10 } }, grid: { color: chartGridColor() }, ...extra.y }
    }
  };
}
const lineChart = new Chart(document.getElementById("chart-line"), {
  type: "line",
  data: { labels: [], datasets: [{ label: "Power (W)", data: [],
    borderColor: "#ffab00", backgroundColor: "rgba(255,171,0,0.08)",
    tension: 0.4, fill: true, pointRadius: 3, pointBackgroundColor: "#ffab00" }] },
  options: makeChartOpts()
});
const barChart = new Chart(document.getElementById("chart-bar"), {
  type: "bar",
  data: { labels: [], datasets: [{ label: "Avg Power (W)", data: [],
    backgroundColor: "rgba(0,229,255,0.6)", borderColor: "#00e5ff",
    borderWidth: 1, borderRadius: 4 }] },
  options: makeChartOpts()
});
const pieChart = new Chart(document.getElementById("chart-pie"), {
  type: "doughnut",
  data: { labels: [], datasets: [{ data: [],
    backgroundColor: ["#00e5ff","#00e676","#ffab00","#ff1744","#7c4dff","#ff6d00"],
    borderWidth: 0 }] },
  options: {
    responsive: true, maintainAspectRatio: false,
    plugins: { legend: { display: true, position: "bottom",
      labels: { color: chartTickColor(), font: { size: 10 }, boxWidth: 10, padding: 12 } } }
  }
});

// ================= HELPERS =================
const symbol     = () => settings.currency === "USD" ? "$" : "Rp";
const formatCost = v  => settings.currency === "USD"
  ? `$ ${v.toFixed(2)}`
  : `Rp ${Math.round(v).toLocaleString("id-ID")}`;

function getSessionEnergy() {
  return Math.max(0, lastknownEnergy - energyBaseline);
}
function getSessionCost() {
  return getSessionEnergy() * settings.tariff;
}

function generateUniqueName(base, usedNames) {
  if (!usedNames.includes(base)) return base;
  let c = 2;
  while (usedNames.includes(`${base} ${c}`)) c++;
  return `${base} ${c}`;
}
function setGauge(el, val, min, max) {
  if (!el) return;
  el.style.strokeDashoffset = 232 - Math.max(0, Math.min(1, (val - min) / (max - min))) * 232;
}
function clearDisplay() {
  if (valVoltage) valVoltage.textContent = "0";
  if (valCurrent) valCurrent.textContent = "0.00";
  if (valPower)   valPower.textContent   = "0";
  if (valEnergy)  valEnergy.textContent  = "0.000";
  if (valCost)    valCost.textContent    = formatCost(0);
  setGauge(gaugeVoltage, 0, 190, 240);
  setGauge(gaugeCurrent, 0, 0, 16);
}
function updateDisplay() {
  const sessEnergy = getSessionEnergy();
  const sessCost   = getSessionCost();
  if (valVoltage) valVoltage.textContent = voltage.toFixed(1);
  if (valCurrent) valCurrent.textContent = current.toFixed(2);
  if (valPower)   valPower.textContent   = firebasePower.toFixed(0);
  if (valEnergy)  valEnergy.textContent  = sessEnergy.toFixed(3);
  if (valCost)    valCost.textContent    = formatCost(sessCost);
  setGauge(gaugeVoltage, voltage, 190, 240);
  setGauge(gaugeCurrent, current, 0, 16);
  const elPF   = document.getElementById("val-pf");
  const elFreq = document.getElementById("val-freq");
  const elApp  = document.getElementById("val-apparent");
  if (elPF)   elPF.textContent   = firebasePF > 0 ? firebasePF.toFixed(2) : "—";
  if (elFreq) elFreq.textContent = firebaseFreq > 0 ? firebaseFreq.toFixed(1) : "—";
  if (elApp)  elApp.textContent  = firebaseApparent > 0 ? firebaseApparent.toFixed(0) : "—";
}
function updateTimer() {
  if (!startTime) return;
  const d = Date.now() - startTime;
  const h = String(Math.floor(d / 3600000)).padStart(2, "0");
  const m = String(Math.floor((d % 3600000) / 60000)).padStart(2, "0");
  const s = String(Math.floor((d % 60000) / 1000)).padStart(2, "0");
  if (subDuration) subDuration.textContent = `Duration: ${h}:${m}:${s}`;
}
function getDuration() {
  if (!startTime) return "00:00:00";
  const d = Date.now() - startTime;
  const h = String(Math.floor(d / 3600000)).padStart(2, "0");
  const m = String(Math.floor((d % 3600000) / 60000)).padStart(2, "0");
  const s = String(Math.floor((d % 60000) / 1000)).padStart(2, "0");
  return `${h}:${m}:${s}`;
}
function setDeviceBadge(state) {
  if (!badgeStatus) return;
  const map = {
    connected: ["badge online",  "● Connected"],
    overload:  ["badge offline", "⚠ Overload!"],
    idle:      ["badge idle",    "● Idle"],
    offline:   ["badge offline", "● Offline"],
    unknown:   ["badge unknown", "● Unknown"]
  };
  const [cls, txt] = map[state] || map.unknown;
  badgeStatus.className = cls;
  badgeStatus.textContent = txt;
}
async function updateSessionCount() {
  sessionCount = await getSessionCount();
  if (valSessionCount) valSessionCount.textContent = sessionCount;
}

// ================= OVERLOAD BANNER =================
function setOverloadBanner(active) {
  if (!overloadBanner) return;
  overloadBanner.style.display = active ? "flex" : "none";
  if (active) overloadBanner.textContent =
    `⚠ OVERLOAD — Daya melebihi ${settings.overloadThreshold}W! Relay dimatikan otomatis.`;
}

// ================= DEVICE TABS =================
async function renderDeviceTabs() {
  if (!deviceTabsEl) return;
  deviceTabsEl.innerHTML = "";
  const history = await getHistory();
  const names   = [...new Set(history.map(h => h.name))];
  names.forEach(name => {
    const btn = document.createElement("button");
    btn.className = `device-tab${activeDevice?.name === name ? " active" : ""}`;
    btn.textContent = name;
    deviceTabsEl.appendChild(btn);
  });
}

// ================= SAVE SESSION =================
async function saveSession() {
  if (sessionSaved || !activeDevice) return;
  const sessEnergy = getSessionEnergy();
  const sessCost   = getSessionCost();
  await pushHistory({
    name:      activeDevice.name,
    duration:  getDuration(),
    power:     parseFloat(firebasePower.toFixed(1)),
    energy:    parseFloat(sessEnergy.toFixed(3)),
    cost:      formatCost(sessCost),
    costRaw:   sessCost,
    date:      new Date().toLocaleDateString("id-ID"),
    timestamp: Date.now()
  });
  sessionSaved = true;
  await updateSessionCount();
  await updateBarPie();
  if (settings.notifSession)
    showToast(`Sesi "${activeDevice.name}" tersimpan ✓`, "success");
}

// ================= RESET MONITORING =================
async function resetMonitoring() {
  clearInterval(timerInterval);
  timerInterval   = null;
  startTime       = null;
  isRunning       = false;
  sessionSaved    = false;
  energyBaseline  = 0;
  activeDevice    = null;
  lastknownEnergy = 0;
  autoSaveTriggered = false;
  offlineSessionStartEnergy = null;
  pendingDeviceName = null;
  pendingSessionId = null;
  await saveActiveSession(null);
  voltage = current = firebasePower = 0;
  clearDisplay();
  if (subDuration)    subDuration.textContent    = "Duration: 00:00:00";
  if (valDeviceName)  valDeviceName.textContent  = "—";
  if (activeDevLabel) activeDevLabel.textContent = "No active device";
  if (btnStop)        btnStop.style.display      = "none";
  setDeviceBadge("idle");
  setOverloadBanner(false);
  renderDeviceTabs();
}

// ================= START MONITORING =================
let deviceConnectTime   = null;
let deviceConnectEnergy = 0;

async function startMonitoring(name) {
  activeDevice    = { id: pendingSessionId || makeId("sess"), name };
  startTime       = deviceConnectTime   || Date.now();
  energyBaseline  = (deviceConnectEnergy !== undefined && deviceConnectEnergy > 0)
                    ? deviceConnectEnergy
                    : firebaseEnergy;
  lastknownEnergy = energyBaseline;
  isRunning       = true;
  sessionSaved    = false;
  autoSaveTriggered = false;
  offlineSessionStartEnergy = null;
  pendingSessionId = null;

  await saveActiveSession({ ...activeDevice, startTime, energyBaseline });
  if (valDeviceName)  valDeviceName.textContent  = name;
  if (activeDevLabel) activeDevLabel.textContent = `Monitoring: ${name}`;
  if (btnStop)        btnStop.style.display      = "inline-flex";
  setDeviceBadge("connected");
  clearInterval(timerInterval);
  timerInterval = setInterval(updateTimer, 1000);
  renderDeviceTabs();

  const retroMs  = Date.now() - startTime;
  const retroMin = Math.round(retroMs / 60000);
  if (retroMin >= 1)
    showToast(`Monitoring "${name}" dimulai (${retroMin} mnt terhitung)`, "success");
  else
    showToast(`Monitoring "${name}" dimulai ▶`, "success");
}

// ================= CHART UPDATE =================
async function updateBarPie() {
  const history  = await getHistory();
  const byDevice = {};
  history.forEach(s => {
    if (!byDevice[s.name]) byDevice[s.name] = { power: 0, energy: 0, count: 0 };
    byDevice[s.name].power  += s.power;
    byDevice[s.name].energy += s.energy;
    byDevice[s.name].count++;
  });
  const names    = Object.keys(byDevice);
  const powers   = names.map(n => parseFloat((byDevice[n].power / byDevice[n].count).toFixed(1)));
  const energies = names.map(n => parseFloat(byDevice[n].energy.toFixed(3)));
  barChart.data.labels = names; barChart.data.datasets[0].data = powers; barChart.update();
  pieChart.data.labels = names; pieChart.data.datasets[0].data = energies; pieChart.update();
}

// ================= MODAL — BERI NAMA DEVICE =================
let namingReminderTimeout = null;

async function openModalAuto() {
  waitingForName = true;
  const history   = await getHistory();
  const usedNames = history.map(h => h.name);
  document.querySelector("#modal-add-device .modal-title")
    .textContent = "⚡ Device Terdeteksi!";
  document.querySelector("#modal-add-device .modal-sub")
    .textContent = "Berikan nama untuk device yang baru terhubung.";
  inputDevName.value = "";
  inputDevName.dataset.usedNames = JSON.stringify(usedNames);
  modalAdd.classList.add("open");
  setTimeout(() => inputDevName.focus(), 100);
  clearTimeout(namingReminderTimeout);
  namingReminderTimeout = setTimeout(() => {
    if (waitingForName) {
      const elapsed = deviceConnectTime
        ? Math.round((Date.now() - deviceConnectTime) / 1000) : "?";
      showToast(`⚠ Device sudah ${elapsed}s terhubung. Segera beri nama!`, "error");
    }
  }, 30000);
}

async function openModalManual() {
  if (!systemOnline) {
    showToast("⚠ ESP32 tidak terhubung — Hanya bisa memulai di Online Mode", "error"); 
    return;
  }
  if (isRunning && !deviceOnline) {
    showToast(`⚠ "${activeDevice.name}" sedang dimonitor`, "error");
    return;
  }
  waitingForName = false;
  const history   = await getHistory();
  const usedNames = history.map(h => h.name);
  document.querySelector("#modal-add-device .modal-title")
    .textContent = "Tambah Device (Online Mode)";
  document.querySelector("#modal-add-device .modal-sub")
    .textContent = "Berikan nama device, lalu klik Start Monitoring untuk menyalakan relay.";
  inputDevName.value = "";
  inputDevName.dataset.usedNames = JSON.stringify(usedNames);
  modalAdd.classList.add("open");
  setTimeout(() => inputDevName.focus(), 100);
}

// ── BUG 1 FIX: Single btnSaveDev listener ────────────────────────
// Original code had TWO addEventListener calls on btnSaveDev:
//   1st: called startMonitoring(name) directly
//   2nd: called sendRelayCommand(true) and set pendingDeviceName
// Both fired on every click, causing a race condition (relay command
// AND immediate startMonitoring ran simultaneously).
// Fix: ONE handler that checks waitingForName to decide the path.
btnSaveDev.addEventListener("click", async () => {
  const usedNames = JSON.parse(inputDevName.dataset.usedNames || "[]");
  let name = inputDevName.value.trim();
  if (!name) name = generateUniqueName("Device", usedNames);
  else name = generateUniqueName(name, usedNames);
  if (name.length > 24) { showToast("Maksimal 24 karakter", "error"); return; }
  closeModal();

  if (waitingForName) {
    // Auto-open path: device already connected, start monitoring directly
    await startMonitoring(name);
  } else {
    // Manual FAB path: turn relay ON first, wait for device detection
    // pendingDeviceName is read in updateMeters when device comes online
    showToast(`Menyalakan relay untuk "${name}"...`, "");
    pendingDeviceName = name;
    pendingSessionId = makeId("sess");
    await sendRelayCommand("START", {
      sessionId: pendingSessionId,
      deviceName: name,
      tariff: settings.tariff,
      threshold: settings.overloadThreshold
    });
  }
});

function closeModal() {
  modalAdd.classList.remove("open");
  inputDevName.value = "";
  waitingForName = false;
  clearTimeout(namingReminderTimeout);
}

fab.addEventListener("click", openModalManual);
btnCancelDev.addEventListener("click", closeModal);
modalAdd.addEventListener("click", e => {
  if (e.target === modalAdd && !waitingForName) closeModal();
});
inputDevName.addEventListener("keydown", e => {
  if (e.key === "Enter") btnSaveDev.click();
});

// ================= STOP SESSION =================
if (btnStop) {
  btnStop.addEventListener("click", async () => {
    if (!isRunning || !activeDevice) {
      showToast("Tidak ada sesi yang berjalan", "error"); return;
    }
    await saveSession();
    await sendRelayCommand("STOP", {
      sessionId: activeDevice.id,
      reason: "USER_STOP"
    });
    showToast("Sesi dihentikan — relay OFF ⏹", "");
    await resetMonitoring();
  });
}

// ================= FIREBASE LIVE LISTENER =================
onValue(ref(db, "live"), snapshot => {
  const data = snapshot.val();
  if (!data) return;

  const sys = data.system || {};
  systemInternet    = sys.internet === true;
  firebaseTimestamp = sys.timestamp || 0;
  firebaseRelay     = sys.relay     === true;
  firebaseOffline   = sys.offline   === true;
  deviceNameFromEsp = sys.deviceName || "Device";  // Ambil nama device dari ESP32

  const dev = data.device || {};
  voltage          = dev.voltage   || 0;
  current          = dev.current   || 0;
  firebasePower    = dev.power     || 0;
  firebaseApparent = dev.apparent  || 0;
  firebasePF       = dev.pf        || 0;
  firebaseFreq     = dev.frequency || 0;
  firebaseEnergy   = dev.energy    || 0;
  firebaseCost     = dev.cost      || 0;
  firebaseOverload = dev.overload  === true;

  if (current > 0.01 && firebasePower > 0.5 && firebaseEnergy > 0) {
    lastknownEnergy = firebaseEnergy;
  }
});

// ================================================================
// MAIN LOOP
// ── BUG 1 FIX: declared as async so await calls inside are valid ──
// Original was a plain function; any await inside silently returned
// a Promise that was never caught, breaking the entire update cycle.
// ================================================================
async function updateMeters() {
  const now  = Math.floor(Date.now() / 1000);
  const diff = now - firebaseTimestamp;

  // ── Hitung systemOnline ──────────────────────────────────
  const dataFresh = firebaseTimestamp > 0 && diff <= STALE_THRESHOLD;
  systemOnline = systemInternet && dataFresh;
  deviceOnline = systemOnline && current > 0.01 && firebasePower > 0.5;

  const webOverload = deviceOnline && firebasePower >= settings.overloadThreshold;

  // ── Offline banner ──────────────────────────────────────
  if (!systemOnline && firebaseTimestamp > 0) {
    if (!offlineDetectedAt) offlineDetectedAt = Date.now();
    const offSec = (Date.now() - offlineDetectedAt) / 1000;
    if (offSec >= 5 && !offlineBannerShown) {
      showOfflineBanner(offlineDetectedAt);
      reconnectToastShown = false;
    }
  } else if (systemOnline) {
    if (offlineBannerShown) {
      hideOfflineBanner();
      if (!reconnectToastShown && offlineDetectedAt) {
        const totalSec = Math.round((Date.now() - offlineDetectedAt) / 1000);
        const label = totalSec < 60 ? `${totalSec}s` : `${Math.round(totalSec / 60)}m`;
        showToast(`✓ ESP32 kembali online (offline ${label})`, "success");
        reconnectToastShown = true;
      }
    }
    offlineDetectedAt = null;
    offlineBannerShown = false;
    autoSaveTriggered  = false;
  } else if (firebaseTimestamp === 0 && !offlineBannerShown) {
    if (!offlineDetectedAt) offlineDetectedAt = Date.now();
    if ((Date.now() - offlineDetectedAt) / 1000 >= 10) {
      showOfflineBanner(offlineDetectedAt);
    }
  }

  // ── Offline session banner ──────────────────────────────
  // Tampilkan banner saat ESP32 dalam mode offline dengan relay ON
  setOfflineSessionBanner(firebaseOffline && firebaseRelay && !systemOnline, deviceNameFromEsp);
  
  // ── Mode status banner ────────────────────────────────
  // Tampilkan badge mode offline dengan jumlah sesi pending sync
  // TODO: Get pending count dari API atau Firebase
  updateModeStatusBanner(!systemOnline, firebaseRelay, 0);

  // ── Update status bar ──────────────────────────────────
  setSystemStatus(systemOnline);
  if (systemOnline) {
    if (subWebStatus) subWebStatus.textContent = "Web: online";
  } else if (firebaseTimestamp > 0) {
    if (subWebStatus) subWebStatus.textContent = diff < 60
      ? `Web: offline (${diff}s)` : `Web: offline (${Math.round(diff / 60)}m)`;
  } else {
    if (subWebStatus) subWebStatus.textContent = "Web: menunggu ESP32...";
  }
  if (subTariff) subTariff.textContent =
    `Tariff: ${symbol()} ${settings.tariff.toLocaleString("id-ID")}/kWh`;

  // ── Relay banner ────────────────────────────────────────
  const showRelayBanner = systemOnline && !firebaseRelay && !isRunning;
  setRelayBanner(showRelayBanner);

  // ── Relay baru ON (ESP32 konfirmasi) ───────────────────
  if (!prevRelayState && firebaseRelay && systemOnline) {
    setRelayBanner(false);
  }
  prevRelayState = firebaseRelay;

  // ── Device baru terdeteksi ─────────────────────────────
  if (!prevDeviceConnected && deviceOnline) {
    deviceConnectTime   = Date.now();
    deviceConnectEnergy = firebaseEnergy;
    lastknownEnergy     = firebaseEnergy;
    if (!isRunning) {
      if (pendingDeviceName) {
        // Manual FAB path: relay was turned on, name was pre-set
        const name = pendingDeviceName;
        pendingDeviceName = null;
        await startMonitoring(name);
      } else if (!waitingForName) {
        // Auto-detect path: device plugged in without FAB
        if (settings.notifDevice)
          showToast("⚡ Device terdeteksi! Berikan nama.", "success");
        openModalAuto();
      }
    }
  }

  // ── Transfer data offline → online ────────────────────
  // Saat esp32 reconnect dari offline mode, data akan disinkron
  if (systemOnline && prevSystemOnline === false && isRunning && activeDevice) {
    if (firebaseEnergy > 0) {
      lastknownEnergy = firebaseEnergy;
      console.log(`[Transfer] Offline→Online: E=${firebaseEnergy.toFixed(4)} kWh`);
      showToast(
        `✓ ESP32 online • Data offline tersinkron: ${firebaseEnergy.toFixed(4)} kWh`, 
        "success"
      );
    } else {
      showToast(`✓ ESP32 online • Mode: ${firebaseOffline ? "OFFLINE" : "ONLINE"}`, "success");
    }
  }

  // ── Auto-save sesi saat offline > AUTO_SAVE_AFTER_OFFLINE_SEC ──
  if (!systemOnline && isRunning && activeDevice && offlineDetectedAt && !autoSaveTriggered) {
    const offSec = (Date.now() - offlineDetectedAt) / 1000;
    if (offSec >= AUTO_SAVE_AFTER_OFFLINE_SEC) {
      autoSaveTriggered = true;
      console.log("[Auto-save] Sesi disimpan karena ESP32 offline > 60 detik");
      saveSession().then(() => {
        if (settings.notifSession)
          showToast(
            `Sesi "${activeDevice.name}" disimpan otomatis (ESP32 offline)`,
            "success"
          );
      });
    }
  }

  prevSystemOnline = systemOnline;

  // ── Device dicabut ─────────────────────────────────────
  if (prevDeviceConnected && !deviceOnline && systemOnline) {
    deviceConnectTime   = null;
    deviceConnectEnergy = 0;
    if (waitingForName) {
      closeModal();
      showToast("Device dicabut sebelum diberi nama", "error");
    } else if (isRunning && activeDevice) {
      if (settings.notifDisconnect)
        showToast(`Device "${activeDevice.name}" dicabut — sesi disimpan`, "");
      saveSession().then(() => resetMonitoring());
    }
  }
  prevDeviceConnected = deviceOnline;

  // ── Overload ───────────────────────────────────────────
  if (webOverload && !prevOverload) {
    if (settings.notifOverload)
      showToast(`⚠ OVERLOAD! ${firebasePower.toFixed(0)}W ≥ ${settings.overloadThreshold}W`, "error");
    setDeviceBadge("overload");
    setOverloadBanner(true);
  }
  if (!webOverload && prevOverload) {
    if (isRunning && deviceOnline) setDeviceBadge("connected");
    setOverloadBanner(false);
    if (isRunning && activeDevice && !sessionSaved) {
      saveSession().then(() => {
        resetMonitoring();
        showToast("Overload teratasi — sesi disimpan. Klik + untuk mulai lagi.", "");
      });
    }
  }
  prevOverload = webOverload;

  // ── Update UI ──────────────────────────────────────────
  if (!activeDevice) { clearDisplay(); setDeviceBadge("idle"); return; }
  if (!systemOnline) { setDeviceBadge("unknown"); return; }
  if (!deviceOnline) { setDeviceBadge("offline"); clearDisplay(); return; }
  if (!webOverload)  setDeviceBadge("connected");
  updateDisplay();

  // ── Update chart power over time ──────────────────────
  const t = new Date().toLocaleTimeString("id-ID", {
    hour: "2-digit", minute: "2-digit", second: "2-digit"
  });
  lineChart.data.labels.push(t);
  lineChart.data.datasets[0].data.push(firebasePower);
  if (lineChart.data.labels.length > 20) {
    lineChart.data.labels.shift();
    lineChart.data.datasets[0].data.shift();
  }
  lineChart.update();
}

function startMetersInterval() {
  if (metersInterval) clearInterval(metersInterval);
  metersInterval = setInterval(updateMeters, settings.refreshInterval || 3000);
}

// ================================================================
// INIT
// ================================================================
const savedActive = await loadActiveSession();
if (savedActive && savedActive.id) {
  activeDevice        = { id: savedActive.id, name: savedActive.name };
  startTime           = savedActive.startTime      || null;
  energyBaseline      = savedActive.energyBaseline || 0;
  lastknownEnergy     = energyBaseline;
  deviceConnectTime   = savedActive.startTime      || null;
  deviceConnectEnergy = savedActive.energyBaseline || 0;
  isRunning           = !!startTime;
  if (valDeviceName)  valDeviceName.textContent  = activeDevice.name;
  if (activeDevLabel) activeDevLabel.textContent = `Monitoring: ${activeDevice.name}`;
  if (btnStop)        btnStop.style.display      = "inline-flex";
  if (startTime) {
    clearInterval(timerInterval);
    timerInterval = setInterval(updateTimer, 1000);
  }
}

updateChartColors(lineChart, barChart, pieChart);
pieChart.options.plugins.legend.labels.color = chartTickColor();
pieChart.update("none");
await renderDeviceTabs();
await updateSessionCount();
await updateBarPie();
startMetersInterval();
