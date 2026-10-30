// Reverb front-end distortion: each engine at its default dwell, fed a -6 dBFS
// 220 Hz sine, must stay clean. A linear tank returns a sine as a sine, but its
// gain differs from one harmonic to the next, so each harmonic is divided by the
// tank's gain at that frequency, read from a -60 dBFS sine the clipper passes
// untouched. What is left measures the dwell clipper alone. A second check holds the wet path's
// 5 Hz DC blocker to an exact high-pass over the raw tank output. No window, no audio device.

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

// The wet path ahead of the mix: a second reverb's own engine, driven the same way through its test
// hook, gives the raw tank output.
void rawTank(int engine, const std::vector<float>& in, std::vector<float>& out, const cv::ReverbParams& p) {
  auto rv = std::make_unique<cv::Reverb>();
  for (size_t i = 0; i < in.size(); i += cv::kBlock) {
    if (engine == cv::kReverbChasm) {
      rv->chasm().process(&in[i], &out[i], cv::kBlock, p.chasm);
    } else {
      rv->spring().process(&in[i], &out[i], cv::kBlock, p.spring);
    }
  }
}

// The reverb's wet-only output against the raw tank output through an exact one-pole high-pass at
// kWetDcBlockHz (double precision). Checks the corner, and that the blocker sits on the wet path of
// both engines. Input is a 0.5 DC step for the first half and a 100 Hz sine after it.
bool dcBlockedOnWetPath(int engine, const char* name) {
  const int total = 3 * cv::kSampleRate;
  const int half = total / 2;
  cv::ReverbParams p;
  p.on = true;
  p.engine = engine;
  p.wet = 1.0f;
  std::vector<float> in(static_cast<size_t>(total)), raw(in.size()), out(in.size());
  for (int i = 0; i < total; ++i) {
    in[static_cast<size_t>(i)] = i < half ? 0.5f
                                          : static_cast<float>(0.3 * std::sin(2.0 * kPi * 100.0 * (i - half) / cv::kSampleRate));
  }
  rawTank(engine, in, raw, p);
  auto rv = std::make_unique<cv::Reverb>();
  for (int i = 0; i < total; i += cv::kBlock) rv->process(&in[static_cast<size_t>(i)], &out[static_cast<size_t>(i)], cv::kBlock, p);

  const double a = 1.0 - std::exp(-2.0 * kPi * cv::kWetDcBlockHz / cv::kSampleRate);
  double lp = 0.0, worst = 0.0;
  for (size_t i = 0; i < raw.size(); ++i) {
    lp += a * (raw[i] - lp);
    worst = std::fmax(worst, std::fabs(static_cast<double>(out[i]) - (raw[i] - lp)));
  }
  constexpr double kMaxDiff = 1e-5;
  const bool matches = worst <= kMaxDiff;
  std::printf("%s %-14s wet path = raw tank through a %.0f Hz high-pass: worst difference %.2g (<= %.0e)\n",
              matches ? "PASS" : "FAIL", name, static_cast<double>(cv::kWetDcBlockHz), worst, kMaxDiff);

  // The 100 Hz component of the last two seconds of the sine, before and after the blocker.
  const std::vector<float> rawTail(raw.end() - cv::kSampleRate, raw.end());
  const std::vector<float> outTail(out.end() - cv::kSampleRate, out.end());
  const double db = 20.0 * std::log10(binMag(outTail, 100.0) / binMag(rawTail, 100.0));
  constexpr double kMaxToneDb = 0.05;
  const bool tone = std::fabs(db) <= kMaxToneDb;
  std::printf("%s %-14s 100 Hz passes the blocker at %.4f dB (within %.2f)\n", tone ? "PASS" : "FAIL", name, db,
              kMaxToneDb);
  return matches && tone;
}

}  // namespace

int main() {
  const int engines[2] = {cv::kReverbChasm, cv::kReverbSpring};
  const char* names[2] = {"CHASM", "SPRING"};
  bool ok = true;
  for (int e = 0; e < 2; ++e) ok = dcBlockedOnWetPath(engines[e], names[e]) && ok;
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
