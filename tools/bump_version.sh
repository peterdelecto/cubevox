#!/bin/bash
# Increments the last field of VERSION.txt (0.01.00 -> 0.01.01; 0.01.99 -> 0.02.00).
# Run once per change, before rebuilding the app.
set -euo pipefail
file="$(cd "$(dirname "$0")/.." && pwd)/VERSION.txt"
IFS=. read -r major minor patch < "$file"
patch=$((10#$patch + 1))
if [ "$patch" -gt 99 ]; then patch=0; minor=$((10#$minor + 1)); fi
printf '%s.%02d.%02d\n' "$major" "$minor" "$patch" > "$file"
cat "$file"
