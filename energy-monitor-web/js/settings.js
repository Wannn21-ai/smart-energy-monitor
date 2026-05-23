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
// BUG FIX #1: Jangan simpan ref sebagai variabel lalu pakai untuk update().
// `update()` dari Firebase SDK v9+ menerima ref object — tapi untuk
// menghindari masalah scope, kita buat path string dan panggil ref() langsung
// di dalam fungsi saveSettingsToFirebase.
const SETTINGS_PATH = `users/${uid}/settings`;
const HISTORY_PATH  = `users/${uid}/history`;

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
try {
  settings = await loadAndApplySettings(uid);
} catch (e) {
  console.error("[SEM] loadAndApplySettings gagal:", e);
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
    // Firebase tidak boleh menerima undefined — filter keluar
    if (val !== undefined && val !== null) clean[key] = val;
  }
  return clean;
}

// ================= SAVE TO FIREBASE =================
// BUG FIX #2: Masalah utama — `update(settingsRef, payload)` kadang gagal
// karena `settingsRef` dibuat di luar fungsi dan ref menjadi stale/invalid
// di beberapa kondisi browser. Solusi: buat ref baru di dalam fungsi,
// dan gunakan set() dengan merge manual sebagai primary (bukan fallback).
// update() tetap dicoba dulu karena lebih efisien untuk partial write.
async function saveSettingsToFirebase(partial) {
  // Sanitasi payload — buang undefined/null
  const payload = sanitizeForFirebase(partial);

  // BUG FIX #3: Jangan mutate `settings` dulu sebelum Firebase berhasil.
  // Simpan merged sebagai local variable dulu.
  const merged = sanitizeForFirebase({ ...settings, ...payload });

  // Buat ref baru di dalam fungsi untuk menghindari stale ref
  const freshRef = ref(db, SETTINGS_PATH);

  let savedOk = false;

  // Coba update() dulu (partial write, lebih efisien)
  try {
    await update(freshRef, payload);
    savedOk = true;
    console.log("[SEM] Settings saved via update():", payload);
  } catch (updateErr) {
    console.warn("[SEM] update() gagal, fallback ke set():", updateErr.message);
    // Fallback: tulis seluruh object settings
    try {
      await set(freshRef, merged);
      savedOk = true;
      console.log("[SEM] Settings saved via set() fallback");
    } catch (setErr) {
      console.error("[SEM] set() juga gagal:", setErr.message);
      // Lempar error supaya caller bisa handle (tampilkan toast error)
      throw setErr;
    }
  }

  // BUG FIX #3 lanjutan: Baru update state lokal SETELAH Firebase berhasil
  if (savedOk) {
    settings = merged;
    localStorage.setItem(`sem_settings_${uid}`, JSON.stringify(settings));
  }
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

// ================= HELPER: button loading state =================
// BUG FIX #4: Buat helper supaya semua tombol konsisten dalam
// menampilkan loading state dan restore teks dengan benar.
function setBtnLoading(btnId, loading, originalText) {
  const btn  = document.getElementById(btnId);
  const span = btn?.querySelector("span");
  if (!btn) return;
  btn.disabled = loading;
  if (span) span.textContent = loading ? "Menyimpan..." : originalText;
}

// ================= SAVE PRICING + THRESHOLD =================
document.getElementById("btn-save-settings").addEventListener("click", async () => {
  const currency  = document.getElementById("currency").value;
  const tariff    = parseFloat(document.getElementById("tariff").value);
  const threshold = parseFloat(document.getElementById("overload-threshold").value);

  if (isNaN(tariff)    || tariff    <= 0) { showToast("Masukkan nilai tarif yang valid",    "error"); return; }
  if (isNaN(threshold) || threshold <= 0) { showToast("Masukkan nilai threshold yang valid", "error"); return; }

  // Ambil teks tombol saat ini untuk restore nanti
  const span = document.querySelector("#btn-save-settings span");
  const originalText = span?.textContent || "Save Settings";
  setBtnLoading("btn-save-settings", true, originalText);

  try {
    await saveSettingsToFirebase({ currency, tariff, overloadThreshold: threshold });

    // Tulis threshold ke /config/threshold — dibaca ESP32 setiap 30 detik
    try {
      await set(ref(db, "config/threshold"), threshold);
      console.log("[SEM] Threshold synced ke /config/threshold:", threshold);
    } catch (e) {
      console.warn("[SEM] Gagal sync threshold ke /config:", e);
      // Tidak fatal — settings utama sudah tersimpan
    }

    showToast("Pengaturan tersimpan ✓", "success");
  } catch (e) {
    console.error("[SEM] Gagal simpan pricing settings:", e);
    showToast("Gagal menyimpan pengaturan — cek koneksi internet", "error");
  } finally {
    setBtnLoading("btn-save-settings", false, originalText);
  }
});

// ================= SAVE PROFILE =================
document.getElementById("btn-save-profile").addEventListener("click", async () => {
  const name = document.getElementById("display-name").value.trim();
  if (!name) { showToast("Nama tidak boleh kosong", "error"); return; }

  const span = document.querySelector("#btn-save-profile span");
  const originalText = span?.textContent || "Update Profile";
  setBtnLoading("btn-save-profile", true, originalText);

  try {
    await updateProfile(auth.currentUser, { displayName: name });
    fillUserInfo(auth.currentUser);
    showToast("Profil diperbarui ✓", "success");
  } catch (e) {
    console.error("[SEM] Gagal update profile:", e);
    showToast("Gagal memperbarui profil", "error");
  } finally {
    setBtnLoading("btn-save-profile", false, originalText);
  }
});

// ================= SAVE APPEARANCE =================
document.getElementById("btn-save-appearance").addEventListener("click", async () => {
  const activeThemeBtn = document.querySelector(".theme-btn.active");
  const theme    = activeThemeBtn?.dataset.theme || "dark";
  const language = document.getElementById("language").value;

  const span = document.querySelector("#btn-save-appearance span");
  const originalText = span?.textContent || "Save Appearance";
  setBtnLoading("btn-save-appearance", true, originalText);

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
    setBtnLoading("btn-save-appearance", false, originalText);
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

  // BUG FIX #5: Validasi refreshInterval sebelum disimpan
  if (isNaN(partial.refreshInterval)) partial.refreshInterval = 3000;

  const span = document.querySelector("#btn-save-notif span");
  const originalText = span?.textContent || "Save Preferences";
  setBtnLoading("btn-save-notif", true, originalText);

  try {
    await saveSettingsToFirebase(partial);
    showToast("Preferensi tersimpan ✓", "success");
  } catch (e) {
    console.error("[SEM] Gagal simpan notifikasi:", e);
    showToast("Gagal menyimpan preferensi", "error");
  } finally {
    setBtnLoading("btn-save-notif", false, originalText);
  }
});

// ================= EXPORT ALL HISTORY =================
document.getElementById("btn-export-all").addEventListener("click", async () => {
  const btn = document.getElementById("btn-export-all");
  btn.disabled = true;

  try {
    const snap = await get(ref(db, HISTORY_PATH));
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
  let snap;
  try {
    snap = await get(ref(db, HISTORY_PATH));
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
    await set(ref(db, HISTORY_PATH), null);
    showToast("Semua riwayat berhasil dihapus", "success");
  } catch (e) {
    console.error("[SEM] Gagal hapus semua history:", e);
    showToast("Gagal menghapus riwayat", "error");
  } finally {
    btn.disabled = false;
  }
});