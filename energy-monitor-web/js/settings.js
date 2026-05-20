import {
  requireAuth, renderShell, fillUserInfo, showToast,
  startStatusWatcher, applyTheme          // FIX: import dari auth-guard, tidak perlu duplikasi
} from "./auth-guard.js";
import { auth } from "./firebase-config.js";
import { updateProfile } from "https://www.gstatic.com/firebasejs/10.12.2/firebase-auth.js";

const user = await requireAuth();
renderShell("settings", "SETTINGS");
fillUserInfo(user);
startStatusWatcher();
const uid = user.uid;

// ================= HELPERS =================
function getHistory() {
  try { return JSON.parse(localStorage.getItem(`sem_history_${uid}`)) || []; }
  catch { return []; }
}
function downloadCSV(content, filename) {
  const blob = new Blob([content], { type: "text/csv" });
  const url  = URL.createObjectURL(blob);
  const a    = document.createElement("a");
  a.href = url; a.download = filename; a.click();
  URL.revokeObjectURL(url);
}

// ================= DEFAULTS =================
const DEFAULTS = {
  currency: "IDR", tariff: 1444.70, 
  overloadThreshold: 2000,
  theme: "dark", language: "en",
  notifDevice: true, notifDisconnect: true, notifSession: true,
  notifOverload: true,
  refreshInterval: 3000
};
let settings = { ...DEFAULTS };
try {
  const saved = JSON.parse(localStorage.getItem(`sem_settings_${uid}`));
  if (saved) settings = { ...DEFAULTS, ...saved };
} catch {}

// ================= TRANSLATIONS =================
const LANG = {
  en: {
    settingsTitle:"Settings", settingsSub:"Manage your preferences and data",
    pricingTitle:"💰 Energy Pricing", currencyLabel:"Currency",
    tariffLabel:"Tariff per kWh", thresholdLabel:"Overload Threshold (Watt)",
    thresholdSub:"Alert when device power exceeds this value",
    savePricing:"Save Settings",
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
    dataSub:"Export or delete all monitoring history data stored on this device.",
    exportAll:"↓ Export All History CSV", deleteAll:"✕ Delete All History",
    aboutTitle:"ℹ About", aboutApp:"Application", aboutVer:"Version",
    aboutHw:"Hardware", aboutCloud:"Cloud",
  },
  id: {
    settingsTitle:"Pengaturan", settingsSub:"Kelola preferensi dan data kamu",
    pricingTitle:"💰 Harga Energi", currencyLabel:"Mata Uang",
    tariffLabel:"Tarif per kWh", thresholdLabel:"Batas Overload (Watt)",
    thresholdSub:"Kirim peringatan saat daya melebihi nilai ini",
    savePricing:"Simpan Pengaturan",
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
    dataSub:"Ekspor atau hapus semua riwayat monitoring di perangkat ini.",
    exportAll:"↓ Ekspor Semua Riwayat CSV", deleteAll:"✕ Hapus Semua Riwayat",
    aboutTitle:"ℹ Tentang", aboutApp:"Aplikasi", aboutVer:"Versi",
    aboutHw:"Perangkat Keras", aboutCloud:"Cloud",
  }
};

function applyLanguage(lang) {
  const t = LANG[lang] || LANG.en;
  const set = (id, val) => { const el = document.getElementById(id); if (el) el.textContent = val; };
  set("txt-settings-title",   t.settingsTitle);
  set("txt-settings-sub",     t.settingsSub);
  set("txt-pricing-title",    t.pricingTitle);
  set("txt-currency-label",   t.currencyLabel);
  set("txt-tariff-label",     t.tariffLabel);
  set("txt-threshold-label",  t.thresholdLabel);
  set("txt-threshold-sub",    t.thresholdSub);
  set("txt-save-pricing",     t.savePricing);
  set("txt-account-title",    t.accountTitle);
  set("txt-name-label",       t.nameLabel);
  set("txt-email-label",      t.emailLabel);
  set("txt-save-profile",     t.saveProfile);
  set("txt-appearance-title", t.appearanceTitle);
  set("txt-theme-label",      t.themeLabel);
  set("txt-theme-dark",       t.themeDark);
  set("txt-theme-darker",     t.themeDarker);
  set("txt-theme-light",      t.themeLight);
  set("txt-lang-label",       t.langLabel);
  set("txt-save-appearance",  t.saveAppearance);
  set("txt-notif-title",      t.notifTitle);
  set("txt-notif-device",     t.notifDevice);
  set("txt-notif-device-sub", t.notifDeviceSub);
  set("txt-notif-disconnect", t.notifDisconnect);
  set("txt-notif-disconnect-sub", t.notifDisconnectSub);
  set("txt-notif-session",    t.notifSession);
  set("txt-notif-session-sub",t.notifSessionSub);
  set("txt-notif-overload",   t.notifOverload);
  set("txt-notif-overload-sub",t.notifOverloadSub);
  set("txt-refresh-label",    t.refreshLabel);
  set("txt-save-notif",       t.saveNotif);
  set("txt-data-title",       t.dataTitle);
  set("txt-data-sub",         t.dataSub);
  set("txt-export-all",       t.exportAll);
  set("txt-delete-all",       t.deleteAll);
  set("txt-about-title",      t.aboutTitle);
  set("txt-about-app",        t.aboutApp);
  set("txt-about-ver",        t.aboutVer);
  set("txt-about-hw",         t.aboutHw);
  set("txt-about-cloud",      t.aboutCloud);
}

