"""The bench letter map and the JSON request builder, shared by the host tools.

The firmware takes no single-byte commands. Every bench action is a framed request,
{"id":N,"cmd":"bench","op":"<name>"}, plus "slot" for slot_off. The familiar letters stay as
host-side shorthand and expand here (op names: core/bench_ops.cpp). DFU is its own command,
enter_dfu, in every build.
"""

import itertools
import json

LETTER_OPS = {
    "p": "stats",
    "r": "report",
    "q": "record_status",
    "m": "reset_peaks",
    "0": "slots_on",
    "k": "pin_knobs",
    "c": "chasm",
    "v": "clip",
    "a": "stop_audio",
    "f": "fault",
    "w": "spin",
    "s": "record_save",
    "e": "record_erase",
    "t": "panel_test",
    "l": "panel_summary",
    "g": "panel_dump",
    "i": "panel_jack",
    "x": "panel_mute",
    "o": "panel_oled",
}
SLOT_LETTERS = "12345678"  # slot_off N
DFU_LETTER = "d"           # the enter_dfu command

QUERY_OPS = {"report", "stats", "record_status"}      # every build
ALL_OPS = set(LETTER_OPS.values()) | {"slot_off"}      # the bench build lists these

_ids = itertools.count(1)


def next_id():
    return next(_ids)


def op_line(op, slot=None, req_id=None):
    """The request line for a bench op, newline included."""
    req = {"id": req_id if req_id is not None else next_id(), "cmd": "bench", "op": op}
    if slot is not None:
        req["slot"] = slot
    return json.dumps(req) + "\n"


def cmd_line(cmd, req_id=None):
    """The request line for a plain command such as enter_dfu, newline included."""
    return json.dumps({"id": req_id if req_id is not None else next_id(), "cmd": cmd}) + "\n"


def letter_line(letter, req_id=None):
    """The request line for a familiar letter, or None when the letter is unknown."""
    if letter == DFU_LETTER:
        return cmd_line("enter_dfu", req_id)
    if letter in SLOT_LETTERS and len(letter) == 1:
        return op_line("slot_off", int(letter), req_id)
    op = LETTER_OPS.get(letter)
    return op_line(op, req_id=req_id) if op else None


def to_line(text):
    """A raw JSON request passes through; a letter expands. Raises ValueError on anything else."""
    if text.lstrip().startswith("{"):
        return text if text.endswith("\n") else text + "\n"
    line = letter_line(text)
    if line is None:
        raise ValueError(f"not a JSON request or a known bench letter: {text!r}")
    return line
