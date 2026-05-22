import {
  requireAuth, renderShell, fillUserInfo, showToast,
  startStatusWatcher, applyTheme, applyLanguage,
  loadAndApplySettings
} from "./auth-guard.js";
import { auth, db, ref, set, get, update } from "./firebase-config.js";
import { updateProfile } from "https://www.gstatic.com/firebasejs/10.12.2/firebase-auth.js";

const user = await requireAuth();
renderShell("settings", "SETTINGS");
fillUserInfo(user);
startStatusWatcher();
const uid = user.uid;

// ================= FIREBASE PATHS =================
const settingsRef = ref(db, `users/${uid}/settings`);
const historyRef  = ref(db, `users/${uid}/history`);

// ================= DEFAULTS =================
const DEFAULTS = {
  currency: "IDR", tariff: 1444.70,
  overloadThreshold: 2000,
  theme: "dark", language: "en",
  notifDevice: true, notifDisconnect: true,
  notifSession: true, notifOverload: true,
  refreshInterval: 3000
};
let settings = { ...DEFAULTS };

// ================= LOAD SETTINGS =================
// Gunakan loadAndApplySettings dari auth-guard (handles theme + language)
settings = await loadAndApplySettings(uid);
applyToUI();

async function saveSettingsToFirebase() {
  try {
    await set(settingsRef, settings);
    localStorage.setItem(`sem_settings_${uid}`, JSON.stringify(settings));
  } catch {
    showToast("Gagal sync ke cloud, tersimpan lokal", "");
    localStorage.setItem(`sem_settings_${uid}`, JSON.stringify(settings));
  }
}

// ================= APPLY TO UI =================
function applyToUI() {
  const el = id => document.getElementById(id);
  if (el("currency"))           el("currency").value           = settings.currency;
  if (el("tariff"))             el("tariff").value             = settings.tariff;
  if (el("overload-threshold")) el("overload-threshold").value = settings.overloadThreshold;
  if (el("language"))           el("language").value           = settings.language;
  if (el("notif-device"))       el("notif-device").checked     = settings.notifDevice;
  if (el("notif-disconnect"))   el("notif-disconnect").checked = settings.notifDisconnect;
  if (el("notif-session"))      el("notif-session").checked    = settings.notifSession;
  if (el("notif-overload"))     el("notif-overload").checked   = settings.notifOverload;
  if (el("refresh-interval"))   el("refresh-interval").value  = settings.refreshInterval;
  if (el("display-name"))       el("display-name").value      = user.displayName || "";
  if (el("display-email"))      el("display-email").value     = user.email || "";
  // applyLanguage & applyTheme sudah dipanggil oleh loadAndApplySettings
  highlightThemeBtn(settings.theme);
  updateConvertedPreview();
}

function highlightThemeBtn(theme) {
  document.querySelectorAll(".theme-btn").forEach(btn =>
    btn.classList.toggle("active", btn.dataset.theme === theme));
}

// ================= TARIFF CONVERTER =================
const USD_RATE = 16500;
function updateConvertedPreview() {
  const currency = document.getElementById("currency")?.value;
  const tariff   = parseFloat(document.getElementById("tariff")?.value);
  const el       = document.getElementById("tariff-converted");
  if (!el || isNaN(tariff) || tariff <= 0) { if (el) el.textContent = ""; return; }
  el.textContent = currency === "IDR"
    ? `≈ $ ${(tariff / USD_RATE).toFixed(4)} / kWh`
    : `≈ Rp ${Math.round(tariff * USD_RATE).toLocaleString("id-ID")} / kWh`;
}
document.getElementById("currency").addEventListener("change", function () {
  const tariffEl = document.getElementById("tariff");
  const cur = parseFloat(tariffEl.value);
  if (!isNaN(cur) && cur > 0) {
    tariffEl.value = this.value === "USD"
      ? (cur / USD_RATE).toFixed(4)
      : Math.round(cur * USD_RATE);
  }
  updateConvertedPreview();
});
document.getElementById("tariff").addEventListener("input", updateConvertedPreview);

