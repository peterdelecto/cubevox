#!/usr/bin/env python3
"""Checks the cubevox serial protocol against a box on USB (spec 2026-10-12-serial-protocol-design.md).

Runs hello, get_layout, a valid set_layout, three invalid ones, status, a malformed and an overlong
line, the bench op checks, a stray-bytes check, and then resets the box to see that the layout
survived. The layout it found is put back at the end. Prints PASS or FAIL per check and exits
non-zero on any FAIL.

The checks follow the build hello reports. A bench build runs a query op and the reset check
(bench spin). A release build must refuse bench fault, record_erase and stop_audio with "op not in
this build", and has no reset check. Both builds must ignore raw bytes outside a line.

The port handling comes from firmware/cubevox_port.py and the bench requests from
firmware/bench_ops.py. With several cu.usbmodem* ports, name one with --port.

  python3 -I tools/protocol_check.py
  python3 -I tools/protocol_check.py --port /dev/cu.usbmodemXXXX --no-reset
"""

import argparse
import os
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "firmware"))  # -I drops the script dir
import bench_ops  # noqa: E402
import cubevox_port  # noqa: E402

SLOTS = 8
STRAY = b"faewdx1k"  # old bench letters, sent outside a line
SAVE_WAIT_S = 8.0     # record saves 2 s after the last change; allow for the flash ops
REPLY_WAIT_S = 3.0
REBOOT_WAIT_S = 30.0  # watchdog reset (2 s) plus USB enumeration


class Link(cubevox_port.Link):
    """The shared port link plus the non-JSON lines seen while waiting for replies."""

    def __init__(self, path, explicit=None):
        super().__init__(path)
        self.explicit = explicit  # the --port value, reused when the box re-enumerates
        self.text = []  # non-JSON lines seen, newest last

    def read_json(self, want_id, wait=REPLY_WAIT_S):
        """The reply with `want_id`; other lines are kept in self.text or dropped."""
        return self.read_reply(want_id, wait, self.text.append)

    def ask(self, cmd, **fields):
        return super().ask(cmd, REPLY_WAIT_S, self.text.append, **fields)

    def bench(self, op, slot=None, wait=REPLY_WAIT_S):
        """Sends a bench op through bench_ops.py and returns its reply."""
        rid = self.take_id()
        self.send_line(bench_ops.op_line(op, slot, rid))
        return self.read_json(rid, wait)

    def drain(self, quiet=0.3):
        """Reads until the port is quiet; returns the non-JSON text lines."""
        lines = []
        while True:
            line = self.read_line(quiet)
            if line is None:
                return lines
            if not line.startswith("{"):
                lines.append(line)


class Report:
    def __init__(self):
        self.failed = 0

    def check(self, name, ok, detail=""):
        print(f"{'PASS' if ok else 'FAIL'} {name}" + (f"  ({detail})" if detail and not ok else ""), flush=True)
        if not ok:
            self.failed += 1
        return ok


def ok_reply(r):
    return isinstance(r, dict) and r.get("ok") is True


def err_reply(r, needle):
    return isinstance(r, dict) and r.get("ok") is False and needle in str(r.get("error", ""))


def check_hello(link, rep):
    hello = link.ask("hello")
    good = ok_reply(hello) and hello.get("proto") == 1 and isinstance(hello.get("fw"), str)
    rep.check("hello: ok, proto 1, fw string", good, str(hello)[:120])
    if not good:
        return None
    ops = hello.get("bench_ops")
    rep.check("hello: build is release or bench, with an op list",
              hello.get("build") in ("release", "bench") and isinstance(ops, list)
              and {"report", "stats", "record_status"} <= set(ops), str((hello.get("build"), ops)))
    want = bench_ops.ALL_OPS if hello.get("build") == "bench" else bench_ops.QUERY_OPS
    rep.check(f"hello: {hello.get('build')} lists exactly its ops",
              isinstance(ops, list) and len(ops) == len(set(ops)) and set(ops) == want, str(ops))
    cards = hello.get("cards", [])
    ids = [c.get("id") for c in cards]
    rep.check("hello: cards listed with unique ids", len(ids) > 1 and len(set(ids)) == len(ids), str(ids))
    knobs_ok = all(
        isinstance(c.get("cost"), dict) and "avg" in c["cost"] and "worst" in c["cost"] and "measured" in c["cost"]
        and all({"name", "kind", "positions", "min", "max", "unit", "curve"} <= set(k) for k in c.get("knobs", []))
        and {"name", "group"} <= set(c)
        for c in cards)
    rep.check("hello: every card has group, cost and knob descriptors", knobs_ok)
    layout = hello.get("layout", {})
    rep.check("hello: layout has a name and 8 known slot ids",
              isinstance(layout.get("name"), str) and len(layout.get("slots", [])) == SLOTS
              and all(s in ids for s in layout["slots"]), str(layout))
    return hello


def check_get(link, rep, hello):
    got = link.ask("get_layout")
    rep.check("get_layout: matches the layout in hello",
              ok_reply(got) and got.get("layout") == hello["layout"], str(got)[:160])
    return got["layout"] if ok_reply(got) else hello["layout"]


