#!/usr/bin/env python3
"""Autonomous CPU stress run for a cubevox box on a clip bench image. Nobody touches the panel.

For each layout the script puts the layout on the box with CLEANUP EQ set to STRONG, its heaviest setting
(set_layout carries settings.cleanup_eq "strong"; the shipped default is off, where the stage's bucket reads
near zero and its gate would measure nothing), forces every slot on (bench all_on), plays
Adam's clip through the chain and measures the per-slot and per-block cycle counters in two passes.
Pass 1 pins every knob at its top and reads the counters after --seconds. Pass 2 hands the knobs back
to the panel and sweeps SEMITONES (Octave's upper knob), SHIFT (Formant's upper knob), Formant MIX
(its lower knob), the reverb MIX (the reverb slot's lower knob), both Echo knobs, both Input gate
knobs and both Gate knobs (Threshold across the whole knob, then Decay), both Unison knobs (Rate, then
Depth), both Distortion knobs (TONE again with DRIVE pinned at 100 %, and TONE stepped inside a DRIVE
glide, the costliest moving block), both Chatter knobs, both Lo-fi knobs, both Apraxia knobs (SENSITIVITY,
RANGE), both Deepthroat knobs (GROWL, MIX) and, when Crowd is placed, both Crowd
knobs from 0 % to 100 % of travel in steps of 10 %, 1 s each, reading the counters after each sweep.
It then flips the toggle of every Echo, Input gate, Gate, Unison, Chatter, Apraxia and Deepthroat
slot ten times, 0.3 s apart, so the engine starts from reset five times, and reads the counters again. The last step flips BYPASS
and CLEANUP EQ the same way (bench ops bypass, cleanup_eq; each leaves the box as it found it) and reads the
counters once more. The CLEANUP EQ flips force the stage off and back on from STRONG.

Per slot the mean is pass 1's mean and the worst is the largest worst over both passes. Across runs
the mean is the mean of the run means and the worst is the lowest run worst, which is the rule in
core/costs.h, so a block that caught an interrupt does not count. The fixed path is read the same
way. CLEANUP EQ comes from the EQ bucket of the stats lines. The fixed path is the block total less the
slot sum for the mean, and for the worst the EQ bucket at its worst plus the rest of the fixed path
(ceiling, BYPASS mix, conversion) at its mean.

Gates (each named in the output when it fails, exit 1 if any fails):
  1. late blocks, overruns and dry blocks are 0 for the whole run
  2. each placed card's mean % and worst % stay at or under its costs.h row
  3. each layout's block mean stays under 75 % and its block worst under 95 %
  4. each layout's CLEANUP EQ bucket (at STRONG) stays at or under kCleanupEq, and its fixed path mean %
     and worst % at or under kFixed, the sum of kCleanupEq, kSoftCeiling and kBypassMix
Each card's slot is held to its own row. A slot that runs a tracker adds the tracker row: Autotune's slot,
a Formant or Apraxia slot that does not sit directly behind Autotune, and a Mob or Deepthroat slot with no
Autotune ahead of it.
A layout the box refuses is a result row ("refused: <reason>") and is not measured.

--dry-run prints the layouts, the CLEANUP EQ setting step, the sweeps, the toggle flips and the fixed-stage
flips planned for each, with the time they take, and opens no port.

Layouts, by name: default, crowd (Crowd in Unison's slot), chasm (the reverb slot runs Chasm),
crowd-chasm, octave-alone (the default with Autotune's slot empty), formant (Formant in Octave's slot),
formant-alone (Formant with Autotune's slot empty), chatter (Chatter in Gate's slot), lofi (Lo-fi in Gate's
slot), apraxia (Apraxia in Octave's slot), throat (Deepthroat in Distortion's slot) and heaviest (Input gate, Formant, Unison and
Crowd, Gate, Distortion, Chatter and the default reverb, with the reverb on Chasm). The saved layout and CLEANUP EQ setting are read first and put back at the end,
together with slots_live,
knobs_live, clip off and the reverb engine, even when the run fails.

Output: a summary on stdout and a table per layout appended to build/bench/<YYYY-MM-DD>-stress.md.
Progress goes to stderr. One percent of a block is 6,400 cycles.

  ./stress.py --flash --runs 2          # build the clip bench image, install it, then run
  ./stress.py --layouts default,crowd --seconds 10
"""

import argparse
import collections
import datetime
import os
import re
import subprocess
import sys
import time

FW = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(FW)
sys.path.insert(0, FW)  # python3 -I leaves the script dir out
import bench_ops  # noqa: E402
import cubevox_port  # noqa: E402

SRC = os.path.join(FW, "cubevox", "src", "core")
COSTS_H = os.path.join(SRC, "costs.h")
LAYOUT_CPP = os.path.join(SRC, "layout.cpp")
REGISTRY_CPP = os.path.join(SRC, "registry.cpp")
BENCH_DIR = os.path.join(REPO, "build", "bench")
DEFAULT_CLIP = os.path.join(REPO, "samples", "adam_vox_sample3_5s.wav")
FLASH_OUT = "/tmp/cubevox-fw-stress"  # the --flash build's own out dir, so no other build shares it

