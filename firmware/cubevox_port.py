#!/usr/bin/env python3
"""The cubevox USB serial port for every host tool: discovery, raw open, line reads, JSON replies.

No pyserial; the OS driver and termios are enough for a CDC port. Opening the port raises DTR,
which makes the firmware print its boot report.

As a command it prints the port path, for shell scripts, and can send one request:

  python3 -I cubevox_port.py                      # print the port; fail when none or several
  python3 -I cubevox_port.py --wait 30            # wait for the box to enumerate
  python3 -I cubevox_port.py --allow-none         # print nothing and exit 0 when no box is present
  python3 -I cubevox_port.py --request enter_dfu  # send {"id":N,"cmd":"enter_dfu"}, exit 0 on ok:true

CUBEVOX_PORT names the port when more than one cu.usbmodem* device is attached.
"""

import argparse
import glob
import json
import os
import select
import sys
import termios
import time
import tty

PORT_GLOB = "/dev/cu.usbmodem*"
PORT_ENV = "CUBEVOX_PORT"
READ_BYTES = 4096
WRITE_TIMEOUT_S = 5.0
POLL_S = 0.5


class PortError(Exception):
    """The port cannot be found, chosen or opened."""


class PortGone(OSError):
    """The port closed or the device vanished while open."""


def list_ports():
    return sorted(glob.glob(PORT_GLOB))


def find_port(explicit=None, wait_s=0.0, allow_none=False):
    """The one cubevox port. Several candidates without an explicit choice is an error.

    An explicit path, else CUBEVOX_PORT, wins and is not checked here; open() reports a bad one.
    Returns None only when allow_none is set and no port appears within wait_s.
    """
    chosen = explicit or os.environ.get(PORT_ENV)
    if chosen:
        return chosen
    end = time.monotonic() + wait_s
    while True:
        ports = list_ports()
        if ports:
            break
        if time.monotonic() >= end:
            if allow_none:
                return None
            raise PortError(f"no {PORT_GLOB} port; is the box enumerated?")
        time.sleep(POLL_S)
    if len(ports) > 1:
        raise PortError(f"{len(ports)} ports match {PORT_GLOB}: {' '.join(ports)}; "
                        f"name one with --port or {PORT_ENV}")
    return ports[0]


class Link:
    """One open raw port that reads lines and exchanges JSON requests by id."""

    def __init__(self, path):
        self.path = path
        self.buf = b""
        self.next_id = 1
        try:
            self.fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        except OSError as e:
            raise PortError(f"cannot open {path}: {e}") from e
        try:
            tty.setraw(self.fd, termios.TCSANOW)  # the default TCSAFLUSH eats the head of the boot report
            attrs = termios.tcgetattr(self.fd)
            attrs[2] |= termios.CLOCAL | termios.CREAD
            termios.tcsetattr(self.fd, termios.TCSANOW, attrs)
        except (termios.error, OSError) as e:
            os.close(self.fd)
            raise PortError(f"cannot configure {path}: {e}") from e

    def close(self):
        os.close(self.fd)

    def write(self, data):
        """Writes every byte; waits for the driver when its buffer is full."""
        view = memoryview(data)
        end = time.monotonic() + WRITE_TIMEOUT_S
        while view:
            try:
                view = view[os.write(self.fd, view):]
            except BlockingIOError:
                left = end - time.monotonic()
                if left <= 0 or not select.select([], [self.fd], [], left)[1]:
                    raise PortGone(f"{self.path}: write blocked for {WRITE_TIMEOUT_S:g} s") from None
            except OSError as e:
                raise PortGone(f"{self.path}: {e}") from e

    def send_line(self, text):
        self.write(text.encode())

    def _fill(self):
        try:
            chunk = os.read(self.fd, READ_BYTES)
        except BlockingIOError:
            return
        except OSError as e:
            raise PortGone(f"{self.path}: {e}") from e
        if not chunk:
            raise PortGone(f"{self.path}: closed")
        self.buf += chunk

    def read_line(self, timeout_s):
        """The next line (no line ending), or None when none completes within timeout_s."""
        end = time.monotonic() + timeout_s
        while b"\n" not in self.buf:
            left = end - time.monotonic()
            if left <= 0:
                return None
            if select.select([self.fd], [], [], left)[0]:
                self._fill()
        raw, self.buf = self.buf.split(b"\n", 1)
        return raw.decode("utf-8", "replace").rstrip("\r")

    def pending(self):
        """Text received after the last complete line."""
        return self.buf.decode("utf-8", "replace")

    def read_reply(self, want_id, wait_s, on_text=None):
        """The JSON reply carrying want_id, or None on timeout.

        Other JSON lines are skipped, non-JSON lines go to on_text, and a line that starts with
        a brace but does not parse is returned as {"_unparsed": line}.
        """
        end = time.monotonic() + wait_s
        while True:
            line = self.read_line(max(end - time.monotonic(), 0.0))
            if line is None:
                return None
            if not line.startswith("{"):
                if on_text:
                    on_text(line)
                continue
            try:
                reply = json.loads(line)
            except ValueError:
                return {"_unparsed": line}
            if reply.get("id") == want_id:
                return reply

    def take_id(self):
        rid = self.next_id
        self.next_id += 1
        return rid

    def ask(self, cmd, wait_s=3.0, on_text=None, **fields):
        """Sends {"id":N,"cmd":cmd,...} and returns the reply with that id, or None."""
        rid = self.take_id()
        self.send_line(json.dumps({"id": rid, "cmd": cmd, **fields}) + "\n")
        return self.read_reply(rid, wait_s, on_text)


def main():
    ap = argparse.ArgumentParser(description="Print the cubevox serial port, or send one request to it.")
    ap.add_argument("--port", default=None, help=f"port path (default: {PORT_ENV}, else the only {PORT_GLOB})")
    ap.add_argument("--wait", type=float, default=0.0, help="seconds to wait for the port to appear")
    ap.add_argument("--allow-none", action="store_true", help="print nothing and exit 0 when no port is present")
    ap.add_argument("--request", metavar="CMD", help="send this command and require ok:true in the reply")
    ap.add_argument("--reply-wait", type=float, default=3.0, help="seconds to wait for the reply")
    a = ap.parse_args()
    try:
        path = find_port(a.port, a.wait, a.allow_none)
        if path is None:
            return 0
        if not a.request:
            print(path)
            return 0
        link = Link(path)
        try:
            reply = link.ask(a.request, a.reply_wait)
        finally:
            link.close()
    except (PortError, PortGone) as e:
        print(f"[ERROR] {e}", file=sys.stderr)
        return 1
    if reply is not None:
        print(json.dumps(reply))
    if not (isinstance(reply, dict) and reply.get("ok") is True):
        print(f"[ERROR] {a.request}: {reply if reply is not None else f'no reply in {a.reply_wait:g} s'}",
              file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
