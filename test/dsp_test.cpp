// The shared helpers of engine/dsp.h, engine/reverb_parts.h and engine/scale.h. DcBlocker: the series
// coefficient against the exact one, a 0.5 DC step settling under -60 dBFS within 200 ms, a 100 Hz tone
// passing within 0.05 dB, and clear(). Then the clamp and gain laws, OnePole, the RBJ setters against
// the double cookbook, the Catmull-Rom forms, the ring reads at their bounds and for non-finite delays,
// the reverbs' allpass, delay, sine table and knob law, and the key tables. No window, no audio device.

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstdio>
#include <initializer_list>
#include <limits>

#include "engine/dsp.h"
#include "engine/reverb_parts.h"
#include "engine/scale.h"

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kStep = 0.5;
constexpr double kSettleDbfs = -60.0;
constexpr double kSettleMs = 200.0;
constexpr double kPassHz = 100.0;
constexpr double kPassDb = 0.05;

bool report(const char* name, bool ok, const char* fmt, double a, double b = 0.0, double c = 0.0) {
  char detail[200];
  std::snprintf(detail, sizeof detail, fmt, a, b, c);
  std::printf("%s %s  %s\n", ok ? "PASS" : "FAIL", name, detail);
  return ok;
}

double exactCoef(double hz) { return 1.0 - std::exp(-2.0 * kPi * hz / cv::kSampleRate); }

// The series stays within float rounding of 1 - exp(-x) at the 5 Hz corner and at the 100 Hz end of
// the range it claims.
bool testCoefficient() {
  bool ok = true;
  for (const double hz : {5.0, 20.0, 100.0}) {
    const double got = cv::dcBlockCoef(static_cast<float>(hz));
    const double want = exactCoef(hz);
    const double rel = std::fabs(got - want) / want;
    ok &= report("dc coefficient", rel < 2.5e-7, "%.0f Hz: coefficient %.9g, relative error %.2g (< 2.5e-7)", hz, got, rel);
  }
  const cv::DcBlocker fresh;
  const bool at5Hz = cv::kWetDcBlockHz == 5.0f && fresh.a == cv::dcBlockCoef(5.0f);
  return report("dc coefficient", at5Hz, "a new blocker sits at %.0f Hz (a = %.9g)", cv::kWetDcBlockHz, fresh.a) && ok;
}

// A 0.5 step in. The output peaks on the first sample and decays at the corner's time constant.
bool testStepSettles() {
  cv::DcBlocker dc;
  const int at200 = static_cast<int>(kSettleMs * cv::kSampleRate / 1000.0);
  const double limit = std::pow(10.0, kSettleDbfs / 20.0);
  double peak = 0.0;
  int firstUnder = -1;
  double worstAfter = 0.0;
  for (int i = 0; i < 4 * cv::kSampleRate; ++i) {
    const double y = std::fabs(static_cast<double>(dc.run(static_cast<float>(kStep))));
    if (y > peak) peak = y;
    if (firstUnder < 0 && y < limit) firstUnder = i;
    if (i >= at200 && y > worstAfter) worstAfter = y;
  }
  const double firstMs = 1000.0 * firstUnder / cv::kSampleRate;
  const double worstDb = 20.0 * std::log10(worstAfter);
  bool ok = report("dc step", firstUnder >= 0 && firstMs <= kSettleMs, "0.5 step under -60 dBFS after %.1f ms (<= 200)",
                   firstMs);
  ok &= report("dc step", worstDb < kSettleDbfs, "worst output from 200 ms on is %.2f dBFS (< -60)", worstDb);
  ok &= report("dc step", peak > 0.4999 * kStep && peak <= kStep, "peak %.4f on a 0.5 step", peak);
  return ok;
}

