// Yawn checks: settled off is a plain copy; RANGE maps -12 st at the pot's bottom, 0 at noon and +6 st
// at its top, the default position reads -5 st, and SHIFT never leaves -12 to +6 st; at SENSITIVITY 50 %
// the drive reads about 1 on a -21 dBFS note, 0.5 on a -27 dBFS note and 0 on a -33 dBFS note, and the
// full point moves with the pot; the follower's attack and release measured on a step; each onset fires
// a sweep that opens at the drive, halves at half the decay and rests at 0 while the note holds, and the
// next note fires it again; a +6 st sweep opens to +6 st and comes back to 0; silence rests at 0; at
// -12 st the grains play at half speed, their window sum holds unity and every read stays in the ring,
// and the output stays finite and under 0 dBFS on a tone and on Adam's clips; the output stays finite
// on the clips at every knob corner; and the card at its default holds the level rule on them. The card
// is set through the registry, as the box sets it. The clip rows skip when samples/ is absent. No
// window, no audio device.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#include "core/chain_params.h"
#include "core/registry.h"
#include "engine/formant_follower.h"
#include "engine/pitch_fx.h"

#include "signal_util.h"

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr int kRate = cv::kSampleRate;

using Signal = std::vector<float>;

bool report(const char* name, bool ok, const char* fmt, double a = 0.0, double b = 0.0, double c = 0.0,
            double d = 0.0) {
  char detail[200];
  std::snprintf(detail, sizeof detail, fmt, a, b, c, d);
  std::printf("%s %s  %s\n", ok ? "PASS" : "FAIL", name, detail);
  return ok;
}

double dbOf(double ratio) { return 20.0 * std::log10(ratio); }

double rmsRange(const Signal& x, size_t from, size_t to) {
  double s = 0.0;
  for (size_t i = from; i < to; ++i) s += static_cast<double>(x[i]) * x[i];
  return std::sqrt(s / static_cast<double>(to - from));
}

// A 150 Hz sawtooth, harmonics at 1/k up to 8 kHz, at peak `peak`.
Signal sawtooth(int n, float peak) {
  Signal x(static_cast<size_t>(n));
  for (int i = 0; i < n; ++i) {
    double s = 0.0;
    for (int k = 1; 150.0 * k < 8000.0; ++k) s += std::sin(2.0 * kPi * 150.0 * k * i / kRate) / k;
    x[static_cast<size_t>(i)] = static_cast<float>(0.55 * peak * s);
  }
  return x;
}

// A sine at `hz` whose peak sits at `peakDbfs`.
Signal sine(int n, double peakDbfs, double hz = 150.0) {
  Signal x(static_cast<size_t>(n));
  const double a = std::pow(10.0, peakDbfs / 20.0);
  for (int i = 0; i < n; ++i) x[static_cast<size_t>(i)] = static_cast<float>(a * std::sin(2.0 * kPi * hz * i / kRate));
  return x;
}

double peakOf(const Signal& x) {
  double p = 0.0;
  for (const float v : x) p = std::max(p, static_cast<double>(std::fabs(v)));
  return p;
}

bool allFinite(const Signal& x) {
  for (const float v : x) {
    if (!std::isfinite(v)) return false;
  }
  return true;
}

// The RANGE pot's position for `st`: -12 st at 0, 0 at noon and +6 st at 1, a straight line each side.
float rangePosition(float st) {
  return st < 0.0f ? 0.5f * (1.0f - st / cv::kFollowerMinRangeSt) : 0.5f * (1.0f + st / cv::kFollowerMaxRangeSt);
}

// The card placed and switched on with its pots at `sens` and `range` (0..1), as compose sets it, and
// its sweep falling over `decayMs`.
ChainParams boxYawn(float sens, float range, bool on = true, float decayMs = cv::kFollowerDefaultDecayMs) {
  ChainParams chain;
  applyPlacement(Card::FormantFollower, chain);
  applyToggle(Card::FormantFollower, on, chain);
  applyKnob(Card::FormantFollower, 0, sens, chain);
  applyKnob(Card::FormantFollower, 1, range, chain);
  chain.follower.tuning.decayMs = decayMs;
  return chain;
}