SLOTS = 8
CYCLES_PER_PERCENT = 6400
BLOCK_MEAN_LIMIT_PERCENT = 75
BLOCK_WORST_LIMIT_PERCENT = 95
SWEEP_VALUES = range(0, 101, 10)  # percent of travel
SWEEP_STEP_S = 1.0
TOGGLE_FLIPS = 10    # a toggle flips off and on five times
TOGGLE_STEP_S = 0.3  # past the slot fade, so each enable finds the engine settled off and resets it
GLIDE_LAG_S = 0.02   # a stepped knob starts its partner this long after it, inside the glide
FLIPPED_CARDS = ("slapback", "input_gate", "gate", "unison", "bloom", "formant_follow", "growl")  # toggles the run flips
# The fixed stages' levers, as (bench op, label, the line the box prints with the flag set, with it clear).
FIXED_LEVERS = (
    ("bypass", "BYPASS", "[bench] BYPASS held on", "[bench] BYPASS follows the top switch"),
    ("cleanup_eq", "CLEANUP EQ", "[bench] CLEANUP EQ forced off", "[bench] CLEANUP EQ follows the menu"),
)
# The words of the protocol's cleanup_eq setting, and the one every layout runs at: STRONG is the heaviest
# setting, and at the shipped default (off) the CLEANUP EQ bucket reads near zero and gates nothing.
CLEANUP_EQ_WORDS = ("off", "light", "strong")
STRESS_CLEANUP_EQ = "strong"
SETTLE_S = 1.5       # the 50 ms fade out and in of a layout change, and the engine clears
REPLY_WAIT_S = 5.0
PORT_WAIT_S = 30.0
TAIL_LINES = 20
LAYOUT_NAMES = ("default", "crowd", "chasm", "crowd-chasm", "octave-alone", "formant", "formant-alone",
                "chatter", "lofi", "apraxia", "throat", "heaviest")
PORT_ERRORS = (cubevox_port.PortError, cubevox_port.PortGone)

# Card id to the costs.h row that bounds it.
ROW_OF = {
    "input_gate": "kInputGate", "autotune": "kAutotune", "octave": "kOctave", "unison": "kUnison",
    "slapback": "kSlapback", "distortion": "kDistortion", "gate": "kGate",
    "reverb_spring": "kReverbSpring", "reverb_chasm": "kReverbChasm", "crowd": "kCrowd",
    "formant": "kFormant", "bloom": "kBloom", "lofi": "kLofi",
    "formant_follow": "kFormantFollower", "growl": "kGrowl",
}
TRACKER_ROW = "kTracker"
FORMANT_CARDS = ("formant", "formant_follow")  # Formant and Apraxia play one voice
READERS = ("crowd", "growl")  # cards that read Autotune's tracker when one sits ahead and run their own otherwise
NEEDED_OPS = ("slots_live", "all_on", "slot_off", "knobs_live", "pin_knobs", "set_knob", "reset_peaks", "stats",
              "clip", "chasm", *(lever[0] for lever in FIXED_LEVERS))
# The fixed path's rows: CLEANUP EQ is read from its bucket, and the three add to kFixed.
FIXED_PARTS = ("kCleanupEq", "kSoftCeiling", "kBypassMix")
FIXED_ROW = "kFixed"

COST_ROW = re.compile(r"constexpr\s+Cost\s+(k\w+)\s*\{\s*(\d+)\s*,\s*(\d+)\s*,\s*(?:true|false)\s*\}")
BLOCKS_LINE = re.compile(r"\[audio\] blocks (\d+) late (\d+) overruns (\d+) dry (\d+) avg (\d+) worst (\d+) of (\d+) cycles")
PER_SLOT_LINE = re.compile(r"\[audio\] per slot ((?:\d+ )+)EQ (\d+)")
SLOT_WORST_LINE = re.compile(r"\[audio\] slot worst ((?:\d+ )+)EQ (\d+)")
CLIP_LINE = re.compile(r"\[bench\] vocal clip (on|off)")
CHASM_LINE = re.compile(r"\[bench\] reverb slot runs (Chasm|its card's engine)")

Stats = collections.namedtuple("Stats", "blocks late overruns dry avg worst budget slot_avg slot_worst eq_avg eq_worst")
# One sweep: the slot index, the knob (1 or 2) it moves, its label, the other knobs pinned first as (knob,
# percent of travel) pairs and, for a glide, the partner knob that steps from 0 to 100 % and back inside it.
Sweep = collections.namedtuple("Sweep", "slot knob label held partner", defaults=((), None))
Gate = collections.namedtuple("Gate", "name ok detail")


class StressError(Exception):
    """The run cannot go on; the message says why."""


def say(text):
    print(f"[stress] {text}", file=sys.stderr, flush=True)


def fmt_k(cycles):
    return f"{cycles / 1000:.1f}k"


def fmt_pct(cycles):
    return f"{cycles / CYCLES_PER_PERCENT:.1f} %"


def fmt_row(percent):
    """A costs.h row cell; a stage with no row of its own shows a dash."""
    return "-" if percent is None else f"{percent} %"


# -- the firmware sources the gates read

def read_text(path):
    try:
        with open(path) as f:
            return f.read()
    except OSError as e:
        raise StressError(f"cannot read {path}: {e}") from e


def read_rows():
    """The costs.h rows as {name: (mean percent, worst percent)}."""
    rows = {m.group(1): (int(m.group(2)), int(m.group(3))) for m in COST_ROW.finditer(read_text(COSTS_H))}
    missing = [n for n in (*ROW_OF.values(), TRACKER_ROW, *FIXED_PARTS) if n not in rows]
    if missing:
        raise StressError(f"{COSTS_H}: no row for {', '.join(missing)}")
    rows[FIXED_ROW] = (sum(rows[n][0] for n in FIXED_PARTS), sum(rows[n][1] for n in FIXED_PARTS))
    return rows


def default_layout():
    """The default layout's card ids, slot 1 to 8, read from layout.cpp through the registry's ids."""
    ids = dict(re.findall(r'\{Card::(\w+),\s*"(\w+)"', read_text(REGISTRY_CPP)))
    body = re.search(r"Layout\s+defaultLayout\(\)\s*\{\s*return\s*\{\{(.*?)\}\};", read_text(LAYOUT_CPP), re.S)
    if not body:
        raise StressError(f"{LAYOUT_CPP}: no defaultLayout() to read")
    names = re.findall(r"Card::(\w+)", body.group(1))
    if len(names) != SLOTS or any(n not in ids for n in names):
        raise StressError(f"{LAYOUT_CPP}: defaultLayout() does not name {SLOTS} registry cards")
    return [ids[n] for n in names]


