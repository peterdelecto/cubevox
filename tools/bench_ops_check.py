#!/usr/bin/env python3
"""Checks the host tools' copies of firmware facts against the firmware sources. No board needed.

firmware/bench_ops.py against the op table in core/bench_ops.cpp, and firmware/bringup.py's
panel thresholds, block rate and SAI kernel clock against core/panel_test.h, engine/common.h
and board/audio.cpp. The stream fault drills' restart budget, relay wait, software reset flag and
audio entry causes against core/audio_recovery.h, core/timing.h, the bootloader's boot_decide.h,
board/fault.cpp and core/audio_fault_cause.h. The survival drills' watchdog period, load rule waits and
burn cap against board/fault.cpp, core/load_rule.h and core/bench_ops.h. build.sh runs this, so a drift fails the build.

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
FAULT_CPP = os.path.join(SRC, "board", "fault.cpp")
AUDIO_FAULT_CAUSE_H = os.path.join(SRC, "core", "audio_fault_cause.h")
AUDIO_RECOVERY_H = os.path.join(SRC, "core", "audio_recovery.h")
TIMING_H = os.path.join(SRC, "core", "timing.h")
LOAD_RULE_H = os.path.join(SRC, "core", "load_rule.h")
BENCH_OPS_H = os.path.join(SRC, "core", "bench_ops.h")
BOOT_DECIDE_H = os.path.join(ROOT, "firmware", "bootloader", "boards", "cubevox", "boot_decide.h")
ENGINE_COMMON_H = os.path.join(ROOT, "engine", "common.h")
ROW = re.compile(r'\{BenchOp::\w+,\s*"(\w+)",\s*OpClass::(Query|Service|Bench)\}')


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
    services = {n for n, c in rows if c == "Service"}
    failed = []
    if len(rows) != len(names) or not rows:
        failed.append(f"table parse: {len(rows)} rows, {len(names)} unique names")
    if names != bench_ops.ALL_OPS:
        failed.append(f"op names differ: only in C++ {sorted(names - bench_ops.ALL_OPS)}, "
                      f"only in bench_ops.py {sorted(bench_ops.ALL_OPS - names)}")
    if queries != bench_ops.QUERY_OPS:
        failed.append(f"query ops differ: C++ {sorted(queries)}, bench_ops.py {sorted(bench_ops.QUERY_OPS)}")
    if services != bench_ops.SERVICE_OPS:
        failed.append(f"service ops differ: C++ {sorted(services)}, bench_ops.py {sorted(bench_ops.SERVICE_OPS)}")
    return failed, len(rows)


def constant_failures():
    """bringup.py constants against the firmware values they copy."""
    pairs = [
        ("PANEL_LOW", bringup.PANEL_LOW, constant(PANEL_TEST_H, "kPanelTestLow")),
        ("PANEL_HIGH", bringup.PANEL_HIGH, constant(PANEL_TEST_H, "kPanelTestHigh")),
        ("BLOCK_RATE_HZ", bringup.BLOCK_RATE_HZ,
         constant(ENGINE_COMMON_H, "kSampleRate") / constant(ENGINE_COMMON_H, "kBlock")),
        ("SAI_HZ", bringup.SAI_HZ, constant(AUDIO_CPP, "kSaiKernelHz")),
        ("RESTARTS_PER_WINDOW", bringup.RESTARTS_PER_WINDOW, constant(AUDIO_RECOVERY_H, "kRestartsPerWindow")),
        ("RESTART_WINDOW_S", bringup.RESTART_WINDOW_S, constant(AUDIO_RECOVERY_H, "kRestartWindowMs") / 1000.0),
        ("RELAY_AFTER_START_S", bringup.RELAY_AFTER_START_S, constant(TIMING_H, "kRelayCloseAfterPowerOnMs") / 1000.0),
        ("SOFTWARE_RESET", bringup.SOFTWARE_RESET, software_reset_flag()),
        ("WATCHDOG_S", bringup.WATCHDOG_S, constant(FAULT_CPP, "kWatchdogMs") / 1000.0),
        ("LOAD_WAIT_FIRST_S", bringup.LOAD_WAIT_FIRST_S, constant(LOAD_RULE_H, "kLoadWaitFirstMs") / 1000.0),
        ("LOAD_WAIT_CAP_S", bringup.LOAD_WAIT_CAP_S, constant(LOAD_RULE_H, "kLoadWaitCapMs") / 1000.0),
        ("LOAD_RESET_AFTER_S", bringup.LOAD_RESET_AFTER_S, constant(LOAD_RULE_H, "kLoadResetAfterMs") / 1000.0),
        ("MAX_BURN_CYCLES", bringup.MAX_BURN_CYCLES, constant(BENCH_OPS_H, "kMaxBenchBurnCycles")),
    ]
    return [f"bringup.py {name} is {have}, firmware has {want}"
            for name, have, want in pairs if not math.isclose(have, want, rel_tol=1e-6)]


def software_reset_flag():
    """CV_RSR_SFTRSTF of the bootloader's boot_decide.h, written as (1u << N)."""
    with open(BOOT_DECIDE_H) as f:
        m = re.search(r"#define\s+CV_RSR_SFTRSTF\s+\(1u\s*<<\s*(\d+)\)", f.read())
    if not m:
        raise ValueError(f"{os.path.relpath(BOOT_DECIDE_H, ROOT)}: no CV_RSR_SFTRSTF")
    return float(1 << int(m.group(1)))


def cause_failures():
    """The audio entry causes bringup.py looks for: a restart's in board/fault.cpp, and the causes that
    reset the box, with their codes, in core/audio_fault_cause.h."""
    wanted = ((FAULT_CPP, (bringup.CAUSE_RESTARTED,)),
              (AUDIO_FAULT_CAUSE_H, (bringup.CAUSE_OUT_OF_RESTARTS, bringup.CAUSE_RESTART_FAILED)))
    failed = []
    for path, causes in wanted:
        with open(path) as f:
            text = f.read()
        failed += [f"bringup.py audio cause '{c}' is not a string in {os.path.relpath(path, SRC)}"
                   for c in causes if f'"{c}"' not in text]
    return failed


def main():
    try:
        failed, n_ops = op_table_failures()
        failed += constant_failures()
        failed += cause_failures()
    except (OSError, ValueError, AttributeError) as e:
        failed, n_ops = [f"cannot read firmware sources: {e}"], 0
    for msg in failed:
        print("FAIL", msg)
    if not failed:
        print(f"PASS host tools match the firmware ({n_ops} bench ops, panel thresholds, block rate, SAI clock, "
              "restart budget, relay wait, reset flag, audio causes, watchdog, load waits, burn cap)")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
