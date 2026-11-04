// Unison checks: the copy at DEPTH 0 and after a switch-off, detune, the top of the knob, level, the dry
// at unity, no-alloc, the LFO against its analytic sine, the wet held back after a reset, the switch-on
// curvature, the pitch of a copy while a knob moves, the glide constants and NaN input. No window, no
// audio device.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <vector>

#include "core/unison_rate.h"
#include "engine/unison.h"

#include "alloc_guard.h"

namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kFreq = 440.0f;
constexpr float kAmp = 0.5f;

std::vector<float> sine(int n, float hz = kFreq, float amp = kAmp) {
  std::vector<float> x(static_cast<size_t>(n));
  for (int i = 0; i < n; ++i) x[static_cast<size_t>(i)] = amp * sinf(2.0f * kPi * hz * i / cv::kSampleRate);
  return x;
}

// Runs the signal through process() in kBlock chunks, with `a` before sample `at` and `b` from there on.
std::vector<float> runSwitch(cv::Unison& u, const std::vector<float>& in, const cv::UnisonParams& a,
                             const cv::UnisonParams& b, int at) {
  std::vector<float> out(in.size());
  const int total = static_cast<int>(in.size());
  for (int i = 0; i < total; i += cv::kBlock) {
    const int n = total - i < cv::kBlock ? total - i : cv::kBlock;
    gInProcess = true;
    u.process(&in[static_cast<size_t>(i)], &out[static_cast<size_t>(i)], n, i < at ? a : b);
    gInProcess = false;
  }
  return out;
}

std::vector<float> run(cv::Unison& u, const std::vector<float>& in, const cv::UnisonParams& p) {
  return runSwitch(u, in, p, p, 0);
}

cv::UnisonParams params(float depth) {
  cv::UnisonParams p;
  p.on = true;
  p.depth = depth;
  return p;
}

// Pitch checks subtract the dry signal, so they run without the level trim.
cv::UnisonParams untrimmed(float depth) {
  cv::UnisonParams p = params(depth);
  p.tuning.trimDb = 0.0f;
  return p;
}

// The tuning with both voices set to voice 1, so the wet holds twice one copy.
cv::UnisonParams oneCopy(cv::UnisonParams p) {
  p.tuning.baseDelayMs[0] = p.tuning.baseDelayMs[1];
  p.tuning.lfoHz[0] = p.tuning.lfoHz[1];
  return p;
}

// Resets the engine and puts both LFOs in phase, which a one-copy tuning needs.
void resetInPhase(cv::Unison& u) {
  u.reset();
  u.setLfoPhase(0, u.lfoPhase(1));
}

float rms(const std::vector<float>& x) {
  double s = 0.0;
  for (float v : x) s += static_cast<double>(v) * v;
  return static_cast<float>(std::sqrt(s / static_cast<double>(x.size())));
}

bool report(const char* name, bool ok, const char* fmt, double a, double b = 0.0, double c = 0.0) {
  char detail[200];
  std::snprintf(detail, sizeof detail, fmt, a, b, c);
  std::printf("%s %s  %s\n", ok ? "PASS" : "FAIL", name, detail);
  return ok;
}

bool allEqual(const std::vector<float>& x, const std::vector<float>& y, size_t from, size_t to) {
  for (size_t i = from; i < to; ++i)
    if (x[i] != y[i]) return false;
  return true;
}

cv::Unison gUnison;

// What the sine at DEPTH 1 reads through the engine, as measured; testLevel holds the engine to it.
constexpr float kLevelSineDb = 3.02f;

// A Unison at DEPTH 0 or off, and one that sits silent, copy their input sample for sample from the start.
bool testPassthrough() {
  const int n = cv::kSampleRate;
  const std::vector<float> in = sine(n);
  gUnison.reset();
  const bool atZero = allEqual(run(gUnison, in, params(0.0f)), in, 0, in.size());
  gUnison.reset();
  cv::UnisonParams off = params(1.0f);
  off.on = false;
  const bool whenOff = allEqual(run(gUnison, in, off), in, 0, in.size());

  const std::vector<float> silence(static_cast<size_t>(n), 0.0f);
  gUnison.reset();
  float maxSilent = 0.0f;
  for (float v : run(gUnison, silence, params(1.0f))) maxSilent = std::fmax(maxSilent, std::fabs(v));

  return report("passthrough", atZero && whenOff && maxSilent == 0.0f,
                "DEPTH 0 == %.0f, off == %.0f, silence max=%.3g (==0)", atZero ? 1.0 : 0.0, whenOff ? 1.0 : 0.0,
                maxSilent);
}

