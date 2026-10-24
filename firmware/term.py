#!/usr/bin/env python3
"""Reads the cubevox USB serial port and prints what arrives.

Opening the port raises DTR, which makes the firmware print its boot report. Exits 1 when
the port cannot be opened or closes before the time is up, and when --expect-image names a
version that the boot report's image line does not carry.

  ./term.py                 # read for 5 s
  ./term.py -t 20 -s f      # send the bench letter 'f' (op fault; bench image only)
  ./term.py -s m -s p -w 5  # send 'm', wait 5 s, send 'p'
  ./term.py -s '{"id":1,"cmd":"status"}'   # a raw JSON request passes through
  ./term.py --expect-image v2.00.27        # fail unless "[boot] image v2.00.27 crc ok" arrives
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


def image_line_matches(line, version):
    """True for the boot report's image line of `version` with a good CRC (board/fault.cpp reportImage)."""
    return line.startswith(f"[boot] image {version} crc ok")


def run(link, seconds, sends, wait_s, expect_image=None):
    """Prints lines for `seconds`, sending one queued request every `wait_s`. Returns the exit code.

    With `expect_image` set, the code is 1 unless the image line of that version arrived.
    """
    end = time.monotonic() + seconds
    send_at = time.monotonic() + wait_s if sends else None
    image_seen = False
    while time.monotonic() < end:
        if send_at and time.monotonic() >= send_at:
            line = sends.pop(0)
            link.send_line(line)
            print(f"[term] sent {line.strip()}", flush=True)
            send_at = time.monotonic() + wait_s if sends else None
        line = link.read_line(POLL_S)
        if line is not None:
            print(line, flush=True)
            image_seen = image_seen or (expect_image is not None and image_line_matches(line, expect_image))
    if expect_image is not None and not image_seen:
        print(f"[ERROR] no '[boot] image {expect_image} crc ok' line arrived, so the box is not "
              "running the image just flashed", flush=True)
        return 1
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("port", nargs="?", default=None)
    ap.add_argument("-t", "--seconds", type=float, default=5.0, help="how long to read")
    ap.add_argument("-s", "--send", action="append", default=[],
                    help="a bench letter or a raw JSON request; repeat for a sequence")
    ap.add_argument("-w", "--wait", type=float, default=1.0,
                    help="seconds before the first send and between sends")
    ap.add_argument("--expect-image", metavar="VERSION",
                    help="exit 1 unless the boot report says '[boot] image VERSION crc ok'")
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
        code = run(link, a.seconds, sends, a.wait, a.expect_image)
    except cubevox_port.PortGone as e:
        print(f"[term] port went away: {e}", flush=True)
        code = 1
    if link.pending():
        print(link.pending(), flush=True)
    link.close()
    return code


if __name__ == "__main__":
    sys.exit(main())
