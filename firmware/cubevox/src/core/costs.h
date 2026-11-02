// Per-card CPU cost as integer percent of one audio block's cycle budget (kBlock samples at the
// sample rate, counted in cycles at 480 MHz), in two columns: the mean block and the worst
// single block. Each is rounded up. kTracker and kAutotune describe one measurement, the pitch
// slot with Autotune on, and only their sum stands for it; see kTracker.
//
// Method of record: firmware/stress.py runs the passes below on four layouts (default, crowd, chasm,
// crowd-chasm) on a clip bench image, two runs each, and gates every placed card against its row
// here. A row dated "stress run" takes its worst from the lowest run worst per layout.
//
// Method: default layout on the voice clip with per-slot cycle counters, in two passes. The first
// pins every knob at its top position. The second catches loads pinned knobs cannot show: it
// switches Autotune on with the clip playing, and sweeps reverb MIX and SEMITONES end to end. A
// card's worst is the larger of the two passes. The worst figure is the lowest worst over repeated
// runs, so interrupt time landing in a block does not count. Octave is the pitch slot with Octave
// on minus with it off. Chasm is costed in the reverb slot. The tail after the slots (comp, EQ,
// ceiling, BYPASS mix, conversion) has its own rows, read from the kStatComp and kStatEq buckets
// and the block total minus the slots. The kStatEq bucket holds both halves of POLISH EQ, the
// front half ahead of the slots and the back half after the comp.
//
// A host figure converts to a board estimate at 50.4k board cycles per host microsecond, which is
// 7.875 % of a block (the ratio Unison's host time gives against its 46.9k on the board). The
// pitch slot's code runs at fewer cycles per host microsecond, 24k to 29k (kTracker), so rows scaled
// from a board figure and rows converted at 50.4k are not at one ratio. kTracker states both, and
// the board's Crowd row (kCrowd) shows the pitch ratio holds for the grain code too.
//
// Octave alone: PitchFx with only Octave on costs 1.04 host us per block (2026-11-01, Adam's clips,
// MIX 100 %, host bench of the CPU pass, +-0.05 us), which is 52.4k board cycles, 8.2 %, at 50.4k.
// CPU pass 2 measured 51.7k (8.1 %) on the board for the same case before the CPU pass. kOctave
// holds 25.7k (4.0 %), so Octave alone pays about 26.7k cycles, 4 points, beyond its row. That is the
// pitch stage's own work: the ring write, the per-sample mix and the crossfade gains. The pair behind
// kAutotune and kTracker was measured as the whole slot with Autotune on, so a layout with Autotune
// placed already carries it. Without Autotune, Octave and Formant pay it through kPitchStage, once.
//
// A layout is accepted when both column sums over its cards, plus the fixed term, the tracker once
// when Autotune, Crowd or Formant is placed, and kPitchStage once when Octave or Formant is placed
// without Autotune, stay under their lines. The trailing comments give the measured cycles, mean
// then worst; a row marked measured=false is an estimate until a board measures it.

#pragma once

#include <stdint.h>