// The card as it ships: SENSITIVITY 40 %, RANGE -5 st, decay 300 ms.
ChainParams boxDefault() {
  return boxYawn(cv::kFollowerDefaultSensitivity, rangePosition(cv::kFollowerDefaultRangeSt));
}

// The pitch stage and the card ahead of it, as the chain runs them, on the heap: the ring is large.
struct Stage {
  cv::PitchFx pitch;
  cv::FormantFollower follower;
  int handed = 0;  // the SHIFT the card handed the stage on the last call, whole semitones
  Stage() {
    pitch.reset();
    follower.reset();
  }

  // One call of n samples.
  void run(const float* in, float* out, int n, const ChainParams& p) {
    const cv::PitchFxParams& fx = follower.follow(in, n, p.pitchFx, p.follower);
    handed = fx.formant.shiftSt;
    pitch.process(in, out, n, fx);
  }
};

// The signal through a fresh stage in kBlock calls. `each` sees the stage after every call.
template <typename Each>
Signal through(const Signal& in, const ChainParams& p, Each each) {
  auto s = std::make_unique<Stage>();
  Signal out(in.size());
  const int total = static_cast<int>(in.size());
  for (int i = 0; i < total; i += cv::kBlock) {
    const int n = total - i < cv::kBlock ? total - i : cv::kBlock;
    s->run(&in[static_cast<size_t>(i)], &out[static_cast<size_t>(i)], n, p);
    each(*s, i + n);
  }
  return out;
}

Signal through(const Signal& in, const ChainParams& p) {
  return through(in, p, [](const Stage&, int) {});
}

// Settled off, the card placed with its toggle off passes the input bit for bit. Switched on and then
// off again, the stage ends on a plain copy once the toggle's fade settles.
bool testOffExact() {
  const Signal in = sawtooth(kRate, 0.5f);
  bool ok = report("off: placed with its toggle off, a plain copy", through(in, boxYawn(1.0f, 1.0f, false)) == in, "");

  auto s = std::make_unique<Stage>();
  const ChainParams on = boxYawn(1.0f, 1.0f, true);
  const ChainParams offAgain = boxYawn(1.0f, 1.0f, false);
  Signal out(in.size());
  bool heard = false;
  bool copy = true;
  for (int pass = 0; pass < 2; ++pass) {
    for (size_t i = 0; i < in.size(); i += cv::kBlock) {
      s->run(&in[i], &out[i], cv::kBlock, pass == 0 ? on : offAgain);
      for (int j = 0; j < cv::kBlock; ++j) {
        if (pass == 0) heard = heard || out[i + static_cast<size_t>(j)] != in[i + static_cast<size_t>(j)];
        if (pass == 1 && i >= in.size() / 2) copy = copy && out[i + static_cast<size_t>(j)] == in[i + static_cast<size_t>(j)];
      }
    }
  }
  ok &= report("off: on for 1 s, then off, ends on a plain copy", heard && copy, "");
  return ok;
}

// The SHIFT the card hands the stage and the glided SHIFT, each at its lowest and highest, over a loud
// 0.5 s note with RANGE set straight on the engine. The voice starts once the tracker has the note, when
// the sweep has already begun to fall, so the glided SHIFT opens short of what the card hands out.
struct Span {
  int handedLow;
  int handedHigh;
  float low;
  float high;
};

Span shiftSpan(float rangeSt) {
  ChainParams p = boxYawn(0.5f, 0.0f);
  p.follower.rangeSt = rangeSt;
  Span s{0, 0, 0.0f, 0.0f};
  through(sawtooth(kRate / 2, 0.5f), p, [&](const Stage& st, int) {
    s.handedLow = std::min(s.handedLow, st.handed);
    s.handedHigh = std::max(s.handedHigh, st.handed);
    s.low = std::min(s.low, st.pitch.shiftSemis());
    s.high = std::max(s.high, st.pitch.shiftSemis());
  });
  return s;
}