// A 100 Hz tone through the blocker: amplitude over the last whole periods against the amplitude in.
bool testTonePasses() {
  cv::DcBlocker dc;
  const int periods = 100;
  const int samples = static_cast<int>(periods * cv::kSampleRate / kPassHz);
  double peak = 0.0;
  for (int i = 0; i < samples; ++i) {
    const double x = 0.5 * std::sin(2.0 * kPi * kPassHz * i / cv::kSampleRate);
    const double y = dc.run(static_cast<float>(x));
    if (i > samples / 2 && std::fabs(y) > peak) peak = std::fabs(y);
  }
  const double db = 20.0 * std::log10(peak / 0.5);
  // |H| of y = x - lowpass(x): H = (1 - a)(1 - z^-1) / (1 - (1 - a) z^-1) at z = exp(j w).
  const double a = exactCoef(cv::kWetDcBlockHz);
  const std::complex<double> zInv = std::polar(1.0, -2.0 * kPi * kPassHz / cv::kSampleRate);
  const double want = 20.0 * std::log10(std::abs((1.0 - a) * (1.0 - zInv) / (1.0 - (1.0 - a) * zInv)));
  bool ok = report("dc tone", std::fabs(db) < kPassDb, "100 Hz passes at %.4f dB (within 0.05)", db);
  ok &= report("dc tone", std::fabs(db - want) < 0.002, "measured %.4f dB, filter response %.4f dB", db, want);
  return ok;
}

// A long DC input leaves nothing, and clear() empties the state.
bool testClear() {
  cv::DcBlocker dc;
  float y = 1.0f;
  for (int i = 0; i < 10 * cv::kSampleRate; ++i) y = dc.run(0.3f);
  // The float state stops moving once a * (x - y) falls under half an ulp of y, about 2e-5 for 0.3.
  bool ok = report("dc settle", std::fabs(y) < 1e-4, "ten seconds of DC leave %.2g (< 1e-4)", std::fabs(y));
  dc.clear();
  ok &= report("dc clear", dc.y == 0.0f && dc.run(0.0f) == 0.0f, "state %.1f after clear()", dc.y);
  return ok;
}

namespace reverb_parts = cv::reverb_parts;

constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();
constexpr float kInf = std::numeric_limits<float>::infinity();

// clampf and the dB and taper laws. A NaN reads as the lower bound, an infinity as the nearer bound.
bool testLaws() {
  const bool clamp = cv::clampf(kNaN, 2.0f, 5.0f) == 2.0f && cv::clampf(kInf, 2.0f, 5.0f) == 5.0f &&
                     cv::clampf(-kInf, 2.0f, 5.0f) == 2.0f && cv::clampf(3.5f, 2.0f, 5.0f) == 3.5f &&
                     cv::clampf(1.0f, 2.0f, 5.0f) == 2.0f && cv::clampf(9.0f, 2.0f, 5.0f) == 5.0f;
  static_assert(cv::clampf(7.0f, 0.0f, 1.0f) == 1.0f, "clampf folds at compile time");
  double dbWorst = 0.0;
  for (const double db : {-60.0, -6.0, -0.5, 6.0, 12.5}) {
    const double want = std::pow(10.0, db / 20.0);
    dbWorst = std::fmax(dbWorst, std::fabs(cv::dbToGain(static_cast<float>(db)) / want - 1.0));
  }
  const bool unity = cv::dbToGain(0.0f) == 1.0f;
  const bool taper = cv::logTaper(20.0f, 2000.0f, 0.0f) == 20.0f &&
                     std::fabs(cv::logTaper(20.0f, 2000.0f, 1.0f) / 2000.0f - 1.0f) < 1e-6f &&
                     std::fabs(cv::logTaper(20.0f, 2000.0f, 0.5f) / std::sqrt(20.0f * 2000.0f) - 1.0f) < 1e-6f;
  bool ok = report("clampf", clamp, "limits, NaN reads as the lower bound, infinities clamp=%.0f", clamp ? 1.0 : 0.0);
  ok &= report("dbToGain", dbWorst < 1.2e-7 && unity, "worst relative error %.2g (< 1.2e-7), 0 dB is exactly 1=%.0f", dbWorst,
               unity ? 1.0 : 0.0);
  ok &= report("logTaper", taper, "ends at from and to, midpoint at the geometric mean=%.0f", taper ? 1.0 : 0.0);
  return ok;
}

