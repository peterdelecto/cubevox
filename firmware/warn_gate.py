#!/usr/bin/env python3
"""Recompile our sketch sources with the warning flags below as errors, third-party includes as -isystem.

Reads the compile commands arduino-cli wrote for the build, so flags and defines match the
firmware build exactly. Only .c and .cpp files under the sketch copy are checked; the STM32 core,
HAL and libraries are never compiled here and their headers are system headers, so their warnings
are silent. Fails when fewer files are checked than the sketch copy holds, so a path mismatch
cannot pass an empty gate.

  warn_gate.py BUILD_DIR [--tail N] [--system-dir DIR ...]
"""

import argparse
import json
import os
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

SKIPPED = "requiredLibraries.cpp"  # arduino-cli's library probe, not our code

# -Wdouble-promotion matters on the single-precision FPU, -Wshadow and -Wundef catch silent
# name and macro mistakes, -Wformat=2 checks the printf-style calls. -Wstack-usage bounds every
# frame on the one stack all interrupts share. processHalf's frame in audio.cpp is the largest at
# 1016 B, with 768 B of block buffers, so 1152 leaves 136 B for locals and refuses a fourth buffer.
WARNING_FLAGS = ["-Wall", "-Wextra", "-Wshadow", "-Wdouble-promotion", "-Wundef", "-Wformat=2",
                 "-Wstack-usage=1152", "-Werror"]


def under(path, dirs):
    """True when path is one of dirs or inside one; a sibling sharing a name prefix does not match."""
    real = os.path.realpath(path)
    return any(real == d or real.startswith(d + os.sep) for d in map(os.path.realpath, dirs))


def command(entry, system_dirs):
    out = []
    for a in entry["arguments"]:
        if a.startswith("-I") and under(a[2:], system_dirs):
            out += ["-isystem", a[2:]]
        else:
            out.append(a)
    i = out.index("-o")
    out = [a for a in out[:i] if a != "-MMD"]
    return out + WARNING_FLAGS + ["-o", os.devnull]


def sketch_sources(sketch):
    found = []
    for root, _, names in os.walk(sketch):
        found += [os.path.join(root, n) for n in names if n.endswith((".cpp", ".c")) and n != SKIPPED]
    return found


def main():
    ap = argparse.ArgumentParser(description=(__doc__ or "").split("\n", 1)[0])
    ap.add_argument("build", help="arduino-cli build path")
    ap.add_argument("--tail", type=int, default=40, help="lines of compiler output to show on failure")
    ap.add_argument("--system-dir", action="append", default=[], metavar="DIR",
                    help="include dirs to treat as system headers; repeat")
    a = ap.parse_args()

    sketch = os.path.realpath(os.path.join(a.build, "sketch"))
    with open(os.path.join(a.build, "compile_commands.json")) as f:
        db = json.load(f)
    entries = [e for e in db if under(e["file"], [sketch]) and e["file"].endswith((".cpp", ".c"))
               and not e["file"].endswith(SKIPPED)]
    expected = len(sketch_sources(sketch))
    if not entries or len(entries) < expected:
        print(f"[warn-gate] FAIL: {len(entries)} compile commands match the {expected} source files under {sketch}")
        return 1

    with ThreadPoolExecutor() as pool:
        results = list(pool.map(
            lambda e: subprocess.run(command(e, a.system_dir), capture_output=True, text=True,
                                     cwd=e["directory"]), entries))
    bad = [r for r in results if r.returncode]
    log = "".join(r.stderr for r in bad)
    print("\n".join(log.splitlines()[-a.tail:]))
    print(f"[warn-gate] {len(entries)} files, {' '.join(WARNING_FLAGS)}: {'FAIL ' + str(len(bad)) if bad else 'ok'}")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
