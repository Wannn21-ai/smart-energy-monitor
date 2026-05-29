import { auth, db, ref, onValue, get } from "./firebase-config.js";
import { onAuthStateChanged, signOut } from "https://www.gstatic.com/firebasejs/10.12.2/firebase-auth.js";

// ── Auth guard ────────────────────────────────────
export function requireAuth() {
  return new Promise((resolve, reject) => {
    const unsub = onAuthStateChanged(auth, user => {
      unsub();
      if (user) { resolve(user); }
      else { window.location.href = "login.html"; reject("not-authenticated"); }
    });
  });
}

// ================================================================
// TRANSLATIONS — dipusatkan di sini supaya semua halaman bisa pakai
// ================================================================
export const LANG = {
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
    // Sidebar & nav
    navDashboard:"Dashboard", navHistory:"History", navAdvanced:"Advanced", navSettings:"Settings",
    signOut:"Sign Out",
    // Dashboard page strings
    dashTitle:"Dashboard", noActiveDevice:"No active device",
    stopSession:"⏹ Stop Session",
    addDeviceTitle:"Add Device",
    addDeviceSub:"Give your device a name to start monitoring",
    deviceNameLabel:"Device Name",
    deviceNamePlaceholder:"e.g. Laptop, AC, Charger",
    cancelBtn:"Cancel", startMonitoring:"Start Monitoring",
    // Stat labels
    labelEnergy:"Energy", labelCost:"Estimated Cost",
    labelSessionCount:"Session Count", labelDeviceName:"Device Name",
    allDevices:"All devices total",
    advReadings:"Advanced Readings", forTech:"For technical reference",
    labelPF:"Power Factor", pfDesc:"0 = poor · 1 = ideal",
    labelFreq:"Frequency", freqDesc:"Hz · standard: 50 Hz",
    labelApparent:"Apparent Power", apparentDesc:"VA · V × I",
    // History page
    histTitle:"History", exportAllCSV:"↓ Export All CSV", deleteAll2:"✕ Delete All",
    searchPlaceholder:"Search by device name...",
    noSessions:"No sessions found",
    noSessionsSub:"Start monitoring a device from the Dashboard",
    allDevicesTab:"All Devices",
    // Advanced page
    advTitle:"Advanced Readings", advSub:"Technical data for engineers & enthusiasts",
    advHeroTitle:"⚠ Technical Reference",
    advHeroSub:"Data on this page is intended for technical analysis. Power Factor and Frequency readings require an active device to be meaningful.",
    clearLog:"✕ Clear Log", exportLog:"↓ Export CSV",
    waitingData:"Waiting for live data...",
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
    // Sidebar & nav
    navDashboard:"Dasbor", navHistory:"Riwayat", navAdvanced:"Lanjutan", navSettings:"Pengaturan",
    signOut:"Keluar",
    // Dashboard page strings
    dashTitle:"Dasbor", noActiveDevice:"Tidak ada device aktif",
    stopSession:"⏹ Hentikan Sesi",
    addDeviceTitle:"Tambah Device",
    addDeviceSub:"Berikan nama untuk device yang terhubung",
    deviceNameLabel:"Nama Device",
    deviceNamePlaceholder:"mis. Laptop, AC, Charger",
    cancelBtn:"Batal", startMonitoring:"Mulai Monitoring",
    // Stat labels
    labelEnergy:"Energi", labelCost:"Estimasi Biaya",
    labelSessionCount:"Jumlah Sesi", labelDeviceName:"Nama Device",
    allDevices:"Total semua device",
    advReadings:"Pembacaan Lanjutan", forTech:"Untuk referensi teknis",
    labelPF:"Faktor Daya", pfDesc:"0 = buruk · 1 = ideal",
    labelFreq:"Frekuensi", freqDesc:"Hz · standar: 50 Hz",
    labelApparent:"Daya Semu", apparentDesc:"VA · V × I",
    // History page
    histTitle:"Riwayat", exportAllCSV:"↓ Ekspor Semua CSV", deleteAll2:"✕ Hapus Semua",
    searchPlaceholder:"Cari nama device...",
    noSessions:"Tidak ada sesi ditemukan",
    noSessionsSub:"Mulai monitoring device dari Dasbor",
    allDevicesTab:"Semua Device",
    // Advanced page
    advTitle:"Pembacaan Lanjutan", advSub:"Data teknis untuk insinyur & penggemar",
    advHeroTitle:"⚠ Referensi Teknis",
    advHeroSub:"Data di halaman ini ditujukan untuk analisis teknis. Pembacaan Power Factor dan Frekuensi memerlukan device aktif.",
    clearLog:"✕ Hapus Log", exportLog:"↓ Ekspor CSV",
    waitingData:"Menunggu data langsung...",
  }
};

