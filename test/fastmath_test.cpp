// Error bounds for engine/fastmath.h: exp2Fast against the double exp2, and the
// Hann rotor against the cosine over the longest grain either voice runs.

#include <cmath>
#include <cstdio>
#include <initializer_list>

#include "engine/fastmath.h"

namespace {

constexpr double kPi = 3.14159265358979323846;

bool report(const char* name, bool ok, const char* fmt, double a, double b = 0.0) {
  char detail[160];
  std::snprintf(detail, sizeof detail, fmt, a, b);
  std::printf("%s %s  %s\n", ok ? "PASS" : "FAIL", name, detail);
  return ok;
}

// 1. Relative error of exp2Fast over -24..24 semitones of ratio (-2..2) and
// the wider -12..12, in 1e-4 steps.
bool testExp2() {
  double worst = 0.0;
  double at = 0.0;
  for (double x = -12.0; x <= 12.0; x += 1e-4) {
    const double want = std::exp2(x);
    const double got = cv::exp2Fast(static_cast<float>(x));
    const double rel = std::fabs(got - want) / want;
    if (rel > worst) {
      worst = rel;
      at = x;
    }
  }
  return report("exp2Fast", worst <= 1e-6, "worst relative error=%.3g at x=%.4f (<= 1e-6)", worst, at);
}

// 2. One grain of the longest PSOLA length (2 * 686 samples), rotor started
// at a fractional lead as launch() does.
bool testRotorGrain() {
  const double len = 2.0 * 686.0;
  const double lead = 0.37;
  cv::HannRotor r;
  r.init(static_cast<float>(2.0 * kPi * lead / len), static_cast<float>(2.0 * kPi / len));
  double worst = 0.0;
  for (int n = 0; n < static_cast<int>(len); ++n) {
    const double want = 0.5 * (1.0 - std::cos(2.0 * kPi * (lead + n) / len));
    worst = std::fmax(worst, std::fabs(r.window() - want));
    r.advance();
  }
  return report("rotor grain", worst <= 1e-4, "worst window error over %.0f samples=%.3g (<= 1e-4)", len, worst);
}

// 3. One block of the granular shifter at the fastest sweep (ratio 2, 20 ms
// window), restarted from an arbitrary phase as prepare() does.
bool testRotorBlock() {
  const double window = 960.0;
  const double step = 2.0 * kPi * (1.0 - 2.0) / window;
  const double phase0 = 2.0 * kPi * 0.8137;
  cv::HannRotor r;
  r.init(static_cast<float>(phase0), static_cast<float>(step));
  double worst = 0.0;
  for (int n = 0; n < 64; ++n) {
    const double want = 0.5 * (1.0 - std::cos(phase0 + n * step));
    worst = std::fmax(worst, std::fabs(r.window() - want));
    r.advance();
  }
  return report("rotor block", worst <= 1e-5, "worst window error over 64 samples=%.3g (<= 1e-5)", worst);
}

// 4. sinTurns against the double sine over the full phase range.
bool testSinTurns() {
  double worst = 0.0;
  auto check = [&](uint32_t p) {
    const double want = std::sin(2.0 * kPi * static_cast<double>(p) / 4294967296.0);
    worst = std::fmax(worst, std::fabs(static_cast<double>(cv::sinTurns(p)) - want));
  };
  uint32_t p = 0;
  for (int i = 0; i < 100000; ++i, p += 42949u) check(p);
  for (uint32_t e : {0u, 1u << 30, 1u << 31, 3u << 30}) check(e);
  const bool exact = cv::sinTurns(0u) == 0.0f && cv::sinTurns(1u << 31) == 0.0f;
  return report("sinTurns", worst < 1e-6 && exact,
                "worst absolute error=%.3g (< 1e-6), sin(0) and sin(pi) exact=%.0f", worst,
                exact ? 1.0 : 0.0);
}

// 5. tanhFast against the double tanh over -10..10, exact saturation at +-9,
// and bit-exact odd symmetry on random inputs.
bool testTanhFast() {
  double worst = 0.0;
  for (int k = 0; k <= 200000; ++k) {
    const float x = static_cast<float>(-10.0 + 20.0 * k / 200000.0);
    const double want = std::tanh(static_cast<double>(x));
    worst = std::fmax(worst, std::fabs(static_cast<double>(cv::tanhFast(x)) - want));
  }
  const bool sat = cv::tanhFast(9.0f) == 1.0f && cv::tanhFast(-9.0f) == -1.0f;
  bool odd = true;
  uint32_t s = 12345u;
  for (int i = 0; i < 1000; ++i) {
    s = s * 1664525u + 1013904223u;
    const float x = static_cast<float>((s >> 8) * (18.0 / 16777216.0) - 9.0);
    odd &= cv::tanhFast(-x) == -cv::tanhFast(x);
  }
  return report("tanhFast", worst < 1e-5 && sat && odd,
                "worst absolute error=%.3g (< 1e-5), +-9 exact and odd symmetry exact=%.0f", worst,
                (sat && odd) ? 1.0 : 0.0);
}

}  // namespace

int main() {
  bool ok = true;
  ok &= testExp2();
  ok &= testRotorGrain();
  ok &= testRotorBlock();
  ok &= testSinTurns();
  ok &= testTanhFast();
  std::printf("%s fastmath\n", ok ? "PASS" : "FAIL");
  return ok ? 0 : 1;
}