// OnePole: the step response at one time constant, the DC gain of each output, and the corner clamp.
bool testOnePole() {
  cv::OnePole p;
  p.set(1000.0f);
  float y = 0.0f;
  for (int i = 0; i < 40; ++i) y = p.lp(1.0f);
  const double a = 1.0 - std::exp(-2.0 * kPi * 1000.0 / cv::kSampleRate);
  const double stepErr = std::fabs(static_cast<double>(y) - (1.0 - std::pow(1.0 - a, 40.0)));
  cv::OnePole lp, hp;
  lp.set(200.0f);
  hp.set(200.0f);
  float last = 0.0f, rest = 1.0f;
  for (int i = 0; i < 20000; ++i) {
    last = lp.lp(0.5f);
    rest = hp.hp(0.5f);
  }
  const bool dc = std::fabs(last - 0.5f) < 1e-6f && std::fabs(rest) < 1e-6f;
  const bool clampLow = cv::onePoleCoef(0.0f) == cv::onePoleCoef(cv::kFilterMinHz) && cv::onePoleCoef(kNaN) == cv::onePoleCoef(cv::kFilterMinHz);
  const bool clampHigh = cv::onePoleCoefAt(1e9f, 24000.0f) == cv::onePoleCoefAt(0.45f * 24000.0f, 24000.0f);
  bool ok = report("one pole", stepErr < 1e-6, "40 steps of a 1 kHz corner sit %.2g from 1 - (1 - a)^40 (< 1e-6)", stepErr);
  ok &= report("one pole", dc && clampLow && clampHigh, "DC gain 1 on lp and 0 on hp, corners clamp, NaN reads as the floor=%.0f",
               (dc && clampLow && clampHigh) ? 1.0 : 0.0);
  return ok;
}

// The double cookbook design as coefficients over a0 = 1, and its magnitude response in dB.
struct Design {
  double b0, b1, b2, a1, a2;
};

Design normalised(double b0, double b1, double b2, double a0, double a1, double a2) {
  return {b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0};
}

double responseDb(double b0, double b1, double b2, double a1, double a2, double hz) {
  const std::complex<double> z1 = std::polar(1.0, -2.0 * kPi * hz / cv::kSampleRate);
  const std::complex<double> z2 = z1 * z1;
  return 20.0 * std::log10(std::abs((b0 + b1 * z1 + b2 * z2) / (1.0 + a1 * z1 + a2 * z2)));
}

double worstDb(const cv::Biquad& f, const Design& d) {
  double worst = 0.0;
  for (const double hz : {30.0, 80.0, 250.0, 1000.0, 3000.0, 8000.0, 16000.0}) {
    const double got = responseDb(f.b0, f.b1, f.b2, f.a1, f.a2, hz);
    const double want = responseDb(d.b0, d.b1, d.b2, d.a1, d.a2, hz);
    worst = std::fmax(worst, std::fabs(got - want));
  }
  return worst;
}

// Each setter's response against the double RBJ design at the corners the stages use.
bool testBiquadDesign() {
  const double w = 2.0 * kPi;
  double worst = 0.0;
  auto lowOrHigh = [&](bool high, double hz, double q) {
    cv::Biquad f;
    high ? f.setHighPass(static_cast<float>(hz), static_cast<float>(q)) : f.setLowPass(static_cast<float>(hz), static_cast<float>(q));
    const double w0 = w * hz / cv::kSampleRate, cw = std::cos(w0), al = std::sin(w0) / (2.0 * q);
    const double s = high ? 1.0 : -1.0;
    worst = std::fmax(worst, worstDb(f, normalised(0.5 * (1.0 + s * cw), -s * (1.0 + s * cw), 0.5 * (1.0 + s * cw), 1.0 + al,
                                                   -2.0 * cw, 1.0 - al)));
  };
  lowOrHigh(false, 1000.0, 0.7071);
  lowOrHigh(false, 8000.0, 1.2);
  lowOrHigh(true, 70.0, 0.7071);
  lowOrHigh(true, 20.0, 0.7071);
  auto peak = [&](double hz, double db, double q) {
    cv::Biquad f;
    f.setPeak(static_cast<float>(hz), static_cast<float>(db), static_cast<float>(q));
    const double w0 = w * hz / cv::kSampleRate, cw = std::cos(w0), al = std::sin(w0) / (2.0 * q), a = std::pow(10.0, db / 40.0);
    worst = std::fmax(worst, worstDb(f, normalised(1.0 + al * a, -2.0 * cw, 1.0 - al * a, 1.0 + al / a, -2.0 * cw, 1.0 - al / a)));
  };
  peak(2000.0, 6.0, 1.0);
  peak(250.0, -9.0, 0.5);
  auto shelf = [&](bool high, double hz, double db, double slope) {
    cv::Biquad f;
    high ? f.setHighShelf(static_cast<float>(hz), static_cast<float>(db), static_cast<float>(slope))
         : f.setLowShelf(static_cast<float>(hz), static_cast<float>(db));
    const double w0 = w * hz / cv::kSampleRate, cw = std::cos(w0), a = std::pow(10.0, db / 40.0);
    const double al = 0.5 * std::sin(w0) * std::sqrt((a + 1.0 / a) * (1.0 / slope - 1.0) + 2.0), beta = 2.0 * std::sqrt(a) * al;
    const double s = high ? 1.0 : -1.0;
    worst = std::fmax(worst, worstDb(f, normalised(a * ((a + 1) + s * (a - 1) * cw + beta), -s * 2.0 * a * ((a - 1) + s * (a + 1) * cw),
                                                   a * ((a + 1) + s * (a - 1) * cw - beta), (a + 1) - s * (a - 1) * cw + beta,
                                                   s * 2.0 * ((a - 1) - s * (a + 1) * cw), (a + 1) - s * (a - 1) * cw - beta)));
  };
  shelf(false, 200.0, 6.0, 1.0);
  shelf(true, 4000.0, -6.0, 1.0);
  shelf(true, 5000.0, 8.0, 0.5);
  cv::Biquad hp;
  hp.setHighPass(70.0f, cv::kButterworthQ);
  const bool dcZero = hp.b0 + hp.b1 + hp.b2 == 0.0f;
  bool ok = report("biquad design", worst < 0.008, "worst response error against the double RBJ design %.4f dB (< 0.008)", worst);
  ok &= report("biquad design", dcZero, "high-pass DC gain is exactly 0=%.0f", dcZero ? 1.0 : 0.0);
  return ok;
}

