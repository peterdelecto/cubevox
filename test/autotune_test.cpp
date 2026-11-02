// Autotune checks: passthrough, scale degrees, correction, out-of-scale notes,
// response time, Octave builds on the corrected note, hard tune, the toggle and
// its level, the halfway point, the tracker's range ends, the first note's glide,
// bleed, the plain copy after it goes off, call sizes, no-alloc. No window, no
// audio device.

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <vector>

#include "engine/pitch_fx.h"

namespace {

std::atomic<bool> gInProcess{false};

void guardAlloc() {
  if (gInProcess.load()) std::abort();
}

}  // namespace

// Allocation in process() aborts the test binary.
void* operator new(std::size_t n) {
  guardAlloc();
  void* p = std::malloc(n);
  if (!p) std::abort();
  return p;
}
void* operator new[](std::size_t n) { return operator new(n); }
void operator delete(void* p) noexcept {
  guardAlloc();
  std::free(p);
}
void operator delete[](void* p) noexcept { operator delete(p); }
void operator delete(void* p, std::size_t) noexcept { operator delete(p); }
void operator delete[](void* p, std::size_t) noexcept { operator delete(p); }

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kA3 = 220.0;
constexpr double kSharp = 226.4;   // A3 + 50 cents
constexpr double kBb3 = 233.1;
constexpr double kB3 = 246.94;
constexpr double kC4 = 261.63;
constexpr int kSettle = cv::kSampleRate * 3 / 10;  // 300 ms
constexpr double kOctaveVibratoCents = 5.5;  // the voice reads 2.7 following the correction, 10.4 behind a 15 ms glide

// Ten harmonics at 1/k amplitude, peak 0.5. hzAt gives the pitch per sample,
// so a step keeps the phase continuous.
template <typename F>
std::vector<float> tone(int n, F hzAt) {
  std::vector<double> x(static_cast<size_t>(n), 0.0);
  double phase = 0.0;
  double peak = 0.0;
  for (int i = 0; i < n; ++i) {
    double s = 0.0;
    for (int k = 1; k <= 10; ++k) s += std::sin(k * phase) / k;
    phase += 2.0 * kPi * hzAt(i) / cv::kSampleRate;
    x[static_cast<size_t>(i)] = s;
    peak = std::max(peak, std::fabs(s));
  }
  std::vector<float> out(x.size());
  for (size_t i = 0; i < x.size(); ++i) out[i] = static_cast<float>(0.5 * x[i] / peak);
  return out;
}

std::vector<float> tone(double hz, int n) {
  return tone(n, [hz](int) { return hz; });
}

double cents(double hz, double ref) { return 1200.0 * std::log2(hz / ref); }

// Median f0 after 300 ms, measured by a fresh tracker.
double medianHz(const std::vector<float>& x) {
  cv::PitchTracker t;
  std::vector<double> hz;
  const int total = static_cast<int>(x.size());
  for (int i = 0; i < total; i += cv::kBlock) {
    const int n = std::min(cv::kBlock, total - i);
    t.push(&x[static_cast<size_t>(i)], n);
    if (i + n > kSettle && t.result().voiced) hz.push_back(t.result().hz);
  }
  if (hz.empty()) return 0.0;
  std::sort(hz.begin(), hz.end());
  return hz[hz.size() / 2];
}

// Tracker pitch and Autotune correction read after one block.
struct Hop {
  int end;
  bool voiced;
  float hz;
  float corr;
};

// Runs in through fx in calls of callSize samples, recording a Hop after each call.
std::vector<float> run(cv::PitchFx& fx, const std::vector<float>& in, const cv::PitchFxParams& p,
                       std::vector<Hop>* hops = nullptr, int callSize = cv::kBlock) {
  std::vector<float> out(in.size());
  if (hops) hops->reserve(in.size() / static_cast<size_t>(callSize) + 1);
  const int total = static_cast<int>(in.size());
  for (int i = 0; i < total; i += callSize) {
    const int n = std::min(callSize, total - i);
    gInProcess = true;
    fx.process(&in[static_cast<size_t>(i)], &out[static_cast<size_t>(i)], n, p);
    gInProcess = false;
    if (hops) hops->push_back({i + n, fx.pitch().voiced, fx.pitch().hz, fx.correctionSemis()});
  }
  return out;
}

bool report(const char* name, bool ok, const char* fmt, double a, double b = 0.0,
            double c = 0.0, double d = 0.0) {
  char detail[200];
  std::snprintf(detail, sizeof detail, fmt, a, b, c, d);
  std::printf("%s %s  %s\n", ok ? "PASS" : "FAIL", name, detail);
  return ok;
}

