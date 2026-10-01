# cubevox — final EQ ("polish")

Date 2026-10-01. Owner: "add a final eq as a module in the protoapp and set its
defaults and its frequency sliders with the intent of adding POLISH for effected
vocals." Panel: Adam agreed EQ can live in the menu ("oh we might want an EQ but
that might be able to live in the menu"), so this is a menu stage; in the prototype
it is a module with sliders. Last in the chain, after Reverb.

## Shape and why

| Band | Default | Why |
|---|---|---|
| High-pass, 2-pole Butterworth | 90 Hz | SM58 proximity, plosives, handling noise; mud into distortion and spring |
| Low-mid dip, peak EQ | 300 Hz, −2.5 dB, Q 1.0 | boxiness that distortion and reverb pile up; a 1 kHz scoop would hollow the voice |
| Presence, peak EQ | 3.5 kHz, +1.5 dB, Q 0.7 | intelligibility; kept mild because the SM58 peak and the BD-2's 2–3 kHz gain are already there; stays below sibilance (6–8 kHz) |
| Air, high shelf | 10 kHz, +1.5 dB | distortion's 6 kHz roll-off and the tank's band limit close the top |

## Engine (`engine/polish.h`)

```
struct PolishTuning {
  float dipQ = 1.0f;
  float presenceQ = 0.7f;
  float airHz = 10000.0f;
};

struct PolishParams {
  bool on = true;
  float hpHz = 90.0f;          // 40..200
  float dipHz = 300.0f;        // 150..600
  float dipDb = -2.5f;         // -6..0
  float presenceHz = 3500.0f;  // 2000..6000
  float presenceDb = 1.5f;     // 0..6
  float airDb = 1.5f;          // 0..4
  PolishTuning tuning;
};

class Polish {
 public:
  void reset();
  void process(const float* in, float* out, int n, const PolishParams& p);
};
```

RBJ biquads (TDF-II): 2nd-order Butterworth high-pass (Q 0.7071), two peak EQs, one
high shelf; coefficients recomputed per block when any field changed. `on` through
the usual 20 ms active smoother with snap, so settled off is sample-exact
passthrough (crossfade dry/processed by `active`). Float only, `std::array`, no
heap/I/O, `-Werror` clean.

## Emulator

`EQ` checkbox module after REVERB (column 3, second box; DISTORTION moved to column 2 under
SLAPBACK so open Tuning headers still fit one screen). Sliders in the main
section: HPF (Hz, log 40–200), Low-mid (Hz log 150–600, dB −6..0), Presence (Hz log
2–6 kHz, dB 0..+6), Air (dB 0..+4). Tuning node: dip Q, presence Q, air Hz. Reset /
Print cover PolishTuning; Print also emits the PolishParams defaults as a comment
line so a tuned shape can be baked. Off on app open like the other modules.

## Render CLI

`--eq` enables; `--eqhp`, `--eqdip <hz>,<db>`, `--eqpres <hz>,<db>`, `--eqair <db>`.
Tuning keys `eqDipQ`, `eqPresenceQ`, `eqAirHz`.

## Tests (`test/polish_test.cpp`, ctest `polish`)

1. Passthrough: `on = false` → bit-exact after 100 ms.
2. Response at defaults (sine RMS ratio out/in over seconds 1–2): 50 Hz ≤ −8 dB;
   300 Hz = −2.5 ± 0.4 dB; 1 kHz = 0 ± 0.4 dB; 3.5 kHz = +1.5 ± 0.4 dB; 12 kHz
   = +1.5 ± 0.5 dB.
3. Flat when all gains are 0 and hp 40 Hz: 1 kHz = 0 ± 0.1 dB.
4. No allocation inside `process()`.

## Done when

`ctest` passes; EQ module shows after REVERB; owner listens and adjusts the defaults.
