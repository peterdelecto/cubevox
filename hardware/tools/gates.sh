#!/usr/bin/env bash
# Layout gates. Usage: hardware/tools/gates.sh [board.kicad_pcb]  (run from repo root)
# One line per gate, exit 1 on any FAIL.
set -u

BOARD="${1:-hardware/cubevox.kicad_pcb}"

# ---- baseline constants (edit here) ----
DRC_EXPECT="{'lib_footprint_mismatch': 9}"
UNCONNECTED_EXPECT=0
# Registered corner exceptions, x,y at 0.01 mm:
#   (111.4,35.55) (111.4,36.165) GND E0120 | (124.6,107.65) 3V3 E0147 | (115.75,37.25) USB_DM E0148
CORNER_ALLOW="111.40,35.55 111.40,36.17 111.40,36.16 124.60,107.65 115.75,37.25"
ANGLE_TOL_DEG=0.05
DIAG_MAX_MM=20

KICAD_PY=/Applications/KiCad/KiCad.app/Contents/Frameworks/Python.framework/Versions/Current/bin/python3
KICAD_CLI=/Applications/KiCad/KiCad.app/Contents/MacOS/kicad-cli
TOOLS=hardware/tools

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
FAILS=0

report() { # status name detail
  printf '%s %s: %s\n' "$1" "$2" "$3"
  [ "$1" = PASS ] || FAILS=$((FAILS + 1))
}

# 1. DRC with schematic parity
"$KICAD_CLI" pcb drc --schematic-parity --format json --severity-all \
  --output "$TMP/drc.json" "$BOARD" >"$TMP/drc.log" 2>&1
if [ -s "$TMP/drc.json" ]; then
  "$KICAD_PY" -I - "$TMP/drc.json" >"$TMP/drc.txt" <<'PY'
import json, sys, collections
d = json.load(open(sys.argv[1]))
c = collections.Counter(v["type"] for v in d.get("violations", []))
print(dict(sorted(c.items())))
print(len(d.get("unconnected_items", [])))
PY
  counts="$(sed -n 1p "$TMP/drc.txt")"
  unc="$(sed -n 2p "$TMP/drc.txt")"
  if [ "$counts" = "$DRC_EXPECT" ] && [ "$unc" = "$UNCONNECTED_EXPECT" ]; then
    report PASS drc "$counts unconnected $unc"
  else
    report FAIL drc "$counts unconnected $unc (expected $DRC_EXPECT unconnected $UNCONNECTED_EXPECT)"
  fi
else
  report FAIL drc "no DRC output: $(head -c 200 "$TMP/drc.log")"
fi

# 2. strict via audit (the script hardcodes its board path, so substitute the argument)
sed "s|^p=.*|p=sys.argv[1]|" "$TOOLS/via_audit_strict.py" >"$TMP/via_audit.py"
out="$("$KICAD_PY" -I "$TMP/via_audit.py" "$BOARD" 2>&1)"
line="$(printf '%s\n' "$out" | grep -m1 'TRUE ORPHANS:')"
if [ "$line" = "TRUE ORPHANS: 0" ]; then report PASS via_audit_strict "$line"
else report FAIL via_audit_strict "${line:-no result: $(printf '%s' "$out" | tail -n 2)}"; fi

# 3. route audit, section 1 (stray traces)
out="$("$KICAD_PY" -I "$TOOLS/route_audit.py" "$BOARD" 2>&1)"
line="$(printf '%s\n' "$out" | grep -m1 '^  total ')"
line="${line#  }"
if [ "$line" = "total 0" ]; then report PASS route_audit "stray traces $line"
else report FAIL route_audit "${line:-no result: $(printf '%s' "$out" | tail -n 2)}"; fi

# 4. pad entry
out="$("$KICAD_PY" -I "$TOOLS/pad_entry_check.py" "$BOARD" 2>&1)"
line="$(printf '%s\n' "$out" | grep -m1 'pad entry violations:')"
if [ "$line" = "pad entry violations: 0" ]; then report PASS pad_entry "$line"
else report FAIL pad_entry "${line:-no result: $(printf '%s' "$out" | tail -n 2)}"; fi

# 5. right-angle corners, minus registered exceptions
out="$("$KICAD_PY" -I "$TOOLS/corner_check.py" "$BOARD" 2>&1)"
if printf '%s\n' "$out" | grep -q 'right-angle corners:'; then
  bad=""; n=0
  while IFS= read -r l; do
    pt="$(printf '%s' "$l" | sed -n 's/.*(\([0-9.-]*\), *\([0-9.-]*\)).*/\1,\2/p')"
    [ -n "$pt" ] || continue
    n=$((n + 1))
    key="$(printf '%.2f,%.2f' "${pt%,*}" "${pt#*,}")"
    case " $CORNER_ALLOW " in *" $key "*) ;; *) bad="$bad ($pt)";; esac
  done < <(printf '%s\n' "$out" | grep -E '^  .*\([0-9.-]+, [0-9.-]+\)')
  if [ -z "$bad" ]; then report PASS corner_check "$n corners, all registered"
  else report FAIL corner_check "unregistered:$bad"; fi
else
  report FAIL corner_check "no result: $(printf '%s' "$out" | tail -n 2)"
fi

# 6. angle discipline
out="$("$KICAD_PY" -I - "$BOARD" "$ANGLE_TOL_DEG" "$DIAG_MAX_MM" 2>/dev/null <<'PY'
import sys, math, pcbnew
b = pcbnew.LoadBoard(sys.argv[1])
tol, dmax = float(sys.argv[2]), float(sys.argv[3])
off, long_diag = [], []
for t in b.GetTracks():
    if t.GetClass() != "PCB_TRACK":
        continue
    dx, dy = (t.GetEnd().x - t.GetStart().x) / 1e6, (t.GetEnd().y - t.GetStart().y) / 1e6
    L = math.hypot(dx, dy)
    if L == 0:
        continue
    a = math.degrees(math.atan2(abs(dy), abs(dx)))
    dev = min(abs(a), abs(a - 45), abs(a - 90))
    where = "%s %s (%.2f,%.2f)" % (t.GetNetname(), t.GetLayerName(), t.GetStart().x / 1e6, t.GetStart().y / 1e6)
    if dev > tol:
        off.append(where)
    elif abs(a - 45) <= tol and L > dmax:
        long_diag.append(where)
print("%d %d %s" % (len(off), len(long_diag), "; ".join((off + long_diag)[:5])))
PY
)"
read -r noff nlong rest <<<"$out"
if [ "$noff" = 0 ] && [ "$nlong" = 0 ]; then report PASS angles "0 off-angle, 0 diagonals > ${DIAG_MAX_MM} mm"
else report FAIL angles "$noff off-angle, $nlong long diagonals $rest"; fi

echo
if [ "$FAILS" -eq 0 ]; then echo "ALL PASS"; else echo "$FAILS gate(s) FAILED"; fi
[ "$FAILS" -eq 0 ]
