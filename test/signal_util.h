#pragma once

// Signals and measurements the reverb tests share.

#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include "engine/common.h"

namespace testutil {

constexpr double kPi = 3.14159265358979323846;
constexpr double kSr = cv::kSampleRate;

using Signal = std::vector<float>;

// Uniform white noise from a linear congruential generator, so a seed always gives the same signal.
inline Signal noise(size_t n, float peak, uint32_t seed = 0x2545F491u) {
  Signal x(n);
  uint32_t r = seed;
  for (float& v : x) {
    r = r * 1664525u + 1013904223u;
    v = peak * (static_cast<float>(r >> 8) / 8388608.0f - 1.0f);
  }
  return x;
}

inline Signal sine(size_t n, double hz, float peak) {
  Signal x(n);
  for (size_t i = 0; i < n; ++i)
    x[i] = peak * static_cast<float>(std::sin(2.0 * kPi * hz * static_cast<double>(i) / kSr));
  return x;
}

inline Signal impulse(size_t n) {
  Signal x(n, 0.0f);
  x[0] = 1.0f;
  return x;
}

inline double rmsOf(const Signal& x, size_t from, size_t to) {
  double s = 0.0;
  for (size_t i = from; i < to; ++i) s += static_cast<double>(x[i]) * x[i];
  return std::sqrt(s / static_cast<double>(to - from));
}

inline float peakOf(const Signal& x) {
  float peak = 0.0f;
  for (const float v : x) peak = std::fmax(peak, std::fabs(v));
  return peak;
}

// 10 log10 of a power ratio.
inline double db(double ratio) { return 10.0 * std::log10(ratio < 1e-30 ? 1e-30 : ratio); }

inline void fft(std::vector<std::complex<double>>& a) {
  const size_t n = a.size();
  for (size_t i = 1, j = 0; i < n; ++i) {
    size_t bit = n >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) std::swap(a[i], a[j]);
  }
  for (size_t len = 2; len <= n; len <<= 1) {
    const std::complex<double> w = std::polar(1.0, -2.0 * kPi / static_cast<double>(len));
    for (size_t i = 0; i < n; i += len) {
      std::complex<double> wk = 1.0;
      for (size_t k = 0; k < len / 2; ++k) {
        const std::complex<double> u = a[i + k], v = a[i + k + len / 2] * wk;
        a[i + k] = u + v;
        a[i + k + len / 2] = u - v;
        wk *= w;
      }
    }
  }
}

// Welch power spectrum over [from, to), Hann window, half overlap. size is a power of two.
inline std::vector<double> spectrum(const Signal& x, size_t from, size_t to, size_t size) {
  std::vector<double> p(size / 2 + 1, 0.0);
  std::vector<std::complex<double>> buf(size);
  for (size_t s = from; s + size <= to; s += size / 2) {
    for (size_t i = 0; i < size; ++i) {
      const double w = 0.5 - 0.5 * std::cos(2.0 * kPi * static_cast<double>(i) / static_cast<double>(size));
      buf[i] = w * x[s + i];
    }
    fft(buf);
    for (size_t k = 0; k < p.size(); ++k) p[k] += std::norm(buf[k]);
  }
  return p;
}

// RBJ band-pass, 0 dB peak.
inline Signal bandPass(const Signal& x, double hz, double q) {
  const double w0 = 2.0 * kPi * hz / kSr;
  const double alpha = std::sin(w0) / (2.0 * q);
  const double a0 = 1.0 + alpha;
  const double b0 = alpha / a0, b2 = -alpha / a0;
  const double a1 = -2.0 * std::cos(w0) / a0, a2 = (1.0 - alpha) / a0;
  Signal y(x.size());
  double z1 = 0.0, z2 = 0.0;
  for (size_t i = 0; i < x.size(); ++i) {
    const double v = b0 * x[i] + z1;
    z1 = -a1 * v + z2;
    z2 = b2 * x[i] - a2 * v;
    y[i] = static_cast<float>(v);
  }
  return y;
}

// Zero-phase RMS envelope: a one-pole on the square, run forward then backward.
inline Signal envelope(const Signal& x, double sec) {
  const double a = 1.0 - std::exp(-1.0 / (sec * kSr));
  std::vector<double> p(x.size());
  double s = 0.0;
  for (size_t i = 0; i < x.size(); ++i) p[i] = s += a * (static_cast<double>(x[i]) * x[i] - s);
  s = 0.0;
  for (size_t i = x.size(); i-- > 0;) p[i] = s += a * (p[i] - s);
  Signal e(x.size());
  for (size_t i = 0; i < x.size(); ++i) e[i] = static_cast<float>(std::sqrt(p[i]));
  return e;
}

// First local maximum in [0, to) within 3 dB of the window's largest value, or -1.
inline long firstPeak(const Signal& e, size_t to) {
  float top = 0.0f;
  for (size_t i = 0; i < to; ++i) top = e[i] > top ? e[i] : top;
  for (size_t i = 1; i + 1 < to; ++i)
    if (e[i] >= 0.708f * top && e[i] >= e[i - 1] && e[i] > e[i + 1]) return static_cast<long>(i);
  return -1;
}

}  // namespace testutil
