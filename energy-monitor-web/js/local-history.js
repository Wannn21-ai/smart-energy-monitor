import { auth, db, ref, get, set, DEVICE_ID } from "./firebase-config.js";

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

function formatDuration(value) {
  if (typeof value === "string" && value.includes(":")) return value;
  const secs = Math.max(0, Number(value || 0));
  const h = String(Math.floor(secs / 3600)).padStart(2, "0");
  const m = String(Math.floor((secs % 3600) / 60)).padStart(2, "0");
  const s = String(Math.floor(secs % 60)).padStart(2, "0");
  return `${h}:${m}:${s}`;
}

function historyIdentity(session) {
  if (session.id) return `sid:${session.id}`;
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
    id: val.id || val.sessionId || key,
    sessionId: val.sessionId || val.id || key,
    duration: formatDuration(val.durationSec ?? val.duration),
    cost: val.costText || formatCost(val.cost),
    timestamp: Number(val.timestamp || val.endTime || 0)
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
      id: entry.id || entry.sessionId || String(keySeed),
      sessionId: entry.sessionId || entry.id || String(keySeed),
      name: entry.name || "Device",
      duration: formatDuration(entry.durationSec ?? entry.duration),
      power: Number(entry.power || 0),
      energy: Number(entry.energy ?? entry.kwh ?? 0),
      cost: entry.costText || formatCost(entry.cost),
      date: entry.date || formatDate(timestamp),
      timestamp,
      pendingSync: entry.pendingSync !== false
    };
  });
}

async function getEspHistoryUrl(uid) {
  try {
    const snap = await get(ref(db, `devices/${DEVICE_ID}/live/system`));
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

async function getDeviceId(uid) {
  try {
    const snap = await get(ref(db, `devices/${DEVICE_ID}/live/system`));
    const sys = snap.exists() ? snap.val() : {};
    const deviceId = sys.deviceId || sys.deviceID || "";
    if (deviceId) {
      localStorage.setItem(`sem_device_id_${uid}`, deviceId);
      return deviceId;
    }
  } catch {}

  return localStorage.getItem(`sem_device_id_${uid}`) || DEVICE_ID;
}

function normalizeCompletedSession(session, sessionId, deviceId, uid) {
  const timestamp = Number(session.timestamp || session.endTime || Date.now());
  const sourcePath = `/devices/${deviceId}/completedSessions/${sessionId}`;
  return {
    ...session,
    id: session.id || sessionId,
    sessionId: session.sessionId || session.id || sessionId,
    deviceId: session.deviceId || deviceId,
    name: session.name || "Device",
    duration: formatDuration(session.durationSec ?? session.duration),
    power: Number(session.power || 0),
    energy: Number(session.energy || 0),
    cost: Number(session.cost || 0),
    costText: session.costText || formatCost(session.cost),
    date: session.date || formatDate(timestamp),
    timestamp,
    syncStatus: "SYNCED",
    pendingSync: false,
    createdFrom: session.createdFrom || "ESP32",
    copiedAt: Date.now(),
    ownerUid: uid,
    sourcePath
  };
}

const activeImports = new Map();

async function countUserHistory(uid) {
  const snap = await get(ref(db, `users/${uid}/history`));
  return snap.exists() ? Object.keys(snap.val() || {}).length : 0;
}

async function importCompletedSessions(uid) {
  const deviceId = DEVICE_ID;
  console.log("[History Import] currentUser.uid", uid);
  try {
    const queueSnap = await get(ref(db, `devices/${deviceId}/completedSessions`));
    const sessions = queueSnap.val() || {};
    const entries = Object.entries(sessions);
    console.log("[History Import] completedSessions count", entries.length);

    let copied = 0;
    for (const [key, session] of Object.entries(sessions)) {
      if (!session || typeof session !== "object") continue;
      const sessionId = String(session.id || session.sessionId || key);
      if (!sessionId) continue;

      const userHistoryRef = ref(db, `users/${uid}/history/${sessionId}`);
      const existing = await get(userHistoryRef);
      if (existing.exists()) {
        console.log("[History Import] skipped duplicate session id", sessionId);
        continue;
      }

      await set(userHistoryRef, normalizeCompletedSession(session, sessionId, deviceId, uid));
      console.log("[History Import] copied session id", sessionId);
      copied++;
    }

    const userHistoryCount = await countUserHistory(uid);
    console.log("[History Import] user history count after import", userHistoryCount);
    return copied;
  } catch (e) {
    console.error("[History Import] Firebase error:", e);
    return 0;
  }
}

export async function importCompletedSessionsForCurrentUser(user = auth.currentUser) {
  const uid = typeof user === "string" ? user : user?.uid;
  if (!uid) {
    console.warn("[History Import] skipped: Firebase Auth currentUser is not available");
    return 0;
  }

  if (activeImports.has(uid)) return activeImports.get(uid);
  const task = importCompletedSessions(uid).finally(() => activeImports.delete(uid));
  activeImports.set(uid, task);
  return task;
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
  await importCompletedSessionsForCurrentUser(uid);

  const [local, firebase] = await Promise.all([
    fetchLocalHistory(uid),
    fetchFirebaseHistory(uid)
  ]);

  const merged = new Map();
  local.forEach(session => merged.set(historyIdentity(session), session));
  firebase.forEach(session => {
    const key = historyIdentity(session);
    merged.set(key, session);
  });

  return [...merged.values()].sort((a, b) => (b.timestamp || 0) - (a.timestamp || 0));
}
