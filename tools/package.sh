#!/bin/bash
# Builds a self-contained cubevox-proto.app for Apple Silicon and zips it into dist/.
# GLFW is copied into the bundle so the app runs on a Mac without Homebrew.
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
build="$root/build"
dist="$root/dist"
app="$dist/cubevox-proto.app"
zip="$dist/cubevox-proto.zip"

cmake -S "$root" -B "$build" -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "$build" -j --target cubevox-proto >/dev/null

rm -rf "$dist"
mkdir -p "$dist"
ditto "$build/cubevox-proto.app" "$app"

bin="$app/Contents/MacOS/cubevox-proto"
frameworks="$app/Contents/Frameworks"
mkdir -p "$frameworks"

# Copy every non-system dylib into the bundle and point the binary at the copy.
otool -L "$bin" | awk 'NR > 1 { print $1 }' | grep -v -E '^(/System/|/usr/lib/)' | while read -r lib; do
  name="$(basename "$lib")"
  cp -L "$lib" "$frameworks/$name"
  chmod u+w "$frameworks/$name"
  install_name_tool -id "@rpath/$name" "$frameworks/$name"
  install_name_tool -change "$lib" "@rpath/$name" "$bin"
done
install_name_tool -add_rpath "@executable_path/../Frameworks" "$bin" 2>/dev/null || true

# Ad-hoc signature; Apple Silicon refuses to run unsigned code.
codesign --force --deep --sign - "$app"

# Fail if anything outside the bundle or the OS is still referenced.
if otool -L "$bin" "$frameworks"/*.dylib | grep -E '^\s+/(opt|usr/local)/'; then
  echo "error: bundle still references Homebrew" >&2
  exit 1
fi

(cd "$dist" && ditto -c -k --keepParent cubevox-proto.app "$zip")
echo "built $zip ($(du -h "$zip" | cut -f1))"
