# cubevox — CHASM reverb (second engine in the reverb block)

Date 2026-10-01. Owner: "port in the chasm reverb from drumsynthv3 as a switchable
option for verb." Panel stage 9 stays two knobs; with CHASM selected they read
DECAY and WOBBLE. Engine select is a prototype radio (SPRING | CHASM) and a params
field so the render CLI can A/B.

Reference (read-only, DrumSynthV3 is frozen):
`/Users/j/Documents/Claude/Projects/DrumSynthV3/firmware/DrumSynthV3/audio_effect_cavern_v3.h`
(class `AudioEffectCavernV3`, the engine; internals keep the Cavern name) and the
CAVERN ear-tune block in `firmware/DrumSynthV3/audio_graph.h` (≈ lines 288–430,
`kCav*`). Upstream: hexeguitar/hexefx_audiolib_F32 `effect_springreverb_F32`,
(c) 2024 Piotr Zapart, MIT. The cubevox header carries the MIT attribution.

## What it is

One feedback loop: dly1 (1945) → allpass 1a..1d (224/420/856/1089) → dly2 (1363)
→ allpass 2a..2d (156/478/956/1289) → back to dly1, with in-loop low-pass
(treble loss) and high-pass (bass cut), feedback `time`. Output tap feeds a
four-stage chirp cascade of short allpasses (lengths 3/5/6/7 samples, 16 units
per stage, coefficients −0.7/−0.65/−0.6/−0.5) — the downward "boing". Input runs
through an allpass (0.6) and a treble cut. WOBBLE displaces the two loop delays'
read heads 90° apart with a sine LFO (DrumSynthV3 deviation 9). Wet-only output.

## Decisions

1. Faithful port of DrumSynthV3's mono version, all its disclosed deviations kept
   (real dly2, 16-unit chirp depth, read-head wobble, float). All delay and
   allpass lengths are **scaled 48/44.1 and rounded** so the times match
   DrumSynthV3 at 48 kHz; the chirp lengths 3/5/6/7 are NOT scaled (they are the
   dispersion structure, not times).