def status_ok(st):
    keys = ("cpu_mean", "cpu_worst", "late", "muted", "seq", "rec_errors", "serial_dropped", "stray_bytes")
    return ok_reply(st) and all(k in st for k in keys)


def check_status(link, rep, label):
    st = link.ask("status")
    rep.check(f"status {label}: all fields present", status_ok(st), str(st))
    if status_ok(st):
        rep.check(f"status {label}: 0 late blocks", st["late"] == 0, f"late {st['late']}")
        rep.check(f"status {label}: no record write errors", st["rec_errors"] == 0, f"rec_errors {st['rec_errors']}")
    return st


def check_set_valid(link, rep, orig):
    """Reverses the slot order (same cards, so the cost sums stay put) under a new name."""
    slots = list(reversed(orig["slots"]))
    reply = link.ask("set_layout", name="protocol_check", slots=slots)
    rep.check("set_layout valid: ok", ok_reply(reply), str(reply))
    got = link.ask("get_layout")
    rep.check("set_layout valid: get_layout returns the new layout",
              ok_reply(got) and got["layout"] == {"name": "protocol_check", "slots": slots}, str(got)[:160])
    return {"name": "protocol_check", "slots": slots}


def check_set_invalid(link, rep, hello, current):
    ids = [c["id"] for c in hello["cards"]]
    reverbs = [c["id"] for c in hello["cards"] if c.get("group") == "reverb"]
    pool = [i for i in ids if i != "empty"]
    slots = list(current["slots"])

    bad = slots[:2] + ["no_such_card"] + slots[3:]
    r = link.ask("set_layout", name="x", slots=bad)
    rep.check("set_layout unknown id: error naming the card and slot", err_reply(r, 'unknown card "no_such_card" in slot 3'), str(r))

    if len(reverbs) >= 2:
        two = ["empty"] * SLOTS
        two[0], two[1] = reverbs[0], reverbs[1]
        r = link.ask("set_layout", name="x", slots=two)
        rep.check("set_layout two reverbs: error", err_reply(r, "two reverb"), str(r))
    else:
        rep.check("set_layout two reverbs: error", False, "hello lists fewer than two reverb cards")

    twice = ["empty"] * SLOTS
    twice[0] = twice[5] = pool[0]
    r = link.ask("set_layout", name="x", slots=twice)
    rep.check("set_layout duplicate card: error", err_reply(r, "same card twice"), str(r))

    r = link.ask("set_layout", name="x", slots=slots[:3])
    rep.check("set_layout short slots: error", err_reply(r, "8 card ids"), str(r))

    for label, name in (("empty", ""), ("32 characters", "n" * 32), ("non-ASCII", "café")):
        r = link.ask("set_layout", name=name, slots=slots)
        rep.check(f"set_layout name {label}: error", err_reply(r, "printable ASCII"), str(r))

    got = link.ask("get_layout")
    rep.check("set_layout invalid: the layout stayed unchanged", ok_reply(got) and got["layout"] == current, str(got)[:160])


def check_bad_lines(link, rep):
    link.write(b"{not json at all\n")
    r = link.read_json(0)
    rep.check("malformed line: error with id 0", err_reply(r, "malformed"), str(r))

    link.write(b'{"id":' + b"1" * 800 + b"}\n")
    r = link.read_json(0)
    rep.check("overlong line: error with id 0", err_reply(r, "too long"), str(r))

    r = link.ask("no_such_cmd")
    rep.check("unknown cmd: error echoing the id", err_reply(r, "unknown cmd"), str(r))

    hello = link.ask("hello")
    rep.check("after bad lines: protocol still answers", ok_reply(hello))


def check_bench_ops(link, rep, build):
    link.drain(0.2)
    link.text.clear()

    # The box prints the stats, then replies, so the lines arrive while read_json waits.
    r = link.bench("stats")
    lines = link.text + link.drain(0.6)
    rep.check("bench stats: ok reply and a stats line on the same port",
              ok_reply(r) and any(l.startswith("[audio]") for l in lines), f"{r} {lines[:3]}")
    r = link.ask("bench", op="zap")
    rep.check("bench unknown op: error", err_reply(r, "unknown op: zap"), str(r))
    r = link.ask("bench")
    rep.check("bench missing op: error", err_reply(r, "missing op"), str(r))
    if build == "bench":
        r = link.bench("slot_off")
        rep.check("bench slot_off without a slot: error", err_reply(r, "missing slot"), str(r))
        r = link.bench("slot_off", 9)
        rep.check("bench slot_off slot 9: error", err_reply(r, "slot must be 1 to 8"), str(r))
    else:
        for op in ("fault", "record_erase", "stop_audio"):
            r = link.bench(op)
            rep.check(f"release bench {op}: refused", err_reply(r, f"op not in this build: {op}"), str(r))
    r = link.ask("status")
    rep.check("bench ops: protocol answers afterwards", status_ok(r))


