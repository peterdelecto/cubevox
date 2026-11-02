// Reverb front-end distortion: each engine at its default dwell, fed a -6 dBFS
// 220 Hz sine, must stay clean. A linear tank returns a sine as a sine, but its
// gain differs from one harmonic to the next, so each harmonic is divided by the
// tank's gain at that frequency, read from a -60 dBFS sine the clipper passes
// untouched. What is left measures the dwell clipper alone. A second check holds the wet path's DC
// under -90 dBFS with no blocker on it. A third ties the struct defaults to the knob curves.
// No window, no audio device.

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

// The wet path's steady state for a sine of amp at hz: the last two seconds, a whole number of
// 220 Hz periods.
std::vector<float> wetTail(int engine, double hz, double amp) {
  auto rv = std::make_unique<cv::Reverb>();
  cv::ReverbParams p;
  p.on = true;
  p.engine = engine;
  p.wet = 1.0f;
  const int total = 4 * cv::kSampleRate;
  std::vector<float> in(static_cast<size_t>(total)), out(in.size());
  for (int i = 0; i < total; ++i)
    in[static_cast<size_t>(i)] = static_cast<float>(amp * std::sin(2.0 * kPi * hz * i / cv::kSampleRate));
  for (int i = 0; i < total; i += cv::kBlock)
    rv->process(&in[static_cast<size_t>(i)], &out[static_cast<size_t>(i)], cv::kBlock, p);
  return std::vector<float>(out.end() - 2 * cv::kSampleRate, out.end());
}

// Harmonic k of the -6 dBFS sine over the tank's gain at k * 220 Hz.
double clipperMag(int engine, const std::vector<float>& tail, int k) {
  constexpr double kSmall = 0.001;
  return binMag(tail, k * kHz) * kSmall / binMag(wetTail(engine, k * kHz, kSmall), k * kHz);
}

double thdPercent(int engine) {
  const std::vector<float> tail = wetTail(engine, kHz, 0.5);
  const double f0 = clipperMag(engine, tail, 1);
  double h = 0.0;
  for (int k = 2; k <= 7; ++k) h += std::pow(clipperMag(engine, tail, k), 2.0);
  return 100.0 * std::sqrt(h) / f0;
}

// The wet path carries no DC. A 0.5 DC step through the wet path alone (crossfade 1) at TIME 0, 50 and
// 100 % leaves a mean over the last second under kMaxDcDbfs, because every loop carries a bass cut.
// The box needs no blocker on the wet path while this holds.
bool wetCarriesNoDc(int engine, const char* name) {
  constexpr double kMaxDcDbfs = -90.0;
  const int total = 3 * cv::kSampleRate;
  const std::vector<float> in(static_cast<size_t>(total), 0.5f);
  double worst = -400.0;
  for (const float time : {0.0f, 0.5f, 1.0f}) {
    auto rv = std::make_unique<cv::Reverb>();
    cv::ReverbParams p;
    p.on = true;
    p.engine = engine;
    p.time = time;
    cv::applyKnobs(p);
    p.wet = 1.0f;
    std::vector<float> out(in.size());
    for (int i = 0; i < total; i += cv::kBlock)
      rv->process(&in[static_cast<size_t>(i)], &out[static_cast<size_t>(i)], cv::kBlock, p);
    double sum = 0.0;
    for (size_t i = out.size() - cv::kSampleRate; i < out.size(); ++i) sum += out[i];
    const double mean = sum / cv::kSampleRate;
    worst = std::fmax(worst, mean == 0.0 ? -400.0 : 20.0 * std::log10(std::fabs(mean)));
  }
  const bool ok = worst < kMaxDcDbfs;
  std::printf("%s %-14s 0.5 DC step: wet mean over the last second %.1f dBFS at worst (< %.0f)\n", ok ? "PASS" : "FAIL",
              name, worst, kMaxDcDbfs);
  return ok;
}

// The struct defaults for the crossfade and both engines' decay are what applyKnobs computes at the
// knobs' middle position. The render tool runs on the struct values and never calls applyKnobs, so a
// curve whose middle moves would otherwise leave the two apart.
bool defaultsMatchKnobs() {
  const cv::ReverbParams defaults;
  cv::ReverbParams knobs;
  cv::applyKnobs(knobs);
  const bool ok = knobs.wet == defaults.wet && knobs.chasm.decay == defaults.chasm.decay &&
                  knobs.spring.decay == defaults.spring.decay;
  std::printf("%s %-14s struct defaults wet %.3f, chasm decay %.3f, spring decay %.3f; applyKnobs gives %.3f, %.3f, %.3f\n",
              ok ? "PASS" : "FAIL", "defaults", static_cast<double>(defaults.wet),
              static_cast<double>(defaults.chasm.decay), static_cast<double>(defaults.spring.decay),
              static_cast<double>(knobs.wet), static_cast<double>(knobs.chasm.decay),
              static_cast<double>(knobs.spring.decay));
  return ok;
}

}  // namespace

int main() {
  const int engines[2] = {cv::kReverbChasm, cv::kReverbSpring};
  const char* names[2] = {"CHASM", "SPRING"};
  bool ok = defaultsMatchKnobs();
  for (int e = 0; e < 2; ++e) ok = wetCarriesNoDc(engines[e], names[e]) && ok;
  for (int e = 0; e < 2; ++e) {
    const double thd = thdPercent(engines[e]);
    const bool pass = thd <= kMaxThdPercent;
    std::printf("%s %-14s default dwell, -6 dBFS sine: THD %.2f %% (<= %.1f)\n", pass ? "PASS" : "FAIL",
                names[e], thd, kMaxThdPercent);
    ok = ok && pass;
  }
  std::printf("%s reverb_clean\n", ok ? "PASS" : "FAIL");
  return ok ? 0 : 1;
}
