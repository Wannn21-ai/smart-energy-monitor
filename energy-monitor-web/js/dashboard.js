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

// ================= SETTINGS =================
const SETTING_DEFAULTS = {
  currency: "IDR", tariff: 1444.70, overloadThreshold: 2000,
  notifDevice: true, notifDisconnect: true, notifSession: true,
  notifOverload: true, refreshInterval: 3000, theme: "dark", language: "en"
};
let settings = { ...SETTING_DEFAULTS };

// loadAndApplySettings dari auth-guard sudah handle theme + language + Firebase sync
settings = await loadAndApplySettings(uid);

// Re-sync settings setiap 10 detik (settings bisa berubah dari device lain)
setInterval(async () => {
  try {
    const snap = await get(settingsRef);
    if (snap.exists()) {
      const remote = { ...SETTING_DEFAULTS, ...snap.val() };
      const changed = JSON.stringify(remote) !== JSON.stringify(settings);
      if (changed) {
        settings = remote;
        localStorage.setItem(`sem_settings_${uid}`, JSON.stringify(settings));
        applyTheme(settings.theme);
        startMetersInterval(); // restart jika refreshInterval berubah
      }
    }
  } catch {}
}, 10000);

// ================= STATE =================
let voltage = 0, current = 0, firebasePower = 0, firebaseTimestamp = 0;
let firebasePF = 0, firebaseFreq = 0, firebaseApparent = 0;
let firebaseEnergy = 0, firebaseCost = 0;
let firebaseOverload = false;
let systemInternet = false, systemOnline = false, deviceOnline = false;
let prevDeviceConnected = false, prevOverload = false;
let isRunning = false, startTime = null, timerInterval = null;
let sessionSaved = false;
let activeDevice = null, waitingForName = false, metersInterval = null;
let energyBaseline = 0;
let sessionCount = 0;
let lastknownEnergy = 0;
let prevSystemOnline = false;

// ================= STORAGE (Firebase) =================
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
  try { await push(historyRef, entry); } catch (e) { console.error("pushHistory:", e); }
}

async function getSessionCount() {
  try {
    const snap = await get(historyRef);
    return snap.exists() ? Object.keys(snap.val()).length : 0;
  } catch { return 0; }
}

