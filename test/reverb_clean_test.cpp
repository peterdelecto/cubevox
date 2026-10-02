// Reverb front-end distortion: each engine at its default DWELL, fed a -6 dBFS
// 220 Hz sine, must stay clean. A linear tank returns a sine as a sine, so the
// harmonics measure the DWELL clipper alone. No window, no audio device.

#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>

#include "engine/reverb.h"

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kHz = 220.0;
constexpr double kMaxThdPercent = 0.5;

// Magnitude of the DFT bin at hz over x, Hann-windowed.
double binMag(const std::vector<float>& x, double hz) {
  double re = 0.0, im = 0.0;
  const size_t n = x.size();
  for (size_t i = 0; i < n; ++i) {
    const double w = 0.5 - 0.5 * std::cos(2.0 * kPi * i / (n - 1));
    const double ph = 2.0 * kPi * hz * i / cv::kSampleRate;
    re += w * x[i] * std::cos(ph);
    im -= w * x[i] * std::sin(ph);
  }
  return std::sqrt(re * re + im * im);
}

double thdPercent(int engine) {
  auto rv = std::make_unique<cv::Reverb>();
  cv::ReverbParams p;
  p.on = true;
  p.engine = engine;
  p.mix = 1.0f;
  const int total = 4 * cv::kSampleRate;
  std::vector<float> in(static_cast<size_t>(total)), out(in.size());
  for (int i = 0; i < total; ++i)
    in[static_cast<size_t>(i)] = static_cast<float>(0.5 * std::sin(2.0 * kPi * kHz * i / cv::kSampleRate));
  for (int i = 0; i < total; i += cv::kBlock)
    rv->process(&in[static_cast<size_t>(i)], &out[static_cast<size_t>(i)], cv::kBlock, p);
  // Steady state: the last two seconds, a whole number of 220 Hz periods.
  const std::vector<float> tail(out.end() - 2 * cv::kSampleRate, out.end());
  const double f0 = binMag(tail, kHz);
  double h = 0.0;
  for (int k = 2; k <= 7; ++k) h += std::pow(binMag(tail, k * kHz), 2.0);
  return 100.0 * std::sqrt(h) / f0;
}

}  // namespace

int main() {
  const int engines[3] = {cv::kReverbSpring, cv::kReverbChasm, cv::kReverbParker};
  const char* names[3] = {"SPRING", "CHASM", "PARKER SPRING"};
  bool ok = true;
  for (int e = 0; e < 3; ++e) {
    const double thd = thdPercent(engines[e]);
    const bool pass = thd <= kMaxThdPercent;
    std::printf("%s %-14s default DWELL, -6 dBFS sine: THD %.2f %% (<= %.1f)\n", pass ? "PASS" : "FAIL",
                names[e], thd, kMaxThdPercent);
    ok = ok && pass;
  }
  std::printf("%s reverb_clean\n", ok ? "PASS" : "FAIL");
  return ok ? 0 : 1;
}
