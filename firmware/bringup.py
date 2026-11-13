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

--stream-faults raises stream faults with the bench ops stream_fault and fail_restart, each drill
from a fresh start past the 5 s healthy window: four faults inside 20 s give three restarts and then
the reset, a fourth fault after the minute restarts plainly, and a failed restart resets the box.
It reads the verdict from the port (the error lines, status `restarts` and `muted`, the next boot
report) and from DIAG.TXT on the update drive, so it needs the bootloader, and the MUTE toggle up.

--survival runs the drill table of docs/specs/2026-10-31-stage-survival-design.md section 3 hands-free
on stress.py's costliest layout with every slot forced on: the overload drills with the bench op burn,
then the restarts (stop_audio, spin, fault, fail_audio_start) and a bad update. It needs the bootloader
and the MUTE toggle up. Each drill reports PASS, FAIL or SKIPPED with its measured numbers, and the log lists them. --dry-run, with
--faults, --update or --survival, prints their drills (for --survival what each sends and checks) and
opens no port.

  ./bringup.py --board 1                 # USB checks, then the manual questions
  ./bringup.py --board 1 --faults --dfu  # also the fault drill and the BOOT0 ROM DFU entry (reflashes)
  ./bringup.py --board 1 --panel         # also the panel walk: pots, toggles, K103, mute, OLED
  ./bringup.py --board 1 --flood         # also 10 s of status requests at full speed, then a hello
  ./bringup.py --board 1 --faults --update   # also the update drills; log board-1.md
  ./bringup.py --board 1 --stream-faults --no-prompt   # the stream fault drills, hands-free
  ./bringup.py --board 1 --survival --no-prompt        # the survival drills, hands-free
  ./bringup.py --board 1 --survival --dry-run          # the survival drill plan; no port
"""

import argparse
import collections
import datetime
import fcntl
import functools
import importlib.util
import os
import re
import select
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
import stress  # noqa: E402

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


FLOOD_S = 10
FLOOD_ID = 900
FLOOD_REQUEST = b'{"id":900,"cmd":"status"}\n'
FLOOD_BATCH = 64


def flood_drill(log, port):
    """Status requests as fast as the port takes them for FLOOD_S, then a hello. The box answers in every
    second of the flood and after it; a receive path that sticks under a full queue answers nothing.
    Requests go out in batches, so the box's receive queue fills and the host waits on it."""
    per_second = [0] * FLOOD_S
    written = 0
    pending = b""
    start = time.monotonic()
    while (now := time.monotonic()) - start < FLOOD_S:
        pending = pending or FLOOD_REQUEST * FLOOD_BATCH
        if select.select([], [port.fd], [], 0.01)[1]:
            try:
                n = os.write(port.fd, pending)
                pending = pending[n:]
                written += n
            except BlockingIOError:
                pass
        while (line := port.read_line(0.001)) is not None:
            if line.startswith(f'{{"id":{FLOOD_ID},'):
                per_second[min(int(now - start), FLOOD_S - 1)] += 1
    port.read(2.0)  # the replies still on their way
    log.check("serial flood: replies every second", min(per_second) > 0,
              f"{written // len(FLOOD_REQUEST)} requests in {FLOOD_S} s, replies per second {per_second}")
    reply = port.ask("hello", 3.0)
    log.check("serial flood: hello after it", reply is not None and "_unparsed" not in reply,
              "answered" if reply else "no reply in 3 s")


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

    A complete copy leaves the drive mounted until the host ejects it (eject_drive). A box on the
    bootloader before v0.06 resets at the last block instead, so the OS may report an error for a
    copy that worked. Callers judge by the drive and the serial port.
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


def eject_drive(volume):
    """Ejects the update drive, which restarts a box that holds a complete update.

    A box on the bootloader before v0.06 has already restarted, so its eject fails. A failure is
    printed as a note and never raised; the drive leaving and the serial port returning decide.
    """
    r = subprocess.run(["diskutil", "eject", volume], capture_output=True, text=True)
    if r.returncode:
        text = " ".join((r.stderr or r.stdout).split())
        print(f"[bringup] diskutil eject {volume} exited {r.returncode}: {text}", flush=True)


def read_text(path):
    """A file's text, decoded as ASCII with undecodable bytes replaced."""
    with open(path, "rb") as f:
        return f.read().decode("ascii", "replace")


def read_drive_text(volume, name, wait_s=5.0):
    """Text of a file on the update drive. Retries while the drive settles after mounting."""
    path = os.path.join(volume, name)
    end = time.monotonic() + wait_s
    while True:
        try:
            return read_text(path)
        except OSError as e:
            if time.monotonic() >= end:
                raise DrillError(f"cannot read {path}: {e}") from e
        time.sleep(0.3)


def run_diskutil(*args):
    r = subprocess.run(["diskutil", *args], capture_output=True, text=True)
    if r.returncode:
        raise DrillError((r.stderr or r.stdout).strip() or f"diskutil {args[0]} exited {r.returncode}")


def remount_drive(wait_s=UPDATE_WAIT_S):
    """Unmounts and mounts the update drive, then waits until DIAG.TXT reads again.

    The bootloader regenerates INFO_UF2.TXT and DIAG.TXT on every mount. macOS serves the copy it
    read at mount time, so DIAG.TXT only refreshes on a remount. The unmount also flushes the host's
    pending writes, so a file just copied is fully counted on the next read.
    """
    run_diskutil("unmount", cubevox_port.volume_path(VOLUME))
    run_diskutil("mount", VOLUME)
    end = time.monotonic() + wait_s
    path = os.path.join(cubevox_port.volume_path(VOLUME), "DIAG.TXT")
    while True:
        if cubevox_port.volume_mounted(VOLUME):
            try:
                read_text(path)
                return
            except OSError:
                pass
        if time.monotonic() >= end:
            raise DrillError(f"{VOLUME} did not return with a readable DIAG.TXT {wait_s:g} s after the remount")
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


BLOCKS = re.compile(r"\d+ written, (\d+) refused, \d+ failed(?:, last family (0x[0-9A-Fa-f]+))?")


