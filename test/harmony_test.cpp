// Harmony checks: tracker, shift, diatonic intervals, bypass, no-alloc.
// No window, no audio device.

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <vector>

#include "engine/harmony.h"

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
constexpr int kSettle = cv::kSampleRate / 10;  // 100 ms

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

// Uniform white noise at -20 dBFS RMS, fixed seed.
std::vector<float> noise(int n) {
  const float amp = 0.1f * std::sqrt(3.0f);
  uint32_t state = 12345u;
  std::vector<float> x(static_cast<size_t>(n));
  for (float& v : x) {
    state = state * 1664525u + 1013904223u;
    v = amp * (2.0f * static_cast<float>(state >> 8) / 16777216.0f - 1.0f);
  }
  return x;
}

// One tracker result per block of kBlock samples, timestamped at block end.
struct Frame {
  int end;
  cv::PitchResult r;
};

std::vector<Frame> track(const std::vector<float>& x, float threshold) {
  cv::PitchTracker t;
  std::vector<Frame> frames;
  frames.reserve(x.size() / cv::kBlock + 1);
  const int total = static_cast<int>(x.size());
  for (int i = 0; i < total; i += cv::kBlock) {
    const int n = std::min(cv::kBlock, total - i);
    t.push(&x[static_cast<size_t>(i)], n, threshold);
    frames.push_back({i + n, t.result()});
  }
  return frames;
}

std::vector<float> run(cv::Harmony& h, const std::vector<float>& in, const cv::HarmonyParams& p) {
  std::vector<float> out(in.size());
  const int total = static_cast<int>(in.size());
  for (int i = 0; i < total; i += cv::kBlock) {
    const int n = std::min(cv::kBlock, total - i);
    gInProcess = true;
    h.process(&in[static_cast<size_t>(i)], &out[static_cast<size_t>(i)], n, p);
    gInProcess = false;
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

const float kThreshold = cv::HarmonyTuning{}.voicedThreshold;

bool testTracker() {
  bool ok = true;
  for (double hz : {110.0, 220.0, 440.0}) {
    const std::vector<Frame> f = track(tone(hz, cv::kSampleRate), kThreshold);
    double worst = 0.0;
    int unvoiced = 0;
    for (const Frame& fr : f) {
      if (fr.end <= kSettle) continue;
      if (!fr.r.voiced) {
        ++unvoiced;
        continue;
      }
      worst = std::max(worst, std::fabs(cents(fr.r.hz, hz)));
    }
    char name[32];
    std::snprintf(name, sizeof name, "tracker %.0f Hz", hz);
    ok &= report(name, unvoiced == 0 && worst <= 5.0,
                 "worst=%.3f cents (+-5), unvoiced frames=%.0f (0)", worst, unvoiced);
  }
  const std::vector<Frame> f = track(noise(cv::kSampleRate), kThreshold);
  int voiced = 0;
  for (const Frame& fr : f)
    if (fr.end > kSettle && fr.r.voiced) ++voiced;
  ok &= report("tracker noise", voiced == 0, "voiced frames=%.0f (0)", voiced);
  return ok;
}

static cv::Harmony gHarmony;

cv::HarmonyParams highVoice() {
  cv::HarmonyParams p;
  p.key = 0;
  p.mix = 1.0f;
  p.slots[0] = {cv::HarmonyVoice::High, 3};
  return p;
}

// Median f0 of the output over seconds 1..2, measured by a fresh tracker.
double outputHz(double inHz) {
  gHarmony.reset();
  const std::vector<float> out = run(gHarmony, tone(inHz, 2 * cv::kSampleRate), highVoice());
  const std::vector<Frame> f = track(out, kThreshold);
  std::vector<double> hz;
  for (const Frame& fr : f)
    if (fr.end > cv::kSampleRate && fr.r.voiced) hz.push_back(fr.r.hz);
  if (hz.empty()) return 0.0;
  std::sort(hz.begin(), hz.end());
  return hz[hz.size() / 2];
}

bool testShift() {
  const double expect = 261.63;
  const double got = outputHz(220.0);
  const double err = got > 0.0 ? cents(got, expect) : 9999.0;
  return report("shift", std::fabs(err) <= 10.0, "out=%.2f Hz, err=%.2f cents (+-10)", got, err);
}

bool testDiatonic() {
  const double in[3] = {261.63, 293.66, 329.63};
  const double semis[3] = {4.0, 3.0, 3.0};
  bool ok = true;
  for (int i = 0; i < 3; ++i) {
    const double got = outputHz(in[i]);
    const double interval = got > 0.0 ? cents(got, in[i]) : 9999.0;
    const double err = interval - 100.0 * semis[i];
    char name[32];
    std::snprintf(name, sizeof name, "diatonic %.2f Hz", in[i]);
    ok &= report(name, std::fabs(err) <= 10.0,
                 "interval=%.2f cents (expect %.0f +-10), err=%.2f", interval, 100.0 * semis[i],
                 err);
  }
  return ok;
}

float maxDiffAfterSettle(const std::vector<float>& in, const std::vector<float>& out) {
  float m = 0.0f;
  for (size_t i = kSettle; i < in.size(); ++i) m = std::max(m, std::fabs(out[i] - in[i]));
  return m;
}

bool testBypass() {
  const std::vector<float> in = tone(220.0, cv::kSampleRate);

  cv::HarmonyParams dryMix = highVoice();
  dryMix.mix = 0.0f;
  dryMix.slots[1] = {cv::HarmonyVoice::Low, 3};
  gHarmony.reset();
  const float a = maxDiffAfterSettle(in, run(gHarmony, in, dryMix));

  cv::HarmonyParams noVoices;
  noVoices.mix = 0.7f;
  gHarmony.reset();
  const float b = maxDiffAfterSettle(in, run(gHarmony, in, noVoices));

  return report("bypass", a < 1e-6f && b < 1e-6f,
                "mix 0 max|out-in|=%.3g, no voices max|out-in|=%.3g (<1e-6)", a, b);
}

bool testNoAlloc() {
  cv::HarmonyParams p = highVoice();
  p.slots[1] = {cv::HarmonyVoice::Lower, 2};
  p.mix = 0.5f;
  const std::vector<float> in = tone(220.0, cv::kBlock * 64);
  gHarmony.reset();
  run(gHarmony, in, p);  // aborts on any new/delete inside process()
  return report("no-alloc", true, "%.0f allocations in process()", 0.0);
}

}  // namespace

int main() {
  bool ok = true;
  ok &= testTracker();
  ok &= testShift();
  ok &= testDiatonic();
  ok &= testBypass();
  ok &= testNoAlloc();
  return ok ? 0 : 1;
}