// The RANGE pot hands the card -12 st at its bottom, -6 st at 25 %, 0 at noon, +3 st at 75 % and +6 st
// at its top, a straight line on each side of noon, and its default position, 7/24 of the travel, reads
// the default of -5 st. RANGE 0 st leaves SHIFT at 0. A RANGE past -12 st or past +6 st set straight on
// the engine still keeps SHIFT within -12 to +6 st.
bool testRangeMap() {
  float at[5];
  for (int k = 0; k < 5; ++k) at[k] = boxYawn(0.5f, 0.25f * static_cast<float>(k)).follower.rangeSt;
  const float position = rangePosition(cv::kFollowerDefaultRangeSt);
  const float dflt = boxYawn(0.5f, position).follower.rangeSt;
  bool ok = report("range: the pot gives -12, -6, 0, +3 and +6 st at 0, 25, 50, 75 and 100 %",
                   at[0] == -12.0f && at[1] == -6.0f && at[2] == 0.0f && at[3] == 3.0f && at[4] == 6.0f,
                   "%+.2f, %+.2f, %+.2f, %+.2f st and +6 at the top", at[0], at[1], at[2], at[3]);
  ok &= report("range: the default position reads -5 st, the engine's default",
               std::fabs(dflt + 5.0f) < 1e-5f && std::fabs(position - 7.0f / 24.0f) < 1e-6f &&
                   cv::FormantFollowerParams().rangeSt == -5.0f,
               "%.1f %% of the travel reads %+.4f st", 100.0 * position, dflt);

  const Span none = shiftSpan(0.0f);
  const Span deep = shiftSpan(-20.0f);
  const Span up = shiftSpan(10.0f);
  ok &= report("range: 0 st leaves SHIFT at 0 on a loud note", none.low == 0.0f && none.high == 0.0f, "%+.3f to %+.3f st",
               none.low, none.high);
  ok &= report("range: -20 st on the engine shifts -12 st at most, +10 st shifts +6 st at most",
               deep.handedLow == -12 && deep.handedHigh == 0 && deep.low >= -12.0f && deep.high <= 0.0f &&
                   up.handedHigh == 6 && up.handedLow == 0 && up.high <= 6.0f && up.low >= 0.0f,
               "handed %+.0f and %+.0f st; glided %+.3f and %+.3f st", deep.handedLow, up.handedHigh, deep.low, up.high);
  return ok;
}

// RANGE +6 st, the pot's top, at SENSITIVITY 50 % on a -14 dBFS note held 0.6 s: the card hands the stage
// +6 st on the onset, the glided SHIFT opens past +5.5 st, and from 150 ms past the decay's end it rests
// within 0.01 st of 0.
bool testUpSweep() {
  int handedTop = 0;
  float high = 0.0f;
  float low = 0.0f;
  float rest = 0.0f;
  const int restFrom = static_cast<int>(cv::kFollowerDefaultDecayMs * cv::kSmpPerMs) + kRate * 15 / 100;
  through(sawtooth(kRate * 6 / 10, 0.2f), boxYawn(0.5f, 1.0f), [&](const Stage& s, int done) {
    handedTop = std::max(handedTop, s.handed);
    high = std::max(high, s.pitch.shiftSemis());
    low = std::min(low, s.pitch.shiftSemis());
    if (done >= restFrom) rest = std::max(rest, std::fabs(s.pitch.shiftSemis()));
  });
  return report("range: +6 st opens to +6 st and returns to 0 while the note holds",
                handedTop == 6 && high > 5.5f && high <= 6.0f && low >= 0.0f && rest < 0.01f,
                "handed %+.0f st, SHIFT peaks %+.3f st, rests within %.4f st", handedTop, high, rest);
}

// The drive's lowest and highest over the last 0.25 s of a 0.5 s sine note at `peakDbfs`, then after
// 0.5 s of silence, at SENSITIVITY `sens`.
struct Reading {
  float low;
  float high;
  float rest;
};

Reading driveOn(double peakDbfs, float sens) {
  Signal in = sine(kRate / 2, peakDbfs);
  in.resize(kRate, 0.0f);
  Reading r{1.0f, 0.0f, 1.0f};
  through(in, boxYawn(sens, 1.0f), [&](const Stage& s, int done) {
    if (done > kRate / 4 && done <= kRate / 2) {
      r.low = std::min(r.low, s.follower.drive());
      r.high = std::max(r.high, s.follower.drive());
    }
    if (done == kRate) r.rest = s.follower.drive();
  });
  return r;
}

