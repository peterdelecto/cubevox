#!/usr/bin/env bash
# Builds the zip Adam installs with install.sh: the emulator as a self-contained
# Apple silicon app plus the clips he can play, in dist/cubevox-emulator.zip.
#
#   tools/package.sh
#
# The zip holds one folder, "cubevox emulator", with the app and a samples/
# folder beside it (firmware/host/source.cpp looks for samples/ next to the app).
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"

# Clips shipped in samples/, one file name per line, each taken from the repo's
# samples/ folder. The emulator opens adam_vox_sample3_5s.wav on its own; Adam
# reaches the others through File > Open (Cmd-O).
sample_dir="$root/samples"
samples=(
  "adam_vox_sample3_5s.wav"
)

# The headless check also opens this clip. It joins the unpacked copy under
# test only and is not shipped.
check_clip="adam_vox_sample_3s.wav"
name="cubevox emulator"
exe="cubevox-emulator"
build="$root/build-release"
stage="$root/dist/$name"
zip="$root/dist/cubevox-emulator.zip"
target="11.0"  # the file menu uses UTType, which starts at macOS 11

for s in "${samples[@]}" "$check_clip"; do
  [[ -f "$sample_dir/$s" ]] || { echo "[ERROR] missing $sample_dir/$s" >&2; exit 1; }
done

cmake -S "$root" -B "$build" -DCMAKE_BUILD_TYPE=Release -DCUBEVOX_BUNDLE_GLFW=ON \
  -DCMAKE_OSX_ARCHITECTURES="arm64" -DCMAKE_OSX_DEPLOYMENT_TARGET="$target" >/dev/null
cmake --build "$build" --target "$exe" -j >/dev/null

rm -rf "$root/dist"
mkdir -p "$stage/samples"
ditto "$build/$exe.app" "$stage/$name.app"
for s in "${samples[@]}"; do cp "$sample_dir/$s" "$stage/samples/"; done
codesign --force --deep --sign - "$stage/$name.app"

# The app must not reach outside the system for libraries: no links, no
# build-machine rpaths (dyld searches those first).
bin="$stage/$name.app/Contents/MacOS/$exe"
if otool -L "$bin" | grep -E '^\s+/(opt|usr/local)/' ||
   otool -l "$bin" | grep -A2 LC_RPATH | grep -E 'path /(opt|usr/local)/'; then
  echo "[ERROR] the app still references Homebrew" >&2
  exit 1
fi
archs="$(lipo -archs "$bin")"
[[ "$archs" == "arm64" ]] || { echo "[ERROR] expected arm64 only, the app has: $archs" >&2; exit 1; }

(cd "$root/dist" && ditto -c -k --norsrc --keepParent "$name" "$zip")

# The unpacked copy must run alone: unzipped into a fresh directory outside the
# repo, started from a directory with no samples/, loading no Homebrew library,
# and playing the clips from the folder it came in.
scratch="$(mktemp -d "${TMPDIR:-/tmp}/cubevox-package.XXXXXX")"
trap 'rm -rf "$scratch"' EXIT
mkdir "$scratch/unpacked" "$scratch/cwd"
ditto -x -k "$zip" "$scratch/unpacked"
unpacked="$scratch/unpacked/$name"
codesign --verify --deep --strict "$unpacked/$name.app"
cp "$sample_dir/$check_clip" "$unpacked/samples/"

log="$scratch/check.log"
(cd "$scratch/cwd" && DYLD_PRINT_LIBRARIES=1 "$unpacked/$name.app/Contents/MacOS/$exe" --check) >"$log" 2>&1 ||
  { cat "$log" >&2; echo "[ERROR] the unpacked app failed its check" >&2; exit 1; }
if grep -E 'dyld.*/(opt|usr/local)/' "$log"; then
  echo "[ERROR] the app loads a Homebrew library at run time" >&2
  exit 1
fi
grep -F "clip: Adam's clips from samples/, $unpacked/" "$log" >/dev/null ||
  { cat "$log" >&2; echo "[ERROR] the unpacked app did not play the clips beside it" >&2; exit 1; }
grep -x "emulator check: ok" "$log"

echo "$zip ($archs, $(du -h "$zip" | cut -f1), macOS $target+)"
