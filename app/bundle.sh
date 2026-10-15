#!/usr/bin/env bash
# Builds dist/cubevox.app and dist/cubevox.app.tar.gz from the Go sources.
#
#   app/bundle.sh                       # dfu-util taken from PATH
#   DFU_UTIL=/path/to/static/dfu-util app/bundle.sh
#
# The binary is universal (arm64 + amd64) when lipo exists, else the host arch only.
# The version comes from `git describe` and is stamped into the binary and Info.plist.
set -euo pipefail

app_dir="$(cd "$(dirname "$0")" && pwd)"
dist="$app_dir/dist"
bundle="$dist/cubevox.app"
macos="$bundle/Contents/MacOS"
version="$(git -C "$app_dir" describe --tags --always)"

rm -rf "$bundle" "$dist/cubevox.app.tar.gz" "$dist/build"
mkdir -p "$macos" "$dist/build"

build() { # $1 = GOARCH, $2 = output
  (cd "$app_dir" && CGO_ENABLED=0 GOOS=darwin GOARCH="$1" \
    go build -trimpath -ldflags "-s -w -X main.version=$version" -o "$2" .)
}

if command -v lipo >/dev/null 2>&1; then
  build arm64 "$dist/build/cubevox-arm64"
  build amd64 "$dist/build/cubevox-amd64"
  lipo -create -output "$macos/cubevox" "$dist/build/cubevox-arm64" "$dist/build/cubevox-amd64"
else
  echo "lipo not found: building $(go env GOARCH) only" >&2
  build "$(go env GOARCH)" "$macos/cubevox"
fi
rm -rf "$dist/build"

dfu="${DFU_UTIL:-$(command -v dfu-util || true)}"
if [ -z "$dfu" ] || [ ! -x "$dfu" ]; then
  echo "dfu-util not found: set DFU_UTIL to a static dfu-util binary" >&2
  exit 1
fi
cp "$dfu" "$macos/dfu-util"
chmod 755 "$macos/dfu-util"
if command -v otool >/dev/null 2>&1 && otool -L "$macos/dfu-util" | awk '/^\t/ {print $1}' | grep -qv -e '^/usr/lib/' -e '^/System/'; then
  echo "warning: $dfu links libraries outside the system; the bundle will not run on other Macs" >&2
fi

cat >"$bundle/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleIdentifier</key><string>com.peterdelecto.cubevox</string>
  <key>CFBundleName</key><string>cubevox</string>
  <key>CFBundleExecutable</key><string>cubevox</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleShortVersionString</key><string>$version</string>
  <key>CFBundleVersion</key><string>$version</string>
  <key>LSUIElement</key><true/>
  <key>LSMinimumSystemVersion</key><string>12.0</string>
</dict>
</plist>
PLIST

if command -v codesign >/dev/null 2>&1; then
  codesign -s - --deep --force "$bundle"
fi

tar -czf "$dist/cubevox.app.tar.gz" -C "$dist" cubevox.app
echo "built $bundle ($version)"
echo "built $dist/cubevox.app.tar.gz"
