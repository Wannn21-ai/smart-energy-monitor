// ================================================
// firebase-config.js
// Konfigurasi Firebase — nilai diambil dari meta tag
// yang di-inject oleh Netlify Environment Variables.
// ================================================
import { initializeApp }  from "https://www.gstatic.com/firebasejs/10.12.2/firebase-app.js";
import { getAuth }        from "https://www.gstatic.com/firebasejs/10.12.2/firebase-auth.js";
import {
  getDatabase, ref, onValue, set, push, remove, get, update
} from "https://www.gstatic.com/firebasejs/10.12.2/firebase-database.js";

function getMeta(name) {
  const el = document.querySelector(`meta[name="firebase-${name}"]`);
  if (!el) { console.warn(`[firebase-config] Meta tag "firebase-${name}" tidak ditemukan.`); return ""; }
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

// PERBAIKAN: Pastikan `update` ikut di-export
export { auth, db, ref, onValue, set, push, remove, get, update };