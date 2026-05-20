import {
  requireAuth, renderShell, fillUserInfo, showToast,
  startStatusWatcher, applyTheme
} from "./auth-guard.js";
import { auth, db, ref, set, get } from "./firebase-config.js";
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
// Coba localStorage dulu (instant), lalu Firebase (sumber kebenaran)
async function loadSettings() {
  try {
    const cached = JSON.parse(localStorage.getItem(`sem_settings_${uid}`));
    if (cached) { settings = { ...DEFAULTS, ...cached }; applyToUI(); }
  } catch {}
  try {
    const snap = await get(settingsRef);
    if (snap.exists()) {
      settings = { ...DEFAULTS, ...snap.val() };
      localStorage.setItem(`sem_settings_${uid}`, JSON.stringify(settings));
      applyToUI();
    }
  } catch (e) { console.warn("Gagal load settings dari Firebase:", e); }
}

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
  applyLanguage(settings.language);
  applyTheme(settings.theme);
  highlightThemeBtn(settings.theme);
  updateConvertedPreview();
}

// ================= TRANSLATIONS =================
const LANG = {
  en: {
    settingsTitle:"Settings", settingsSub:"Manage your preferences — synced across all devices",
    pricingTitle:"💰 Energy Pricing", currencyLabel:"Currency",
    tariffLabel:"Tariff per kWh", thresholdLabel:"Overload Threshold (Watt)",
    thresholdSub:"Alert when device power exceeds this value",
    savePricing:"Save Pricing & Threshold",
    accountTitle:"👤 Account", nameLabel:"Display Name", emailLabel:"Email",
    saveProfile:"Update Profile",
    appearanceTitle:"🎨 Appearance", themeLabel:"Theme",
    themeDark:"Dark", themeDarker:"Darker", themeLight:"Light",
    langLabel:"Language", saveAppearance:"Save Appearance",
    notifTitle:"🔔 Notifications & Dashboard",
    notifDevice:"Device connected notification",
    notifDeviceSub:"Show toast when device is plugged in",
    notifDisconnect:"Device disconnected notification",
    notifDisconnectSub:"Show toast when device is unplugged",
    notifSession:"Session saved notification",
    notifSessionSub:"Show toast when session is saved",
    notifOverload:"Overload notification",
    notifOverloadSub:"Show alert when power exceeds threshold",
    refreshLabel:"Dashboard refresh interval", saveNotif:"Save Preferences",
    dataTitle:"📊 Data Control",
    dataSub:"History and settings are synced to your account across all devices.",
    exportAll:"↓ Export All History CSV", deleteAll:"✕ Delete All History",
    aboutTitle:"ℹ About", aboutApp:"Application", aboutVer:"Version",
    aboutHw:"Hardware", aboutCloud:"Cloud",
  },
  id: {
    settingsTitle:"Pengaturan", settingsSub:"Kelola preferensi kamu — tersinkron di semua perangkat",
    pricingTitle:"💰 Harga Energi", currencyLabel:"Mata Uang",
    tariffLabel:"Tarif per kWh", thresholdLabel:"Batas Overload (Watt)",
    thresholdSub:"Kirim peringatan saat daya melebihi nilai ini",
    savePricing:"Simpan Tarif & Threshold",
    accountTitle:"👤 Akun", nameLabel:"Nama Tampilan", emailLabel:"Email",
    saveProfile:"Perbarui Profil",
    appearanceTitle:"🎨 Tampilan", themeLabel:"Tema",
    themeDark:"Gelap", themeDarker:"Lebih Gelap", themeLight:"Terang",
    langLabel:"Bahasa", saveAppearance:"Simpan Tampilan",
    notifTitle:"🔔 Notifikasi & Dashboard",
    notifDevice:"Notifikasi device terhubung",
    notifDeviceSub:"Tampilkan notifikasi saat device dicolok",
    notifDisconnect:"Notifikasi device dicabut",
    notifDisconnectSub:"Tampilkan notifikasi saat device dicabut",
    notifSession:"Notifikasi sesi tersimpan",
    notifSessionSub:"Tampilkan notifikasi saat sesi disimpan",
    notifOverload:"Notifikasi overload",
    notifOverloadSub:"Tampilkan peringatan saat daya melebihi batas",
    refreshLabel:"Interval refresh dashboard", saveNotif:"Simpan Preferensi",
    dataTitle:"📊 Kontrol Data",
    dataSub:"Riwayat dan pengaturan tersinkron ke akun kamu di semua perangkat.",
    exportAll:"↓ Ekspor Semua Riwayat CSV", deleteAll:"✕ Hapus Semua Riwayat",
    aboutTitle:"ℹ Tentang", aboutApp:"Aplikasi", aboutVer:"Versi",
    aboutHw:"Perangkat Keras", aboutCloud:"Cloud",
  }
};

function applyLanguage(lang) {
  const t = LANG[lang] || LANG.en;
  const s = (id, val) => { const el = document.getElementById(id); if (el) el.textContent = val; };
  s("txt-settings-title",       t.settingsTitle);
  s("txt-settings-sub",         t.settingsSub);
  s("txt-pricing-title",        t.pricingTitle);
  s("txt-currency-label",       t.currencyLabel);
  s("txt-tariff-label",         t.tariffLabel);
  s("txt-threshold-label",      t.thresholdLabel);
  s("txt-threshold-sub",        t.thresholdSub);
  s("txt-save-pricing",         t.savePricing);
  s("txt-account-title",        t.accountTitle);
  s("txt-name-label",           t.nameLabel);
  s("txt-email-label",          t.emailLabel);
  s("txt-save-profile",         t.saveProfile);
  s("txt-appearance-title",     t.appearanceTitle);
  s("txt-theme-label",          t.themeLabel);
  s("txt-theme-dark",           t.themeDark);
  s("txt-theme-darker",         t.themeDarker);
  s("txt-theme-light",          t.themeLight);
  s("txt-lang-label",           t.langLabel);
  s("txt-save-appearance",      t.saveAppearance);
  s("txt-notif-title",          t.notifTitle);
  s("txt-notif-device",         t.notifDevice);
  s("txt-notif-device-sub",     t.notifDeviceSub);
  s("txt-notif-disconnect",     t.notifDisconnect);
  s("txt-notif-disconnect-sub", t.notifDisconnectSub);
  s("txt-notif-session",        t.notifSession);
  s("txt-notif-session-sub",    t.notifSessionSub);
  s("txt-notif-overload",       t.notifOverload);
  s("txt-notif-overload-sub",   t.notifOverloadSub);
  s("txt-refresh-label",        t.refreshLabel);
  s("txt-save-notif",           t.saveNotif);
  s("txt-data-title",           t.dataTitle);
  s("txt-data-sub",             t.dataSub);
  s("txt-export-all",           t.exportAll);
  s("txt-delete-all",           t.deleteAll);
  s("txt-about-title",          t.aboutTitle);
  s("txt-about-app",            t.aboutApp);
  s("txt-about-ver",            t.aboutVer);
  s("txt-about-hw",             t.aboutHw);
  s("txt-about-cloud",          t.aboutCloud);
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
  localStorage.setItem(`sem_theme_${uid}`, theme);
  applyLanguage(language);
  applyTheme(theme);
  showToast("Appearance saved ✓", "success");
});
document.querySelectorAll(".theme-btn").forEach(btn => {
  btn.addEventListener("click", () => {
    highlightThemeBtn(btn.dataset.theme);
    applyTheme(btn.dataset.theme);
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

// ================= INIT =================
await loadSettings();