// An engaged card switched off, or taken to DEPTH 0, settles to a plain copy: from 0.4 s on the output
// equals the input sample for sample.
bool testSettleToCopy() {
  const int n = 2 * cv::kSampleRate;
  const int at = cv::kSampleRate;
  const size_t from = static_cast<size_t>(at + 2 * cv::kSampleRate / 5);
  const std::vector<float> in = sine(n);
  cv::UnisonParams off = params(1.0f);
  off.on = false;
  gUnison.reset();
  const std::vector<float> outOff = runSwitch(gUnison, in, params(1.0f), off, at);
  const bool offEqual = allEqual(outOff, in, from, in.size());
  gUnison.reset();
  const std::vector<float> outZero = runSwitch(gUnison, in, params(1.0f), params(0.0f), at);
  const bool zeroEqual = allEqual(outZero, in, from, in.size());
  const bool wasWet = !allEqual(outOff, in, static_cast<size_t>(at / 2), static_cast<size_t>(at));
  return report("settle to copy", wasWet && offEqual && zeroEqual,
                "wet before the step %.0f, == after 0.4 s: off %.0f, DEPTH 0 %.0f", wasWet ? 1.0 : 0.0,
                offEqual ? 1.0 : 0.0, zeroEqual ? 1.0 : 0.0);
}

// Peak |cents| from upward zero-crossing periods over samples [from, to), linearly interpolated.
float peakCents(const std::vector<float>& x, int from, int to) {
  float peak = 0.0f;
  float prevCross = -1.0f;
  for (size_t i = static_cast<size_t>(from); i + 1 < static_cast<size_t>(to); ++i) {
    if (!(x[i] < 0.0f && x[i + 1] >= 0.0f)) continue;
    const float cross = static_cast<float>(i) + (-x[i]) / (x[i + 1] - x[i]);
    if (prevCross >= 0.0f) {
      const float hz = cv::kSampleRate / (cross - prevCross);
      const float cents = std::fabs(1200.0f * log2f(hz / kFreq));
      if (cents > peak) peak = cents;
    }
    prevCross = cross;
  }
  return peak;
}

// Largest pitch deviation of a copy at rest, in cents: its delay falls at 2 pi f swing seconds a second
// at the fastest, and a falling delay is the larger deviation.
float steadyCents(const cv::UnisonParams& p) {
  const cv::UnisonTuning& t = p.tuning;
  const float over = p.depth > t.topFrom ? (p.depth - t.topFrom) / (1.0f - t.topFrom) : 0.0f;
  const float swingSec =
      (t.swingMinMs + (t.swingMaxMs - t.swingMinMs) * p.depth) * (1.0f + (t.topSwingGain - 1.0f) * over) * 0.001f;
  return -1200.0f * log2f(1.0f - 2.0f * kPi * t.lfoHz[1] * swingSec);
}

bool testDetune() {
  const int n = 4 * cv::kSampleRate;
  const std::vector<float> in = sine(n);
  const cv::UnisonParams p = oneCopy(untrimmed(1.0f));
  resetInPhase(gUnison);
  const std::vector<float> out = run(gUnison, in, p);
  std::vector<float> wet(in.size());
  for (size_t i = 0; i < in.size(); ++i) wet[i] = out[i] - in[i];
  const float cents = peakCents(wet, cv::kSampleRate / 2, n);
  const float expect = steadyCents(p);
  const float tol = 0.05f * expect;
  return report("detune", std::fabs(cents - expect) <= tol, "peak=%.2f cents (expect %.1f +-%.1f)", cents, expect, tol);
}

// The tuning without the top of the knob.
cv::UnisonParams flatTop(cv::UnisonParams p) {
  p.tuning.topSwingGain = 1.0f;
  p.tuning.topWetDb = 0.0f;
  return p;
}

// Fraction of the top reach in use at DEPTH d.
double overAt(const cv::UnisonTuning& t, double d) {
  return d > t.topFrom ? (d - t.topFrom) / (1.0 - t.topFrom) : 0.0;
}