bool sameCoefs(const cv::Biquad& a, const cv::Biquad& b) {
  return a.b0 == b.b0 && a.b1 == b.b1 && a.b2 == b.b2 && a.a1 == b.a1 && a.a2 == b.a2;
}

bool stable(const cv::Biquad& f) {
  return std::isfinite(f.b0) && std::isfinite(f.b1) && std::isfinite(f.b2) && std::fabs(f.a2) < 1.0f &&
         std::fabs(f.a1) < 1.0f + f.a2;
}

// The corner forms write the same coefficients as the setters, the peak Q floors, and a NaN or an
// infinity for a corner, a gain or a Q leaves a stable section.
bool testBiquadForms() {
  bool same = true;
  for (const float hz : {50.0f, 1200.0f, 9000.0f}) {
    for (const float db : {-8.0f, 3.0f}) {
      cv::Biquad a, b;
      a.setPeak(hz, db, 0.9f);
      b.setPeakAmp(cv::rbjCorner(hz), cv::rbjAmp(db), 0.9f);
      same &= sameCoefs(a, b);
      a.setLowShelf(hz, db);
      b.setLowShelfAmp(cv::rbjCorner(hz), cv::rbjAmp(db));
      same &= sameCoefs(a, b);
      a.setHighShelf(hz, db, 0.4f);
      b.setHighShelfAmp(cv::rbjCorner(hz), cv::rbjAmp(db), 0.4f);
      same &= sameCoefs(a, b);
    }
  }
  cv::Biquad floor0, floorMin;
  floor0.setPeak(1000.0f, 6.0f, 0.0f);
  floorMin.setPeak(1000.0f, 6.0f, cv::kPeakMinQ);
  cv::Biquad slopeLow, slopeMin;
  slopeLow.setHighShelf(1000.0f, 6.0f, 0.0f);
  slopeMin.setHighShelf(1000.0f, 6.0f, cv::kShelfMinSlope);
  const bool floors = sameCoefs(floor0, floorMin) && sameCoefs(slopeLow, slopeMin);
  bool finite = true;
  for (const float bad : {kNaN, kInf, -kInf}) {
    cv::Biquad f;
    f.setLowPass(bad, 0.7071f);
    finite &= stable(f);
    f.setHighPass(bad, 0.7071f);
    finite &= stable(f);
    f.setPeak(bad, 6.0f, 1.0f);
    finite &= stable(f);
    f.setPeak(1000.0f, 6.0f, bad);
    finite &= bad == kInf || stable(f);  // an infinite Q is a zero-width peak, not a stability question
    f.setHighShelf(1000.0f, 6.0f, bad);
    finite &= stable(f);
  }
  bool ok = report("biquad forms", same, "setPeakAmp, setLowShelfAmp and setHighShelfAmp match the dB setters=%.0f", same ? 1.0 : 0.0);
  ok &= report("biquad forms", floors, "peak Q floors at %.1f and shelf slope at %.1f", cv::kPeakMinQ, cv::kShelfMinSlope);
  ok &= report("biquad forms", finite, "NaN and infinity leave a stable section=%.0f", finite ? 1.0 : 0.0);
  return ok;
}

