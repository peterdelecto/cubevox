# cubevox — unison effect + prototype emulator

Date 2026-10-01. Approved in chat; this file is the implementation reference.

## Purpose

cubevox is a live vocal effects box for Adam Keith (SM58 in, tabletop, one knob per
parameter). Final target is an STM32H7 (H7 VIT6 board). Effects get prototyped one at a
time on the Mac in a minimal ImGui emulator that plays a loaded loop through the effect
while the knob is tweaked. This spec covers the first effect, UNISON, and the emulator
skeleton every later effect reuses.

Full panel (for context; only UNISON is built here):

| # | Stage | Knob 1 | Knob 2 |
|---|---|---|---|
| 1 | Input (XLR / 1/4" combo, SM58) | Gain | |
| 2 | Soft gate (always on) | menu | |
| 3 | Autotune | Key (detented encoder) | Correction |
| 4 | Octave | Semitones | Mix |
| 5 | Unison | Depth | |
| 6 | Slapback | Intensity | |
| 7 | Distortion (always on) | Drive | |
| 8 | Gate | Threshold | |
| 9 | Spring reverb | Tension | Dwell |
| 10 | Output | menu | |

## Decisions

1. Shared portable engine. Effect code lives in `engine/*.h`, plain C++17, and the H7
   firmware includes the same files unchanged. What is tuned on the Mac is what ships.
2. Unison is chorus-style (owner pick B): two detuned copies whose detune wobbles with
   a slow LFO. Owner 2026-10-01: a fixed per-voice detune (the doubler flavour) is also
   in the tuning set so the two can be blended; defaults 0 cents keep the pure chorus.
3. One panel knob, DEPTH, drives swing, wet level, and therefore detune together.
   The constants behind that mapping are exposed in the emulator's Tuning section so
   the knob can be tuned by ear against real loops, then baked back as defaults.
4. Own repo at `/Projects/cubevox` with its own `.git`, like Chopper / Splitter /
   fusion3dp. ImGui vendored as a git submodule at the granularbox2 pin
   (`580c00c3`), miniaudio 0.11.25 copied as a single header.

## Engine contract (`engine/common.h`)

```
namespace cv {
constexpr int kSampleRate = 48000;
constexpr int kBlock      = 64;
}
```

Rules for every file under `engine/`:

1. No heap in `process()`. No `<vector>`, `<string>`, `new`, or `malloc` anywhere in
   the header. State sits in `std::array` members.
2. Compiles with `-fno-exceptions -fno-rtti -Wall -Wextra -Werror`.
3. `float` math only. No `double`, no `<iostream>`.
4. Mono in, mono out, block length `n <= kBlock`.
5. Parameters arrive by const reference each block; the engine smooths what needs
   smoothing itself.

## Unison (`engine/unison.h`)

### Interface

```
namespace cv {

struct UnisonTuning {            // defaults = the shipped mapping
  float baseDelayMs[2] = {15.0f, 22.0f};
  float lfoHz[2]       = {0.60f, 0.83f};
  float swingMinMs     = 0.2f;   // modulation swing (peak) at DEPTH 0
  float swingMaxMs     = 2.5f;   // modulation swing (peak) at DEPTH 1
  float wetMaxDb       = -6.0f;  // per-voice level at DEPTH 1 (two voices sum ~0 dB)
  float detuneCents[2] = {0.0f, 0.0f};  // fixed per-voice detune at DEPTH 1
  float windowMs       = 20.0f;  // crossfade window of the dual-tap shifter (5–30)
};

struct UnisonParams {
  bool  on    = false;           // panel toggle
  float depth = 0.0f;            // panel knob, 0..1
  UnisonTuning tuning;
};

class Unison {
 public:
  void reset();
  void process(const float* in, float* out, int n, const UnisonParams& p);
};

}
```

### Behaviour

1. Two voices reading one shared delay line of `kDelayMs = 80` ms in a `std::array`.
   Each voice's centre is `baseDelayMs[v] + swing * sin(phase_v)` with cubic
   (Catmull-Rom) interpolation. Each voice has its own LFO phase at `lfoHz[v]`; the two
   rates differ so the voices never lock.
1a. Fixed detune per voice is a crossfaded dual-tap sawtooth shifter around that centre:
   two taps half a window apart at `centre + window * (p - 0.5)`, Hann gains summing
   to 1, sawtooth phase advancing `(1 - ratio) / windowSamples` per sample with
   `ratio = 2^(detuneCents[v] * depth / 1200)`. Phase parked at 0 puts the live tap
   exactly at centre, so 0 cents is the plain chorus read.
2. `swing = lerp(swingMinMs, swingMaxMs, depth)` ms. `wetGain = depth *
   dbToLin(wetMaxDb)` per voice. Dry gain is 1.0 always.
3. `out = in + wetGain * (voice0 + voice1)`.
4. `depth` is smoothed by a one-pole with ~20 ms time constant before use. `on == false`
   is treated as `depth = 0` through the same smoother, so toggling is click-free.
5. At `depth == 0` (settled) the output equals the input sample-exact. The wet path is
   still computed so that turning the knob up does not start from an empty delay.
6. Peak detune in cents for a sine swing `A` seconds at `f` Hz is
   `1200 * log2(1 + 2*pi*f*A)`. With the defaults, DEPTH 1 on voice 1 (0.83 Hz,
   2.5 ms) gives about ±22 cents; DEPTH 0.5 about ±12 cents.

## Emulator (`emulator/main.cpp`)

Dear ImGui + GLFW + OpenGL 3 + miniaudio, copied from the granularbox2 proto skeleton.
Window ~900x520, one page, no scrolling.

Layout, top to bottom:

1. Row: `Load loop` (Cocoa NSOpenPanel via `emulator/open_panel.mm`), loaded filename,
   `Play` / `Stop`, output peak meter in dBFS.
2. Panel mirror, exactly what Adam gets on the box: `UNISON` checkbox, `DEPTH` slider
   shown as `%.0f %%`.
3. Collapsible `Tuning` (default collapsed, dev only): sliders in real units for
   `baseDelayMs[0..1]` (5–40 ms), `lfoHz[0..1]` (0.1–3 Hz), `swingMinMs` (0–1 ms),
   `swingMaxMs` (0.5–6 ms), `wetMaxDb` (-24–0 dB), `detuneCents[0..1]` (±30 cents),
   `windowMs` (5–30 ms). `Reset to defaults` button.
   `Print tuning` button writes the current `UnisonTuning` initialiser to stdout in a
   form that pastes straight into `unison.h`.

Audio:

1. miniaudio playback device, f32, 2 channels, 48000 Hz. Mono engine output goes to
   both channels.
2. Loops decode to mono f32 at 48 kHz through `ma_decoder`. Any format miniaudio reads
   (WAV, MP3, FLAC). The loop always repeats.
3. Loop swap: new buffer published through `std::atomic<LoopBuffer*>`; the old one is
   retired to a UI-thread list and deleted two callbacks later. Playhead resets on load.

Threading:

1. `ProtoParams { bool playing; UnisonParams unison; }` double-buffered. UI writes the
   non-published slot and stores the index with `memory_order_release`; the callback
   loads with `acquire` and copies the struct once per callback.
2. `ProtoState { float peakDb; float playheadNorm; }` goes the other way the same way.
3. The callback processes in `kBlock` chunks. No locks, no allocation, no logging.

`cubevox-proto --layout` creates an ImGui context with no window and no audio device,
renders four frames of the layout, and exits 0. Gates run this, never the window.

## Render CLI (`tools/render.cpp`)

`cubevox-render <in> <out.wav> --depth <0..1> [--on 0|1] [--tuning k=v ...]`.
Decodes the input to mono 48 kHz, runs it through `Unison` in `kBlock` chunks, writes
a 48 kHz mono f32 WAV through `ma_encoder`. Used by the tests and for A/B listening
without the GUI.

## Tests (`test/unison_test.cpp`, ctest)

No window, no audio device.

1. Passthrough. 1 s of 440 Hz sine at `depth = 0`, after discarding the first 50 ms,
   `|out - in| < 1e-6` for every sample. Silence in, silence out at any depth.
2. Detune. 4 s of 440 Hz sine at `depth = 1`, voice 1 only (voice 0 wet forced to 0 via
   a test hook `setVoiceEnabled`), wet only. Instantaneous frequency from zero-crossing
   periods; peak deviation over one LFO cycle lands in 15–30 cents.
3. Level. Same signal, both voices, full mix: output RMS within ±3 dB of input RMS
   measured over 4 s.
4. No allocation. `operator new` / `delete` replaced in the test binary with versions
   that `abort()` while a global `gInProcess` flag is set around `process()`.
5. Fixed detune. Depth 1, voice 1 only, swing 0, `detuneCents[1] = 10`, 440 Hz sine:
   mean detune over seconds 1–4 from zero-crossing periods lands in 10 ± 2 cents.
   (Measures ~9.3; the Hann crossfade blends tap phases and biases zero-crossing
   frequency low, so the band is deliberately loose.)

Also in ctest: `cubevox-proto --layout` exits 0.

## Build

```
cmake -S . -B build && cmake --build build -j && ctest --test-dir build
open build/cubevox-proto.app
```

Targets: `cubevox-proto` (MACOSX_BUNDLE, Info.plist.in from granularbox2 pattern),
`cubevox-render`, `unison_test`. Engine is an INTERFACE library carrying the flags in
the contract above. macOS frameworks: Cocoa, OpenGL, IOKit, CoreAudio,
CoreFoundation, AudioToolbox, AudioUnit. GLFW via pkg-config/`find_package(glfw3)`.

## Out of scope for this step

Live mic input in the emulator, the other nine stages, presets, a
high-pass on the wet path, stereo output from the engine. Each is a later addition
that fits the structure above without redesign.

## Done when

1. `ctest` passes all five checks and the output is pasted next to the claim.
2. `cubevox-proto.app` opens from Finder, loads one of the owner's vocal loops, plays it
   looped, and DEPTH audibly changes the doubling while UNISON off is bit-identical to
   the dry loop.
3. The owner has listened and either accepted the default knob mapping or produced a
   new `UnisonTuning` via `Print tuning` that is then baked into `unison.h`.
