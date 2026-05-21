#!/bin/bash
# ================================================================
# inject-env.sh
# Inject Firebase environment variables ke semua file HTML
# Dijalankan saat build di Vercel
# ================================================================

set -e  # stop jika ada error

echo "🔧 Injecting environment variables..."

for file in energy-monitor-web/*.html; do
  echo "  → Processing: $file"
  sed -i \
    -e "s|NETLIFY_ENV_FIREBASE_API_KEY|${FIREBASE_API_KEY}|g" \
    -e "s|NETLIFY_ENV_FIREBASE_AUTH_DOMAIN|${FIREBASE_AUTH_DOMAIN}|g" \
    -e "s|NETLIFY_ENV_FIREBASE_DATABASE_URL|${FIREBASE_DATABASE_URL}|g" \
    -e "s|NETLIFY_ENV_FIREBASE_PROJECT_ID|${FIREBASE_PROJECT_ID}|g" \
    -e "s|NETLIFY_ENV_FIREBASE_STORAGE_BUCKET|${FIREBASE_STORAGE_BUCKET}|g" \
    -e "s|NETLIFY_ENV_FIREBASE_MESSAGING_SENDER_ID|${FIREBASE_MESSAGING_SENDER_ID}|g" \
    -e "s|NETLIFY_ENV_FIREBASE_APP_ID|${FIREBASE_APP_ID}|g" \
    "$file"
done

echo "✅ Done! All env variables injected."