namespace costs {

struct Cost {
  uint8_t avg;    // mean block
  uint8_t worst;  // worst single block
  bool measured;  // false while the numbers are estimates, so the app shows "est."
};

constexpr Cost operator+(Cost a, Cost b) {
  return {static_cast<uint8_t>(a.avg + b.avg), static_cast<uint8_t>(a.worst + b.worst),
          a.measured && b.measured};
}

constexpr Cost kEmpty{0, 0, true};
constexpr Cost kInputGate{4, 5, true};     // 23.7k / 26.0k, the board's figures for the earlier tuning; the revised one runs at 0.97 of them on the host, so they bound it until a rerun
constexpr Cost kAutotune{5, 7, false};     // the pair's estimate less kTracker: its voice, mix and fine pass; see kTracker. The worst carries one extra point because the stress run of 2026-11-01 read the pitch slot (Autotune, Octave and tracker) at 16.5 to 16.8 % worst against the 16 % the rows summed to
constexpr Cost kOctave{5, 5, true};        // 25.7k / 27.2k, its own voice only
constexpr Cost kUnison{8, 8, true};        // 46.9k / 47.7k
constexpr Cost kSlapback{5, 5, true};      // 26.3k / 26.6k
constexpr Cost kDistortion{8, 11, true};   // 45.5k to 46.8k / 64.5k to 67.7k, stress run 2026-11-01 (worst 10.1 to 10.6 %, lowest run worst per layout)
constexpr Cost kGate{4, 5, true};          // 24.7k / 27.0k
constexpr Cost kReverbSpring{15, 15, true};  // ~92k: three springs, from the board's boot worst 107.4k against the two-half engine's 72.0k (1.49x of 61.9k); two-pass bench pending
constexpr Cost kReverbChasm{17, 17, true};   // 102.2k to 102.5k / 105.5k to 108.2k, stress run 2026-11-01 in the reverb slot (mean 16.0 %, worst 16.5 to 16.9 %)

// The pitch tracker, its history fill and its coarse analysis, which Autotune, Crowd and Formant
// share and a layout pays once. kTracker and kAutotune are one estimate split in two. The pair was measured on
// the board before the pitch CPU pass at 83.2k cycles for the mean block (13.0 %) and 131.9k for
// the worst (20.6 %); this estimate scales those by the host ratio the CPU pass measured on Adam's
// clips with Autotune on at the defaults, host us per block: mean 2.83 before, 1.66 after (x0.587);
// voiced hop block 5.58 before, 2.83 after (x0.507). That gives 48.8k cycles (7.6 %) for the mean
// and 66.9k (10.5 %) for the worst, which is about 29k board cycles per host us on the mean block
// and 24k on the voiced hop block. The pair splits by the tracker's host share of Autotune on
// (Crowd on with Autotune off against Autotune on, same clips, 2026-11-01): mean 0.68 of 1.60 us
// (43 %), voiced hop block 1.21 of 2.75 us (44 %). The tracker takes 3.2 % and 4.6 %, rounded up
// to 4 and 5; Autotune's row takes the rest, 4.4 % and 5.9 %, rounded up to 5 and 6. Both rows are
// estimates, and a board bench of the pair after the pass is owed. At the 50.4k ratio the tracker's
// 0.68 us would read 5.4 points mean and its 1.21 us 9.5 points worst, against the 4 and 5 held
// here.
constexpr Cost kTracker{4, 5, false};

// The pitch stage's own work when Octave or Formant runs and Autotune is not placed, which no card row
// holds. Octave alone is 52.4k board cycles (8.2 %); kOctave's row is 5, so the term is 8.2 less 5,
// 3.2, rounded up to 4. The stage does not vary block to block, so the worst equals the mean. The
// layout pays it once; with Autotune placed its rows carry it. An estimate until a board measures
// Formant alone, whose own row is also an estimate.
constexpr Cost kPitchStage{4, 4, false};

// The eight-copy card, measured on the board on 2026-11-01. The crowd bench layout (Input gate,
// Octave, Crowd, Slapback, Gate, Spring reverb) ran Adam's 5.5 s clip with every knob at its top,
// two runs of 24 s, with no late, overrun or dry block. Crowd's slot averaged 78.7k cycles (12.3 %).
// Its worst block was 94.97k (14.8 %) in the lower run and 101.0k in the other. The tracker ran in
// Octave's slot, which cost 47.5k (7.4 %), so this row holds the card without the tracker. The host
// gave 2.40 us per block (VOICES 80 %, WIDTH 50 %, default tuning), which reads 19 points at the
// 50.4k ratio and 11 points at the pitch code's 29k. The board's 12.3 % sits at the pitch code's
// ratio, so that ratio holds for Crowd too. The mean comes from knobs at top. The worst comes from
// the two-pass stress run, where the WIDTH and VOICES sweeps raise it to a lowest run worst of 15.6 %
// in the crowd layout and 15.9 % in crowd-chasm.
constexpr Cost kCrowd{13, 16, true};  // 78.7k / 99.6k to 101.6k, board 2026-11-01; mean from the crowd bench layout slot 3 with knobs at top, worst from the stress run's sweeps; the tracker ran in Octave's slot, 47.5k

// The Formant card, an alternate for Octave in slot 3, and the Lo-fi and Chatter cards, alternates for
// Gate in slot 7. Each row is an estimate until the stress run measures the card on the board; the
// run's figure replaces it.
constexpr Cost kFormant{3, 4, false};  // est.: one formant voice is about one MOB copy (1.5 %), plus the mix and the fine pass; the fine pass counts twice when Autotune is placed, about 1 point
constexpr Cost kLofi{2, 2, false};     // est.: five biquads, the tanh and the crush, about 80 cycles a sample (0.8 %), rounded up for a block that moves a knob, which also ramps 25 coefficients a sample (about 30 cycles more); the host has no bench of it
constexpr Cost kBloom{3, 5, false};    // est. from the host bench of 2026-11-02 after the voice-major rewrite (bench_chatter and blocks, -O2, minimum of 12 runs, median of 5 interleaved passes, 50.4k board cycles per host us). Adam's clip through the Input gate at -55 dBFS: Apple clang 17 reads 5.36 ns a sample, 17.3k cycles, 2.7 %; g++ 15, the firmware's compiler family, 5.07 ns, 16.4k, 2.6 %; the higher rounds up to 3. Echo benches 7.83 ns and 8.13 ns in the same run, against 26.3k on the board. The busiest block reads 9.11 ns on clang, 29.4k, 4.6 %, and 8.47 ns on g++, which rounds up to 5. A stutter of 60 ms words at AMOUNT 100 % reads 9.54 ns on clang, 4.8 %, which the mean does not carry

// What every block spends whatever the layout: POLISH EQ's front half ahead of the slots, and the
// tail after them.
constexpr Cost kOutputEq{6, 7, false};     // est. 32.7k / 38k for both POLISH EQ halves and the sample conversion; 18.1k on the board for 7 biquads (9.2k measured, 8.9k est.) times the host ratio 1.55 for 9 biquads and the detector, plus 4.6k conversion; the worst adds the coefficient work of a block that switches on
constexpr Cost kPolishComp{1, 1, false};   // two biquads and the fast log and exp per sample
constexpr Cost kSoftCeiling{1, 1, false};  // a compare per sample; tanh only past -2 dBFS
constexpr Cost kBypassMix{1, 1, false};    // the dry copy, the input energy sum, the BYPASS fade
constexpr Cost kFixed = kOutputEq + kPolishComp + kSoftCeiling + kBypassMix;

constexpr uint8_t kBudgetAvgPercent = 75;
constexpr uint8_t kBudgetWorstPercent = 95;

}  // namespace costs
