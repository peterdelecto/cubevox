#!/usr/bin/env bash
# Installs the Vox Processor Prototype into a folder on the Desktop.
#
#   curl -fsSL https://raw.githubusercontent.com/peterdelecto/cubevox/main/tools/install.sh | bash
#
# tools/package.sh builds the zip and a person attaches it to a release. The workflows attach
# firmware files only, so the latest release does not carry the zip, and the URL below names the
# newest release that does. Running the script again replaces the app and the samples in place.
set -euo pipefail

url="https://github.com/peterdelecto/cubevox/releases/download/v2.00.04/vox-processor-prototype.zip"
name="Vox Processor Prototype"
dest="$HOME/Desktop/$name"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

echo "Downloading $name..."
curl -fL --progress-bar "$url" -o "$tmp/vox.zip"
ditto -x -k "$tmp/vox.zip" "$tmp/x"

mkdir -p "$dest"
rm -rf "$dest/$name.app"
ditto "$tmp/x/$name" "$dest"
xattr -dr com.apple.quarantine "$dest" 2>/dev/null || true

echo "Installed to $dest"
open "$dest"
