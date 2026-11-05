// The Formant engine's base and voice sitting on one lag, and its reuse of Autotune's tracker directly
// behind the Autotune card. No window, no audio device.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>

#include "engine/autotune_fx.h"
#include "engine/formant_fx.h"

namespace {

using Signal = std::vector<float>;

constexpr double kPi = 3.14159265358979323846;

// Ten harmonics at 1/k amplitude, peak 0.5, with a 5 Hz vibrato of 20 cents.
Signal voice(double hz, int n) {
  Signal x(static_cast<size_t>(n));
  double phase = 0.0;
  double peak = 0.0;
  std::vector<double> y(x.size());
  for (int i = 0; i < n; ++i) {
    const double t = static_cast<double>(i) / cv::kSampleRate;
    double s = 0.0;
    for (int k = 1; k <= 10; ++k) s += std::sin(k * phase) / k;
    phase += 2.0 * kPi * hz * std::pow(2.0, 20.0 * std::sin(2.0 * kPi * 5.0 * t) / 1200.0) / cv::kSampleRate;
    y[static_cast<size_t>(i)] = s;
    peak = std::max(peak, std::fabs(s));
  }
  for (size_t i = 0; i < x.size(); ++i) x[i] = static_cast<float>(0.5 * y[i] / peak);
  return x;
}

template <typename F>
Signal run(const Signal& in, F&& block) {
  Signal out(in.size());
  const int total = static_cast<int>(in.size());
  for (int i = 0; i < total; i += cv::kBlock) {
    const int n = std::min(cv::kBlock, total - i);
    block(&in[static_cast<size_t>(i)], &out[static_cast<size_t>(i)], n, i);
  }
  return out;
}

bool report(const char* name, bool ok, double a, double b = 0.0) {
  std::printf("%s %s  %.2f %.2f\n", ok ? "PASS" : "FAIL", name, a, b);
  return ok;
}

// Normalised correlation of a with b delayed by lag samples, over [from, to).
double corrAt(const Signal& a, const Signal& b, int lag, size_t from, size_t to) {
  double ab = 0.0;
  double aa = 0.0;
  double bb = 0.0;
  for (size_t i = from; i < to; ++i) {
    const double x = a[i];
    const double y = b[i - static_cast<size_t>(lag)];
    ab += x * y;
    aa += x * x;
    bb += y * y;
  }
  return ab / std::sqrt(aa * bb + 1e-30);
}

// At SHIFT 0 and MIX 50 % the voice rebuilds the base, so the output is the input delayed by the lag.
bool testBase() {
  bool ok = true;
  const double hz = 220.0;
  const Signal in = voice(hz, cv::kSampleRate * 2);
  auto eng = std::make_unique<cv::FormantFx>();
  const Signal out = run(in, [&](const float* i, float* o, int n, int) {
    cv::FormantParams p;
    p.on = true;
    p.shiftSt = 0;
    p.mix = 0.5f;
    eng->process(i, o, n, p);
  });
  const size_t from = static_cast<size_t>(cv::kSampleRate);
  const size_t to = out.size();
  const int want = static_cast<int>(eng->lagForTest() + 0.5f);
  double best = -1.0;
  int bestLag = 0;
  for (int lag = want - 8; lag <= want + 8; ++lag) {
    const double c = corrAt(out, in, lag, from, to);
    if (c > best) {
      best = c;
      bestLag = lag;
    }
  }
  ok &= report("formant base and voice sit on one lag at SHIFT 0", best > 0.98 && std::abs(bestLag - want) <= 3, best,
               static_cast<double>(bestLag - want));
  const double lagMs = eng->lagForTest() / cv::kSmpPerMs;
  ok &= report("formant on its own tracker lags the grain delay plus a period",
               std::fabs(lagMs - (1040.0 + 48000.0 / hz) / 48.0) < 0.5, lagMs);
  return ok;
}

// Directly behind Autotune the engine reads Autotune's tracker and lags by the floor alone.
bool testReuse() {
  bool ok = true;
  const double hz = 220.0;
  const Signal in = voice(hz, cv::kSampleRate * 2);
  auto at = std::make_unique<cv::AutotuneFx>();
  auto fm = std::make_unique<cv::FormantFx>();
  Signal mid(in.size());
  const Signal out = run(in, [&](const float* i, float* o, int n, int pos) {
    cv::AutotuneParams ap;
    ap.on = true;
    ap.responseMs = 1.0f;
    cv::TrackerNeeds need;
    need.run = true;
    need.fine = true;
    need.outOfRange = true;
    at->process(i, &mid[static_cast<size_t>(pos)], n, ap, need);
    cv::UpstreamPitch up;
    up.result = &at->pitch();
    up.shiftSemis = at->correctionSemis();
    up.latency = at->outputLatency();
    cv::FormantParams fp;
    fp.on = true;
    fp.shiftSt = 0;
    fp.mix = 1.0f;
    fm->process(&mid[static_cast<size_t>(pos)], o, n, fp, up);
  });
  const double period = 48000.0 / hz;
  const double floorLag = 1.5 * period + 4.0;
  ok &= report("formant behind autotune lags the floor", std::fabs(fm->lagForTest() - floorLag) < 3.0, fm->lagForTest(),
               floorLag);
  const size_t from = static_cast<size_t>(cv::kSampleRate);
  const int want = static_cast<int>(fm->baseLagForTest() + 0.5f);
  double best = -1.0;
  for (int lag = want - 8; lag <= want + 8; ++lag) best = std::max(best, corrAt(out, mid, lag, from, out.size()));
  ok &= report("formant behind autotune rebuilds autotune's output at the lag it reports", best > 0.95, best);
  ok &= report("formant behind autotune ran no tracker of its own", fm->ownPitch().period == 0.0f, fm->ownPitch().period);
  return ok;
}

}  // namespace

int main() {
  bool ok = true;
  ok &= testBase();
  ok &= testReuse();
  return ok ? 0 : 1;
}
