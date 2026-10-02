// Autotune checks: passthrough, correction, scale vs chromatic, response time,
// Harmony builds on the corrected note, no-alloc. No window, no audio device.

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

const float kThreshold = cv::HarmonyTuning{}.voicedThreshold;

// Median f0 after 300 ms, measured by a fresh tracker.
double medianHz(const std::vector<float>& x) {
  cv::PitchTracker t;
  std::vector<double> hz;
  const int total = static_cast<int>(x.size());
  for (int i = 0; i < total; i += cv::kBlock) {
    const int n = std::min(cv::kBlock, total - i);
    t.push(&x[static_cast<size_t>(i)], n, kThreshold);
    if (i + n > kSettle && t.result().voiced) hz.push_back(t.result().hz);
  }
  if (hz.empty()) return 0.0;
  std::sort(hz.begin(), hz.end());
  return hz[hz.size() / 2];
}

// Tracker pitch and Autotune correction read after one block.
struct Hop {
  int end;
  float hz;
  float corr;
};

std::vector<float> run(cv::PitchFx& fx, const std::vector<float>& in,
                       const cv::PitchFxParams& p, std::vector<Hop>* hops = nullptr) {
  std::vector<float> out(in.size());
  if (hops) hops->reserve(in.size() / cv::kBlock + 1);
  const int total = static_cast<int>(in.size());
  for (int i = 0; i < total; i += cv::kBlock) {
    const int n = std::min(cv::kBlock, total - i);
    gInProcess = true;
    fx.process(&in[static_cast<size_t>(i)], &out[static_cast<size_t>(i)], n, p);
    gInProcess = false;
    if (hops) hops->push_back({i + n, fx.pitch().hz, fx.correctionSemis()});
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

// Autotune alone in key C; Harmony and Octave off.
cv::PitchFxParams autotuneOnly(float responseMs, int engine = 1) {
  cv::PitchFxParams p;
  p.autotune.on = true;
  p.autotune.engine = engine;
  p.autotune.key = 0;
  p.autotune.responseMs = responseMs;
  p.harmony.on = false;
  p.harmony.key = 0;
  p.octave.on = false;
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

// 1. Off is bit-exact. On with an in-tune A3, the output matches the input
// delayed by the shifter latency; the lag is searched around kGrainDelay.
bool testPassthrough() {
  const std::vector<float> in = tone(kA3, cv::kSampleRate);
  cv::PitchFxParams off = autotuneOnly(60.0f);
  off.autotune.on = false;
  gFx.reset();
  const int diff = countDiff(in, run(gFx, in, off), cv::kSampleRate / 10);
  bool ok = report("passthrough off", diff == 0, "samples differing after 100 ms=%.0f (0)", diff);

  gFx.reset();
  const std::vector<float> out = run(gFx, in, autotuneOnly(60.0f));
  const int from = kSettle;
  const int to = static_cast<int>(in.size());
  const double ref = rms(in, from, to);
  double bestDb = 999.0;
  int bestLag = 0;
  for (int lag = cv::PsolaVoice::kGrainDelay - 500; lag <= cv::PsolaVoice::kGrainDelay + 500; ++lag) {
    double s = 0.0;
    for (int i = from; i < to; ++i) {
      const double d = out[static_cast<size_t>(i)] - in[static_cast<size_t>(i - lag)];
      s += d * d;
    }
    const double db = 20.0 * std::log10(std::sqrt(s / (to - from)) / ref + 1e-12);
    if (db < bestDb) {
      bestDb = db;
      bestLag = lag;
    }
  }
  ok &= report("passthrough in tune", bestDb <= -40.0,
               "|out - in| RMS=%.1f dB re input at lag %.0f samples (<= -40)", bestDb, bestLag);
  return ok;
}

// 2. A3 + 50 cents pulls to A3 on both engines.
bool testCorrection() {
  bool ok = true;
  const char* names[2] = {"correction A", "correction B"};
  for (int e = 0; e < 2; ++e) {
    const double got = outputHz(kSharp, autotuneOnly(20.0f, e));
    const double err = got > 0.0 ? cents(got, kA3) : 9999.0;
    ok &= report(names[e], std::fabs(err) <= 5.0, "in=%.2f Hz, out=%.2f Hz, err vs A3=%.2f cents (+-5)",
                 kSharp, got, err);
  }
  return ok;
}

// 3. Bb3 is not in C major: scale mode pulls to A3 or B3, chromatic leaves it.
bool testScale() {
  const double got = outputHz(kBb3, autotuneOnly(20.0f));
  const double errA = got > 0.0 ? cents(got, kA3) : 9999.0;
  const double errB = got > 0.0 ? cents(got, kB3) : 9999.0;
  bool ok = report("scale Bb3 in C", std::fabs(errA) <= 5.0 || std::fabs(errB) <= 5.0,
                   "out=%.2f Hz, err vs A3=%.2f, vs B3=%.2f cents (one within +-5)", got, errA, errB);
  cv::PitchFxParams chrom = autotuneOnly(20.0f);
  chrom.autotune.chromatic = true;
  const double gotC = outputHz(kBb3, chrom);
  const double errC = gotC > 0.0 ? cents(gotC, kBb3) : 9999.0;
  ok &= report("chromatic Bb3", std::fabs(errC) <= 5.0, "out=%.2f Hz, err vs Bb3=%.2f cents (+-5)",
               gotC, errC);
  return ok;
}

// 4. Step A3 to A3 + 50 cents at 1 s, response 200 ms. Time runs from the
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

// 5. Harmony High on the corrected note: A3 + 50 cents gives C4, not C4 + 50.
bool testHarmonyFollows() {
  cv::PitchFxParams p = autotuneOnly(20.0f);
  p.harmony.on = true;
  p.harmony.mix = 1.0f;
  p.harmony.slots[0] = {cv::HarmonyVoice::High, 3};
  const double got = outputHz(kSharp, p);
  const double err = got > 0.0 ? cents(got, kC4) : 9999.0;
  return report("feeds harmony", std::fabs(err) <= 10.0,
                "out=%.2f Hz, err vs C4=%.2f cents (+-10)", got, err);
}

}  // namespace

int main() {
  bool ok = true;
  ok &= testPassthrough();
  ok &= testCorrection();
  ok &= testScale();
  ok &= testResponse();
  ok &= testHarmonyFollows();
  // Every process() call above ran under the allocation guard.
  ok &= report("no allocation", true, "process() ran under the new/delete guard", 0.0);
  std::printf("%s autotune\n", ok ? "PASS" : "FAIL");
  return ok ? 0 : 1;
}