// DEPTH 0.3 to 0.6: both top terms are exact neutrals, so once the hold after the reset and the glides
// are over (2 s) the output equals the tuning without them, sample for sample, at the shipped swing, at
// the RATE 50 % swing, and after a depth step that ends at 0.6. The hold itself reaches as far as the
// tuning's widest swing, which the top lengthens.
bool testBelowTop() {
  const std::vector<float> in = sine(3 * cv::kSampleRate);
  const size_t settled = static_cast<size_t>(2 * cv::kSampleRate);
  const float depths[3] = {0.3f, 0.5f, 0.6f};
  bool same = true;
  for (float depth : depths) {
    for (int rate = 0; rate < 2; ++rate) {
      cv::UnisonParams p = params(depth);
      if (rate) unisonrate::apply(p.tuning, 50.0f);
      cv::Unison a, b;
      const std::vector<float> top = run(a, in, p);
      const std::vector<float> flat = run(b, in, flatTop(p));
      same = same && allEqual(top, flat, settled, in.size()) && a.swingSamples() == b.swingSamples();
    }
  }
  const std::vector<float> longIn = sine(6 * cv::kSampleRate);
  cv::Unison a, b;
  const std::vector<float> top = runSwitch(a, longIn, params(0.2f), params(0.6f), 3 * cv::kSampleRate);
  const std::vector<float> flat = runSwitch(b, longIn, flatTop(params(0.2f)), flatTop(params(0.6f)), 3 * cv::kSampleRate);
  same = same && allEqual(top, flat, static_cast<size_t>(5 * cv::kSampleRate), longIn.size());
  std::printf("%s below-top  DEPTH 0.3, 0.5, 0.6 and a 0.2 to 0.6 step equal the tuning without the top (==): %s\n",
              same ? "PASS" : "FAIL", same ? "yes" : "NO");
  return same;
}

// Swing at the end of a run long enough for the glide to settle, in ms.
double settledSwingMs(const cv::UnisonParams& p) {
  const std::vector<float> in = sine(2 * cv::kSampleRate);
  cv::Unison u;
  run(u, in, p);
  return static_cast<double>(u.swingSamples()) * 1000.0 / cv::kSampleRate;
}

// The swing grows from 1 to topSwingGain over DEPTH topFrom to 1, linearly.
bool testTopSwing() {
  const cv::UnisonTuning t;
  const float depths[5] = {0.3f, 0.5f, 0.6f, 0.8f, 1.0f};
  bool ok = true;
  for (float depth : depths) {
    const double was = settledSwingMs(flatTop(params(depth)));
    const double now = settledSwingMs(params(depth));
    const double want = 1.0 + (static_cast<double>(t.topSwingGain) - 1.0) * overAt(t, depth);
    const bool row = std::fabs(now / was - want) < 1e-3;
    ok &= row;
    char name[40];
    std::snprintf(name, sizeof name, "top swing %.1f", static_cast<double>(depth));
    char detail[96];
    std::snprintf(detail, sizeof detail, "was %.4f ms, now %.4f ms, x%.4f (want x%.4f +-1e-3)", was, now,
                  now / was, want);
    std::printf("%s %s  %s\n", row ? "PASS" : "FAIL", name, detail);
  }
  return ok;
}

// RMS of the wet path alone (dry subtracted, no trim) once the glides have settled.
double wetRms(const cv::UnisonParams& p) {
  const int n = 3 * cv::kSampleRate;
  const std::vector<float> in = sine(n);
  cv::Unison u;
  const std::vector<float> out = run(u, in, p);
  std::vector<float> wet;
  for (int i = cv::kSampleRate; i < n; ++i) wet.push_back(out[static_cast<size_t>(i)] - in[static_cast<size_t>(i)]);
  return static_cast<double>(rms(wet));
}

