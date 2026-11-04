// Polish compressor checks on the one tuning the box runs (FIRM): the tuning table, the static curve,
// ballistics, sidechain high-pass and cap; bit-exact off, silence and no-alloc. No window, no
// audio device.

#include <cfenv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "engine/polish_comp.h"

#include "alloc_guard.h"

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr int kSecond = cv::kSampleRate;
constexpr double kTau = 0.632;  // share of the way a one-pole covers in one time constant
constexpr double kQuietMinAbs = 1e-3;  // a -50 dBFS RMS sine peaks at 4.5e-3

using Signal = std::vector<float>;

// A sine whose RMS is rmsDb dBFS.
Signal sine(double hz, double rmsDb, int n) {
  const double amp = std::sqrt(2.0) * std::pow(10.0, rmsDb / 20.0);
  Signal x(static_cast<size_t>(n));
  for (int i = 0; i < n; ++i)
    x[static_cast<size_t>(i)] = static_cast<float>(amp * std::sin(2.0 * kPi * hz * i / cv::kSampleRate));
  return x;
}

Signal join(Signal a, const Signal& b) {
  a.insert(a.end(), b.begin(), b.end());
  return a;
}

double rmsOf(const Signal& x, int from, int to) {
  double s = 0.0;
  for (int i = from; i < to; ++i) s += static_cast<double>(x[static_cast<size_t>(i)]) * x[static_cast<size_t>(i)];
  return std::sqrt(s / (to - from));
}

// Runs the signal through process() in kBlock chunks. `paramsAt` gives the params of the block
// that starts at sample i.
template <typename F>
Signal runWith(cv::PolishComp& c, const Signal& in, F paramsAt) {
  Signal out(in.size());
  const int total = static_cast<int>(in.size());
  for (int i = 0; i < total; i += cv::kBlock) {
    const int n = total - i < cv::kBlock ? total - i : cv::kBlock;
    const cv::PolishCompParams p = paramsAt(i);
    gInProcess = true;
    c.process(&in[static_cast<size_t>(i)], &out[static_cast<size_t>(i)], n, p);
    gInProcess = false;
  }
  return out;
}

Signal run(cv::PolishComp& c, const Signal& in, const cv::PolishCompParams& p) {
  return runWith(c, in, [&](int) { return p; });
}

// Unity makeup, so the gain is the gain reduction alone.
cv::PolishCompParams pureParams(const cv::PolishCompTuning& t = cv::PolishCompTuning()) {
  cv::PolishCompParams p;
  p.tuning = t;
  p.tuning.trimDb = 0.0f;
  return p;
}

struct Named {
  const char* name;
  cv::PolishCompTuning tuning;
};

const Named kFirm = {"firm", cv::PolishCompTuning()};

std::string labelled(const char* what, const Named& n) { return std::string(what) + " " + n.name; }

bool report(const char* name, bool ok, const char* fmt, double a, double b = 0.0, double c = 0.0,
            double d = 0.0, double e = 0.0) {
  char detail[220];
  std::snprintf(detail, sizeof detail, fmt, a, b, c, d, e);
  std::printf("%s %s  %s\n", ok ? "PASS" : "FAIL", name, detail);
  return ok;
}

// Gain reduction of a steady sine, measured over seconds 1 to 2 of a 2 s run.
double steadyReductionDb(double hz, double rmsDb, const cv::PolishCompTuning& t) {
  static cv::PolishComp comp;
  comp.reset();
  const Signal in = sine(hz, rmsDb, 2 * kSecond);
  const Signal out = run(comp, in, pureParams(t));
  // The leading 0.0 keeps an exact unity gain from printing as -0.00.
  return 0.0 - 20.0 * std::log10(rmsOf(out, kSecond, 2 * kSecond) / rmsOf(in, kSecond, 2 * kSecond));
}

// The spec's FIRM row, pinned with the detector window and the sidechain corner.
bool testTuning() {
  const cv::PolishCompTuning& t = kFirm.tuning;
  const bool table = t.thresholdDb == -30.0f && t.ratio == 4.0f && t.kneeDb == 4.0f && t.attackMs == 3.0f &&
                     t.releaseMs == 100.0f && t.maxReductionDb == 18.0f && t.rmsMs == 10.0f && t.sidechainHz == 150.0f;
  return report("tuning", table, "the table matches the spec=%.0f (1)", table ? 1.0 : 0.0);
}

// The textbook soft-knee curve: none below the knee, quadratic through it, then 1 - 1/ratio per dB.
double kneeReductionDb(double levelDb, const cv::PolishCompTuning& t) {
  const double over = levelDb - t.thresholdDb;
  const double slope = 1.0 - 1.0 / t.ratio;
  double gr;
  if (2.0 * over < -t.kneeDb) {
    gr = 0.0;
  } else if (2.0 * std::fabs(over) <= t.kneeDb) {
    const double x = over + 0.5 * t.kneeDb;
    gr = slope * x * x / (2.0 * t.kneeDb);
  } else {
    gr = slope * over;
  }
  return gr < t.maxReductionDb ? gr : static_cast<double>(t.maxReductionDb);
}

