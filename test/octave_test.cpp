// Octave checks: shift up/down, fifth, settled off is a plain copy (from the start and after an off
// edge), the tracker hears only the dry input, no allocation, formants that move with the pitch, the
// wet path's DC blocker, noise level, and SEMITONES through 0 without a cut. No window, no audio
// device.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "alloc_guard.h"
#include "pitch_series.h"
#include "ringed_tracker.h"
#include "signal_util.h"

using testutil::peakCurvature;

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr int kSettle = cv::kSampleRate / 10;  // 100 ms, for the checks that wait out a start-up
constexpr int kSampleWindow = cv::kSampleRate / 2;  // the steady-state window of a step check

// Ten harmonics at 1/k amplitude, peak-normalised to 0.5.
std::vector<float> tone(double hz, int n) {
  std::vector<double> x(static_cast<size_t>(n), 0.0);
  double peak = 0.0;
  for (int i = 0; i < n; ++i) {
    double s = 0.0;
    for (int k = 1; k <= 10; ++k) s += std::sin(2.0 * kPi * hz * k * i / cv::kSampleRate) / k;
    x[static_cast<size_t>(i)] = s;
    peak = std::max(peak, std::fabs(s));
  }
  std::vector<float> out(x.size());
  for (size_t i = 0; i < x.size(); ++i) out[i] = static_cast<float>(0.5 * x[i] / peak);
  return out;
}

// The series' pitch() read after one block, stamped with the block's end sample.
struct Hop {
  int end;
  cv::PitchResult r;
};

std::vector<float> run(PitchSeries& fx, const std::vector<float>& in, const cv::PitchCardParams& p,
                       std::vector<Hop>* hops = nullptr) {
  std::vector<float> out(in.size());
  if (hops) hops->reserve(in.size() / cv::kBlock + 1);
  const int total = static_cast<int>(in.size());
  for (int i = 0; i < total; i += cv::kBlock) {
    const int n = std::min(cv::kBlock, total - i);
    gInProcess = true;
    fx.process(&in[static_cast<size_t>(i)], &out[static_cast<size_t>(i)], n, p);
    gInProcess = false;
    if (hops) hops->push_back({i + n, fx.pitch()});
  }
  return out;
}

double cents(double hz, double ref) { return 1200.0 * std::log2(hz / ref); }

bool report(const char* name, bool ok, const char* fmt, double a, double b = 0.0,
            double c = 0.0, double d = 0.0) {
  char detail[200];
  std::snprintf(detail, sizeof detail, fmt, a, b, c, d);
  std::printf("%s %s  %s\n", ok ? "PASS" : "FAIL", name, detail);
  return ok;
}

static PitchSeries gFx;

cv::PitchCardParams octaveOnly(int semis) {
  cv::PitchCardParams p;
  p.octave.on = true;
  p.octave.semitones = semis;
  p.octave.mix = 1.0f;
  return p;
}

// Median f0 of the output over seconds 1..2, measured by a fresh tracker.
double outputHz(double inHz, const cv::PitchCardParams& p) {
  gFx.reset();
  const std::vector<float> out = run(gFx, tone(inHz, 2 * cv::kSampleRate), p);
  RingedTracker t;
  std::vector<double> hz;
  const int total = static_cast<int>(out.size());
  for (int i = 0; i < total; i += cv::kBlock) {
    const int n = std::min(cv::kBlock, total - i);
    t.push(&out[static_cast<size_t>(i)], n);
    if (i + n > cv::kSampleRate && t.result().voiced) hz.push_back(t.result().hz);
  }
  if (hz.empty()) return 0.0;
  std::sort(hz.begin(), hz.end());
  return hz[hz.size() / 2];
}

// The granular shifter spreads each partial into window-rate sidebands. The tracker reads the pitch
// within 3 cents at the worst of the three intervals, so the gate sits at +-5 cents.
bool checkShift(const char* name, int semis, double expect) {
  const double got = outputHz(220.0, octaveOnly(semis));
  const double err = got > 0.0 ? cents(got, expect) : 9999.0;
  return report(name, std::fabs(err) <= 5.0, "out=%.2f Hz, expect %.2f, err=%.2f cents (+-5)",
                got, expect, err);
}

bool testOctave() {
  bool ok = true;
  ok &= checkShift("octave up", 12, 440.0);
  ok &= checkShift("octave down", -12, 110.0);
  return ok;
}

bool testFifth() { return checkShift("fifth", 7, 329.63); }