def layout_table():
    """{name: (card ids, reverb slot runs Chasm)} for the layouts."""
    base = default_layout()
    if "unison" not in base:
        raise StressError("the default layout has no unison slot for the crowd layout to replace")
    if "autotune" not in base or "octave" not in base:
        raise StressError("the default layout has no Autotune or Octave slot for the pitch layouts to change")
    if "gate" not in base:
        raise StressError("the default layout has no gate slot for the chatter and lofi layouts to replace")
    if "distortion" not in base:
        raise StressError("the default layout has no distortion slot for the throat layout to replace")
    reverbs = [c for c in base if c.startswith("reverb_")]
    if not reverbs:
        raise StressError("the default layout has no reverb slot for the heaviest layout to keep")
    crowd = ["crowd" if c == "unison" else c for c in base]
    # Octave and Formant alone leave the stage's tracker idle, so their rows show without Autotune's.
    octave_alone = ["empty" if c == "autotune" else c for c in base]
    formant = ["formant" if c == "octave" else c for c in base]
    formant_alone = ["formant" if c == "octave" else ("empty" if c == "autotune" else c) for c in base]
    chatter = ["bloom" if c == "gate" else c for c in base]
    lofi = ["lofi" if c == "gate" else c for c in base]
    apraxia = ["formant_follow" if c == "octave" else c for c in base]
    throat = ["growl" if c == "distortion" else c for c in base]
    heaviest = ["input_gate", "formant", "unison", "crowd", "gate", "distortion", "bloom", reverbs[0]]
    return {"default": (base, False), "crowd": (crowd, False), "chasm": (base, True), "crowd-chasm": (crowd, True),
            "octave-alone": (octave_alone, False), "formant": (formant, False), "formant-alone": (formant_alone, False),
            "chatter": (chatter, False), "lofi": (lofi, False), "apraxia": (apraxia, False), "throat": (throat, False),
            "heaviest": (heaviest, True)}


def slot_bounds(slots, chasm, rows):
    """Per slot, (mean percent, worst percent) it may use, or None for an empty slot, and the set of slots
    that run a tracker.

    A slot pays the tracker row when it runs one, as layoutCost counts them: Autotune's slot, a Formant or
    Apraxia slot that does not sit directly behind Autotune (only Empty slots between), and a Mob or
    Deepthroat slot with no Autotune ahead. The reverb slot reads Chasm's row while the chasm op is on.
    """
    def row_of(card):
        return rows[ROW_OF["reverb_chasm" if chasm and card.startswith("reverb_") else card]]

    runs_tracker = set()
    autotune_ahead = False
    adjacent = False
    for i, card in enumerate(slots):
        if card == "empty":
            continue
        if card == "autotune":
            runs_tracker.add(i)
            autotune_ahead = adjacent = True
            continue
        if card in FORMANT_CARDS:
            if not (autotune_ahead and adjacent):
                runs_tracker.add(i)
        elif card in READERS and not autotune_ahead:
            runs_tracker.add(i)
        adjacent = False
    bounds = []
    for i, card in enumerate(slots):
        if card == "empty":
            bounds.append(None)
            continue
        mean, worst = row_of(card)
        if i in runs_tracker:
            mean, worst = mean + rows[TRACKER_ROW][0], worst + rows[TRACKER_ROW][1]
        bounds.append((mean, worst))
    return bounds, runs_tracker


# -- the box

def parse_stats(lines):
    """The newest block, per-slot and slot-worst lines in `lines` as a Stats."""
    blocks = per = worst = None
    for line in lines:
        blocks = BLOCKS_LINE.match(line) or blocks
        per = PER_SLOT_LINE.match(line) or per
        worst = SLOT_WORST_LINE.match(line) or worst
    if not (blocks and per and worst):
        raise StressError("the stats reply lacked its [audio] blocks, per slot or slot worst line")
    slot_avg = [int(v) for v in per.group(1).split()]
    slot_worst = [int(v) for v in worst.group(1).split()]
    if len(slot_avg) != SLOTS or len(slot_worst) != SLOTS:
        raise StressError(f"the stats lines carry {len(slot_avg)} and {len(slot_worst)} slots, not {SLOTS}")
    b = [int(v) for v in blocks.groups()]
    return Stats(b[0], b[1], b[2], b[3], b[4], b[5], b[6], slot_avg, slot_worst, int(per.group(2)), int(worst.group(2)))


