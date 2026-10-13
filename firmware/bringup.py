#!/usr/bin/env python3
"""First-article bring-up for a cubevox board (checklist in hardware/RUNLOG.md, boards ordered).

Run after first power-up, with the board on USB. A real board gets the bench image first (the
drills need it); the script checks the boot report, times the audio block rate, optionally drills
the fault paths and DFU entry, asks for the readings only a person can take, then flashes the
release image and confirms hello reports build "release". --weact stays on the bench image.
Every command goes out as a framed JSON request through bench_ops.py. The log goes to
hardware/bringup/.

  ./bringup.py --board 1                 # USB checks, then the manual questions
  ./bringup.py --board 1 --faults --dfu  # also the fault drill and DFU entry (reflashes)
  ./bringup.py --board 1 --panel         # also the panel walk: pots, toggles, K103, mute, OLED
  ./bringup.py --weact --no-oled --no-prompt --faults --dfu   # dry run on the WeAct bench board
"""

import argparse
import datetime
import glob
import json
import os
import re
import select
import subprocess
import sys
import termios
import time
import tty

FW = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(FW)
sys.path.insert(0, FW)  # python3 -I leaves the script dir out
import bench_ops  # noqa: E402

BLOCK_RATE_HZ = 48000 / 64
SAI_HZ = 49152000
SAI_TOL_PPM = 100