static cv::PitchFx gFx;

// Autotune alone in key C; Octave off.
cv::PitchFxParams autotuneOnly(float responseMs) {
  cv::PitchFxParams p;
  p.autotune.on = true;
  p.autotune.key = 0;
  p.autotune.responseMs = responseMs;
  return p;
}

double outputHz(double inHz, const cv::PitchFxParams& p) {
  gFx.reset();
  return medianHz(run(gFx, tone(inHz, 2 * cv::kSampleRate), p));
}

double rms(const std::vector<float>& x, int from, int to) {
  double s = 0.0;
  for (int i = from; i < to; ++i) s += static_cast<double>(x[static_cast<size_t>(i)]) * x[static_cast<size_t>(i)];
  return std::sqrt(s / (to - from));
}

int countDiff(const std::vector<float>& a, const std::vector<float>& b, int from) {
  int n = 0;
  for (size_t i = static_cast<size_t>(from); i < a.size(); ++i)
    if (a[i] != b[i]) ++n;
  return n;
}

// 1. Off from the start is bit-exact. On with an in-tune A3, the output matches the input delayed by
// the voice's latency, the grain delay plus one period. That delay is not a whole number of samples
// and a steady tone matches at every period of lag, so the test takes the best whole-sample lag within
// 500 samples of kGrainDelay and prints only the residual.
bool testPassthrough() {
  const std::vector<float> in = tone(kA3, cv::kSampleRate);
  cv::PitchFxParams off = autotuneOnly(60.0f);
  off.autotune.on = false;
  gFx.reset();
  const int diff = countDiff(in, run(gFx, in, off), 0);
  bool ok = report("passthrough off", diff == 0, "samples differing=%.0f (0)", diff);

  gFx.reset();
  const std::vector<float> out = run(gFx, in, autotuneOnly(60.0f));
  const int from = kSettle;
  const int to = static_cast<int>(in.size());
  const double ref = rms(in, from, to);
  double bestDb = 999.0;
  for (int lag = cv::PsolaVoice::kGrainDelay - 500; lag <= cv::PsolaVoice::kGrainDelay + 500; ++lag) {
    double s = 0.0;
    for (int i = from; i < to; ++i) {
      const double d = out[static_cast<size_t>(i)] - in[static_cast<size_t>(i - lag)];
      s += d * d;
    }
    const double db = 20.0 * std::log10(std::sqrt(s / (to - from)) / ref + 1e-12);
    bestDb = std::min(bestDb, db);
  }
  ok &= report("passthrough in tune", bestDb <= -40.0,
               "|out - in| RMS=%.1f dB re input at the best lag within 500 samples of the grain delay (<= -40)",
               bestDb);
  return ok;
}

// 2. Scale degrees: every semitone of C major from C4 to B4 in key C. A scale note is its own
// degree; a note between two scale notes sits a tie away from both and snaps to the lower one.
bool testDegrees() {
  constexpr int kDegreeOf[12] = {0, 0, 1, 1, 2, 3, 3, 4, 4, 5, 5, 6};
  constexpr float kDevOf[12] = {0, 1, 0, 1, 0, 0, 1, 0, 1, 0, 1, 0};
  int bad = 0;
  for (int s = 0; s < 12; ++s) {
    const float hz = 261.6256f * std::exp2(static_cast<float>(s) / 12.0f);
    const cv::Degree d = cv::degreeOf(hz, 0);
    if (d.index != kDegreeOf[s] || std::fabs(d.dev - kDevOf[s]) > 1e-3f) ++bad;
  }
  // G major (KEY position 1) holds F#, so F#4 is its own degree there.
  const cv::Degree fs = cv::degreeOf(369.994f, 1);
  const bool fsOk = fs.index == 6 && std::fabs(fs.dev) < 1e-3f;
  return report("scale degrees", bad == 0 && fsOk, "wrong degrees in C=%.0f (0), F#4 in G degree %.0f (6)", bad,
                fs.index);
}

// 3. A3 + 50 cents pulls to A3.
bool testCorrection() {
  const double got = outputHz(kSharp, autotuneOnly(20.0f));
  const double err = got > 0.0 ? cents(got, kA3) : 9999.0;
  return report("correction", std::fabs(err) <= 5.0, "in=%.2f Hz, out=%.2f Hz, err vs A3=%.2f cents (+-5)",
                kSharp, got, err);
}