function highlightThemeBtn(theme) {
  document.querySelectorAll(".theme-btn").forEach(btn => {
    btn.classList.toggle("active", btn.dataset.theme === theme); });
}

// ================= TARIFF CONVERTER =================
const USD_RATE = 16500;
function updateConvertedPreview() {
  const currency = document.getElementById("currency").value;
  const tariff   = parseFloat(document.getElementById("tariff").value);
  const el       = document.getElementById("tariff-converted");
  if (isNaN(tariff) || tariff <= 0) { el.textContent = ""; return; }
  el.textContent = currency === "IDR"
    ? `≈ $ ${(tariff / USD_RATE).toFixed(4)} / kWh`
    : `≈ Rp ${Math.round(tariff * USD_RATE).toLocaleString("id-ID")} / kWh`;
}
document.getElementById("currency").addEventListener("change", function () {
  const tariffEl = document.getElementById("tariff");
  const current  = parseFloat(tariffEl.value);
  if (!isNaN(current) && current > 0) {
    tariffEl.value = this.value === "USD"
      ? (current / USD_RATE).toFixed(4)
      : Math.round(current * USD_RATE);
  }
  updateConvertedPreview();
});
document.getElementById("tariff").addEventListener("input", updateConvertedPreview);

// ================= LOAD UI =================
document.getElementById("currency").value          = settings.currency;
document.getElementById("tariff").value            = settings.tariff;
document.getElementById("overload-threshold").value = settings.overloadThreshold;
document.getElementById("language").value          = settings.language;
document.getElementById("notif-device").checked    = settings.notifDevice;
document.getElementById("notif-disconnect").checked= settings.notifDisconnect;
document.getElementById("notif-session").checked   = settings.notifSession;
document.getElementById("notif-overload").checked   = settings.notifOverload;
document.getElementById("refresh-interval").value  = settings.refreshInterval;
document.getElementById("display-name").value      = user.displayName || "";
document.getElementById("display-email").value     = user.email || "";
applyLanguage(settings.language);
applyTheme(settings.theme);
highlightThemeBtn(settings.theme);
updateConvertedPreview();

// ================= SAVE PRICING =================
document.getElementById("btn-save-settings").addEventListener("click", () => {
  const currency = document.getElementById("currency").value;
  const tariff   = parseFloat(document.getElementById("tariff").value);
  const threshold= pasrseFloat(document.getElementById("overload-threshold").value);
  if (isNaN(tariff) || tariff <= 0) { showToast("Enter a valid tariff value", "error"); return; }
  if (isNaN(threshold) || threshold <= 0) { showToast("Enter a valid threshold value", "error"); return; }
  settings = { ...settings, currency, tariff, overloadThreshold: threshold };
  localStorage.setItem(`sem_settings_${uid}`, JSON.stringify(settings));
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
document.getElementById("btn-save-appearance").addEventListener("click", () => {
  const theme    = document.querySelector(".theme-btn.active")?.dataset.theme || "dark";
  const language = document.getElementById("language").value;
  settings = { ...settings, theme, language };
  localStorage.setItem(`sem_settings_${uid}`, JSON.stringify(settings));
  localStorage.setItem(`sem_theme_${uid}`,    theme);
  localStorage.setItem(`sem_language_${uid}`, language);
  applyLanguage(language);
  applyTheme(theme); // FIX: shared function dari auth-guard
  showToast("Appearance saved ✓", "success");
});
document.querySelectorAll(".theme-btn").forEach(btn => {
  btn.addEventListener("click", () => { highlightThemeBtn(btn.dataset.theme); applyTheme(btn.dataset.theme); });
});

// ================= SAVE NOTIFICATIONS =================
document.getElementById("btn-save-notif").addEventListener("click", () => {
  const notifDevice     = document.getElementById("notif-device").checked;
  const notifDisconnect = document.getElementById("notif-disconnect").checked;
  const notifSession    = document.getElementById("notif-session").checked;
  const notifOverload   = document.getElementById("notif-overload").checked;
  const refreshInterval = parseInt(document.getElementById("refresh-interval").value);
  settings = { ...settings, notifDevice, notifDisconnect, notifSession, notifOverload, refreshInterval };
  localStorage.setItem(`sem_settings_${uid}`, JSON.stringify(settings));
  showToast("Preferences saved ✓", "success");
});

// ================= EXPORT ALL =================
document.getElementById("btn-export-all").addEventListener("click", () => {
  const history = getHistory();
  if (history.length === 0) { showToast("No history to export", "error"); return; }
  let csv = "Name,Duration,Power (W),Energy (kWh),Cost,Date\n";
  history.forEach(s => { csv += `${s.name},${s.duration},${s.power},${s.energy},${s.cost},${s.date}\n`; });
  downloadCSV(csv, "sem_all_history.csv");
  showToast("Exported ✓", "success");
});

// ================= DELETE ALL =================
document.getElementById("btn-delete-all").addEventListener("click", () => {
  if (getHistory().length === 0) { showToast("Nothing to delete", "error"); return; }
  if (!confirm("Delete ALL history? This cannot be undone.")) return;
  localStorage.removeItem(`sem_history_${uid}`);
  showToast("All history deleted", "");
});