// The full point is -6 - 0.30 x SENSITIVITY % dBFS and the drive rises over the 12 dB under it, so at
// 50 % it reads 1 at -21 dBFS, 0.5 at -27 dBFS and 0 at -33 dBFS. At 0 % the full point is -6 dBFS and
// at 100 % it is -36 dBFS. Silence reads 0.
bool testDriveReadings() {
  bool ok = true;
  struct Row {
    float sens;
    double db;
    float want;
  };
  for (const Row& row : {Row{0.5f, -21.0, 1.0f}, Row{0.5f, -27.0, 0.5f}, Row{0.5f, -33.0, 0.0f}, Row{0.0f, -6.0, 1.0f},
                         Row{0.0f, -18.0, 0.0f}, Row{1.0f, -36.0, 1.0f}, Row{1.0f, -48.0, 0.0f}}) {
    const Reading r = driveOn(row.db, row.sens);
    char name[96];
    std::snprintf(name, sizeof name, "drive: SENSITIVITY %.0f %%, a %.0f dBFS note reads %.1f", 100.0 * row.sens, row.db,
                  static_cast<double>(row.want));
    const float slack = row.want == 0.0f ? 0.0f : 0.04f;
    ok &= report(name, r.low >= row.want - slack && r.high <= row.want + 0.01f && r.rest == 0.0f,
                 "%.3f to %.3f, silence after %.3f", r.low, r.high, r.rest);
  }
  return ok;
}

// The time a one-pole's distance to its target takes to fall to 1/e, read between block ends on the log
// of the distance, which a one-pole stepped once a block follows in a straight line.
double timeToOneOverE(const std::vector<double>& distance) {
  const double ms = 1000.0 * cv::kBlock / kRate;
  double before = 1.0;
  for (size_t k = 0; k < distance.size(); ++k) {
    if (distance[k] <= std::exp(-1.0)) {
      const double t0 = ms * static_cast<double>(k);
      return t0 + ms * (-1.0 - std::log(before)) / (std::log(distance[k]) - std::log(before));
    }
    before = distance[k];
  }
  return -1.0;
}

// A step from silence to a held level of 0.25 for 100 ms, then back to silence for 300 ms, straight into
// the follower: it covers 1 - 1/e of the rise in 3 ms and of the fall in 70 ms.
bool testAttackRelease() {
  auto f = std::make_unique<cv::FormantFollower>();
  f->reset();
  cv::FormantFollowerParams p;
  p.placed = true;
  constexpr float kStep = 0.25f;
  std::array<float, cv::kBlock> held;
  held.fill(kStep);
  const std::array<float, cv::kBlock> quiet{};
  std::vector<double> rise;
  std::vector<double> fall;
  for (int b = 0; b < 75; ++b) {
    f->shiftFor(held.data(), cv::kBlock, p);
    rise.push_back(1.0 - f->level() / kStep);
  }
  const double top = f->level();
  for (int b = 0; b < 225; ++b) {
    f->shiftFor(quiet.data(), cv::kBlock, p);
    fall.push_back(f->level() / top);
  }
  const double attack = timeToOneOverE(rise);
  const double release = timeToOneOverE(fall);
  bool ok = report("follower: attack 3 ms on a step", std::fabs(attack - 3.0) < 0.1, "%.2f ms", attack);
  ok &= report("follower: release 70 ms on a step", std::fabs(release - 70.0) < 1.0, "%.2f ms", release);
  return ok;
}

// What one note of the sweep test read. Times are in samples from the note's start, each call read at
// its end.
struct NoteReading {
  float open = 0.0f;       // the sweep's highest over the first 30 ms
  int lastOnset = 0;       // the last call in the first 60 ms whose sweep sat on the drive, the last onset
  float half = -1.0f;      // the sweep at half the decay after that call
  float at150 = -1.0f;     // the sweep 150 ms into the note
  float held = 0.0f;       // the sweep's highest from one call past the decay's end to the note's end
  float shiftPeak = 0.0f;  // the glided SHIFT's deepest over the first 100 ms, in st
  float shiftRest = 0.0f;  // its furthest from 0 from 100 ms past the decay's end, five glide times, in st
};

