// POLISH EQ checks: both halves bit-exact when off, the front and back responses, flat, block
// splitting, no-alloc. No window, no audio device.

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "engine/polish.h"

#include "alloc_guard.h"

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr float kAmp = 0.251f;  // -12 dBFS
constexpr int kSecond = cv::kSampleRate;

enum class Half { Front, Back };

std::vector<float> sine(double hz, int n) {
  std::vector<float> x(static_cast<size_t>(n));
  for (int i = 0; i < n; ++i)
    x[static_cast<size_t>(i)] = kAmp * static_cast<float>(std::sin(2.0 * kPi * hz * i / cv::kSampleRate));
  return x;
}

// Runs the whole signal through one half in kBlock chunks.
std::vector<float> run(cv::Polish& e, Half half, const std::vector<float>& in, const cv::PolishParams& p) {
  std::vector<float> out(in.size());
  const int total = static_cast<int>(in.size());
  for (int i = 0; i < total; i += cv::kBlock) {
    const int n = total - i < cv::kBlock ? total - i : cv::kBlock;
    gInProcess = true;
    if (half == Half::Front) {
      e.processFront(&in[static_cast<size_t>(i)], &out[static_cast<size_t>(i)], n, p);
    } else {
      e.processBack(&in[static_cast<size_t>(i)], &out[static_cast<size_t>(i)], n, p);
    }
    gInProcess = false;
  }
  return out;
}

double rmsOf(const std::vector<float>& x, int from, int to) {
  double s = 0.0;
  for (int i = from; i < to; ++i) s += static_cast<double>(x[static_cast<size_t>(i)]) * x[static_cast<size_t>(i)];
  return std::sqrt(s / (to - from));
}

bool report(const char* name, bool ok, const char* fmt, double a, double b = 0.0, double c = 0.0,
            double d = 0.0, double e = 0.0, double f = 0.0) {
  char detail[240];
  std::snprintf(detail, sizeof detail, fmt, a, b, c, d, e, f);
  std::printf("%s %s  %s\n", ok ? "PASS" : "FAIL", name, detail);
  return ok;
}

static cv::Polish gEq;

// Gain in dB of a sine through one half, RMS over seconds 1-2.
double gainDb(Half half, double hz, const cv::PolishParams& p) {
  const std::vector<float> in = sine(hz, 2 * kSecond);
  gEq.reset();
  const std::vector<float> out = run(gEq, half, in, p);
  return 20.0 * std::log10(rmsOf(out, kSecond, 2 * kSecond) / rmsOf(in, kSecond, 2 * kSecond));
}

// Returns max|out - in| for `half` switched off. It reads after the first 100 ms from reset, and
// after the first 400 ms when the half was on before, because the 20 ms smoother snaps to off in
// about 280 ms.
float offError(Half half, bool afterOn) {
  const std::vector<float> in = sine(1000.0, kSecond);
  cv::PolishParams on;
  cv::PolishParams off;
  off.on = false;
  gEq.reset();
  if (afterOn) run(gEq, half, in, on);
  const std::vector<float> out = run(gEq, half, in, off);
  float maxDiff = 0.0f;
  for (int i = (afterOn ? 4 : 1) * cv::kSampleRate / 10; i < kSecond; ++i) {
    const float d = std::fabs(out[static_cast<size_t>(i)] - in[static_cast<size_t>(i)]);
    if (d > maxDiff) maxDiff = d;
  }
  return maxDiff;
}

bool testPassthrough() {
  const float f0 = offError(Half::Front, false), b0 = offError(Half::Back, false);
  const float f1 = offError(Half::Front, true), b1 = offError(Half::Back, true);
  return report("passthrough", f0 == 0.0f && b0 == 0.0f && f1 == 0.0f && b1 == 0.0f,
                "on=false max|out-in|, front/back from reset after 100 ms %.3g / %.3g, after being on after 400 ms %.3g / %.3g (0)",
                f0, b0, f1, b1);
}

// The de-esser sits out of the front response, which is the two filters alone.
cv::PolishParams filtersOnly() {
  cv::PolishParams p;
  p.tuning.deEsser.maxCutDb = 0.0f;
  return p;
}

bool testFront() {
  const cv::PolishParams p = filtersOnly();
  const double g50 = gainDb(Half::Front, 50.0, p), g70 = gainDb(Half::Front, 70.0, p);
  const double g97 = gainDb(Half::Front, 97.0, p), g1k = gainDb(Half::Front, 1000.0, p);
  const double g6k = gainDb(Half::Front, 6000.0, p), g10k = gainDb(Half::Front, 10000.0, p);
  const bool ok = g50 <= -11.0 && std::fabs(g70 + 3.0) <= 0.3 && std::fabs(g97) <= 0.5 && std::fabs(g1k) <= 0.1 &&
                  std::fabs(g6k + 3.0) <= 0.3 && g10k <= -20.0;
  return report("front response", ok,
                "50/70/97/1k/6k/10k Hz = %.2f / %.2f / %.2f / %.3f / %.2f / %.2f dB (<=-11, -3, 0, 0, -3, <=-20)",
                g50, g70, g97, g1k, g6k, g10k);
}