// The five levels sit at fixed offsets from the threshold: under the knee, at its centre, just over
// it, and two steps up the ratio slope.
bool testStaticCurve(const Named& n) {
  const cv::PolishCompTuning& t = n.tuning;
  const double offsets[] = {-12.0, 0.0, 6.0, 12.0, 18.0};
  double got[5], worst = 0.0;
  for (int i = 0; i < 5; ++i) {
    const double level = t.thresholdDb + offsets[i];
    got[i] = steadyReductionDb(1000.0, level, t);
    worst = std::fmax(worst, std::fabs(got[i] - kneeReductionDb(level, t)));
  }
  std::printf("     %s reduction at threshold -12/0/+6/+12/+18 dB = %.2f / %.2f / %.2f / %.2f / %.2f dB\n", n.name, got[0],
              got[1], got[2], got[3], got[4]);
  return report(labelled("static curve", n).c_str(), worst <= 0.3, "worst distance from the knee formula=%.3f dB (<= 0.3)",
                worst);
}

// A level 3 dB past what the cap allows: the slope asks for more than the cap, and the cap wins.
bool testReductionCap(const Named& n) {
  const cv::PolishCompTuning& t = n.tuning;
  const double slope = 1.0 - 1.0 / t.ratio;
  const double asked = t.maxReductionDb + 3.0;
  const double got = steadyReductionDb(1000.0, t.thresholdDb + asked / slope, t);
  return report(labelled("reduction cap", n).c_str(), std::fabs(got - t.maxReductionDb) <= 0.2,
                "the slope asks %.0f dB and %.2f dB is applied (cap %.0f +-0.2)", asked, got, t.maxReductionDb);
}

// Gain reduction in dB per sample from out/in, NaN where the input sits too close to a zero
// crossing to divide by.
std::vector<double> reductionTrace(const Signal& in, const Signal& out, double minAbs) {
  std::vector<double> gr(in.size(), std::nan(""));
  for (size_t i = 0; i < in.size(); ++i) {
    if (std::fabs(in[i]) < minAbs) continue;
    gr[i] = -20.0 * std::log10(static_cast<double>(out[i]) / in[i]);
  }
  return gr;
}

double lastValid(const std::vector<double>& gr, int idx) {
  while (std::isnan(gr[static_cast<size_t>(idx)])) --idx;
  return gr[static_cast<size_t>(idx)];
}

// Milliseconds from sample `from` until the trace has covered kTau of the way from its value just
// before `from` to its value at `settled`. -1 when it never gets there.
double timeConstantMs(const std::vector<double>& gr, int from, int settled) {
  const double start = lastValid(gr, from - 1);
  const double end = lastValid(gr, settled);
  const double mark = start + kTau * (end - start);
  const bool rising = end > start;
  for (int i = from; i <= settled; ++i) {
    const double v = gr[static_cast<size_t>(i)];
    if (std::isnan(v)) continue;
    if (rising ? v >= mark : v <= mark) return 1000.0 * (i - from) / cv::kSampleRate;
  }
  return -1.0;
}

// Attack and release as the gain smoother's time constants. The detector sits still at -12 dBFS
// RMS while the threshold moves, so the target gain reduction steps and nothing else does.
bool testBallistics(const Named& n) {
  static cv::PolishComp comp;
  comp.reset();
  const Signal in = sine(1000.0, -12.0, 3 * kSecond);
  const Signal out = runWith(comp, in, [&n](int i) {
    cv::PolishCompParams p = pureParams(n.tuning);
    p.tuning.thresholdDb = (i >= kSecond && i < 2 * kSecond) ? -30.0f : -18.0f;
    return p;
  });
  const std::vector<double> gr = reductionTrace(in, out, 0.25 * std::sqrt(2.0) * std::pow(10.0, -12.0 / 20.0));
  const double attack = timeConstantMs(gr, kSecond, 2 * kSecond - 1);
  const double release = timeConstantMs(gr, 2 * kSecond, 3 * kSecond - 1);
  const cv::PolishCompTuning& t = n.tuning;
  const bool ok = std::fabs(attack - t.attackMs) <= 0.2 * t.attackMs &&
                  std::fabs(release - t.releaseMs) <= 0.2 * t.releaseMs;
  return report(labelled("ballistics", n).c_str(), ok, "attack %.1f ms (%.0f +-20%%), release %.1f ms (%.0f +-20%%)", attack,
                t.attackMs, release, t.releaseMs);
}