class Box:
    """One open port and the bench requests the run needs."""

    def __init__(self, link):
        self.link = link
        self.alerts = []          # [ERROR] and [WARN] lines seen
        self.chasm = None         # the reverb engine as last printed, True for Chasm
        self.start_chasm = None   # the engine the box started on
        self.armed = False        # a bench image with the ops was found, so there is something to restore

    def note(self, line):
        if line.startswith(("[ERROR]", "[WARN]")):
            self.alerts.append(line)

    def wait(self, seconds):
        """Reads for `seconds`, so the port never backs up while the box runs."""
        end = time.monotonic() + seconds
        while True:
            left = end - time.monotonic()
            if left <= 0:
                return
            line = self.link.read_line(left)
            if line is not None:
                self.note(line)

    def ask(self, cmd, **fields):
        reply = self.link.ask(cmd, REPLY_WAIT_S, self.note, **fields)
        if reply is None:
            raise StressError(f"{cmd}: no reply in {REPLY_WAIT_S:g} s")
        return reply

    def bench(self, op, slot=None, knob=None, value=None):
        """(reply, the text lines that came before it). The box prints, then replies."""
        rid = self.link.take_id()
        lines = []
        self.link.send_line(bench_ops.op_line(op, slot=slot, req_id=rid, knob=knob, value=value))
        reply = self.link.read_reply(rid, REPLY_WAIT_S, lines.append)
        for line in lines:
            self.note(line)
        if reply is None:
            raise StressError(f"bench {op}: no reply in {REPLY_WAIT_S:g} s")
        return reply, lines

    def bench_ok(self, op, **args):
        reply, lines = self.bench(op, **args)
        if reply.get("ok") is not True:
            raise StressError(f"bench {op}: {reply.get('error', reply)}")
        return lines

    def stats(self):
        return parse_stats(self.bench_ok("stats"))

    def set_clip(self, want):
        """Toggles the clip until it reads `want`. Turning it off with none compiled in is already off."""
        for _ in range(2):
            reply, lines = self.bench("clip")
            if reply.get("ok") is not True:
                if not want and "no clip compiled in" in str(reply.get("error")):
                    return
                raise StressError(f"bench clip: {reply.get('error', reply)}")
            state = [m.group(1) for m in map(CLIP_LINE.match, lines) if m]
            if not state:
                raise StressError("bench clip: no '[bench] vocal clip' line came back")
            if (state[-1] == "on") == want:
                return
        raise StressError(f"bench clip: the clip would not turn {'on' if want else 'off'}")

    def toggle_chasm(self):
        lines = self.bench_ok("chasm")
        state = [m.group(1) for m in map(CHASM_LINE.match, lines) if m]
        if not state:
            raise StressError("bench chasm: no '[bench] reverb slot runs' line came back")
        self.chasm = state[-1] == "Chasm"

    def set_chasm(self, want):
        for _ in range(2):
            if self.chasm == want:
                return
            self.toggle_chasm()
        if self.chasm != want:
            raise StressError("bench chasm: the reverb engine would not switch")

    def flip_lever(self, op):
        """Flips a fixed-stage flag and says whether it reads set afterward."""
        lines = self.bench_ok(op)
        _, label, set_line, clear_line = next(lever for lever in FIXED_LEVERS if lever[0] == op)
        state = [line for line in lines if line.startswith((set_line, clear_line))]
        if not state:
            raise StressError(f"bench {op}: no '{set_line}' or '{clear_line}' line came back")
        return state[-1].startswith(set_line)

    def clear_levers(self):
        """Every fixed-stage flag back to clear. A flip shows the new state, so a set flag takes a second flip."""
        for op, *_ in FIXED_LEVERS:
            if self.flip_lever(op):
                self.flip_lever(op)

    def get_layout(self):
        """(name, slots, CLEANUP EQ setting word) of the box's saved state."""
        reply = self.ask("get_layout")
        layout = reply.get("layout")
        if reply.get("ok") is not True or not isinstance(layout, dict) or "slots" not in layout:
            raise StressError(f"get_layout: {str(reply)[:160]}")
        settings = reply.get("settings")
        cleanup_eq = settings.get("cleanup_eq") if isinstance(settings, dict) else None
        if cleanup_eq not in CLEANUP_EQ_WORDS:
            raise StressError(f"get_layout: no cleanup_eq setting in the reply: {str(reply)[:160]}")
        return layout["name"], layout["slots"], cleanup_eq

    def set_layout(self, name, slots, cleanup_eq):
        """Puts the layout and the CLEANUP EQ setting on the box. None on success, else the box's reason for refusing."""
        reply = self.ask("set_layout", name=name, slots=slots, settings={"cleanup_eq": cleanup_eq})
        if reply.get("ok") is True:
            self.wait(SETTLE_S)
            return None
        return str(reply.get("error", reply))


def open_box(port, wait_s):
    path = cubevox_port.find_port(port, wait_s=wait_s)
    time.sleep(1.0)  # let the port settle after it enumerates
    box = Box(cubevox_port.Link(path))
    box.wait(1.0)    # the boot report a fresh open prints
    return box


# -- one measured run

def sweeps(slots):
    """The Sweep list this layout gets: each slot's knobs end to end, the Distortion cases that move a
    block, and the gates and Unison, whose moving blocks cost more than their settled ones."""
    found = []
    for i, card in enumerate(slots):
        if card == "octave":
            found.append(Sweep(i, 1, "SEMITONES"))
        if card == "formant":
            found.append(Sweep(i, 1, "SHIFT"))
            found.append(Sweep(i, 2, "Formant MIX"))
        if card.startswith("reverb_"):
            found.append(Sweep(i, 2, "reverb MIX"))
        if card == "slapback":
            found.append(Sweep(i, 1, "Echo TIME"))
            found.append(Sweep(i, 2, "Echo AMOUNT"))
        if card in ("input_gate", "gate"):
            name = "Input gate" if card == "input_gate" else "Gate"
            found.append(Sweep(i, 1, f"{name} Threshold"))
            found.append(Sweep(i, 2, f"{name} Decay"))
        if card == "unison":
            found.append(Sweep(i, 1, "Unison Rate"))
            found.append(Sweep(i, 2, "Unison Depth"))
        if card == "distortion":
            found.append(Sweep(i, 1, "Distortion TONE"))
            found.append(Sweep(i, 2, "Distortion DRIVE"))
            found.append(Sweep(i, 1, "Distortion TONE at DRIVE 100 %", held=((2, 100),)))
            found.append(Sweep(i, 2, "Distortion DRIVE with TONE stepped inside the glide", partner=1))
        if card == "bloom":
            found.append(Sweep(i, 1, "Chatter LENGTH"))
            found.append(Sweep(i, 2, "Chatter AMOUNT"))
        if card == "lofi":
            found.append(Sweep(i, 1, "Lo-fi TONE"))
            found.append(Sweep(i, 2, "Lo-fi AMOUNT"))
        if card == "crowd":
            found.append(Sweep(i, 1, "Crowd DIVERGE"))
            found.append(Sweep(i, 2, "Crowd VOICES"))
        if card == "formant_follow":
            found.append(Sweep(i, 1, "Apraxia SENSITIVITY"))
            found.append(Sweep(i, 2, "Apraxia RANGE"))
        if card == "growl":
            found.append(Sweep(i, 1, "Deepthroat GROWL"))
            found.append(Sweep(i, 2, "Deepthroat MIX"))
    return found