bool testBypass() {
  const std::vector<float> in = tone(220.0, cv::kSampleRate);
  float worst = 0.0f;
  for (float mix : {0.0f, 0.5f, 1.0f}) {
    cv::PitchCardParams p = octaveOnly(0);
    p.octave.mix = mix;
    gFx.reset();
    const std::vector<float> out = run(gFx, in, p);
    for (size_t i = 0; i < in.size(); ++i) worst = std::max(worst, std::fabs(out[i] - in[i]));
  }
  cv::PitchCardParams off = octaveOnly(12);
  off.octave.mix = 0.5f;
  off.octave.on = false;
  gFx.reset();
  const std::vector<float> offOut = run(gFx, in, off);
  for (size_t i = 0; i < in.size(); ++i) worst = std::max(worst, std::fabs(offOut[i] - in[i]));
  return report("bypass", worst == 0.0f, "semitones 0 at mix 0/0.5/1 and on=false max|out-in|=%.3g (0)", worst);
}

// Octave on for a second, then off by the toggle and by SEMITONES 0. Once the fades settle (277 ms
// on the box) the stage is a plain copy, bit for bit, whatever it did before.
bool testOffEdge() {
  constexpr int kEdge = cv::kSampleRate;
  constexpr int kExact = kEdge + cv::kSampleRate * 3 / 10;  // 300 ms after the edge
  const std::vector<float> in = tone(220.0, 2 * cv::kSampleRate);
  bool ok = true;
  for (int pass = 0; pass < 2; ++pass) {
    cv::PitchCardParams on = octaveOnly(7);
    on.octave.mix = 0.5f;
    cv::PitchCardParams off = on;
    if (pass == 0) {
      off.octave.on = false;
    } else {
      off.octave.semitones = 0;
    }
    gFx.reset();
    std::vector<float> out(in.size());
    const int total = static_cast<int>(in.size());
    for (int i = 0; i < total; i += cv::kBlock) {
      const int n = std::min(cv::kBlock, total - i);
      gFx.process(&in[static_cast<size_t>(i)], &out[static_cast<size_t>(i)], n, i < kEdge ? on : off);
    }
    size_t differs = 0;
    for (size_t i = kExact; i < in.size(); ++i) differs += out[i] != in[i] ? 1u : 0u;
    ok &= report(pass == 0 ? "off edge, toggle" : "off edge, semitones 0", differs == 0,
                 "%.0f samples differ from the input 300 ms after the edge (0)", static_cast<double>(differs));
  }
  return ok;
}

// Octave and Autotune on together; the tracker must still hear only the dry tone.
bool testWiring() {
  cv::PitchCardParams p = octaveOnly(12);
  p.autotune.on = true;
  p.autotune.responseMs = 20.0f;
  gFx.reset();
  std::vector<Hop> hops;
  run(gFx, tone(220.0, 2 * cv::kSampleRate), p, &hops);
  int bad = 0;
  int checked = 0;
  double worst = 0.0;
  for (const Hop& h : hops) {
    if (h.end <= kSettle) continue;
    ++checked;
    if (!h.r.voiced) {
      ++bad;
      continue;
    }
    const double err = std::fabs(cents(h.r.hz, 220.0));
    worst = std::max(worst, err);
    if (err > 5.0) ++bad;
  }
  return report("wiring", bad == 0 && checked > 0,
                "pitch() worst=%.3f cents (+-5), bad hops=%.0f of %.0f (0)", worst, bad, checked);
}

bool testNoAlloc() {
  cv::PitchCardParams p = octaveOnly(-12);
  p.octave.mix = 0.5f;
  p.autotune.on = true;
  const std::vector<float> in = tone(220.0, cv::kBlock * 64);
  gFx.reset();
  run(gFx, in, p);  // aborts on any new/delete inside process()
  return report("no-alloc", true, "%.0f blocks ran without an allocation in process() (it aborts on one)",
                static_cast<double>(in.size() / cv::kBlock));
}

// Goertzel magnitude of x at hz.
double goertzel(const float* x, int n, double hz) {
  const double w = 2.0 * kPi * hz / cv::kSampleRate;
  const double c = 2.0 * std::cos(w);
  double s1 = 0.0;
  double s2 = 0.0;
  for (int i = 0; i < n; ++i) {
    const double s0 = x[i] + c * s1 - s2;
    s2 = s1;
    s1 = s0;
  }
  return std::sqrt(s1 * s1 + s2 * s2 - c * s1 * s2);
}

