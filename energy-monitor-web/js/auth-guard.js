// ================================================
// auth-guard.js
// Shared across all pages that require login.
// Handles: auth guard, shell render, theme, charts
// ================================================
import { auth, db, ref, onValue } from "./firebase-config.js";
import { onAuthStateChanged, signOut } from "https://www.gstatic.com/firebasejs/10.12.2/firebase-auth.js";

// ── Auth guard ──────────────────────────────────
export function requireAuth() {
  return new Promise((resolve, reject) => {
    const unsub = onAuthStateChanged(auth, user => {
      unsub();
      if (user) { resolve(user); }
      else { window.location.href = "login.html"; reject("not-authenticated"); }
    });
  });
}

// ── Shell render ────────────────────────────────
export function renderShell(activePage, pageTitle) {
  const sidebarEl = document.getElementById("sidebar");
  const topbarEl  = document.getElementById("topbar");

  const navItems = [
    { href: "index.html",    icon: "⊡", label: "Dashboard", key: "dashboard" },
    { href: "history.html",  icon: "◷", label: "History",   key: "history"   },
    { href: "settings.html", icon: "◈", label: "Settings",  key: "settings"  },
  ];

  sidebarEl.innerHTML = `
    <div class="sidebar-logo">
      <div class="sidebar-logo-icon">
        <img src="assets/logo/LOGO.png" alt="SEM Logo" style="width: 22px; height: 22px;object-fit:contain" />
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
          <span class="nav-icon">${n.icon}</span>${n.label}
        </a>`).join("")}
    </nav>
    <div class="sidebar-bottom">
      <div class="sidebar-user" id="sidebar-user">
        <div class="sidebar-user-avatar" id="user-avatar">?</div>
        <div class="sidebar-user-info">
          <div class="sidebar-user-name"  id="user-name">Loading...</div>
          <div class="sidebar-user-email" id="user-email"></div>
        </div>
      </div>
      <div style="margin-top:8px;">
        <button id="btn-logout" class="btn btn-ghost" style="width:100%;justify-content:center;font-size:12px;">
          Sign Out
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

  // ── Sidebar toggle ──
  const sidebar    = document.getElementById("sidebar");
  const menuBtn    = document.getElementById("menu-btn");
  let backdrop     = document.getElementById("sidebar-backdrop");
  if (!backdrop) {
    backdrop = document.createElement("div");
    backdrop.id = "sidebar-backdrop";
    backdrop.className = "sidebar-backdrop";
    document.body.appendChild(backdrop);
  }
  const isMobile    = () => window.innerWidth <= 768;
  const openSidebar  = () => { sidebar.classList.add("open"); if (isMobile()) backdrop.classList.add("show"); };
  const closeSidebar = () => { sidebar.classList.remove("open"); backdrop.classList.remove("show"); };
  menuBtn.addEventListener("click", () => sidebar.classList.contains("open") ? closeSidebar() : openSidebar());
  backdrop.addEventListener("click", closeSidebar);
  window.addEventListener("resize", () => { if (!isMobile()) backdrop.classList.remove("show"); });
  if (!isMobile()) openSidebar();

  // ── Logout ──
  document.getElementById("btn-logout").addEventListener("click", async () => {
    await signOut(auth);
    window.location.href = "login.html";
  });
}

// ── User info ───────────────────────────────────
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

// ── Status dot ──────────────────────────────────
export function setSystemStatus(online) {
  const dot  = document.getElementById("system-dot");
  const text = document.getElementById("system-status-text");
  if (!dot) return;
  dot.className   = `status-dot ${online ? "online" : "offline"}`;
  text.textContent = online ? "Online" : "Offline";
}

// ── Firebase status watcher ─────────────────────
export function startStatusWatcher() {
  onValue(ref(db, "live/system"), snapshot => {
    const sys  = snapshot.val() || {};
    const now  = Math.floor(Date.now() / 1000);
    const diff = now - (sys.timestamp || 0);
    setSystemStatus(sys.internet === true && sys.timestamp > 0 && diff <= 120);
  });
}

// ── Toast ───────────────────────────────────────
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

// ── Theme (FIX: dipusatkan di sini, tidak perlu duplikasi di settings.js & dashboard.js) ──
export function applyTheme(theme) {
  const r = document.documentElement;
  if (theme === "darker") {
    r.style.setProperty("--bg-base",      "#000000");
    r.style.setProperty("--bg-surface",   "#0a0a0a");
    r.style.setProperty("--bg-elevated",  "#111111");
    r.style.setProperty("--bg-hover",     "#181818");
    r.style.setProperty("--text-primary", "#f0f0f0");
    r.style.setProperty("--text-secondary","#888888");
    r.style.setProperty("--text-muted",   "#444444");
    r.style.setProperty("--border",       "rgba(255,255,255,0.07)");
    r.style.setProperty("--border-accent","rgba(255,255,255,0.15)");
    r.style.setProperty("--chart-tick",   "#555555");
    r.style.setProperty("--chart-grid",   "rgba(255,255,255,0.03)");
  } else if (theme === "light") {
    r.style.setProperty("--bg-base",      "#f0f2f5");
    r.style.setProperty("--bg-surface",   "#ffffff");
    r.style.setProperty("--bg-elevated",  "#f8f9fa");
    r.style.setProperty("--bg-hover",     "#e9ecef");
    r.style.setProperty("--text-primary", "#1a1a1a");
    r.style.setProperty("--text-secondary","#555555");
    r.style.setProperty("--text-muted",   "#999999");
    r.style.setProperty("--border",       "rgba(0,0,0,0.08)");
    r.style.setProperty("--border-accent","rgba(0,0,0,0.15)");
    // FIX: chart tick & grid untuk light mode agar terbaca
    r.style.setProperty("--chart-tick",   "#999999");
    r.style.setProperty("--chart-grid",   "rgba(0,0,0,0.06)");
  } else {
    // dark (default)
    r.style.setProperty("--bg-base",      "#0a0a0a");
    r.style.setProperty("--bg-surface",   "#111111");
    r.style.setProperty("--bg-elevated",  "#1a1a1a");
    r.style.setProperty("--bg-hover",     "#222222");
    r.style.setProperty("--text-primary", "#f0f0f0");
    r.style.setProperty("--text-secondary","#888888");
    r.style.setProperty("--text-muted",   "#444444");
    r.style.setProperty("--border",       "rgba(255,255,255,0.07)");
    r.style.setProperty("--border-accent","rgba(255,255,255,0.15)");
    r.style.setProperty("--chart-tick",   "#666666");
    r.style.setProperty("--chart-grid",   "rgba(255,255,255,0.04)");
  }
}

// ── Update Chart.js tick & grid warna setelah tema berubah ──
// FIX: chart di light mode sebelumnya tetap gelap karena warna hardcoded
export function updateChartColors(...charts) {
  const tick = getComputedStyle(document.documentElement)
    .getPropertyValue("--chart-tick").trim() || "#666";
  const grid = getComputedStyle(document.documentElement)
    .getPropertyValue("--chart-grid").trim() || "rgba(255,255,255,0.04)";

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
    chart.update("none"); // update tanpa animasi
  });
}