// ── Utility: set text ─────────────────────────────
function s(id, val) { const el = document.getElementById(id); if (el) el.textContent = val; }

// ================================================================
// applyLanguage — apply ke semua halaman (bukan hanya settings)
// Dipanggil dari renderShell setelah settings di-load
// ================================================================
export function applyLanguage(lang) {
  const t = LANG[lang] || LANG.en;

  // ── Settings page ──
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

  // ── Dashboard page ──
  s("page-title-dash",          t.dashTitle);
  s("active-device-label-txt",  t.noActiveDevice);
  s("btn-stop-txt",             t.stopSession);
  s("modal-add-title",          t.addDeviceTitle);
  s("modal-add-sub",            t.addDeviceSub);
  s("modal-device-label",       t.deviceNameLabel);
  s("modal-cancel-txt",         t.cancelBtn);
  s("modal-save-txt",           t.startMonitoring);
  s("stat-label-energy",        t.labelEnergy);
  s("stat-label-cost",          t.labelCost);
  s("stat-label-sessions",      t.labelSessionCount);
  s("stat-label-device",        t.labelDeviceName);
  s("stat-all-devices",         t.allDevices);
  s("adv-readings-title",       t.advReadings);
  s("adv-readings-sub",         t.forTech);
  s("adv-label-pf",             t.labelPF);
  s("adv-desc-pf",              t.pfDesc);
  s("adv-label-freq",           t.labelFreq);
  s("adv-desc-freq",            t.freqDesc);
  s("adv-label-apparent",       t.labelApparent);
  s("adv-desc-apparent",        t.apparentDesc);

  // ── History page ──
  s("page-title-history",       t.histTitle);
  s("btn-export-all-txt",       t.exportAllCSV);
  s("btn-delete-all-txt",       t.deleteAll2);
  const searchEl = document.getElementById("search-input");
  if (searchEl) searchEl.placeholder = t.searchPlaceholder;

  // ── Advanced page ──
  s("page-title-advanced",      t.advTitle);
  s("page-sub-advanced",        t.advSub);
  s("adv-hero-title",           t.advHeroTitle);
  s("adv-hero-sub",             t.advHeroSub);
  s("btn-clear-log-txt",        t.clearLog);
  s("btn-export-log-txt",       t.exportLog);

  // ── Sidebar nav labels (rendered by renderShell) ──
  s("nav-label-dashboard",      t.navDashboard);
  s("nav-label-history",        t.navHistory);
  s("nav-label-advanced",       t.navAdvanced);
  s("nav-label-settings",       t.navSettings);
  s("sidebar-signout-txt",      t.signOut);
}

// ================================================================
// Shell render — sekarang load settings dari Firebase/localStorage
// lalu apply theme + language sebelum render selesai
// ================================================================
export function renderShell(activePage, pageTitle) {
  const sidebarEl = document.getElementById("sidebar");
  const topbarEl  = document.getElementById("topbar");
  const navItems  = [
    { href: "index.html",    icon: "⊡", key: "dashboard", labelId: "nav-label-dashboard", defaultLabel: "Dashboard" },
    { href: "history.html",  icon: "◷", key: "history",   labelId: "nav-label-history",   defaultLabel: "History"   },
    { href: "advanced.html", icon: "◈", key: "advanced",  labelId: "nav-label-advanced",  defaultLabel: "Advanced"  },
    { href: "settings.html", icon: "⚙", key: "settings",  labelId: "nav-label-settings",  defaultLabel: "Settings"  },
  ];

  sidebarEl.innerHTML = `
    <div class="sidebar-logo">
      <div class="sidebar-logo-icon">
        <img src="assets/logo/LOGO.png" alt="SEM Logo" style="width:22px;height:22px;object-fit:contain" onerror="this.style.display='none';this.parentElement.textContent='⚡'"/>
      </div>
      <div>
        <div class="sidebar-logo-text">SMART ENERGY</div>
        <div class="sidebar-logo-sub">Monitoring</div>
      </div>
    </div>
    <nav class="sidebar-nav">
      <div class="sidebar-section-label">Navigation</div>
      ${navItems.map(n => `
        <a href="${n.href}" class="${activePage === n.key ? "active" : ""}">
          <span class="nav-icon">${n.icon}</span>
          <span id="${n.labelId}">${n.defaultLabel}</span>
        </a>`).join("")}
    </nav>
    <div class="sidebar-bottom">
      <div class="sidebar-user" id="sidebar-user">
        <div class="sidebar-user-avatar" id="user-avatar">?</div>
        <div class="sidebar-user-info">
          <div class="sidebar-user-name" id="user-name">Loading...</div>
          <div class="sidebar-user-email" id="user-email"></div>
        </div>
      </div>
      <div style="margin-top:8px;">
        <button id="btn-logout" class="btn btn-ghost" style="width:100%;justify-content:center;font-size:12px;">
          <span id="sidebar-signout-txt">Sign Out</span>
        </button>
      </div>
    </div>`;

  topbarEl.innerHTML = `
    <button class="topbar-menu-btn" id="menu-btn">☰</button>
    <div class="topbar-title">${pageTitle}</div>
    <div class="topbar-status">
      <div class="status-dot" id="system-dot"></div>
      <span id="system-status-text">Connecting...</span>
    </div>`;

  // Sidebar toggle
  const sidebar  = document.getElementById("sidebar");
  const menuBtn  = document.getElementById("menu-btn");
  let backdrop   = document.getElementById("sidebar-backdrop");
  if (!backdrop) {
    backdrop = document.createElement("div");
    backdrop.id = "sidebar-backdrop";
    backdrop.className = "sidebar-backdrop";
    document.body.appendChild(backdrop);
  }
  const isMobile   = () => window.innerWidth <= 768;
  const openSidebar  = () => { sidebar.classList.add("open"); if (isMobile()) backdrop.classList.add("show"); };
  const closeSidebar = () => { sidebar.classList.remove("open"); backdrop.classList.remove("show"); };
  menuBtn.addEventListener("click", () => sidebar.classList.contains("open") ? closeSidebar() : openSidebar());
  backdrop.addEventListener("click", closeSidebar);
  window.addEventListener("resize", () => { if (!isMobile()) backdrop.classList.remove("show"); });
  if (!isMobile()) openSidebar();

  // Logout
  document.getElementById("btn-logout").addEventListener("click", async () => {
    await signOut(auth);
    window.location.href = "login.html";
  });
}