// Reads one note from its calls' sweeps, drives and SHIFTs. An onset sets the ramp to 1, so the sweep of
// an onset call equals the drive.
NoteReading readNote(const std::vector<float>& sweep, const std::vector<float>& drive, const std::vector<float>& shift,
                     int decay) {
  NoteReading r;
  const auto timeOf = [](size_t b) { return static_cast<int>(b + 1) * cv::kBlock; };
  for (size_t b = 0; b < sweep.size(); ++b) {
    const int t = timeOf(b);
    if (t <= kRate * 6 / 100 && drive[b] > 0.0f && sweep[b] == drive[b]) r.lastOnset = t;
    if (t <= kRate * 3 / 100) r.open = std::max(r.open, sweep[b]);
    if (std::abs(t - kRate * 15 / 100) <= cv::kBlock / 2) r.at150 = sweep[b];
    if (t <= kRate / 10) r.shiftPeak = std::min(r.shiftPeak, shift[b]);
  }
  for (size_t b = 0; b < sweep.size(); ++b) {
    const int t = timeOf(b);
    if (std::abs(t - (r.lastOnset + decay / 2)) <= cv::kBlock / 2) r.half = sweep[b];
    if (t > r.lastOnset + decay + cv::kBlock) r.held = std::max(r.held, sweep[b]);
    if (t >= r.lastOnset + decay + kRate / 10) r.shiftRest = std::max(r.shiftRest, std::fabs(shift[b]));
  }
  return r;
}

// The sweep at SENSITIVITY 50 % and RANGE -7 st over `decayMs`: a -14 dBFS note held 0.6 s, 0.4 s of
// silence, and the note again. The rise detector refires the onset for the first 10 ms or so of each
// note, so the ramp starts from the last of those calls. Also reads the silence between the notes from
// 300 ms on.
bool testSweep(float decayMs) {
  constexpr int kNote = kRate * 6 / 10;
  constexpr int kGap = kRate * 4 / 10;
  Signal in = sawtooth(2 * kNote + kGap, 0.2f);
  for (int i = kNote; i < kNote + kGap; ++i) in[static_cast<size_t>(i)] = 0.0f;
  const int starts[2] = {0, kNote + kGap};
  std::vector<float> sweep[2];
  std::vector<float> drive[2];
  std::vector<float> shift[2];
  float restSweep = 0.0f;
  float restDrive = 0.0f;
  float restShift = 0.0f;
  through(in, boxYawn(0.5f, rangePosition(-7.0f), true, decayMs), [&](const Stage& s, int done) {
    if (done > kNote + kRate * 3 / 10 && done <= kNote + kGap) {
      restSweep = std::max(restSweep, s.follower.sweep());
      restDrive = std::max(restDrive, s.follower.drive());
      restShift = std::max(restShift, std::fabs(s.pitch.shiftSemis()));
    }
    for (int k = 0; k < 2; ++k) {
      const int t = done - starts[k];
      if (t <= 0 || t > kNote) continue;
      sweep[k].push_back(s.follower.sweep());
      drive[k].push_back(s.follower.drive());
      shift[k].push_back(s.pitch.shiftSemis());
    }
  });
  const int decay = static_cast<int>(decayMs * cv::kSmpPerMs);
  const NoteReading note[2] = {readNote(sweep[0], drive[0], shift[0], decay), readNote(sweep[1], drive[1], shift[1], decay)};

  bool ok = true;
  for (int k = 0; k < 2; ++k) {
    const NoteReading& r = note[k];
    char name[128];
    std::snprintf(name, sizeof name, "sweep: decay %.0f ms, note %d opens at 1, halves at half the decay, 0 from its end",
                  static_cast<double>(decayMs), k + 1);
    ok &= report(name, r.open > 0.999f && std::fabs(r.half - 0.5f) < 0.02f && r.held == 0.0f,
                 "open %.3f, last onset at %.1f ms, at half the decay %.3f, held %.3f", r.open,
                 1000.0 * r.lastOnset / kRate, r.half, r.held);

    // The ramp is a straight line from the last onset: 1 - (150 ms - onset) / decay at 150 ms, 0.456 at
    // the owner's 250 ms pick.
    const float line = 1.0f - static_cast<float>(kRate * 15 / 100 - r.lastOnset) / static_cast<float>(decay);
    std::snprintf(name, sizeof name, "sweep: decay %.0f ms, note %d falls in a straight line", static_cast<double>(decayMs),
                  k + 1);
    ok &= report(name, std::fabs(r.at150 - line) < 0.01f, "at 150 ms %.3f, the line %.3f", r.at150, line);
    std::snprintf(name, sizeof name, "sweep: decay %.0f ms, note %d SHIFT opens past -5.5 st and rests within 0.01 st of 0",
                  static_cast<double>(decayMs), k + 1);
    ok &= report(name, r.shiftPeak < -5.5f && r.shiftRest < 0.01f, "peak %+.2f st, rests within %.4f st", r.shiftPeak,
                 r.shiftRest);
  }
  char name[96];
  std::snprintf(name, sizeof name, "sweep: decay %.0f ms, silence from 300 ms reads 0 and SHIFT rests at 0",
                static_cast<double>(decayMs));
  ok &= report(name, restSweep == 0.0f && restDrive == 0.0f && restShift < 0.01f, "sweep %.3f, drive %.3f, SHIFT within %.4f st",
               restSweep, restDrive, restShift);
  return ok;
}

