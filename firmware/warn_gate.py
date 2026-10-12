#!/usr/bin/env python3
"""Recompile our sketch sources with -Wall -Wextra -Werror, third-party includes as -isystem.

Reads the compile commands arduino-cli wrote for the build, so flags and defines match the
firmware build exactly. Only files under the sketch copy are checked; the STM32 core, HAL, libraries and the
verbatim platform/ copies are never compiled here and their headers are system headers, so their warnings are silent.
"""
import json, os, subprocess, sys
from concurrent.futures import ThreadPoolExecutor

build, tail = sys.argv[1], int(sys.argv[2]) if len(sys.argv) > 2 else 40
sketch = os.path.realpath(os.path.join(build, "sketch")) + os.sep
home = os.path.expanduser("~")
third_party = (home + "/Library/Arduino15/", home + "/Documents/Arduino/libraries/",
               os.path.dirname(os.path.abspath(__file__)) + "/platform")


def command(entry):
    out = []
    for a in entry["arguments"]:
        if a.startswith("-I") and a[2:].startswith(third_party):
            out += ["-isystem", a[2:]]
        else:
            out.append(a)
    i = out.index("-o")
    out = [a for a in out[:i] if a != "-MMD"]
    return out + ["-Wall", "-Wextra", "-Werror", "-o", os.devnull]


entries = [e for e in json.load(open(os.path.join(build, "compile_commands.json")))
           if os.path.realpath(e["file"]).startswith(sketch) and e["file"].endswith((".cpp", ".ino.cpp"))]
entries = [e for e in entries if not e["file"].endswith("requiredLibraries.cpp")]


def run(e):
    return subprocess.run(command(e), capture_output=True, text=True, cwd=e["directory"])


with ThreadPoolExecutor() as pool:
    results = list(pool.map(run, entries))
bad = [r for r in results if r.returncode]
log = "".join(r.stderr for r in bad)
print("\n".join(log.splitlines()[-tail:]))
print(f"[warn-gate] {len(entries)} files, -Wall -Wextra -Werror: {'FAIL ' + str(len(bad)) if bad else 'ok'}")
sys.exit(1 if bad else 0)
