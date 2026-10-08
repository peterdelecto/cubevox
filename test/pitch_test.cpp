// PitchTracker checks: vibrato tracking, steady-tone accuracy, silence, no-alloc.
// No window, no audio device.

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <new>
#include <vector>

#include "engine/harmony.h"
#include "engine/pitch.h"

namespace {

std::atomic<bool> gInProcess{false};

void guardAlloc() {
  if (gInProcess.load()) std::abort();
}

}  // namespace

// Allocation in push() aborts the test binary.
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
constexpr int kSettle = cv::kSampleRate / 10;  // 100 ms

// Mirrors the tracker's private kFineW and kFineSpan. The tracker keeps the
// newest kFineSpan samples and compares the oldest kFineW of them against a
// copy one lag newer, so a hop's pitch belongs to the sample at
// end - kFineSpan + kFineW / 2 + lag / 2.
constexpr int kFineWindow = 1024;
constexpr int kFineSpan = 1710;

int pitchCentre(int end, double hz) {
  return end - kFineSpan + kFineWindow / 2 + static_cast<int>(0.5 * cv::kSampleRate / hz);
}

// Ten harmonics at 1/k amplitude, peak 0.5. hzAt gives the pitch per sample,
// so a modulated pitch keeps the phase continuous.
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

// One result per hop, recorded where the hop ends.
struct Hop {
  int end;
  bool voiced;
  float hz;
};

// Feeds x in kBlock chunks under the allocation guard and records the result
// after every block that ends on a hop boundary.
std::vector<Hop> run(cv::PitchTracker& t, const std::vector<float>& x) {
  std::vector<Hop> hops;
  hops.reserve(x.size() / cv::PitchTracker::kHop + 1);
  const int total = static_cast<int>(x.size());
  for (int i = 0; i < total; i += cv::kBlock) {
    const int n = std::min(cv::kBlock, total - i);
    gInProcess = true;
    t.push(&x[static_cast<size_t>(i)], n, kThreshold);
    gInProcess = false;
    const int end = i + n;
    if (end % cv::PitchTracker::kHop == 0) hops.push_back({end, t.result().voiced, t.result().rawHz});
  }
  return hops;
}

bool report(const char* name, bool ok, const char* fmt, double a, double b = 0.0,
            double c = 0.0, double d = 0.0) {
  char detail[200];
  std::snprintf(detail, sizeof detail, fmt, a, b, c, d);
  std::printf("%s %s  %s\n", ok ? "PASS" : "FAIL", name, detail);
  return ok;
}

// 1. A3 with 5 Hz, +-30 cent vibrato: each voiced hop's rawHz matches the
// analytic pitch at the centre of its comparison within 3 cents.
bool testVibrato() {
  auto hzAt = [](int i) {
    return kA3 * std::exp2(0.3 / 12.0 * std::sin(2.0 * kPi * 5.0 * i / cv::kSampleRate));
  };
  const std::vector<float> x = tone(3 * cv::kSampleRate, hzAt);
  cv::PitchTracker t;
  const std::vector<Hop> hops = run(t, x);
  double worst = 0.0;
  int total = 0;
  int voiced = 0;
  for (const Hop& h : hops) {
    if (h.end <= kSettle) continue;
    ++total;
    if (!h.voiced) continue;
    ++voiced;
    worst = std::max(worst, std::fabs(cents(h.hz, hzAt(pitchCentre(h.end, h.hz)))));
  }
  const double pct = total > 0 ? 100.0 * voiced / total : 0.0;
  bool ok = report("vibrato tracks", voiced > 0 && worst <= 3.0,
                   "worst error vs analytic=%.2f cents (<= 3.0)", worst);
  ok &= report("vibrato voiced", pct >= 95.0, "voiced hops=%.1f %% (>= 95)", pct);
  return ok;
}

// 2. Steady 220 Hz: every hop after 100 ms voiced and within 0.5 cent.
bool testSteady() {
  const std::vector<float> x = tone(kA3, 2 * cv::kSampleRate);
  cv::PitchTracker t;
  const std::vector<Hop> hops = run(t, x);
  double worst = 0.0;
  int unvoiced = 0;
  for (const Hop& h : hops) {
    if (h.end <= kSettle) continue;
    if (!h.voiced) {
      ++unvoiced;
      continue;
    }
    worst = std::max(worst, std::fabs(cents(h.hz, kA3)));
  }
  return report("steady exact", unvoiced == 0 && worst <= 0.5,
                "worst error=%.3f cents (<= 0.5), unvoiced hops=%.0f (0)", worst, unvoiced);
}

// 3. Silence is never voiced once the first hop has passed.
bool testSilence() {
  const std::vector<float> x(static_cast<size_t>(cv::kSampleRate), 0.0f);
  cv::PitchTracker t;
  const std::vector<Hop> hops = run(t, x);
  int voiced = 0;
  for (const Hop& h : hops)
    if (h.end > cv::PitchTracker::kHop && h.voiced) ++voiced;
  return report("silence unvoiced", voiced == 0, "voiced hops after the first=%.0f (0)", voiced);
}

// 4. The pane-sum coarse search matches the direct 256-sample search.
struct Coarse {
  float tau;
  bool periodic;
};

float oracleSumSqDiff(const float* a, const float* b, int n) {
  float s0 = 0.0f, s1 = 0.0f, s2 = 0.0f, s3 = 0.0f;
  for (int j = 0; j < n; j += 4) {
    const float e0 = a[j] - b[j];
    const float e1 = a[j + 1] - b[j + 1];
    const float e2 = a[j + 2] - b[j + 2];
    const float e3 = a[j + 3] - b[j + 3];
    s0 += e0 * e0;
    s1 += e1 * e1;
    s2 += e2 * e2;
    s3 += e3 * e3;
  }
  return (s0 + s1) + (s2 + s3);
}

