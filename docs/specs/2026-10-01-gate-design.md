# cubevox — gates (input soft gate + panel GATE)

Date 2026-10-01. From the Adam exchange: "noise gate -> distortion" in his rig; Jeff's
proposed box order "soft gate on the way in ... distortion -> noise gate" got "Yea
probably"; "noise gate thresholds ... sometimes I set them at a show for monitors."
So: one engine, two instances.

| Instance | Where | Panel | Purpose |
|---|---|---|---|
| Input soft gate | first in chain | menu (always on) | tame stage bleed and handling noise before the pitch trackers and distortion |
| GATE | after Distortion, before Reverb | THRESHOLD knob | kill distortion hiss/bleed between phrases; reverb tails stay intact |

## Engine (`engine/gate.h`)

```
struct GateTuning {
  float attackMs = 1.0f;
  float holdMs = 150.0f;
  float releaseMs = 250.0f;
  float rangeDb = -40.0f;      // attenuation when closed (soft gate uses -12)
  float kneeDb = 12.0f;        // soft knee width around threshold
  float detectorHpHz = 120.0f; // detector side-chain high-pass
  float hysteresisDb = 6.0f;   // close threshold = open threshold - hysteresis
};

struct GateParams {
  bool on = true;
  float thresholdDb = -40.0f;  // panel knob -70..-10 (soft gate default -55)
  GateTuning tuning;
};

class Gate {
 public:
  void reset();
  void process(const float* in, float* out, int n, const GateParams& p);
  float gainDb() const;        // current gain for the emulator readout
};
```

Behaviour:

1. Detector: `hp1(in, detectorHpHz)` → peak follower (instant attack, 20 ms release)
   → dB.
2. Target gain: above `threshold` → 0 dB; below `threshold − knee` → `rangeDb`;
   in between, linear in dB across the knee. Hysteresis: once open, the gate uses
   `threshold − hysteresisDb` as its reference until it closes.
3. Timing: opening follows the target with the attack time constant; when the
   target drops, the hold timer runs for `holdMs` before the release time constant
   takes over.
4. Gain smoothed per sample in the dB domain (a linear one-pole needs ~8.5 time
   constants to fall 74 dB; in dB the release lands range+6 dB in ~2.6·release). `on == false` → gain fixed at 0 dB through a 20 ms smoother with
   snap, so settled off is sample-exact passthrough.
5. Float only, `std::array`, no heap/I/O, `-Werror` clean.

## Emulator

Two modules:

- `INPUT GATE` at the top of column 1, above HARMONY: checkbox, `THRESHOLD` slider
  −70..−10 dB (default −55), Tuning node with the GateTuning fields (range default
  −12 dB), live readout `Gain: -x dB`.
- `GATE` in column 2 after DISTORTION: checkbox, `THRESHOLD` −70..−10 dB (default
  −40), Tuning node (range −80), readout.
Both off on open like the rest. Callback order: inputGate → pitchFx → unison →
slapback → distortion → gate → reverb → polish. If a column overruns in the probe,
move Output EQ or Slapback, keeping signal order down-then-across.

## Render CLI

`--ingate <dB>` and `--gate <dB>` enable each at that threshold. Tuning keys
`ing*` / `gt*` + field name.

## Tests (`test/gate_test.cpp`, ctest `gate`)

1. Passthrough: `on = false` → bit-exact after 100 ms; `on = true` with a signal
   10 dB above threshold → after 100 ms `|out − in| < 1e-4`.
2. Closes: 1 kHz at −60 dBFS, threshold −40, range −80 → steady-state gain ≤ −70 dB.
3. Attack/release timing: tone switched from silence to −20 dBFS: gain reaches
   −1 dB within attack + 5 ms; switched back to silence: gain stays above −1 dB for
   hold ± 10 ms, then reaches `range + 6 dB` within release·3.
4. Hysteresis: tone at −41 dBFS (1 dB under the −40 threshold) after the gate has
   opened stays open (gain > −1 dB) while a tone at −44 dBFS closes it.
5. No allocation inside `process()`.

## Done when

`ctest` passes; both gates on the face; owner sets the GATE threshold on the
distorted loop and listens for chatter.

## Build notes

Module gap trimmed 40 → 37 px so column 2 (Unison, Slapback, Distortion, Gate) fits
with Distortion › Post open (814 of 816 px).

Defaults changed 2026-10-01 (owner: panel GATE too aggressive): hold 60 to 150 ms,
release 120 to 250 ms, range -80 to -40 dB, knee 6 to 12 dB, hysteresis 3 to 6 dB.
The input gate keeps its own range, hold and release overrides. gate_test fixes the
old values explicitly so its timing and closing checks keep the same physics.