// One Formant voice at rate 0.5 (-12 st) and ratio 1, driven as the stage drives it at a steady `period`:
// calls of kBlock for 1 s at the lag the stage hands it, Autotune's voice delay. Its ring holds 1.0 up to
// `reach` samples behind the newest sample and NaN past that, so the output is the grains' window sum
// while every read stays within `reach`, and NaN once one reads past it.
struct WindowSum {
  bool finite;
  double low;   // the sum's lowest and highest after two grain lengths
  double high;
};

WindowSum halfSpeedSum(float period, int reach) {
  constexpr int kLen = cv::kVoiceRingLen;
  const auto at = [](int i) { return static_cast<size_t>((i % kLen + kLen) % kLen); };
  const float nan = std::nanf("");
  auto ring = std::make_unique<cv::VoiceRing>();
  ring->fill(nan);
  for (int d = 0; d <= reach; ++d) (*ring)[at(-1 - d)] = 1.0f;
  cv::formant_detail::FormantPsolaVoice voice;
  voice.reset();
  voice.latchGain(true);
  const float lag = static_cast<float>(cv::PsolaVoice::kGrainDelay) + period;
  const int settle = static_cast<int>(16.0f * period);
  WindowSum s{true, 1e9, 0.0};
  std::array<float, cv::kBlock> out;
  int head = 0;
  for (int t = 0; t < kRate; t += cv::kBlock) {
    const int first = head;
    for (int j = 0; j < cv::kBlock; ++j) (*ring)[at(head + j)] = 1.0f;
    head = static_cast<int>(at(head + cv::kBlock));
    for (int j = 0; j < cv::kBlock; ++j) (*ring)[at(head - 2 - reach - j)] = nan;
    voice.prepare(period, 1.0f, 0.5f, lag, cv::kBlock);
    out.fill(0.0f);
    voice.render(*ring, first, cv::kBlock, out.data());
    for (int j = 0; j < cv::kBlock; ++j) {
      const double v = out[static_cast<size_t>(j)];
      s.finite = s.finite && std::isfinite(v);
      if (t + j < settle) continue;
      s.low = std::min(s.low, v);
      s.high = std::max(s.high, v);
    }
  }
  return s;
}

// The fewest samples behind the newest that the voice's reads reach at `period`, found by bisection, or
// -1 when even the whole ring less a call is not enough.
int deepestRead(float period) {
  int lo = 0;
  int hi = cv::kVoiceRingLen - cv::kBlock - 2;
  if (!halfSpeedSum(period, hi).finite) return -1;
  while (hi - lo > 1) {
    const int mid = (lo + hi) / 2;
    if (halfSpeedSum(period, mid).finite) {
      hi = mid;
    } else {
      lo = mid;
    }
  }
  return hi;
}