// Root-sum-square Goertzel magnitude within +-halfHz of hz over second 1..2.
// A granular shifter spreads each harmonic into window-rate sidebands, so a
// single bin at the harmonic would read the comb ripple, not the envelope.
double bandAmp(const std::vector<float>& x, double hz, double halfHz) {
  double e = 0.0;
  const int steps = static_cast<int>(halfHz / 2.0);
  for (int d = -steps; d <= steps; ++d) {
    const double a = goertzel(&x[cv::kSampleRate], cv::kSampleRate, hz + 2.0 * d);
    e += a * a;
  }
  return std::sqrt(e);
}

// Amplitude-weighted mean frequency over the given harmonic numbers of f0.
double centroidHz(const std::vector<float>& x, double f0, int first, int last, int step) {
  double num = 0.0;
  double den = 0.0;
  for (int k = first; k <= last; k += step) {
    const double a = bandAmp(x, f0 * k, 0.45 * f0 * step);
    num += f0 * k * a;
    den += a;
  }
  return den > 0.0 ? num / den : 0.0;
}

// Median f0 of a signal over seconds 1..2, from a fresh tracker.
double medianHz(const std::vector<float>& x) {
  RingedTracker t;
  std::vector<double> hz;
  const int total = static_cast<int>(x.size());
  for (int i = 0; i < total; i += cv::kBlock) {
    const int n = std::min(cv::kBlock, total - i);
    t.push(&x[static_cast<size_t>(i)], n);
    if (i + n > cv::kSampleRate && t.result().voiced) hz.push_back(t.result().hz);
  }
  if (hz.empty()) return 0.0;
  std::sort(hz.begin(), hz.end());
  return hz[hz.size() / 2];
}

// The granular shifter moves the envelope with the pitch: the centroid in harmonics of its own
// f0 matches the input's.
bool testFormantMoves() {
  const std::vector<float> in = tone(220.0, 2 * cv::kSampleRate);
  const double inRel = centroidHz(in, 220.0, 1, 10, 1) / 220.0;
  gFx.reset();
  const std::vector<float> c = run(gFx, in, octaveOnly(-12));
  const double fC = medianHz(c);
  const double rC = centroidHz(c, fC, 1, 10, 1) / fC / inRel;
  return report("formants move", std::fabs(rC - 1.0) <= 0.10,
                "centroid/f0 x%.3f of input (+-10%%), f0=%.1f", rC, fC);
}

// A 0.5 DC step through the wet path at unity trim (MIX 1, so the dry path adds nothing). The card sits on
// in 0.1 s of silence first, so its fade is up when the step arrives. The voice's DC blocker takes the step
// away: from the output's peak the level falls under -60 dBFS within 200 ms and stays there. The 5 Hz
// one-pole falls from a peak of 0.477 to -60 dBFS in 196 ms, so the limit sits 2 % over the filter's own
// decay.
bool testDcBlocked() {
  constexpr double kLimit = 1e-3;  // -60 dBFS
  constexpr double kSettleMs = 200.0;
  cv::PitchCardParams p = octaveOnly(12);
  p.octave.tuning.trimDb = 0.0f;
  std::vector<float> in(static_cast<size_t>(cv::kSampleRate * 11 / 10), 0.5f);
  std::fill(in.begin(), in.begin() + cv::kSampleRate / 10, 0.0f);
  gFx.reset();
  const std::vector<float> out = run(gFx, in, p);

  size_t peakAt = 0;
  double peak = 0.0;
  for (size_t i = 0; i < out.size(); ++i) {
    if (std::fabs(out[i]) > peak) {
      peak = std::fabs(out[i]);
      peakAt = i;
    }
  }
  size_t lastOver = peakAt;
  for (size_t i = peakAt; i < out.size(); ++i) {
    if (std::fabs(out[i]) >= kLimit) lastOver = i;
  }
  const double settleMs = 1000.0 * static_cast<double>(lastOver + 1 - peakAt) / cv::kSampleRate;
  bool ok = report("dc blocked", peak > 0.45 && peak < 0.55, "wet peak %.3f on a 0.5 step (0.45..0.55)", peak);
  ok &= report("dc blocked", settleMs <= kSettleMs, "under -60 dBFS %.1f ms after the peak (<= 200)", settleMs);
  return ok;
}

