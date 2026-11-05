// Error bounds for engine/fastmath.h against the double libm, over the ranges the callers use, with
// each band set at about twice the measured error: exp2Fast, log2Fast, the dB conversions, sinCosFast,
// sinTurns, tanhFast, the Hann rotor's block, the constants and the NaN behaviour.

#include <cmath>
#include <cstdio>
#include <cstdint>
#include <initializer_list>
#include <limits>

#include "engine/fastmath.h"

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();

bool report(const char* name, bool ok, const char* fmt, double a, double b = 0.0) {
  char detail[160];
  std::snprintf(detail, sizeof detail, fmt, a, b);
  std::printf("%s %s  %s\n", ok ? "PASS" : "FAIL", name, detail);
  return ok;
}

// 1. Relative error of exp2Fast over the callers' range of -12..12 (Autotune, Octave, Formant, Echo),
// in 1e-4 steps, and over -126..126, which takes in Bloom's -23.9 and Autotune's -15.
bool testExp2() {
  double worst = 0.0;
  double at = 0.0;
  auto sweep = [&](double lo, double hi, double stepSize) {
    for (double x = lo; x <= hi; x += stepSize) {
      const float xf = static_cast<float>(x);
      const double want = std::exp2(static_cast<double>(xf));
      const double rel = std::fabs(static_cast<double>(cv::exp2Fast(xf)) - want) / want;
      if (rel > worst) {
        worst = rel;
        at = x;
      }
    }
  };
  sweep(-12.0, 12.0, 1e-4);
  sweep(-126.0, 126.0, 1e-3);
  return report("exp2Fast", worst <= 1.5e-7, "worst relative error=%.3g at x=%.4f (<= 1.5e-7)", worst, at);
}

// 2. log2Fast against the double log2 over the detector range, 1e-9 to 16, log-spaced.
bool testLog2Fast() {
  double worst = 0.0;
  double at = 0.0;
  for (double e = -30.0; e <= 4.0; e += 1e-5) {
    const float x = static_cast<float>(std::exp2(e));
    const double err = std::fabs(static_cast<double>(cv::log2Fast(x)) - std::log2(static_cast<double>(x)));
    if (err > worst) {
      worst = err;
      at = e;
    }
  }
  return report("log2Fast", worst <= 3.3e-5, "worst absolute error=%.3g at log2=%.2f (<= 3.3e-5)", worst, at);
}

// 3. One block of the granular shifter at the fastest sweep (ratio 2, 20 ms window), restarted from an
// arbitrary phase as Octave's prepare() does, stepped the way its render loop steps it.
bool testRotorBlock() {
  const double window = 960.0;
  const double step = 2.0 * kPi * (1.0 - 2.0) / window;
  const double phase0 = 2.0 * kPi * 0.8137;
  cv::HannRotor r;
  r.init(static_cast<float>(phase0), static_cast<float>(step));
  float c = r.c, s = r.s;
  double worst = 0.0;
  for (int n = 0; n < 64; ++n) {
    const double want = 0.5 * (1.0 - std::cos(phase0 + n * step));
    worst = std::fmax(worst, std::fabs(static_cast<double>(0.5f - 0.5f * c) - want));
    const float c2 = c * r.dc - s * r.ds;
    s = s * r.dc + c * r.ds;
    c = c2;
  }
  return report("rotor block", worst <= 2.6e-7, "worst window error over 64 samples=%.3g (<= 2.6e-7)", worst);
}

// 4. sinCosFast, which seeds the rotor, against the double sine and cosine. Under 0.14 rad the error is
// relative and within float rounding of the true value. Over +-2 pi (every caller's range) and over the
// +-16 pi it documents it is absolute.
bool testSinCosFast() {
  double worstSmall = 0.0;
  for (int k = -140000; k <= 140000; ++k) {
    const float x = static_cast<float>(k * 1e-6);
    float s = 0.0f;
    float c = 0.0f;
    cv::sinCosFast(x, s, c);
    const double xd = static_cast<double>(x);
    const double sinErr = std::fabs(static_cast<double>(s) - std::sin(xd));
    if (k != 0) worstSmall = std::fmax(worstSmall, sinErr / std::fabs(std::sin(xd)));
    worstSmall = std::fmax(worstSmall, std::fabs(static_cast<double>(c) - std::cos(xd)) / std::cos(xd));
  }
  auto absolute = [](double turns) {
    double worst = 0.0;
    for (int k = -200000; k <= 200000; ++k) {
      const float x = static_cast<float>(2.0 * kPi * turns * k / 200000.0);
      float s = 0.0f;
      float c = 0.0f;
      cv::sinCosFast(x, s, c);
      const double xd = static_cast<double>(x);
      worst = std::fmax(worst, std::fabs(static_cast<double>(s) - std::sin(xd)));
      worst = std::fmax(worst, std::fabs(static_cast<double>(c) - std::cos(xd)));
    }
    return worst;
  };
  const double worstTurn = absolute(1.0);
  const double worstWide = absolute(8.0);
  const bool narrow = report("sinCosFast", worstSmall <= 1.2e-7 && worstTurn <= 1.4e-7,
                             "worst relative error under 0.14 rad=%.3g (<= 1.2e-7), absolute over +-2 pi=%.3g (<= 1.4e-7)",
                             worstSmall, worstTurn);
  const bool wide = report("sinCosFast wide", worstWide <= 1.4e-7, "absolute over +-16 pi=%.3g (<= 1.4e-7)", worstWide);
  return narrow && wide;
}