// ================================================================
// loadAndApplySettings — dipanggil oleh setiap halaman setelah
// requireAuth(). Membaca settings dari cache → Firebase, lalu
// apply theme + language ke semua elemen di halaman saat ini.
// ================================================================
export async function loadAndApplySettings(uid) {
  const DEFAULTS = {
    currency: "IDR", tariff: 1444.70, overloadThreshold: 2000,
    theme: "dark", language: "en",
    notifDevice: true, notifDisconnect: true, notifSession: true,
    notifOverload: true, refreshInterval: 3000
  };

  // 1. Baca dari localStorage dulu (instant, tidak flicker)
  let settings = { ...DEFAULTS };
  try {
    const cached = JSON.parse(localStorage.getItem(`sem_settings_${uid}`));
    if (cached) settings = { ...DEFAULTS, ...cached };
  } catch {}

  // Apply segera dari cache supaya tidak flicker
  applyTheme(settings.theme);
  applyLanguage(settings.language);

  // 2. Fetch dari Firebase (sumber kebenaran)
  try {
    const snap = await get(ref(db, `users/${uid}/settings`));
    if (snap.exists()) {
      const remote = { ...DEFAULTS, ...snap.val() };
      // Sync ke localStorage supaya halaman lain & device lain langsung dapat
      localStorage.setItem(`sem_settings_${uid}`, JSON.stringify(remote));
      // Apply jika berbeda dari cache
      if (remote.theme    !== settings.theme)    applyTheme(remote.theme);
      if (remote.language !== settings.language) applyLanguage(remote.language);
      settings = remote;
    }
  } catch (e) {
    console.warn("[SEM] Gagal load settings dari Firebase:", e);
  }

  try {
    const appSnap = await get(ref(db, "config/app"));
    if (appSnap.exists()) {
      const shared = appSnap.val() || {};
      const sharedThreshold = Number(shared.overloadThreshold ?? shared.threshold);
      const sharedTariff = Number(shared.electricityCostPerKwh ?? shared.tariff ?? shared.tarif);
      const next = { ...settings };
      if (Number.isFinite(sharedThreshold) && sharedThreshold > 0) next.overloadThreshold = sharedThreshold;
      if (Number.isFinite(sharedTariff) && sharedTariff > 0) next.tariff = sharedTariff;
      if (JSON.stringify(next) !== JSON.stringify(settings)) {
        settings = next;
        localStorage.setItem(`sem_settings_${uid}`, JSON.stringify(settings));
      }
    } else {
      const thresholdSnap = await get(ref(db, "config/threshold"));
      if (thresholdSnap.exists()) {
        const sharedThreshold = Number(thresholdSnap.val());
        if (Number.isFinite(sharedThreshold) && sharedThreshold > 0 &&
            sharedThreshold !== settings.overloadThreshold) {
          settings = { ...settings, overloadThreshold: sharedThreshold };
          localStorage.setItem(`sem_settings_${uid}`, JSON.stringify(settings));
        }
      }
    }
  } catch (e) {
    console.warn("[SEM] Gagal load config global:", e);
  }

  return settings;
}