// The per-voice wet level rises by topWetDb x over, in dB. Both sides keep the same swing, so the
// taps match and the ratio of the wet RMS is the gain alone. The last figure is the shipped
// tuning against the tuning without the top, where the wider swing moves the two voices against
// each other a little as well.
bool testTopWet() {
  const cv::UnisonTuning t;
  const float depths[5] = {0.3f, 0.5f, 0.6f, 0.8f, 1.0f};
  bool ok = true;
  for (float depth : depths) {
    cv::UnisonParams a = untrimmed(depth);
    a.tuning.topSwingGain = 1.0f;
    cv::UnisonParams b = a;
    b.tuning.topWetDb = 0.0f;
    const double ratio = wetRms(a) / wetRms(b);
    const double want = std::pow(10.0, static_cast<double>(t.topWetDb) * overAt(t, depth) / 20.0);
    const bool row = std::fabs(ratio / want - 1.0) < 1e-3;
    ok &= row;
    const double was = wetRms(flatTop(untrimmed(depth)));
    const double now = wetRms(untrimmed(depth));
    char name[40];
    std::snprintf(name, sizeof name, "top wet %.1f", static_cast<double>(depth));
    char detail[160];
    std::snprintf(detail, sizeof detail,
                  "wet gain +%.4f dB (want +%.4f dB, +-1e-3 of the ratio); shipped wet RMS %.2f dBFS to %.2f dBFS",
                  20.0 * std::log10(ratio), 20.0 * std::log10(want), 20.0 * std::log10(was),
                  20.0 * std::log10(now));
    std::printf("%s %s  %s\n", row ? "PASS" : "FAIL", name, detail);
  }
  return ok;
}

// Largest |second difference| of x over [from, to): a jump of the taps shows as a spike here.
double peakCurvature(const std::vector<float>& x, int from, int to) {
  double worst = 0.0;
  for (int i = std::max(from, 2); i < to; ++i) {
    const size_t k = static_cast<size_t>(i);
    worst = std::max(worst, std::fabs(static_cast<double>(x[k]) - 2.0 * x[k - 1] + x[k - 2]));
  }
  return worst;
}

bool testLevel() {
  const int n = 4 * cv::kSampleRate;
  const std::vector<float> in = sine(n);
  gUnison.reset();
  const std::vector<float> out = run(gUnison, in, params(1.0f));
  const float db = 20.0f * log10f(rms(out) / rms(in));
  // The sine is deterministic and the top of the knob is in the reading. The level row in level_test holds
  // the default to the rule on Adam's clips.
  return report("level", std::fabs(db - kLevelSineDb) <= 0.05f, "out/in=%.2f dB (%.2f +-0.05)", db, kLevelSineDb);
}

// The dry voice passes at unity at every DEPTH, trim included. A lone impulse after a silent second
// reads the dry gain before any wet copy returns, since the nearest tap sits well behind it.
bool testDry() {
  std::array<double, 3> db{};
  bool ok = true;
  const float depths[3] = {0.25f, 0.5f, 1.0f};
  const int at = cv::kSampleRate;
  for (size_t k = 0; k < 3; ++k) {
    std::vector<float> in(static_cast<size_t>(cv::kSampleRate + cv::kSampleRate / 2), 0.0f);
    in[static_cast<size_t>(at)] = 1.0f;
    cv::Unison u;
    u.reset();
    const std::vector<float> out = run(u, in, params(depths[k]));
    const float dry = out[static_cast<size_t>(at)];
    db[k] = 20.0 * std::log10(static_cast<double>(dry));
    ok = ok && dry == 1.0f;
  }
  return report("dry", ok, "dry gain at DEPTH 25/50/100 %%: %.2f, %.2f, %.2f dB (0.00)", db[0], db[1], db[2]);
}

bool testNoAlloc() {
  const std::vector<float> in = sine(cv::kBlock * 8);
  gUnison.reset();
  run(gUnison, in, params(0.7f));  // aborts on any new/delete inside process()
  return report("no-alloc", true, "%.0f allocations in process()", 0.0);
}

// The integer-phase LFO delay tracks the analytic sine at the quantised frequency over 10 s, at three
// Rate settings. Voice 1 starts half a turn on.
bool testLfoAnalytic() {
  const float rates[3] = {0.06f, 0.2f, 0.9f};
  const int blocks = 10 * cv::kSampleRate / cv::kBlock;
  const std::vector<float> silence(static_cast<size_t>(cv::kBlock), 0.0f);
  std::vector<float> out(static_cast<size_t>(cv::kBlock));
  double worstDelay = 0.0;
  double worstHz = 0.0;
  for (float rate : rates) {
    cv::UnisonParams p = params(0.8f);
    p.tuning.lfoHz[0] = rate;
    p.tuning.lfoHz[1] = 1.5f * rate;
    uint32_t inc[2];
    double fq[2];
    for (int v = 0; v < 2; ++v) {
      inc[v] = static_cast<uint32_t>(p.tuning.lfoHz[v] / cv::kSampleRate * 4294967296.0f + 0.5f);
      fq[v] = static_cast<double>(inc[v]) / 4294967296.0 * cv::kSampleRate;
      worstHz = std::fmax(worstHz, std::fabs(fq[v] - static_cast<double>(p.tuning.lfoHz[v])));
    }
    gUnison.reset();
    for (int b = 1; b <= blocks; ++b) {
      gUnison.process(silence.data(), out.data(), cv::kBlock, p);
      const double k = static_cast<double>(b) * cv::kBlock;
      for (int v = 0; v < 2; ++v) {
        const double sw = static_cast<double>(gUnison.swingSamples());
        const double got = sw * static_cast<double>(cv::sinTurns(gUnison.lfoPhase(v)));
        const double start = v == 1 ? 3.14159265358979323846 : 0.0;
        const double want = sw * std::sin(start + 2.0 * 3.14159265358979323846 * fq[v] * k / cv::kSampleRate);
        worstDelay = std::fmax(worstDelay, std::fabs(got - want));
      }
    }
  }
  return report("lfo analytic", worstDelay < 0.05 && worstHz < 1e-5,
                "worst delay error=%.3g samples (<0.05), worst freq error=%.3g Hz (<1e-5)", worstDelay, worstHz);
}