// At -12 st a grain plays at rate 0.5 and lasts 2p / 0.5, four periods, so four overlap. At the 220 Hz
// tone's period and at the tracker's slowest, 686 samples, the window sum holds within 0.5 dB of unity
// through a held second, and the deepest read stays inside the ring with a call to spare.
bool testHalfSpeedVoice() {
  bool ok = true;
  for (const float period : {static_cast<float>(kRate) / 220.0f, static_cast<float>(cv::PitchTracker::kMaxPeriod)}) {
    const int deepest = deepestRead(period);
    const WindowSum s = halfSpeedSum(period, cv::kVoiceRingLen - cv::kBlock - 2);
    char name[112];
    std::snprintf(name, sizeof name, "half speed: period %.1f samples, window sum within 0.5 dB, reads inside the ring",
                  static_cast<double>(period));
    ok &= report(name, deepest > 0 && s.finite && dbOf(s.low) >= -0.5 && dbOf(s.high) <= 0.5,
                 "sum %+.3f to %+.3f dB; deepest read %.0f samples behind the newest, ring %.0f", dbOf(s.low), dbOf(s.high),
                 deepest, cv::kVoiceRingLen);
  }
  return ok;
}

// The stage on a -21 dBFS 220 Hz sine held 1 s at SENSITIVITY 100 %, so the drive sits at 1, and RANGE
// -12 st with a 2000 ms decay: the card hands the stage -12 st, the glided SHIFT passes -11.5 st, and
// the output stays finite and under 0 dBFS.
bool testHalfSpeedTone() {
  int handed = 0;
  float deepest = 0.0f;
  const Signal out = through(sine(kRate, -21.0, 220.0), boxYawn(1.0f, 0.0f, true, cv::kFollowerMaxDecayMs),
                             [&](const Stage& s, int) {
                               handed = std::min(handed, s.handed);
                               deepest = std::min(deepest, s.pitch.shiftSemis());
                             });
  const double peak = peakOf(out);
  const double voiced = peakOf(Signal(out.begin() + kRate / 10, out.end()));
  return report("half speed: a -21 dBFS 220 Hz tone at -12 st, finite and under 0 dBFS",
                handed == -12 && deepest < -11.5f && allFinite(out) && peak <= 1.0,
                "handed %+.0f st, SHIFT reaches %+.3f st, peak %+.2f dBFS, %+.2f dBFS from 100 ms", handed, deepest,
                dbOf(peak), dbOf(voiced));
}

using testutil::readWav;

// Adam's three clips, each after 0.4 s of silence so the enable fade settles. Empty when samples/ is
// absent; `missing` is set when the folder is there and a clip in it is missing or empty.
struct Clip {
  std::string name;
  Signal padded;
};
constexpr size_t kPad = kRate * 2 / 5;

std::vector<Clip> clips(bool& missing) {
  std::vector<Clip> out;
  missing = false;
  std::error_code ec;
  const bool folder = std::filesystem::is_directory(CUBEVOX_SAMPLES_DIR, ec);
  for (const char* name : {"adam_vox_sample_3s", "adam_vox_sample2_4s", "adam_vox_sample3_5s"}) {
    const Signal clip = readWav(std::string(CUBEVOX_SAMPLES_DIR) + "/" + name + ".wav");
    if (clip.empty()) {
      missing = missing || folder;
      continue;
    }
    Signal padded(kPad, 0.0f);
    padded.insert(padded.end(), clip.begin(), clip.end());
    out.push_back({name, padded});
  }
  return out;
}

// Every knob corner, SENSITIVITY 0 and 100 % by RANGE -12, 0 and +6 st by decay 10 and 2000 ms, on each
// clip.
bool testClipCorners(const std::vector<Clip>& all) {
  bool finite = true;
  int runs = 0;
  for (const Clip& c : all) {
    for (const float sens : {0.0f, 1.0f}) {
      for (const float range : {0.0f, 0.5f, 1.0f}) {
        for (const float decayMs : {cv::kFollowerMinDecayMs, cv::kFollowerMaxDecayMs}) {
          const Signal out = through(c.padded, boxYawn(sens, range, true, decayMs));
          for (const float v : out) finite = finite && std::isfinite(v);
          ++runs;
        }
      }
    }
  }
  return report("clips: finite at every knob corner", finite, "%.0f runs", runs);
}