float oracleParabolic(float a, float b, float c) {
  const float denom = a - 2.0f * b + c;
  if (denom <= 0.0f) return 0.0f;
  const float off = 0.5f * (a - c) / denom;
  return off < -1.0f ? -1.0f : (off > 1.0f ? 1.0f : off);
}

Coarse oracleCoarse(const float* x, float threshold) {
  constexpr int kW = cv::PitchTracker::kCoarseWForTest;
  constexpr int kMin = cv::PitchTracker::kCoarseMinForTest;
  constexpr int kMax = cv::PitchTracker::kCoarseMaxForTest;
  std::vector<float> c(kMax + 1, 0.0f);
  float running = 0.0f;
  for (int t = 1; t <= kMax; ++t) {
    const float d = oracleSumSqDiff(x, x + t, kW);
    running += d;
    c[static_cast<size_t>(t)] = running > 0.0f ? d * t / running : 1.0f;
  }
  auto at = [&c](int t) { return c[static_cast<size_t>(t)]; };
  int best = kMin;
  int first = -1;
  for (int t = kMin; t <= kMax; ++t) {
    if (at(t) < at(best)) best = t;
    if (first < 0 && at(t) < threshold) first = t;
  }
  if (first >= 0) {
    while (first < kMax && at(first + 1) < at(first)) ++first;
  }
  const int pick = first >= 0 ? first : best;
  const float off = pick < kMax ? oracleParabolic(at(pick - 1), at(pick), at(pick + 1)) : 0.0f;
  return {static_cast<float>(pick) + off, at(best) < threshold};
}

// 8 harmonics at 1/n, pitch gliding 150 -> 300 Hz over 2 s then 5 Hz +-30 cent
// vibrato, 2 Hz +-30 % tremolo, -40 dB white noise, 200 ms gap in the middle.
std::vector<float> voiceLike(int n) {
  std::vector<float> out(static_cast<size_t>(n));
  uint32_t rng = 12345u;
  double phase = 0.0;
  const int glide = 2 * cv::kSampleRate;
  const int gapStart = n / 2 - cv::kSampleRate / 10;
  for (int i = 0; i < n; ++i) {
    const double hz =
        i < glide ? 150.0 * std::pow(2.0, static_cast<double>(i) / glide)
                  : 300.0 * std::exp2(0.3 / 12.0 *
                                      std::sin(2.0 * kPi * 5.0 * (i - glide) / cv::kSampleRate));
    phase += 2.0 * kPi * hz / cv::kSampleRate;
    double s = 0.0;
    for (int k = 1; k <= 8; ++k) s += std::sin(k * phase) / k;
    s *= 0.2 * (1.0 + 0.3 * std::sin(2.0 * kPi * 2.0 * i / cv::kSampleRate));
    rng = rng * 1664525u + 1013904223u;
    const double white = (static_cast<double>(rng >> 8) / 8388608.0 - 1.0) * 1.7320508;
    s += 0.01 * white;
    if (i >= gapStart && i < gapStart + cv::kSampleRate / 5) s = 0.0;
    out[static_cast<size_t>(i)] = static_cast<float>(s);
  }
  return out;
}

bool oracleCase(const char* name, const std::vector<float>& x) {
  cv::PitchTracker t;
  int hops = 0, tauOk = 0, perOk = 0;
  const int total = static_cast<int>(x.size());
  for (int i = 0; i + cv::kBlock <= total; i += cv::kBlock) {
    gInProcess = true;
    t.push(&x[static_cast<size_t>(i)], cv::kBlock, kThreshold, true);
    gInProcess = false;
    if ((i + cv::kBlock) % cv::PitchTracker::kHop != 0) continue;
    const Coarse o = oracleCoarse(t.coarseFrameForTest(), kThreshold);
    ++hops;
    if (std::fabs(t.lastCoarseTau() - o.tau) <= 1.0f) ++tauOk;
    if (t.lastCoarsePeriodic() == o.periodic) ++perOk;
  }
  const double tauPct = 100.0 * tauOk / hops;
  const double perPct = 100.0 * perOk / hops;
  char label[64];
  std::snprintf(label, sizeof label, "coarse oracle %s", name);
  return report(label, tauPct >= 99.5 && perPct >= 99.0,
                "hops=%.0f, tau within 1 = %.2f %% (>= 99.5), periodic equal = %.2f %% (>= 99)",
                hops, tauPct, perPct);
}

bool testOracle() {
  const std::vector<float> vib = tone(3 * cv::kSampleRate, [](int i) {
    return kA3 * std::exp2(0.3 / 12.0 * std::sin(2.0 * kPi * 5.0 * i / cv::kSampleRate));
  });
  bool ok = oracleCase("vibrato", vib);
  ok &= oracleCase("voice-like", voiceLike(4 * cv::kSampleRate));
  return ok;
}

}  // namespace

int main() {
  bool ok = true;
  ok &= testVibrato();
  ok &= testSteady();
  ok &= testSilence();
  ok &= testOracle();
  // Every push() call above ran under the allocation guard.
  ok &= report("no allocation", true, "push() ran under the new/delete guard", 0.0);
  std::printf("%s pitch\n", ok ? "PASS" : "FAIL");
  return ok ? 0 : 1;
}
