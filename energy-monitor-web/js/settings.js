import {
  requireAuth, renderShell, fillUserInfo, showToast,
  startStatusWatcher, applyTheme, applyLanguage,
  loadAndApplySettings
} from "./auth-guard.js";
// PERBAIKAN #1: Pastikan `update` ikut diimport dari firebase-config
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
// PERBAIKAN #2: Tangkap error agar kegagalan load settings
// tidak menghentikan eksekusi seluruh module
try {
  settings = await loadAndApplySettings(uid);
} catch (e) {
  console.error("[SEM] loadAndApplySettings gagal:", e);
  // Fallback ke localStorage
  try {
    const cached = JSON.parse(localStorage.getItem(`sem_settings_${uid}`));
    if (cached) settings = { ...DEFAULTS, ...cached };
  } catch {}
  applyTheme(settings.theme);
  applyLanguage(settings.language);
}

applyToUI();

// ================= SANITIZE HELPER =================
function sanitizeForFirebase(obj) {
  const clean = {};
  for (const [key, val] of Object.entries(obj)) {
    if (val !== undefined && val !== null) clean[key] = val;
  }
  return clean;
}

// ================= SAVE TO FIREBASE =================
// PERBAIKAN #3: Ganti update() dengan set() + merge manual
// supaya tidak bergantung pada `update` yang kadang tidak ter-export
// dengan benar di beberapa bundler/browser. Juga tambah try/catch
// yang lebih ketat agar error tidak propagate ke handler tombol lain.
async function saveSettingsToFirebase(partial) {
  const payload = sanitizeForFirebase(partial);
  // Merge dengan settings yang ada dulu
  const merged = sanitizeForFirebase({ ...settings, ...payload });
  settings = { ...settings, ...payload };

  try {
    // Coba pakai update dulu (lebih efisien, partial write)
    await update(settingsRef, payload);
    console.log("[SEM] Settings berhasil disimpan ke Firebase (update):", payload);
  } catch (updateErr) {
    console.warn("[SEM] update() gagal, mencoba set() penuh:", updateErr);
    try {
      // Fallback: set seluruh settings object
      await set(settingsRef, merged);
      console.log("[SEM] Settings berhasil disimpan ke Firebase (set fallback)");
    } catch (setErr) {
      console.error("[SEM] set() juga gagal:", setErr);
      showToast("Gagal sync ke cloud, tersimpan lokal", "");
    }
  }

  // Selalu simpan ke localStorage sebagai backup
  localStorage.setItem(`sem_settings_${uid}`, JSON.stringify(settings));
}

// ================= APPLY TO UI =================
function applyToUI() {
  const el = id => document.getElementById(id);
  if (el("currency"))           el("currency").value           = settings.currency          ?? "IDR";
  if (el("tariff"))             el("tariff").value             = settings.tariff            ?? 1444.70;
  if (el("overload-threshold")) el("overload-threshold").value = settings.overloadThreshold ?? 2000;
  if (el("language"))           el("language").value           = settings.language          ?? "en";
  if (el("notif-device"))       el("notif-device").checked     = settings.notifDevice       ?? true;
  if (el("notif-disconnect"))   el("notif-disconnect").checked = settings.notifDisconnect   ?? true;
  if (el("notif-session"))      el("notif-session").checked    = settings.notifSession      ?? true;
  if (el("notif-overload"))     el("notif-overload").checked   = settings.notifOverload     ?? true;
  if (el("refresh-interval"))   el("refresh-interval").value  = settings.refreshInterval   ?? 3000;
  if (el("display-name"))       el("display-name").value      = user.displayName || "";
  if (el("display-email"))      el("display-email").value     = user.email || "";
  highlightThemeBtn(settings.theme || "dark");
  updateConvertedPreview();
}

function highlightThemeBtn(theme) {
  document.querySelectorAll(".theme-btn").forEach(btn =>
    btn.classList.toggle("active", btn.dataset.theme === theme));
}

