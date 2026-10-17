#!/usr/bin/env python3
"""Reads the cubevox USB serial port and prints what arrives.

Opening the port raises DTR, which makes the firmware print its boot report. Exits 1 when
the port cannot be opened or closes before the time is up.

  ./term.py                 # read for 5 s
  ./term.py -t 20 -s f      # send the bench letter 'f' (op fault; bench image only)
  ./term.py -s m -s p -w 5  # send 'm', wait 5 s, send 'p'
  ./term.py -s '{"id":1,"cmd":"status"}'   # a raw JSON request passes through
  ./term.py /dev/cu.usbmodemXXXX

A letter expands to a framed JSON request through bench_ops.py (see its letter map). With
several cu.usbmodem* ports, name one as the argument or in CUBEVOX_PORT.
"""

import argparse
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))  # python3 -I leaves the script dir out
import bench_ops  # noqa: E402
import cubevox_port  # noqa: E402

POLL_S = 0.1


def run(link, seconds, sends, wait_s):
    """Prints lines for `seconds`, sending one queued request every `wait_s`. Returns the exit code."""
    end = time.monotonic() + seconds
    send_at = time.monotonic() + wait_s if sends else None
    while time.monotonic() < end:
        if send_at and time.monotonic() >= send_at:
            line = sends.pop(0)
            link.send_line(line)
            print(f"[term] sent {line.strip()}", flush=True)
            send_at = time.monotonic() + wait_s if sends else None
        line = link.read_line(POLL_S)
        if line is not None:
            print(line, flush=True)
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("port", nargs="?", default=None)
    ap.add_argument("-t", "--seconds", type=float, default=5.0, help="how long to read")
    ap.add_argument("-s", "--send", action="append", default=[],
                    help="a bench letter or a raw JSON request; repeat for a sequence")
    ap.add_argument("-w", "--wait", type=float, default=1.0,
                    help="seconds before the first send and between sends")
    a = ap.parse_args()
    try:
        sends = [bench_ops.to_line(s) for s in a.send]
    except ValueError as e:
        sys.exit(f"[ERROR] {e}")
    if sends:
        a.seconds = max(a.seconds, a.wait * len(sends) + 2.0)

    try:
        link = cubevox_port.Link(cubevox_port.find_port(a.port))
    except cubevox_port.PortError as e:
        sys.exit(f"[ERROR] {e}")
    print(f"[term] {link.path} open for {a.seconds:g} s", flush=True)
    try:
        code = run(link, a.seconds, sends, a.wait)
    except cubevox_port.PortGone as e:
        print(f"[term] port went away: {e}", flush=True)
        code = 1
    if link.pending():
        print(link.pending(), flush=True)
    link.close()
    return code


if __name__ == "__main__":
    sys.exit(main())