// catmullRom against its tap-weight form, through its knots, exact on a line, and the step overshoot
// that the soft ceiling after it absorbs.
bool testCatmullRom() {
  uint32_t s = 7u;
  auto rnd = [&s]() {
    s = s * 1664525u + 1013904223u;
    return static_cast<float>((s >> 8) * (2.0 / 16777216.0) - 1.0);
  };
  double worstForm = 0.0;
  double worstSum = 0.0;
  for (int i = 0; i < 20000; ++i) {
    const float t = static_cast<float>((s >> 8) % 1000) * 0.001f;
    const float xm = rnd(), x0 = rnd(), x1 = rnd(), x2 = rnd();
    const cv::CatmullRomWeights w = cv::catmullRomWeights(t);
    worstForm = std::fmax(worstForm, std::fabs(cv::catmullRom(xm, x0, x1, x2, t) - (w.m * xm + w.z * x0 + w.p * x1 + w.q * x2)));
    worstSum = std::fmax(worstSum, std::fabs(w.m + w.z + w.p + w.q - 1.0f));
  }
  const bool knots = cv::catmullRom(0.3f, -0.2f, 0.7f, 0.1f, 0.0f) == -0.2f &&
                     std::fabs(cv::catmullRom(0.3f, -0.2f, 0.7f, 0.1f, 1.0f) - 0.7f) < 1e-6f;
  double lineErr = 0.0;
  for (int k = 0; k < 100; ++k) {
    const float t = static_cast<float>(k) * 0.01f;
    lineErr = std::fmax(lineErr, std::fabs(cv::catmullRom(-1.0f, 0.0f, 1.0f, 2.0f, t) - t));
  }
  float peak = 0.0f;
  for (int k = 0; k <= 1000; ++k) peak = std::fmax(peak, cv::catmullRom(0.0f, 1.0f, 1.0f, 1.0f, static_cast<float>(k) * 0.001f));
  bool ok = report("catmullRom", worstForm < 1.1e-6 && worstSum < 5e-7,
                   "weights match the cubic within %.2g (< 1.1e-6), sum to 1 within %.2g (< 5e-7)", worstForm, worstSum);
  ok &= report("catmullRom", knots && lineErr < 1e-6, "passes through its knots and is exact on a line (%.2g, < 1e-6)", lineErr);
  ok &= report("catmullRom", peak > 1.0740f && peak < 1.0745f, "a 0-to-1 step overshoots to %.4f (1.0741)", peak);
  return ok;
}

// A ring of 32 filled with a line of age: value 10 + 2 * age, so an exact read at delay d gives 10 + 2 d.
// The ring sits between two NaN guards, so a read outside the ring returns NaN.
struct GuardedRing {
  static constexpr int kLen = 32;
  float lo[8];
  float ring[kLen];
  float hi[8];

  explicit GuardedRing(int head) {
    for (float& v : lo) v = kNaN;
    for (float& v : hi) v = kNaN;
    for (int age = 0; age < kLen; ++age) ring[(head - age + kLen) % kLen] = 10.0f + 2.0f * static_cast<float>(age);
  }
};

bool testRingReads() {
  constexpr int kLen = GuardedRing::kLen;
  double worst = 0.0;
  for (int head = 0; head < kLen; ++head) {
    const GuardedRing g(head);
    for (const float d : {1.25f, 2.0f, 2.25f, 7.5f, 15.999f, 28.5f, 29.0f, 30.0f}) {
      worst = std::fmax(worst, std::fabs(cv::readRing<kLen>(g.ring, head, d) - (10.0f + 2.0f * d)));
    }
  }
  const GuardedRing g(5);
  // A NaN or infinite delay gives a NaN fraction, so readRing answers NaN there. Every other delay,
  // however far out, indexes inside the ring.
  bool finite = true;
  for (const float d : {1e30f, -1e30f, 0.25f, -3.0f, 100.0f}) finite &= std::isfinite(cv::readRing<kLen>(g.ring, 5, d));
  const bool wrap = cv::wrapOnce<kLen>(-1) == kLen - 1 && cv::wrapOnce<kLen>(kLen) == 0 && cv::wrapOnce<kLen>(7) == 7 &&
                    cv::wrapOnce<kLen>(kLen + 3) == 3 && cv::wrapOnce<kLen>(3 * kLen) == 0;
  bool ok = report("readRing", worst < 2e-5, "exact on a line for delays 1.25 to 30 at every head, worst %.2g (< 2e-5)", worst);
  ok &= report("readRing", finite, "out-of-span delays stay inside the ring=%.0f", finite ? 1.0 : 0.0);
  ok &= report("wrapOnce", wrap, "wraps one length either way and sends anything farther to 0=%.0f", wrap ? 1.0 : 0.0);
  return ok;
}