// 4. Bb3 is not in C major: Autotune pulls it to A3 or B3.
bool testScale() {
  const double got = outputHz(kBb3, autotuneOnly(20.0f));
  const double errA = got > 0.0 ? cents(got, kA3) : 9999.0;
  const double errB = got > 0.0 ? cents(got, kB3) : 9999.0;
  return report("scale Bb3 in C", std::fabs(errA) <= 5.0 || std::fabs(errB) <= 5.0,
                "out=%.2f Hz, err vs A3=%.2f, vs B3=%.2f cents (one within +-5)", got, errA, errB);
}

// 5. Step A3 to A3 + 50 cents at 1 s, response 200 ms. Time runs from the
// first hop where the tracker reports > 225 Hz to the hop where the
// correction has moved 63 % of the way to its final value.
bool testResponse() {
  constexpr int kStep = cv::kSampleRate;
  const std::vector<float> in =
      tone(3 * cv::kSampleRate, [](int i) { return i < kStep ? kA3 : kSharp; });
  std::vector<Hop> hops;
  gFx.reset();
  run(gFx, in, autotuneOnly(200.0f), &hops);

  size_t first = 0;
  while (first < hops.size() && !(hops[first].end > kStep && hops[first].hz > 225.0f)) ++first;
  if (first == hops.size()) return report("response 200 ms", false, "tracker never reported > 225 Hz", 0.0);
  const double start = hops[first - 1].corr;
  const double final = hops.back().corr;
  const double goal = start + 0.63 * (final - start);
  double hitMs = -1.0;
  for (size_t h = first; h < hops.size(); ++h) {
    if (std::fabs(hops[h].corr - start) >= std::fabs(goal - start)) {
      const Hop& a = hops[h - 1];
      const Hop& b = hops[h];
      const double end = a.end + (b.end - a.end) * (goal - a.corr) / (b.corr - a.corr);
      hitMs = (end - hops[first].end) * 1000.0 / cv::kSampleRate;
      break;
    }
  }
  return report("response 200 ms", hitMs >= 150.0 && hitMs <= 250.0,
                "63%% of %.1f cents at %.1f ms after the tracker saw the step (150..250)",
                (final - start) * 100.0, hitMs);
}

// 6. Octave +3 on the corrected note: A3 + 50 cents gives C4, not C4 + 50. The
// granular shifter holds +-15 cents.
bool testOctaveFollows() {
  cv::PitchFxParams p = autotuneOnly(20.0f);
  p.octave.on = true;
  p.octave.mix = 1.0f;
  p.octave.semitones = 3;
  const double got = outputHz(kSharp, p);
  const double err = got > 0.0 ? cents(got, kC4) : 9999.0;
  return report("feeds octave", std::fabs(err) <= 15.0,
                "out=%.2f Hz, err vs C4=%.2f cents (+-15)", got, err);
}

// 6b. Octave +12 on a note that hard tune holds on A3 against a 5.5 Hz, +-30 cent vibrato comes out on
// A4. The correction is what cancels the vibrato, so the voice has to follow it as it moves: the
// output pitch's RMS distance from A4, read by a fresh tracker.
bool testOctaveFollowsVibrato() {
  constexpr double kVibratoHz = 5.5;
  constexpr double kA4 = 2.0 * kA3;
  const std::vector<float> in = tone(3 * cv::kSampleRate, [](int i) {
    return kA3 * std::exp2(0.3 / 12.0 * std::sin(2.0 * kPi * kVibratoHz * i / cv::kSampleRate));
  });
  cv::PitchFxParams p = autotuneOnly(cv::AutotuneParams{}.responseMs);
  p.octave.on = true;
  p.octave.mix = 1.0f;
  p.octave.semitones = 12;
  gFx.reset();
  const std::vector<float> out = run(gFx, in, p);
  cv::PitchTracker t;
  double sum = 0.0;
  int n = 0;
  const int total = static_cast<int>(out.size());
  for (int i = 0; i < total; i += cv::kBlock) {
    t.push(&out[static_cast<size_t>(i)], std::min(cv::kBlock, total - i));
    if (i > kSettle && t.result().voiced) {
      const double c = cents(t.result().rawHz, kA4);
      sum += c * c;
      ++n;
    }
  }
  const double rmsCents = n > 0 ? std::sqrt(sum / n) : 9999.0;
  return report("octave follows vibrato", rmsCents <= kOctaveVibratoCents,
                "octave +12 on hard-tuned A3 under 5.5 Hz vibrato: %.2f cents RMS from A4 (<= %.1f)", rmsCents,
                kOctaveVibratoCents);
}

