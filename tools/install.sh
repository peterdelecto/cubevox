#!/usr/bin/env bash
# Installs the cubevox emulator into a folder on the Desktop.
#
#   curl -fsSL https://raw.githubusercontent.com/peterdelecto/cubevox/main/tools/install.sh | bash
#
# The command never changes. tools/publish_emulator.sh replaces the zip on the "emulator" release, and
# running the command again replaces the app and the shipped clips in place and leaves any other clip
# in the samples folder alone.
set -euo pipefail

url="https://github.com/peterdelecto/cubevox/releases/download/emulator/cubevox-emulator.zip"

name="cubevox emulator"
dest="$HOME/Desktop/$name"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

echo "Downloading $name..."
curl -fL --progress-bar "$url" -o "$tmp/emulator.zip"
ditto -x -k "$tmp/emulator.zip" "$tmp/x"

mkdir -p "$dest"
rm -rf "$dest/$name.app"
ditto "$tmp/x/$name" "$dest"
xattr -dr com.apple.quarantine "$dest" 2>/dev/null || true

echo "Installed to $dest"
open "$dest"