// Out/in RMS after the pad.
double levelDb(const Clip& c, const Signal& out) {
  return dbOf(rmsRange(out, kPad, out.size()) / rmsRange(c.padded, kPad, c.padded.size()));
}

// The card on each clip, out/in RMS after the pad.
double levelDb(const Clip& c, const ChainParams& p) { return levelDb(c, through(c.padded, p)); }

// The pot's two ends at SENSITIVITY 50 % and the 300 ms decay on each clip. At -12 st, where the grains
// play at half speed, the output stays finite and under 0 dBFS. The level at both ends prints beside it;
// the level rule binds the default only.
bool testClipEnds(const std::vector<Clip>& all) {
  bool ok = true;
  for (const Clip& c : all) {
    const Signal deep = through(c.padded, boxYawn(0.5f, 0.0f));
    const Signal up = through(c.padded, boxYawn(0.5f, 1.0f));
    char name[96];
    std::snprintf(name, sizeof name, "half speed: -12 st on %s, finite and under 0 dBFS", c.name.c_str());
    ok &= report(name, allFinite(deep) && peakOf(deep) <= 1.0 && allFinite(up), "peak %+.2f dBFS, the input's %+.2f dBFS",
                 dbOf(peakOf(deep)), dbOf(peakOf(c.padded)));
    std::printf("NOTE level SENSITIVITY 50 %% %-22s RANGE -12 st %+6.2f dB, +6 st %+6.2f dB (peak %+.2f dBFS)\n",
                c.name.c_str(), levelDb(c, deep), levelDb(c, up), dbOf(peakOf(up)));
  }
  return ok;
}

// The level rule: the card at its default reads 0 to +0.5 dB over its input on each clip. The owner's
// three picks print beside it.
bool testLevel(const std::vector<Clip>& all) {
  bool ok = true;
  for (const Clip& c : all) {
    const double db = levelDb(c, boxDefault());
    char name[96];
    std::snprintf(name, sizeof name, "level: the default on %s", c.name.c_str());
    ok &= report(name, db >= 0.0 && db <= 0.5, "%+.2f dB (want 0..+0.5)", db);
  }
  struct Pick {
    float sens;
    float decayMs;
  };
  for (const Pick& pick : {Pick{0.5f, 250.0f}, Pick{0.5f, 500.0f}, Pick{0.25f, 250.0f}}) {
    for (const Clip& c : all) {
      std::printf("NOTE level SENSITIVITY %.0f %% RANGE -7 st decay %.0f ms %-22s %+6.2f dB\n", 100.0 * pick.sens,
                  static_cast<double>(pick.decayMs), c.name.c_str(),
                  levelDb(c, boxYawn(pick.sens, rangePosition(-7.0f), true, pick.decayMs)));
    }
  }
  return ok;
}

}  // namespace

int main() {
  bool ok = testOffExact();
  ok &= testRangeMap();
  ok &= testDriveReadings();
  ok &= testAttackRelease();
  ok &= testSweep(250.0f);
  ok &= testSweep(cv::kFollowerDefaultDecayMs);
  ok &= testUpSweep();
  ok &= testHalfSpeedVoice();
  ok &= testHalfSpeedTone();
  bool missing = false;
  const std::vector<Clip> all = clips(missing);
  if (missing) {
    std::printf("FAIL clips: samples/ is here and a clip in it is missing or empty\n");
    ok = false;
  } else if (all.empty()) {
    std::printf("SKIP clips: samples/ is not in this checkout\n");
  } else {
    ok &= testClipCorners(all);
    ok &= testClipEnds(all);
    ok &= testLevel(all);
  }
  std::printf("%s formant_follower\n", ok ? "PASS" : "FAIL");
  return ok ? 0 : 1;
}
