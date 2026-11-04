"""Fusion-side helpers of the Decal add-in. Every module in this package imports adsk.

The log, the cancel signal and the progress wrapper live here because the dialog, the
headless entry in api.py and the bake all share them.
"""

import datetime
import os
import time
import traceback

import adsk
import adsk.core

ATTR_GROUP = 'cubevox-decal'
LOG_PATH = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'decal.log')

# Fusion's events are pumped at most this often, because each adsk.doEvents() costs a
# round trip and the bake calls step() once per piece and per profile.

PUMP_SECONDS = 0.05


class Cancelled(Exception):
    """The user pressed Cancel on the progress dialog."""


def log(line: str) -> None:
    """Append one stamped line to decal.log next to Decal.py."""
    stamp = datetime.datetime.now().strftime('%Y-%m-%d %H:%M:%S')
    with open(LOG_PATH, 'a', encoding='utf-8') as fh:
        fh.write(f'{stamp} {line}\n')


def log_failure(where: str, exc: BaseException) -> None:
    """Append the exception and its traceback to decal.log."""
    text = ''.join(traceback.format_exception(type(exc), exc, exc.__traceback__))
    log(f'{where} failed with {type(exc).__name__}: {exc}\n{text.rstrip()}')


class Progress:
    """Drives a Fusion progress dialog, or nothing in a headless run.

    begin() names a stage and the span of the bar it fills, in percent. step() moves within
    the stage, pumps Fusion's events when pump is set and raises Cancelled once the user
    has pressed Cancel. The preview passes pump=False, because pumping events inside an
    event handler lets Fusion fire the next event before the current one returns.
    """

    def __init__(self, dialog: 'adsk.core.ProgressDialog | None' = None, pump: bool = True):
        self.dialog = dialog
        self.pump = pump
        self.start = 0.0
        self.end = 100.0
        self.last = 0.0

    def begin(self, label: str, start: float, end: float) -> None:
        self.start, self.end = float(start), float(end)
        if self.dialog is not None:
            self.dialog.message = f'{label}  %p%'
        self.last = 0.0
        self.step(0.0)

    def step(self, fraction: float) -> None:
        now = time.perf_counter()
        if now - self.last < PUMP_SECONDS and 0.0 < fraction < 1.0:
            return
        self.last = now
        if self.dialog is not None:
            span = self.end - self.start
            self.dialog.progressValue = int(round(self.start + span * min(max(fraction, 0.0), 1.0)))
        if self.pump:
            adsk.doEvents()
        if self.dialog is not None and self.dialog.wasCancelled:
            raise Cancelled()