// The reverbs' soft clip and Delay and Allpass.
bool testReverbParts() {
  using cv::kReverbClipHeadroom;
  using cv::softClip;
  using cv::softClipHeadroom;
  bool odd = true;
  float prev = softClip(-10.0f);
  bool mono = true;
  double maxSlope = 0.0;
  for (int k = -1000; k <= 1000; ++k) {
    const float x = static_cast<float>(k) * 0.005f;
    odd &= softClip(-x) == -softClip(x);
    mono &= softClip(x) >= prev - 2e-7f;  // the curve rounds up to 1 + 1.2e-7 just short of the rail
    maxSlope = std::fmax(maxSlope, (softClip(x + 0.005f) - softClip(x)) / 0.005);
    prev = softClip(x);
  }
  const bool rails = softClip(kInf) == 1.0f && softClip(-kInf) == -1.0f && softClip(3.0f) == 1.0f;
  const bool headroom = std::fabs(softClipHeadroom(0.01f, kReverbClipHeadroom) - 0.01f) < 1e-5f && softClipHeadroom(100.0f, 4.5f) == 4.5f;
  bool ok = report("softClip", odd && mono && rails && headroom && maxSlope < 1.0000001,
                   "odd, monotone, slope %.7f at most 1, rails at +-1, headroom form unity below the knee", maxSlope);

  reverb_parts::Delay<32> dl;
  for (int i = 0; i < 32; ++i) dl.write(static_cast<float>(i));
  const bool delay = dl.read(0.0f) == 0.0f && dl.read(31.0f) == 31.0f && std::fabs(dl.read(30.5f) - 30.5f) < 1e-5f &&
                     dl.read(kNaN) == 0.0f && dl.read(-5.0f) == 0.0f && dl.read(100.0f) == 31.0f && dl.read(kInf) == 31.0f;
  ok &= report("Delay", delay, "linear read on [0, N - 1], out of span and NaN clamp=%.0f", delay ? 1.0 : 0.0);

  // An allpass has unit gain at every frequency; k = 0.6, -0.883 and 0.9 are the reverbs' coefficients.
  double allpassWorst = 0.0;
  for (const float k : {0.6f, -0.883f, 0.9f}) {
    reverb_parts::Allpass<113> ap;
    double in2 = 0.0, out2 = 0.0;
    for (int i = 0; i < 48000; ++i) {
      const float x = static_cast<float>(std::sin(2.0 * kPi * 997.0 * i / cv::kSampleRate));
      const float y = ap.run(x, k);
      if (i >= 24000) {
        in2 += static_cast<double>(x) * x;
        out2 += static_cast<double>(y) * y;
      }
    }
    allpassWorst = std::fmax(allpassWorst, std::fabs(10.0 * std::log10(out2 / in2)));
  }
  ok &= report("Allpass", allpassWorst < 1e-7, "unit gain within %.2g dB at k 0.6, -0.883 and 0.9 (< 1e-7)", allpassWorst);
  return ok;
}