def flipped_slots(slots):
    """(slot index, card) for each slot whose toggle the run flips."""
    return [(i, card) for i, card in enumerate(slots) if card in FLIPPED_CARDS]


def plan_seconds(slots, seconds):
    """Seconds one run takes: the read, the sweeps, the toggle flips and the fixed-stage flips."""
    flips = (len(flipped_slots(slots)) + len(FIXED_LEVERS)) * TOGGLE_FLIPS * TOGGLE_STEP_S
    return seconds + len(sweeps(slots)) * len(SWEEP_VALUES) * SWEEP_STEP_S + flips


def run_sweep(box, sweep):
    """One sweep: the held knobs pinned, then the knob stepped from 0 % to 100 %. A partner knob steps
    to 100 % and back on alternate steps, a moment after the knob, while its glide runs."""
    slot = sweep.slot + 1
    for held_knob, held_value in sweep.held:
        box.bench_ok("set_knob", slot=slot, knob=held_knob, value=held_value)
    for n, value in enumerate(SWEEP_VALUES):
        box.bench_ok("set_knob", slot=slot, knob=sweep.knob, value=value)
        if sweep.partner is None:
            box.wait(SWEEP_STEP_S)
            continue
        box.wait(GLIDE_LAG_S)
        box.bench_ok("set_knob", slot=slot, knob=sweep.partner, value=100 if n % 2 == 0 else 0)
        box.wait(SWEEP_STEP_S - GLIDE_LAG_S)


def pass_two(box, slots):
    """The swept pass: every sweep, the slot toggle flips, then the fixed-stage flips. Each reads the
    counters after it ends, and the returned list holds those reads."""
    later = []
    for sweep in sweeps(slots):
        say(f"pass 2, {sweep.label} (slot {sweep.slot + 1}) 0 % to 100 %")
        run_sweep(box, sweep)
        later.append(box.stats())
        box.bench_ok("knobs_live")
    for slot, card in flipped_slots(slots):
        say(f"pass 2, {TOGGLE_FLIPS} flips of the {card} toggle (slot {slot + 1})")
        for _ in range(TOGGLE_FLIPS):
            box.bench_ok("slot_off", slot=slot + 1)  # each call flips the slot's force-off bit
            box.wait(TOGGLE_STEP_S)
        later.append(box.stats())
    for op, label, *_ in FIXED_LEVERS:
        say(f"pass 2, {TOGGLE_FLIPS} flips of {label}")
        for _ in range(TOGGLE_FLIPS):
            box.flip_lever(op)
            box.wait(TOGGLE_STEP_S)
        later.append(box.stats())
    return later


def measure_run(box, slots, seconds):
    """One run: pass 1 at the top, pass 2 swept. Returns the run's numbers."""
    box.bench_ok("slots_live")
    box.bench_ok("all_on")
    box.set_clip(True)
    box.bench_ok("knobs_live")
    box.bench_ok("pin_knobs")
    box.bench_ok("reset_peaks")
    say(f"pass 1, knobs at top, {seconds:g} s")
    box.wait(seconds)
    first = box.stats()
    box.bench_ok("knobs_live")
    box.bench_ok("reset_peaks")
    later = pass_two(box, slots)
    every = [first, *later]
    # The fixed path less its EQ bucket (ceiling, BYPASS mix, conversion) is read from the mean only.
    fixed_mean = first.avg - sum(first.slot_avg)
    rest = max(0, fixed_mean - first.eq_avg)
    eq_worst = max(s.eq_worst for s in every)
    return {
        "blocks": first.blocks,
        "block_mean": first.avg,
        "block_worst": max(s.worst for s in every),
        "slot_mean": first.slot_avg,
        "slot_worst": [max(s.slot_worst[i] for s in every) for i in range(SLOTS)],
        "eq_mean": first.eq_avg,
        "eq_worst": eq_worst,
        "fixed_mean": fixed_mean,
        "fixed_worst": eq_worst + rest,
        "late": first.late + (later[-1].late if later else 0),
        "overruns": first.overruns + (later[-1].overruns if later else 0),
        "dry": first.dry + (later[-1].dry if later else 0),
        "budget": first.budget,
    }


def aggregate(runs):
    """Mean of the means, lowest of the worsts, and the counters summed."""
    n = len(runs)
    mean_of = {key: sum(r[key] for r in runs) / n
               for key in ("block_mean", "eq_mean", "fixed_mean")}
    low_of = {key: min(r[key] for r in runs) for key in ("block_worst", "eq_worst", "fixed_worst")}
    return {
        "runs": n,
        **mean_of,
        **low_of,
        "slot_mean": [sum(r["slot_mean"][i] for r in runs) / n for i in range(SLOTS)],
        "slot_worst": [min(r["slot_worst"][i] for r in runs) for i in range(SLOTS)],
        "late": sum(r["late"] for r in runs),
        "overruns": sum(r["overruns"] for r in runs),
        "dry": sum(r["dry"] for r in runs),
        "budget": runs[0]["budget"],
    }


