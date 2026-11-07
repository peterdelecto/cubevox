// Lo-fi checks: settled off, silence, band corners, honk, crush law, hold, chunking, enable edge, TONE steps
// with the crush, a NaN or infinity in, a NaN knob, the tanh table, level, no-alloc.
// AMOUNT in every row is the pot position, 0 to 1. No window, no audio device. The level check reads
// Adam's clips from samples/ and skips when they are absent.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

#include "engine/lofi.h"
#include "engine/tanh_cubic.h"

#include "alloc_guard.h"
#include "clip_util.h"
#include "signal_util.h"

namespace {

using testutil::Signal;
using testutil::kPi;
using testutil::noise;
using testutil::peakCurvature;
using testutil::rmsOf;
using testutil::sine;

constexpr int kSecond = cv::kSampleRate;

// A pot position just under the law's AMOUNT 50 %, where the band is closed and driven and the crush
// has not begun.
constexpr float kBandPot = 0.8333f;

bool report(const char* name, bool ok, const char* fmt, double a = 0.0, double b = 0.0, double c = 0.0) {
  char detail[320];
  std::snprintf(detail, sizeof detail, fmt, a, b, c);
  std::printf("%s %s  %s\n", ok ? "PASS" : "FAIL", name, detail);
  return ok;
}

// Runs the signal through process() in chunks of `chunk` samples.
Signal run(cv::Lofi& l, const Signal& in, const cv::LofiParams& p, int chunk = cv::kBlock) {
  Signal out(in.size());
  const int total = static_cast<int>(in.size());
  for (int i = 0; i < total; i += chunk) {
    const int n = total - i < chunk ? total - i : chunk;
    gInProcess = true;
    l.process(&in[static_cast<size_t>(i)], &out[static_cast<size_t>(i)], n, p);
    gInProcess = false;
  }
  return out;
}

// Amplitude of one frequency over [from, to).
double goertzel(const Signal& x, size_t from, size_t to, double hz) {
  const double w = 2.0 * kPi * hz / cv::kSampleRate;
  double re = 0.0, im = 0.0;
  for (size_t i = from; i < to; ++i) {
    const double v = x[i];
    const double ph = w * static_cast<double>(i - from);
    re += v * std::cos(ph);
    im += v * std::sin(ph);
  }
  return 2.0 * std::sqrt(re * re + im * im) / static_cast<double>(to - from);
}

static cv::Lofi gLofi;

// The tuning with the drive, the honk and the makeup out, so the sweep sees the band alone.
cv::LofiParams bandOnly(float tone, float amount) {
  cv::LofiParams p;
  p.tone = tone;
  p.amount = amount;
  p.tuning.honkDb = 0.0f;
  p.tuning.driveDb = 0.0f;
  for (auto& row : p.tuning.makeupDb) row.fill(0.0f);
  p.tuning.trimDb = 0.0f;
  return p;
}

// Gain of the engine at one frequency, from a -40 dBFS sine, so the tanh stays linear.
double gainAt(const cv::LofiParams& p, double hz) {
  const size_t n = kSecond / 3;
  const Signal in = sine(n, hz, 0.01f);
  gLofi.reset();
  const Signal out = run(gLofi, in, p);
  const size_t from = n / 2;
  return goertzel(out, from, in.size(), hz) / goertzel(in, from, in.size(), hz);
}

constexpr double kMinus3Db = 0.70794578;

// The frequency in [lo, hi] where the gain crosses -3 dB, by bisection on a log scale. rising is true
// for the high-pass flank.
double cornerHz(const cv::LofiParams& p, double lo, double hi, bool rising) {
  for (int i = 0; i < 16; ++i) {
    const double mid = std::sqrt(lo * hi);
    const bool above = gainAt(p, mid) > kMinus3Db;
    if (above == rising) hi = mid;
    else lo = mid;
  }
  return std::sqrt(lo * hi);
}

bool testSettledOff() {
  const Signal in = noise(2 * kSecond, 0.4f);
  cv::LofiParams p;
  p.on = false;
  gLofi.reset();
  const Signal never = run(gLofi, in, p);
  size_t differNever = 0;
  for (size_t i = 0; i < in.size(); ++i) differNever += never[i] != in[i];

  // On for 0.5 s, then off: from 0.5 s after the off edge the output is the input.
  cv::LofiParams on;
  gLofi.reset();
  run(gLofi, Signal(in.begin(), in.begin() + kSecond / 2), on);
  const Signal rest = run(gLofi, Signal(in.begin() + kSecond / 2, in.end()), p);
  size_t differOn = 0, differAfter = 0;
  for (size_t i = 0; i < rest.size(); ++i) {
    const bool differs = rest[i] != in[i + static_cast<size_t>(kSecond / 2)];
    if (i < static_cast<size_t>(kSecond / 2)) differOn += differs;
    else differAfter += differs;
  }
  return report("settled off", differNever == 0 && differAfter == 0 && differOn > 0,
                "never on: %.0f samples differ (0); 0.5 s after the off edge: %.0f differ (0)",
                static_cast<double>(differNever), static_cast<double>(differAfter));
}

bool testSilence() {
  const Signal zero(static_cast<size_t>(kSecond / 2), 0.0f);
  const float settings[][2] = {{0.5f, 0.5f}, {0.0f, 1.0f}, {1.0f, 1.0f}, {0.5f, 0.0f}, {0.2f, 0.9f}};
  size_t nonzero = 0;
  for (const auto& s : settings) {
    cv::LofiParams p;
    p.tone = s[0];
    p.amount = s[1];
    gLofi.reset();
    const Signal out = run(gLofi, zero, p);
    for (float v : out) nonzero += v != 0.0f;
  }
  return report("silence", nonzero == 0, "zeros in give %.0f nonzero samples out over 5 settings (0)",
                static_cast<double>(nonzero));
}

bool testBandCorners() {
  bool ok = true;
  double worst = 0.0;
  for (const float tone : {0.0f, 0.5f, 1.0f}) {
    const cv::LofiParams p = bandOnly(tone, kBandPot);
    const double fc = 600.0 * std::pow(4.0, tone);
    const double hp = cornerHz(p, fc / 4.0, fc, true);
    const double lp = cornerHz(p, fc, fc * 4.0, false);
    const double hpErr = hp / (fc / 2.0) - 1.0, lpErr = lp / (fc * 2.0) - 1.0;
    worst = std::max({worst, std::fabs(hpErr), std::fabs(lpErr)});
    ok &= std::fabs(hpErr) <= 0.05 && std::fabs(lpErr) <= 0.05;
    std::printf("     tone %.1f: high-pass -3 dB at %.0f Hz (law %.0f), low-pass at %.0f Hz (law %.0f)\n", tone, hp,
                fc / 2.0, lp, fc * 2.0);
  }
  return report("band corners", ok, "band closed: worst corner error %.1f %% (<= 5)", 100.0 * worst);
}

bool testHonk() {
  cv::LofiParams withHonk = bandOnly(0.5f, kBandPot);
  withHonk.tuning.honkDb = 6.0f;
  const cv::LofiParams without = bandOnly(0.5f, kBandPot);
  const double fc = 1200.0;
  const double db = 20.0 * std::log10(gainAt(withHonk, fc) / gainAt(without, fc));
  cv::LofiParams low = withHonk;
  low.amount = 0.0f;
  cv::LofiParams lowWithout = without;
  lowWithout.amount = 0.0f;
  const double dbLow = 20.0 * std::log10(gainAt(low, fc) / gainAt(lowWithout, fc));
  return report("honk", std::fabs(db - 6.0) <= 0.3 && std::fabs(dbLow) <= 0.1,
                "honk at the centre: band closed %.2f dB (6 +-0.3), AMOUNT 0 %.2f dB (0 +-0.1)", db, dbLow);
}

// The law's end points at the pot's top and part way up the crush. The pot covers the law's AMOUNT from 0
// to 60 %, so the crush law (48 kHz and 16 bits down to 6 kHz and 5 bits over the law's 50 to 100 %)
// reaches a fifth of its travel.
bool testCrushLaw() {
  const cv::LofiTuning t;
  const cv::Lofi::Law top = cv::Lofi::lawAt(0.5f, 1.0f, t);
  const cv::Lofi::Law mid = cv::Lofi::lawAt(0.5f, 0.9f, t);
  const cv::Lofi::Law edge = cv::Lofi::lawAt(0.5f, kBandPot, t);
  const double topHz = top.holdRatio * static_cast<double>(cv::kSampleRate);
  const double midHz = mid.holdRatio * static_cast<double>(cv::kSampleRate);
  const double wantTop = 48000.0 * std::pow(0.125, 0.2), wantMid = 48000.0 * std::pow(0.125, 0.08);
  const bool ok = std::fabs(topHz - wantTop) < 1.0 && std::fabs(top.bits - 13.8f) < 1e-4f &&
                  std::fabs(midHz - wantMid) < 1.0 && std::fabs(mid.bits - 15.12f) < 1e-4f && edge.holdRatio == 1.0f &&
                  !edge.crush && std::fabs(edge.bits - 16.0f) < 1e-4f &&
                  std::fabs(static_cast<double>(top.step) - std::exp2(1.0 - 13.8)) < 1e-7;
  return report("crush law", ok, "pot top: %.0f Hz, %.2f bits; pot 90 %%: %.0f Hz (31668, 13.80; 40644)", topHz,
                static_cast<double>(top.bits), midHz);
}

// Where a held output changes. A hold of 8 samples changes the value only on one grid of every 8 samples.
struct HeldGrid {
  size_t offGrid;      // changes off the grid
  double onGridFrac;   // share of grid points that change
};

HeldGrid heldGrid(const Signal& out, size_t from) {
  size_t first = from + 1;
  while (first < out.size() && out[first] == out[first - 1]) ++first;
  const size_t offset = first % 8;
  size_t offGrid = 0, onGrid = 0, changed = 0;
  for (size_t i = from + 1; i < out.size(); ++i) {
    if (i % 8 == offset) {
      ++onGrid;
      changed += out[i] != out[i - 1];
    } else {
      offGrid += out[i] != out[i - 1];
    }
  }
  return {offGrid, static_cast<double>(changed) / static_cast<double>(onGrid)};
}

// The sorted distinct values of out[from..], and the smallest gap between two of them.
double smallestGap(const Signal& out, size_t from, size_t* distinct) {
  std::vector<float> values(out.begin() + static_cast<std::ptrdiff_t>(from), out.end());
  std::sort(values.begin(), values.end());
  values.erase(std::unique(values.begin(), values.end()), values.end());
  double gap = 1e9;
  for (size_t i = 1; i < values.size(); ++i) gap = std::min(gap, static_cast<double>(values[i]) - values[i - 1]);
  *distinct = values.size();
  return gap;
}

// With the hold at 6 kHz and 5 bits at the pot's top, the output holds each value for 8 samples, and the
// values are multiples of one step.
bool testCrush() {
  cv::LofiParams p;
  p.amount = 1.0f;
  p.tuning.holdMaxHz = p.tuning.holdMinHz = 6000.0f;
  p.tuning.bitsMax = p.tuning.bitsMin = 5.0f;
  gLofi.reset();
  const Signal out = run(gLofi, noise(kSecond, 0.5f), p);
  const size_t from = static_cast<size_t>(kSecond / 2);
  const HeldGrid grid = heldGrid(out, from);

  const cv::Lofi::Law law = cv::Lofi::lawAt(p.tone, p.amount, p.tuning);
  size_t distinct = 0;
  const double gapRatio = smallestGap(out, from, &distinct) / (static_cast<double>(law.makeup / law.drive) * law.step);
  const bool ok = grid.offGrid == 0 && grid.onGridFrac >= 0.85 && std::fabs(gapRatio - 1.0) < 1e-3 && distinct <= 33 &&
                  distinct >= 25;
  std::printf("     held runs: %.0f values change off the 8-sample grid (0), %.0f %% of grid points change (>= 85)\n",
              static_cast<double>(grid.offGrid), 100.0 * grid.onGridFrac);
  return report("crush", ok, "5 bits at 6 kHz: smallest output gap %.4f of one step (1), %.0f distinct values (25..33)",
                gapRatio, static_cast<double>(distinct));
}

// Held values per second at the output, from the changes in 1 s of noise.
double heldRateHz(const cv::LofiParams& p) {
  const Signal in = noise(kSecond, 0.5f);
  gLofi.reset();
  const Signal out = run(gLofi, in, p);
  const size_t from = static_cast<size_t>(kSecond / 4);
  size_t changes = 0;
  for (size_t i = from + 1; i < out.size(); ++i) changes += out[i] != out[i - 1];
  return static_cast<double>(changes) * kSecond / static_cast<double>(out.size() - from - 1);
}

// At the pot's top the held rate follows the law, and the hold's Nyquist sits well over the band's top at
// every TONE, so the hold does not fold the band back into itself.
bool testHold() {
  bool ok = true;
  double worstRatio = 1e9, worstRate = 0.0;
  for (const float tone : {0.0f, 0.5f, 0.75f, 1.0f}) {
    cv::LofiParams p = bandOnly(tone, 1.0f);
    p.tuning.bitsMin = 16.0f;
    const cv::Lofi::Law law = cv::Lofi::lawAt(tone, 1.0f, p.tuning);
    const double holdHz = static_cast<double>(law.holdRatio) * cv::kSampleRate;
    const double ratio = 0.5 * holdHz / law.lpHz;
    const double measured = heldRateHz(p);
    worstRatio = std::min(worstRatio, ratio);
    worstRate = std::max(worstRate, std::fabs(measured / holdHz - 1.0));
    ok &= ratio >= 3.0 && std::fabs(measured / holdHz - 1.0) < 0.01;
    std::printf("     tone %.2f: low-pass %.0f Hz, held rate %.0f Hz (Nyquist %.1f x the corner), at the output %.0f Hz\n",
                static_cast<double>(tone), static_cast<double>(law.lpHz), holdHz, ratio, measured);
  }
  return report("hold", ok,
                "pot top: the hold's Nyquist is >= 3 x the low-pass corner (worst %.2f x), the output's held rate is within "
                "%.2f %% of the law (<= 1)",
                worstRatio, 100.0 * worstRate);
}

// Settled knobs give the same samples for any call size, since the voice never sees the chunking.
bool testChunking() {
  const Signal in = noise(20000, 0.3f);
  cv::LofiParams p;
  p.tone = 0.3f;
  p.amount = 0.95f;
  gLofi.reset();
  const Signal ref = run(gLofi, in, p);
  size_t differ = 0;
  for (const int chunk : {1, 7, 64, 100, 20000}) {
    gLofi.reset();
    const Signal out = run(gLofi, in, p, chunk);
    for (size_t i = 0; i < in.size(); ++i) differ += out[i] != ref[i];
  }
  return report("chunking", differ == 0, "call sizes 1, 7, 64, 100, 20000 differ from 64 in %.0f samples (0)",
                static_cast<double>(differ));
}

// Off for 4 ms and on again keeps the running voice; off long enough to settle restarts it.
bool testEnableEdge() {
  const int total = kSecond;
  const Signal in = noise(static_cast<size_t>(total), 0.5f);
  cv::LofiParams on;
  on.amount = 1.0f;
  cv::LofiParams off = on;
  off.on = false;

  gLofi.reset();
  const Signal ref = run(gLofi, in, on);

  auto withGap = [&](int gapSamples) {
    const size_t start = static_cast<size_t>(kSecond / 4);
    const size_t end = start + static_cast<size_t>(gapSamples);
    gLofi.reset();
    Signal out(in.size());
    auto part = [&](size_t a, size_t b, const cv::LofiParams& q) {
      const Signal seg(in.begin() + static_cast<std::ptrdiff_t>(a), in.begin() + static_cast<std::ptrdiff_t>(b));
      const Signal o = run(gLofi, seg, q, 1);
      std::copy(o.begin(), o.end(), out.begin() + static_cast<std::ptrdiff_t>(a));
    };
    part(0, start, on);
    part(start, end, off);
    part(end, in.size(), on);
    return out;
  };
  auto tailDiffers = [&](const Signal& out) {
    size_t d = 0;
    for (size_t i = static_cast<size_t>(3 * total / 4); i < in.size(); ++i) d += out[i] != ref[i];
    return d;
  };
  const size_t quick = tailDiffers(withGap(195));
  const size_t settled = tailDiffers(withGap(24003));
  return report("enable edge", quick == 0 && settled > 0,
                "off 195 samples then on: %.0f samples differ from the uninterrupted run (0); off 24003 samples: %.0f (> 0)",
                static_cast<double>(quick), static_cast<double>(settled));
}

// TONE steps with the crush on. The band and the hold rate ramp across the block, so the output bends no
// harder just after a step than it does at either setting in steady state (knob_step_test's rule).
bool testToneStepWithCrush() {
  constexpr size_t kStepAt = 2 * kSecond, kTotal = 4 * kSecond;
  const Signal in = sine(kTotal, 1000.0, 0.25f);
  struct Step {
    float from, to;
  };
  double worst = 0.0;
  Step at{};
  for (const Step s : {Step{0.2f, 0.215f}, Step{0.5f, 0.515f}, Step{0.8f, 0.815f}, Step{0.45f, 0.75f}}) {
    for (const float amount : {0.9f, 1.0f}) {
      cv::LofiParams a, b;
      a.tone = s.from;
      b.tone = s.to;
      a.amount = b.amount = amount;
      gLofi.reset();
      Signal out = run(gLofi, Signal(in.begin(), in.begin() + static_cast<std::ptrdiff_t>(kStepAt)), a);
      const Signal tail = run(gLofi, Signal(in.begin() + static_cast<std::ptrdiff_t>(kStepAt), in.end()), b);
      out.insert(out.end(), tail.begin(), tail.end());
      const double steady = std::max(peakCurvature(out, kStepAt - kSecond / 2, kStepAt),
                                     peakCurvature(out, kTotal - kSecond / 2, kTotal));
      const double ratio = peakCurvature(out, kStepAt, kStepAt + kSecond / 10) / steady;
      std::printf("     TONE %.3f to %.3f at AMOUNT %.0f %%: x%.4f\n", static_cast<double>(s.from),
                  static_cast<double>(s.to), 100.0 * static_cast<double>(amount), ratio);
      if (ratio >= worst) {
        worst = ratio;
        at = s;
      }
    }
  }
  return report("tone step, crush", worst <= 1.05,
                "TONE steps at AMOUNT 90 and 100 %%, worst %.2f to %.2f (peak curvature x%.4f of steady state, <= 1.05)",
                static_cast<double>(at.from), static_cast<double>(at.to), worst);
}

// A NaN or an infinity in must reach the output so the chain's guard sees it. The guard then clears the
// engine, and the next clean block is finite. A quantizer that cast to int would turn the NaN into a
// finite zero with the crush on, and the guard would never fire.
bool testNonFinite() {
  // The bad sample lands after the enable fade has settled, so it reaches the output only through the voice.
  const Signal clean = sine(kSecond / 2, 1000.0, 0.25f);
  constexpr size_t kBadAt = kSecond / 3;
  const float inf = std::numeric_limits<float>::infinity();
  const float bad[] = {std::numeric_limits<float>::quiet_NaN(), inf, -inf};
  size_t silent = 0, stuck = 0, rows = 0;
  for (const float amount : {0.0f, 0.3f, 0.6f, kBandPot, 0.9f, 1.0f}) {
    for (const float value : bad) {
      cv::LofiParams p;
      p.amount = amount;
      Signal in = clean;
      in[kBadAt] = value;
      gLofi.reset();
      const Signal out = run(gLofi, in, p);
      bool reached = false;
      for (size_t i = kBadAt + 1; i < out.size(); ++i) reached = reached || !std::isfinite(out[i]);
      gLofi.reset();
      const Signal after = run(gLofi, clean, p);
      bool finite = true;
      for (const float v : after) finite = finite && std::isfinite(v);
      silent += !reached;
      stuck += !finite;
      ++rows;
    }
  }
  return report("non-finite in", silent == 0 && stuck == 0,
                "NaN, +inf and -inf at 6 AMOUNTs (%.0f cases): %.0f never reach the output (0), %.0f not finite after a reset (0)",
                static_cast<double>(rows), static_cast<double>(silent), static_cast<double>(stuck));
}

// A NaN knob reads as 0 at the boundary clamp, so it never lodges in a smoother: the output stays finite
// through a block of it and goes on playing.
bool testNanKnob() {
  const Signal in = sine(kSecond / 4, 1000.0, 0.25f);
  const size_t block = static_cast<size_t>(cv::kBlock);
  cv::LofiParams good, bad;
  bad.tone = bad.amount = std::numeric_limits<float>::quiet_NaN();
  gLofi.reset();
  Signal out = run(gLofi, Signal(in.begin(), in.begin() + static_cast<std::ptrdiff_t>(8 * block)), good);
  const Signal mid = run(gLofi, Signal(in.begin() + static_cast<std::ptrdiff_t>(8 * block),
                                       in.begin() + static_cast<std::ptrdiff_t>(9 * block)), bad);
  const Signal rest = run(gLofi, Signal(in.begin() + static_cast<std::ptrdiff_t>(9 * block), in.end()), good);
  out.insert(out.end(), mid.begin(), mid.end());
  out.insert(out.end(), rest.begin(), rest.end());
  bool finite = true;
  for (const float v : out) finite = finite && std::isfinite(v);
  const double tailRms = rmsOf(out, out.size() - block, out.size());
  return report("nan knob", finite && tailRms > 0.01, "TONE and AMOUNT NaN for one block: output finite %.0f, last block RMS %.3f (> 0.01)",
                finite ? 1.0 : 0.0, tailRms);
}

// The tanh table against std::tanh, odd bit for bit, monotone to one float step, and a NaN or an infinity
// in gives a NaN.
bool testTanhCubic() {
  constexpr float kUlpNearOne = 1.2e-7f;  // two float steps under 1, where a segment's rounding can dip once
  double worstErr = 0.0;
  bool odd = true, monotone = true;
  float prev = -2.0f;
  for (int i = -120000; i <= 120000; ++i) {
    const float x = static_cast<float>(i) * 1e-4f;
    const float y = cv::tanhCubic(x);
    worstErr = std::max(worstErr, std::fabs(static_cast<double>(y) - std::tanh(static_cast<double>(x))));
    const float yn = cv::tanhCubic(-x);
    uint32_t a, b;
    const float negated = -y;
    std::memcpy(&a, &negated, 4);
    std::memcpy(&b, &yn, 4);
    odd = odd && a == b;
    monotone = monotone && y >= prev - kUlpNearOne;
    prev = y;
  }
  const float nan = std::numeric_limits<float>::quiet_NaN();
  const float inf = std::numeric_limits<float>::infinity();
  const bool nonFinite = std::isnan(cv::tanhCubic(nan)) && std::isnan(cv::tanhCubic(inf)) && std::isnan(cv::tanhCubic(-inf));
  return report("tanh table", worstErr < 3e-6 && odd && monotone && nonFinite,
                "|x| <= 12: worst error %.2e from std::tanh (< 3e-6), odd %.0f, monotone to one float step %.0f, NaN and infinity give NaN",
                worstErr, odd ? 1.0 : 0.0, monotone ? 1.0 : 0.0);
}

using testutil::readWav;

// RMS out over RMS in, in dB, of the clip. 0.4 s of silence ahead of it lets the enable fade settle.
double levelDb(const Signal& clip, float tone, float amount) {
  constexpr size_t kPad = kSecond * 2 / 5;
  Signal in(kPad, 0.0f);
  in.insert(in.end(), clip.begin(), clip.end());
  cv::LofiParams p;
  p.tone = tone;
  p.amount = amount;
  gLofi.reset();
  const Signal out = run(gLofi, in, p);
  return 20.0 * std::log10(rmsOf(out, kPad, out.size()) / rmsOf(in, kPad, in.size()));
}

// Vocal-like program: ten harmonics at 1/k on a 180 Hz tone with vibrato and a 2 Hz envelope.
Signal program() {
  const int n = 3 * kSecond;
  Signal x(static_cast<size_t>(n));
  const Signal hiss = noise(x.size(), 0.02f);
  double phase = 0.0;
  for (size_t i = 0; i < x.size(); ++i) {
    const double t = static_cast<double>(i) / cv::kSampleRate;
    phase += 2.0 * kPi * 180.0 * std::pow(2.0, 20.0 * std::sin(2.0 * kPi * 5.0 * t) / 1200.0) / cv::kSampleRate;
    double v = 0.0;
    for (int k = 1; k <= 10; ++k) v += std::sin(k * phase) / k;
    x[i] = static_cast<float>(v * 0.5 * (1.0 - std::cos(2.0 * kPi * 2.0 * t))) + hiss[i];
  }
  const double g = std::pow(10.0, -18.0 / 20.0) / rmsOf(x, 0, x.size());
  for (float& v : x) v = static_cast<float>(v * g);
  return x;
}

// Past the band's closing the drive and the makeup settle, so a program holds one level to the pot's top.
// (The table is fitted on the clips, so the synthetic program only has to stay flat there.)
bool testProgramLevel() {
  const Signal prog = program();
  double lo = 1e9, hi = -1e9;
  for (int k = 0; k <= 8; ++k) {
    const double db = levelDb(prog, 0.5f, kBandPot + (1.0f - kBandPot) * static_cast<float>(k) / 8.0f);
    lo = std::min(lo, db);
    hi = std::max(hi, db);
  }
  return report("level, program", hi - lo <= 0.3, "pot 83 to 100 %% on the synthetic program: %.2f dB spread (<= 0.3)",
                hi - lo);
}

// Each of Adam's clips over the whole reach of both knobs, 5 % steps, holds within 1 dB of unity.
bool testClipLevel(const char* name) {
  const Signal clip = readWav(std::string(CUBEVOX_SAMPLES_DIR) + "/" + name + ".wav");
  if (clip.empty()) {
    // samples/ is git-ignored, so CI has none and the row skips. With the folder there, a missing or
    // empty clip fails it.
    std::error_code ec;
    if (std::filesystem::is_directory(CUBEVOX_SAMPLES_DIR, ec)) {
      std::printf("FAIL level, %s  the clip is missing or empty\n", name);
      return false;
    }
    std::printf("SKIP level, %s  samples/ is not in this checkout\n", name);
    return true;
  }
  double worst = 0.0;
  for (int t = 0; t <= 20; ++t) {
    for (int a = 0; a <= 20; ++a) {
      const double db = levelDb(clip, static_cast<float>(t) / 20.0f, static_cast<float>(a) / 20.0f);
      worst = std::max(worst, std::fabs(db));
    }
  }
  const double def = levelDb(clip, 0.5f, 0.5f);
  char label[96];
  std::snprintf(label, sizeof label, "level, %s", name);
  return report(label, worst <= 1.0,
                "TONE x AMOUNT grid in 5 %% steps: worst %.2f dB from unity (<= 1), default %+.2f dB", worst, def);
}

bool testLevel() {
  bool ok = testProgramLevel();
  for (const char* name : clips::kNames) ok &= testClipLevel(name);
  return ok;
}

bool testNoAlloc() {
  const Signal in = noise(kSecond / 2, 0.3f);
  cv::LofiParams p;
  p.amount = 0.9f;
  gLofi.reset();
  run(gLofi, in, p);
  p.tone = 0.1f;
  p.amount = 0.2f;
  run(gLofi, in, p, 17);
  return report("no allocation", true, "process() ran with knob moves and odd call sizes without allocating");
}

}  // namespace

int main() {
  bool ok = true;
  ok &= testSettledOff();
  ok &= testSilence();
  ok &= testBandCorners();
  ok &= testHonk();
  ok &= testCrushLaw();
  ok &= testCrush();
  ok &= testHold();
  ok &= testChunking();
  ok &= testEnableEdge();
  ok &= testToneStepWithCrush();
  ok &= testNonFinite();
  ok &= testNanKnob();
  ok &= testTanhCubic();
  ok &= testLevel();
  ok &= testNoAlloc();
  return ok ? 0 : 1;
}
