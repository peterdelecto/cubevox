// Octave checks: shift up/down, fifth, bypass, shared-tracker wiring, no-alloc.
// No window, no audio device.

#include <algorithm>
#include <atomic>
#include <cmath>
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

const float kThreshold = cv::HarmonyTuning{}.voicedThreshold;

// PitchFx::pitch() read after one block, stamped with the block's end sample.
struct Hop {
  int end;
  cv::PitchResult r;
};

std::vector<float> run(cv::PitchFx& fx, const std::vector<float>& in, const cv::PitchFxParams& p,
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

static cv::PitchFx gFx;

cv::PitchFxParams octaveOnly(int semis) {
  cv::PitchFxParams p;
  p.octave.semitones = semis;
  p.octave.mix = 1.0f;
  return p;
}

// Median f0 of the output over seconds 1..2, measured by a fresh tracker.
double outputHz(double inHz, const cv::PitchFxParams& p) {
  gFx.reset();
  const std::vector<float> out = run(gFx, tone(inHz, 2 * cv::kSampleRate), p);
  cv::PitchTracker t;
  std::vector<double> hz;
  const int total = static_cast<int>(out.size());
  for (int i = 0; i < total; i += cv::kBlock) {
    const int n = std::min(cv::kBlock, total - i);
    t.push(&out[static_cast<size_t>(i)], n, kThreshold);
    if (i + n > cv::kSampleRate && t.result().voiced) hz.push_back(t.result().hz);
  }
  if (hz.empty()) return 0.0;
  std::sort(hz.begin(), hz.end());
  return hz[hz.size() / 2];
}

bool checkShift(const char* name, int semis, double expect) {
  const double got = outputHz(220.0, octaveOnly(semis));
  const double err = got > 0.0 ? cents(got, expect) : 9999.0;
  return report(name, std::fabs(err) <= 10.0, "out=%.2f Hz, expect %.2f, err=%.2f cents (+-10)",
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
    cv::PitchFxParams p = octaveOnly(0);
    p.octave.mix = mix;
    gFx.reset();
    const std::vector<float> out = run(gFx, in, p);
    for (size_t i = kSettle; i < in.size(); ++i) worst = std::max(worst, std::fabs(out[i] - in[i]));
  }
  cv::PitchFxParams off = octaveOnly(12);
  off.octave.mix = 0.5f;
  off.octave.on = false;
  gFx.reset();
  const std::vector<float> offOut = run(gFx, in, off);
  for (size_t i = kSettle; i < in.size(); ++i) worst = std::max(worst, std::fabs(offOut[i] - in[i]));
  return report("bypass", worst < 1e-6f, "mix 0/0.5/1 and on=false max|out-in|=%.3g (<1e-6)", worst);
}

// Octave and Harmony on together; the tracker must still hear only the dry tone.
bool testWiring() {
  cv::PitchFxParams p = octaveOnly(12);
  p.harmony.mix = 1.0f;
  p.harmony.slots[0] = {cv::HarmonyVoice::High, 3};
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
  cv::PitchFxParams p = octaveOnly(-12);
  p.octave.mix = 0.5f;
  p.harmony.mix = 0.5f;
  p.harmony.slots[0] = {cv::HarmonyVoice::High, 3};
  p.harmony.slots[1] = {cv::HarmonyVoice::Lower, 2};
  const std::vector<float> in = tone(220.0, cv::kBlock * 64);
  gFx.reset();
  run(gFx, in, p);  // aborts on any new/delete inside process()
  return report("no-alloc", true, "%.0f allocations in process()", 0.0);
}

}  // namespace

int main() {
  bool ok = true;
  ok &= testOctave();
  ok &= testFifth();
  ok &= testBypass();
  ok &= testWiring();
  ok &= testNoAlloc();
  return ok ? 0 : 1;
}