// The process() call sizes every call-size case runs: odd, short, the box's block, and longer.
constexpr int kCallSizes[] = {17, 40, cv::kBlock, 200};

int countNonFinite(const std::vector<float>& x) {
  int n = 0;
  for (float v : x) n += std::isfinite(v) ? 0 : 1;
  return n;
}

// 7. Hard tune at the shipped defaults: A3 with a 5 Hz, +-30 cent vibrato comes out held on A3, at
// every call size. Adam's own box measures about 2 cents median off the note.
double hardTuneCents(int callSize, int& nonFinite) {
  const std::vector<float> in = tone(3 * cv::kSampleRate, [](int i) {
    return kA3 * std::exp2(0.3 / 12.0 * std::sin(2.0 * kPi * 5.0 * i / cv::kSampleRate));
  });
  cv::PitchFxParams p = autotuneOnly(cv::AutotuneParams{}.responseMs);
  gFx.reset();
  const std::vector<float> out = run(gFx, in, p, nullptr, callSize);
  nonFinite += countNonFinite(out);
  cv::PitchTracker t;
  double sum = 0.0;
  int n = 0;
  const int total = static_cast<int>(out.size());
  for (int i = 0; i < total; i += cv::kBlock) {
    t.push(&out[static_cast<size_t>(i)], std::min(cv::kBlock, total - i));
    if (i > kSettle && t.result().voiced) {
      const double c = cents(t.result().rawHz, kA3);
      sum += c * c;
      ++n;
    }
  }
  return n > 0 ? std::sqrt(sum / n) : 9999.0;
}

bool testHardTune() {
  bool ok = true;
  for (int size : kCallSizes) {
    int nonFinite = 0;
    const double rmsCents = hardTuneCents(size, nonFinite);
    char name[48];
    std::snprintf(name, sizeof name, "hard tune holds vibrato, calls of %d", size);
    ok &= report(name, rmsCents <= 4.0 && nonFinite == 0,
                 "+-30 cent vibrato in, %.2f cents RMS off A3 out (<= 4), non-finite samples %.0f (0)", rmsCents,
                 nonFinite);
  }
  return ok;
}

// Largest |second difference| of x over [from, to): a cut in the waveform shows as a spike here.
double peakCurvature(const std::vector<float>& x, int from, int to) {
  double worst = 0.0;
  for (int i = std::max(from, 2); i < to; ++i) {
    const size_t k = static_cast<size_t>(i);
    worst = std::max(worst, std::fabs(static_cast<double>(x[k]) - 2.0 * x[k - 1] + x[k - 2]));
  }
  return worst;
}

// Largest |x| over [from, to).
double peakLevel(const std::vector<float>& x, int from, int to) {
  double worst = 0.0;
  for (int i = from; i < to; ++i) worst = std::max(worst, std::fabs(static_cast<double>(x[static_cast<size_t>(i)])));
  return worst;
}

// Autotune off for gapBlocks and on again over a held A3 + 50 cents, in calls of callSize samples; a
// call that overlaps the gap runs off. The output crossfades between the corrected voice and the dry
// input, so for its level it may bend no harder than either does alone. The equal-power crossfade
// lifts the level where the two voices add in phase (the toggle level check bounds that rise), so a
// fade window that peaks above steady state reads against its own peak. Returns the peak curvature
// from the off edge to 100 ms past the on edge over the larger steady peak, so scaled.
double toggleRatio(int gapBlocks, int callSize, int& nonFinite) {
  const int offAt = cv::kSampleRate / cv::kBlock * cv::kBlock;
  const int onAt = offAt + gapBlocks * cv::kBlock;
  const int total = offAt + 2 * cv::kSampleRate;
  const std::vector<float> in = tone(kSharp, total);
  std::vector<float> out(in.size());
  const cv::PitchFxParams on = autotuneOnly(20.0f);
  cv::PitchFxParams off = on;
  off.autotune.on = false;
  gFx.reset();
  for (int i = 0; i < total; i += callSize) {
    const int n = std::min(callSize, total - i);
    const bool gap = i < onAt && i + n > offAt;
    gInProcess = true;
    gFx.process(&in[static_cast<size_t>(i)], &out[static_cast<size_t>(i)], n, gap ? off : on);
    gInProcess = false;
  }
  nonFinite += countNonFinite(out);
  const int offCall = offAt / callSize * callSize;                 // the first call that runs off
  const int onCall = (onAt + callSize - 1) / callSize * callSize;  // the first call back on
  const int to = onCall + cv::kSampleRate / 10;
  const int window = cv::kSampleRate / 2;
  const double steady = std::max({peakCurvature(out, offCall - window, offCall),
                                  peakCurvature(out, total - window, total), peakCurvature(in, 0, total)});
  const double steadyPeak = std::max({peakLevel(out, offCall - window, offCall), peakLevel(out, total - window, total),
                                      peakLevel(in, 0, total)});
  const double rise = std::max(1.0, peakLevel(out, offCall, to) / steadyPeak);
  return peakCurvature(out, offCall, to) / steady / rise;
}