def check_stray_bytes(link, rep):
    """Raw bytes outside a line must not act: no record write, no late block, audio still running."""
    link.ask("status")  # opens the load window
    time.sleep(0.3)
    before = link.ask("status")
    if not status_ok(before):
        rep.check("stray bytes: status before", False, str(before))
        return
    link.write(STRAY)
    time.sleep(0.5)
    after = link.ask("status")
    if not rep.check("stray bytes: port still answers", status_ok(after), str(after)):
        return
    rep.check("stray bytes: record seq unchanged", after["seq"] == before["seq"], f"{before['seq']} -> {after['seq']}")
    rep.check("stray bytes: late blocks unchanged", after["late"] == before["late"], f"{before['late']} -> {after['late']}")
    rep.check(f"stray bytes: counted {len(STRAY)}", after["stray_bytes"] == before["stray_bytes"] + len(STRAY),
              f"{before['stray_bytes']} -> {after['stray_bytes']}")
    rep.check("stray bytes: audio still running", after["cpu_mean"] > 0, f"cpu_mean {after['cpu_mean']}")
    rep.check("stray bytes: output not muted by them", after["muted"] == before["muted"],
              f"{before['muted']} -> {after['muted']}")


def wait_saved(link, rep, before_seq, label):
    deadline = time.time() + SAVE_WAIT_S
    st = None
    while time.time() < deadline:
        time.sleep(0.5)
        st = link.ask("status")
        if status_ok(st) and st["seq"] > before_seq:
            return rep.check(f"{label}: record saved (seq {before_seq} -> {st['seq']})", True)
    return rep.check(f"{label}: record saved within {SAVE_WAIT_S:g} s", False, str(st))


def reopen_after_reset(link, rep):
    """Triggers a watchdog reset with the bench spin op and reconnects."""
    old = link.path
    link.send_line(bench_ops.op_line("spin", req_id=link.take_id()))
    time.sleep(0.5)
    link.close()
    deadline = time.monotonic() + REBOOT_WAIT_S
    # The box spins for the watchdog period with its port still up; wait for the port to go first.
    while cubevox_port.list_ports() and time.monotonic() < deadline:
        time.sleep(0.1)
    last_error = "no port appeared"
    while time.monotonic() < deadline:
        time.sleep(0.5)
        try:
            new = Link(cubevox_port.find_port(link.explicit), link.explicit)
        except cubevox_port.PortError as e:
            last_error = str(e)  # not enumerated yet, or several ports; retried until the deadline
            continue
        time.sleep(1.0)
        new.drain(0.5)
        rep.check("reset: box came back on USB", True)
        return new
    rep.check("reset: box came back on USB", False, f"{last_error} within {REBOOT_WAIT_S:g} s (was {old})")
    return None


def check_survives_reset(link, rep, expected):
    link = reopen_after_reset(link, rep)
    if link is None:
        return None
    got = link.ask("get_layout")
    rep.check("reset: layout survived", ok_reply(got) and got["layout"] == expected, str(got)[:160])
    return link


def restore(link, rep, orig):
    st = link.ask("status")
    before = st["seq"] if status_ok(st) else 0
    r = link.ask("set_layout", name=orig["name"], slots=orig["slots"])
    rep.check("restore: original layout set", ok_reply(r), str(r))
    wait_saved(link, rep, before, "restore")


def run(link, rep, no_reset):
    hello = check_hello(link, rep)
    if hello is None:
        return
    orig = check_get(link, rep, hello)
    link.ask("status")  # opens the load window
    check_status(link, rep, "before")

    st = link.ask("status")
    before = st["seq"] if status_ok(st) else 0
    new = check_set_valid(link, rep, orig)
    check_set_invalid(link, rep, hello, new)
    check_status(link, rep, "after applies")
    check_bad_lines(link, rep)
    build = hello.get("build")
    check_bench_ops(link, rep, build)

    wait_saved(link, rep, before, "set_layout")
    check_stray_bytes(link, rep)  # after the save, so a changed seq can only come from the bytes
    if build != "bench":
        print("SKIP reset check: a release build has no bench spin op", flush=True)
    elif not no_reset:
        link = check_survives_reset(link, rep, new)
        if link is None:
            return
    restore(link, rep, orig)
    check_status(link, rep, "end")
    link.close()


def main():
    ap = argparse.ArgumentParser(description="Check the cubevox serial protocol against a box on USB.")
    ap.add_argument("--port", default=None,
                    help="serial port (default: CUBEVOX_PORT, else the only /dev/cu.usbmodem*)")
    ap.add_argument("--no-reset", action="store_true", help="skip the reset-and-reload check")
    a = ap.parse_args()

    try:
        link = Link(cubevox_port.find_port(a.port), a.port)
    except cubevox_port.PortError as e:
        sys.exit(f"[ERROR] {e}")
    print(f"[check] {link.path}", flush=True)
    time.sleep(1.0)  # the boot report prints when the port opens
    link.drain(0.5)
    rep = Report()
    try:
        run(link, rep, a.no_reset)
    except OSError as e:
        rep.check("serial port stayed open", False, str(e))
    print(f"[check] {'all passed' if rep.failed == 0 else str(rep.failed) + ' failed'}")
    sys.exit(1 if rep.failed else 0)


if __name__ == "__main__":
    main()
