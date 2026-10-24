#!/bin/bash
# Increments the last field of VERSION.txt (0.01.00 -> 0.01.01; 0.01.99 -> 0.02.00).
# Run once per change, before rebuilding the app.
set -euo pipefail
file="$(cd "$(dirname "$0")/.." && pwd)/VERSION.txt"
read -r current < "$file"
if ! [[ "$current" =~ ^[0-9]+\.[0-9][0-9]\.[0-9][0-9]$ ]]; then
  echo "[ERROR] VERSION.txt is '$current'; it must look like 2.00.10"
  exit 1
fi
IFS=. read -r major minor patch <<< "$current"
# The fields carry leading zeros, which bash would read as octal without the 10# prefix.
minor=$((10#$minor))
patch=$((10#$patch + 1))
if [ "$patch" -gt 99 ]; then patch=0; minor=$((minor + 1)); fi
if [ "$minor" -gt 99 ]; then
  echo "[ERROR] $current has no next version; each field after the major holds two digits"
  exit 1
fi
next="$(printf '%s.%02d.%02d' "$major" "$minor" "$patch")"
echo "$next" > "$file"
echo "$next"
