#!/usr/bin/env python3
"""Checks firmware/bench_ops.py against the op table in core/bench_ops.cpp. No board needed.

  python3 -I tools/bench_ops_check.py
"""

import os
import re
import sys

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
sys.path.insert(0, os.path.join(ROOT, "firmware"))  # -I drops the script dir
import bench_ops  # noqa: E402

TABLE = os.path.join(ROOT, "firmware", "cubevox", "src", "core", "bench_ops.cpp")
ROW = re.compile(r'\{BenchOp::\w+,\s*"(\w+)",\s*OpClass::(Query|Bench)\}')


def main():
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
    for msg in failed:
        print("FAIL", msg)
    if not failed:
        print(f"PASS bench_ops.py matches core/bench_ops.cpp ({len(rows)} ops)")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
