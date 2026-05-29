import { db, ref, get } from "./firebase-config.js";

const LOCAL_FETCH_TIMEOUT_MS = 1500;

function toTimestampMs(value) {
  const n = Number(value || 0);
  if (!Number.isFinite(n) || n <= 0) return 0;
  return n > 1000000000000 ? n : n * 1000;
}

function formatDate(ms) {
  if (!ms) return "-";
  return new Date(ms).toLocaleDateString("id-ID");
}

function formatCost(value) {
  if (typeof value === "string") return value;
  const n = Number(value || 0);
  return `Rp ${Math.round(n).toLocaleString("id-ID")}`;
}

function historyIdentity(session) {
  if (session.sessionId) return `sid:${session.sessionId}`;
  if (session.timestamp) return `ts:${Math.round(session.timestamp / 1000)}:${session.name || ""}`;
  return `key:${session._key || Math.random().toString(36).slice(2)}`;
}

function normalizeFirebaseHistory(raw) {
  if (!raw) return [];
  return Object.entries(raw).map(([key, val]) => ({
    ...val,
    _key: key,
    _source: "firebase",
    timestamp: Number(val.timestamp || 0)
  }));
}

function normalizeLocalHistory(entries) {
  if (!Array.isArray(entries)) return [];
  return entries.map((entry, index) => {
    const timestamp = toTimestampMs(entry.timestamp) ||
      toTimestampMs(entry.end_ts) ||
      toTimestampMs(entry.start_ts);
    const keySeed = entry.sessionId || entry.end_ts || entry.start_ts || index;
    return {
      ...entry,
      _key: `local_${keySeed}`,
      _source: "local",
      name: entry.name || "Device",
      duration: entry.duration || "00:00:00",
      power: Number(entry.power || 0),
      energy: Number(entry.energy ?? entry.kwh ?? 0),
      cost: formatCost(entry.cost),
      date: entry.date || formatDate(timestamp),
      timestamp,
      pendingSync: entry.pendingSync !== false
    };
  });
}

async function getEspHistoryUrl(uid) {
  try {
    const snap = await get(ref(db, "live/system"));
    const sys = snap.exists() ? snap.val() : {};
    const ip = sys.ip || sys.localIp || "";
    if (ip) {
      localStorage.setItem(`sem_esp_ip_${uid}`, ip);
      return `http://${ip}/history`;
    }
  } catch {}

  const cachedIp = localStorage.getItem(`sem_esp_ip_${uid}`);
  return cachedIp ? `http://${cachedIp}/history` : "";
}

async function fetchLocalHistory(uid) {
  const url = await getEspHistoryUrl(uid);
  if (!url) return [];

  const controller = new AbortController();
  const timer = setTimeout(() => controller.abort(), LOCAL_FETCH_TIMEOUT_MS);
  try {
    const res = await fetch(url, { cache: "no-store", signal: controller.signal });
    if (!res.ok) return [];
    return normalizeLocalHistory(await res.json());
  } catch (e) {
    console.warn("[History] Local ESP32 history unavailable:", e?.message || e);
    return [];
  } finally {
    clearTimeout(timer);
  }
}

async function fetchFirebaseHistory(uid) {
  try {
    const snap = await get(ref(db, `users/${uid}/history`));
    return snap.exists() ? normalizeFirebaseHistory(snap.val()) : [];
  } catch (e) {
    console.warn("[History] Firebase history unavailable:", e?.message || e);
    return [];
  }
}

export async function loadDeviceHistory(uid) {
  const [local, firebase] = await Promise.all([
    fetchLocalHistory(uid),
    fetchFirebaseHistory(uid)
  ]);

  const merged = new Map();
  local.forEach(session => merged.set(historyIdentity(session), session));
  firebase.forEach(session => {
    const key = historyIdentity(session);
    if (!merged.has(key)) merged.set(key, session);
  });

  return [...merged.values()].sort((a, b) => (b.timestamp || 0) - (a.timestamp || 0));
}
