#!/usr/bin/env bash
# Builds the zip Adam installs with install.sh: a universal, self-contained app
# plus the sample loops, in dist/vox-processor-prototype.zip.
#
#   tools/package.sh            # samples from ~/Music/samples
#   SAMPLE_DIR=/path tools/package.sh
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
sample_dir="${SAMPLE_DIR:-$HOME/Music/samples}"
samples=(
  "Adam Vox 01.wav"
  "Adam Vox 02.wav"
  "Adam Vox A-sharp Autone.wav"
  "Adam Vox C-sharp Autotune.wav"
  "Adam Vox Dist Delay.wav"
  "Adam Vox Mellow Dist.wav"
  "Feeling the Flow 108.wav"
  "Gospel Improv 01 123.wav"
)
name="Vox Processor Prototype"
build="$root/build-release"
stage="$root/dist/$name"
zip="$root/dist/vox-processor-prototype.zip"

cmake -S "$root" -B "$build" -DCMAKE_BUILD_TYPE=Release -DCUBEVOX_BUNDLE_GLFW=ON \
  -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0 >/dev/null
cmake --build "$build" --target cubevox-proto -j >/dev/null

rm -rf "$root/dist"
mkdir -p "$stage/Samples"
ditto "$build/cubevox-proto.app" "$stage/$name.app"
for s in "${samples[@]}"; do cp "$sample_dir/$s" "$stage/Samples/"; done
codesign --force --deep --sign - "$stage/$name.app"

# The app must not reach outside the system for libraries: no links, no
# build-machine rpaths (dyld searches those first), nothing loaded at run time.
bin="$stage/$name.app/Contents/MacOS/cubevox-proto"
if otool -L "$bin" | grep -E '^\s+/(opt|usr/local)/' ||
   otool -l "$bin" | grep -A2 LC_RPATH | grep -E 'path /(opt|usr/local)/'; then
  echo "[ERROR] the app still references Homebrew" >&2
  exit 1
fi
if DYLD_PRINT_LIBRARIES=1 "$bin" --layout 2>&1 | grep -E 'dyld.*/(opt|usr/local)/'; then
  echo "[ERROR] the app loads a Homebrew library at run time" >&2
  exit 1
fi

(cd "$root/dist" && ditto -c -k --keepParent "$name" "$zip")
echo "$zip ($(lipo -archs "$bin"), $(du -h "$zip" | cut -f1))"
