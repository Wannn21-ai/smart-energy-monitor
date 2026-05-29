import {
  requireAuth, renderShell, fillUserInfo, showToast,
  startStatusWatcher, loadAndApplySettings
} from "./auth-guard.js";
import { db, ref, set, onValue } from "./firebase-config.js";
import { loadDeviceHistory } from "./local-history.js";

const user = await requireAuth();
renderShell("history", "HISTORY");
fillUserInfo(user);
startStatusWatcher();
const uid = user.uid;

// Load dan apply theme + language dari Firebase/localStorage
await loadAndApplySettings(uid);

// ================= FIREBASE =================
const historyRef = ref(db, `users/${uid}/history`);

let historyData  = [];
let activeFilter = "all";
let searchKeyword = "";

// ================= LOAD =================
let refreshToken = 0;
async function refreshHistory() {
  const token = ++refreshToken;
  historyData = await loadDeviceHistory(uid);
  if (token !== refreshToken) return;
  render();
}
onValue(historyRef, refreshHistory);
await refreshHistory();

// ================= ELEMENTS =================
const listEl       = document.getElementById("history-list");
const countEl      = document.getElementById("history-count");
const filterTabsEl = document.getElementById("filter-tabs");
const searchInput  = document.getElementById("search-input");
const btnExportAll = document.getElementById("btn-export-all");
const btnDeleteAll = document.getElementById("btn-delete-all");

// ================= FILTER TABS =================
function buildFilterTabs() {
  const names = [...new Set(historyData.map(s => s.name))];
  filterTabsEl.innerHTML = `<button class="device-tab${activeFilter === "all" ? " active" : ""}" data-filter="all">All Devices</button>`;
  names.forEach(name => {
    const btn = document.createElement("button");
    btn.className = `device-tab${activeFilter === name ? " active" : ""}`;
    btn.dataset.filter = name;
    btn.textContent = name;
    filterTabsEl.appendChild(btn);
  });
}
filterTabsEl.addEventListener("click", e => {
  const btn = e.target.closest(".device-tab");
  if (!btn) return;
  activeFilter = btn.dataset.filter;
  buildFilterTabs(); render();
});

// ================= RENDER =================
function render() {
  buildFilterTabs();
  let data = historyData;
  if (activeFilter !== "all") data = data.filter(s => s.name === activeFilter);
  if (searchKeyword) data = data.filter(s => s.name.toLowerCase().includes(searchKeyword));
  countEl.textContent = `${data.length} session${data.length !== 1 ? "s" : ""} recorded`;
  if (data.length === 0) {
    listEl.innerHTML = `
      <div class="empty-state">
        <div class="empty-state-icon">◷</div>
        <div class="empty-state-title">No sessions found</div>
        <div class="empty-state-sub">Start monitoring a device from the Dashboard</div>
      </div>`;
    return;
  }
  listEl.innerHTML = "";
  data.forEach(session => {
    const card = document.createElement("div");
    card.className = "history-card";
    card.dataset.key = session._key;
    card.innerHTML = `
      <div>
        <div class="history-card-name">${session.name}</div>
        <div class="history-card-meta">
          <div class="history-meta-item">⏱<span>${session.duration}</span></div>
          <div class="history-meta-item">⚡<span>${session.power} W</span></div>
          <div class="history-meta-item">🔋<span>${session.energy} kWh</span></div>
          <div class="history-meta-item">💰<span>${session.cost}</span></div>
        </div>
      </div>
      <div class="history-card-actions">
        <div>
          <div class="history-date">${session.date}</div>
          <div style="display:flex;gap:8px;justify-content:flex-end;">
            <button class="btn btn-icon btn-export" data-key="${session._key}" title="Export CSV">↓</button>
            <button class="btn btn-danger btn-delete" data-key="${session._key}" title="Delete">✕</button>
          </div>
        </div>
      </div>`;
    listEl.appendChild(card);
  });
}

// ================= CLICK EVENTS =================
listEl.addEventListener("click", async e => {
  if (e.target.classList.contains("btn-export")) {
    e.stopPropagation();
    const key     = e.target.dataset.key;
    const session = historyData.find(s => s._key === key);
    if (session) exportSingleCSV(session);
    return;
  }
  if (e.target.classList.contains("btn-delete")) {
    e.stopPropagation();
    const key = e.target.dataset.key;
    const session = historyData.find(s => s._key === key);
    if (session?._source === "local") {
      showToast("Local ESP32 history tetap disimpan di LittleFS", "error");
      return;
    }
    if (!confirm("Delete this session?")) return;
    try {
      await set(ref(db, `users/${uid}/history/${key}`), null);
      showToast("Session deleted", "");
    } catch { showToast("Failed to delete", "error"); }
    return;
  }
  const card = e.target.closest(".history-card");
  if (card) {
    const session = historyData.find(s => s._key === card.dataset.key);
    sessionStorage.setItem(`sem_selected_key_${uid}`, card.dataset.key);
    if (session) {
      sessionStorage.setItem(`sem_selected_session_${uid}`, JSON.stringify(session));
    }
    window.location.href = "history-detail.html";
  }
});

// ================= SEARCH =================
searchInput.addEventListener("input", () => {
  searchKeyword = searchInput.value.toLowerCase().trim();
  render();
});

// ================= EXPORT ALL =================
btnExportAll.addEventListener("click", () => {
  if (historyData.length === 0) { showToast("No data to export", "error"); return; }
  let csv = "Name,Duration,Power (W),Energy (kWh),Cost,Date\n";
  historyData.forEach(s => { csv += `${s.name},${s.duration},${s.power},${s.energy},${s.cost},${s.date}\n`; });
  downloadCSV(csv, "sem_all_history.csv");
  showToast("Exported successfully ✓", "success");
});

// ================= DELETE ALL =================
btnDeleteAll.addEventListener("click", async () => {
  if (historyData.length === 0) { showToast("Nothing to delete", "error"); return; }
  if (!confirm("Delete ALL history? This cannot be undone.")) return;
  try {
    await set(historyRef, null);
    showToast("All history deleted", "");
  } catch { showToast("Failed to delete", "error"); }
});

// ================= CSV HELPERS =================
function exportSingleCSV(session) {
  const csv = `Name,Duration,Power (W),Energy (kWh),Cost,Date\n${session.name},${session.duration},${session.power},${session.energy},${session.cost},${session.date}`;
  downloadCSV(csv, `${session.name}_session.csv`);
  showToast(`Exported ${session.name} ✓`, "success");
}
function downloadCSV(content, filename) {
  const blob = new Blob([content], { type: "text/csv" });
  const url  = URL.createObjectURL(blob);
  const a    = document.createElement("a");
  a.href = url; a.download = filename; a.click();
  URL.revokeObjectURL(url);
}