// The LFO against the true sine, and the knob law and wobble bound.
bool testSineAndKnobs() {
  double lfoWorst = 0.0;
  bool inRange = true;
  for (const uint32_t phase8 : {0u, 64u, 200u}) {
    for (uint32_t acc = 0; acc < 0xFFFF0000u; acc += 0x00123457u) {
      const double turns = static_cast<double>(acc) / 4294967296.0 + static_cast<double>(phase8) / 256.0;
      const double want = 0.5 + 0.5 * std::sin(2.0 * kPi * turns);
      const float got = reverb_parts::lfo(acc, phase8);
      inRange &= got >= 0.0f && got <= 1.0f;
      lfoWorst = std::fmax(lfoWorst, std::fabs(static_cast<double>(got) - want));
    }
  }
  const reverb_parts::KnobCurve curve{0.25f, 0.5f, 0.85f};
  const bool knob = curve.at(0.0f) == 0.25f && curve.at(0.5f) == 0.5f && curve.at(1.0f) == 0.85f && curve.at(kNaN) == 0.25f &&
                    curve.at(-1.0f) == 0.25f && curve.at(2.0f) == 0.85f && std::fabs(curve.at(0.25f) - 0.375f) < 1e-6f;
  const bool wobble = std::fabs(reverb_parts::wobbleError(1.0f, 0.0f) - 1e-6f) < 1e-9f;
  bool ok = report("lfo", lfoWorst < 1e-6 && inRange, "stays in [0, 1], within %.2g of the true sine (< 1e-6, sinTurns' bound)", lfoWorst);
  ok &= report("KnobCurve", knob && wobble, "hits lo, mid and hi, NaN reads as lo; the wobble bound carries the sine term=%.0f",
               (knob && wobble) ? 1.0 : 0.0);
  return ok;
}

// midiOf against the double conversion, the twelve key tables, and the hold band.
bool testScale() {
  using namespace cv;
  double midiWorst = 0.0;
  for (double hz = 60.0; hz <= 1100.0; hz *= 1.0005) {
    const double want = 69.0 + 12.0 * std::log2(static_cast<double>(static_cast<float>(hz)) / 440.0);
    midiWorst = std::fmax(midiWorst, std::fabs(midiOf(static_cast<float>(hz)) - want));
  }
  bool tables = true;
  bool seen[12] = {};
  for (int k = 0; k < kKeyCount; ++k) {
    seen[kKeyRoot[static_cast<size_t>(k)]] = true;
    tables &= k == 0 || (kKeyRoot[static_cast<size_t>(k)] - kKeyRoot[static_cast<size_t>(k - 1)] + 12) % 12 == 7;
  }
  for (bool s : seen) tables &= s;
  // Every whole note deviates 0 on the scale and +1 off it, so a tie snaps to the lower note.
  bool deviation = true;
  for (int k = 0; k < kKeyCount; ++k) {
    for (int note = 36; note <= 96; ++note) {
      const int degree = ((note - kKeyRoot[static_cast<size_t>(k)]) % 12 + 12) % 12;
      bool onScale = false;
      for (int d : kMajor) onScale |= d == degree;
      deviation &= scaleDeviation(static_cast<float>(note), k) == (onScale ? 0.0f : 1.0f);
    }
  }
  // From a held note the next one takes over at kHoldSemis past the halfway point, in both directions.
  bool hold = true;
  for (int k = 0; k < kKeyCount; ++k) {
    float lowNote = -1.0f;
    for (int note = 48; note <= 72; ++note) {
      const float n = static_cast<float>(note);
      if (scaleDeviation(n, k) != 0.0f) continue;
      if (lowNote >= 0.0f) {
        const float mid = 0.5f * (lowNote + n);
        hold &= heldNote(mid + 0.14f, k, lowNote) == lowNote && heldNote(mid + 0.16f, k, lowNote) == n &&
                heldNote(mid - 0.14f, k, n) == n && heldNote(mid - 0.16f, k, n) == lowNote;
      }
      lowNote = n;
    }
  }
  bool ok = report("midiOf", midiWorst < 1.2e-5, "60 to 1100 Hz within %.2g semitones of the double (< 1.2e-5)", midiWorst);
  ok &= report("scale", tables && deviation && hold, "twelve keys by fifths, whole notes deviate 0 or +1, hold band at %.2f semitones=%.0f",
               kHoldSemis, (tables && deviation && hold) ? 1.0 : 0.0);
  return ok;
}

}  // namespace

int main() {
  bool ok = true;
  ok &= testCoefficient();
  ok &= testStepSettles();
  ok &= testTonePasses();
  ok &= testClear();
  ok &= testLaws();
  ok &= testOnePole();
  ok &= testBiquadDesign();
  ok &= testBiquadForms();
  ok &= testCatmullRom();
  ok &= testRingReads();
  ok &= testReverbParts();
  ok &= testSineAndKnobs();
  ok &= testScale();
  std::printf("%s dsp\n", ok ? "PASS" : "FAIL");
  return ok ? 0 : 1;
}
