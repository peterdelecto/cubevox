"""Lint a foreman track plan against canon 132/136 geometry before landing it.

plan_lint.py PLAN.json: reports off-angle segments, end legs that are not axis-aligned or
are shorter than END_LEG_MM, jogs (axis straight <= 3 mm between two diagonals) and
diagonals longer than 6 mm."""
import json
import math
import sys

END_LEG_MM = 0.7


def kind(a, b):
    dx, dy = b[0] - a[0], b[1] - a[1]
    if abs(dx) < 1e-6 or abs(dy) < 1e-6:
        return "axis"
    if abs(abs(dx) - abs(dy)) < 1e-4:
        return "diag"
    return "off"


def lint(plan):
    problems = []
    for n, op in enumerate(plan):
        if op.get("op") != "track":
            continue
        p = op["points"]
        segs = list(zip(p, p[1:]))
        kinds = [kind(a, b) for a, b in segs]
        lens = [math.hypot(b[0] - a[0], b[1] - a[1]) for a, b in segs]
        for k, (kd, ln) in enumerate(zip(kinds, lens)):
            if kd == "off":
                problems.append((n, "off-angle segment %d" % k, segs[k]))
            if kd == "diag" and ln > 6.0:
                problems.append((n, "diagonal %.1f mm" % ln, segs[k]))
            if kd == "axis" and 0 < k < len(segs) - 1 and kinds[k - 1] == "diag" \
                    and kinds[k + 1] == "diag" and ln <= 3.0:
                problems.append((n, "jog %.2f mm" % ln, segs[k]))
        for k in (0, len(segs) - 1):
            if kinds[k] != "axis" or lens[k] < END_LEG_MM - 1e-6:
                problems.append((n, "end leg %s %.2f mm" % (kinds[k], lens[k]), segs[k]))
    return problems


if __name__ == "__main__":
    probs = lint(json.load(open(sys.argv[1])))
    for pr in probs:
        print(pr)
    print(len(probs), "problems")
