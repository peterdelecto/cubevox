#!/usr/bin/env bash
# Builds a universal (arm64 + x86_64) dfu-util with libusb linked in statically.
#
#   app/dfu-util-static.sh [output]        # default: app/dist/dfu-util
#
# Needs clang, make, lipo, pkg-config and curl: all on GitHub's macos-14 runner. The result
# links only system libraries and frameworks, so it runs on any Mac the app supports.
# bundle.sh picks it up through DFU_UTIL.
set -euo pipefail

libusb_version=1.0.27
libusb_sha256=ffaa41d741a8a3bee244ac8e54a72ea05bf2879663c098c82fc5757853441575
dfu_version=0.11
dfu_sha256=b4b53ba21a82ef7e3d4c47df2952adf5fa494f499b6b0b57c58c5d04ae8ff19e
min_macos=12.0

app_dir="$(cd "$(dirname "$0")" && pwd)"
out="${1:-$app_dir/dist/dfu-util}"
mkdir -p "$(dirname "$out")"
out="$(cd "$(dirname "$out")" && pwd)/$(basename "$out")"

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
jobs="$(sysctl -n hw.ncpu)"

fetch() { # $1 = url, $2 = sha256, $3 = file name
  curl -fsSL "$1" -o "$work/$3"
  echo "$2  $work/$3" | shasum -a 256 -c -
}

fetch "https://github.com/libusb/libusb/releases/download/v$libusb_version/libusb-$libusb_version.tar.bz2" \
  "$libusb_sha256" libusb.tar.bz2
fetch "https://dfu-util.sourceforge.net/releases/dfu-util-$dfu_version.tar.gz" \
  "$dfu_sha256" dfu-util.tar.gz
mkdir "$work/libusb-src" "$work/dfu-src"
tar -xjf "$work/libusb.tar.bz2" -C "$work/libusb-src" --strip-components=1
tar -xzf "$work/dfu-util.tar.gz" -C "$work/dfu-src" --strip-components=1

build_arch() { # $1 = arm64 | x86_64
  local arch="$1" prefix="$work/prefix-$1" host
  case "$arch" in
    arm64) host=aarch64-apple-darwin ;;
    x86_64) host=x86_64-apple-darwin ;;
  esac
  local cc="clang -arch $arch -mmacosx-version-min=$min_macos"

  # Out-of-tree builds keep the two arches from sharing object files.
  mkdir "$work/libusb-$arch" "$work/dfu-$arch"
  (cd "$work/libusb-$arch" &&
    "$work/libusb-src/configure" --host="$host" --prefix="$prefix" \
      --disable-shared --enable-static CC="$cc" >/dev/null &&
    make -j "$jobs" >/dev/null && make install >/dev/null)

  # USB_CFLAGS / USB_LIBS skip pkg-config so the static archive and the macOS frameworks
  # libusb needs are named explicitly.
  (cd "$work/dfu-$arch" &&
    "$work/dfu-src/configure" --host="$host" CC="$cc" \
      USB_CFLAGS="-I$prefix/include/libusb-1.0" \
      USB_LIBS="$prefix/lib/libusb-1.0.a -framework IOKit -framework CoreFoundation -framework Security" \
      >/dev/null &&
    make -j "$jobs" >/dev/null)
  strip -x "$work/dfu-$arch/src/dfu-util"
}

build_arch arm64
build_arch x86_64
lipo -create -output "$out" "$work/dfu-arm64/src/dfu-util" "$work/dfu-x86_64/src/dfu-util"

# Fail the build if anything but system libraries got linked in.
for arch in arm64 x86_64; do
  if otool -arch "$arch" -L "$out" | tail -n +2 | grep -qv -e '/usr/lib/' -e '/System/'; then
    echo "dfu-util ($arch) links a non-system library:" >&2
    otool -arch "$arch" -L "$out" >&2
    exit 1
  fi
done

"$out" --version | head -1
lipo -info "$out"
echo "built $out"