// 5. sinTurns against the double sine over the full phase range.
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
  return report("sinTurns", worst < 3.2e-7 && exact,
                "worst absolute error=%.3g (< 3.2e-7), sin(0) and sin(pi) exact=%.0f", worst,
                exact ? 1.0 : 0.0);
}

// 6. tanhFast against the double tanh over -10..10, exact saturation at +-9,
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
  return report("tanhFast", worst < 5.2e-7 && sat && odd,
                "worst absolute error=%.3g (< 5.2e-7), +-9 exact and odd symmetry exact=%.0f", worst,
                (sat && odd) ? 1.0 : 0.0);
}

// 7. dbFast against 20 log10 from the gate's detector floor (1e-9) to +12 dBFS, log-spaced.
bool testDbFast() {
  double worst = 0.0;
  double at = 0.0;
  for (double e = -9.0; e <= 0.6; e += 1e-5) {
    const float x = static_cast<float>(std::pow(10.0, e));
    const double want = 20.0 * std::log10(static_cast<double>(x));
    const double err = std::fabs(static_cast<double>(cv::dbFast(x)) - want);
    if (err > worst) {
      worst = err;
      at = want;
    }
  }
  return report("dbFast", worst < 2.2e-4, "worst error=%.3g dB at %.2f dB (< 2.2e-4)", worst, at);
}

// 8. dbToGainFast against 10^(dB / 20) over -140..+24 dB, relative, and DbGain on top of it.
bool testDbToGainFast() {
  double worst = 0.0;
  double at = 0.0;
  for (double db = -140.0; db <= 24.0; db += 1e-3) {
    const float d = static_cast<float>(db);
    const double want = std::pow(10.0, static_cast<double>(d) / 20.0);
    const double rel = std::fabs(static_cast<double>(cv::dbToGainFast(d)) - want) / want;
    if (rel > worst) {
      worst = rel;
      at = db;
    }
  }
  cv::DbGain g;
  const bool unity = g.gain() == 1.0f;
  g.set(-6.0f);
  const bool follows = g.gain() == cv::dbToGainFast(-6.0f);
  g.set(0.0f);
  const bool backToUnity = g.gain() == 1.0f;
  const bool gainOk = unity && follows && backToUnity;
  const bool fast = report("dbToGainFast", worst <= 1.6e-6, "worst relative error=%.3g at %.2f dB (<= 1.6e-6)", worst, at);
  const bool gain = report("DbGain", gainOk, "starts at 1, follows set() and returns to 1 exactly=%.0f", gainOk ? 1.0 : 0.0);
  return fast && gain;
}

// 9. The shared constants against their double values, to float rounding.
bool testConstants() {
  const double e = std::fabs(static_cast<double>(cv::kLog2E) - std::log2(std::exp(1.0))) / std::log2(std::exp(1.0));
  const double db = std::fabs(static_cast<double>(cv::kDbPerLog2) - 20.0 * std::log10(2.0)) / (20.0 * std::log10(2.0));
  const double pd = std::fabs(static_cast<double>(cv::kLog2PerDb) - std::log2(10.0) / 20.0) / (std::log2(10.0) / 20.0);
  const bool turn = cv::kTurn32 == 4294967296.0f;
  return report("constants", e < 2.7e-8 && db < 2.7e-8 && pd < 2.7e-8 && turn,
                "kLog2E, kDbPerLog2, kLog2PerDb worst relative error=%.3g (< 2.7e-8), kTurn32 is 2^32=%.0f",
                std::fmax(e, std::fmax(db, pd)), turn ? 1.0 : 0.0);
}

// 10. A NaN or an infinity in gives a finite result out of every function that casts to int, so the
// cast never sees a value it cannot hold.
bool testNonFinite() {
  const float inf = std::numeric_limits<float>::infinity();
  bool ok = true;
  for (float x : {kNaN, inf, -inf}) {
    const float e = cv::exp2Fast(x);
    float s = 0.0f;
    float c = 0.0f;
    cv::sinCosFast(x, s, c);
    ok &= std::isfinite(e) && e > 0.0f && std::isfinite(s) && std::isfinite(c);
  }
  ok &= cv::exp2Fast(kNaN) == cv::exp2Fast(-200.0f);  // NaN reads as the lower clamp
  ok &= cv::exp2Fast(inf) == cv::exp2Fast(200.0f);
  ok &= cv::dbToGainFast(kNaN) == cv::exp2Fast(-126.0f);
  return report("non-finite input", ok, "exp2Fast and sinCosFast stay finite for NaN and +-inf=%.0f", ok ? 1.0 : 0.0);
}

}  // namespace

int main() {
  bool ok = true;
  ok &= testExp2();
  ok &= testLog2Fast();
  ok &= testRotorBlock();
  ok &= testSinCosFast();
  ok &= testSinTurns();
  ok &= testTanhFast();
  ok &= testDbFast();
  ok &= testDbToGainFast();
  ok &= testConstants();
  ok &= testNonFinite();
  std::printf("%s fastmath\n", ok ? "PASS" : "FAIL");
  return ok ? 0 : 1;
}
