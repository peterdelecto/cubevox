// DcBlocker checks: the series coefficient against the exact one, a 0.5 DC step settling under
// -60 dBFS within 200 ms, a 100 Hz tone passing within 0.05 dB, and clear(). No window, no audio
// device.

#include <cmath>
#include <complex>
#include <cstdio>
#include <initializer_list>

#include "engine/dsp.h"

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
    ok &= report("dc coefficient", rel < 5e-7, "%.0f Hz: coefficient %.9g, relative error %.2g (< 5e-7)", hz, got, rel);
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

}  // namespace

int main() {
  bool ok = true;
  ok &= testCoefficient();
  ok &= testStepSettles();
  ok &= testTonePasses();
  ok &= testClear();
  std::printf("%s dsp\n", ok ? "PASS" : "FAIL");
  return ok ? 0 : 1;
}