bool testNoise() {
  const int n = 2 * cv::kSampleRate;
  std::vector<float> in(static_cast<size_t>(n));
  uint64_t s = 0x9E3779B97F4A7C15ULL;
  for (float& v : in) {
    s = s * 6364136223846793005ULL + 1442695040888963407ULL;
    v = 0.25f * (static_cast<float>(s >> 40) / 8388608.0f - 1.0f);
  }
  const cv::PitchCardParams p = octaveOnly(-12);
  gFx.reset();
  const std::vector<float> out = run(gFx, in, p);
  double ri = 0.0;
  double ro = 0.0;
  for (int i = cv::kSampleRate; i < n; ++i) {
    ri += static_cast<double>(in[static_cast<size_t>(i)]) * in[static_cast<size_t>(i)];
    ro += static_cast<double>(out[static_cast<size_t>(i)]) * out[static_cast<size_t>(i)];
  }
  const double gainDb = 10.0 * std::log10(ro / ri);
  // The trim is set on Adam's clips. White noise reads 1 dB under unity at that trim.
  constexpr double kExpectDb = -1.0;
  return report("noise level", std::fabs(gainDb - kExpectDb) <= 1.5, "out/in %+.2f dB (%+.1f +-1.5)", gainDb, kExpectDb);
}

// SEMITONES +1 to 0 to -1 with 0 held for gapBlocks: the stage switches off at 0, and its voice
// must keep running while the crossfade is up.
constexpr int kStepAt = cv::kSampleRate / cv::kBlock * cv::kBlock;
constexpr int kThroughTotal = 2 * cv::kSampleRate;

// The card sits on through silence until its ring holds the taps' reach, so its voice starts with the tone.
// The step's curvature depends on where the grain window sits against the tone, and this is the phase the
// limits were set on. The run returns the output from the tone's start.
std::vector<float> throughZeroRun(float mix, int gapBlocks, double hz = 220.0) {
  const int lead = cv::OctaveVoice::deepestRead(cv::OctaveTuning{});
  const std::vector<float> tune = tone(hz, kThroughTotal);
  std::vector<float> in(static_cast<size_t>(lead), 0.0f);
  in.insert(in.end(), tune.begin(), tune.end());
  std::vector<float> out(in.size());
  cv::PitchCardParams p = octaveOnly(1);
  p.octave.mix = mix;
  gFx.reset();
  gFx.process(in.data(), out.data(), lead, p);
  for (int i = 0; i < kThroughTotal; i += cv::kBlock) {
    const int block = (i - kStepAt) / cv::kBlock;
    p.octave.semitones = i < kStepAt ? 1 : (block < gapBlocks ? 0 : -1);
    gFx.process(&in[static_cast<size_t>(lead + i)], &out[static_cast<size_t>(lead + i)], cv::kBlock, p);
  }
  out.erase(out.begin(), out.begin() + lead);
  return out;
}

// No sample bends harder than in steady state.
double throughZeroRatio(float mix, int gapBlocks) {
  const std::vector<float> out = throughZeroRun(mix, gapBlocks);
  const double steady = std::max(peakCurvature(out, kStepAt - kSampleWindow, kStepAt),
                                 peakCurvature(out, kThroughTotal - kSampleWindow, kThroughTotal));
  return peakCurvature(out, kStepAt, kStepAt + gapBlocks * cv::kBlock + cv::kSampleRate / 10) / steady;
}

// RMS of x over [from, from + len).
double windowRms(const std::vector<float>& x, int from, int len) {
  double e = 0.0;
  for (int i = from; i < from + len; ++i) e += static_cast<double>(x[static_cast<size_t>(i)]) * x[static_cast<size_t>(i)];
  return std::sqrt(e / len);
}

// The lowest level from the step to 100 ms after the dwell, in windows of one period of a 100 Hz
// tone (480 samples, whose wet at +-1 st pulses by about 1 dB), in dB under the wet's RMS before the
// step. The two voices are unrelated signals, and an equal-power crossfade of those keeps the level;
// a linear one dips by up to 3 dB at its midpoint.
double throughZeroDipDb(float mix, int gapBlocks) {
  constexpr int kPeriod = 480;
  const std::vector<float> out = throughZeroRun(mix, gapBlocks, 100.0);
  const double steady = windowRms(out, kStepAt - kSampleWindow, kSampleWindow);
  double low = 1e9;
  const int end = kStepAt + gapBlocks * cv::kBlock + cv::kSampleRate / 10;
  for (int i = kStepAt; i < end; i += 32) low = std::min(low, windowRms(out, i, kPeriod));
  return -20.0 * std::log10(low / steady);
}

