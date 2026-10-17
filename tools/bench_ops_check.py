#!/usr/bin/env python3
"""Checks the host tools' copies of firmware facts against the firmware sources. No board needed.

firmware/bench_ops.py against the op table in core/bench_ops.cpp, and firmware/bringup.py's
panel thresholds, block rate and SAI kernel clock against core/panel_test.h, engine/common.h
and board/audio.cpp. build.sh runs this, so a drift fails the build.

  python3 -I tools/bench_ops_check.py
"""

import math
import os
import re
import sys

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
sys.path.insert(0, os.path.join(ROOT, "firmware"))  # -I drops the script dir
import bench_ops  # noqa: E402
import bringup  # noqa: E402

SRC = os.path.join(ROOT, "firmware", "cubevox", "src")
TABLE = os.path.join(SRC, "core", "bench_ops.cpp")
PANEL_TEST_H = os.path.join(SRC, "core", "panel_test.h")
AUDIO_CPP = os.path.join(SRC, "board", "audio.cpp")
ENGINE_COMMON_H = os.path.join(ROOT, "engine", "common.h")
ROW = re.compile(r'\{BenchOp::\w+,\s*"(\w+)",\s*OpClass::(Query|Bench)\}')


def constant(path, name):
    """The numeric value of `constexpr <type> <name> = <number>;` in a source file."""
    with open(path) as f:
        m = re.search(rf"\bconstexpr\s+\w+\s+{name}\s*=\s*([0-9.]+)f?\s*;", f.read())
    if not m:
        raise ValueError(f"{os.path.relpath(path, ROOT)}: no constexpr {name}")
    return float(m.group(1))


def op_table_failures():
    with open(TABLE) as f:
        rows = ROW.findall(f.read())
    names = {n for n, _ in rows}
    queries = {n for n, c in rows if c == "Query"}
    failed = []
    if len(rows) != len(names) or not rows:
        failed.append(f"table parse: {len(rows)} rows, {len(names)} unique names")
    if names != bench_ops.ALL_OPS:
        failed.append(f"op names differ: only in C++ {sorted(names - bench_ops.ALL_OPS)}, "
                      f"only in bench_ops.py {sorted(bench_ops.ALL_OPS - names)}")
    if queries != bench_ops.QUERY_OPS:
        failed.append(f"query ops differ: C++ {sorted(queries)}, bench_ops.py {sorted(bench_ops.QUERY_OPS)}")
    return failed, len(rows)


def constant_failures():
    """bringup.py constants against the firmware values they copy."""
    pairs = [
        ("PANEL_LOW", bringup.PANEL_LOW, constant(PANEL_TEST_H, "kPanelTestLow")),
        ("PANEL_HIGH", bringup.PANEL_HIGH, constant(PANEL_TEST_H, "kPanelTestHigh")),
        ("BLOCK_RATE_HZ", bringup.BLOCK_RATE_HZ,
         constant(ENGINE_COMMON_H, "kSampleRate") / constant(ENGINE_COMMON_H, "kBlock")),
        ("SAI_HZ", bringup.SAI_HZ, constant(AUDIO_CPP, "kSaiKernelHz")),
    ]
    return [f"bringup.py {name} is {have}, firmware has {want}"
            for name, have, want in pairs if not math.isclose(have, want, rel_tol=1e-6)]


def main():
    try:
        failed, n_ops = op_table_failures()
        failed += constant_failures()
    except (OSError, ValueError) as e:
        failed, n_ops = [f"cannot read firmware sources: {e}"], 0
    for msg in failed:
        print("FAIL", msg)
    if not failed:
        print(f"PASS host tools match the firmware ({n_ops} bench ops, panel thresholds, block rate, SAI clock)")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