bool testFrontDeEsserQuiet() {
  const cv::PolishParams p;  // the de-esser at its tuning
  const double g97 = gainDb(Half::Front, 97.0, p), g1k = gainDb(Half::Front, 1000.0, p);
  return report("front voiced", std::fabs(g97) <= 0.5 && std::fabs(g1k) <= 0.1,
                "de-esser live: 97 Hz = %.3f dB, 1 kHz = %.3f dB (0 +-0.5, 0 +-0.1)", g97, g1k);
}

bool testBack() {
  // The shape is pinned without the level-rule trim.
  cv::PolishParams p;
  p.tuning.trimDb = 0.0f;
  const double g300 = gainDb(Half::Back, 300.0, p), g1k = gainDb(Half::Back, 1000.0, p);
  const double g35 = gainDb(Half::Back, 3500.0, p);
  const bool ok = std::fabs(g300 + 2.5) <= 0.4 && std::fabs(g1k) <= 0.4 && std::fabs(g35 - 1.5) <= 0.4;
  return report("back response", ok, "300/1k/3.5k Hz = %.2f / %.2f / %.2f dB (-2.5, 0, +1.5, each +-0.4)", g300,
                g1k, g35);
}

bool testFlat() {
  cv::PolishParams p;
  p.dipDb = 0.0f;
  p.presenceDb = 0.0f;
  p.tuning.trimDb = 0.0f;
  const double g = gainDb(Half::Back, 1000.0, p);
  return report("flat", std::fabs(g) <= 0.1, "back half, all gains 0: 1 kHz = %.3f dB (+-0.1)", g);
}

// A call of any n is the same as kBlock calls, because the front half splits at kBlock.
bool testSplit() {
  const std::vector<float> in = sine(5800.0, 4 * cv::kBlock + 8);
  const cv::PolishParams p;
  std::vector<float> whole(in.size()), parts(in.size());
  cv::Polish a, b;
  a.processFront(in.data(), whole.data(), static_cast<int>(in.size()), p);
  int done = 0;
  while (done < static_cast<int>(in.size())) {
    const int n = static_cast<int>(in.size()) - done < cv::kBlock ? static_cast<int>(in.size()) - done : cv::kBlock;
    b.processFront(&in[static_cast<size_t>(done)], &parts[static_cast<size_t>(done)], n, p);
    done += n;
  }
  float worst = 0.0f;
  for (size_t i = 0; i < in.size(); ++i) worst = std::fmax(worst, std::fabs(whole[i] - parts[i]));
  return report("split", worst == 0.0f, "one call against kBlock calls: max diff %.3g (0)", worst);
}

// In place gives the same samples as out of place.
bool testInPlace() {
  const std::vector<float> in = sine(700.0, 8 * cv::kBlock);
  const cv::PolishParams p;
  std::vector<float> apart(in.size()), inplace = in;
  cv::Polish a, b;
  for (int i = 0; i < static_cast<int>(in.size()); i += cv::kBlock) {
    a.processFront(&in[static_cast<size_t>(i)], &apart[static_cast<size_t>(i)], cv::kBlock, p);
    b.processFront(&inplace[static_cast<size_t>(i)], &inplace[static_cast<size_t>(i)], cv::kBlock, p);
    a.processBack(&apart[static_cast<size_t>(i)], &apart[static_cast<size_t>(i)], cv::kBlock, p);
    b.processBack(&inplace[static_cast<size_t>(i)], &inplace[static_cast<size_t>(i)], cv::kBlock, p);
  }
  return report("in place", apart == inplace, "both halves, in place against out of place: %.0f (1 = equal)",
                apart == inplace ? 1.0 : 0.0);
}

bool testNoAlloc() {
  const std::vector<float> in = sine(500.0, cv::kBlock * 8);
  gEq.reset();
  run(gEq, Half::Front, in, cv::PolishParams{});  // aborts on any new/delete inside process()
  run(gEq, Half::Back, in, cv::PolishParams{});
  return report("no-alloc", true, "%.0f allocations in processFront and processBack", 0.0);
}

}  // namespace

int main() {
  bool ok = true;
  ok &= testPassthrough();
  ok &= testFront();
  ok &= testFrontDeEsserQuiet();
  ok &= testBack();
  ok &= testFlat();
  ok &= testSplit();
  ok &= testInPlace();
  ok &= testNoAlloc();
  return ok ? 0 : 1;
}