def run_layout(box, name, slots, chasm, runs, seconds):
    """A result dict for one layout. 'refused' or 'error' replaces the numbers when it applies."""
    result = {"name": name, "slots": slots, "chasm": chasm}
    done = []
    try:
        refusal = box.set_layout(f"stress {name}", slots, STRESS_CLEANUP_EQ)
        if refusal:
            result["refused"] = refusal
            return result
        box.set_chasm(chasm)
        for n in range(1, runs + 1):
            say(f"{name}: run {n} of {runs}")
            done.append(measure_run(box, slots, seconds))
    except (StressError, *PORT_ERRORS) as e:
        result["error"] = str(e)
        if isinstance(e, PORT_ERRORS):
            result["lost"] = True
    if done:
        result["agg"] = aggregate(done)
        result["per_run"] = done
    return result


# -- gates and reports

def judge_fixed(agg, rows, label, table, gates):
    """Adds the fixed stages to the table and holds each to its row: the EQ bucket to kCleanupEq, and the
    fixed path to kFixed, the sum of the three fixed rows."""
    parts = (("CLEANUP EQ", "eq", "kCleanupEq"),("fixed path", "fixed", FIXED_ROW))
    for name, key, row_name in parts:
        row_mean, row_worst = rows[row_name]
        over = []
        for kind, value, row in (("mean", agg[f"{key}_mean"], row_mean), ("worst", agg[f"{key}_worst"], row_worst)):
            within = value / CYCLES_PER_PERCENT <= row
            if not within:
                over.append(kind)
            gates.append(Gate(f"{label}: {name} {kind} within {row_name}", within,
                              f"{value / CYCLES_PER_PERCENT:.2f} % {'within' if within else 'over'} {row} %"))
        table.append(("-", name, agg[f"{key}_mean"], row_mean, agg[f"{key}_worst"], row_worst,
                      "ok" if not over else "FAIL " + " and ".join(over)))


def judge(result, rows):
    """Fills result['table'], result['gates'] and result['trackers'] from the aggregate.

    Every check is a gate, passed or not, so the count in the output is the number of checks made.
    """
    agg = result["agg"]
    bounds, trackers = slot_bounds(result["slots"], result["chasm"], rows)
    table, gates = [], []
    label = result["name"]
    for what, count in (("late blocks", agg["late"]), ("overruns", agg["overruns"]), ("dry blocks", agg["dry"])):
        gates.append(Gate(f"{label}: {what} are 0", count == 0, f"{count} over {agg['runs']} run(s)"))
    for i, card in enumerate(result["slots"]):
        if bounds[i] is None:
            continue
        mean, worst = agg["slot_mean"][i], agg["slot_worst"][i]
        row_mean, row_worst = bounds[i]
        over = []
        for kind, value, row in (("mean", mean, row_mean), ("worst", worst, row_worst)):
            within = value / CYCLES_PER_PERCENT <= row
            if not within:
                over.append(kind)
            gates.append(Gate(f"{label}: slot {i + 1} {card} {kind} within its row", within,
                              f"{value / CYCLES_PER_PERCENT:.2f} % {'within' if within else 'over'} {row} %"))
        table.append((i + 1, card + ("*" if i in trackers else ""), mean, row_mean, worst, row_worst,
                      "ok" if not over else "FAIL " + " and ".join(over)))
    judge_fixed(agg, rows, label, table, gates)
    block_mean, block_worst = agg["block_mean"], agg["block_worst"]
    gates.append(Gate(f"{label}: block mean under {BLOCK_MEAN_LIMIT_PERCENT} %",
                      block_mean / CYCLES_PER_PERCENT < BLOCK_MEAN_LIMIT_PERCENT, fmt_pct(block_mean)))
    gates.append(Gate(f"{label}: block worst under {BLOCK_WORST_LIMIT_PERCENT} %",
                      block_worst / CYCLES_PER_PERCENT < BLOCK_WORST_LIMIT_PERCENT, fmt_pct(block_worst)))
    result["table"], result["gates"], result["trackers"] = table, gates, trackers


def block_line(agg):
    return (f"block total: mean {fmt_k(agg['block_mean'])} ({fmt_pct(agg['block_mean'])}), "
            f"worst {fmt_k(agg['block_worst'])} ({fmt_pct(agg['block_worst'])}) of {fmt_k(agg['budget'])}, "
            f"late {agg['late']}, overruns {agg['overruns']}, dry {agg['dry']}")


def summary_lines(results):
    out = []
    for r in results:
        title = f"{r['name']} ({' '.join(r['slots'])}{', reverb on Chasm' if r['chasm'] else ''})"
        out.append(title)
        if "refused" in r:
            out.append(f"  refused: {r['refused']}")
            continue
        if "error" in r:
            out.append(f"  error: {r['error']}")
        if "agg" not in r:
            continue
        for slot, card, mean, row_mean, worst, row_worst, verdict in r["table"]:
            out.append(f"  {slot} {card:<14} mean {fmt_k(mean):>7} {fmt_pct(mean):>7} / {fmt_row(row_mean):>4}"
                       f"   worst {fmt_k(worst):>7} {fmt_pct(worst):>7} / {fmt_row(row_worst):>4}   {verdict}")
        out.append("  " + block_line(r["agg"]))
    return out