// The second LFO starts half a turn after the first, so the copies detune in opposite directions at once.
bool testPhaseOffset() {
  gUnison.reset();
  const uint32_t gap = gUnison.lfoPhase(1) - gUnison.lfoPhase(0);
  return report("lfo start", gap == 0x80000000u, "second LFO starts %.0f of 2^32 turns on (2^31)",
                static_cast<double>(gap));
}

// After a reset the line holds nothing, so the wet stays at 0 until the line is written past the farthest
// tap, and the input passes as a copy until then. The wet then fades in from live audio.
bool testHold() {
  const std::vector<float> in = sine(cv::kSampleRate);
  const cv::UnisonParams p = params(1.0f);
  gUnison.reset();
  const std::vector<float> out = run(gUnison, in, p);
  const cv::UnisonTuning& t = p.tuning;
  const float widest = std::max(t.swingMinMs, t.swingMaxMs * t.topSwingGain);
  const float reachMs = std::max(t.baseDelayMs[0], t.baseDelayMs[1]) + widest;
  const size_t reach = static_cast<size_t>((reachMs * cv::kSmpPerMs) + 3.0f);
  bool wet = false;
  for (size_t i = reach + 4 * cv::kBlock; i < in.size() && !wet; ++i) wet = out[i] != in[i];
  const bool copy = allEqual(out, in, 0, reach);
  return report("hold", copy && wet, "farthest tap %.0f samples out: copy before it %.0f, wet after it %.0f",
                static_cast<double>(reach), copy ? 1.0 : 0.0, wet ? 1.0 : 0.0);
}

// The curvature of the output in 5 ms windows over 10 to 100 ms after Unison switches on, against the
// largest window of the same card running steadily on. A tap that reads the edge of an empty line steps
// the output and reads several times over.
double switchOnRatio(float hz, float depth) {
  const int n = 3 * cv::kSampleRate;
  const std::vector<float> in = sine(n, hz, 0.25f);
  cv::UnisonParams p = params(depth);
  unisonrate::apply(p.tuning, 50.0f);
  cv::Unison steady;
  steady.reset();
  const std::vector<float> on = run(steady, in, p);
  double reference = 0.0;
  for (int w = cv::kSampleRate; w + 240 <= n; w += 240) reference = std::max(reference, peakCurvature(on, w, w + 240));
  cv::Unison fresh;
  fresh.reset();
  const std::vector<float> start = run(fresh, in, p);
  double worst = 0.0;
  for (int w = cv::kSampleRate / 100; w < cv::kSampleRate / 10; w += 240) {
    worst = std::max(worst, peakCurvature(start, w, w + 240));
  }
  return worst / reference;
}

bool testSwitchOn() {
  bool ok = true;
  double worst = 0.0;
  for (float hz : {220.0f, 110.0f}) {
    for (float depth : {0.5f, 1.0f}) {
      const double r = switchOnRatio(hz, depth);
      worst = std::max(worst, r);
      ok &= r <= 1.1;
    }
  }
  return report("switch-on", ok, "curvature in the first 100 ms: x%.3f of steady state, 110 and 220 Hz (<= 1.1)", worst);
}