def parse_refused(fields):
    """(refused block count, last refused family id or None) from DIAG.TXT's 'blocks' line.

    The line reads `N written, N refused, N failed`. The bootloader adds `, last family 0x...` once a
    block was refused and `, first failure at 0x...` once a write failed. Without the line: (0, None).
    """
    m = BLOCKS.match(fields.get("blocks", ""))
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
    """Drags `src` onto the mounted drive, ejects it and waits for the box to restart into the application.

    Opens ctx.port. The bootloader restarts the box when the host ejects the drive after the last
    block. Only the drive and the serial port decide, whatever the OS reported.
    """
    ctx.drop_port()
    volume = ctx.volume()
    copy_error = copy_to_drive(volume, src, name)
    done = time.monotonic()
    eject_drive(volume)
    if not cubevox_port.wait_volume_gone(VOLUME, UPDATE_WAIT_S):
        raise DrillError(f"{VOLUME} is still mounted {UPDATE_WAIT_S:g} s after the copy and the eject"
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


def other_image(tmp, image):
    """A .uf2 of the stamped `image` under another header version, stamped again: the same code with another
    CRC, so the box takes it for a new image and the running image's mid-song mark does not count for it."""
    data = bytearray(read_bytes(image))
    try:
        c = stamp_image.Contract(stamp_image.DEFAULT_HEADER)
        version = stamp_image.parse_version(c, bytes(data), image)
        at = c.header_offset + c.off_version + len(version) - 1
        data[at] = ord("y") if data[at] == ord("x") else ord("x")
        stamped, _ = stamp_image.stamp_bytes(c, bytes(data), image)
    except stamp_image.ToolError as e:
        raise DrillError(str(e)) from e
    path = os.path.join(tmp, "other.bin")
    write_bytes(path, stamped)
    return rewrap(tmp, path, "other.uf2")


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
        "other": other_image(ctx.tmp, image),
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

    A refused file leaves the box in update mode, so a drive that vanishes is a failure. DIAG.TXT
    only changes on a mount, so each read follows a remount. The drive is absent for a moment
    inside the remount, so the vanished-drive check runs just before it.
    """
    blocks = os.path.getsize(path) // stamp_image.UF2_BLOCK_BYTES
    error = copy_to_drive(ctx.volume(), path, COPY_NAME)
    end = time.monotonic() + UPDATE_WAIT_S
    while True:
        if not cubevox_port.volume_mounted(VOLUME):
            raise DrillError(f"the drive went away after the copy; the box restarted instead of refusing"
                             f"{os_note(error)}")
        remount_drive()
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
            seen = parse_fields(read_text(path)).get("installed")
        except OSError as e:
            error = str(e)  # the drive is between two sessions while the box restarts
        if seen == want or time.monotonic() >= end:
            return seen, error
        time.sleep(0.5)


def drill_flipped_byte(ctx):
    ensure_drive(ctx)
    volume = ctx.volume()
    copy_error = copy_to_drive(volume, ctx.files["flipped"], COPY_NAME)
    # the image check after the last block fails, so the box keeps the drive; a remount rebuilds INFO_UF2.TXT
    remount_drive()
    seen, read_error = wait_installed(ctx, "invalid", UPDATE_WAIT_S)
    r = install(ctx, ctx.files["bench"], COPY_NAME)
    got = f"installed '{seen}'" + (f" (last read error: {read_error})" if seen != "invalid" and read_error else "")
    detail = (f"after the flipped copy the remounted drive reports {got}; a full copy then restarted the box "
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
    """A new image that faults at each start lands in update mode after CV_BOOT_FAIL_LIMIT starts.

    The running image runs past the healthy window first, so it carries the mid-song mark, and its own
    faults would restart the box as it was. The new image is ctx.files["other"], another CRC, so the mark
    does not count for it. The update drills (8) and the survival drills (Bad update) both run this.
    """
    limit = contract_value("CV_BOOT_FAIL_LIMIT")
    ctx.app()
    time.sleep(contract_value("CV_HEALTHY_MS") / 1000.0 + HEALTHY_MARGIN_S)  # the running image's mark is written
    enter_update_mode(ctx, UPDATE_WAIT_S)  # the path the menu's Update firmware item takes
    install(ctx, ctx.files["other"], COPY_NAME)
    seen, state = [], "app"
    while state == "app" and len(seen) <= limit:
        if ctx.port is None:
            ctx.port = Port(wait_s=2.0, settle_s=FAST_SETTLE_S)
        seen.append(read_boot_fails(ctx.port))
        trigger_reset(ctx, "f", FAULT_WAIT_S)  # the new image faults as soon as its port opens
        state = wait_state(ctx, UPDATE_WAIT_S)
    measured = f"{len(seen)} starts"
    if state != "drive":
        return (FAIL, f"after {len(seen)} faulting starts the box shows {state or 'neither port nor drive'}, "
                f"want the drive", measured)
    diag = parse_fields(read_drive_text(ctx.volume(), "DIAG.TXT"))
    reason, fault = diag.get("reason", "?"), diag.get("fault", "?")
    r = install(ctx, ctx.files["bench"], COPY_NAME)
    good = len(seen) == limit and reason == "BootFailed" and not fault.startswith("none")
    return ((PASS if good else FAIL), f"the new image faulted at {len(seen)} starts (want {limit}), then update "
            f"mode; DIAG.TXT reason {reason}, fault {fault}; status boot_fails at each start {seen}; the bench "
            f"image then restarted the box ({r.back_s:.1f} s)", measured)


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


# -- drills 10 to 12: drill 10 is removed; 11 and 12 need the operator at the box

def countdown(seconds):
    """Sleeps with a progress line every 10 s, because the box is silent for the whole wait."""
    left = int(seconds)
    while left > 0:
        print(f"  waiting, {left} s left", flush=True)
        step = min(10, left)
        time.sleep(step)
        left -= step


def drill_removed(ctx):
    return SKIPPED, "removed: bootloader v0.09 does not read MENU at a start", ""


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
    (8, "a new image's failed starts land in update mode", drill_failed_starts, False),
    (9, "layout survives an update", drill_layout, False),
    (10, "removed (no MENU path since bootloader v0.09)", drill_removed, False),
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
    """Runs the drills 1 to 13 (10 is removed). Takes over `port`, the open port of the running bench application."""
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


# -- stream fault drills (--stream-faults): the restart budget and the reset it ends in
# The bench ops stream_fault and fail_restart raise the faults; the port and DIAG.TXT give the verdict.

RESTARTS_PER_WINDOW = 3          # core/audio_recovery.h kRestartsPerWindow
RESTART_WINDOW_S = 60.0          # core/audio_recovery.h kRestartWindowMs
RELAY_AFTER_START_S = 6.0        # core/timing.h kRelayCloseAfterPowerOnMs
SOFTWARE_RESET = 1 << 24         # RCC_RSR SFTRSTF, the bootloader's CV_RSR_SFTRSTF
CAUSE_RESTARTED = "stream restarted"            # board/fault.cpp, the audio entries' fault text
CAUSE_OUT_OF_RESTARTS = "A1 repeated stream faults"   # core/audio_fault_cause.h, the code first
CAUSE_RESTART_FAILED = "A2 stream restart failed"
FAULT_GAP_S = 2.0                # between faults: past the mute, the restart and the release
WINDOW_MARGIN_S = 2.0            # past the minute, so the oldest restart has left the window
RESET_GONE_S = 8.0               # the planned reset waits up to 3 s for the stores, then drops the port
RELAY_MIN_S = RELAY_AFTER_START_S - 1.0   # from the port dropping to status muted false
RELAY_MAX_S = RELAY_AFTER_START_S + 4.0   # plus the bootloader, setup and the relay settle
RELAY_WAIT_S = 15.0
RESTART_LINE = r"\[WARN\] audio: restarting the stream after"
OUT_OF_RESTARTS_LINE = r"\[ERROR\] audio: another fault inside a minute of three restarts; resetting"
RESTART_FAILED_LINE = r"\[ERROR\] audio: .*; resetting"
RESET_LINE = r"^\[boot\] reset:.*software"

DiagEntry = collections.namedtuple("DiagEntry", "sequence kind firmware up rst fails reason fault sai ovr")
DIAG_ENTRY = re.compile(r"^#(\d+) (\S+) (\S+) up=(\S+) rst=(\S+) fails=(\S+) (\S+) fa=\S+ (.*?) "
                        r"sai=(\S+) ovr=(\S+)(?: ob=.*)?$")


def count_value(text):
    """A decimal count field, or None for the '?' of a value nobody knew."""
    return int(text) if text.isdigit() else None


def parse_diag_entries(text):
    """DIAG.TXT's entry lines (`#sequence kind firmware up= rst= fails= reason fa= fault sai= ovr=`), newest first."""
    entries = []
    for line in text.replace("\r", "").split("\n"):
        m = DIAG_ENTRY.match(line.strip())
        if m:
            entries.append(DiagEntry(int(m.group(1)), m.group(2), m.group(3), m.group(4), hex_value(m.group(5)),
                                     m.group(6), m.group(7), m.group(8), count_value(m.group(9)),
                                     count_value(m.group(10))))
    return entries


def judge_stream_reset(entries, cause, restarts):
    """Whether the entries, newest first, show a reset an audio fault started. Returns (ok, detail).

    The newest audio entry names `cause`, a boot entry after a software reset sits above it, and the
    `restarts` entries below it are audio entries reading CAUSE_RESTARTED. Update entries from the
    drill's own trip to the drive may sit on top.
    """
    at = next((i for i, e in enumerate(entries) if e.kind == "audio"), None)
    if at is None:
        return False, "DIAG.TXT lists no audio entry"
    fault = entries[at]
    if fault.fault != cause:
        return False, f"newest audio entry #{fault.sequence} reads '{fault.fault}', want '{cause}'"
    boots = [e for e in entries[:at] if e.kind == "boot"]
    if not boots:
        return False, f"no boot entry after audio entry #{fault.sequence}"
    boot = boots[-1]
    if boot.rst is None or not boot.rst & SOFTWARE_RESET:
        return False, f"boot entry #{boot.sequence} has rst={boot.rst}, want a software reset"
    before = entries[at + 1:at + 1 + restarts]
    if len(before) < restarts or any(e.kind != "audio" or e.fault != CAUSE_RESTARTED for e in before):
        seen = [f"#{e.sequence} {e.kind} '{e.fault}'" for e in before]
        return False, f"want {restarts} '{CAUSE_RESTARTED}' entries below #{fault.sequence}, DIAG.TXT has {seen}"
    tail = f", below it {restarts} '{CAUSE_RESTARTED}'" if restarts else ""
    return True, f"#{boot.sequence} boot after a software reset, #{fault.sequence} audio '{cause}'{tail}"


def fresh_start(ctx):
    """Resets the box by the watchdog op and waits past the healthy window, so the start counts healthy
    and the restart budget is full."""
    ctx.app()
    trigger_reset(ctx, "w", RESET_WAIT_S)
    if wait_state(ctx, UPDATE_WAIT_S) != "app":
        raise DrillError("the application did not come back after the watchdog reset")
    ctx.port = Port(wait_s=2.0)
    time.sleep(contract_value("CV_HEALTHY_MS") / 1000.0 + HEALTHY_MARGIN_S)
    return ctx.port


def raise_fault(port, expect):
    """Sends stream_fault and returns the first line matching `expect`, or None after FAULT_WAIT_S."""
    port.send(bench_ops.op_line("stream_fault"))
    return find(port.read(FAULT_WAIT_S, until=expect), expect)


def status_field(port, name):
    reply = port.ask("status", 2.0)
    if not isinstance(reply, dict) or name not in reply:
        raise DrillError(f"status: no {name} in {str(reply)[:120]}")
    return reply[name]


def seconds_until_unmuted(port, since, seen):
    """Seconds from `since` until status reads muted false, or None after RELAY_WAIT_S. Log lines go to `seen`."""
    while time.monotonic() - since < RELAY_WAIT_S:
        reply = port.ask("status", 1.0, on_text=seen.append)
        if isinstance(reply, dict) and reply.get("muted") is False:
            return time.monotonic() - since
        time.sleep(PORT_POLL_S)
    return None


def follow_reset(ctx, expect):
    """After the op that starts an audio reset: the error line, the port dropping, the software reset in
    the next boot report, and the seconds from the drop until status reads muted false (or None)."""
    path = ctx.port.path
    line = find(ctx.port.read(RESET_GONE_S), expect)  # returns early when the reset drops the port
    if line is None:
        raise DrillError(f"no line matching '{expect}' before the reset")
    ctx.drop_port()
    if not wait_port_gone(path, 1.0):
        raise DrillError(f"serial port still there {RESET_GONE_S:g} s after the fault")
    gone = time.monotonic()
    if wait_port(UPDATE_WAIT_S) is None:
        raise DrillError(f"no serial port {UPDATE_WAIT_S:g} s after the reset")
    ctx.port = Port(wait_s=1.0, settle_s=FAST_SETTLE_S)
    seen = []
    relay_s = seconds_until_unmuted(ctx.port, gone, seen)
    if find(seen, RESET_LINE) is None and find(boot_report(ctx.port), RESET_LINE) is None:
        raise DrillError("the boot report after the fault shows no software reset")
    return line.group(0), relay_s


def relay_note(relay_s):
    """(ok, text) for the time from the reset until the output unmuted."""
    if relay_s is None:
        return False, f"status still reads muted {RELAY_WAIT_S:g} s after the reset (is the MUTE toggle down?)"
    ok = RELAY_MIN_S <= relay_s <= RELAY_MAX_S
    return ok, f"unmuted {relay_s:.1f} s after the reset, want {RELAY_MIN_S:g} to {RELAY_MAX_S:g} s"


def diag_entries_now(ctx):
    """DIAG.TXT's entries from the update drive; the bench image goes back on after."""
    enter_update_mode(ctx, UPDATE_WAIT_S)
    entries = parse_diag_entries(read_drive_text(ctx.volume(), "DIAG.TXT"))
    install(ctx, ctx.files["bench"], COPY_NAME)
    return entries


def drill_restart_budget(ctx):
    """Four faults inside 20 s, more than 5 s after a start: three restarts, then the reset."""
    port = fresh_start(ctx)
    for n in range(1, RESTARTS_PER_WINDOW + 1):
        if raise_fault(port, RESTART_LINE) is None:
            return False, f"fault {n}: no restart line in {FAULT_WAIT_S:g} s"
        time.sleep(FAULT_GAP_S)
    restarts = status_field(port, "restarts")
    if restarts != RESTARTS_PER_WINDOW:
        return False, f"status restarts {restarts} after {RESTARTS_PER_WINDOW} faults"
    port.send(bench_ops.op_line("stream_fault"))
    line, relay_s = follow_reset(ctx, OUT_OF_RESTARTS_LINE)
    relay_ok, relay = relay_note(relay_s)
    diag_ok, diag = judge_stream_reset(diag_entries_now(ctx), CAUSE_OUT_OF_RESTARTS, RESTARTS_PER_WINDOW)
    return relay_ok and diag_ok, f"{RESTARTS_PER_WINDOW} restarts, then '{line}'; {relay}; DIAG.TXT: {diag}"


def drill_budget_refills(ctx):
    """Three faults, then a fourth once the minute has passed: a plain restart, no reset."""
    port = fresh_start(ctx)
    first = None
    for n in range(1, RESTARTS_PER_WINDOW + 1):
        if raise_fault(port, RESTART_LINE) is None:
            return False, f"fault {n}: no restart line in {FAULT_WAIT_S:g} s"
        first = first if first is not None else time.monotonic()
        time.sleep(FAULT_GAP_S)
    time.sleep(max(0.0, first + RESTART_WINDOW_S + WINDOW_MARGIN_S - time.monotonic()))
    either = f"{RESTART_LINE}|{OUT_OF_RESTARTS_LINE}"
    line = raise_fault(port, either)
    if line is None or re.search(OUT_OF_RESTARTS_LINE, line.group(0)):
        return False, f"the fault after the minute gave {line.group(0) if line else 'no line'}, want a restart"
    restarts = status_field(port, "restarts")
    if restarts != RESTARTS_PER_WINDOW + 1:
        return False, f"status restarts {restarts}, want {RESTARTS_PER_WINDOW + 1}"
    return True, f"a fault {RESTART_WINDOW_S + WINDOW_MARGIN_S:g} s after the first restarted the stream; restarts {restarts}"


def drill_failed_restart(ctx):
    """fail_restart, then one fault: the restart fails and the box resets."""
    port = fresh_start(ctx)
    port.send(bench_ops.op_line("fail_restart"))
    if find(port.read(FAULT_WAIT_S, until=r"next stream restart fails"), r"next stream restart fails") is None:
        return False, "fail_restart did not answer '[bench] the next stream restart fails'"
    port.send(bench_ops.op_line("stream_fault"))
    line, relay_s = follow_reset(ctx, RESTART_FAILED_LINE)
    relay_ok, relay = relay_note(relay_s)
    diag_ok, diag = judge_stream_reset(diag_entries_now(ctx), CAUSE_RESTART_FAILED, 0)
    return relay_ok and diag_ok, f"'{line}'; {relay}; DIAG.TXT: {diag}"


STREAM_DRILLS = (
    ("stream faults past the budget reset the box", drill_restart_budget),
    ("a fault after the minute restarts plainly", drill_budget_refills),
    ("a failed restart resets the box", drill_failed_restart),
)


def stream_fault_drills(log, port):
    """Runs the stream fault drills on the bench image. Takes over `port` and returns the open port of the
    running application, or None when the box could not be brought back."""
    with tempfile.TemporaryDirectory(prefix="cubevox-stream-") as tmp:
        ctx = Ctx(log, False, tmp)
        ctx.port = port
        try:
            ctx.files = {"bench": os.path.join(fw_outdir("bench"), "cubevox.uf2")}
        except DrillError as e:
            log.check("stream fault drills", False, str(e))
            return ctx.port
        for name, fn in STREAM_DRILLS:
            try:
                ok, detail = fn(ctx)
            except (DrillError, *PORT_ERRORS) as e:
                ok, detail = False, str(e)
                ctx.drop_port()
            log.check(name, ok, detail)
        try:
            return ctx.app()
        except (DrillError, *PORT_ERRORS) as e:
            log.check("back to the application after the stream drills", False, str(e))
            return None


# -- survival drills (--survival): docs/specs/2026-10-31-stage-survival-design.md section 3
# The restart code, the warm start and its times come from the boot report; the overload net from the
# status fields held, steps, late and muted. The screen has no readout over the port, so the OVERLOAD
# row and the tick are not read.

SURVIVAL_LAYOUT = "costliest"   # stress.py layout_table()
SURVIVAL_CLEANUP_EQ = "strong"  # the setting the stress run measures the layout at
GAP_LIMIT_MS = 1500             # the spec's bound on the gap; the box times setup() to the relay
WATCHDOG_S = 2.0                # board/fault.cpp kWatchdogMs: sound this long, then the restart
WATCHDOG_TOL_S = 1.0
STEP_LIMIT_S = 0.12             # an overload step lands inside 120 ms
STEP_WAIT_S = 2.0
LOAD_WAIT_FIRST_S = 2.0         # core/load_rule.h kLoadWaitFirstMs
LOAD_WAIT_CAP_S = 16.0          # core/load_rule.h kLoadWaitCapMs
LOAD_RESET_AFTER_S = 60.0       # core/load_rule.h kLoadResetAfterMs: a card back this long starts at 2 s again
LOAD_WAIT_TOL_S = 0.5
LOAD_HOLDS = 5                  # the waits 2, 4, 8, 16 and 16 s
OVERLOAD_STAYS_S = 60.0
RETURN_WAIT_S = 6.0
MAX_BURN_CYCLES = 1000000       # core/bench_ops.h kMaxBenchBurnCycles
SAVE_LOAD = 0.80                # the burn brings the mean block to 80 % of the budget
SAVES_UNDER_LOAD = 20
SAVE_GAP_S = 0.5
RUN_BEFORE_REPEAT_S = 10.0
REPEAT_FAULTS = 5
REPEAT_GAP_S = 2.0
STATS_SETTLE_S = 2.0
STATUS_POLL_S = 0.02
LATE_SETTLE_S = 0.5
BURN_CARD = "crowd"             # Mob, slot 4 of the costliest layout: the overload drills burn in its slot
BURN_MARGIN = 0.10              # the slot burn puts the mean block this far past the budget, plus the spread
BURN_BLOCK_CAP = 1.5            # the chain's worst block and the burn together stay under this many blocks
STEP_SETTLE_S = 0.5             # after the step, no second step while the burnt card is out
OUTSIDE_LOAD = 1.10             # the burn outside the cards, as a share of the block budget
OUTSIDE_S = 20.0
NOT_TAKEABLE = ("empty", "input_gate")  # the load rule never takes the Input gate out
START_LINE = r"^\[boot\] start: (warm|cold).*$"
CODE_LINE = r"^\[boot\] restart code (\S+)"
REPORT_END = r"^\[(boot\] pots|ERROR\] pots:)"


class SurvivalCtx(Ctx):
    """Ctx plus the costliest layout and what the overload drills hand on."""

    def __init__(self, log, tmp, slots):
        super().__init__(log, False, tmp)
        self.slots = slots
        self.budget = None   # cycles in one block
        self.burn = None     # cycles a block, in the burnt slot, that make every block late while its card runs
        self.burn_slot = None  # the slot, counted from 0, that the burn is tied to and the step takes out
        self.held_at = None  # host time the overload step was seen
        self.back_at = None  # host time the held card came back


def bench_ask(port, op, wait_s=5.0, **args):
    """Sends a bench op that must answer ok. Returns the text lines the box printed before the reply."""
    lines = []
    reply = port.ask("bench", wait_s, lines.append, op=op, **args)
    if not isinstance(reply, dict) or reply.get("ok") is not True:
        raise DrillError(f"bench {op}: {str(reply)[:160]}")
    return lines


def status_now(port):
    reply = port.ask("status", 1.0)
    if not isinstance(reply, dict) or reply.get("ok") is not True:
        raise DrillError(f"status: {str(reply)[:120]}")
    return reply


def poll_status(port, since, seconds, stop=None):
    """(seconds after `since`, status reply) pairs until `seconds` after `since`, or until stop(reply) holds."""
    seen = []
    while time.monotonic() - since < seconds:
        reply = status_now(port)
        seen.append((time.monotonic() - since, reply))
        if stop and stop(reply):
            break
        time.sleep(STATUS_POLL_S)
    return seen


def layout_state(port):
    reply = ask_ok(port, "get_layout")
    return reply.get("layout"), reply.get("settings")


def card_at(ctx, slot):
    return f"slot {slot + 1} ({ctx.slots[slot]})"


def held_slots(mask):
    return [s for s in range(stress.SLOTS) if mask >> s & 1]


# -- the restarts

def start_report(port):
    """A fresh boot report from the running box."""
    port.read(0.3)
    port.send("r")
    return port.read(4.0, until=REPORT_END)


def come_back(ctx, gone, drops):
    """After the reset that dropped the port at host time `gone`: (seconds from the drop until status reads
    muted false, or None; the boot report). Rides out `drops` more resets, as A4's second restart."""
    last = None
    for _ in range(drops + 2):
        if wait_state(ctx, UPDATE_WAIT_S) != "app":
            raise DrillError(f"no serial port {UPDATE_WAIT_S:g} s after the reset (update mode, or no start)")
        try:
            ctx.port = Port(wait_s=1.0, settle_s=FAST_SETTLE_S)
            unmuted_s = seconds_until_unmuted(ctx.port, gone, [])
            return unmuted_s, start_report(ctx.port)
        except PORT_ERRORS as e:
            last = e
            path = ctx.port.path if ctx.port is not None else None
            ctx.drop_port()
            if path:
                wait_port_gone(path, RESET_GONE_S)
    raise DrillError(f"the box did not stay up after the reset: {last}")


def judge_restart(lines, want_code, before, after, unmuted_s):
    """(ok, detail, measured) for one return: a warm start, the code, the same layout and settings, the panel
    read again, and setup() to the relay inside the gap bound."""
    start = find(lines, START_LINE)
    code = find(lines, CODE_LINE)
    relay = re.search(r"relay (\d+) ms", start.group(0)) if start else None
    snapshot = re.search(r"first snapshot (\d+) ms", start.group(0)) if start else None
    relay_ms = int(relay.group(1)) if relay else None
    got = code.group(1) if code else "none"
    wrong = []
    if not start or start.group(1) != "warm":
        wrong.append(f"start {start.group(1) if start else 'line missing'}, want warm")
    if got != want_code:
        wrong.append(f"code {got}, want {want_code}")
    if after != before:
        wrong.append("the layout or its settings changed")
    if snapshot is None:
        wrong.append("no first snapshot of the panel")
    if relay_ms is None or relay_ms > GAP_LIMIT_MS:
        wrong.append(f"setup() to relay {relay_ms if relay_ms is not None else 'not reported'}, limit {GAP_LIMIT_MS} ms")
    if unmuted_s is None:
        wrong.append(f"status still muted {RELAY_WAIT_S:g} s after the reset")
    host = f"{unmuted_s:.2f} s" if unmuted_s is not None else "never"
    detail = (f"{start.group(0) if start else 'no start line'}; code {got}; host saw status unmuted {host} after "
              f"the port dropped, USB enumeration included")
    measured = f"setup() to relay {relay_ms} ms" if relay_ms is not None else "relay not reported"
    return not wrong, "; ".join(wrong + [detail]), measured


def restart_drill(ctx, send, want_code, drops=0):
    """Sends the op that ends in a restart and judges the return. Returns (ok, detail, measured, seconds
    from the op to the port dropping)."""
    port = ctx.app()
    before = layout_state(port)
    path = port.path
    sent = time.monotonic()
    send(port)
    port.read(RESET_GONE_S)  # returns early when the reset drops the port
    gone = time.monotonic()
    ctx.drop_port()
    if not wait_port_gone(path, 1.0):
        raise DrillError(f"serial port still there {RESET_GONE_S:g} s after the op")
    unmuted_s, lines = come_back(ctx, gone, drops)
    bench_ask(ctx.port, "all_on")  # a restart drops the bench's forced slots
    ok, detail, measured = judge_restart(lines, want_code, before, layout_state(ctx.port), unmuted_s)
    return ok, detail, measured, gone - sent


def send_letter(letter):
    return lambda port: port.send(letter)


def drill_a3(ctx):
    ok, detail, measured, _ = restart_drill(ctx, send_letter("a"), "A3")
    return (PASS if ok else FAIL), detail, measured


def drill_w1(ctx):
    ok, detail, measured, drop_s = restart_drill(ctx, send_letter("w"), "W1")
    on_time = abs(drop_s - WATCHDOG_S) <= WATCHDOG_TOL_S
    detail = f"port dropped {drop_s:.1f} s after spin, want {WATCHDOG_S:g} s; {detail}"
    return (PASS if ok and on_time else FAIL), detail, f"{measured}, the watchdog after {drop_s:.1f} s"


def drill_c1(ctx):
    ok, detail, measured, _ = restart_drill(ctx, send_letter("f"), "C1")
    entries = diag_entries_now(ctx)
    bench_ask(ctx.app(), "all_on")
    boot = next((e for e in entries if e.kind == "boot"), None)
    entry_ok = boot is not None and boot.fault.startswith("C1")
    diag = f"DIAG.TXT newest boot entry #{boot.sequence} '{boot.fault}'" if boot else "DIAG.TXT has no boot entry"
    return (PASS if ok and entry_ok else FAIL), f"{detail}; {diag}", measured


def drill_c1_repeated(ctx):
    ctx.app()
    time.sleep(RUN_BEFORE_REPEAT_S)  # past the 5 s that write the mid-song mark
    gaps, wrong = [], []
    for n in range(1, REPEAT_FAULTS + 1):
        ok, detail, measured, _ = restart_drill(ctx, send_letter("f"), "C1")
        gaps.append(measured)
        if not ok:
            wrong.append(f"fault {n}: {detail}")
        time.sleep(REPEAT_GAP_S)
    if wrong:
        return FAIL, "; ".join(wrong), "; ".join(gaps)
    return PASS, f"{REPEAT_FAULTS} restarts, each warm with C1 and the same layout, never update mode", "; ".join(gaps)


def drill_a4(ctx):
    bench_ask(ctx.app(), "fail_audio_start")
    ok, detail, measured, _ = restart_drill(ctx, send_letter("f"), "A4", drops=1)
    return (PASS if ok else FAIL), detail, measured


# -- the overload net

def chain_stats(port):
    """The chain's block and per-slot cycles over STATS_SETTLE_S, with no burn."""
    bench_ask(port, "reset_peaks")
    time.sleep(STATS_SETTLE_S)
    try:
        return stress.parse_stats(bench_ask(port, "stats"))
    except stress.StressError as e:
        raise DrillError(str(e)) from e


def size_burn(ctx, stats):
    """The burn in Mob's slot that puts every block past the budget while Mob runs: the budget less the mean
    block, plus the block-to-block spread and BURN_MARGIN. It spins only while Mob runs, so once the rule
    takes Mob out the blocks fall back to the chain alone and none is late."""
    if BURN_CARD not in ctx.slots:
        raise DrillError(f"the {SURVIVAL_LAYOUT} layout has no {BURN_CARD} card to burn in")
    ctx.burn_slot = ctx.slots.index(BURN_CARD)
    ctx.budget = stats.budget
    spread = max(0, stats.worst - stats.avg)
    burn = stats.budget - stats.avg + spread + int(BURN_MARGIN * stats.budget)
    # A block past two block times would make the next one overrun, and overruns mute the box.
    burn = min(burn, int(BURN_BLOCK_CAP * stats.budget) - stats.worst)
    ctx.burn = max(0, min(MAX_BURN_CYCLES, burn))


def percent(cycles, budget):
    return f"{100.0 * cycles / budget:.1f} %"


def burn_in_slot(port, ctx, cycles):
    bench_ask(port, "burn", slot=ctx.burn_slot + 1, cycles=cycles)


def slowest_takeable(ctx, stats):
    """The takeable card with the most mean cycles, Autotune aside, as the burnt card is compared with."""
    takeable = [s for s, card in enumerate(ctx.slots) if card not in NOT_TAKEABLE + ("autotune",)]
    return max(takeable, key=lambda s: stats.slot_avg[s])


def drill_overload(ctx):
    port = ctx.app()
    stats = chain_stats(port)
    size_burn(ctx, stats)
    slow = slowest_takeable(ctx, stats)
    before = status_now(port)
    sent = time.monotonic()
    burn_in_slot(port, ctx, ctx.burn)
    seen = poll_status(port, sent, STEP_WAIT_S, lambda r: r.get("steps", 0) > before["steps"])
    plan = (f"burn {ctx.burn} cycles a block ({percent(ctx.burn, stats.budget)}) in {card_at(ctx, ctx.burn_slot)} on "
            f"a chain averaging {percent(stats.avg, stats.budget)}, worst {percent(stats.worst, stats.budget)}; "
            f"slowest card by its own mean {card_at(ctx, slow)}")
    if not seen or seen[-1][1]["steps"] <= before["steps"]:
        return FAIL, f"{plan}; no step in {STEP_WAIT_S:g} s", ""
    step_s = seen[-1][0]
    ctx.held_at = sent + step_s
    seen += poll_status(port, sent, step_s + STEP_SETTLE_S)
    steps = seen[-1][1]["steps"] - before["steps"]
    masks = {r["held"] for _, r in seen if r.get("held")}
    muted = any(r.get("muted") for _, r in seen)
    ok = step_s <= STEP_LIMIT_S and masks == {1 << ctx.burn_slot} and steps == 1 and not muted
    held = ", ".join(card_at(ctx, s) for m in sorted(masks) for s in held_slots(m)) or "nothing"
    detail = (f"{plan}; step {1000 * step_s:.0f} ms after the burn, limit {1000 * STEP_LIMIT_S:.0f} ms; held {held}, "
              f"want {card_at(ctx, ctx.burn_slot)} alone; {steps} steps in {step_s + STEP_SETTLE_S:.1f} s, want 1; "
              f"{'MUTED' if muted else 'never muted'}; the OVERLOAD row is not read")
    return (PASS if ok else FAIL), detail, f"{1000 * step_s:.0f} ms to the step"


def drill_overload_ends(ctx):
    if ctx.held_at is None:
        return SKIPPED, "the overload drill held no card", ""
    port = ctx.app()
    bench_ask(port, "burn", cycles=0)
    seen = poll_status(port, ctx.held_at, RETURN_WAIT_S, lambda r: r.get("held") == 0)
    if not seen or seen[-1][1]["held"] != 0:
        return FAIL, f"the card is still held {RETURN_WAIT_S:g} s after it went", ""
    back_s = seen[-1][0]
    ctx.back_at = ctx.held_at + back_s
    ok = abs(back_s - LOAD_WAIT_FIRST_S) <= LOAD_WAIT_TOL_S
    return ((PASS if ok else FAIL), f"the card came back {back_s:.2f} s after it went, want {LOAD_WAIT_FIRST_S:g} s "
            f"± {LOAD_WAIT_TOL_S:g} s; the row is not read", f"{back_s:.2f} s to return")


def drill_one_late(ctx):
    port = ctx.app()
    budget = ctx.budget or chain_stats(port).budget
    cycles = min(budget, MAX_BURN_CYCLES)
    before = status_now(port)
    bench_ask(port, "burn", cycles=cycles, blocks=1)
    time.sleep(LATE_SETTLE_S)
    after = status_now(port)
    late, steps = after["late"] - before["late"], after["steps"] - before["steps"]
    return ((PASS if late == 1 and steps == 0 else FAIL),
            f"burn {cycles} cycles for one block: {late} late, {steps} steps, want 1 and 0; the tick is not heard",
            f"{late} late blocks")


def drill_save_under_load(ctx):
    port = ctx.app()
    stats = chain_stats(port)
    burn = max(0, min(MAX_BURN_CYCLES, int(SAVE_LOAD * stats.budget) - stats.avg))
    before = status_now(port)
    bench_ask(port, "burn", cycles=burn)
    for _ in range(SAVES_UNDER_LOAD):
        bench_ask(port, "record_save")
        time.sleep(SAVE_GAP_S)
    bench_ask(port, "burn", cycles=0)
    after = status_now(port)
    late, steps = after["late"] - before["late"], after["steps"] - before["steps"]
    return ((PASS if late == 0 and steps == 0 else FAIL),
            f"{SAVES_UNDER_LOAD} record_save {SAVE_GAP_S:g} s apart under a burn of {burn} cycles a block, mean block "
            f"{percent(stats.avg + burn, stats.budget)}: {late} late, {steps} steps, want 0 and 0", f"{late} late blocks")


def hold_lengths(seen):
    """The lengths in seconds of the holds that began and ended inside `seen`, and every held mask seen."""
    lengths, masks, began = [], set(), None
    for t, reply in seen:
        held = reply.get("held", 0)
        if held:
            masks.add(held)
            began = t if began is None else began
        elif began is not None:
            lengths.append(t - began)
            began = None
    return lengths, masks


def load_waits():
    """The waits the load rule gives one card taken out again and again: 2, 4, 8, 16, 16 s."""
    waits, wait = [], LOAD_WAIT_FIRST_S
    while len(waits) < LOAD_HOLDS:
        waits.append(wait)
        wait = min(2 * wait, LOAD_WAIT_CAP_S)
    return waits


def drill_overload_stays(ctx):
    if ctx.burn is None:
        return SKIPPED, "the overload drill sized no burn", ""
    port = ctx.app()
    if ctx.back_at is not None:
        time.sleep(max(0.0, ctx.back_at + LOAD_RESET_AFTER_S + HEALTHY_MARGIN_S - time.monotonic()))
    before = status_now(port)
    sent = time.monotonic()
    burn_in_slot(port, ctx, ctx.burn)
    seen = poll_status(port, sent, OVERLOAD_STAYS_S)
    bench_ask(port, "burn", cycles=0)
    holds, masks = hold_lengths(seen)
    want, after = load_waits(), seen[-1][1] if seen else before
    wrong = []
    if len(holds) < LOAD_HOLDS or any(abs(g - w) > LOAD_WAIT_TOL_S for g, w in zip(holds, want)):
        wrong.append(f"want holds of {', '.join(f'{w:g}' for w in want)} s ± {LOAD_WAIT_TOL_S:g} s")
    if masks != {1 << ctx.burn_slot}:
        wrong.append(f"held masks {sorted(masks)}, want only {card_at(ctx, ctx.burn_slot)}")
    if any(r.get("muted") for _, r in seen):
        wrong.append("status read muted")
    if after["restarts"] != before["restarts"]:
        wrong.append(f"{after['restarts'] - before['restarts']} stream restarts")
    measured = f"holds {', '.join(f'{h:.1f}' for h in holds)} s"
    detail = f"{after['steps'] - before['steps']} steps in {OVERLOAD_STAYS_S:g} s, the port stayed up"
    return (FAIL if wrong else PASS), "; ".join(wrong + [detail]), measured


def drill_overload_outside(ctx):
    """A burn past the whole block that belongs to no card: the rule takes out every card it may, the Input
    gate stays in, and after burn 0 every card comes back."""
    port = ctx.app()
    budget = ctx.budget or chain_stats(port).budget
    cycles = min(MAX_BURN_CYCLES, int(OUTSIDE_LOAD * budget))
    takeable = sum(1 << s for s, card in enumerate(ctx.slots) if card not in NOT_TAKEABLE)
    gate = sum(1 << s for s, card in enumerate(ctx.slots) if card == "input_gate")
    before = status_now(port)
    sent = time.monotonic()
    bench_ask(port, "burn", cycles=cycles)
    seen = poll_status(port, sent, OUTSIDE_S)
    bench_ask(port, "burn", cycles=0)
    off = time.monotonic()
    back = poll_status(port, off, LOAD_WAIT_CAP_S + HEALTHY_MARGIN_S, lambda r: r.get("held") == 0)
    every = [t for t, r in seen if r.get("held", 0) & takeable == takeable]
    ever = 0
    for _, r in seen + back:
        ever |= r.get("held", 0)
    after = (back or seen)[-1][1] if (back or seen) else before
    wrong = []
    if not every:
        wrong.append(f"held at most mask {ever}, never every takeable card (mask {takeable})")
    if ever & gate:
        wrong.append("the Input gate was taken out")
    if any(r.get("muted") for _, r in seen + back):
        wrong.append("status read muted")
    if after["restarts"] != before["restarts"]:
        wrong.append(f"{after['restarts'] - before['restarts']} stream restarts")
    if after.get("held") != 0:
        wrong.append(f"held mask {after.get('held')} {LOAD_WAIT_CAP_S + HEALTHY_MARGIN_S:g} s after burn 0, want 0")
    out_s = f"{every[0]:.2f} s" if every else "never"
    back_s = f"{back[-1][0]:.1f} s" if back and back[-1][1].get("held") == 0 else "not back"
    detail = (f"burn {cycles} cycles a block ({percent(cycles, budget)}) in no slot for {OUTSIDE_S:g} s: every "
              f"takeable card out {out_s} after the burn, {after['steps'] - before['steps']} steps; every card back "
              f"{back_s} after burn 0; the port stayed up")
    return (FAIL if wrong else PASS), "; ".join(wrong + [detail]), f"all out after {out_s}, all back after {back_s}"


# (case, what it sends, what it checks, function), in run order. The overload drills run first, from one
# start, so the load rule's waits begin at 2 s; each restart drill then starts the box again.
SURVIVAL_DRILLS = (
    ("Overload", "reset_peaks and stats with no burn, then burn in slot 4 (Mob): the block budget less the mean "
     "block, plus the spread and 10 % of the budget, spun only while Mob runs",
     "slot 4 alone in status held inside 120 ms, one step in the 0.5 s after it, muted false; ms to the step",
     drill_overload),
    ("Overload ends", "burn 0", "status held 0 again 2 s ± 0.5 s after the step; s to return", drill_overload_ends),
    ("One late block", "burn (the block budget) for 1 block", "status late +1, steps +0; late count", drill_one_late),
    ("Save under load", "burn to an 80 % mean block, record_save 20 times 0.5 s apart, burn 0",
     "status late +0, steps +0; late count", drill_save_under_load),
    ("Overload stays", "the slot 4 burn for 60 s once Mob has been back 60 s, then burn 0",
     "holds of 2, 4, 8, 16, 16 s ± 0.5 s on slot 4 alone, muted false, no restart; the holds in s",
     drill_overload_stays),
    ("Overload outside the cards", "burn in no slot at 110 % of the block budget for 20 s, then burn 0",
     "every card but the Input gate held at once, the Input gate never, muted false, no restart, held 0 again "
     "within 18 s of burn 0; s to all out and to all back", drill_overload_outside),
    ("A3", "stop_audio", "a warm start, restart code A3, the same layout and settings, the panel read again, "
     "setup() to relay <= 1500 ms; the gap", drill_a3),
    ("W1", "spin", "the port drops 2 s ± 1 s after the op, then as A3 with W1; the gap after the watchdog", drill_w1),
    ("C1", "fault, then enter_dfu for DIAG.TXT and the bench image copied back",
     "as A3 with C1, and DIAG.TXT's newest boot entry reads C1; the gap", drill_c1),
    ("C1 repeated", "10 s of running, then fault five times, 2 s after each return",
     "each return as A3 with C1, never update mode; the gap, five times", drill_c1_repeated),
    ("A4", "fail_audio_start, then fault", "a second restart, then as A3 with A4; the gap", drill_a4),
    ("Bad update", "after 7 s of running, enter_dfu and the bench image under another header version (another "
     "CRC), then fault at each start as its port opens",
     "update mode after 3 starts, DIAG.TXT reason BootFailed, the bench image copied back; starts",
     drill_failed_starts),
)


def survival_slots():
    try:
        return stress.layout_table()[SURVIVAL_LAYOUT][0]
    except stress.StressError as e:
        raise DrillError(str(e)) from e


def survival_setup(ctx):
    """Puts the costliest layout on the box at CLEANUP EQ STRONG with every slot forced on. Returns the layout
    and settings it found."""
    port = ctx.app()
    found = layout_state(port)
    ask_ok(port, "set_layout", name=f"survival {SURVIVAL_LAYOUT}", slots=ctx.slots,
           settings={"cleanup_eq": SURVIVAL_CLEANUP_EQ})
    bench_ask(port, "all_on")
    time.sleep(stress.SETTLE_S)
    return found


def survival_restore(ctx, found):
    layout, settings = found
    port = ctx.app()
    bench_ask(port, "burn", cycles=0)
    ask_ok(port, "set_layout", name=layout["name"], slots=layout["slots"],
           settings={"cleanup_eq": settings["cleanup_eq"]})
    bench_ask(port, "slots_live")


def run_survival_drill(ctx, number, case, fn):
    run_drill(ctx, number, case, fn, False)
    try:
        bench_ask(ctx.app(), "burn", cycles=0)  # a drill that stopped early leaves no burn behind
    except (DrillError, *PORT_ERRORS) as e:
        print(f"[WARN] burn 0 after drill {number}: {e}", flush=True)


def survival_drills(log, port):
    """Runs the survival drills on the bench image. Takes over `port` and returns the open port of the running
    application, or None when the box could not be brought back."""
    with tempfile.TemporaryDirectory(prefix="cubevox-survival-") as tmp:
        ctx = SurvivalCtx(log, tmp, [])
        ctx.port = port
        try:
            ctx.slots = survival_slots()
            outdir = fw_outdir("bench")
            ctx.files = {"bench": os.path.join(outdir, "cubevox.uf2"),
                         "other": other_image(tmp, os.path.join(outdir, "cubevox.ino.bin"))}
            found = survival_setup(ctx)
        except (DrillError, *PORT_ERRORS) as e:
            log.check("survival drills", False, str(e))
            return ctx.port
        for number, (case, _, _, fn) in enumerate(SURVIVAL_DRILLS, 1):
            run_survival_drill(ctx, number, case, fn)
        try:
            survival_restore(ctx, found)
            return ctx.app()
        except (DrillError, *PORT_ERRORS) as e:
            log.check("survival drills: the layout found put back", False, str(e))
            return None


def survival_plan(board):
    """--survival --dry-run: each drill, what it sends and what it checks. Opens no port."""
    try:
        slots = survival_slots()
    except DrillError as e:
        print(f"[ERROR] {e}", flush=True)
        return 1
    print(f"[survival] board {board}; layout {SURVIVAL_LAYOUT}: {', '.join(slots)}; CLEANUP EQ "
          f"{SURVIVAL_CLEANUP_EQ}; every slot forced on (all_on, again after each restart)")
    for number, (case, sends, checks, _) in enumerate(SURVIVAL_DRILLS, 1):
        print(f"DRILL {number:02d} {case}\n  sends:  {sends}\n  checks: {checks}")
    print("[survival] then burn 0, the layout and CLEANUP EQ found, and slots_live")
    return 0


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


def dry_run(a):
    """--dry-run: the drills that --faults, --update and --survival name. Opens no port."""
    if not (a.faults or a.update or a.survival):
        sys.exit("[ERROR] --dry-run goes with --faults, --update or --survival")
    if a.faults:
        print("[faults] stop_audio: the stall line, then a software reset; spin: a watchdog reset; "
              "fault: the fault record after the reset")
    if a.update:
        for number, name, fn, operator in DRILLS:
            doc = (fn.__doc__ or "").strip().splitlines()
            print(f"DRILL {number:02d} {name}{' (needs the operator)' if operator else ''}"
                  + (f"\n  {doc[0]}" if doc else ""))
    return survival_plan(a.board) if a.survival else 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--board", required=True, help="board number or label for the log name")
    ap.add_argument("--no-oled", action="store_true", help="skip the OLED check")
    ap.add_argument("--faults", action="store_true", help="drill audio stall, watchdog and bus fault")
    ap.add_argument("--stream-faults", action="store_true",
                    help="drill stream fault restarts, the reset past the budget and a failed restart (needs the bootloader)")
    ap.add_argument("--dfu", action="store_true", help="drill ROM DFU entry by BOOT0 and flash back over it")
    ap.add_argument("--panel", action="store_true", help="walk every pot and toggle, then K103, mute and OLED")
    ap.add_argument("--flood", action="store_true", help="10 s of status requests at full speed, then a hello")
    ap.add_argument("--update", action="store_true",
                    help="run the 13 update drills against the bootloader's drive; the log is board-N.md")
    ap.add_argument("--survival", action="store_true",
                    help="run the stage survival drill table on the costliest layout (needs the bootloader)")
    ap.add_argument("--dry-run", action="store_true",
                    help="with --faults, --update or --survival: print the drills, open no port")
    ap.add_argument("--no-prompt", action="store_true", help="skip every question to the operator")
    ap.add_argument("--port", default=None, help="serial port, needed only when several cu.usbmodem* exist")
    a = ap.parse_args()
    if a.dry_run:
        return dry_run(a)
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
    if a.flood:
        flood_drill(log, port)
    if a.panel:
        panel_test(log, port, not a.no_prompt, not a.no_oled)
    if a.faults:
        port = fault_drill(log, port)
    if a.stream_faults:
        port = stream_fault_drills(log, port)
        if port is None:
            return
    if a.survival:
        port = survival_drills(log, port)
        if port is None:
            return
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
        drills = [f"DRILL {n:02d} {status:7} {name}: {detail} [measured: {measured or '-'}]"
                  for n, name, status, detail, measured in log.drills]
        text = f"cubevox bring-up, board {a.board}, {stamp}\n" + "\n".join(log.rows + drills) + "\n"
    with open(path, "w") as f:
        f.write(text)
    print(f"\n[bringup] {log.failed} failed; log {path}")
    print(f"[bringup] checks that need a person at the bench: {CHECKLIST}")
    return 1 if log.failed else 0


if __name__ == "__main__":
    sys.exit(main())
