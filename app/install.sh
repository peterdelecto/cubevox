#!/bin/sh
# Installs cubevox.app from the latest GitHub release.
#
#   curl -fsSL https://github.com/peterdelecto/cubevox/releases/latest/download/install.sh | sh
#
# Goes to /Applications when writable, else ~/Applications. Replaces an older copy. No sudo.
set -euo pipefail

# CUBEVOX_URL and CUBEVOX_DEST exist so the script can be tested without a release or /Applications.
url="${CUBEVOX_URL:-https://github.com/peterdelecto/cubevox/releases/latest/download/cubevox.app.tar.gz}"

if [ -n "${CUBEVOX_DEST:-}" ]; then
  dest="$CUBEVOX_DEST"
  mkdir -p "$dest"
elif [ -w /Applications ]; then
  dest="/Applications"
else
  dest="$HOME/Applications"
  mkdir -p "$dest"
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

echo "Downloading cubevox..."
curl -fL --progress-bar "$url" -o "$tmp/cubevox.app.tar.gz"
tar -xzf "$tmp/cubevox.app.tar.gz" -C "$tmp"
test -x "$tmp/cubevox.app/Contents/MacOS/cubevox"

# Stop a running copy so the old bundle is not replaced under a live process.
pkill -f "$dest/cubevox.app/Contents/MacOS/cubevox" 2>/dev/null || true

rm -rf "$dest/cubevox.app"
mv "$tmp/cubevox.app" "$dest/cubevox.app"

echo "Installed to $dest/cubevox.app"
echo "Double-click it to open the page."