bool testThroughZero() {
  constexpr double kMaxRatio = 1.0;
  constexpr double kMaxDipDb = 2.0;
  bool ok = true;
  for (float mix : {1.0f, 0.10f}) {
    double worst = 0.0;
    double dip = -100.0;
    for (int gap : {1, 4, 8, 15}) {
      worst = std::max(worst, throughZeroRatio(mix, gap));
      dip = std::max(dip, throughZeroDipDb(mix, gap));
    }
    char name[48];
    std::snprintf(name, sizeof name, "through 0 at mix %.0f %%", static_cast<double>(mix * 100.0f));
    ok &= report(name, worst <= kMaxRatio,
                 "0 held 1.3 to 20 ms: peak curvature x%.3f of steady state (<= %.1f)", worst, kMaxRatio);
    std::snprintf(name, sizeof name, "through 0 level at mix %.0f %%", static_cast<double>(mix * 100.0f));
    ok &= report(name, dip <= kMaxDipDb, "0 held 1.3 to 20 ms: lowest level %.2f dB under steady state (<= %.1f)", dip,
                 kMaxDipDb);
  }
  return ok;
}

// The toggle crossfades the dry tone into the wet and back by the Octave fade alone. A switch off
// and on, quick or slow, bends no harder than steady state either way.
bool testToggle() {
  constexpr int kOffAt = cv::kSampleRate;
  const std::vector<float> in = tone(220.0, 3 * cv::kSampleRate);
  double worst = 0.0;
  for (float mix : {1.0f, 0.10f}) {
    for (int offBlocks : {2, 20, 400}) {
      cv::PitchCardParams p = octaveOnly(7);
      p.octave.mix = mix;
      gFx.reset();
      std::vector<float> out(in.size());
      const int total = static_cast<int>(in.size());
      const int onAt = kOffAt + offBlocks * cv::kBlock;
      for (int i = 0; i < total; i += cv::kBlock) {
        p.octave.on = i < kOffAt || i >= onAt;
        gFx.process(&in[static_cast<size_t>(i)], &out[static_cast<size_t>(i)], cv::kBlock, p);
      }
      const double steady = std::max(peakCurvature(out, kOffAt - kSampleWindow, kOffAt),
                                     peakCurvature(out, total - kSampleWindow, total));
      const int end = onAt + cv::kSampleRate / 2;
      worst = std::max(worst, peakCurvature(out, kOffAt, end) / steady);
    }
  }
  return report("toggle off and on", worst <= 1.05,
                "Octave off for 2, 20 and 400 blocks then on: peak curvature x%.3f of steady state (<= 1.05)", worst);
}

// A voice whose MIX sits at 0 is not heard, so it sits out. MIX leaving 0, by a touch or a jump, picks
// it up in step: no click on a steady tone, measured as peak curvature in the 100 ms after the step
// against the larger of the settled peaks before and after, as knob_step_test does.
bool testMixLeavesZero() {
  constexpr int kStepAt = 2 * cv::kSampleRate;
  constexpr int kTotal = 2 * kStepAt;
  const std::vector<float> in = tone(220.0, kTotal);  // a longer run than the through-zero checks
  double worst = 0.0;
  for (int semis : {12, -5}) {
    for (float mix : {0.015f, 0.5f, 1.0f}) {
      cv::PitchCardParams p = octaveOnly(semis);
      p.octave.mix = 0.0f;
      gFx.reset();
      std::vector<float> out(in.size());
      for (int i = 0; i < kTotal; i += cv::kBlock) {
        if (i == kStepAt) p.octave.mix = mix;
        gFx.process(&in[static_cast<size_t>(i)], &out[static_cast<size_t>(i)], cv::kBlock, p);
      }
      const double steady = std::max(peakCurvature(out, kStepAt - kSampleWindow, kStepAt),
                                     peakCurvature(out, kTotal - kSampleWindow, kTotal));
      worst = std::max(worst, peakCurvature(out, kStepAt, kStepAt + cv::kSampleRate / 10) / steady);
    }
  }
  return report("mix leaves 0", worst <= 1.05, "peak curvature after MIX 0 to 1.5, 50, 100 %%: x%.4f of steady state (<= 1.05)", worst);
}

