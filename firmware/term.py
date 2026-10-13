#!/usr/bin/env python3
"""Reads the cubevox USB serial port and prints what arrives.

Opening the port raises DTR, which makes the firmware print its boot report. No pyserial;
the OS driver and termios are enough for a CDC port.

  ./term.py                 # read for 5 s
  ./term.py -t 20 -s f      # send the bench letter 'f' (op fault; bench image only)
  ./term.py -s m -s p -w 5  # send 'm', wait 5 s, send 'p'
  ./term.py -s '{"id":1,"cmd":"status"}'   # a raw JSON request passes through
  ./term.py /dev/cu.usbmodemXXXX

A letter expands to a framed JSON request through bench_ops.py (see its letter map).
"""

import argparse
import glob
import os
import select
import sys
import termios
import time
import tty

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))  # python3 -I leaves the script dir out
import bench_ops  # noqa: E402


def find_port():
    ports = glob.glob("/dev/cu.usbmodem*")
    if not ports:
        sys.exit("[ERROR] no /dev/cu.usbmodem* port; is the box enumerated?")
    return ports[0]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("port", nargs="?", default=None)
    ap.add_argument("-t", "--seconds", type=float, default=5.0, help="how long to read")
    ap.add_argument("-s", "--send", action="append", default=[],
                    help="a bench letter or a raw JSON request; repeat for a sequence")
    ap.add_argument("-w", "--wait", type=float, default=1.0,
                    help="seconds before the first send and between sends")
    a = ap.parse_args()
    port = a.port or find_port()
    try:
        sends = [bench_ops.to_line(s) for s in a.send]
    except ValueError as e:
        sys.exit(f"[ERROR] {e}")
    if sends:
        a.seconds = max(a.seconds, a.wait * len(sends) + 2.0)

    fd = os.open(port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    tty.setraw(fd, termios.TCSANOW)  # the default TCSAFLUSH eats the head of the boot report
    attrs = termios.tcgetattr(fd)
    attrs[2] |= termios.CLOCAL | termios.CREAD
    termios.tcsetattr(fd, termios.TCSANOW, attrs)
    print(f"[term] {port} open for {a.seconds:g} s", flush=True)

    end = time.monotonic() + a.seconds
    send_at = time.monotonic() + a.wait if sends else None
    buf = b""
    while time.monotonic() < end:
        if send_at and time.monotonic() >= send_at:
            s = sends.pop(0)
            os.write(fd, s.encode())
            print(f"[term] sent {s.strip()}", flush=True)
            send_at = time.monotonic() + a.wait if sends else None
        r, _, _ = select.select([fd], [], [], 0.1)
        if not r:
            continue
        try:
            chunk = os.read(fd, 4096)
        except OSError:
            print("[term] port went away", flush=True)
            break
        if not chunk:
            continue
        buf += chunk
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            print(line.decode("utf-8", "replace").rstrip("\r"), flush=True)
    if buf:
        print(buf.decode("utf-8", "replace"), flush=True)
    os.close(fd)


if __name__ == "__main__":
    main()
