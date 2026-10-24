#!/usr/bin/env python3
"""First-article bring-up for a cubevox board (checklist in hardware/RUNLOG.md).

Run after first power-up, with the board on USB. A real board gets the bench image first (the
drills need it); the script checks the boot report, times the audio block rate, optionally drills
the fault paths and DFU entry, asks for the readings only a person can take, then flashes the
release image and confirms hello reports build "release".
Every command goes out as a framed JSON request through bench_ops.py. The log goes to
hardware/bringup/, and the final summary names hardware/bringup/bench-checklist.md, the
measurements this script cannot take. With several cu.usbmodem* ports, name one with --port.

--dfu drills the ROM DFU, which only BOOT0 reaches once the bootloader is in sector 0, and flashes
back with flash.sh --rom. --update runs the 13 drills of
docs/specs/2026-10-24-field-update-design.md against the bootloader's update drive
(/Volumes/CUBEVOX). It needs the bootloader installed and the bench image running. The operator
drills (3, 4, 10, 11, 12) wait for Enter, and --no-prompt skips them. Drill 1 also fails a unit
whose DIAG.TXT option bytes line lacks BOR level 3 or a write protected sector 0.
Drill 13 is the release image check, so --update does not flash the release image again. Its log
is hardware/bringup/board-N.md with the UID, the bootloader and firmware versions, the checks and
the drill table.

  ./bringup.py --board 1                 # USB checks, then the manual questions
  ./bringup.py --board 1 --faults --dfu  # also the fault drill and the BOOT0 ROM DFU entry (reflashes)
  ./bringup.py --board 1 --panel         # also the panel walk: pots, toggles, K103, mute, OLED
  ./bringup.py --board 1 --faults --update   # also the update drills; log board-1.md
"""

import argparse
import collections
import datetime
import fcntl
import functools
import importlib.util
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import traceback

FW = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(FW)
sys.path.insert(0, FW)  # python3 -I leaves the script dir out
import bench_ops  # noqa: E402
import cubevox_port  # noqa: E402

STAMP_TOOL = os.path.join(REPO, "tools", "stamp_image.py")
BRINGUP_DIR = os.path.join(REPO, "hardware", "bringup")  # the logs and the checklist for a person at the bench
CHECKLIST = os.path.join(BRINGUP_DIR, "bench-checklist.md")


def load_tool(name, path):
    """A module loaded from its file. tools/ is outside the import path of this script and of the editor."""
    spec = importlib.util.spec_from_file_location(name, path)
    if spec is None or spec.loader is None:
        raise ImportError(f"[ERROR] cannot load {path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


stamp_image = load_tool("stamp_image", STAMP_TOOL)

SAMPLE_RATE_HZ = 48000
BLOCK_FRAMES = 64
BLOCK_RATE_HZ = SAMPLE_RATE_HZ / BLOCK_FRAMES
SAI_HZ = 49152000
SAI_TOL_PPM = 100
ENUMERATE_SETTLE_S = 1.0
SCRIPT_FAIL_TAIL_LINES = 20
PORT_ERRORS = (cubevox_port.PortError, cubevox_port.PortGone)


class Port(cubevox_port.Link):
    """CDC port that waits for the box to enumerate. Opening raises DTR, so the firmware prints its boot report."""

    def __init__(self, wait_s=20.0, settle_s=ENUMERATE_SETTLE_S):
        path = cubevox_port.find_port(wait_s=wait_s)
        time.sleep(settle_s)
        super().__init__(path)

    def send(self, s):
        """Sends a bench letter or a raw JSON request as one framed line."""
        self.send_line(bench_ops.to_line(s))

    def read(self, seconds, until=None):
        """Lines for up to `seconds`; stops early once a line matches `until`."""
        lines = []
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            try:
                line = self.read_line(end - time.monotonic())
            except cubevox_port.PortGone as e:
                print(f"[bringup] port went away: {e}", flush=True)  # expected when a drill resets the board
                break
            if line is None:
                break
            lines.append(line)
            if until and re.search(until, line):
                break
        return lines


class Log:
    def __init__(self):
        self.rows = []
        self.failed = 0
        self.facts = {}    # UID, bootloader and firmware version, filled by the update drills
        self.drills = []   # (number, name, status, detail, measured time)

    def check(self, name, ok, detail):
        state = "PASS" if ok else "FAIL"
        self.failed += 0 if ok else 1
        self.rows.append(f"{state:5} {name}: {detail}")
        print(self.rows[-1], flush=True)

    def note(self, name, detail):
        self.rows.append(f"NOTE  {name}: {detail}")
        print(self.rows[-1], flush=True)

    def drill(self, number, name, status, detail, measured=""):
        self.failed += 1 if status == "FAIL" else 0
        self.drills.append((number, name, status, detail, measured))
        print(f"DRILL {number:02d} {status:7} {name}: {detail}", flush=True)


def boot_report(port):
    lines = port.read(4.0, until=r"^\[(boot\] pots|ERROR\] pots:)")
    if not any(l.startswith("[boot] cubevox") for l in lines):
        port.send("r")
        lines += port.read(4.0, until=r"^\[(boot\] pots|ERROR\] pots:)")
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

    want = "[boot] pins XSMT C5 MUTE_N B5 SAI E2 E3 E4 E5 E6 OLED B10 B11"
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
    goal = "low"  # the end being tried when the loop stops, used in the timeout message
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
              "K103 switched to LINE: one click, no thump from the output?")
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
    lines = port.read(10.0)  # the stall line, then the port drops when the box resets
    log.check("audio stall mutes", find(lines, r"\[ERROR\] audio stalled") is not None,
              "stall reported, output muted" if find(lines, r"audio stalled") else "no stall line before the reset")
    port.close()
    port = Port(wait_s=20.0)
    m = find(boot_report(port), r"^\[boot\] reset:.*software")
    log.check("audio stall resets", m is not None, m.group(0) if m else "no software reset after the stall")

    port, _, m = reset_and_report(port, "w", r"^\[boot\] reset:.*watchdog")
    log.check("watchdog reset", m is not None, m.group(0) if m else "reset cause did not show watchdog")

    port, _, m = reset_and_report(port, "f", r"^\[ERROR\] last reset: fault .*")
    log.check("fault recorded across reset", m is not None, m.group(0) if m else "no fault record after a forced fault")
    return port


