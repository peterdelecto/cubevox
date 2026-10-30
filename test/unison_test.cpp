// Unison checks: passthrough, detune, the top of the knob, level, no-alloc, the LFO against its
// analytic sine, and a RATE step that glides instead of jumping. No window, no audio device.

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <vector>

#include "core/unison_rate.h"
#include "engine/unison.h"

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

constexpr float kPi = 3.14159265358979323846f;
constexpr float kFreq = 440.0f;
constexpr float kAmp = 0.5f;

std::vector<float> sine(int n) {
  std::vector<float> x(static_cast<size_t>(n));
  for (int i = 0; i < n; ++i)
    x[static_cast<size_t>(i)] = kAmp * sinf(2.0f * kPi * kFreq * i / cv::kSampleRate);
  return x;
}

// Runs the whole signal through process() in kBlock chunks.
std::vector<float> run(cv::Unison& u, const std::vector<float>& in,
                       const cv::UnisonParams& p) {
  std::vector<float> out(in.size());
  const int total = static_cast<int>(in.size());
  for (int i = 0; i < total; i += cv::kBlock) {
    const int n = total - i < cv::kBlock ? total - i : cv::kBlock;
    gInProcess = true;
    u.process(&in[static_cast<size_t>(i)], &out[static_cast<size_t>(i)], n, p);
    gInProcess = false;
  }
  return out;
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

float rms(const std::vector<float>& x) {
  double s = 0.0;
  for (float v : x) s += static_cast<double>(v) * v;
  return static_cast<float>(std::sqrt(s / static_cast<double>(x.size())));
}

bool report(const char* name, bool ok, const char* fmt, double a, double b = 0.0,
            double c = 0.0) {
  char detail[160];
  std::snprintf(detail, sizeof detail, fmt, a, b, c);
  std::printf("%s %s  %s\n", ok ? "PASS" : "FAIL", name, detail);
  return ok;
}

static cv::Unison gUnison;

bool testPassthrough() {
  const int n = cv::kSampleRate;
  const std::vector<float> in = sine(n);
  gUnison.reset();
  const std::vector<float> out = run(gUnison, in, params(0.0f));
  float maxDiff = 0.0f;
  for (int i = cv::kSampleRate / 20; i < n; ++i) {
    const float d = std::fabs(out[static_cast<size_t>(i)] - in[static_cast<size_t>(i)]);
    if (d > maxDiff) maxDiff = d;
  }

  const std::vector<float> silence(static_cast<size_t>(n), 0.0f);
  gUnison.reset();
  const std::vector<float> sOut = run(gUnison, silence, params(1.0f));
  float maxSilent = 0.0f;
  for (float v : sOut) maxSilent = std::fmax(maxSilent, std::fabs(v));

  return report("passthrough", maxDiff < 1e-6f && maxSilent == 0.0f,
                "max|out-in|=%.3g (<1e-6), silence max=%.3g (==0)", maxDiff, maxSilent);
}

// Peak |cents| from upward zero-crossing periods, linearly interpolated.
float peakCents(const std::vector<float>& x, int skip) {
  float peak = 0.0f;
  float prevCross = -1.0f;
  for (size_t i = static_cast<size_t>(skip); i + 1 < x.size(); ++i) {
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

bool testDetune() {
  const int n = 4 * cv::kSampleRate;
  const std::vector<float> in = sine(n);
  gUnison.reset();
  gUnison.setVoiceEnabled(0, false);
  const std::vector<float> out = run(gUnison, in, untrimmed(1.0f));
  gUnison.setVoiceEnabled(0, true);

  std::vector<float> wet(in.size());
  for (size_t i = 0; i < in.size(); ++i) wet[i] = out[i] - in[i];
  const float cents = peakCents(wet, cv::kSampleRate / 2);
  // Expected peak follows the shipped tuning, so retuning by ear moves the band instead of
  // breaking it.
  const cv::UnisonTuning t;
  const float swingSec = t.swingMaxMs * t.topSwingGain * 0.001f;
  const float expect = 1200.0f * std::log2(1.0f + 2.0f * kPi * t.lfoHz[1] * swingSec);
  const float tol = 0.15f * expect;
  return report("detune", std::fabs(cents - expect) <= tol,
                "peak=%.2f cents (expect %.1f +-%.1f)", cents, expect, tol);
}

// The tuning without the top of the knob, which is the law as it stood before topFrom existed.
cv::UnisonParams flatTop(cv::UnisonParams p) {
  p.tuning.topSwingGain = 1.0f;
  p.tuning.topWetDb = 0.0f;
  return p;
}

// Fraction of the top reach in use at DEPTH d.
double overAt(const cv::UnisonTuning& t, double d) {
  return d > t.topFrom ? (d - t.topFrom) / (1.0 - t.topFrom) : 0.0;
}

// Runs blocks at depth d0 for the first half of the signal and d1 for the second.
std::vector<float> runStepped(cv::Unison& u, const std::vector<float>& in, cv::UnisonParams p, float d0,
                              float d1) {
  std::vector<float> out(in.size());
  const int total = static_cast<int>(in.size());
  for (int i = 0; i < total; i += cv::kBlock) {
    const int n = total - i < cv::kBlock ? total - i : cv::kBlock;
    p.depth = i < total / 2 ? d0 : d1;
    u.process(&in[static_cast<size_t>(i)], &out[static_cast<size_t>(i)], n, p);
  }
  return out;
}

// DEPTH 0.3 to 0.6: both top terms are exact neutrals, so the whole output equals the tuning
// without them, sample for sample, at the shipped swing, at the RATE 50 % swing, and across a
// depth step that ends at 0.6.
bool testBelowTop() {
  const std::vector<float> in = sine(cv::kSampleRate);
  const float depths[3] = {0.3f, 0.5f, 0.6f};
  bool same = true;
  for (float depth : depths) {
    for (int rate = 0; rate < 2; ++rate) {
      cv::UnisonParams p = params(depth);
      if (rate) unisonrate::apply(p.tuning, 50.0f);
      cv::Unison a, b;
      const std::vector<float> top = run(a, in, p);
      const std::vector<float> flat = run(b, in, flatTop(p));
      for (size_t i = 0; i < in.size(); ++i)
        if (top[i] != flat[i]) same = false;
      if (a.swingSamples() != b.swingSamples()) same = false;
    }
  }
  cv::Unison a, b;
  const cv::UnisonParams p = params(0.0f);
  const std::vector<float> top = runStepped(a, in, p, 0.2f, 0.6f);
  const std::vector<float> flat = runStepped(b, in, flatTop(p), 0.2f, 0.6f);
  for (size_t i = 0; i < in.size(); ++i)
    if (top[i] != flat[i]) same = false;
  std::printf("%s below-top  DEPTH 0.3, 0.5, 0.6 and a 0.2 to 0.6 step equal the tuning without the top (==): %s\n",
              same ? "PASS" : "FAIL", same ? "yes" : "NO");
  return same;
}

// Swing at the end of a run long enough for the depth glide to settle, in ms.
double settledSwingMs(const cv::UnisonParams& p) {
  const std::vector<float> in = sine(cv::kSampleRate);
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

// RMS of the wet path alone (dry subtracted, no trim) once the depth glide has settled.
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

// One knob-filter step of RATE above 68 %, where the swing follows the rate, at DEPTH 1. The
// taps glide to the new swing, so the output bends no harder than in steady state at either rate.
bool testRateStep() {
  const int n = 4 * cv::kSampleRate;
  const int stepAt = 2 * cv::kSampleRate;
  const std::vector<float> in = sine(n);
  cv::UnisonParams before = params(1.0f);
  cv::UnisonParams after = before;
  unisonrate::apply(before.tuning, 70.0f);
  unisonrate::apply(after.tuning, 71.5f);
  std::vector<float> out(in.size());
  gUnison.reset();
  for (int i = 0; i < n; i += cv::kBlock)
    gUnison.process(&in[static_cast<size_t>(i)], &out[static_cast<size_t>(i)], cv::kBlock, i < stepAt ? before : after);
  const double steady = std::max(peakCurvature(out, cv::kSampleRate, stepAt), peakCurvature(out, n - cv::kSampleRate, n));
  const double step = peakCurvature(out, stepAt, stepAt + cv::kSampleRate / 10);
  return report("rate step", step <= steady,
                "RATE 70 to 71.5 %%: peak curvature %.4g after the step, %.4g steady (<=)", step, steady);
}

bool testLevel() {
  const int n = 4 * cv::kSampleRate;
  const std::vector<float> in = sine(n);
  gUnison.reset();
  const std::vector<float> out = run(gUnison, in, params(1.0f));
  const float db = 20.0f * log10f(rms(out) / rms(in));
  // Default wetMaxDb is -4.0 dB (owner ear tuning) and trimDb -1.4 dB scales with depth
  // (level rule at DEPTH 0.5). At DEPTH 1 on this sine the output measures 2.24 dB, which
  // includes the +2.5 dB of the top of the knob (0.91 dB without it); band is 2.24 +-1.5.
  return report("level", std::fabs(db - 2.24f) <= 1.5f, "out/in=%.2f dB (2.24 +-1.5)", db);
}

bool testNoAlloc() {
  const std::vector<float> in = sine(cv::kBlock * 8);
  gUnison.reset();
  run(gUnison, in, params(0.7f));  // aborts on any new/delete inside process()
  return report("no-alloc", true, "%.0f allocations in process()", 0.0);
}

// The integer-phase LFO delay tracks the analytic sine at the quantised
// frequency over 10 s, at three Rate settings.
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
        const double want = sw * std::sin(2.0 * 3.14159265358979323846 * fq[v] * k / cv::kSampleRate);
        worstDelay = std::fmax(worstDelay, std::fabs(got - want));
      }
    }
  }
  return report("lfo analytic", worstDelay < 0.05 && worstHz < 1e-5,
                "worst delay error=%.3g samples (<0.05), worst freq error=%.3g Hz (<1e-5)",
                worstDelay, worstHz);
}

}  // namespace

int main() {
  bool ok = true;
  ok &= testPassthrough();
  ok &= testDetune();
  ok &= testBelowTop();
  ok &= testTopSwing();
  ok &= testTopWet();
  ok &= testLevel();
  ok &= testNoAlloc();
  ok &= testLfoAnalytic();
  ok &= testRateStep();
  return ok ? 0 : 1;
}