// 8. A flick inside the crossfade keeps the running voice; a long gap lets the crossfade settle,
// and the voice then starts clean once the tracker has rebuilt, at every call size. The fade's own
// slope adds under 1 %; a voice that loses its period at the off switch reads 1.4 to 5.4 times
// steady state.
bool testToggle() {
  constexpr double kMaxRatio = 1.05;
  bool ok = true;
  for (int size : kCallSizes) {
    double worst = 0.0;
    int worstGap = 0;
    int nonFinite = 0;
    for (int gap : {1, 4, 15, 60, 300}) {
      const double r = toggleRatio(gap, size, nonFinite);
      if (r >= worst) {
        worst = r;
        worstGap = gap;
      }
    }
    char name[32];
    std::snprintf(name, sizeof name, "toggle, calls of %d", size);
    ok &= report(name, worst <= kMaxRatio && nonFinite == 0,
                 "off for 1.3 to 400 ms: peak curvature x%.3f of steady state at %.0f blocks off (<= %.2f), "
                 "non-finite samples %.0f (0)",
                 worst, worstGap, kMaxRatio, nonFinite);
  }
  return ok;
}

// The crossfade's level over steady tones: Autotune on at 0.5 s and off at 1.25 s while the Crowd card
// keeps the tracker running, in two-period windows over the 200 ms after each switch. The dip reads
// against the quieter of the dry input and the corrected voice, the rise against the louder.
struct ToggleLevel {
  double dip = 0.0;
  double rise = -99.0;
};

void toggleLevelAt(double hz, ToggleLevel& worst) {
  const int n = 2 * cv::kSampleRate;
  const std::vector<float> in = tone(hz, n);
  cv::PitchFxParams on = autotuneOnly(1.0f);
  on.crowdOn = true;
  cv::PitchFxParams off = on;
  off.autotune.on = false;
  gFx.reset();
  const std::vector<float> wet = run(gFx, in, on);
  const double dry = rms(in, cv::kSampleRate / 4, cv::kSampleRate / 2);
  const double corrected = rms(wet, cv::kSampleRate, cv::kSampleRate * 3 / 2);
  const int onAt = cv::kSampleRate / 2 / cv::kBlock * cv::kBlock;
  const int offAt = cv::kSampleRate * 5 / 4 / cv::kBlock * cv::kBlock;
  std::vector<float> out(in.size());
  gFx.reset();
  for (int i = 0; i < n; i += cv::kBlock) {
    gInProcess = true;
    gFx.process(&in[static_cast<size_t>(i)], &out[static_cast<size_t>(i)], cv::kBlock, i < onAt || i >= offAt ? off : on);
    gInProcess = false;
  }
  const int win = static_cast<int>(std::lround(2.0 * cv::kSampleRate / hz));
  for (int edge : {onAt, offAt}) {
    for (int i = edge; i + win < edge + cv::kSampleRate / 5; i += 24) {
      const double r = rms(out, i, i + win);
      worst.dip = std::min(worst.dip, 20.0 * std::log10(r / std::min(dry, corrected)));
      worst.rise = std::max(worst.rise, 20.0 * std::log10(r / std::max(dry, corrected)));
    }
  }
}

// 9. Switching Autotune on or off mid-note: the corrected voice lags the dry input by 2 to 5 periods
// and sits up to a semitone away, so the equal-power crossfade dips where the two cancel and rises
// where they add. Pinned 0.5 dB over this pass's figures (dip 3.9 dB, rise 2.5 dB); the linear fade
// before it dipped 6.9 dB.
bool testToggleLevel() {
  constexpr double kMaxDipDb = 4.4;
  constexpr double kMaxRiseDb = 3.0;
  ToggleLevel worst;
  for (double hz : {97.0, 98.0, 99.0, 100.0, 101.0, 102.0, 103.0, 104.0, 105.0, 106.0, 110.0, 130.81, 146.83, 220.0})
    toggleLevelAt(hz, worst);
  return report("toggle level", -worst.dip <= kMaxDipDb && worst.rise <= kMaxRiseDb,
                "tones 97 to 220 Hz, two-period windows: largest dip %.2f dB (<= %.1f), largest rise %.2f dB (<= %.1f)",
                -worst.dip, kMaxDipDb, worst.rise, kMaxRiseDb);
}