// process() takes any n. Calls of 1 to 1000 samples stay finite and within -80 dB of the 64-sample run
// (the voice's glide and window sync step per call, so the runs differ by rounding and a hair of timing).
bool testAnyLength() {
  const std::vector<float> in = tone(220.0, cv::kSampleRate);
  cv::PitchCardParams p = octaveOnly(7);
  p.octave.mix = 0.5f;
  gFx.reset();
  const std::vector<float> ref = run(gFx, in, p);
  double refPower = 0.0;
  for (float v : ref) refPower += static_cast<double>(v) * v;
  bool finite = true;
  double worstDb = -200.0;
  for (int size : {1, 7, 100, 1000}) {
    gFx.reset();
    std::vector<float> out(in.size());
    const int total = static_cast<int>(in.size());
    for (int i = 0; i < total; i += size) {
      gFx.process(&in[static_cast<size_t>(i)], &out[static_cast<size_t>(i)], std::min(size, total - i), p);
    }
    double diff = 0.0;
    for (size_t i = 0; i < out.size(); ++i) {
      finite = finite && std::isfinite(out[i]);
      diff += (static_cast<double>(out[i]) - ref[i]) * (static_cast<double>(out[i]) - ref[i]);
    }
    worstDb = std::max(worstDb, 10.0 * std::log10(diff / refPower + 1e-20));
  }
  return report("any call length", finite && worstDb <= -80.0,
                "calls of 1, 7, 100 and 1000 samples: finite %.0f (1), worst %.1f dB from the 64-sample run (<= -80)",
                finite ? 1.0 : 0.0, worstDb);
}

// One NaN in the input latches the voice. The chain's guard cuts that block and resets every engine, so
// the pitch stage's reset has to bring the voice back: finite and sounding again on the next call.
bool testNanRecovery() {
  const std::vector<float> in = tone(220.0, cv::kSampleRate);
  cv::PitchCardParams p = octaveOnly(12);
  p.octave.mix = 0.5f;
  gFx.reset();
  std::vector<float> out(in.size());
  const int poisonAt = cv::kSampleRate / 2;
  bool latched = false;
  for (int i = 0; i < cv::kSampleRate; i += cv::kBlock) {
    std::vector<float> block(in.begin() + i, in.begin() + i + cv::kBlock);
    if (i == poisonAt / cv::kBlock * cv::kBlock) block[5] = NAN;
    gFx.process(block.data(), &out[static_cast<size_t>(i)], cv::kBlock, p);
    if (i > poisonAt) latched = latched || !std::isfinite(out[static_cast<size_t>(i)]);
  }
  gFx.reset();
  const std::vector<float> after = run(gFx, in, p);
  bool finite = true;
  double peak = 0.0;
  for (size_t i = static_cast<size_t>(cv::kSampleRate) / 2; i < after.size(); ++i) {
    finite = finite && std::isfinite(after[i]);
    peak = std::max(peak, static_cast<double>(std::fabs(after[i])));
  }
  return report("nan recovery", latched && finite && peak > 0.1,
                "a NaN latches the voice (%.0f), and after reset() the output is finite (%.0f) and sounds (peak %.2f)",
                latched ? 1.0 : 0.0, finite ? 1.0 : 0.0, peak);
}

// When the input goes to digital silence, the DC blocker's tail falls to exact 0 without passing
// through subnormal floats, which cost cycles on some cores. The tail would take about 2.5 s to
// reach the subnormal range, so the silence runs 4 s.
bool testSilenceRest() {
  std::vector<float> in = tone(220.0, cv::kSampleRate);
  in.resize(5 * static_cast<size_t>(cv::kSampleRate), 0.0f);
  gFx.reset();
  const std::vector<float> out = run(gFx, in, octaveOnly(12));
  size_t subnormal = 0;
  for (const float x : out) subnormal += std::fpclassify(x) == FP_SUBNORMAL ? 1u : 0u;
  size_t live = 0;
  for (size_t i = 4 * static_cast<size_t>(cv::kSampleRate); i < out.size(); ++i) live += out[i] != 0.0f ? 1u : 0u;
  return report("silence rest", subnormal == 0 && live == 0, "%.0f subnormal samples (0); %.0f of the last second's samples are not 0 (0)",
                static_cast<double>(subnormal), static_cast<double>(live));
}

}  // namespace

int main() {
  bool ok = true;
  ok &= testOctave();
  ok &= testFifth();
  ok &= testBypass();
  ok &= testOffEdge();
  ok &= testWiring();
  ok &= testNoAlloc();
  ok &= testFormantMoves();
  ok &= testNoise();
  ok &= testDcBlocked();
  ok &= testThroughZero();
  ok &= testToggle();
  ok &= testMixLeavesZero();
  ok &= testAnyLength();
  ok &= testNanRecovery();
  ok &= testSilenceRest();
  return ok ? 0 : 1;
}
