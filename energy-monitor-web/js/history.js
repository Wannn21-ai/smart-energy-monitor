import { requireAuth, renderShell, fillUserInfo, showToast, startStatusWatcher } from "./auth-guard.js";
const user = await requireAuth();
renderShell("history", "HISTORY");
fillUserInfo(user);
startStatusWatcher();
const uid = user.uid;

// ================= DATA =================
function getHistory() {
  try { return JSON.parse(localStorage.getItem(`sem_history_${uid}`)) || []; }
  catch { return []; }
}
function saveHistory(data) { localStorage.setItem(`sem_history_${uid}`, JSON.stringify(data)); }

let historyData    = getHistory();
let activeFilter   = "all";
let searchKeyword  = "";

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
  const btn = e.target.closest(".device-tab"); if (!btn) return;
  activeFilter = btn.dataset.filter;
  buildFilterTabs(); render();
});

// ================= RENDER =================
function render() {
  historyData = getHistory();
  buildFilterTabs();

  let data = historyData;
  if (activeFilter !== "all") data = data.filter(s => s.name === activeFilter);
  if (searchKeyword)          data = data.filter(s => s.name.toLowerCase().includes(searchKeyword));

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
    // FIX: simpan session.id di data attribute, bukan array index
    // Index bisa berubah jika ada filter aktif atau item dihapus sebelumnya
    card.dataset.id = session.id;
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
            <button class="btn btn-icon btn-export" data-id="${session.id}" title="Export CSV">↓</button>
            <button class="btn btn-danger btn-delete" data-id="${session.id}" title="Delete">✕</button>
          </div>
        </div>
      </div>`;
    listEl.appendChild(card);
  });
}

// ================= CLICK EVENTS =================
listEl.addEventListener("click", e => {
  // Export single
  if (e.target.classList.contains("btn-export")) {
    e.stopPropagation();
    const id      = Number(e.target.dataset.id);
    const session = historyData.find(s => s.id === id);
    if (session) exportSingleCSV(session);
    return;
  }

  // FIX: delete by id bukan index — aman meski filter aktif atau urutan berubah
  if (e.target.classList.contains("btn-delete")) {
    e.stopPropagation();
    const id = Number(e.target.dataset.id);
    if (!confirm("Delete this session?")) return;
    historyData = historyData.filter(s => s.id !== id);
    saveHistory(historyData);
    showToast("Session deleted", "");
    render();
    return;
  }

  // Open detail — simpan id, bukan index
  const card = e.target.closest(".history-card");
  if (card) {
    // FIX: simpan session id bukan realIndex agar tidak salah halaman
    localStorage.setItem(`sem_selected_${uid}`, card.dataset.id);
    window.location.href = "history-detail.html";
  }
});

// ================= SEARCH =================
searchInput.addEventListener("input", () => {
  searchKeyword = searchInput.value.toLowerCase().trim(); render();
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
btnDeleteAll.addEventListener("click", () => {
  if (historyData.length === 0) { showToast("Nothing to delete", "error"); return; }
  if (!confirm("Delete ALL history? This cannot be undone.")) return;
  saveHistory([]); historyData = [];
  showToast("All history deleted", ""); render();
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

// ================= INIT =================
render();