// ── User info ─────────────────────────────────────
export function fillUserInfo(user) {
  const avatarEl = document.getElementById("user-avatar");
  const nameEl   = document.getElementById("user-name");
  const emailEl  = document.getElementById("user-email");
  if (!avatarEl) return;
  const name = user.displayName || user.email.split("@")[0];
  avatarEl.textContent = name.charAt(0).toUpperCase();
  nameEl.textContent   = name;
  emailEl.textContent  = user.email;
}

// ── Status dot ────────────────────────────────────
export function setSystemStatus(online) {
  const dot  = document.getElementById("system-dot");
  const text = document.getElementById("system-status-text");
  if (!dot) return;
  dot.className    = `status-dot ${online ? "online" : "offline"}`;
  text.textContent = online ? "Online" : "Offline";
}

// ── Firebase status watcher ───────────────────────
export function startStatusWatcher() {
  onValue(ref(db, "live/system"), snapshot => {
    const sys  = snapshot.val() || {};
    const now  = Math.floor(Date.now() / 1000);
    const diff = now - (sys.timestamp || 0);
    setSystemStatus(sys.internet === true && sys.timestamp > 0 && diff <= 15);
  });
}

// ── Toast ─────────────────────────────────────────
export function showToast(msg, type = "") {
  let toast = document.getElementById("global-toast");
  if (!toast) {
    toast = document.createElement("div");
    toast.id = "global-toast";
    toast.className = "toast";
    document.body.appendChild(toast);
  }
  toast.className = `toast ${type}`;
  toast.textContent = msg;
  requestAnimationFrame(() => toast.classList.add("show"));
  clearTimeout(toast._t);
  toast._t = setTimeout(() => toast.classList.remove("show"), 3000);
}

// ── Theme ─────────────────────────────────────────
export function applyTheme(theme) {
  const r = document.documentElement;
  if (theme === "darker") {
    r.style.setProperty("--bg-base",        "#000000");
    r.style.setProperty("--bg-surface",     "#0a0a0a");
    r.style.setProperty("--bg-elevated",    "#111111");
    r.style.setProperty("--bg-hover",       "#181818");
    r.style.setProperty("--text-primary",   "#f0f0f0");
    r.style.setProperty("--text-secondary", "#888888");
    r.style.setProperty("--text-muted",     "#444444");
    r.style.setProperty("--border",         "rgba(255,255,255,0.07)");
    r.style.setProperty("--border-accent",  "rgba(255,255,255,0.15)");
    r.style.setProperty("--chart-tick",     "#555555");
    r.style.setProperty("--chart-grid",     "rgba(255,255,255,0.03)");
  } else if (theme === "light") {
    r.style.setProperty("--bg-base",        "#f0f2f5");
    r.style.setProperty("--bg-surface",     "#ffffff");
    r.style.setProperty("--bg-elevated",    "#f8f9fa");
    r.style.setProperty("--bg-hover",       "#e9ecef");
    r.style.setProperty("--text-primary",   "#1a1a1a");
    r.style.setProperty("--text-secondary", "#555555");
    r.style.setProperty("--text-muted",     "#999999");
    r.style.setProperty("--border",         "rgba(0,0,0,0.08)");
    r.style.setProperty("--border-accent",  "rgba(0,0,0,0.15)");
    r.style.setProperty("--chart-tick",     "#999999");
    r.style.setProperty("--chart-grid",     "rgba(0,0,0,0.06)");
  } else { // dark (default)
    r.style.setProperty("--bg-base",        "#0a0a0a");
    r.style.setProperty("--bg-surface",     "#111111");
    r.style.setProperty("--bg-elevated",    "#1a1a1a");
    r.style.setProperty("--bg-hover",       "#222222");
    r.style.setProperty("--text-primary",   "#f0f0f0");
    r.style.setProperty("--text-secondary", "#888888");
    r.style.setProperty("--text-muted",     "#444444");
    r.style.setProperty("--border",         "rgba(255,255,255,0.07)");
    r.style.setProperty("--border-accent",  "rgba(255,255,255,0.15)");
    r.style.setProperty("--chart-tick",     "#666666");
    r.style.setProperty("--chart-grid",     "rgba(255,255,255,0.04)");
  }
}

export function updateChartColors(...charts) {
  const tick = getComputedStyle(document.documentElement).getPropertyValue("--chart-tick").trim() || "#666";
  const grid = getComputedStyle(document.documentElement).getPropertyValue("--chart-grid").trim() || "rgba(255,255,255,0.04)";
  charts.forEach(chart => {
    if (!chart) return;
    const scales = chart.options?.scales || {};
    ["x", "y"].forEach(axis => {
      if (!scales[axis]) return;
      if (!scales[axis].ticks) scales[axis].ticks = {};
      if (!scales[axis].grid)  scales[axis].grid  = {};
      scales[axis].ticks.color = tick;
      scales[axis].grid.color  = grid;
    });
    chart.update("none");
  });
}