// ================= TARIFF CONVERTER =================
const USD_RATE = 16500;
function updateConvertedPreview() {
  const currencyEl = document.getElementById("currency");
  const tariffEl   = document.getElementById("tariff");
  const el         = document.getElementById("tariff-converted");
  if (!el || !currencyEl || !tariffEl) return;
  const currency = currencyEl.value;
  const tariff   = parseFloat(tariffEl.value);
  if (isNaN(tariff) || tariff <= 0) { el.textContent = ""; return; }
  el.textContent = currency === "IDR"
    ? `≈ $ ${(tariff / USD_RATE).toFixed(4)} / kWh`
    : `≈ Rp ${Math.round(tariff * USD_RATE).toLocaleString("id-ID")} / kWh`;
}

document.getElementById("currency").addEventListener("change", function () {
  const tariffEl = document.getElementById("tariff");
  const cur      = parseFloat(tariffEl.value);
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
  const currencyEl  = document.getElementById("currency");
  const tariffEl    = document.getElementById("tariff");
  const thresholdEl = document.getElementById("overload-threshold");

  const currency  = currencyEl.value;
  const tariff    = parseFloat(tariffEl.value);
  const threshold = parseFloat(thresholdEl.value);

  if (isNaN(tariff)    || tariff    <= 0) { showToast("Masukkan nilai tarif yang valid",    "error"); return; }
  if (isNaN(threshold) || threshold <= 0) { showToast("Masukkan nilai threshold yang valid", "error"); return; }

  const btn  = document.getElementById("btn-save-settings");
  const span = btn.querySelector("span");
  btn.disabled      = true;
  span.textContent  = "Menyimpan...";

  try {
    await saveSettingsToFirebase({ currency, tariff, overloadThreshold: threshold });

    // Tulis threshold ke /config/threshold — dibaca ESP32 setiap 30 detik
    try {
      await set(ref(db, "config/threshold"), threshold);
      console.log("[SEM] Threshold synced ke /config/threshold:", threshold);
    } catch (e) {
      console.warn("[SEM] Gagal sync threshold ke /config:", e);
    }

    showToast("Pengaturan tersimpan ✓", "success");
  } catch (e) {
    console.error("[SEM] Gagal simpan pricing settings:", e);
    showToast("Gagal menyimpan pengaturan", "error");
  } finally {
    btn.disabled     = false;
    span.textContent = "Save Settings";
  }
});

// ================= SAVE PROFILE =================
document.getElementById("btn-save-profile").addEventListener("click", async () => {
  const nameEl = document.getElementById("display-name");
  const name   = nameEl.value.trim();
  if (!name) { showToast("Nama tidak boleh kosong", "error"); return; }

  const btn  = document.getElementById("btn-save-profile");
  const span = btn.querySelector("span");
  btn.disabled     = true;
  span.textContent = "Menyimpan...";

  try {
    await updateProfile(auth.currentUser, { displayName: name });
    fillUserInfo(auth.currentUser);
    showToast("Profil diperbarui ✓", "success");
  } catch (e) {
    console.error("[SEM] Gagal update profile:", e);
    showToast("Gagal memperbarui profil", "error");
  } finally {
    btn.disabled     = false;
    span.textContent = "Update Profile";
  }
});

// ================= SAVE APPEARANCE =================
document.getElementById("btn-save-appearance").addEventListener("click", async () => {
  const activeThemeBtn = document.querySelector(".theme-btn.active");
  const theme    = activeThemeBtn?.dataset.theme || "dark";
  const language = document.getElementById("language").value;

  const btn  = document.getElementById("btn-save-appearance");
  const span = btn.querySelector("span");
  btn.disabled     = true;
  span.textContent = "Menyimpan...";

  try {
    await saveSettingsToFirebase({ theme, language });
    localStorage.setItem(`sem_theme_${uid}`, theme);
    applyTheme(theme);
    applyLanguage(language);
    showToast("Tampilan tersimpan ✓", "success");
  } catch (e) {
    console.error("[SEM] Gagal simpan appearance:", e);
    showToast("Gagal menyimpan tampilan", "error");
  } finally {
    btn.disabled     = false;
    span.textContent = "Save Appearance";
  }
});

