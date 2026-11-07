// Spring reverb checks: three echo spacings, the sweep and its growth, the half-band pair round the
// 24 kHz loops, odd chunk sizes, level across TIME, stability at the shipped tuning and at the clamp
// ceilings, the disperser against a section-by-section reference, the return low-pass, Reverb
// passthrough, engine switches, the size bound and no-alloc. No window, no audio device.

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <vector>

#include "engine/reverb.h"
#include "engine/spring.h"

#include "alloc_guard.h"
#include "signal_util.h"

namespace {

namespace sd = cv::spring_detail;

constexpr double kPi = 3.14159265358979323846;
constexpr double kMs = cv::kSampleRate / 1000.0;
using Signal = std::vector<float>;

bool report(const char* name, bool pass, const char* fmt, ...) {
  std::printf("%s %-18s ", pass ? "PASS" : "FAIL", name);
  va_list args;
  va_start(args, fmt);
  std::vfprintf(stdout, fmt, args);
  va_end(args);
  std::printf("\n");
  return pass;
}

// Plain loops: no diffusion, no wobble, filters wide open.
cv::SpringParams plain() {
  cv::SpringParams p;
  p.tuning.diffuse = 0.0f;
  p.tuning.wobbleDepth = 0.0f;
  p.tuning.trebleLossHz = cv::kFilterMaxHz;
  p.tuning.bassCutHz = cv::kFilterMinHz;
  p.tuning.returnLpHz = cv::kFilterMaxHz;
  return p;
}

Signal run(const Signal& in, const cv::SpringParams& p) {
  auto spring = std::make_unique<cv::Spring>();
  Signal out(in.size());
  for (size_t i = 0; i + cv::kBlock <= in.size(); i += cv::kBlock)
    spring->process(&in[i], &out[i], cv::kBlock, p);
  return out;
}

Signal impulse(int n) { return testutil::impulse(static_cast<size_t>(n)); }
Signal noise(int n, float amp) { return testutil::noise(static_cast<size_t>(n), amp, 0x12345678u); }
double rms(const Signal& x, size_t from, size_t to) { return testutil::rmsOf(x, from, to); }

std::complex<double> dft(const Signal& x, size_t from, size_t to, double hz) {
  const double w = 2.0 * kPi * hz / cv::kSampleRate;
  std::complex<double> acc = 0.0;
  for (size_t i = from; i < to; ++i)
    acc += static_cast<double>(x[i]) * std::polar(1.0, -w * static_cast<double>(i - from));
  return acc;
}

// Group delay in samples of x[from..to) at hz, from the phase slope.
double groupDelay(const Signal& x, size_t from, size_t to, double hz) {
  const double dHz = 2.0;
  const double dw = 2.0 * kPi * dHz / cv::kSampleRate;
  const double dphi = std::arg(dft(x, from, to, hz + dHz) / dft(x, from, to, hz - dHz));
  return static_cast<double>(from) - dphi / (2.0 * dw);
}

// 1. With no diffusion or sweep, the first three echoes land at springMs1, springMs2 and springMs3,
// to within two samples.
bool echoSpacing() {
  constexpr double kTolMs = 2.0 / kMs;
  cv::SpringParams p = plain();
  p.tuning.chirpSections = 0;
  const Signal y = run(impulse(4800), p);
  const auto peakIn = [&](double fromMs, double toMs) {
    size_t best = static_cast<size_t>(fromMs * kMs);
    for (size_t i = best; i < static_cast<size_t>(toMs * kMs); ++i)
      if (std::fabs(y[i]) > std::fabs(y[best])) best = i;
    return static_cast<double>(best) / kMs;
  };
  const double s1 = p.tuning.springMs1, s2 = p.tuning.springMs2, s3 = p.tuning.springMs3;
  const double e1 = peakIn(1.0, (s1 + s2) / 2.0);
  const double e2 = peakIn((s1 + s2) / 2.0, (s2 + s3) / 2.0);
  const double e3 = peakIn((s2 + s3) / 2.0, (s3 + 2.0 * s1) / 2.0);
  const bool ok = std::fabs(e1 - s1) <= kTolMs && std::fabs(e2 - s2) <= kTolMs && std::fabs(e3 - s3) <= kTolMs;
  return report("echo spacing", ok, "echoes at %.2f / %.2f / %.2f ms (want %.0f / %.0f / %.0f, within %.3f)", e1, e2,
                e3, s1, s2, s3, kTolMs);
}

// 2. Each echo sweeps up, so 3.8 kHz arrives after 300 Hz. Echo 2 has passed the disperser twice,
// so its lag is about twice echo 1's. All three springs sit at the shortest line's longest spacing,
// and eight sections keep each echo's sweep inside that spacing.
bool sweep() {
  constexpr int kSections = 8;
  constexpr double kMinLagMs = 3.0, kGrowthLo = 1.9, kGrowthHi = 2.1;
  const float spacing = std::floor(sd::kMaxMs1);
  cv::SpringParams p = plain();
  p.tuning.springMs1 = p.tuning.springMs2 = p.tuning.springMs3 = spacing;
  p.tuning.chirpSections = kSections;
  p.decay = 1.0f;
  const double s = static_cast<double>(spacing);
  const Signal y = run(impulse(static_cast<int>(4.0 * s * kMs)), p);
  const auto lagMs = [&](int k) {
    const size_t from = static_cast<size_t>((k - 0.25) * s * kMs);
    const size_t to = static_cast<size_t>((k + 0.75) * s * kMs);
    return (groupDelay(y, from, to, 3800.0) - groupDelay(y, from, to, 300.0)) / kMs;
  };
  const double lag1 = lagMs(1), lag2 = lagMs(2);
  const double growth = lag2 / lag1;
  const bool ok = lag1 > kMinLagMs && growth > kGrowthLo && growth < kGrowthHi;
  return report("sweep, grows", ok, "3.8 kHz minus 300 Hz arrival %+.1f ms (echo 1), %+.1f ms (echo 2), growth %.2f",
                lag1, lag2, growth);
}

// 2b. The half-band pair. The first echo of the plain loop passes no diffuser or disperser, so its
// spectrum is the decimating and interpolating filters' response with the loop's two wide one-poles,
// which take 1.6 dB at 8 kHz. Against 1 kHz it reads -1.5 dB at 8 kHz, -3.2 dB at 10 kHz, -8 dB at
// 12 kHz and -48 dB at 16 kHz. Above 10 kHz the fold of the input's content round 12 kHz is part of it.
bool halfBand() {
  cv::SpringParams p = plain();
  p.tuning.chirpSections = 0;
  const Signal y = run(impulse(3000), p);
  const size_t from = static_cast<size_t>(30.0 * kMs), to = static_cast<size_t>(41.0 * kMs);
  const auto levelDb = [&](double hz) { return 20.0 * std::log10(std::abs(dft(y, from, to, hz))); };
  const double ref = levelDb(1000.0);
  const double d8 = levelDb(8000.0) - ref, d10 = levelDb(10000.0) - ref, d12 = levelDb(12000.0) - ref,
               d16 = levelDb(16000.0) - ref;
  const bool ok = d8 > -2.1 && d8 < -1.0 && d10 > -4.2 && d10 < -2.4 && d12 < -6.0 && d16 < -35.0;
  return report("half-band", ok,
                "echo 1 against 1 kHz: %+.2f dB at 8 kHz (-2.1..-1.0), %+.2f at 10 kHz (-4.2..-2.4), %+.1f at 12 kHz (<-6), "
                "%+.1f at 16 kHz (<-35)",
                d8, d10, d12, d16);
}

// 2c. Odd chunk sizes, which cut the even and odd input samples at any place, give the output of 64-sample
// chunks to the last bit when nothing is smoothed or wobbling.
bool chunkSizes() {
  const cv::SpringParams p = plain();
  const Signal x = noise(cv::kSampleRate, 0.25f);
  const auto runChunks = [&](size_t chunk) {
    auto spring = std::make_unique<cv::Spring>();
    Signal out(x.size());
    for (size_t i = 0; i < x.size(); i += chunk) {
      const size_t n = std::min(chunk, x.size() - i);
      spring->process(&x[i], &out[i], static_cast<int>(n), p);
    }
    return out;
  };
  const Signal ref = runChunks(cv::kBlock);
  size_t worstChunk = 0;
  double worst = 0.0;
  for (const size_t chunk : {size_t{1}, size_t{2}, size_t{7}, size_t{63}, size_t{65}, size_t{100}, size_t{1000}}) {
    const Signal out = runChunks(chunk);
    double d = 0.0;
    for (size_t i = 0; i < out.size(); ++i) d = std::fmax(d, std::fabs(static_cast<double>(out[i]) - ref[i]));
    if (d > worst) {
      worst = d;
      worstChunk = chunk;
    }
  }
  return report("chunk sizes", worst == 0.0, "chunks of 1, 2, 7, 63, 65, 100 and 1000 against 64: worst difference %.3g (chunk %zu)",
                worst, worstChunk);
}

// 3. The input scales by sqrt((1 - g^2) / g), so the tail's level holds as TIME moves.
bool levelAcrossTime() {
  constexpr double kMaxDb = 0.45;
  const Signal x = noise(4 * cv::kSampleRate, 0.25f);
  cv::SpringParams lo, hi;
  lo.decay = 0.0f;
  hi.decay = 1.0f;
  const size_t from = static_cast<size_t>(2 * cv::kSampleRate);
  const double a = rms(run(x, lo), from, x.size()), b = rms(run(x, hi), from, x.size());
  const double db = 20.0 * std::log10(b / a);
  return report("level TIME", std::fabs(db) < kMaxDb, "TIME 1 vs 0: %+.2f dB (want within %.1f)", db, kMaxDb);
}

// 4. Full TIME on 20 s of loud noise stays finite and dies after the input stops.
bool stable() {
  constexpr float kMaxPeak = 3.8f;
  constexpr double kMaxTail = 1e-25;
  const int on = 20 * cv::kSampleRate;
  Signal x = noise(on + 20 * cv::kSampleRate, 1.0f);
  for (size_t i = static_cast<size_t>(on); i < x.size(); ++i) x[i] = 0.0f;
  cv::SpringParams p;
  p.decay = 1.0f;
  p.tuning.restLevel = 0.0f;  // the loops decay on their own; a rest would hide the tail
  const Signal y = run(x, p);
  float peak = 0.0f;
  bool finite = true;
  for (float v : y) {
    finite = finite && std::isfinite(v);
    peak = std::fmax(peak, std::fabs(v));
  }
  const double tail = rms(y, y.size() - static_cast<size_t>(cv::kSampleRate / 10), y.size());
  const bool ok = finite && peak < kMaxPeak && tail < kMaxTail;
  return report("stability", ok, "peak %.2f over 20 s (<%.1f), tail RMS %.2g 20 s after input stops (<%.0e)",
                static_cast<double>(peak), static_cast<double>(kMaxPeak), tail, kMaxTail);
}

// 5. The same with every tuning value at the end the engine clamps it to: feedback 0.97, the widest
// wobble at its fastest rate, the largest disperser and diffuser coefficients. 2 s of full-scale
// noise, then 58 s of silence.
bool stableAtCeilings() {
  constexpr float kMaxPeak = 2.4f;
  constexpr double kMaxTail = 3.5e-20;
  const int on = 2 * cv::kSampleRate;
  Signal x = noise(60 * cv::kSampleRate, 1.0f);
  for (size_t i = static_cast<size_t>(on); i < x.size(); ++i) x[i] = 0.0f;
  cv::SpringParams p;
  p.decay = 1.0f;
  p.tuning.feedbackHi = sd::kFeedbackMax;
  p.tuning.wobbleDepth = sd::kMaxWobble;
  p.tuning.wobbleRateHz = sd::kMaxWobbleHz;
  p.tuning.chirpA = 0.95f;
  p.tuning.diffuse = 0.9f;
  p.tuning.restLevel = 0.0f;
  const Signal y = run(x, p);
  float peak = 0.0f;
  bool finite = true;
  for (float v : y) {
    finite = finite && std::isfinite(v);
    peak = std::fmax(peak, std::fabs(v));
  }
  const double tail = rms(y, y.size() - static_cast<size_t>(cv::kSampleRate / 10), y.size());
  const bool ok = finite && peak < kMaxPeak && tail < kMaxTail;
  return report("stability, clamps", ok, "peak %.2f (<%.1f), tail RMS %.2g at 59 s (<%.0e)", static_cast<double>(peak),
                static_cast<double>(kMaxPeak), tail, kMaxTail);
}

// 6. The return low-pass sits on the summed output: the impulse response at the default returnLpHz
// over the one at the widest corner matches the ratio of the two one-poles at 1 and 8 kHz. The loops
// run at 24 kHz, so the one-poles do, and 12 kHz is their Nyquist.
bool returnLowPass() {
  constexpr double kTolDb = 0.01;
  constexpr double kLoopHz = sd::kLoopRate;
  cv::SpringParams wide = plain();
  wide.decay = 0.0f;
  cv::SpringParams lp = wide;
  lp.tuning.returnLpHz = cv::SpringTuning{}.returnLpHz;
  const Signal x = impulse(2 * cv::kSampleRate);
  const Signal yw = run(x, wide), yl = run(x, lp);
  const auto onePoleDb = [](double cornerHz, double hz) {
    const double c = cv::onePoleCoefAt(static_cast<float>(cornerHz), static_cast<float>(kLoopHz));
    const std::complex<double> h = c / (1.0 - (1.0 - c) * std::polar(1.0, -2.0 * kPi * hz / kLoopHz));
    return 20.0 * std::log10(std::abs(h));
  };
  bool ok = true;
  double got[2] = {0.0, 0.0}, want[2] = {0.0, 0.0}, worst = 0.0;
  const double hz[2] = {1000.0, 8000.0};
  for (int i = 0; i < 2; ++i) {
    got[i] = 20.0 * std::log10(std::abs(dft(yl, 0, x.size(), hz[i])) / std::abs(dft(yw, 0, x.size(), hz[i])));
    want[i] = onePoleDb(lp.tuning.returnLpHz, hz[i]) - onePoleDb(wide.tuning.returnLpHz, hz[i]);
    worst = std::fmax(worst, std::fabs(got[i] - want[i]));
    ok = ok && std::fabs(got[i] - want[i]) < kTolDb;
  }
  ok = ok && want[1] < -3.0;
  return report("return low-pass", ok,
                "%.0f Hz corner: %+.2f dB at 1 kHz (want %+.2f), %+.2f dB at 8 kHz (want %+.2f), worst difference %.3f dB",
                static_cast<double>(lp.tuning.returnLpHz), got[0], want[0], got[1], want[1], worst);
}

// 7. Reverb engine Spring: settled off is a bit-exact copy on every sample; process() never
// allocates.
bool reverbBlock() {
  auto rv = std::make_unique<cv::Reverb>();
  cv::ReverbParams p;
  p.engine = cv::kReverbSpring;
  p.wet = 1.0f;
  const Signal x = noise(2 * cv::kSampleRate, 0.5f);
  Signal y(x.size());
  const size_t off = x.size() / 2;
  gInProcess = true;
  for (size_t i = 0; i < off; i += cv::kBlock) rv->process(&x[i], &y[i], cv::kBlock, p);
  p.on = false;
  for (size_t i = off; i + cv::kBlock <= x.size(); i += cv::kBlock) rv->process(&x[i], &y[i], cv::kBlock, p);
  gInProcess = false;
  // The off fade snaps to 0 about 280 ms after the switch; from 500 ms on every sample must match.
  size_t settled = x.size();
  while (settled > off && y[settled - 1] == x[settled - 1]) --settled;
  size_t mismatches = 0;
  for (size_t i = off + static_cast<size_t>(cv::kSampleRate / 2); i < x.size(); ++i)
    mismatches += y[i] != x[i];
  const double settledMs = static_cast<double>(settled - off) / kMs;
  return report("reverb block", mismatches == 0,
                "on=false out == in on every sample from %.1f ms after the switch, no allocation", settledMs);
}

// A section-by-section reference for Dispersers::run, one chain per spring.
struct DisperserRef {
  std::vector<float> state = std::vector<float>(3 * sd::kMaxSections * sd::kStretch, 0.0f);
  int pos = 0;