// ================= SAVE PRICING + THRESHOLD =================
document.getElementById("btn-save-settings").addEventListener("click", async () => {
  const currency  = document.getElementById("currency").value;
  const tariff    = parseFloat(document.getElementById("tariff").value);
  const threshold = parseFloat(document.getElementById("overload-threshold").value);
  if (isNaN(tariff)    || tariff    <= 0) { showToast("Enter a valid tariff value",    "error"); return; }
  if (isNaN(threshold) || threshold <= 0) { showToast("Enter a valid threshold value", "error"); return; }
  settings = { ...settings, currency, tariff, overloadThreshold: threshold };
  await saveSettingsToFirebase();
  // Tulis threshold ke /config/threshold — dibaca ESP32 setiap 30 detik
  try {
    await set(ref(db, "config/threshold"), threshold);
    console.log("[SEM] Threshold synced:", threshold);
  } catch (e) { console.warn("[SEM] Gagal sync threshold:", e); }
  showToast("Settings saved ✓", "success");
});

// ================= SAVE PROFILE =================
document.getElementById("btn-save-profile").addEventListener("click", async () => {
  const name = document.getElementById("display-name").value.trim();
  if (!name) { showToast("Name cannot be empty", "error"); return; }
  try {
    await updateProfile(auth.currentUser, { displayName: name });
    fillUserInfo(auth.currentUser);
    showToast("Profile updated ✓", "success");
  } catch { showToast("Failed to update profile", "error"); }
});

// ================= SAVE APPEARANCE =================
document.getElementById("btn-save-appearance").addEventListener("click", async () => {
  const theme    = document.querySelector(".theme-btn.active")?.dataset.theme || "dark";
  const language = document.getElementById("language").value;
  settings = { ...settings, theme, language };
  await saveSettingsToFirebase();
  // Apply langsung + simpan ke localStorage supaya halaman lain ikut
  localStorage.setItem(`sem_theme_${uid}`, theme);
  applyTheme(theme);
  applyLanguage(language);
  showToast("Appearance saved ✓", "success");
});
document.querySelectorAll(".theme-btn").forEach(btn => {
  btn.addEventListener("click", () => {
    highlightThemeBtn(btn.dataset.theme);
    applyTheme(btn.dataset.theme); // preview langsung
  });
});

// ================= SAVE NOTIFICATIONS =================
document.getElementById("btn-save-notif").addEventListener("click", async () => {
  settings = {
    ...settings,
    notifDevice:     document.getElementById("notif-device").checked,
    notifDisconnect: document.getElementById("notif-disconnect").checked,
    notifSession:    document.getElementById("notif-session").checked,
    notifOverload:   document.getElementById("notif-overload").checked,
    refreshInterval: parseInt(document.getElementById("refresh-interval").value)
  };
  await saveSettingsToFirebase();
  showToast("Preferences saved ✓", "success");
});

// ================= EXPORT ALL =================
document.getElementById("btn-export-all").addEventListener("click", async () => {
  try {
    const snap = await get(historyRef);
    if (!snap.exists()) { showToast("No history to export", "error"); return; }
    const history = Object.values(snap.val()).sort((a, b) => b.timestamp - a.timestamp);
    let csv = "Name,Duration,Power (W),Energy (kWh),Cost,Date\n";
    history.forEach(s => { csv += `${s.name},${s.duration},${s.power},${s.energy},${s.cost},${s.date}\n`; });
    const blob = new Blob([csv], { type: "text/csv" });
    const url  = URL.createObjectURL(blob);
    const a    = document.createElement("a");
    a.href = url; a.download = "sem_all_history.csv"; a.click();
    URL.revokeObjectURL(url);
    showToast("Exported ✓", "success");
  } catch { showToast("Failed to export", "error"); }
});

// ================= DELETE ALL =================
document.getElementById("btn-delete-all").addEventListener("click", async () => {
  if (!confirm("Delete ALL history? This cannot be undone.")) return;
  try {
    await set(historyRef, null);
    showToast("All history deleted", "");
  } catch { showToast("Failed to delete", "error"); }
});