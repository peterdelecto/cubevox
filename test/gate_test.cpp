// Gate checks: passthrough, closing, attack/release timing, hysteresis, no-alloc.
// No window, no audio device.

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <vector>

#include "engine/gate.h"

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
constexpr int kMs = cv::kSampleRate / 1000;

float dbfs(double db) { return static_cast<float>(std::pow(10.0, db / 20.0)); }

// Feeds n samples one at a time. amp 0 is silence. Records the gain after each sample.
// Phase runs on across calls so a tone has no discontinuity.
class Rig {
 public:
  cv::Gate gate;
  long pos = 0;

  std::vector<float> feed(int n, double hz, float amp, const cv::GateParams& p,
                          std::vector<float>* in = nullptr, std::vector<float>* out = nullptr) {
    std::vector<float> g(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i, ++pos) {
      const float x = amp * static_cast<float>(std::sin(2.0 * kPi * hz * static_cast<double>(pos) / cv::kSampleRate));
      float y = 0.0f;
      gInProcess = true;
      gate.process(&x, &y, 1, p);
      gInProcess = false;
      g[static_cast<size_t>(i)] = gate.gainDb();
      if (in) in->push_back(x);
      if (out) out->push_back(y);
    }
    return g;
  }
};

bool report(const char* name, bool ok, const char* fmt, double a, double b = 0.0, double c = 0.0,
            double d = 0.0, double e = 0.0) {
  char detail[220];
  std::snprintf(detail, sizeof detail, fmt, a, b, c, d, e);
  std::printf("%s %s  %s\n", ok ? "PASS" : "FAIL", name, detail);
  return ok;
}

bool testPassthrough() {
  cv::GateParams off;
  off.on = false;
  Rig a;
  a.gate.reset();
  std::vector<float> in, out;
  a.feed(cv::kSampleRate, 1000.0, dbfs(-60.0), off, &in, &out);
  float offDiff = 0.0f;
  for (size_t i = static_cast<size_t>(100 * kMs); i < in.size(); ++i)
    offDiff = std::fmax(offDiff, std::fabs(out[i] - in[i]));

  cv::GateParams on;  // threshold -40
  Rig b;
  b.gate.reset();
  in.clear();
  out.clear();
  b.feed(cv::kSampleRate, 1000.0, dbfs(-30.0), on, &in, &out);
  float onDiff = 0.0f;
  for (size_t i = static_cast<size_t>(100 * kMs); i < in.size(); ++i)
    onDiff = std::fmax(onDiff, std::fabs(out[i] - in[i]));
  return report("passthrough", offDiff == 0.0f && onDiff < 1e-4f,
                "off max|out-in|=%.3g (0); on, +10 dB over threshold, max|out-in|=%.3g (<1e-4)", offDiff,
                onDiff);
}

bool testCloses() {
  cv::GateParams p;
  Rig r;
  r.gate.reset();
  const std::vector<float> g = r.feed(2 * cv::kSampleRate, 1000.0, dbfs(-60.0), p);
  const float last = g.back();
  return report("closes", last <= -70.0f, "1 kHz -60 dBFS, thr -40: steady gain=%.2f dB (<=-70)", last);
}

// First sample index in g at or past `from` where pred holds, else -1.
template <class F>
int firstWhere(const std::vector<float>& g, int from, F pred) {
  for (size_t i = static_cast<size_t>(from); i < g.size(); ++i)
    if (pred(g[i])) return static_cast<int>(i);
  return -1;
}

bool testTiming() {
  cv::GateParams p;
  const cv::GateTuning& t = p.tuning;
  Rig r;
  r.gate.reset();
  r.feed(2 * cv::kSampleRate, 1000.0, 0.0f, p);  // settle closed

  // Attack: tone at -20 dBFS.
  const std::vector<float> up = r.feed(500 * kMs, 1000.0, dbfs(-20.0), p);
  const int tAtt = firstWhere(up, 0, [](float g) { return g >= -1.0f; });
  const double attMs = tAtt < 0 ? -1.0 : static_cast<double>(tAtt) / kMs;
  const bool attOk = tAtt >= 0 && attMs <= static_cast<double>(t.attackMs) + 5.0;

  // Release: back to silence. The detector falls from -20 dBFS to the close
  // level before the hold timer starts, so that fall is part of the expected delay.
  const std::vector<float> dn = r.feed(2 * cv::kSampleRate, 1000.0, 0.0f, p);
  const int tLeave = firstWhere(dn, 0, [](float g) { return g < -1.0f; });
  const double holdMs = tLeave < 0 ? -1.0 : static_cast<double>(tLeave) / kMs;
  const double detFallMs = 20.0 * std::log(std::pow(10.0, ((p.thresholdDb - t.hysteresisDb) - -20.0) / -20.0));
  const double lo = static_cast<double>(t.holdMs) - 10.0;
  const double hi = static_cast<double>(t.holdMs) + detFallMs + 10.0;
  const bool holdOk = tLeave >= 0 && holdMs >= lo && holdMs <= hi;
  const int tRel = firstWhere(dn, 0, [&](float g) { return g <= t.rangeDb + 6.0f; });
  const double relMs = tRel < 0 ? -1.0 : static_cast<double>(tRel) / kMs - holdMs;
  const bool relOk = tRel >= 0 && relMs <= 3.0 * static_cast<double>(t.releaseMs);
  report("timing attack", attOk, "gain >= -1 dB after %.2f ms (<= attack+5 = %.1f)", attMs, t.attackMs + 5.0);
  report("timing hold", holdOk, "gain < -1 dB after %.1f ms (hold %.0f -10 .. +detector fall %.1f +10 = %.1f)",
         holdMs, t.holdMs, detFallMs, hi);
  return report("timing release", relOk, "range+6 dB reached %.1f ms after the drop (<= 3*release = %.0f)",
                relMs, 3.0 * t.releaseMs) && attOk && holdOk;
}

bool testHysteresis() {
  cv::GateParams p;  // threshold -40, hysteresis 3
  Rig r;
  r.gate.reset();
  r.feed(500 * kMs, 1000.0, dbfs(-20.0), p);  // open
  const std::vector<float> stay = r.feed(1000 * kMs, 1000.0, dbfs(-41.0), p);
  float minStay = 0.0f;
  for (float g : stay) minStay = std::fmin(minStay, g);
  const std::vector<float> shut = r.feed(1500 * kMs, 1000.0, dbfs(-44.0), p);
  const float endShut = shut.back();
  return report("hysteresis", minStay > -1.0f && endShut < -10.0f,
                "-41 dBFS after open: min gain %.2f dB (> -1); -44 dBFS: gain %.2f dB (< -10)", minStay,
                endShut);
}

bool testNoAlloc() {
  cv::GateParams p;
  Rig r;
  r.gate.reset();
  r.feed(cv::kBlock * 8, 500.0, dbfs(-30.0), p);  // aborts on any new/delete inside process()
  return report("no-alloc", true, "%.0f allocations in process()", 0.0);
}

}  // namespace

int main() {
  bool ok = true;
  ok &= testPassthrough();
  ok &= testCloses();
  ok &= testTiming();
  ok &= testHysteresis();
  ok &= testNoAlloc();
  return ok ? 0 : 1;
}
