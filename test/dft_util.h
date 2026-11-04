#pragma once

// Narrow-band measurements for the card tests: the Hann-windowed DFT magnitude of one frequency, and the
// frequency of the strongest peak inside a band, found by a coarse scan and a finer one round the best.

#include <cmath>
#include <complex>
#include <cstddef>
#include <vector>

#include "engine/common.h"

namespace dft {

using Signal = std::vector<float>;

// |sum x[n] w[n] e^(-j 2 pi f n / fs)| over [from, to), w a Hann window, divided by the window's sum.
inline double magnitude(const Signal& x, size_t from, size_t to, double hz) {
  constexpr double kPi = 3.14159265358979323846;
  const double n = static_cast<double>(to - from);
  const std::complex<double> rot = std::polar(1.0, -2.0 * kPi * hz / cv::kSampleRate);
  std::complex<double> phasor = 1.0;
  std::complex<double> sum = 0.0;
  double window = 0.0;
  for (size_t i = from; i < to; ++i) {
    const double w = 0.5 - 0.5 * std::cos(2.0 * kPi * static_cast<double>(i - from) / n);
    sum += static_cast<double>(x[i]) * w * phasor;
    window += w;
    phasor *= rot;
  }
  return std::abs(sum) / window;
}

// The frequency in [lo, hi] Hz where magnitude() peaks. The scan steps `coarse` Hz, then 20 steps of
// coarse / 20 on each side of the best, then 20 steps of coarse / 400.
inline double peakHz(const Signal& x, size_t from, size_t to, double lo, double hi, double coarse) {
  double best = lo;
  double bestMag = -1.0;
  for (double f = lo; f <= hi; f += coarse) {
    const double m = magnitude(x, from, to, f);
    if (m > bestMag) {
      bestMag = m;
      best = f;
    }
  }
  for (double span = coarse; span > coarse / 300.0; span /= 20.0) {
    const double step = span / 20.0;
    const double centre = best;
    for (double f = centre - span; f <= centre + span; f += step) {
      const double m = magnitude(x, from, to, f);
      if (m > bestMag) {
        bestMag = m;
        best = f;
      }
    }
  }
  return best;
}

}  // namespace dft
