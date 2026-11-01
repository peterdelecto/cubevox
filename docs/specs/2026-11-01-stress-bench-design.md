# cubevox — hands-free bench and stress run

Date 2026-11-01. Owner, after a MOB measurement waited an hour on the panel toggles: "you can
control the toggles. we need autonomous stress testing." Built the same day in v2.00.83. This
file states what the box and the Mac can do without a hand on the panel, and what the first run
found.

## Rule

A bench never waits on the owner. Every panel state a measurement needs is set from the serial
port: the layout (`set_layout`), the slot toggles, the knobs, the mic (the baked clip), the
reverb engine. A missing lever is a bench op to add, never a blocker to report. Bench image only;
the release image carries none of it and answers `op not in this build`.

## Panel levers (bench ops, `2026-10-12-serial-protocol-design.md`)

| Op | Arguments | Does |
|---|---|---|
| `slot_on` | `slot` 1..8 | forces that slot's toggle on |
| `all_on` | | forces all eight on |
| `slots_live` | | clears the forced-on and forced-off masks; the panel rules |
| `set_knob` | `slot` 1..8, `knob` 1 (upper) or 2 (lower), `value` 0..100 % | pins one knob; a stepped knob lands on the nearest step |
| `knobs_live` | | clears every per-knob pin and the pin-at-top |

A slot runs when `(toggle || forcedOn) && !forcedOff`, so `slot_off` still wins. A per-knob pin
wins over `pin_knobs`. `stats` prints both masks. Letters in `bench_ops.py`: `n` all_on,
`u` slots_live, `y` knobs_live; `slot_on` and `set_knob` go as JSON.

## The run: `firmware/stress.py`

1. Needs a bench image with the clip baked: `CLIP=../samples/adam_vox_sample3_5s.wav BENCH=1
   ./build.sh`, or `./stress.py --flash`, which builds it into `/tmp/cubevox-fw-stress` and
   installs it through the CUBEVOX drive. Preflight: `hello` says bench and `clip` answers ok.
2. Layouts by name, `--layouts`: `default`; `crowd` (default with the MOB card in slot 4 for
   Unison); `chasm` (default with the reverb slot on Chasm); `crowd-chasm` (both). A layout the
   box will not load is a result row with the box's reason, not a failure.
3. Per layout, per run (`--runs`, default 2): all slots forced on, clip on, knobs at top,
   counters reset, a read after `--seconds` (default 20) is pass 1. Pass 2 sweeps SEMITONES, the
   reverb's lower knob and, when MOB is placed, its two knobs, 0 to 100 % in steps of 10 at 1 s
   each, reading after each sweep. A slot's mean is pass 1; its worst is the largest worst over
   both passes; across runs the mean is averaged and the worst is the lowest run worst, the rule
   in `costs.h`.
4. The first slot holding Autotune, Octave or MOB runs the shared pitch stage, so the slots after
   it read near zero. That slot is held to the sum of the placed pitch rows plus the tracker row
   and is marked `*` in the tables.
5. Gates, every one counted: late, overrun and dry blocks 0 for the run; each placed card's mean
   and worst within its `costs.h` row (rows parsed from the file); each layout's block mean under
   75 % and worst under 95 %. Exit 1 on any failure. 84 gates over the four layouts.
6. Output: a summary on stdout and `build/bench/<date>-stress.md`, appended per run. The script
   restores the layout it found, clears the levers, turns the clip off and puts the reverb slot
   back on its card's engine, in a `finally`.

`stress.py` is the method of record for the cost rows: a row changes when the run measures past
it, by the lowest run worst rounded up, never the other way without a run.

## First run, 2026-11-01, box with Adam's 5.5 s clip, two runs

| Layout | Block mean | Block worst |
|---|---|---|
| default | 48.7 % | 55.5 % |
| MOB for Unison | 58.1 % | 65.8 % |
| default with Chasm | 51.8 % | 58.7 % |
| MOB with Chasm | 61.1 % | 68.8 % |

Lines 75 % and 95 %. Zero late, overrun or dry blocks in every layout. Four worst-block rows were
a point or two under what the sweeps measured and were raised: Autotune 6 to 7 (the pitch slot
read 16.5 to 16.8 % against the 16 % sum), Distortion 10 to 11 (10.1 to 10.6 %), MOB 15 to 16
(15.6 to 15.9 %), Chasm 16 / 16 to 17 / 17 (mean 16.0 %, worst 16.5 to 16.9 %). After the rows,
`gates: 84 of 84 passed`. Tightest margins: Chasm worst 16.9 % against 17, the pitch slot 16.8 %
against 17, Distortion 10.6 % against 11, MOB 15.8 % against 16.

## Verification

`ctest` 28 of 28; release and bench `./build.sh` end in `[warn-gate] ... ok`; `tools/protocol_check.py`
against the bench box ends `[check] all passed`, including the argument-range errors and the
release refusal; the run above.