double midiOf(double hz) { return 69.0 + 12.0 * std::log2(hz / 440.0); }

// The output's note per hop, read by a fresh tracker from 500 ms on.
struct Notes {
  double changesPerSec;  // changes of the nearest semitone
  double lo;             // lowest and highest pitch, MIDI
  double hi;
};

Notes outputNotes(const std::vector<float>& out) {
  cv::PitchTracker t;
  int changes = 0, prev = -999;
  double lo = 1e9, hi = -1e9;
  const int total = static_cast<int>(out.size());
  for (int i = 0; i + cv::kBlock <= total; i += cv::kBlock) {
    t.push(&out[static_cast<size_t>(i)], cv::kBlock);
    const int end = i + cv::kBlock;
    if (end < cv::kSampleRate / 2 || end % cv::PitchTracker::kHop != 0 || !t.result().voiced) continue;
    const double m = midiOf(t.result().rawHz);
    lo = std::min(lo, m);
    hi = std::max(hi, m);
    const int note = static_cast<int>(std::lround(m));
    if (prev != -999 && note != prev) ++changes;
    prev = note;
  }
  return {changes / ((total - cv::kSampleRate / 2) / static_cast<double>(cv::kSampleRate)), lo, hi};
}

// 10. A pitch near the halfway point between two scale notes holds one note at MECHANICAL. C#4 15
// cents past halfway in KEY C under a 5 Hz, +-30 cent vibrato crosses the halfway point ten times a
// second, and before the hold the output jumped C4 to D4 as often. A steady A3 exactly halfway in KEY
// Eb holds Ab3.
bool testHalfway() {
  const double cs4Past = 440.0 * std::exp2((61.15 - 69.0) / 12.0);
  const std::vector<float> vib = tone(3 * cv::kSampleRate, [cs4Past](int i) {
    return cs4Past * std::exp2(0.3 / 12.0 * std::sin(2.0 * kPi * 5.0 * i / cv::kSampleRate));
  });
  cv::PitchFxParams p = autotuneOnly(1.0f);
  gFx.reset();
  const Notes a = outputNotes(run(gFx, vib, p));
  p.autotune.key = 9;  // Eb / Cm
  gFx.reset();
  const Notes b = outputNotes(run(gFx, tone(kA3, 3 * cv::kSampleRate), p));
  const bool held = b.changesPerSec == 0.0 && std::fabs(b.lo - 56.0) <= 0.05 && std::fabs(b.hi - 56.0) <= 0.05;
  return report("halfway holds", a.changesPerSec == 0.0 && held,
                "C#4 +15 c vibrato in C: %.1f note changes a second (0); A3 in Eb: %.1f a second (0), out MIDI "
                "%.2f..%.2f (56 +-0.05)",
                a.changesPerSec, b.changesPerSec, b.lo, b.hi);
}

// Normalised autocorrelation of x over [from, from + w) against x lag samples ahead.
double ncorr(const std::vector<float>& x, int from, int w, double lag) {
  const int li = static_cast<int>(std::floor(lag));
  const double fr = lag - li;
  double sxy = 0.0, sxx = 0.0, syy = 0.0;
  for (int i = from; i < from + w; ++i) {
    const size_t j = static_cast<size_t>(i + li);
    const double a = x[static_cast<size_t>(i)];
    const double b = (1.0 - fr) * x[j] + fr * x[j + 1];
    sxy += a * b;
    sxx += a * a;
    syy += b * b;
  }
  return sxy / std::sqrt(std::max(sxx * syy, 1e-30));
}

