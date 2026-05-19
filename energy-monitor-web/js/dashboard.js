import {
  requireAuth, renderShell, fillUserInfo,
  setSystemStatus, showToast, applyTheme,
  updateChartColors,
  startStatusWatcher   // FIX poin 5: tambahkan agar konsisten dengan halaman lain
} from "./auth-guard.js";
import { db, ref, onValue } from "./firebase-config.js";

// ================= INIT =================
const user = await requireAuth();
renderShell("dashboard", "DASHBOARD");
fillUserInfo(user);
startStatusWatcher();   // FIX poin 5: mulai watcher status dot topbar
const uid = user.uid;

// ================= SETTINGS =================
const SETTING_DEFAULTS = {
  currency: "IDR", tariff: 1444.70,
  notifDevice: true, notifDisconnect: true, notifSession: true,
  refreshInterval: 3000
};
let settings = { ...SETTING_DEFAULTS };
try {
  const saved = JSON.parse(localStorage.getItem(`sem_settings_${uid}`));
  if (saved) settings = { ...SETTING_DEFAULTS, ...saved };
} catch {}

const savedTheme = localStorage.getItem(`sem_theme_${uid}`);
if (savedTheme) applyTheme(savedTheme);

// ================= STATE =================
let voltage = 0, current = 0;
let firebasePower = 0, firebaseEnergy = 0, firebaseCost = 0, firebaseTimestamp = 0;
let systemInternet = false, systemOnline = false, deviceOnline = false;
let prevDeviceConnected = false;
let isRunning = false, startTime = null, timerInterval = null;
let sessionSaved = false, energy = 0;
let activeDevice = null;
let waitingForName = false;
let metersInterval = null;

// ================= STORAGE =================
const storageKey  = key => `sem_${key}_${uid}`;
function getStorage(key) {
  try { return JSON.parse(localStorage.getItem(storageKey(key))) || (key === "history" || key === "devices" ? [] : null); }
  catch { return key === "history" || key === "devices" ? [] : null; }
}
function setStorage(key, val) { localStorage.setItem(storageKey(key), JSON.stringify(val)); }
const getHistory     = () => getStorage("history");
const saveHistory    = d  => setStorage("history", d);
const getDevices     = () => getStorage("devices");
const saveDevices    = d  => setStorage("devices", d);
const getActiveDevice = () => getStorage("active");
const saveActiveDevice = d => setStorage("active", d);

// ================= ELEMENTS =================
const valVoltage     = document.getElementById("val-voltage");
const valCurrent     = document.getElementById("val-current");
const valPower       = document.getElementById("val-power");
const valEnergy      = document.getElementById("val-energy");
const valCost        = document.getElementById("val-cost");
const valDeviceName  = document.getElementById("val-device-name");
const subDuration    = document.getElementById("sub-duration");
const subTariff      = document.getElementById("sub-tariff");
const subWebStatus   = document.getElementById("sub-web-status");
const valSessionCount = document.getElementById("val-session-count");
const activeDevLabel = document.getElementById("active-device-label");
const badgeStatus    = document.getElementById("badge-device-status");
const deviceTabsEl   = document.getElementById("device-tabs");
const btnStop        = document.getElementById("btn-stop");
const fab            = document.getElementById("fab-add");
const modalAdd       = document.getElementById("modal-add-device");
const modalTitle     = document.querySelector("#modal-add-device .modal-title");
const modalSub       = document.querySelector("#modal-add-device .modal-sub");
const inputDevName   = document.getElementById("input-device-name");
const btnCancelDev   = document.getElementById("btn-cancel-device");
const btnSaveDev     = document.getElementById("btn-save-device");
const gaugeVoltage   = document.getElementById("gauge-voltage");
const gaugeCurrent   = document.getElementById("gauge-current");