class Port:
    """CDC port opened raw. Opening raises DTR, which makes the firmware print its boot report."""

    def __init__(self, wait_s=20.0):
        end = time.monotonic() + wait_s
        while True:
            ports = glob.glob("/dev/cu.usbmodem*")
            if ports:
                break
            if time.monotonic() > end:
                raise RuntimeError("no /dev/cu.usbmodem* port")
            time.sleep(0.5)
        self.name = ports[0]
        time.sleep(1.0)
        self.fd = os.open(self.name, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        tty.setraw(self.fd)
        attrs = termios.tcgetattr(self.fd)
        attrs[2] |= termios.CLOCAL | termios.CREAD
        termios.tcsetattr(self.fd, termios.TCSANOW, attrs)
        self.buf = b""

    def send(self, s):
        """Sends a bench letter or a raw JSON request as one framed line."""
        self.os_write(bench_ops.to_line(s))

    def os_write(self, line):
        os.write(self.fd, line.encode())

    def read(self, seconds, until=None):
        """Lines for up to `seconds`; stops early once a line matches `until`."""
        lines = []
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            r, _, _ = select.select([self.fd], [], [], 0.1)
            if not r:
                continue
            try:
                chunk = os.read(self.fd, 4096)
            except OSError:
                break  # the board reset and the port went away
            self.buf += chunk
            while b"\n" in self.buf:
                raw, self.buf = self.buf.split(b"\n", 1)
                line = raw.decode("utf-8", "replace").rstrip("\r")
                lines.append(line)
                if until and re.search(until, line):
                    return lines
        return lines

    def close(self):
        os.close(self.fd)


class Log:
    def __init__(self):
        self.rows = []
        self.failed = 0

    def check(self, name, ok, detail):
        state = "PASS" if ok else "FAIL"
        self.failed += 0 if ok else 1
        self.rows.append(f"{state:5} {name}: {detail}")
        print(self.rows[-1], flush=True)

    def note(self, name, detail):
        self.rows.append(f"NOTE  {name}: {detail}")
        print(self.rows[-1], flush=True)


def boot_report(port):
    lines = port.read(4.0, until=r"^\[(boot\] audio|ERROR\] audio:)")
    if not any(l.startswith("[boot] cubevox") for l in lines):
        port.send("r")
        lines += port.read(4.0, until=r"^\[(boot\] audio|ERROR\] audio:)")
    return lines


def find(lines, pattern):
    for line in lines:
        m = re.search(pattern, line)
        if m:
            return m
    return None


def check_boot(log, lines, want_oled):
    m = find(lines, r"^\[boot\] build (\S+)")
    log.note("image", m.group(1) if m else "build line missing")
    m = find(lines, r"^\[boot\] reset: (.*)$")
    log.note("reset cause", m.group(1) if m else "not reported")
    log.check("fault record", find(lines, r"^\[boot\] fault: none") is not None,
              "none" if find(lines, r"fault: none") else "fault recorded, see log")

    m = find(lines, r"silicon rev (\S+) \((0x[0-9A-F]+)\) dev (0x[0-9A-F]+) rom bootloader id (0x[0-9A-F]+)")
    if m:
        log.check("silicon revision", m.group(1) == "V", f"rev {m.group(1)} ({m.group(2)}), dev {m.group(3)}")
        log.note("ROM bootloader ID at 0x1FF1E7FE", m.group(4))
    else:
        log.check("silicon revision", False, "clock line missing")

    m = find(lines, r"pll source (HSE|NOT HSE) (\d+) MHz, sysclk (\d+) MHz")
    log.check("crystal start-up (PLL on HSE)", bool(m) and m.group(1) == "HSE",
              f"{m.group(1)} {m.group(2)} MHz" if m else "clock line missing")
    log.check("core clock", bool(m) and m.group(3) == "480", f"{m.group(3)} MHz" if m else "missing")

    m = find(lines, r"usb (\d+) Hz (from PLL1Q|WRONG), sai1 (\d+) Hz")
    log.check("USB clock", bool(m) and m.group(2) == "from PLL1Q" and m.group(1) == "48000000",
              f"{m.group(1)} Hz {m.group(2)}" if m else "missing")
    if m:
        ppm = (int(m.group(3)) - SAI_HZ) / SAI_HZ * 1e6
        log.check("SAI kernel clock", abs(ppm) <= SAI_TOL_PPM, f"{m.group(3)} Hz ({ppm:+.1f} ppm)")

    want = "[boot] pins XSMT C5 MUTE_N B5 SAI E2 E3 E4 E5 E6"
    got = find(lines, r"^\[boot\] pins .*$")
    log.check("pin decode", bool(got) and got.group(0).strip() == want,
              got.group(0) if got else "pins line missing")

    m = find(lines, r"icache (\S+), dcache (\S+), fpu (\S+)")
    log.check("caches and FPU", bool(m) and m.groups() == ("on", "on", "on"),
              ", ".join(m.groups()) if m else "missing")
    m = find(lines, r"^\[boot\] audio running.*|^\[ERROR\] audio: .*")
    log.check("audio init", bool(m) and "running" in m.group(0), m.group(0) if m else "no audio line")
    if want_oled:
        log.check("OLED", find(lines, r"OLED at 0x3C") is not None,
                  "found at 0x3C" if find(lines, r"OLED at 0x3C") else "not found on I2C2")


def stats(port):
    """Block counters, timed on the host when the line arrives.

    Lines queued earlier are drained first, so the count read is current. The host clock is
    independent of the board's crystal, which is what makes the block rate a check of it.
    """
    port.read(0.3)
    port.send("p")
    lines = port.read(3.0, until=r"blocks \d+")
    t = time.monotonic()
    m = find(lines, r"blocks (\d+) late (\d+) overruns (\d+)")
    return (t, *map(int, m.groups())) if m else None


def check_audio(log, port, seconds=20.0):
    port.send("m")
    port.read(1.0)
    a = stats(port)
    time.sleep(seconds)
    b = stats(port)
    if not (a and b):
        log.check("audio block rate", False, "no stats line")
        return
    rate = (b[1] - a[1]) / (b[0] - a[0])
    err = (rate - BLOCK_RATE_HZ) / BLOCK_RATE_HZ * 100
    log.check("audio block rate (48 kHz SAI)", abs(err) < 1.0,
              f"{rate:.1f} blocks/s against {BLOCK_RATE_HZ:.0f} blocks/s ({err:+.2f} %)")
    log.check("late blocks and overruns", b[2] == 0 and b[3] == 0,
              f"{b[2]} late, {b[3]} overruns in {b[1]} blocks")


PANEL_POT = re.compile(r"^\[panel\] (raw )?pot (\d+) slot (\d) ([AB]) (\d\.\d\d)")
PANEL_TOGGLE = re.compile(r"^\[panel\] (raw )?toggle slot (\d) (on|off)")
PANEL_LOW = 0.05
PANEL_HIGH = 0.95
PANEL_WAIT_S = 20.0


def panel_event(line):
    """('pot', raw, (slot, knob), pot number, value) or ('toggle', raw, slot, state) or None."""
    m = PANEL_POT.match(line)
    if m:
        return ("pot", bool(m.group(1)), (int(m.group(3)), m.group(4)), int(m.group(2)), float(m.group(5)))
    m = PANEL_TOGGLE.match(line)
    if m:
        return ("toggle", bool(m.group(1)), int(m.group(2)), m.group(3))
    return None


def watch(port, seconds, handler):
    """Feeds stream lines to `handler` until it returns something other than None."""
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        for line in port.read(0.3):
            result = handler(line)
            if result is not None:
                return result
    return None


def panel_dump(port):
    """Pot values from the 'g' command, keyed by (slot, knob) as (pot number, value)."""
    port.read(0.2)
    port.send("g")
    pots = {}
    for line in port.read(3.0, until=r"raw toggle slot 8"):
        ev = panel_event(line)
        if ev and ev[0] == "pot":
            pots[ev[2]] = (ev[3], ev[4])
    return pots


def wait_pot(port, key, goal, others):
    """Waits for `key` to reach `goal` ('low' or 'high'). Returns ok, reversed or None (timeout)."""
    def handler(line):
        ev = panel_event(line)
        if not ev or ev[1] or ev[0] != "pot":
            return None
        if ev[2] != key:
            others.add(f"slot {ev[2][0]} {ev[2][1]} (pot {ev[3]:02d})")
            return None
        value = ev[4]
        if goal == "low":
            return "ok" if value < PANEL_LOW else ("reversed" if value > PANEL_HIGH else None)
        return "ok" if value > PANEL_HIGH else None
    return watch(port, PANEL_WAIT_S, handler)


def panel_pot(log, port, slot, knob):
    key = (slot, knob)
    others = set()
    verdict = "ok"
    for goal, word in (("low", "LEFT (counter-clockwise)"), ("high", "RIGHT (clockwise)")):
        print(f"  Turn slot {slot} knob {knob} fully {word}.", flush=True)
        start = panel_dump(port).get(key)
        reached = start is not None and ((start[1] < PANEL_LOW) if goal == "low" else (start[1] > PANEL_HIGH))
        verdict = "ok" if reached else wait_pot(port, key, goal, others)
        if verdict != "ok":
            break
    nn = panel_dump(port).get(key, (0, 0.0))[0]
    name = f"panel slot {slot} knob {knob} (pot {nn:02d})"
    other = f"; movement also seen on {', '.join(sorted(others))}" if others else ""
    if verdict == "ok" and not others:
        log.check(name, True, "full travel, left then right")
    elif verdict == "reversed":
        log.check(name, False, f"read high when turned left (reversed){other}")
    elif verdict is None:
        log.check(name, False, f"no travel to the {goal} end seen in {PANEL_WAIT_S:.0f} s{other}")
    else:
        log.check(name, False, f"full travel but not alone{other}")


def panel_toggle(log, port, slot):
    print(f"  Flick toggle {slot} both ways.", flush=True)
    port.read(0.2)
    seen, others = set(), set()

    def handler(line):
        ev = panel_event(line)
        if not ev or ev[1] or ev[0] != "toggle":
            return None
        if ev[2] == slot:
            seen.add(ev[3])
        else:
            others.add(f"slot {ev[2]}")
        return True if seen == {"on", "off"} else None
    watch(port, PANEL_WAIT_S, handler)
    name = f"panel toggle {slot}"
    other = f"; edges also seen on {', '.join(sorted(others))}" if others else ""
    if seen == {"on", "off"} and not others:
        log.check(name, True, "both states seen")
    else:
        log.check(name, False, f"saw {', '.join(sorted(seen)) or 'no edge'}{other}")


def panel_yes(log, port, name, command, expect, question):
    """Sends a bench command, waits for its reply line, asks the operator, then sends it again."""
    port.read(0.2)
    port.send(command)
    sent = port.read(2.0, until=expect)
    if not find(sent, expect):
        log.check(name, False, "no reply from the box")
        return
    answer = input(question + " (y/n): ").strip().lower()
    log.check(name, answer.startswith("y"), "operator " + ("confirmed" if answer.startswith("y") else "says no"))
    if command != "o":  # the pattern stays up until the test ends
        port.send(command)  # restores the setting
        port.read(1.0)


def panel_test(log, port, prompt, want_oled):
    if not prompt:
        log.note("panel test", "SKIPPED (--no-prompt)")
        return
    port.send("t")
    if not find(port.read(2.0, until=r"test on"), r"test on"):
        log.check("panel test mode", False, "no reply to t")
        return
    print("\nPanel walk: do each step within 20 s.", flush=True)
    for slot in range(1, 9):
        for knob in "AB":
            panel_pot(log, port, slot, knob)
        panel_toggle(log, port, slot)
    port.send("l")
    summary = find(port.read(2.0, until=r"toggles \d+/8"), r"^\[panel\] pots.*")
    log.note("panel summary", summary.group(0) if summary else "no summary line")
    panel_yes(log, port, "K103 relay click", "i", r"K103 to",
              "K103 switched to PEDAL: one click, no thump from the output?")
    panel_yes(log, port, "mute relay click", "x", r"mute held",
              "Output muted with a K102 click?")
    if want_oled:
        panel_yes(log, port, "OLED test pattern", "o", r"OLED pattern up",
                  "Border, checkerboard and three text lines (5X7 OK, 9X12, 16X24) readable?")
    port.send("t")
    port.read(1.0)


def reset_and_report(port, command, expect, timeout_s=10.0):
    """Sends a command that resets the board and returns the next boot report."""
    port.send(command)
    port.read(timeout_s)  # returns early when the reset drops the port
    port.close()
    new = Port(wait_s=20.0)
    lines = boot_report(new)
    return new, lines, find(lines, expect)


def fault_drill(log, port):
    port.send("a")
    lines = port.read(3.0, until=r"audio stalled")
    log.check("audio stall mutes", find(lines, r"\[ERROR\] audio stalled") is not None,
              "stall reported, output muted" if find(lines, r"audio stalled") else "no stall line within 3 s")

    port, lines, m = reset_and_report(port, "w", r"^\[boot\] reset:.*watchdog")
    log.check("watchdog reset", m is not None, m.group(0) if m else "reset cause did not show watchdog")

    port, lines, m = reset_and_report(port, "f", r"^\[ERROR\] last reset: fault .*")
    log.check("fault recorded across reset", m is not None, m.group(0) if m else "no fault record after a forced fault")
    return port


def in_dfu():
    out = subprocess.run(["dfu-util", "-l"], capture_output=True, text=True).stdout
    return "0483:df11" in out


def wait_dfu(seconds):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        if in_dfu():
            return True
        time.sleep(1.0)
    return False


def flash_variant(variant, weact):
    """Builds and flashes the release or bench image. The WeAct board only ever runs bench."""
    env = dict(os.environ, TAIL="2", VARIANT=variant)
    env.pop("BENCH", None)
    env.pop("BOARD", None)
    if weact:
        env["BOARD"] = "weact"  # implies the bench image
    elif variant == "bench":
        env["BENCH"] = "1"
    if subprocess.run([os.path.join(FW, "build.sh")], env=env, capture_output=True).returncode != 0:
        return False
    return subprocess.run([os.path.join(FW, "flash.sh")], env=env, capture_output=True).returncode == 0


def flash_back(weact):
    """Reflashes the bench image the drills run on."""
    return flash_variant("bench", weact)


def check_release_image(log):
    """Flashes the release image and confirms hello reports build "release"."""
    flashed = flash_variant("release", False)
    log.check("flash release image", flashed, "build.sh + flash.sh VARIANT=release")
    if not flashed:
        return
    try:
        port = Port(wait_s=30.0)
    except RuntimeError as e:
        log.check("release image build", False, str(e))
        return
    req_id = bench_ops.next_id()
    port.os_write(bench_ops.cmd_line("hello", req_id))
    build = None
    for line in port.read(5.0, until=r'^\{.*"proto"'):
        if line.startswith("{") and '"proto"' in line:
            try:
                build = json.loads(line).get("build")
            except ValueError:
                pass
    port.close()
    log.check("release image build", build == "release", f'hello build "{build}"')


def dfu_drill(log, port, weact, prompt):
    port.send("d")
    port.read(2.0)
    port.close()
    ok = wait_dfu(15)
    log.check("DFU entry by command (menu path)", ok, "0483:df11 listed" if ok else "no DFU device in 15 s")
    if ok:
        log.check("flash back over DFU", flash_back(weact), "build.sh + flash.sh for this board")

    if prompt:
        input("\nHold BOOT0, tap reset, release BOOT0, then press Enter. ")
        ok = wait_dfu(30)
        log.check("DFU entry by BOOT0", ok, "0483:df11 listed" if ok else "no DFU device in 30 s")
        if ok:
            log.check("flash back over DFU", flash_back(weact), "build.sh + flash.sh for this board")
    return Port(wait_s=30.0)


MANUAL = [
    ("input current at 9.6 V", "Bench supply current at 9.6 V, before USB (mA): "),
    ("placement orientation", "U1, SOT-23s, diodes, polarised caps match the silkscreen? (y/n, note): "),
    ("menu DFU entry", "MENU > Update firmware > click reaches DFU (dfu-util -l)? (y/n/skip): "),
    ("OLED readable", "OLED shows the idle screen and the knob readout? (y/n/skip): "),
]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--board", default="weact", help="board number or label for the log name")
    ap.add_argument("--weact", action="store_true", help="WeAct bench board: flash back the weact build")
    ap.add_argument("--no-oled", action="store_true", help="skip the OLED check")
    ap.add_argument("--faults", action="store_true", help="drill audio stall, watchdog and bus fault")
    ap.add_argument("--dfu", action="store_true", help="drill DFU entry; reflashes the board")
    ap.add_argument("--panel", action="store_true", help="walk every pot and toggle, then K103, mute and OLED")
    ap.add_argument("--no-prompt", action="store_true", help="skip every question to the operator")
    a = ap.parse_args()

    log = Log()
    if not a.weact:
        # A real board gets the bench image for the drills and the release image at the end.
        flashed = flash_variant("bench", False)
        log.check("flash bench image", flashed, "build.sh BENCH=1 + flash.sh VARIANT=bench")
        if not flashed:
            return finish(log, a)
    try:
        port = Port()
    except RuntimeError as e:
        log.check("USB enumeration", False, str(e))
        return finish(log, a)
    log.check("USB enumeration", True, port.name)

    check_boot(log, boot_report(port), not a.no_oled)
    check_audio(log, port)
    if a.panel:
        panel_test(log, port, not a.no_prompt, not a.no_oled)
    if a.faults:
        port = fault_drill(log, port)
    if a.dfu:
        port = dfu_drill(log, port, a.weact, not a.no_prompt)
        check_boot(log, boot_report(port), not a.no_oled)
    port.close()
    if not a.weact:
        check_release_image(log)

    if not a.no_prompt:
        for name, question in MANUAL:
            log.note(name, input(question).strip() or "no answer")
    return finish(log, a)


def finish(log, a):
    stamp = datetime.datetime.now().strftime("%Y-%m-%d-%H%M")
    out_dir = os.path.join(REPO, "hardware", "bringup")
    os.makedirs(out_dir, exist_ok=True)
    path = os.path.join(out_dir, f"{stamp}-board-{a.board}.txt")
    with open(path, "w") as f:
        f.write(f"cubevox bring-up, board {a.board}, {stamp}\n")
        f.write("\n".join(log.rows) + "\n")
    print(f"\n[bringup] {log.failed} failed; log {path}")
    return 1 if log.failed else 0


if __name__ == "__main__":
    sys.exit(main())