// The pitch of x near nearHz over one second from 600 ms, for pitches the tracker cannot read: the
// best normalised autocorrelation at m periods, m chosen so the lag spans 400 samples or more, in
// quarter samples within half a period of m periods (3 semitones when m is 1).
double periodicHz(const std::vector<float>& x, double nearHz) {
  const double p = cv::kSampleRate / nearHz;
  const int m = std::max(1, static_cast<int>(400.0 / p));
  const double span = m == 1 ? std::exp2(3.0 / 12.0) : 1.0 + 0.45 / m;
  double best = -2.0, bestLag = m * p;
  for (double lag = std::floor(m * p / span); lag <= m * p * span; lag += 0.25) {
    const double r = ncorr(x, cv::kSampleRate * 6 / 10, cv::kSampleRate, lag);
    if (r > best) {
      best = r;
      bestLag = lag;
    }
  }
  return m * cv::kSampleRate / bestLag;
}

// 11. At the ends of the tracker's range the note passes uncorrected. 68 Hz reads at the slow end of
// the search and 1,030 Hz at the fast end, both flagged clamped, and the output stays on the sung note;
// before this pass the edge reading pulled them 85 cents sharp and 21 cents flat. 1,200 Hz reads an
// octave down, unclamped, and still lands D6.
bool testRangeEnds() {
  const cv::PitchFxParams p = autotuneOnly(1.0f);
  gFx.reset();
  const double low = cents(periodicHz(run(gFx, tone(68.0, 2 * cv::kSampleRate), p), 68.0), 68.0);
  gFx.reset();
  const double high = cents(periodicHz(run(gFx, tone(1030.0, 2 * cv::kSampleRate), p), 1030.0), 1030.0);
  constexpr double kD6 = 1174.66;
  gFx.reset();
  const double above = cents(periodicHz(run(gFx, tone(1200.0, 2 * cv::kSampleRate), p), kD6), kD6);
  return report("range ends", std::fabs(low) <= 15.0 && std::fabs(high) <= 15.0 && std::fabs(above) <= 11.0,
                "68 Hz out %+.1f c off the sung note, 1,030 Hz %+.1f c (+-15); 1,200 Hz out %+.1f c off D6 (+-11)", low,
                high, above);
}

// 12. The first note after a reset glides at RESPONSE like every other note. At 500 ms, A3 + 40 cents
// from a reset and D4 - 30 cents after 250 ms of silence each move the same share of their way in the
// 50 ms after the tracker first publishes them, within 10 %. Before, the first note snapped 98 % of
// its way at once.
double shareIn50Ms(const std::vector<Hop>& hops, size_t first, double want) {
  const double start = first > 0 ? hops[first - 1].corr : 0.0;
  const size_t at = first + static_cast<size_t>(cv::kSampleRate / 20 / cv::kBlock);
  return (hops[at].corr - start) / (want - start);
}

bool testFirstNote() {
  const int n1 = cv::kSampleRate * 6 / 10, gap = cv::kSampleRate / 4, n2 = cv::kSampleRate * 6 / 10;
  std::vector<float> in = tone(440.0 * std::exp2((57.4 - 69.0) / 12.0), n1);
  in.insert(in.end(), static_cast<size_t>(gap), 0.0f);
  const std::vector<float> second = tone(440.0 * std::exp2((61.7 - 69.0) / 12.0), n2);
  in.insert(in.end(), second.begin(), second.end());
  std::vector<Hop> hops;
  gFx.reset();
  run(gFx, in, autotuneOnly(500.0f), &hops);
  size_t f1 = 0, f2 = 0;
  while (f1 < hops.size() && hops[f1].hz <= 0.0f) ++f1;
  while (f2 < hops.size() && !(hops[f2].end > n1 + gap && hops[f2].hz > 280.0f)) ++f2;
  if (f2 + cv::kSampleRate / 20 / cv::kBlock >= hops.size()) return report("first note glides", false, "no second note", 0.0);
  const double s1 = shareIn50Ms(hops, f1, -0.4), s2 = shareIn50Ms(hops, f2, 0.3);
  return report("first note glides", std::fabs(s1 - s2) <= 0.1 * s2,
                "RESPONSE 500 ms, share of the way in 50 ms: first note %.1f %%, next note after a gap %.1f %% "
                "(within 10 %%)",
                100.0 * s1, 100.0 * s2);
}