2. Knob laws from DrumSynthV3: DECAY → `time = lerp(timeLo, timeHi, x)` with
   0.35 / 0.86; in-loop bass cut 200 Hz at DECAY ≤ 0.5 sliding to 100 Hz at 1.
   WOBBLE → `depth = x² · depthMax` (192 samples), `rate = lerp(0.5, 7.0, x)` Hz,
   Level compensation: DrumSynthV3's rise table was measured on drum-bus material
   and does not transfer (measured here: white noise FALLS 2.1–2.3 dB once any
   wobble is on, then stays flat, from the interpolated read head's HF loss). So
   `wet *= dbToLin(wobbleLevelDb · min(x / 0.25, 1))`, `wobbleLevelDb` a tuning
   item, default +2.0 dB. No gain clamp (float lines).
3. Fixed shaping from DrumSynthV3: treble loss 3000 Hz, loop treble cut off (1.0),
   input treble cut 0.95, input trim 0.5. Wet level is a tuning item (`wetDb`,
   default −6 like Spring). Dry at unity.
4. `engine/reverb.h` wraps both: `ReverbParams { int engine; bool on; float knob1,
   knob2; SpringParams spring; ChasmParams chasm; }` is NOT used — instead keep
   the two param structs independent and let `Reverb` hold `SpringParams` and
   `ChasmParams` side by side with its own `engine` and `on`. The selected engine
   processes; the other is reset on switch. Off is sample-exact passthrough via
   the same active smoother pattern.

## Engine

### `engine/chasm.h`

```
struct ChasmTuning {
  float timeLo = 0.35f, timeHi = 0.86f;
  float trebleLossHz = 3000.0f;
  float loopTrebleCut = 1.0f;       // 1 = off
  float inputTrebleCut = 0.95f;
  float bassCutHz = 200.0f, bassCutHzTop = 100.0f;
  float wobbleDepthMax = 192.0f;    // samples
  float wobbleRateLo = 0.5f, wobbleRateHi = 7.0f;
  float inputTrim = 0.5f;
  float wobbleLevelDb = 2.0f;       // wet lift at full wobble, ramps in over x 0..0.25
  float wetDb = -6.0f;
};

struct ChasmParams {
  float decay = 0.5f;    // knob 1
  float wobble = 0.3f;   // knob 2
  ChasmTuning tuning;
};

class Chasm {
 public:
  void reset();
  // Wet-only; caller mixes. n <= kBlock.
  void process(const float* in, float* wet, int n, const ChasmParams& p);
};
```

Storage all `std::array<float, N>` with N from the scaled lengths (+ wobble guard
on dly1/dly2 ≥ depthMax + 4). Per-block coefficient updates; per-sample loop as in
the reference. Own 256-entry sine table or `sinf` per sample (per-sample `sinf`
is acceptable here: one call per sample). `-Werror` clean, no heap/I/O.

### `engine/reverb.h`

```
struct ReverbParams {
  bool on = true;
  int engine = 0;        // 0 SPRING, 1 CHASM
  SpringParams spring;   // spring.on is ignored; Reverb::on rules
  ChasmParams chasm;
};

class Reverb {
 public:
  void reset();
  void process(const float* in, float* out, int n, const ReverbParams& p);
  Spring& spring();      // test hooks pass through
};
```

`out = in + active · ((dry − 1) · in + wetG · wet_engine)` with `dry = cos(mix·π/2)`,
`wetG = sin(mix·π/2)`, `mix` a panel knob 0..1 smoothed 20 ms (default 0.5).
Mix 0 is a bit-exact copy even while an engine runs. Spring's own on/active path is bypassed by
driving it with `on = true` and gating at this level, so there is one smoother.
Spring's `wetDb` and Chasm's `wetDb` stay separate, both default 0 dB, and act as
level trims behind MIX.

## Emulator

The SPRING block becomes **REVERB**: checkbox `REVERB`, radio `SPRING | CHASM`,
knob 1 labelled `TENSION` or `DECAY`, knob 2 `DWELL` or `WOBBLE` by engine, both
`%.0f %%`, then `MIX` `%.0f %%` opening at 50 %, shared by both engines. Tuning header under it has `Spring` and `Chasm` sub-tree-nodes (Spring's
split further as already specified). `ProtoParams.spring` is replaced by
`ProtoParams.reverb` (`cv::ReverbParams`).

## Render CLI

`--reverb <spring|chasm>` selects and enables; `--tension/--dwell` keep working
for spring; `--decay <0..1>`, `--wobble <0..1>` for chasm; `--rmix <0..1>` sets MIX
(default 0.5). Tuning keys `chm` +
field name (`chmTimeLo`, `chmWobbleDepthMax`, `chmWetDb`, …).

## Tests (`test/chasm_test.cpp`, ctest `chasm`)

1. Passthrough: `Reverb` with `on = false` → bit-exact after 100 ms; engine 1,
   `on = true`, silence in → silence out; `mix = 0`, `on = true`, engine 1 running →
   bit-exact after 100 ms.
2. Decay tracks DECAY: impulse through Chasm wet; RMS 1.0–1.5 s re 0–0.5 s at
   decay 1 exceeds that at decay 0 by ≥ 15 dB.
3. Chirp runs DOWN (this is what distinguishes it from Spring): band-pass the
   wet impulse response at 500 Hz and 4 kHz (Q 4, zero-phase 10 ms envelopes,
   first local maximum within 3 dB of the window max); the 4 kHz peak arrives
   BEFORE the 500 Hz peak within 0–60 ms.
4. Wobble modulates: 2 s of 1 kHz sine; wet spectrum at wobble 1 has ≥ 10 dB
   more energy in 900–1100 Hz excluding ±5 Hz of 1 kHz than at wobble 0.
5. Wobble level compensation: wet RMS at wobble 0.25, 0.5 and 1 within ±1.5 dB
   of wobble 0 on white noise (measured raw −2.1/−2.2/−2.3 dB; +2 dB lift).
6. Stable: decay 1, wobble 1, 3 s white noise at −6 dBFS then 3 s silence; no
   NaN/inf, |wet| < 4, last 0.5 s RMS below first 0.5 s of silence.
7. No allocation inside `process()`.

Spring's test file is unchanged; it keeps driving `Spring` directly.

## Done when

`ctest` passes; REVERB block switches engines with relabelled knobs; owner A/Bs
Spring and Chasm on a vocal loop.

Level rule (2026-10-01, see `2026-10-01-level-rule.md`): `wetDb` default is now 17.4 dB (was 0), set so CHASM at MIX 0.5 reads +0.26 dB out/in.
