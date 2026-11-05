#!/usr/bin/env bash
# Builds the emulator zip and puts it on the release that install.sh downloads from, so Adam's curl
# command picks up the new copy the next time he runs it.
#
#   tools/publish_emulator.sh
#
# The release is tagged "emulator" and marked pre-release, so the Mac app's latest-release check and the
# firmware releases (tags v*) never see it. Each run replaces the zip on it.
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
repo="peterdelecto/cubevox"
tag="emulator"
zip="$root/dist/cubevox-emulator.zip"

"$root/tools/package.sh"

version="$(tr -d '[:space:]' < "$root/VERSION.txt")"
notes="cubevox emulator v$version. Install or update with:
curl -fsSL https://raw.githubusercontent.com/$repo/main/tools/install.sh | bash"

if gh release view "$tag" -R "$repo" >/dev/null 2>&1; then
  gh release upload "$tag" "$zip" -R "$repo" --clobber
  gh release edit "$tag" -R "$repo" --notes "$notes"
else
  gh release create "$tag" "$zip" -R "$repo" --prerelease --title "cubevox emulator" --notes "$notes" \
    --target main
fi

echo "[publish] v$version on https://github.com/$repo/releases/tag/$tag"