document.querySelectorAll(".theme-btn").forEach(btn => {
  btn.addEventListener("click", () => {
    highlightThemeBtn(btn.dataset.theme);
    applyTheme(btn.dataset.theme); // preview langsung
  });
});

// ================= SAVE NOTIFICATIONS =================
document.getElementById("btn-save-notif").addEventListener("click", async () => {
  const partial = {
    notifDevice:     document.getElementById("notif-device").checked,
    notifDisconnect: document.getElementById("notif-disconnect").checked,
    notifSession:    document.getElementById("notif-session").checked,
    notifOverload:   document.getElementById("notif-overload").checked,
    refreshInterval: parseInt(document.getElementById("refresh-interval").value)
  };

  const btn  = document.getElementById("btn-save-notif");
  const span = btn.querySelector("span");
  btn.disabled     = true;
  span.textContent = "Menyimpan...";

  try {
    await saveSettingsToFirebase(partial);
    showToast("Preferensi tersimpan ✓", "success");
  } catch (e) {
    console.error("[SEM] Gagal simpan notifikasi:", e);
    showToast("Gagal menyimpan preferensi", "error");
  } finally {
    btn.disabled     = false;
    span.textContent = "Save Preferences";
  }
});

// ================= EXPORT ALL HISTORY =================
// PERBAIKAN #4: Tombol export/delete menggunakan ID yang benar
// sesuai settings.html (btn-export-all & btn-delete-all)
document.getElementById("btn-export-all").addEventListener("click", async () => {
  const btn = document.getElementById("btn-export-all");
  btn.disabled = true;

  try {
    const snap = await get(historyRef);
    if (!snap.exists()) {
      showToast("Tidak ada riwayat untuk diekspor", "error");
      return;
    }
    const history = Object.values(snap.val()).sort((a, b) => b.timestamp - a.timestamp);
    let csv = "Name,Duration,Power (W),Energy (kWh),Cost,Date\n";
    history.forEach(s => {
      csv += `${s.name},${s.duration},${s.power},${s.energy},${s.cost},${s.date}\n`;
    });
    const blob = new Blob([csv], { type: "text/csv" });
    const url  = URL.createObjectURL(blob);
    const a    = document.createElement("a");
    a.href     = url;
    a.download = "sem_all_history.csv";
    document.body.appendChild(a);
    a.click();
    document.body.removeChild(a);
    URL.revokeObjectURL(url);
    showToast(`Berhasil diekspor (${history.length} sesi) ✓`, "success");
  } catch (e) {
    console.error("[SEM] Gagal export history:", e);
    showToast("Gagal mengekspor riwayat", "error");
  } finally {
    btn.disabled = false;
  }
});

// ================= DELETE ALL HISTORY =================
document.getElementById("btn-delete-all").addEventListener("click", async () => {
  // PERBAIKAN #5: Cek dulu apakah ada data sebelum konfirmasi
  let snap;
  try {
    snap = await get(historyRef);
  } catch (e) {
    showToast("Gagal memeriksa riwayat", "error");
    return;
  }

  if (!snap.exists()) {
    showToast("Tidak ada riwayat untuk dihapus", "error");
    return;
  }

  const count = Object.keys(snap.val()).length;
  if (!confirm(`Hapus SEMUA ${count} riwayat sesi? Tindakan ini tidak dapat dibatalkan.`)) return;

  const btn = document.getElementById("btn-delete-all");
  btn.disabled = true;

  try {
    await set(historyRef, null);
    showToast("Semua riwayat berhasil dihapus", "success");
  } catch (e) {
    console.error("[SEM] Gagal hapus semua history:", e);
    showToast("Gagal menghapus riwayat", "error");
  } finally {
    btn.disabled = false;
  }
});