// The same times end to end, through the 10 ms RMS window, for a level step 18 dB over the
// threshold. Printed for the owner. The bounds only catch a detector that has stopped following
// the level: the attack lands within the detector window of the smoother's, the release within half again.
bool testStepResponse(const Named& n) {
  static cv::PolishComp comp;
  const cv::PolishCompTuning& t = n.tuning;
  const double loudDb = t.thresholdDb + 18.0;
  comp.reset();
  const int quiet = kSecond / 2;
  const Signal up = join(sine(1000.0, -50.0, quiet), sine(1000.0, loudDb, kSecond));
  const std::vector<double> rise = reductionTrace(up, run(comp, up, pureParams(t)), kQuietMinAbs);
  const double attack = timeConstantMs(rise, quiet, quiet + kSecond - 1);

  comp.reset();
  const int loud = kSecond;
  const Signal down = join(sine(1000.0, loudDb, loud), sine(1000.0, -50.0, 3 * kSecond / 2));
  const std::vector<double> fall = reductionTrace(down, run(comp, down, pureParams(t)), kQuietMinAbs);
  const double release = timeConstantMs(fall, loud, loud + 3 * kSecond / 2 - 1);

  const bool ok = attack >= t.attackMs && attack <= t.attackMs + 15.0 && release >= t.releaseMs &&
                  release <= 1.5 * t.releaseMs;
  return report(labelled("step response", n).c_str(), ok, "level step up %.1f ms over a %.0f ms smoother, down %.1f ms over %.0f ms",
                attack, t.attackMs, release, t.releaseMs);
}

// A 50 Hz tone at -6 dBFS RMS sits 38 dB down after the 4-pole sidechain filter, well under every
// tuning's knee. Without the filter it would reduce as much as the 1 kHz tone does.
bool testSidechain(const Named& n) {
  const double levelDb = -6.0;
  const double low = steadyReductionDb(50.0, levelDb, n.tuning);
  const double mid = steadyReductionDb(1000.0, levelDb, n.tuning);
  return report(labelled("sidechain high-pass", n).c_str(), std::fabs(low) < 0.5 && mid > 3.0,
                "%.0f dBFS RMS reduces %.2f dB at 50 Hz (<0.5), %.2f dB at 1 kHz (>3)", levelDb, low, mid);
}

bool testOffBitExact() {
  static cv::PolishComp comp;
  comp.reset();
  const Signal in = sine(1000.0, -12.0, 2 * kSecond);
  const Signal out = runWith(comp, in, [](int i) {
    cv::PolishCompParams p;
    p.on = i < kSecond;
    return p;
  });
  int lastDiff = kSecond - 1;
  for (int i = kSecond; i < 2 * kSecond; ++i)
    if (out[static_cast<size_t>(i)] != in[static_cast<size_t>(i)]) lastDiff = i;
  const double settleMs = 1000.0 * (lastDiff + 1 - kSecond) / cv::kSampleRate;

  comp.reset();
  cv::PolishCompParams off;
  off.on = false;
  const Signal fresh = run(comp, in, off);
  const bool freshExact = fresh == in;
  return report("off is bit-exact", settleMs <= 400.0 && freshExact,
                "exact %.0f ms after the switch (<= 400), exact from reset=%.0f (1)", settleMs, freshExact ? 1.0 : 0.0);
}

// The probe proves the exception flags work on this platform, so a clean result means something.
bool underflowFlagWorks() {
  std::feclearexcept(FE_ALL_EXCEPT);
  volatile float tiny = 1e-30f;
  volatile float product = tiny * tiny;
  (void)product;
  return std::fetestexcept(FE_UNDERFLOW) != 0;
}

bool testSilence() {
  static cv::PolishComp comp;
  comp.reset();
  const Signal burst = sine(300.0, 0.0, kSecond / 2);
  const Signal silence(static_cast<size_t>(4 * kSecond), 0.0f);
  run(comp, burst, pureParams());

  const bool flagsWork = underflowFlagWorks();
  std::feclearexcept(FE_ALL_EXCEPT);
  const Signal out = run(comp, silence, pureParams());
  const int raised = std::fetestexcept(FE_INVALID | FE_DIVBYZERO | FE_UNDERFLOW);

  int bad = 0;
  for (float v : out) {
    const int kind = std::fpclassify(v);
    if (kind == FP_NAN || kind == FP_INFINITE || kind == FP_SUBNORMAL || v != 0.0f) ++bad;
  }
  const Signal after = run(comp, sine(1000.0, -12.0, kSecond / 2), pureParams());
  bool finite = true;
  for (float v : after) finite = finite && std::isfinite(v);

  if (!flagsWork) std::printf("     (floating-point exception flags unavailable here, flag check skipped)\n");
  return report("silence", bad == 0 && finite && (raised == 0 || !flagsWork),
                "%.0f bad samples in 4 s of silence, FP exception bits=%.0f (0), finite after=%.0f (1)", bad, raised,
                finite ? 1.0 : 0.0);
}

bool testNoAlloc() {
  static cv::PolishComp comp;
  comp.reset();
  run(comp, sine(500.0, -12.0, cv::kBlock * 8), cv::PolishCompParams{});  // aborts on any new/delete inside process()
  return report("no-alloc", true, "%.0f allocations in process()", 0.0);
}

}  // namespace

int main() {
  bool ok = testTuning();
  ok &= testStaticCurve(kFirm);
  ok &= testReductionCap(kFirm);
  ok &= testBallistics(kFirm);
  ok &= testStepResponse(kFirm);
  ok &= testSidechain(kFirm);
  ok &= testOffBitExact();
  ok &= testSilence();
  ok &= testNoAlloc();
  return ok ? 0 : 1;
}