def in_dfu():
    r = subprocess.run(["dfu-util", "-l"], capture_output=True, text=True)
    if r.returncode:
        print(f"[bringup] dfu-util -l failed (exit {r.returncode}): {r.stderr.strip()}", flush=True)
        return False
    return "0483:df11" in r.stdout


def wait_dfu(seconds):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        if in_dfu():
            return True
        time.sleep(1.0)
    return False


def fw_env(variant):
    """The environment build.sh derives its variant and out dir from."""
    env = dict(os.environ, TAIL=str(SCRIPT_FAIL_TAIL_LINES), VARIANT=variant)
    env.pop("BENCH", None)
    return env


def flash_variant(variant, rom=False):
    """Builds and flashes the release or bench image.

    flash.sh uses the update drive, or the ST ROM DFU with rom=True. build.sh derives the out dir
    from VARIANT; a failing script's output tail is shown.
    """
    env = fw_env(variant)
    for script, args in (("build.sh", []), ("flash.sh", ["--rom"] if rom else [])):
        r = subprocess.run([os.path.join(FW, script), *args], env=env, text=True,
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        if r.returncode:
            tail = "\n".join(r.stdout.splitlines()[-SCRIPT_FAIL_TAIL_LINES:])
            print(f"[bringup] {script} ({variant}) exited {r.returncode}:\n{tail}", flush=True)
            return False
    return True


def check_release_image(log):
    """Flashes the release image and confirms hello reports build "release"."""
    flashed = flash_variant("release")
    log.check("flash release image", flashed, "build.sh + flash.sh VARIANT=release")
    if not flashed:
        return
    try:
        port = Port(wait_s=30.0)
        reply = port.ask("hello", 5.0)
        port.close()
    except PORT_ERRORS as e:
        log.check("release image build", False, str(e))
        return
    if reply is None:
        log.check("release image build", False, "no hello reply in 5 s")
    elif "_unparsed" in reply:
        log.check("release image build", False, f"hello reply is not JSON: {reply['_unparsed'][:120]}")
    else:
        build = reply.get("build")
        log.check("release image build", build == "release", f'hello build "{build}"')


def dfu_drill(log, port, prompt):
    """The ST ROM DFU, which only BOOT0 reaches once the bootloader is in.

    enter_dfu and the menu open the update drive, which the update drills cover.
    """
    port.close()
    if not prompt:
        log.note("DFU entry by BOOT0", "SKIPPED (--no-prompt)")
        return Port(wait_s=30.0)
    input("\nHold BOOT0, tap reset, release BOOT0, then press Enter. ")
    ok = wait_dfu(30)
    log.check("DFU entry by BOOT0", ok, "0483:df11 listed" if ok else "no DFU device in 30 s")
    if ok:
        flashed = flash_variant("bench", rom=True)
        log.check("flash back over DFU", flashed, "build.sh + flash.sh --rom for this board")
    return Port(wait_s=30.0)


# ---- update drills -------------------------------------------------------------------------------
# The 13 drills of docs/specs/2026-10-24-field-update-design.md. Each moves the box between the
# running application and the bootloader's update drive and judges a step by what the host can see:
# the serial port, the drive, INFO_UF2.TXT, DIAG.TXT and the replies of hello, status and get_layout.
# What the OLED showed is a question to the operator at the end of the run.

PASS, FAIL, SKIPPED = "PASS", "FAIL", "SKIPPED"
Install = collections.namedtuple("Install", "copy_error gone_s back_s")

VOLUME_APPEAR_S = 5.0     # drill 1: the drive mounts this soon after enter_dfu
PORT_RETURN_S = 10.0      # drill 2: the serial port returns this soon after the drive leaves
UPDATE_WAIT_S = 15.0      # steps the drill table gives no limit
REPLUG_WAIT_S = 30.0      # the operator replugs USB or cycles the power
RESET_WAIT_S = 12.0       # the watchdog op takes its period to reset the box
FAULT_WAIT_S = 5.0
HEALTHY_MARGIN_S = 2.0    # on top of the healthy window, so the failed-start count reads 0
FAST_SETTLE_S = 0.3       # a drill that acts inside the healthy window skips the long settle
PORT_POLL_S = 0.25
VOLUME = "CUBEVOX"        # UF2_VOLUME_LABEL of the bootloader's board.h
COPY_NAME = "cubevox.uf2"
PARTIAL_FRACTION = 0.3
PARTIAL_SETTLE_S = 2.0    # lets the OS push a partial copy to the box before the link breaks


class DrillError(Exception):
    """A drill step failed. The message becomes the drill's detail."""


@functools.lru_cache(maxsize=None)
def contract():
    """Every integer #define of boot_block.h, the one definition of the addresses and limits."""
    try:
        return stamp_image.read_defines(stamp_image.DEFAULT_HEADER)
    except stamp_image.ToolError as e:
        raise DrillError(str(e)) from e


def contract_value(name):
    value = contract().get(name)
    if value is None:
        raise DrillError(f"{name} is not defined in boot_block.h")
    return value


class Ctx:
    """What the drills share: the temp files, the open application port and the facts."""

    def __init__(self, log, prompt, tmp):
        self.log = log
        self.prompt = prompt
        self.tmp = tmp
        self.files = {}
        self.port = None
        self.uid = None          # the drive's serial, drill 1
        self.app_serial = None   # the application's serial, from hello
        self.fw = None

    def app(self):
        """The open port of the running application. Restores the box first when it is elsewhere."""
        if self.port is None:
            restore_app(self)
        return self.port

    def drop_port(self):
        if self.port is not None:
            try:
                self.port.close()
            except OSError as e:
                print(f"[WARN] closing {self.port.path}: {e}", flush=True)
            self.port = None

    def volume(self):
        return cubevox_port.volume_path(VOLUME)


# -- what the host can see of the box

def port_present():
    """True when a serial port node exists: the one named with --port, else any cu.usbmodem*."""
    named = os.environ.get(cubevox_port.PORT_ENV)
    return os.path.exists(named) if named else bool(cubevox_port.list_ports())


def wait_port(wait_s):
    """Seconds until a serial port node exists, or None after wait_s."""
    start = time.monotonic()
    while not port_present():
        if time.monotonic() - start >= wait_s:
            return None
        time.sleep(PORT_POLL_S)
    return time.monotonic() - start


def wait_port_gone(path, wait_s):
    end = time.monotonic() + wait_s
    while os.path.exists(path):
        if time.monotonic() >= end:
            return False
        time.sleep(PORT_POLL_S)
    return True


def wait_state(ctx, wait_s):
    """'app' once a serial port exists, 'drive' once the update drive is mounted, None after wait_s."""
    end = time.monotonic() + wait_s
    while True:
        if port_present():
            return "app"
        if cubevox_port.volume_mounted(VOLUME):
            return "drive"
        if time.monotonic() >= end:
            return None
        time.sleep(PORT_POLL_S)


def read_bytes(path):
    try:
        with open(path, "rb") as f:
            return f.read()
    except OSError as e:
        raise DrillError(f"cannot read {path}: {e.strerror}") from e


def write_bytes(path, data):
    try:
        with open(path, "wb") as f:
            f.write(data)
    except OSError as e:
        raise DrillError(f"cannot write {path}: {e.strerror}") from e


def flush_to_device(fd):
    """Pushes written data to the box. F_FULLFSYNC also asks the drive to flush; fsync is the fallback."""
    try:
        fcntl.fcntl(fd, fcntl.F_FULLFSYNC)
    except OSError:
        os.fsync(fd)


def copy_to_drive(volume, src, name, fraction=1.0):
    """Writes src, or its first `fraction` in whole UF2 blocks, to the drive. Returns None or the error text.

    The box resets after the last block, so the OS may report an error for a copy that worked.
    Callers judge by the drive and the serial port.
    """
    data = read_bytes(src)
    block = stamp_image.UF2_BLOCK_BYTES
    size = len(data) if fraction >= 1.0 else max(1, int(len(data) // block * fraction)) * block
    error, fd = None, None
    try:
        fd = os.open(os.path.join(volume, name), os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o644)
        view = memoryview(data[:size])
        while view:
            view = view[os.write(fd, view):]
        flush_to_device(fd)
    except OSError as e:
        error = str(e)
    if fd is not None:
        try:
            os.close(fd)
        except OSError as e:
            error = error or str(e)
    return error


def read_uncached(path):
    """A file's text, read around the page cache so a DIAG.TXT the bootloader rewrote shows."""
    fd = os.open(path, os.O_RDONLY)
    try:
        fcntl.fcntl(fd, fcntl.F_NOCACHE, 1)
        chunks = []
        while True:
            chunk = os.read(fd, 4096)
            if not chunk:
                return b"".join(chunks).decode("ascii", "replace")
            chunks.append(chunk)
    finally:
        os.close(fd)


def read_drive_text(volume, name, wait_s=5.0):
    """Text of a file on the update drive. Retries while the drive settles after mounting."""
    path = os.path.join(volume, name)
    end = time.monotonic() + wait_s
    while True:
        try:
            return read_uncached(path)
        except OSError as e:
            if time.monotonic() >= end:
                raise DrillError(f"cannot read {path}: {e}") from e
        time.sleep(0.3)


FIELD = re.compile(r"^([A-Za-z][A-Za-z0-9 -]*): (.*)$")


def parse_fields(text):
    """The `key: value` lines of INFO_UF2.TXT or DIAG.TXT with lower-case keys, plus line 1 as 'title'."""
    lines = [line.strip() for line in text.replace("\r", "").split("\n")]
    fields = {"title": lines[0]}
    for line in lines[1:]:
        m = FIELD.match(line)
        if m:
            fields.setdefault(m.group(1).lower(), m.group(2))
    return fields


def parse_refused(fields):
    """(refused block count, last refused family id or None) from DIAG.TXT's 'refused blocks' line."""
    m = re.match(r"(\d+)(?:, last family (0x[0-9A-Fa-f]+))?", fields.get("refused blocks", ""))
    if not m:
        return 0, None
    return int(m.group(1)), (int(m.group(2), 16) if m.group(2) else None)


OPTION_BYTES_WANT = ("BOR level 3", "sector 0 protected")


def option_bytes_problem(value):
    """Why a unit fails on DIAG.TXT's `option bytes` value, or None when it shows BOR level 3 and sector 0 protected."""
    if value is None:
        return "DIAG.TXT has no 'option bytes' line"
    missing = [f"'{want}'" for want in OPTION_BYTES_WANT if want not in value]
    if missing:
        return f"DIAG.TXT option bytes '{value}' lacks {' and '.join(missing)}"
    return None


def hex_value(text):
    """The integer in a '0x...' field, or None."""
    try:
        return int(text, 16)
    except (TypeError, ValueError):
        return None


# -- the operator and the serial protocol

def ask_operator(text):
    """Prints one instruction and waits for Enter. An answer starting with n says it did not go that way."""
    try:
        answer = input(f"\n  {text}\n  Enter when done, n if it did not go that way: ")
    except EOFError as e:
        raise DrillError("stdin is closed; run with --no-prompt to skip the operator drills") from e
    return not answer.strip().lower().startswith("n")


def ask_ok(port, cmd, wait_s=5.0, **fields):
    """The reply to a command when it says ok:true, else a DrillError that shows what came back."""
    reply = port.ask(cmd, wait_s, **fields)
    if reply is None:
        raise DrillError(f"{cmd}: no reply in {wait_s:g} s")
    if reply.get("ok") is not True:
        raise DrillError(f"{cmd}: {str(reply)[:160]}")
    return reply


def get_layout(port):
    layout = ask_ok(port, "get_layout").get("layout")
    if not (isinstance(layout, dict) and "name" in layout and "slots" in layout):
        raise DrillError(f"get_layout: no layout in the reply: {str(layout)[:120]}")
    return layout


def layout_key(layout):
    return layout["name"], tuple(layout["slots"])


def note_hello(ctx, hello):
    ctx.fw = hello.get("fw")
    ctx.log.facts["Firmware"] = f"{hello.get('fw')} ({hello.get('build')})"


# -- moving the box between the application and the drive

def restore_app(ctx):
    """Brings the box back to the running application and opens its port.

    A mounted drive gets the bench image, so a failed drill does not strand the ones after it.
    """
    state = wait_state(ctx, REPLUG_WAIT_S)
    if state == "app":
        ctx.port = Port(wait_s=1.0)
    elif state == "drive":
        install(ctx, ctx.files["bench"], COPY_NAME)
    else:
        raise DrillError(f"the box is neither a serial port nor the {VOLUME} drive "
                         f"after {REPLUG_WAIT_S:g} s")


def install(ctx, src, name, port_wait_s=UPDATE_WAIT_S):
    """Drags `src` onto the mounted drive and waits for the box to restart into the application.

    Opens ctx.port. The box resets after the last block, so the OS may report an error for a copy
    that worked. Only the drive and the serial port decide.
    """
    ctx.drop_port()
    copy_error = copy_to_drive(ctx.volume(), src, name)
    done = time.monotonic()
    if not cubevox_port.wait_volume_gone(VOLUME, UPDATE_WAIT_S):
        raise DrillError(f"{VOLUME} is still mounted {UPDATE_WAIT_S:g} s after the copy"
                         f"{os_note(copy_error)}")
    gone_s = time.monotonic() - done
    back_s = wait_port(port_wait_s)
    if back_s is None:
        raise DrillError(f"no serial port {port_wait_s:g} s after the drive left")
    ctx.port = Port(wait_s=1.0)
    return Install(copy_error, gone_s, back_s)


def os_note(error):
    return f"; the OS reported: {error}" if error else ""


def copy_note(r):
    return os_note(r.copy_error)


def enter_update_mode(ctx, wait_s):
    """enter_dfu from the running application. Returns the seconds until the drive mounted."""
    port = ctx.app()
    ask_ok(port, "enter_dfu", 3.0)
    path = port.path
    ctx.drop_port()
    start = time.monotonic()
    if cubevox_port.wait_volume(VOLUME, wait_s) is None:
        raise DrillError(f"{VOLUME} is not mounted {wait_s:g} s after enter_dfu")
    mount_s = time.monotonic() - start
    if os.path.exists(path):
        raise DrillError(f"serial port {path} is still there with the drive mounted")
    return mount_s


def ensure_drive(ctx):
    """The mounted update drive's path. Asks the application for the drive when it is not up."""
    if not cubevox_port.volume_mounted(VOLUME):
        enter_update_mode(ctx, UPDATE_WAIT_S)
    return ctx.volume()


# -- the files the drills copy

def fw_outdir(variant):
    """The out dir build.sh uses for a variant. The script is the one place that rule lives."""
    r = subprocess.run([os.path.join(FW, "build.sh"), "--outdir"], env=fw_env(variant),
                       capture_output=True, text=True)
    out = r.stdout.strip()
    if r.returncode or not out:
        raise DrillError(f"build.sh --outdir exited {r.returncode}: {(r.stdout + r.stderr).strip()[:160]}")
    return out


def build_release():
    """Builds the release image so its .uf2 is current. Returns the out dir."""
    r = subprocess.run([os.path.join(FW, "build.sh")], env=fw_env("release"), text=True,
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if r.returncode:
        tail = "\n".join(r.stdout.splitlines()[-SCRIPT_FAIL_TAIL_LINES:])
        raise DrillError(f"build.sh (release) exited {r.returncode}:\n{tail}")
    return fw_outdir("release")


def rewrap(tmp, image, name, base=None, family_id=None):
    """A .uf2 of the stamped image at another base or in another family, written by stamp_image.py."""
    out = os.path.join(tmp, name)
    cmd = [sys.executable, "-I", STAMP_TOOL, "uf2", image, out]
    if base is not None:
        cmd += ["--base", f"0x{base:08X}"]
    if family_id is not None:
        cmd += ["--family-id", f"0x{family_id:08X}"]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        raise DrillError((r.stderr or r.stdout).strip() or f"stamp_image.py exited {r.returncode}")
    return out


def flip_payload_byte(path):
    """Flips one bit in the payload of the middle block. Headers and end magics stay valid, so only the CRC knows."""
    blob = bytearray(read_bytes(path))
    block = (len(blob) // stamp_image.UF2_BLOCK_BYTES) // 2
    blob[block * stamp_image.UF2_BLOCK_BYTES + stamp_image.UF2_HEADER_BYTES + 17] ^= 0x01
    write_bytes(path, bytes(blob))


def uf2_family(path):
    """The family id in a .uf2's first block, the eighth word of the header stamp_image.make_uf2 writes."""
    head = read_bytes(path)[:stamp_image.UF2_HEADER_BYTES]
    if len(head) < stamp_image.UF2_HEADER_BYTES:
        raise DrillError(f"{path} is shorter than one UF2 block header")
    return struct.unpack_from("<8I", head, 0)[7]


def stamp_version(uf2):
    """The version `stamp_image.py check` reads from a .uf2, so a stale build shows before the copy."""
    r = subprocess.run([sys.executable, "-I", STAMP_TOOL, "check", uf2], capture_output=True, text=True)
    m = re.match(r"ok (\S+)", r.stdout)
    if r.returncode or not m:
        raise DrillError((r.stderr or r.stdout).strip() or f"stamp_image.py check exited {r.returncode}")
    return m.group(1)


def release_version():
    """VERSION.txt, the text the release file name and the image header both come from."""
    lines = read_bytes(os.path.join(REPO, "VERSION.txt")).decode("ascii", "replace").splitlines()
    version = lines[0].strip() if lines else ""
    if not re.fullmatch(r"[0-9]+\.[0-9][0-9]\.[0-9][0-9]", version):
        raise DrillError(f"VERSION.txt is '{version}'; it must look like 2.00.10")
    return version


def prepare_files(ctx):
    """Fills ctx.files from the installed bench image: the good .uf2 and the three bad ones.

    The foreign file carries family 0, which the bootloader refuses block by block.
    """
    outdir = fw_outdir("bench")
    image = os.path.join(outdir, "cubevox.ino.bin")
    bench = os.path.join(outdir, "cubevox.uf2")
    for path in (image, bench):
        if not os.path.isfile(path):
            raise DrillError(f"{path} is missing; build the bench image first")
    flipped = rewrap(ctx.tmp, image, "flipped.uf2")
    flip_payload_byte(flipped)
    ctx.files = {
        "bench": bench,
        "wrongbase": rewrap(ctx.tmp, image, "wrongbase.uf2", base=contract_value("CV_APP_END")),
        "foreign": rewrap(ctx.tmp, image, "foreign.uf2", family_id=0),
        "flipped": flipped,
    }


# -- drills 1 and 2: into the drive and out again

def drill_enter_dfu(ctx):
    mount_s = enter_update_mode(ctx, VOLUME_APPEAR_S)
    measured = f"{mount_s:.1f} s"
    info = parse_fields(read_drive_text(ctx.volume(), "INFO_UF2.TXT"))
    uid = info.get("serial", "")
    if not re.fullmatch(r"[0-9A-F]{24}", uid):
        return FAIL, f"INFO_UF2.TXT serial is '{uid}', want 24 hex characters", measured
    if ctx.app_serial and ctx.app_serial != uid:
        return FAIL, f"hello serial {ctx.app_serial} differs from the drive serial {uid}", measured
    ctx.uid = uid
    ctx.log.facts.update({"UID": uid, "Bootloader": info["title"]})
    options = parse_fields(read_drive_text(ctx.volume(), "DIAG.TXT")).get("option bytes")
    problem = option_bytes_problem(options)
    if problem:
        return FAIL, problem, measured
    return (PASS, f"serial port gone, drive mounted in {mount_s:.1f} s (limit {VOLUME_APPEAR_S:g} s); "
            f"{info['title']}; installed {info.get('installed', '?')}; option bytes {options}", measured)


def drill_install_bench(ctx):
    ensure_drive(ctx)
    r = install(ctx, ctx.files["bench"], COPY_NAME, PORT_RETURN_S)
    hello = ask_ok(ctx.port, "hello")
    note_hello(ctx, hello)
    detail = (f"drive left {r.gone_s:.1f} s after the copy, serial port back {r.back_s:.1f} s later "
              f"(limit {PORT_RETURN_S:g} s); hello build {hello.get('build')}, fw {hello.get('fw')}")
    return (PASS if hello.get("build") == "bench" else FAIL), detail + copy_note(r), f"{r.back_s:.1f} s"


# -- drills 3 and 4: a copy cut short

def partial_copy(ctx, instruction):
    """Copies the first 30 % of the bench file, then asks the operator to break the link.

    False when the operator says it did not go that way.
    """
    error = copy_to_drive(ensure_drive(ctx), ctx.files["bench"], COPY_NAME, PARTIAL_FRACTION)
    if error:
        raise DrillError(f"the partial copy failed: {error}")
    time.sleep(PARTIAL_SETTLE_S)
    return ask_operator(f"{int(PARTIAL_FRACTION * 100)} % of the file is on the drive, so {instruction}")


def drill_usb_pull(ctx):
    if not partial_copy(ctx, "pull the USB-C cable, wait 3 s and plug it back in."):
        return FAIL, "operator says the cable was not pulled", ""
    if cubevox_port.wait_volume(VOLUME, REPLUG_WAIT_S) is None:
        return FAIL, f"{VOLUME} did not return {REPLUG_WAIT_S:g} s after the replug", ""
    if port_present():
        return FAIL, "a serial port appeared; the application started instead of waiting", ""
    r = install(ctx, ctx.files["bench"], COPY_NAME)
    return PASS, f"drive returned and the box kept waiting; a full copy then restarted it ({r.back_s:.1f} s)", ""


def drill_power_cut(ctx):
    if not partial_copy(ctx, "switch the 9 V off, wait 3 s and switch it on."):
        return FAIL, "operator says the power was not cut", ""
    if cubevox_port.wait_volume(VOLUME, REPLUG_WAIT_S) is None:
        return FAIL, f"{VOLUME} did not appear {REPLUG_WAIT_S:g} s after power-on", ""
    diag = parse_fields(read_drive_text(ctx.volume(), "DIAG.TXT"))
    reason, address = diag.get("reason", "?"), diag.get("fault address", "?")
    good = reason == "ImageInvalid" or (reason == "ImageUnreadable" and bool(hex_value(address)))
    r = install(ctx, ctx.files["bench"], COPY_NAME)
    return ((PASS if good else FAIL), f"DIAG.TXT reason {reason}, fault address {address}; "
            f"a full copy then restarted the box ({r.back_s:.1f} s)", "")


# -- drills 5 to 7: files the bootloader must refuse or catch

Refusal = collections.namedtuple("Refusal", "refused family blocks copy_error")


def refused_copy(ctx, path):
    """Copies a file the bootloader must refuse, then reads DIAG.TXT until every block is counted.

    A refused file leaves the box in update mode, so a drive that vanishes is a failure.
    """
    blocks = os.path.getsize(path) // stamp_image.UF2_BLOCK_BYTES
    error = copy_to_drive(ctx.volume(), path, COPY_NAME)
    end = time.monotonic() + UPDATE_WAIT_S
    while True:
        if not cubevox_port.volume_mounted(VOLUME):
            raise DrillError(f"the drive went away after the copy; the box restarted instead of refusing"
                             f"{os_note(error)}")
        refused, family = parse_refused(parse_fields(read_drive_text(ctx.volume(), "DIAG.TXT")))
        if refused >= blocks or time.monotonic() >= end:
            return Refusal(refused, family, blocks, error)
        time.sleep(0.5)


def drill_wrong_base(ctx):
    before = layout_key(get_layout(ctx.app()))
    ensure_drive(ctx)
    refusal = refused_copy(ctx, ctx.files["wrongbase"])
    r = install(ctx, ctx.files["bench"], COPY_NAME)
    same = layout_key(get_layout(ctx.port)) == before
    detail = (f"{refusal.refused} of {refusal.blocks} blocks refused and the drive stayed up; layout "
              f"{'unchanged' if same else 'CHANGED'} after the restart ({r.back_s:.1f} s)")
    good = refusal.refused >= refusal.blocks and same
    return (PASS if good else FAIL), detail + copy_note(refusal), ""


def drill_other_family(ctx):
    expected = uf2_family(ctx.files["foreign"])
    ensure_drive(ctx)
    refusal = refused_copy(ctx, ctx.files["foreign"])
    installed = parse_fields(read_drive_text(ctx.volume(), "INFO_UF2.TXT")).get("installed", "?")
    r = install(ctx, ctx.files["bench"], COPY_NAME)
    last = "none" if refusal.family is None else hex(refusal.family)
    detail = (f"{refusal.refused} of {refusal.blocks} blocks refused, last family {last} (file {hex(expected)}); "
              f"drive still reports installed {installed}; a full copy then restarted the box ({r.back_s:.1f} s)")
    good = refusal.refused >= refusal.blocks and refusal.family == expected
    return (PASS if good else FAIL), detail + copy_note(refusal), ""


def wait_installed(ctx, want, wait_s):
    """Polls INFO_UF2.TXT until its Installed line says `want`. Returns (last value seen, last read error)."""
    path = os.path.join(ctx.volume(), "INFO_UF2.TXT")
    end = time.monotonic() + wait_s
    seen, error = None, None
    while True:
        try:
            seen = parse_fields(read_uncached(path)).get("installed")
        except OSError as e:
            error = str(e)  # the drive is between two sessions while the box restarts
        if seen == want or time.monotonic() >= end:
            return seen, error
        time.sleep(0.5)


def drill_flipped_byte(ctx):
    ensure_drive(ctx)
    copy_error = copy_to_drive(ctx.volume(), ctx.files["flipped"], COPY_NAME)  # the box restarts after the last block
    seen, read_error = wait_installed(ctx, "invalid", UPDATE_WAIT_S)
    r = install(ctx, ctx.files["bench"], COPY_NAME)
    got = f"installed '{seen}'" + (f" (last read error: {read_error})" if seen != "invalid" and read_error else "")
    detail = (f"after the flipped copy the restarted drive reports {got}; a full copy then restarted the box "
              f"({r.back_s:.1f} s)")
    return (PASS if seen == "invalid" else FAIL), detail + os_note(copy_error if seen != "invalid" else None), ""


# -- drill 8: failed starts

def read_boot_fails(port):
    """boot_fails from status, or None when the box does not answer inside the healthy window."""
    reply = port.ask("status", 1.5)
    return reply.get("boot_fails") if isinstance(reply, dict) else None


def trigger_reset(ctx, op, wait_s):
    """Sends a bench op that resets the box and waits for the serial port to drop."""
    port = ctx.port
    path = port.path
    port.send(op)
    port.read(wait_s)  # returns early when the reset drops the port
    ctx.drop_port()
    if not wait_port_gone(path, 1.0):
        raise DrillError(f"serial port still there {wait_s:g} s after bench op {op}")


def drill_failed_starts(ctx):
    limit = contract_value("CV_BOOT_FAIL_LIMIT")
    healthy_s = contract_value("CV_HEALTHY_MS") / 1000.0
    ctx.app()
    time.sleep(healthy_s + HEALTHY_MARGIN_S)  # the running start has reported healthy, so the count is 0
    trigger_reset(ctx, "w", RESET_WAIT_S)     # a fresh start; the faults land inside its healthy window
    seen = []
    for n in range(1, limit + 1):
        if wait_state(ctx, UPDATE_WAIT_S) != "app":
            return FAIL, f"the application did not start before fault {n} of {limit}", ""
        ctx.port = Port(wait_s=2.0, settle_s=FAST_SETTLE_S)
        seen.append(read_boot_fails(ctx.port))
        trigger_reset(ctx, "f", FAULT_WAIT_S)
    state = wait_state(ctx, UPDATE_WAIT_S)
    if state != "drive":
        return FAIL, f"after {limit} faults the box shows {state or 'neither port nor drive'}, want the drive", ""
    diag = parse_fields(read_drive_text(ctx.volume(), "DIAG.TXT"))
    reason, fault = diag.get("reason", "?"), diag.get("fault", "?")
    r = install(ctx, ctx.files["bench"], COPY_NAME)
    good = reason == "BootFailed" and not fault.startswith("none")
    return ((PASS if good else FAIL), f"started {limit} times, then update mode; DIAG.TXT reason {reason}, "
            f"fault {fault}; status boot_fails at each start {seen}; a full copy then restarted the box "
            f"({r.back_s:.1f} s)", "")


# -- drill 9: the layout

def drill_layout(ctx):
    before = get_layout(ctx.app())
    name = "drill9b" if before["name"] == "drill9" else "drill9"
    new = {"name": name, "slots": [before["slots"][1], before["slots"][0], *before["slots"][2:]]}
    ask_ok(ctx.port, "set_layout", **new)
    if layout_key(get_layout(ctx.port)) != layout_key(new):
        return FAIL, "get_layout differs from the layout set, before any update", ""
    enter_update_mode(ctx, UPDATE_WAIT_S)
    r = install(ctx, ctx.files["bench"], COPY_NAME)
    kept = layout_key(get_layout(ctx.port)) == layout_key(new)
    ask_ok(ctx.port, "set_layout", name=before["name"], slots=before["slots"])  # leaves the box as found
    return ((PASS if kept else FAIL), f"layout '{name}' with slots 1 and 2 swapped "
            f"{'survived' if kept else 'did NOT survive'} the update ({r.back_s:.1f} s); original layout set again", "")


# -- drills 10 to 12: the operator at the box

def countdown(seconds):
    """Sleeps with a progress line every 10 s, because the box is silent for the whole wait."""
    left = int(seconds)
    while left > 0:
        print(f"  waiting, {left} s left", flush=True)
        step = min(10, left)
        time.sleep(step)
        left -= step


def drill_menu_held(ctx):
    ctx.app()
    ctx.drop_port()
    if not ask_operator("Hold MENU, switch the 9 V off and on while holding it, "
                        "and release when the OLED says 'Drop the firmware on'."):
        return FAIL, "operator says MENU was not held through the power cycle", ""
    if cubevox_port.wait_volume(VOLUME, REPLUG_WAIT_S) is None:
        return FAIL, f"{VOLUME} did not appear {REPLUG_WAIT_S:g} s after power-on", ""
    reason = parse_fields(read_drive_text(ctx.volume(), "DIAG.TXT")).get("reason", "?")
    r = install(ctx, ctx.files["bench"], COPY_NAME)
    return ((PASS if reason == "MenuHeld" else FAIL),
            f"DIAG.TXT reason {reason}; a full copy then restarted the box ({r.back_s:.1f} s)", "")


def drill_no_computer(ctx):
    wait_s = contract_value("CV_UPDATE_TIMEOUT_MS") / 1000.0 + 10.0
    ctx.app()
    ctx.drop_port()
    # A host that enumerates switches the 60 s timeout off for good, and serial needs a host. The
    # menu with USB-C unplugged calls the same enterDfu() as the enter_dfu command.
    if not ask_operator("Unplug the USB-C cable, turn MENU to Update firmware, click twice, "
                        "and keep USB-C out until this script says to plug it in."):
        return FAIL, "operator says update mode was not entered", ""
    countdown(wait_s)
    print("  Plug the USB-C cable back in now.", flush=True)
    state = wait_state(ctx, REPLUG_WAIT_S)
    measured = f"{wait_s:g} s"
    if state == "app":
        return PASS, f"after {wait_s:g} s with no computer the serial port came back, no drive", measured
    if state == "drive":
        return FAIL, f"the box was still in update mode {wait_s:g} s after the click", measured
    return FAIL, f"neither a serial port nor the drive appeared {REPLUG_WAIT_S:g} s after the replug", measured


def drill_menu_entry(ctx):
    ctx.app()
    shown = f"'You have {ctx.fw}.'" if ctx.fw else "the installed version"
    ctx.drop_port()
    if not ask_operator(f"Turn MENU to Update firmware, click, check that the confirm screen shows "
                        f"{shown}, and click again."):
        return FAIL, "operator says the confirm screen or the click did not behave", ""
    if cubevox_port.wait_volume(VOLUME, UPDATE_WAIT_S) is None:
        return FAIL, f"{VOLUME} did not mount after the second click", ""
    r = install(ctx, ctx.files["bench"], COPY_NAME)
    return PASS, f"the second click opened the drive; a full copy then restarted the box ({r.back_s:.1f} s)", ""


# -- drill 13: what Adam does

def drill_release(ctx):
    version = release_version()
    uf2 = os.path.join(build_release(), "cubevox.uf2")
    stamped = stamp_version(uf2)
    if stamped != f"v{version}":
        return FAIL, f"the release image says {stamped} but VERSION.txt says v{version}; the build is stale", ""
    ensure_drive(ctx)
    r = install(ctx, uf2, f"cubevox-v{version}.uf2", PORT_RETURN_S)
    hello = ask_ok(ctx.port, "hello")
    note_hello(ctx, hello)
    want = (("build", "release"), ("fw", f"v{version}"), ("serial", ctx.uid or ctx.app_serial))
    wrong = [f"hello {key} is {hello.get(key)!r}, want {value!r}" for key, value in want if hello.get(key) != value]
    measured = f"{r.back_s:.1f} s"
    if wrong:
        return FAIL, "; ".join(wrong), measured
    return PASS, f"hello build release, fw v{version}, serial {hello.get('serial')}{copy_note(r)}", measured


# (number, name, function, needs the operator)
DRILLS = (
    (1, "enter_dfu opens the update drive", drill_enter_dfu, False),
    (2, "bench .uf2 copy restarts the application", drill_install_bench, False),
    (3, "USB pulled at 30 % of a copy", drill_usb_pull, True),
    (4, "9 V cut at 30 % of a copy", drill_power_cut, True),
    (5, "wrong base address refused", drill_wrong_base, False),
    (6, "other family refused", drill_other_family, False),
    (7, "flipped byte lands in update mode", drill_flipped_byte, False),
    (8, "failed starts land in update mode", drill_failed_starts, False),
    (9, "layout survives an update", drill_layout, False),
    (10, "MENU held at power-on", drill_menu_held, True),
    (11, "no computer, back to the application", drill_no_computer, True),
    (12, "menu entry and confirm screen", drill_menu_entry, True),
    (13, "release .uf2 copy", drill_release, False),
)


def run_drill(ctx, number, name, fn, operator):
    """Runs one drill and logs its verdict. A failure drops the port, so the next drill restores the box."""
    if operator and not ctx.prompt:
        ctx.log.drill(number, name, SKIPPED, "needs the operator; --no-prompt")
        return
    measured = ""
    try:
        status, detail, measured = fn(ctx)
    except (DrillError, *PORT_ERRORS) as e:
        status, detail = FAIL, str(e)
        print(f"[ERROR] drill {number}: {detail}", flush=True)
        ctx.drop_port()
    except Exception as e:  # a bug in one drill must not lose the others' results
        traceback.print_exc()
        status, detail = FAIL, f"unexpected {type(e).__name__}: {e}"
        ctx.drop_port()
    ctx.log.drill(number, name, status, detail, measured)


def update_drills(log, port, a):
    """Runs the 13 drills. Takes over `port`, the open port of the running bench application."""
    with tempfile.TemporaryDirectory(prefix="cubevox-update-") as tmp:
        ctx = Ctx(log, not a.no_prompt, tmp)
        ctx.port = port
        try:
            prepare_files(ctx)
            hello = ask_ok(ctx.port, "hello")
        except (DrillError, *PORT_ERRORS) as e:
            print(f"[ERROR] update drills: {e}", flush=True)
            log.check("update drills", False, str(e))
            for number, name, _, _ in DRILLS:
                log.drill(number, name, SKIPPED, "setup failed")
            ctx.drop_port()
            return
        note_hello(ctx, hello)
        ctx.app_serial = hello.get("serial")
        log.facts["UID"] = ctx.app_serial or "unknown"
        for number, name, fn, operator in DRILLS:
            run_drill(ctx, number, name, fn, operator)
        ctx.drop_port()


MANUAL = [
    ("input current at 9.6 V", "Bench supply current at 9.6 V, before USB (mA): "),
    ("placement orientation", "U1, SOT-23s, diodes, polarised caps match the silkscreen? (y/n, note): "),
    ("menu update entry", "MENU > Update firmware > two clicks open the CUBEVOX drive? (y/n/skip): "),
    ("OLED readable", "OLED shows the idle screen and the knob readout? (y/n/skip): "),
]
UPDATE_SCREENS = ("update-mode OLED screens",
                  "During the drills did the OLED show 'Drop the firmware on', 'The cable came out.', "
                  "'The update stopped.', 'Startup failed.', 'Writing the firmware.', "
                  "'That is not a cubevox' and 'The update is done.'? (y/n/skip): ")


def manual_questions(a):
    """The closing questions. The update drills answer the menu entry one and add the update screens."""
    questions = [q for q in MANUAL if not (a.update and q[0] == "menu update entry")]
    if a.update and not a.no_oled:
        questions.append(UPDATE_SCREENS)
    return questions


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--board", required=True, help="board number or label for the log name")
    ap.add_argument("--no-oled", action="store_true", help="skip the OLED check")
    ap.add_argument("--faults", action="store_true", help="drill audio stall, watchdog and bus fault")
    ap.add_argument("--dfu", action="store_true", help="drill ROM DFU entry by BOOT0 and flash back over it")
    ap.add_argument("--panel", action="store_true", help="walk every pot and toggle, then K103, mute and OLED")
    ap.add_argument("--update", action="store_true",
                    help="run the 13 update drills against the bootloader's drive; the log is board-N.md")
    ap.add_argument("--no-prompt", action="store_true", help="skip every question to the operator")
    ap.add_argument("--port", default=None, help="serial port, needed only when several cu.usbmodem* exist")
    a = ap.parse_args()
    if a.dfu and not shutil.which("dfu-util"):
        sys.exit("[ERROR] dfu-util not installed (brew install dfu-util)")  # before the operator holds BOOT0
    if a.port:
        os.environ[cubevox_port.PORT_ENV] = a.port  # flash.sh inherits it

    log = Log()
    try:
        run_checks(log, a)
    except PORT_ERRORS as e:
        log.check("serial port", False, str(e))
    return finish(log, a)


def run_checks(log, a):
    # The board gets the bench image for the drills and the release image at the end.
    flashed = flash_variant("bench")
    log.check("flash bench image", flashed, "build.sh VARIANT=bench + flash.sh")
    if not flashed:
        return
    try:
        port = Port()
    except PORT_ERRORS as e:
        log.check("USB enumeration", False, str(e))
        return
    log.check("USB enumeration", True, port.path)

    check_boot(log, boot_report(port), not a.no_oled)
    check_audio(log, port)
    if a.panel:
        panel_test(log, port, not a.no_prompt, not a.no_oled)
    if a.faults:
        port = fault_drill(log, port)
    if a.dfu:
        port = dfu_drill(log, port, not a.no_prompt)
        check_boot(log, boot_report(port), not a.no_oled)
    if a.update:
        update_drills(log, port, a)
    else:
        port.close()
    if not a.update:
        check_release_image(log)  # under --update, drill 13 is the release image check

    if not a.no_prompt:
        for name, question in manual_questions(a):
            log.note(name, input(question).strip() or "no answer")


def cell(text):
    """A Markdown table cell has no pipes and no line breaks."""
    return str(text).replace("|", "/").replace("\n", " ")


def markdown_log(log, a, stamp):
    """board-N.md: the unit's UID and versions, the checks as text and the drill table."""
    facts = {"Date": stamp, "UID": "unknown", "Bootloader": "unknown", "Firmware": "unknown", **log.facts}
    facts["Failed"] = log.failed
    lines = [f"# cubevox bring-up, board {a.board}", "", "| Item | Value |", "|---|---|"]
    lines += [f"| {key} | {cell(value)} |" for key, value in facts.items()]
    lines += ["", "## Checks", "", "```text", *log.rows, "```", "", "## Update drills", "",
              "| # | Drill | Result | Time | Detail |", "|---|---|---|---|---|"]
    lines += [f"| {n} | {cell(name)} | {status} | {cell(measured)} | {cell(detail)} |"
              for n, name, status, detail, measured in log.drills]
    return "\n".join(lines) + "\n"


def finish(log, a):
    stamp = datetime.datetime.now().strftime("%Y-%m-%d-%H%M")
    os.makedirs(BRINGUP_DIR, exist_ok=True)
    if a.update:
        path = os.path.join(BRINGUP_DIR, f"board-{a.board}.md")
        text = markdown_log(log, a, stamp)
    else:
        path = os.path.join(BRINGUP_DIR, f"{stamp}-board-{a.board}.txt")
        text = f"cubevox bring-up, board {a.board}, {stamp}\n" + "\n".join(log.rows) + "\n"
    with open(path, "w") as f:
        f.write(text)
    print(f"\n[bringup] {log.failed} failed; log {path}")
    print(f"[bringup] checks that need a person at the bench: {CHECKLIST}")
    return 1 if log.failed else 0


if __name__ == "__main__":
    sys.exit(main())