def report_lines(results, header):
    out = [header, ""]
    for r in results:
        out.append(f"### {r['name']}")
        out.append("")
        out.append(f"Cards, slot 1 to 8: {', '.join(r['slots'])}{'; the reverb slot runs Chasm' if r['chasm'] else ''}.")
        out.append("")
        if "refused" in r:
            out += [f"refused: {r['refused']}", ""]
            continue
        if "error" in r:
            out += [f"error: {r['error']}", ""]
        if "agg" not in r:
            continue
        out.append("| slot | card | mean cycles | mean % | row mean % | worst cycles | worst % | row worst % | verdict |")
        out.append("|---|---|---|---|---|---|---|---|---|")
        for slot, card, mean, row_mean, worst, row_worst, verdict in r["table"]:
            out.append(f"| {slot} | {card} | {fmt_k(mean)} | {fmt_pct(mean)} | {fmt_row(row_mean)} | "
                       f"{fmt_k(worst)} | {fmt_pct(worst)} | {fmt_row(row_worst)} | {verdict} |")
        out.append("")
        if r["trackers"]:
            out.append("`*` The slot runs a pitch tracker, so its row includes the tracker row. Autotune's slot runs "
                       "one, Formant and Apraxia run one unless they sit directly behind Autotune, and Mob and "
                       "Deepthroat run one when no Autotune sits ahead of them.")
            out.append("")
        out.append(block_line(r["agg"]))
        out.append("")
        for n, run in enumerate(r["per_run"], 1):
            out.append(f"- run {n}: block mean {fmt_k(run['block_mean'])} ({fmt_pct(run['block_mean'])}), "
                       f"worst {fmt_k(run['block_worst'])} ({fmt_pct(run['block_worst'])}), "
                       f"late {run['late']}, overruns {run['overruns']}, dry {run['dry']}")
        out.append("")
    return out


def gate_lines(results):
    gates = [g for r in results for g in r.get("gates", [])]
    failed = [g for g in gates if not g.ok]
    out = [f"gates: {len(gates) - len(failed)} of {len(gates)} passed"]
    out += [f"  FAIL {g.name} ({g.detail})" for g in failed]
    return out, failed


# -- flashing