// Active session disimpan di Firebase supaya persist antar device/tab
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
    backgroundColor: "rgba(0,229,255,0.6)", borderColor: "#00e5ff", borderWidth: 1, borderRadius: 4 }] },
  options: makeChartOpts()
});
const pieChart = new Chart(document.getElementById("chart-pie"), {
  type: "doughnut",
  data: { labels: [], datasets: [{ data: [],
    backgroundColor: ["#00e5ff","#00e676","#ffab00","#ff1744","#7c4dff","#ff6d00"], borderWidth: 0 }] },
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

function getSessionEnergy() { return Math.max(0, lastknownEnergy - energyBaseline); }
function getSessionCost()   { return getSessionEnergy() * settings.tariff; }

function generateUniqueName(base, usedNames) {
  if (!usedNames.includes(base)) return base;
  let c = 2;
  while (usedNames.includes(`${base} ${c}`)) c++;
  return `${base} ${c}`;
}
function setGauge(el, val, min, max) {
  el.style.strokeDashoffset = 232 - Math.max(0, Math.min(1, (val - min) / (max - min))) * 232;
}
function clearDisplay() {
  valVoltage.textContent = "0"; valCurrent.textContent = "0.00";
  valPower.textContent   = "0"; valEnergy.textContent  = "0.000";
  valCost.textContent    = formatCost(0);
  setGauge(gaugeVoltage, 0, 190, 240);
  setGauge(gaugeCurrent, 0, 0, 16);
}
function updateDisplay() {
  const sessEnergy = getSessionEnergy();
  const sessCost   = getSessionCost();
  valVoltage.textContent = voltage.toFixed(1);
  valCurrent.textContent = current.toFixed(2);
  valPower.textContent   = firebasePower.toFixed(0);
  valEnergy.textContent  = sessEnergy.toFixed(3);
  valCost.textContent    = formatCost(sessCost);
  setGauge(gaugeVoltage, voltage, 190, 240);
  setGauge(gaugeCurrent, current, 0, 16);
  const elPF   = document.getElementById("val-pf");
  const elFreq = document.getElementById("val-freq");
  const elApp  = document.getElementById("val-apparent");
  if (elPF)   elPF.textContent   = firebasePF.toFixed(2);
  if (elFreq) elFreq.textContent = firebaseFreq.toFixed(1);
  if (elApp)  elApp.textContent  = firebaseApparent.toFixed(0);
}
function updateTimer() {
  if (!startTime) return;
  // startTime adalah kapan device PERTAMA terdeteksi (bukan kapan user input nama)
  const d = Date.now() - startTime;
  const h = String(Math.floor(d / 3600000)).padStart(2, "0");
  const m = String(Math.floor((d % 3600000) / 60000)).padStart(2, "0");
  const s = String(Math.floor((d % 60000) / 1000)).padStart(2, "0");
  subDuration.textContent = `Duration: ${h}:${m}:${s}`;
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
  const map = {
    connected: ["badge online",  "● Connected"],
    overload:  ["badge offline", "⚠ Overload!"],
    idle:      ["badge idle",    "● Idle"],
    offline:   ["badge offline", "● Offline"],
    unknown:   ["badge unknown", "● Unknown"]
  };
  const [cls, txt] = map[state] || map.unknown;
  badgeStatus.className = cls; badgeStatus.textContent = txt;
}
async function updateSessionCount() {
  sessionCount = await getSessionCount();
  valSessionCount.textContent = sessionCount;
}

// ================= OVERLOAD BANNER =================
function setOverloadBanner(active) {
  if (!overloadBanner) return;
  overloadBanner.style.display = active ? "flex" : "none";
  if (active) overloadBanner.textContent = `⚠ OVERLOAD DETECTED — Power exceeds ${settings.overloadThreshold}W threshold!`;
}

// ================= DEVICE TABS =================
async function renderDeviceTabs() {
  deviceTabsEl.innerHTML = "";
  const history = await getHistory();
  const names = [...new Set(history.map(h => h.name))];
  names.forEach(name => {
    const btn = document.createElement("button");
    btn.className = `device-tab${activeDevice?.name === name ? " active" : ""}`;
    btn.textContent = name;
    deviceTabsEl.appendChild(btn);
  });
}

// ================= SAVE SESSION =================
async function saveSession() {
  const sessEnergy = getSessionEnergy();
  const sessCost   = getSessionCost();
  if (sessionSaved || !activeDevice || sessEnergy <= 0) {
    if (!sessionSaved && activeDevice && sessEnergy <= 0) {
      console.warn("[SEM] saveSession: sessEnergy=0, lastknownEnergy=", lastknownEnergy, "baseline=", energyBaseline);
    }
    return;
  }
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
  if (settings.notifSession) showToast(`Sesi "${activeDevice.name}" tersimpan ✓`, "success");
}

// ================= RESET =================
async function resetMonitoring() {
  clearInterval(timerInterval);
  timerInterval = null; startTime = null; isRunning = false;
  sessionSaved = false; energyBaseline = 0; activeDevice = null;

  lastknownEnergy = 0;
  await saveActiveSession(null);
  voltage = current = firebasePower = 0;
  clearDisplay();
  subDuration.textContent    = "Duration: 00:00:00";
  valDeviceName.textContent  = "—";
  activeDevLabel.textContent = "No active device";
  btnStop.style.display      = "none";
  setDeviceBadge("idle");
  setOverloadBanner(false);
  renderDeviceTabs();
}

// ================================================================
// START MONITORING
// BUG FIX #6: startTime & energyBaseline diset ke SAAT DEVICE
// PERTAMA TERDETEKSI (deviceConnectTime), bukan saat user input nama.
// Jadi kalau user telat 1 menit, waktu + energi tetap akurat.
// ================================================================
let deviceConnectTime   = null; // kapan device pertama terdeteksi
let deviceConnectEnergy = 0;    // energyBaseline saat device pertama terdeteksi

async function startMonitoring(name) {
  activeDevice = { id: `dev_${Date.now()}`, name };

  // Gunakan waktu & energi saat device PERTAMA TERDETEKSI
  // (bukan saat user submit nama)
  startTime      = deviceConnectTime   || Date.now();
  energyBaseline = deviceConnectEnergy !== undefined ? deviceConnectEnergy : firebaseEnergy;
  lastknownEnergy = energyBaseline;
  isRunning = true; sessionSaved = false;
  await saveActiveSession({ ...activeDevice, startTime, energyBaseline });
  valDeviceName.textContent  = name;
  activeDevLabel.textContent = `Monitoring: ${name}`;
  btnStop.style.display      = "inline-flex";
  setDeviceBadge("connected");
  clearInterval(timerInterval);
  timerInterval = setInterval(updateTimer, 1000);
  renderDeviceTabs();

  // Hitung berapa lama device sudah terhubung sebelum user input nama
  const retroMs = Date.now() - startTime;
  const retroMin = Math.round(retroMs / 60000);
  if (retroMin >= 1) {
    showToast(`Monitoring "${name}" dimulai ▶ (sudah ${retroMin} mnt terhitung)`, "success");
  } else {
    showToast(`Monitoring "${name}" dimulai ▶`, "success");
  }
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

// ================= MODAL =================
// Timer untuk notifikasi "sudah X menit menunggu nama"
let namingReminderTimeout = null;

async function openModalAuto() {
  waitingForName = true;
  const history   = await getHistory();
  const usedNames = history.map(h => h.name);
  const modalTitleEl = document.querySelector("#modal-add-device .modal-title");
  const modalSubEl   = document.querySelector("#modal-add-device .modal-sub");
  if (modalTitleEl) modalTitleEl.textContent = "⚡ Device Terdeteksi!";
  if (modalSubEl)   modalSubEl.textContent   = "Berikan nama untuk device yang baru terhubung.";
  inputDevName.value     = "";
  inputDevName.dataset.usedNames = JSON.stringify(usedNames);
  modalAdd.classList.add("open");
  setTimeout(() => inputDevName.focus(), 100);

  // Reminder setelah 30 detik jika user belum input nama
  clearTimeout(namingReminderTimeout);
  namingReminderTimeout = setTimeout(() => {
    if (waitingForName) {
      const elapsed = deviceConnectTime ? Math.round((Date.now() - deviceConnectTime) / 1000) : "?";
      showToast(`⚠ Device sudah ${elapsed}s terhubung. Segera beri nama!`, "error");
    }
  }, 30000);
}

async function openModalManual() {
  if (!deviceOnline) { showToast("Tidak ada device yang terhubung", "error"); return; }
  if (isRunning && activeDevice) { showToast(`"${activeDevice.name}" sedang dimonitor`, "error"); return; }
  waitingForName = false;
  const history   = await getHistory();
  const usedNames = history.map(h => h.name);
  const modalTitleEl = document.querySelector("#modal-add-device .modal-title");
  const modalSubEl   = document.querySelector("#modal-add-device .modal-sub");
  if (modalTitleEl) modalTitleEl.textContent = "Tambah Device";
  if (modalSubEl)   modalSubEl.textContent   = "Berikan nama untuk device yang terhubung.";
  inputDevName.value     = "";
  inputDevName.dataset.usedNames = JSON.stringify(usedNames);
  modalAdd.classList.add("open");
  setTimeout(() => inputDevName.focus(), 100);
}

function closeModal() {
  modalAdd.classList.remove("open");
  inputDevName.value = "";
  waitingForName = false;
  clearTimeout(namingReminderTimeout);
}

fab.addEventListener("click", openModalManual);
btnCancelDev.addEventListener("click", closeModal);
modalAdd.addEventListener("click", e => { if (e.target === modalAdd && !waitingForName) closeModal(); });
btnSaveDev.addEventListener("click", async () => {
  const usedNames = JSON.parse(inputDevName.dataset.usedNames || "[]");
  let name = inputDevName.value.trim();
  if (!name) name = generateUniqueName("Device", usedNames);
  else name = generateUniqueName(name, usedNames);
  if (name.length > 24) { showToast("Maksimal 24 karakter", "error"); return; }
  closeModal();
  await startMonitoring(name);
});
inputDevName.addEventListener("keydown", e => { if (e.key === "Enter") btnSaveDev.click(); });

// ================= STOP =================
btnStop.addEventListener("click", async () => {
  if (!isRunning || !activeDevice) { showToast("Tidak ada sesi yang berjalan", "error"); return; }
  await saveSession();
  await resetMonitoring();
});

// ================= FIREBASE LIVE LISTENER =================
onValue(ref(db, "live"), snapshot => {
  const data = snapshot.val();
  if (!data) return;
  const sys = data.system || {};
  systemInternet    = sys.internet === true;
  firebaseTimestamp = sys.timestamp || 0;
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

// ================= MAIN LOOP =================
function updateMeters() {
  const now  = Math.floor(Date.now() / 1000);
  const diff = now - firebaseTimestamp;
  systemOnline = systemInternet && firebaseTimestamp > 0 && diff <= 120;
  deviceOnline = systemOnline && current > 0.01 && firebasePower > 0.5;

  // Cek overload dari sisi web menggunakan settings.overloadThreshold
  const webOverload = deviceOnline && firebasePower > 0 && firebasePower >= settings.overloadThreshold;

  setSystemStatus(systemOnline);
  subWebStatus.textContent = `Web: ${systemOnline ? "online" : "offline"}`;
  subTariff.textContent    = `Tariff: ${symbol()} ${settings.tariff.toLocaleString("id-ID")}/kWh`;

  // ── Device baru connect ──
  if (!prevDeviceConnected && deviceOnline) {
    // FIX #6: Simpan waktu & energi saat device PERTAMA terdeteksi
    deviceConnectTime   = Date.now();
    deviceConnectEnergy = firebaseEnergy;
    lastknownEnergy    = firebaseEnergy;

    if (!isRunning && !waitingForName) {
      if (settings.notifDevice) showToast("⚡ Device baru terdeteksi! Silakan beri nama.", "success");
      openModalAuto();
    }
  }

  // ── Device disconnect ──
// ── System tiba-tiba offline (ESP32 dimatikan tanpa cabut device) ──
if (prevSystemOnline && !systemOnline && isRunning && activeDevice) {
  // Simpan sesi sebelum reset — ini yang hilang sebelumnya
  if (settings.notifDisconnect)
    showToast(`⚠ Sistem offline — sesi "${activeDevice.name}" disimpan`, "error");
  saveSession().then(() => resetMonitoring());
}
prevSystemOnline = systemOnline;

// ── Device disconnect (sistem masih online, device dicabut) ──
if (prevDeviceConnected && !deviceOnline && systemOnline) {
  deviceConnectTime   = null;
  deviceConnectEnergy = 0;
  if (waitingForName) {
    closeModal(); showToast("Device dicabut sebelum diberi nama", "error");
  } else if (isRunning && activeDevice) {
    if (settings.notifDisconnect)
      showToast(`Device "${activeDevice.name}" dicabut — sesi dihentikan`, "error");
    saveSession().then(() => resetMonitoring());
  }
}

  // ── Overload ──
  if (webOverload && !prevOverload) {
    if (settings.notifOverload)
      showToast(`⚠ OVERLOAD! Daya melebihi ${settings.overloadThreshold}W`, "error");
    setDeviceBadge("overload");
    setOverloadBanner(true);
  }
  if (!webOverload && prevOverload) {
    if (isRunning && deviceOnline) setDeviceBadge("connected");
    setOverloadBanner(false);
    showToast("✓ Overload teratasi", "success");
  }
  prevOverload = webOverload;

  if (!activeDevice) { clearDisplay(); setDeviceBadge("idle"); return; }
  if (!systemOnline) { setDeviceBadge("unknown"); clearDisplay(); return; }
  if (!deviceOnline) { setDeviceBadge("offline"); clearDisplay(); return; }

  if (!webOverload) setDeviceBadge("connected");
  updateDisplay();

  const t = new Date().toLocaleTimeString("id-ID", { hour: "2-digit", minute: "2-digit", second: "2-digit" });
  lineChart.data.labels.push(t);
  lineChart.data.datasets[0].data.push(firebasePower);
  if (lineChart.data.labels.length > 20) {
    lineChart.data.labels.shift(); lineChart.data.datasets[0].data.shift();
  }
  lineChart.update();
}

function startMetersInterval() {
  if (metersInterval) clearInterval(metersInterval);
  metersInterval = setInterval(updateMeters, settings.refreshInterval || 3000);
}

// ================= INIT =================
// Restore active session dari Firebase
const savedActive = await loadActiveSession();
if (savedActive && savedActive.id) {
  activeDevice        = { id: savedActive.id, name: savedActive.name };
  startTime           = savedActive.startTime      || null;
  energyBaseline      = savedActive.energyBaseline || 0;
  lastknownEnergy     = energyBaseline;
  deviceConnectTime   = savedActive.startTime      || null;
  deviceConnectEnergy = savedActive.energyBaseline || 0;
  isRunning           = !!startTime;
  valDeviceName.textContent  = activeDevice.name;
  activeDevLabel.textContent = `Monitoring: ${activeDevice.name}`;
  btnStop.style.display      = "inline-flex";
  if (startTime) { clearInterval(timerInterval); timerInterval = setInterval(updateTimer, 1000); }
}

updateChartColors(lineChart, barChart, pieChart);
pieChart.options.plugins.legend.labels.color = chartTickColor();
pieChart.update("none");
await renderDeviceTabs();
await updateSessionCount();
await updateBarPie();
startMetersInterval();