  float& at(int chain, int section) {
    return state[static_cast<size_t>((chain * sd::kMaxSections + section) * sd::kStretch + pos)];
  }

  void run(float x[3], int sections, float a) {
    for (int c = 0; c < 3; ++c) {
      float y = x[c];
      for (int u = 0; u < sections; ++u) {
        float& z = at(c, u);
        const float o = z + a * y;
        z = y - a * o;
        y = o;
      }
      x[c] = y;
    }
    if (++pos >= sd::kStretch) pos = 0;
  }
};

// 8. The disperser matches the reference at section counts that exercise the paired loop, the odd
// tail and the cleared tail, for both signs of the coefficient and for zero.
bool dispersers() {
  constexpr double kMaxDiff = 1e-5;
  constexpr int kSamples = 20000;
  uint32_t seed = 12345u;
  const auto rnd = [&seed]() {
    seed = seed * 1664525u + 1013904223u;
    return static_cast<float>(seed >> 8) / 8388608.0f - 1.0f;
  };
  double worst = 0.0;
  for (const int sections : {0, 1, 2, 3, 4, 5, 17, 31, 32}) {
    for (const float a : {0.8f, -0.8f, 0.0f}) {
      auto d = std::make_unique<sd::Dispersers>();
      DisperserRef ref;
      for (int i = 0; i < kSamples; ++i) {
        float x1 = rnd(), x2 = rnd(), x3 = rnd();
        float r[3] = {x1, x2, x3};
        d->run(x1, x2, x3, sections, a);
        ref.run(r, sections, a);
        worst = std::fmax(worst, std::fabs(static_cast<double>(x1) - r[0]));
        worst = std::fmax(worst, std::fabs(static_cast<double>(x2) - r[1]));
        worst = std::fmax(worst, std::fabs(static_cast<double>(x3) - r[2]));
      }
    }
  }
  return report("dispersers", worst <= kMaxDiff, "worst difference from the reference %.2g over 0..32 sections (<= %.0e)",
                worst, kMaxDiff);
}

// One segment of a Reverb run: the block's parameters and how long they hold.
Signal feed(cv::Reverb& rv, const Signal& in, const cv::ReverbParams& p) {
  Signal out(in.size());
  for (size_t i = 0; i + cv::kBlock <= in.size(); i += cv::kBlock) rv.process(&in[i], &out[i], cv::kBlock, p);
  return out;
}

float peakOf(const Signal& x) {
  float peak = 0.0f;
  for (float v : x) peak = std::fmax(peak, std::fabs(v));
  return peak;
}

// 9. An engine that is left, and a block that is switched off and on, restart clear. Silence after
// each hop is exactly zero. An off and on inside 100 ms keeps the running tail.
bool engineSwitches() {
  const Signal loud = noise(cv::kSampleRate, 0.5f);
  const Signal quiet(static_cast<size_t>(cv::kSampleRate), 0.0f);
  const Signal gap(static_cast<size_t>(cv::kSampleRate / 20), 0.0f);
  cv::ReverbParams spring, chasm;
  spring.wet = chasm.wet = 1.0f;
  spring.engine = cv::kReverbSpring;
  chasm.engine = cv::kReverbChasm;

  float hops[5] = {};
  {
    auto rv = std::make_unique<cv::Reverb>();
    feed(*rv, loud, spring);
    hops[0] = peakOf(feed(*rv, quiet, chasm));  // Spring to Chasm
  }
  {
    auto rv = std::make_unique<cv::Reverb>();
    feed(*rv, loud, chasm);
    hops[1] = peakOf(feed(*rv, quiet, spring));  // Chasm to Spring
  }
  {
    auto rv = std::make_unique<cv::Reverb>();
    feed(*rv, loud, spring);
    feed(*rv, gap, chasm);
    hops[2] = peakOf(feed(*rv, quiet, spring));  // and back
  }
  {
    auto rv = std::make_unique<cv::Reverb>();
    feed(*rv, loud, chasm);
    feed(*rv, gap, spring);
    hops[3] = peakOf(feed(*rv, quiet, chasm));  // and back
  }
  {
    auto rv = std::make_unique<cv::Reverb>();
    cv::ReverbParams off = spring;
    off.on = false;
    feed(*rv, loud, spring);
    feed(*rv, quiet, off);  // long enough to settle off
    hops[4] = peakOf(feed(*rv, quiet, spring));  // settled off, then on
  }
  bool clean = true;
  for (const float h : hops) clean = clean && h == 0.0f;

  auto rv = std::make_unique<cv::Reverb>();
  cv::ReverbParams off = spring;
  off.on = false;
  feed(*rv, loud, spring);
  feed(*rv, gap, off);
  const float kept = peakOf(feed(*rv, quiet, spring));
  constexpr float kMinKept = 0.15f;
  return report("engine switches", clean && kept > kMinKept,
                "silence after S>C %.2g, C>S %.2g, S>C>S %.2g, C>S>C %.2g, off>on %.2g (all 0); 50 ms off then on keeps %.3g",
                static_cast<double>(hops[0]), static_cast<double>(hops[1]), static_cast<double>(hops[2]),
                static_cast<double>(hops[3]), static_cast<double>(hops[4]), static_cast<double>(kept));
}

// 10. Tuning values far outside their ranges stay finite, because the engine clamps each one.
bool extremeTuning() {
  constexpr float kBigPeak = 100.0f;
  using Set = void (*)(cv::SpringTuning&);
  const Set cases[] = {
      [](cv::SpringTuning& t) { t.wetDb = 1000.0f; },         [](cv::SpringTuning& t) { t.wetDb = -1000.0f; },
      [](cv::SpringTuning& t) { t.feedbackHi = 5.0f; },       [](cv::SpringTuning& t) { t.feedbackLo = -1.0f; },
      [](cv::SpringTuning& t) { t.springMs1 = -50.0f; },      [](cv::SpringTuning& t) { t.springMs3 = 1000.0f; },
      [](cv::SpringTuning& t) { t.chirpSections = -5; },      [](cv::SpringTuning& t) { t.chirpSections = 100; },
      [](cv::SpringTuning& t) { t.chirpA = -3.0f; },          [](cv::SpringTuning& t) { t.chirpA = 5.0f; },
      [](cv::SpringTuning& t) { t.diffuse = 5.0f; },          [](cv::SpringTuning& t) { t.trebleLossHz = 0.0f; },
      [](cv::SpringTuning& t) { t.bassCutHz = 1e9f; },        [](cv::SpringTuning& t) { t.wobbleDepth = 1000.0f; },
      [](cv::SpringTuning& t) { t.wobbleRateHz = 1000.0f; },
  };
  const Signal x = noise(cv::kSampleRate, 0.5f);
  int bad = 0;
  float worst = 0.0f;
  for (const Set set : cases) {
    cv::SpringParams p;
    p.decay = 1.0f;
    set(p.tuning);
    for (const float v : run(x, p)) {
      if (!std::isfinite(v) || std::fabs(v) > kBigPeak) {
        ++bad;
        break;
      }
      worst = std::fmax(worst, std::fabs(v));
    }
  }
  return report("extreme tuning", bad == 0, "%zu out-of-range values stay finite, largest output %.1f (< %.0f)",
                sizeof cases / sizeof cases[0], static_cast<double>(worst), static_cast<double>(kBigPeak));
}

// 11. A silent input lets the springs rest once every loop has decayed below restLevel, and the first
// block with sound ends the rest. The output of a run that rests stays within a few millionths of a
// run that never does, across both edges. A tail that is still audible keeps the springs running at
// MIX 0 as at any other MIX.
bool rests() {
  constexpr double kMaxDiff = 1.6e-6;
  constexpr double kMaxEntryPeak = 1.2e-6;  // the never-resting run's output just ahead of the rest
  constexpr int kSr = cv::kSampleRate;
  Signal x = noise(16 * kSr, 0.5f);
  const size_t burst1 = static_cast<size_t>(kSr), burst2 = static_cast<size_t>(8 * kSr);
  for (size_t i = 0; i < x.size(); ++i)
    if (i >= burst1 && (i < burst2 || i >= burst2 + static_cast<size_t>(kSr))) x[i] = 0.0f;
  cv::SpringParams on, never;
  on.decay = never.decay = 1.0f;
  never.tuning.restLevel = 0.0f;
  auto a = std::make_unique<cv::Spring>();
  auto b = std::make_unique<cv::Spring>();
  Signal ya(x.size()), yb(x.size());
  size_t firstRest = 0, restedBlocks = 0, ending = 0;
  bool awakeAtOnce = false;
  for (size_t i = 0; i + cv::kBlock <= x.size(); i += cv::kBlock) {
    a->process(&x[i], &ya[i], cv::kBlock, on);
    b->process(&x[i], &yb[i], cv::kBlock, never);
    if (a->resting()) {
      if (firstRest == 0) firstRest = i;
      ++restedBlocks;
      if (i < burst2) ending = i;
    }
    if (i == burst2) awakeAtOnce = !a->resting();
  }
  double diff = 0.0;
  for (size_t i = 0; i < x.size(); ++i) diff = std::fmax(diff, std::fabs(static_cast<double>(ya[i]) - yb[i]));
  double entryPeak = 0.0;
  for (size_t i = firstRest - static_cast<size_t>(kSr / 100); i < firstRest; ++i)
    entryPeak = std::fmax(entryPeak, std::fabs(static_cast<double>(yb[i])));

  // MIX 0 with the tail still audible half a second after the sound stops.
  auto rv = std::make_unique<cv::Reverb>();
  cv::ReverbParams p;
  p.wet = 0.0f;
  p.spring.decay = 1.0f;
  feed(*rv, noise(kSr, 0.5f), p);
  feed(*rv, Signal(static_cast<size_t>(kSr / 2), 0.0f), p);
  const bool runsAtMixZero = !rv->spring().resting();

  const double restS = static_cast<double>(firstRest) / kSr - 1.0;
  const bool ok = firstRest > 0 && ending > firstRest && awakeAtOnce && runsAtMixZero && diff < kMaxDiff &&
                  entryPeak < kMaxEntryPeak;
  return report("rest", ok,
                "rests %.2f s after the sound stops and wakes in the block sound returns; %zu of %zu blocks rest; "
                "worst difference from a run that never rests %.2g (<%.0e), output ahead of the rest %.2g (<%.0e); "
                "MIX 0 tail runs: %s",
                restS, restedBlocks, x.size() / cv::kBlock, diff, kMaxDiff, entryPeak, kMaxEntryPeak,
                runsAtMixZero ? "yes" : "no");
}

// 12. The Spring object stays inside its RAM bound. The loops run at 24 kHz and each line holds its
// spring's default read, so the three lines take 11 KB.
bool sizeBound() {
  constexpr size_t kMaxBytes = 16384;
  return report("size", sizeof(cv::Spring) <= kMaxBytes, "sizeof(cv::Spring) = %zu bytes (<= %zu)", sizeof(cv::Spring),
                kMaxBytes);
}

}  // namespace

int main() {
  bool ok = true;
  ok &= echoSpacing();
  ok &= sweep();
  ok &= halfBand();
  ok &= chunkSizes();
  ok &= levelAcrossTime();
  ok &= stable();
  ok &= stableAtCeilings();
  ok &= returnLowPass();
  ok &= reverbBlock();
  ok &= dispersers();
  ok &= engineSwitches();
  ok &= extremeTuning();
  ok &= rests();
  ok &= sizeBound();
  std::printf("%s spring\n", ok ? "PASS" : "FAIL");
  return ok ? 0 : 1;
}