def flash(clip, port):
    """Builds the clip bench image into its own out dir and installs it with flash.sh."""
    if not os.path.isfile(clip):
        raise StressError(f"clip {clip} not found; name one with --clip")
    env = dict(os.environ, BENCH="1", CLIP=clip, OUT=FLASH_OUT, TAIL=str(TAIL_LINES))
    env.pop("VARIANT", None)
    if port:
        env["CUBEVOX_PORT"] = port
    for script in ("build.sh", "flash.sh"):
        say(f"{script} (bench image with {os.path.basename(clip)})")
        r = subprocess.run([os.path.join(FW, script)], env=env, text=True,
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        if r.returncode:
            tail = "\n".join(r.stdout.splitlines()[-TAIL_LINES:])
            raise StressError(f"{script} exited {r.returncode}:\n{tail}")


# -- the run

def preflight(box):
    """The hello reply, after checking the image. A bench image with the clip answers every needed op."""
    hello = box.ask("hello")
    build, fw = hello.get("build"), hello.get("fw")
    if hello.get("ok") is not True or build != "bench":
        raise StressError(f"hello says build {build!r} (fw {fw}); the run needs a clip bench image, "
                          "which ./stress.py --flash builds and installs")
    missing = [op for op in NEEDED_OPS if op not in hello.get("bench_ops", [])]
    if missing:
        raise StressError(f"hello says build 'bench' (fw {fw}) without the ops {', '.join(missing)}; "
                          "install a current bench image with ./stress.py --flash")
    box.armed = True
    try:
        box.set_clip(True)
    except StressError as e:
        raise StressError(f"hello says build 'bench' (fw {fw}) but the clip op failed ({e}); "
                          "build with CLIP=<wav> BENCH=1 ./build.sh or use --flash") from e
    box.toggle_chasm()  # the first toggle shows the engine the box started on
    box.start_chasm = not box.chasm
    return hello


def restore(port, box, name, slots, cleanup_eq):
    """Puts the box back as it was and returns the steps that failed. A lost port gets one reopen."""
    try:
        box.ask("hello")
    except (StressError, *PORT_ERRORS):
        try:
            box.link.close()
        except OSError:
            pass
        try:
            fresh = open_box(port, PORT_WAIT_S)
        except PORT_ERRORS as e:
            return [f"reopen the port ({e})"]
        fresh.start_chasm, fresh.chasm = box.start_chasm, None
        box = fresh
    failed = []
    steps = [
        ("slots_live", lambda: box.bench_ok("slots_live")),
        ("knobs_live", lambda: box.bench_ok("knobs_live")),
        ("fixed-stage flags", box.clear_levers),
        ("clip off", lambda: box.set_clip(False)),
        ("reverb engine", lambda: restore_chasm(box)),
        ("saved layout and CLEANUP EQ", lambda: restore_layout(box, name, slots, cleanup_eq)),
    ]
    for label, step in steps:
        try:
            step()
        except (StressError, *PORT_ERRORS) as e:
            failed.append(f"{label} ({e})")
    return failed


def restore_chasm(box):
    """Back to the engine the box started on. A reopened port has not seen the engine, so one toggle shows it."""
    if box.start_chasm is None:
        return
    if box.chasm is None:
        box.toggle_chasm()
    box.set_chasm(box.start_chasm)


def restore_layout(box, name, slots, cleanup_eq):
    if name is None:
        return
    if box.get_layout() == (name, slots, cleanup_eq):
        return
    refusal = box.set_layout(name, slots, cleanup_eq)
    if refusal:
        raise StressError(f"the box refused the saved layout: {refusal}")


def plan_lines(name, slots, chasm, runs, seconds, rows):
    """What one layout's runs do, as text."""
    out = [f"{name} ({' '.join(slots)}{', reverb on Chasm' if chasm else ''}), {runs} run(s) of "
           f"{plan_seconds(slots, seconds):.0f} s"]
    bounds, trackers = slot_bounds(slots, chasm, rows)
    for i, (card, bound) in enumerate(zip(slots, bounds)):
        if bound is not None:
            out.append(f"  bound: slot {i + 1} {card}{'*' if i in trackers else ''} mean {bound[0]} %, worst {bound[1]} %")
    out.append(f'  setting: CLEANUP EQ at STRONG (cleanup_eq "{STRESS_CLEANUP_EQ}") for the whole run; the box\'s own setting goes back after')
    out.append(f"  pass 1: knobs at top, {seconds:g} s")
    for sweep in sweeps(slots):
        extra = f", partner knob {sweep.partner} stepped inside the glide" if sweep.partner else ""
        held = "".join(f", knob {k} held at {v} %" for k, v in sweep.held)
        out.append(f"  sweep: {sweep.label}, slot {sweep.slot + 1} knob {sweep.knob}, 0 % to 100 %{held}{extra}")
    for slot, card in flipped_slots(slots):
        out.append(f"  toggle: {TOGGLE_FLIPS} flips of the {card} toggle, slot {slot + 1}")
    for op, label, *_ in FIXED_LEVERS:
        out.append(f"  fixed stage: {TOGGLE_FLIPS} flips of {label} (bench op {op})")
    return out


def dry_run(wanted, runs, seconds):
    """Prints the plan for the named layouts. Reads the firmware sources and opens no port."""
    try:
        table = layout_table()
        rows = read_rows()
    except StressError as e:
        print(f"[ERROR] {e}", file=sys.stderr)
        return 1
    for name in wanted:
        slots, chasm = table[name]
        print("\n".join(plan_lines(name, slots, chasm, runs, seconds, rows)))
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--flash", action="store_true",
                    help="build the clip bench image (CLIP=<clip> BENCH=1 ./build.sh) and install it with flash.sh first")
    ap.add_argument("--runs", type=int, default=2, help="runs per layout (default 2)")
    ap.add_argument("--layouts", default=",".join(LAYOUT_NAMES),
                    help=f"comma list of {', '.join(LAYOUT_NAMES)} (default all)")
    ap.add_argument("--seconds", type=float, default=20.0, help="pass 1 read window per run (default 20)")
    ap.add_argument("--port", default=None, help="serial port path (default CUBEVOX_PORT, else the only cu.usbmodem*)")
    ap.add_argument("--clip", default=DEFAULT_CLIP, help="WAV for --flash (default samples/adam_vox_sample3_5s.wav)")
    ap.add_argument("--dry-run", action="store_true",
                    help="print the sweeps, toggle flips and fixed-stage flips planned for each layout and exit; no port")
    a = ap.parse_args()
    wanted = [n for n in a.layouts.split(",") if n]
    unknown = [n for n in wanted if n not in LAYOUT_NAMES]
    if unknown or not wanted or a.runs < 1 or a.seconds <= 0:
        ap.error(f"unknown layout {', '.join(unknown)}" if unknown else "need layouts, --runs >= 1 and --seconds > 0")
    if a.dry_run:
        return dry_run(wanted, a.runs, a.seconds)

    box = start_name = start_slots = start_cleanup_eq = hello = None
    results, failed_restore, code = [], [], 1
    try:
        rows = read_rows()
        table = layout_table()
        if a.flash:
            flash(a.clip, a.port)
        box = open_box(a.port, PORT_WAIT_S)
        hello = preflight(box)
        start_name, start_slots, start_cleanup_eq = box.get_layout()
        say(f"box fw {hello.get('fw')}, saved layout {start_name!r}: {' '.join(start_slots)}, CLEANUP EQ {start_cleanup_eq}")
        for name in wanted:
            slots, chasm = table[name]
            result = run_layout(box, name, slots, chasm, a.runs, a.seconds)
            if "agg" in result:
                judge(result, rows)
            results.append(result)
            if result.get("lost"):
                break
        code = 0
    except StressError as e:
        print(f"[ERROR] {e}", file=sys.stderr)
    except PORT_ERRORS as e:
        print(f"[ERROR] port: {e}", file=sys.stderr)
    except KeyboardInterrupt:
        print("[ERROR] interrupted", file=sys.stderr)
    finally:
        if box is not None and box.armed:
            failed_restore = restore(a.port, box, start_name, start_slots, start_cleanup_eq)
        for step in failed_restore:
            print(f"[ERROR] restore failed: {step}", file=sys.stderr)
        if box is not None:
            try:
                box.link.close()
            except OSError:
                pass
    if not results:
        return 1

    gate_out, failed = gate_lines(results)
    refused = [r["name"] for r in results if "refused" in r]
    errors = [r["name"] for r in results if "error" in r]
    out = summary_lines(results) + gate_out
    if refused:
        out.append(f"refused, not measured: {', '.join(refused)}")
    if errors:
        out.append(f"stopped early: {', '.join(errors)}")
    if box is not None and box.alerts:
        out.append(f"box lines with [ERROR] or [WARN]: {len(box.alerts)}, first: {box.alerts[0]}")
    now = datetime.datetime.now()
    path = os.path.join(BENCH_DIR, f"{now:%Y-%m-%d}-stress.md")
    header = (f"## {now:%H:%M:%S} stress run, fw {hello.get('fw')}, {a.runs} run(s), pass 1 read {a.seconds:g} s, "
              f"layouts {', '.join(wanted)}")
    os.makedirs(BENCH_DIR, exist_ok=True)
    follows = os.path.exists(path) and os.path.getsize(path) > 0
    with open(path, "a") as f:
        f.write(("\n" if follows else "") + "\n".join(report_lines(results, header) + gate_out) + "\n")
    out.append(f"report appended to {path}")
    print("\n".join(out), flush=True)
    return 1 if (failed or errors or failed_restore or code) else 0


if __name__ == "__main__":
    sys.exit(main())