// ================= CHART OPTIONS =================
function chartTickColor() {
  return getComputedStyle(document.documentElement).getPropertyValue("--chart-tick").trim() || "#666";
}
function chartGridColor() {
  return getComputedStyle(document.documentElement).getPropertyValue("--chart-grid").trim() || "rgba(255,255,255,0.04)";
}
function makeChartOpts(extraScales = {}) {
  return {
    responsive: true, maintainAspectRatio: false,
    plugins: { legend: { display: false } },
    scales: {
      x: { ticks: { color: chartTickColor(), font: { size: 10 } }, grid: { color: chartGridColor() }, ...extraScales.x },
      y: { ticks: { color: chartTickColor(), font: { size: 10 } }, grid: { color: chartGridColor() }, ...extraScales.y }
    }
  };
}

const lineChart = new Chart(document.getElementById("chart-line"), {
  type: "line",
  data: {
    labels: [],
    datasets: [{ label: "Power (W)", data: [], borderColor: "#ffab00", backgroundColor: "rgba(255,171,0,0.08)",
      tension: 0.4, fill: true, pointRadius: 3, pointBackgroundColor: "#ffab00" }]
  },
  options: makeChartOpts()
});
const barChart = new Chart(document.getElementById("chart-bar"), {
  type: "bar",
  data: {
    labels: [],
    datasets: [{ label: "Avg Power (W)", data: [], backgroundColor: "rgba(0,229,255,0.6)",
      borderColor: "#00e5ff", borderWidth: 1, borderRadius: 4 }]
  },
  options: makeChartOpts()
});
const pieChart = new Chart(document.getElementById("chart-pie"), {
  type: "doughnut",
  data: {
    labels: [],
    datasets: [{ data: [], backgroundColor: ["#00e5ff","#00e676","#ffab00","#ff1744","#7c4dff","#ff6d00"], borderWidth: 0 }]
  },
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

function generateUniqueName(base) {
  const used = getHistory().map(i => i.name);
  if (!used.includes(base)) return base;
  let c = 2;
  while (used.includes(`${base} ${c}`)) c++;
  return `${base} ${c}`;
}
function setGauge(el, val, min, max) {
  el.style.strokeDashoffset = 232 - Math.max(0, Math.min(1, (val - min) / (max - min))) * 232;
}
function clearDisplay() {
  valVoltage.textContent = "0"; valCurrent.textContent = "0.00";
  valPower.textContent = "0";   valEnergy.textContent = "0.000";
  valCost.textContent = formatCost(0);
  setGauge(gaugeVoltage, 0, 190, 240); setGauge(gaugeCurrent, 0, 0, 16);
}
function updateDisplay() {
  valVoltage.textContent = voltage.toFixed(1);
  valCurrent.textContent = current.toFixed(2);
  valPower.textContent   = firebasePower.toFixed(0);
  valEnergy.textContent  = firebaseEnergy.toFixed(3);
  valCost.textContent    = formatCost(firebaseCost);
  setGauge(gaugeVoltage, voltage, 190, 240);
  setGauge(gaugeCurrent, current, 0, 16);
}
function updateTimer() {
  if (!startTime) return;
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
    connected: ["badge online", "● Connected"],
    idle:      ["badge idle",    "● Idle"],
    offline:   ["badge offline", "● Offline"],
    unknown:   ["badge unknown", "● Unknown"]
  };
  const [cls, txt] = map[state] || map.unknown;
  badgeStatus.className   = cls;
  badgeStatus.textContent = txt;
}
function updateSessionCount() { valSessionCount.textContent = getHistory().length; }

// ================= DEVICE TABS =================
function renderDeviceTabs() {
  deviceTabsEl.innerHTML = "";
  getDevices().forEach(dev => {
    const btn = document.createElement("button");
    btn.className = `device-tab${activeDevice?.id === dev.id ? " active" : ""}`;
    btn.textContent = dev.name;
    deviceTabsEl.appendChild(btn);
  });
}

// ================= SAVE SESSION =================
function saveSession() {
  if (sessionSaved || !activeDevice || firebaseEnergy <= 0) return;
  const history = getHistory();
  history.unshift({
    id: Date.now(), name: activeDevice.name, duration: getDuration(),
    power: parseFloat(firebasePower.toFixed(1)),
    energy: parseFloat(firebaseEnergy.toFixed(3)),
    cost: formatCost(firebaseCost), costRaw: firebaseCost,
    date: new Date().toLocaleDateString("id-ID"), timestamp: Date.now()
  });
  saveHistory(history);
  const devices = getDevices();
  if (!devices.find(d => d.id === activeDevice.id)) {
    devices.push({ id: activeDevice.id, name: activeDevice.name });
    saveDevices(devices);
  }
  updateSessionCount();
  updateBarPie();
  sessionSaved = true;
  if (settings.notifSession) showToast(`Sesi "${activeDevice.name}" tersimpan ✓`, "success");
}

// ================= RESET =================
function resetMonitoring() {
  clearInterval(timerInterval); timerInterval = null;
  startTime = null; isRunning = false; sessionSaved = false;
  energy = 0; activeDevice = null; saveActiveDevice(null);
  voltage = current = firebasePower = firebaseEnergy = firebaseCost = 0;
  clearDisplay();
  subDuration.textContent = "Duration: 00:00:00";
  valDeviceName.textContent  = "—";
  activeDevLabel.textContent = "No active device";
  btnStop.style.display = "none";
  setDeviceBadge("idle");
  renderDeviceTabs();
}

// ================= START =================
function startMonitoring(name) {
  activeDevice = { id: `dev_${Date.now()}`, name };
  saveActiveDevice(activeDevice);
  startTime  = Date.now();
  isRunning  = true;
  sessionSaved = false;
  valDeviceName.textContent  = name;
  activeDevLabel.textContent = `Monitoring: ${name}`;
  btnStop.style.display = "inline-flex";
  setDeviceBadge("connected");
  clearInterval(timerInterval);
  timerInterval = setInterval(updateTimer, 1000);
  renderDeviceTabs();
  showToast(`Monitoring "${name}" dimulai ▶`, "success");
}

// ================= CHART UPDATE =================
function updateBarPie() {
  const history  = getHistory();
  const byDevice = {};
  history.forEach(s => {
    if (!byDevice[s.name]) byDevice[s.name] = { power: 0, energy: 0, count: 0 };
    byDevice[s.name].power  += s.power;
    byDevice[s.name].energy += s.energy;
    byDevice[s.name].count  += 1;
  });
  const names    = Object.keys(byDevice);
  const powers   = names.map(n => parseFloat((byDevice[n].power / byDevice[n].count).toFixed(1)));
  const energies = names.map(n => parseFloat(byDevice[n].energy.toFixed(3)));
  barChart.data.labels = names; barChart.data.datasets[0].data = powers; barChart.update();
  pieChart.data.labels = names; pieChart.data.datasets[0].data = energies; pieChart.update();
}

// ================= MODAL =================
function openModalAuto() {
  waitingForName = true;
  modalTitle.textContent = "⚡ Device Terdeteksi!";
  modalSub.textContent   = "Berikan nama untuk device yang baru terhubung sebelum monitoring dimulai.";
  inputDevName.value     = "";
  modalAdd.classList.add("open");
  setTimeout(() => inputDevName.focus(), 100);
}
function openModalManual() {
  if (!deviceOnline) { showToast("Tidak ada device yang terhubung ke sistem", "error"); return; }
  if (isRunning && activeDevice) { showToast(`"${activeDevice.name}" sedang dimonitor`, "error"); return; }
  waitingForName = false;
  modalTitle.textContent = "Tambah Device";
  modalSub.textContent   = "Berikan nama untuk device yang terhubung.";
  inputDevName.value     = "";
  modalAdd.classList.add("open");
  setTimeout(() => inputDevName.focus(), 100);
}
function closeModal() {
  modalAdd.classList.remove("open");
  inputDevName.value = "";
  waitingForName = false;
}

fab.addEventListener("click", openModalManual);
btnCancelDev.addEventListener("click", closeModal);
modalAdd.addEventListener("click", e => { if (e.target === modalAdd && !waitingForName) closeModal(); });
btnSaveDev.addEventListener("click", () => {
  let name = inputDevName.value.trim();
  if (!name) name = generateUniqueName("Device");
  else name = generateUniqueName(name);
  if (name.length > 24) { showToast("Maksimal 24 karakter", "error"); return; }
  closeModal();
  startMonitoring(name);
});
inputDevName.addEventListener("keydown", e => { if (e.key === "Enter") btnSaveDev.click(); });

// ================= STOP =================
btnStop.addEventListener("click", () => {
  if (!isRunning || !activeDevice) { showToast("Tidak ada sesi yang berjalan", "error"); return; }
  saveSession();
  resetMonitoring();
});

// ================= FIREBASE =================
onValue(ref(db, "live"), snapshot => {
  const data = snapshot.val();
  if (!data) return;
  const sys = data.system || {};
  systemInternet   = sys.internet === true;
  firebaseTimestamp = sys.timestamp || 0;
  const dev = data.device || {};
  voltage       = dev.voltage || 0;
  current       = dev.current || 0;
  firebasePower  = dev.power  || 0;
  firebaseEnergy = dev.energy || 0;
  firebaseCost   = dev.cost   || 0;
});

// ================= MAIN LOOP =================
function updateMeters() {
  const now  = Math.floor(Date.now() / 1000);
  const diff = now - firebaseTimestamp;
  systemOnline = systemInternet && firebaseTimestamp > 0 && diff <= 120;
  deviceOnline = systemOnline && (current > 0.01 && firebasePower > 0.5);

  // FIX poin 5: setSystemStatus masih dipanggil di sini untuk update realtime
  // (startStatusWatcher() di atas hanya untuk dot topbar via Firebase listener,
  //  updateMeters menimpa dengan data yang lebih presisi dari loop utama)
  setSystemStatus(systemOnline);
  subWebStatus.textContent = `Web: ${systemOnline ? "online" : "offline"}`;
  subTariff.textContent    = `Tariff: ${symbol()} ${settings.tariff.toLocaleString("id-ID")}/kWh`;

  if (!prevDeviceConnected && deviceOnline) {
    if (!isRunning && !waitingForName) {
      if (settings.notifDevice) showToast("⚡ Device baru terdeteksi! Silakan beri nama device.", "success");
      openModalAuto();
    }
  }
  if (prevDeviceConnected && !deviceOnline && systemOnline) {
    if (waitingForName) {
      closeModal();
      showToast("Device dicabut sebelum diberi nama", "error");
    } else if (isRunning && activeDevice) {
      if (settings.notifDisconnect) showToast(`Device "${activeDevice.name}" dicabut — sesi dihentikan otomatis`, "error");
      saveSession();
      resetMonitoring();
    }
  }
  prevDeviceConnected = deviceOnline;

  if (!activeDevice) { clearDisplay(); setDeviceBadge("idle"); return; }
  if (!systemOnline)  { setDeviceBadge("unknown"); clearDisplay(); return; }
  if (!deviceOnline)  { setDeviceBadge("offline"); clearDisplay(); return; }

  setDeviceBadge("connected");
  updateDisplay();
  energy = firebaseEnergy;

  const t = new Date().toLocaleTimeString("id-ID", { hour: "2-digit", minute: "2-digit", second: "2-digit" });
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

setInterval(() => {
  try {
    const saved = JSON.parse(localStorage.getItem(`sem_settings_${uid}`));
    if (saved && saved.refreshInterval && saved.refreshInterval !== settings.refreshInterval) {
      settings = { ...SETTING_DEFAULTS, ...saved };
      startMetersInterval();
    }
  } catch {}
}, 5000);

// ================= INIT =================
const savedActive = getActiveDevice();
if (savedActive) {
  activeDevice = savedActive;
  startTime    = null;
  isRunning    = false;
  valDeviceName.textContent  = activeDevice.name;
  activeDevLabel.textContent = `Monitoring: ${activeDevice.name}`;
  btnStop.style.display = "inline-flex";
}

updateChartColors(lineChart, barChart, pieChart);
pieChart.options.plugins.legend.labels.color = chartTickColor();
pieChart.update("none");
renderDeviceTabs();
updateSessionCount();
updateBarPie();
startMetersInterval();