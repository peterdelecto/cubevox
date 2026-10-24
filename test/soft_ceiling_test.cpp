// Output ceiling checks: bit-exact inside the knee, bounded and smooth above it, finite and odd for
// every float. No window, no audio device.

#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "engine/soft_ceiling.h"

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr int kRate = 48000;
constexpr double kOneUlp = 1.2e-7;  // one float step in [0.5, 1) is 6e-8; two cover a rounding either way

uint32_t bitsOf(float v) {
  uint32_t u;
  std::memcpy(&u, &v, sizeof u);
  return u;
}

float floatOf(uint32_t u) {
  float v;
  std::memcpy(&v, &u, sizeof v);
  return v;
}

double toDb(double lin) { return 20.0 * std::log10(lin); }

// Sample i of a sine whose peak is peakDb dBFS.
float sineAt(double hz, double peakDb, int i) {
  return static_cast<float>(std::pow(10.0, peakDb / 20.0) * std::sin(2.0 * kPi * hz * i / kRate));
}

bool report(const char* name, bool ok, const char* fmt, double a, double b = 0.0, double c = 0.0, double d = 0.0) {
  char detail[220];
  std::snprintf(detail, sizeof detail, fmt, a, b, c, d);
  std::printf("%s %s  %s\n", ok ? "PASS" : "FAIL", name, detail);
  return ok;
}

// A -3 dBFS peak sine passes bit for bit, and so does every float inside the knee, both signs.
bool testBitExact() {
  const double hzList[] = {55.0, 997.0, 5000.0};
  double sineSamples = 0.0, differ = 0.0;
  for (double hz : hzList) {
    for (int i = 0; i < kRate; ++i) {
      const float x = sineAt(hz, -3.0, i);
      sineSamples += 1.0;
      if (bitsOf(cv::softCeiling(x)) != bitsOf(x)) differ += 1.0;
    }
  }

  double floats = 0.0;
  const uint32_t kneeBits = bitsOf(cv::kSoftCeilingKnee);
  for (uint32_t b = 0; b <= kneeBits; b += 31) {
    const float x = floatOf(b);
    floats += 2.0;
    if (bitsOf(cv::softCeiling(x)) != bitsOf(x)) differ += 1.0;
    if (bitsOf(cv::softCeiling(-x)) != bitsOf(-x)) differ += 1.0;
  }
  const float edges[] = {cv::kSoftCeilingKnee, -cv::kSoftCeilingKnee, 0.0f, -0.0f, FLT_MIN, -FLT_MIN, 1e-45f, -1e-45f};
  for (float x : edges) {
    floats += 1.0;
    if (bitsOf(cv::softCeiling(x)) != bitsOf(x)) differ += 1.0;
  }
  return report("bit-exact below the knee", differ == 0.0,
                "%.0f samples of a -3 dBFS sine and %.0f floats up to the -2 dBFS knee, %.0f differ (0)", sineSamples,
                floats, differ);
}

// A +6 dBFS transient, as a sine and as a single sample, and a hotter one for the margin.
bool testTransient() {
  float sinePeak = 0.0f;
  for (int i = 0; i < kRate; ++i) sinePeak = std::fmax(sinePeak, std::fabs(cv::softCeiling(sineAt(997.0, 6.0, i))));
  const float pulse = static_cast<float>(std::pow(10.0, 6.0 / 20.0));
  const float pulsePeak = std::fmax(cv::softCeiling(pulse), -cv::softCeiling(-pulse));
  const float hotPeak = std::fmax(cv::softCeiling(1000.0f), -cv::softCeiling(-1000.0f));
  const bool ok = sinePeak < 1.0f && pulsePeak < 1.0f && hotPeak <= 1.0f;
  return report("transient stays under 0 dBFS", ok,
                "+6 dBFS sine peaks at %.7f (%.5f dBFS), +6 dBFS single sample at %.7f, +60 dBFS at %.7f (limit 1.0)",
                sinePeak, toDb(sinePeak), pulsePeak, hotPeak);
}

// Above the knee the curve rises with no step, no reversal and no corner.
bool testSmooth() {
  const int steps = 40000;
  const double stride = 1e-4;
  double worstDrop = 0.0, worstRise = 0.0;
  float prevX = 0.0f;
  float prevY = cv::softCeiling(prevX);
  for (int i = 1; i <= steps; ++i) {
    const float x = static_cast<float>(i * stride);
    const float y = cv::softCeiling(x);
    const double dy = static_cast<double>(y) - prevY;
    const double dx = static_cast<double>(x) - prevX;
    worstDrop = std::fmax(worstDrop, -dy);
    worstRise = std::fmax(worstRise, dy - dx);
    prevX = x;
    prevY = y;
  }

  const double knee = cv::kSoftCeilingKnee;
  const float above = static_cast<float>(knee + 0.01);
  const double kneeSlope = (static_cast<double>(cv::softCeiling(above)) - knee) / (static_cast<double>(above) - knee);

  const bool ok = worstDrop <= kOneUlp && worstRise <= kOneUlp && kneeSlope >= 0.99 && kneeSlope <= 1.0;
  return report("smooth above the knee", ok,
                "worst drop %.2g and worst gain over unity %.2g in 0..4 (<= 1.2e-7), slope over the first 0.01 past the knee "
                "%.4f (0.99..1.0)",
                worstDrop, worstRise, kneeSlope);
}

// Every float exponent, both signs, plus the special values: finite, within 1.0, odd, NaN to 0.
bool testEveryInput() {
  double inputs = 0.0, outOfRange = 0.0, notOdd = 0.0, nanKept = 0.0;
  auto check = [&](float x) {
    const float y = cv::softCeiling(x);
    inputs += 1.0;
    if (!std::isfinite(y) || std::fabs(y) > 1.0f) outOfRange += 1.0;
    if (std::isnan(x)) {
      if (y != 0.0f) nanKept += 1.0;
    } else if (bitsOf(cv::softCeiling(-x)) != bitsOf(-y)) {
      notOdd += 1.0;
    }
  };
  for (uint64_t b = 0; b <= 0xFFFFFFFFull; b += 4099) check(floatOf(static_cast<uint32_t>(b)));
  const float specials[] = {INFINITY, -INFINITY, NAN, -NAN, FLT_MAX, -FLT_MAX, 1e30f, -1e30f};
  for (float x : specials) check(x);
  return report("every input", outOfRange == 0.0 && notOdd == 0.0 && nanKept == 0.0,
                "%.0f floats incl. inf and NaN: %.0f not finite or past 1.0 (0), %.0f not odd (0), %.0f NaN not mapped to 0 (0)",
                inputs, outOfRange, notOdd, nanKept);
}

}  // namespace

int main() {
  bool ok = testBitExact();
  ok &= testTransient();
  ok &= testSmooth();
  ok &= testEveryInput();
  return ok ? 0 : 1;
}