// Pink noise from a seeded LCG through Kellet's three-pole filter, as crowd_test's program makes it,
// scaled to rmsDb dBFS.
std::vector<float> pinkNoise(int n, double rmsDb) {
  std::vector<double> x(static_cast<size_t>(n));
  uint64_t s = 0x9E3779B97F4A7C15ULL;
  double b0 = 0.0, b1 = 0.0, b2 = 0.0, sum = 0.0;
  for (double& v : x) {
    s = s * 6364136223846793005ULL + 1442695040888963407ULL;
    const double w = static_cast<double>(s >> 11) / 4503599627370496.0 - 1.0;
    b0 = 0.99765 * b0 + w * 0.0990460;
    b1 = 0.96300 * b1 + w * 0.2965164;
    b2 = 0.57000 * b2 + w * 1.0526913;
    v = b0 + b1 + b2 + w * 0.1848;
    sum += v * v;
  }
  const double g = std::pow(10.0, rmsDb / 20.0) / std::sqrt(sum / n);
  std::vector<float> out(x.size());
  for (size_t i = 0; i < x.size(); ++i) out[i] = static_cast<float>(x[i] * g);
  return out;
}

// 13. A wrong tracker frame stays out of the target. A3 at -20 dBFS with pink noise 5 dB under reads a
// period three times too long on some hops, which in KEY E lands halfway between two notes; the target
// takes the median there instead. Voiced hops pulled 0.2 st or more off the in-tune A3 over 3 s: 28
// before this pass, 16 now.
bool testBleed() {
  const int n = 3 * cv::kSampleRate;
  std::vector<float> x = tone(kA3, n);
  const float toneGain = static_cast<float>(std::pow(10.0, -20.0 / 20.0) / rms(x, 0, n));
  const std::vector<float> noise = pinkNoise(n, -25.0);
  for (size_t i = 0; i < x.size(); ++i) x[i] = x[i] * toneGain + noise[i];
  cv::PitchFxParams p = autotuneOnly(1.0f);
  p.autotune.key = 4;  // E / C#m
  std::vector<Hop> hops;
  gFx.reset();
  run(gFx, x, p, &hops);
  int pulled = 0;
  for (const Hop& h : hops) {
    if (h.end < kSettle || h.end % cv::PitchTracker::kHop != 0 || !h.voiced) continue;
    if (std::fabs(h.corr) >= 0.2f) ++pulled;
  }
  return report("bleed", pulled <= 18,
                "A3 with pink noise 5 dB under, KEY E: %.0f voiced hops pulled 0.2 st or more (<= 18)", pulled);
}

// 14. A stage switched off ends as a plain copy of its input (engine/CLAUDE.md). Autotune runs on a held
// A3 + 50 cents for 1 s, so the corrected voice differs from the input, then goes off. From 300 ms
// after the off edge every output sample equals its input, with no tolerance.
bool testSwitchedOff() {
  const int offAt = cv::kSampleRate / cv::kBlock * cv::kBlock;
  const int total = offAt + cv::kSampleRate;
  const std::vector<float> in = tone(kSharp, total);
  const cv::PitchFxParams on = autotuneOnly(1.0f);
  cv::PitchFxParams off = on;
  off.autotune.on = false;
  std::vector<float> out(in.size());
  gFx.reset();
  for (int i = 0; i < total; i += cv::kBlock) {
    const int n = std::min(cv::kBlock, total - i);
    gInProcess = true;
    gFx.process(&in[static_cast<size_t>(i)], &out[static_cast<size_t>(i)], n, i < offAt ? on : off);
    gInProcess = false;
  }
  int differingOn = 0, differingOff = 0;
  for (int i = offAt - kSettle; i < offAt; ++i)
    differingOn += out[static_cast<size_t>(i)] != in[static_cast<size_t>(i)] ? 1 : 0;
  for (int i = offAt + kSettle; i < total; ++i)
    differingOff += out[static_cast<size_t>(i)] != in[static_cast<size_t>(i)] ? 1 : 0;
  return report("switched off is a plain copy", differingOn > 0 && differingOff == 0,
                "samples differing in the 300 ms while on=%.0f (> 0), from 300 ms after off=%.0f (0)", differingOn,
                differingOff);
}

}  // namespace

int main() {
  bool ok = true;
  ok &= testPassthrough();
  ok &= testDegrees();
  ok &= testCorrection();
  ok &= testScale();
  ok &= testResponse();
  ok &= testOctaveFollows();
  ok &= testOctaveFollowsVibrato();
  ok &= testHardTune();
  ok &= testToggle();
  ok &= testToggleLevel();
  ok &= testHalfway();
  ok &= testRangeEnds();
  ok &= testFirstNote();
  ok &= testBleed();
  ok &= testSwitchedOff();
  // Every process() call above ran under the allocation guard.
  ok &= report("no allocation", true, "process() ran under the new/delete guard", 0.0);
  std::printf("%s autotune\n", ok ? "PASS" : "FAIL");
  return ok ? 0 : 1;
}