// Peak cents of a copy over 600 ms after a knob moves at sample `at`.
float movePeakCents(const cv::UnisonParams& from, const cv::UnisonParams& to, int at) {
  const int total = at + 3 * cv::kSampleRate / 5;
  const std::vector<float> in = sine(total);
  resetInPhase(gUnison);
  const std::vector<float> out = runSwitch(gUnison, in, oneCopy(from), oneCopy(to), at);
  std::vector<float> wet(in.size());
  for (size_t i = 0; i < in.size(); ++i) wet[i] = out[i] - in[i];
  return peakCents(wet, at, total);
}

// The worst pitch of a copy while a knob moves, over eight moments of the slow LFO's cycle, against the
// larger deviation the two settings reach at rest. A smooth glide keeps it near 1; a swing that runs
// ahead of the LFO speed bends the copy by several times that.
double moveRatio(const cv::UnisonParams& from, const cv::UnisonParams& to) {
  constexpr int kMoments = 8;
  constexpr int kSpread = 3 * cv::kSampleRate / 7 / cv::kBlock * cv::kBlock;
  float worst = 0.0f;
  for (int k = 0; k < kMoments; ++k) worst = std::max(worst, movePeakCents(from, to, 2 * cv::kSampleRate + k * kSpread));
  return static_cast<double>(worst / std::max(steadyCents(oneCopy(from)), steadyCents(oneCopy(to))));
}

cv::UnisonParams atRate(float ratePercent, float depth) {
  cv::UnisonParams p = untrimmed(depth);
  unisonrate::apply(p.tuning, ratePercent);
  return p;
}

bool moveRow(const char* name, double ratio, double limit) {
  return report(name, ratio <= limit, "peak pitch %.2f of the larger rest deviation (<= %.2f)", ratio, limit);
}

// A RATE jump moves the swing and both LFO speeds, and above 68 % the swing narrows as the speed rises.
// Each glides on 150 ms, so the copy bends no more than a fraction over its rest deviation.
bool testRateMove() {
  bool ok = true;
  ok &= moveRow("move RATE 0 to 100 %", moveRatio(atRate(0.0f, 1.0f), atRate(100.0f, 1.0f)), 1.7);
  ok &= moveRow("move RATE 68 to 100 %", moveRatio(atRate(68.0f, 1.0f), atRate(100.0f, 1.0f)), 1.7);
  ok &= moveRow("move RATE 50 to 100 %", moveRatio(atRate(50.0f, 1.0f), atRate(100.0f, 1.0f)), 1.7);
  ok &= moveRow("move RATE 100 to 0 %", moveRatio(atRate(100.0f, 1.0f), atRate(0.0f, 1.0f)), 1.7);
  return ok;
}

// A DEPTH jump moves the swing alone, on the same glide.
bool testDepthMove() {
  bool ok = true;
  ok &= moveRow("move DEPTH 10 to 100 %", moveRatio(atRate(50.0f, 0.1f), atRate(50.0f, 1.0f)), 1.25);
  ok &= moveRow("move DEPTH 50 to 100 %", moveRatio(atRate(50.0f, 0.5f), atRate(50.0f, 1.0f)), 1.25);
  ok &= moveRow("move DEPTH 100 to 50 %", moveRatio(atRate(50.0f, 1.0f), atRate(50.0f, 0.5f)), 1.25);
  return ok;
}

// The literal keep factor of the glide sits on libm's value to a relative 1e-6, the way the 20 ms literals do.
bool testGlideConstants() {
  using U = cv::Unison;
  const double kRel =
      std::fabs(static_cast<double>(U::kGlideKeepBlock) / cv::smooth::keep(U::kGlideSec, cv::kBlock) - 1.0);
  return report("glide constant", kRel < 1e-6, "keep over a block off by %.2g (<1e-6)", kRel);
}

// A glide that has run its course lands within 1e-3 sample of its target. A swing that stalled 0.1 sample
// short would end on a step of that size.
bool testGlideLands() {
  const std::vector<float> in = sine(4 * cv::kSampleRate);
  cv::UnisonParams p = params(1.0f);
  unisonrate::apply(p.tuning, 100.0f);
  gUnison.reset();
  run(gUnison, in, p);
  const float want = p.tuning.swingMaxMs * p.tuning.topSwingGain * cv::kSmpPerMs;
  const float off = std::fabs(gUnison.swingSamples() - want);
  return report("glide lands", off <= 1e-3f, "swing %.6f samples, off by %.2g (<= 1e-3)",
                static_cast<double>(gUnison.swingSamples()), static_cast<double>(off));
}

