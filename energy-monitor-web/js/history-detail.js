import { requireAuth, renderShell, fillUserInfo, showToast, startStatusWatcher } from "./auth-guard.js";
import { db, ref, get } from "./firebase-config.js";

const user = await requireAuth();
renderShell("history", "SESSION DETAIL");
fillUserInfo(user);
startStatusWatcher();
const uid = user.uid;

// Ambil key yang disimpan oleh history.js
const selectedKey = sessionStorage.getItem(`sem_selected_key_${uid}`);

const detailGrid  = document.getElementById("detail-grid");
const detailEmpty = document.getElementById("detail-empty");
const nameEl      = document.getElementById("detail-device-name");
const dateEl      = document.getElementById("detail-date");
const btnExport   = document.getElementById("btn-export");

if (!selectedKey) {
  detailEmpty.style.display = "flex";
  btnExport.style.display   = "none";
} else {
  try {
    const snap = await get(ref(db, `users/${uid}/history/${selectedKey}`));
    if (!snap.exists()) {
      detailEmpty.style.display = "flex";
      btnExport.style.display   = "none";
    } else {
      const session = snap.val();
      nameEl.textContent = session.name;
      dateEl.textContent = `Recorded on ${session.date}`;
      const fields = [
        { label: "Duration",        value: session.duration,  accent: "var(--cyan)"  },
        { label: "Power",           value: `${session.power} W`, accent: "var(--amber)" },
        { label: "Energy",          value: `${session.energy} kWh`, accent: "var(--green)" },
        { label: "Estimated Cost",  value: session.cost,      accent: "var(--cyan)"  },
        { label: "Date",            value: session.date,      accent: ""             },
      ];
      fields.forEach(f => {
        const item = document.createElement("div");
        item.className = "detail-item";
        item.innerHTML = `
          <div class="detail-item-label">${f.label}</div>
          <div class="detail-item-value" style="${f.accent ? `color:${f.accent}` : ""}">${f.value}</div>`;
        detailGrid.appendChild(item);
      });
      btnExport.addEventListener("click", () => {
        const csv = `Name,Duration,Power (W),Energy (kWh),Cost,Date\n${session.name},${session.duration},${session.power},${session.energy},${session.cost},${session.date}`;
        const blob = new Blob([csv], { type: "text/csv" });
        const url  = URL.createObjectURL(blob);
        const a    = document.createElement("a");
        a.href = url; a.download = `${session.name}_session.csv`; a.click();
        URL.revokeObjectURL(url);
        showToast("Exported ✓", "success");
      });
    }
  } catch {
    detailEmpty.style.display = "flex";
    btnExport.style.display   = "none";
  }
}