// ================================================
// firebase-config.js
// Konfigurasi Firebase — nilai diambil dari meta tag
// yang di-inject oleh Netlify Environment Variables.
//
// Setup di Netlify:
//   Site Settings → Environment Variables → tambahkan:
//   FIREBASE_API_KEY, FIREBASE_AUTH_DOMAIN,
//   FIREBASE_DATABASE_URL, FIREBASE_PROJECT_ID,
//   FIREBASE_STORAGE_BUCKET, FIREBASE_MESSAGING_SENDER_ID,
//   FIREBASE_APP_ID
//
// Lalu di netlify.toml aktifkan injeksi via _headers
// atau gunakan file netlify/edge-functions (lihat README).
// Cara termudah: isi langsung di sini untuk deployment
// pribadi, tapi JANGAN commit credentials.h ke GitHub.
// ================================================

import { initializeApp } from "https://www.gstatic.com/firebasejs/10.12.2/firebase-app.js";
import { getAuth }        from "https://www.gstatic.com/firebasejs/10.12.2/firebase-auth.js";
import {
  getDatabase, ref, onValue, set, push, remove, get
} from "https://www.gstatic.com/firebasejs/10.12.2/firebase-database.js";

// ── Baca config dari meta tag (di-inject saat build) ──────────────────────────
// Jika meta tag tidak ada (dev lokal), fallback ke object kosong agar error
// terlihat jelas di console daripada crash diam-diam.
function getMeta(name) {
  const el = document.querySelector(`meta[name="firebase-${name}"]`);
  if (!el) {
    console.warn(`[firebase-config] Meta tag "firebase-${name}" tidak ditemukan.`);
    return "";
  }
  return el.content;
}

const firebaseConfig = {
  apiKey:            getMeta("api-key"),
  authDomain:        getMeta("auth-domain"),
  databaseURL:       getMeta("database-url"),
  projectId:         getMeta("project-id"),
  storageBucket:     getMeta("storage-bucket"),
  messagingSenderId: getMeta("messaging-sender-id"),
  appId:             getMeta("app-id"),
};

const app  = initializeApp(firebaseConfig);
const auth = getAuth(app);
const db   = getDatabase(app);

export { auth, db, ref, onValue, set, push, remove, get };