// A bad block (NaN DEPTH, swing, LFO speed) leaves nothing behind: the output stays finite through it and
// the level returns to a clean run's within 0.1 dB once the glides settle.
bool testNan() {
  const int n = 4 * cv::kSampleRate;
  const std::vector<float> in = sine(n);
  const float nan = std::nanf("");
  cv::UnisonParams bad = params(nan);
  bad.tuning.swingMaxMs = nan;
  bad.tuning.lfoHz[0] = nan;
  bad.tuning.lfoHz[1] = nan;
  cv::Unison u;
  u.reset();
  std::vector<float> out(in.size());
  bool finite = true;
  for (int i = 0; i < n; i += cv::kBlock) {
    const int block = i / cv::kBlock;
    const cv::UnisonParams& p = block == 250 || block == 251 ? bad : params(0.8f);
    u.process(&in[static_cast<size_t>(i)], &out[static_cast<size_t>(i)], cv::kBlock, p);
  }
  for (float v : out) finite = finite && std::isfinite(v);
  cv::Unison clean;
  clean.reset();
  const std::vector<float> ref = run(clean, in, params(0.8f));
  const std::vector<float> tail(out.begin() + 3 * cv::kSampleRate, out.end());
  const std::vector<float> refTail(ref.begin() + 3 * cv::kSampleRate, ref.end());
  const double db = 20.0 * std::log10(static_cast<double>(rms(tail) / rms(refTail)));
  return report("nan", finite && std::fabs(db) < 0.1, "finite %.0f, level after the bad blocks %+.3f dB of a clean run (+-0.1)",
                finite ? 1.0 : 0.0, db);
}

// The farthest tap at the widest swing the RATE law gives stays inside the line.
bool testTapsFit() {
  bool fits = cv::Unison::tapsFit(cv::UnisonTuning{});
  float reachMs = 0.0f;
  for (float rate = 0.0f; rate <= 100.0f; rate += 1.0f) {
    cv::UnisonTuning t;
    unisonrate::apply(t, rate);
    fits = fits && cv::Unison::tapsFit(t);
    const float top = std::max(t.swingMinMs, t.swingMaxMs * t.topSwingGain);
    reachMs = std::max(reachMs, std::max(t.baseDelayMs[0], t.baseDelayMs[1]) + top);
  }
  return report("taps fit", fits, "farthest tap %.1f ms over RATE 0 to 100 %%, inside the line: %.0f",
                static_cast<double>(reachMs), fits ? 1.0 : 0.0);
}

// A tap set beyond either end of the line reads the end of the line, so a tuning outside the range stays
// inside the memory and tapsFit says so.
bool testClampedTaps() {
  const std::vector<float> in = sine(cv::kSampleRate);
  const auto render = [&](float baseMs) {
    cv::UnisonParams p = params(1.0f);
    p.tuning.swingMinMs = p.tuning.swingMaxMs = 0.0f;
    p.tuning.baseDelayMs = {baseMs, baseMs};
    cv::Unison u;
    u.reset();
    return run(u, in, p);
  };
  cv::UnisonTuning wide;
  wide.baseDelayMs = {100.0f, 60.0f};
  const bool far = allEqual(render(100.0f), render(60.0f), 0, in.size());
  const bool near = allEqual(render(0.0f), render(0.01f), 0, in.size());
  return report("clamped taps", far && near && !cv::Unison::tapsFit(wide),
                "100 ms == 60 ms: %.0f, 0 ms == 0.01 ms: %.0f, tapsFit refuses the wide tuning: %.0f", far ? 1.0 : 0.0,
                near ? 1.0 : 0.0, cv::Unison::tapsFit(wide) ? 0.0 : 1.0);
}

}  // namespace

int main() {
  bool ok = true;
  ok &= testPassthrough();
  ok &= testSettleToCopy();
  ok &= testDetune();
  ok &= testBelowTop();
  ok &= testTopSwing();
  ok &= testTopWet();
  ok &= testLevel();
  ok &= testDry();
  ok &= testNoAlloc();
  ok &= testLfoAnalytic();
  ok &= testPhaseOffset();
  ok &= testHold();
  ok &= testSwitchOn();
  ok &= testRateMove();
  ok &= testDepthMove();
  ok &= testGlideConstants();
  ok &= testGlideLands();
  ok &= testNan();
  ok &= testTapsFit();
  ok &= testClampedTaps();
  return ok ? 0 : 1;
}
