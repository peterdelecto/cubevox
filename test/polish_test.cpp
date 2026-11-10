// VOX EQ checks: both halves bit-exact when off, the front response with and without the cuts, the cuts'
// fade, the whole stage's fade on every switch, the back response, the de-esser behind the low-pass, flat, call splitting, the setting ranges, no-alloc. No window, no audio device.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "engine/polish.h"

#include "alloc_guard.h"

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr float kAmp = 0.251f;  // -12 dBFS
constexpr int kSecond = cv::kSampleRate;

enum class Half { Front, Back };

std::vector<float> sine(double hz, int n) {
  std::vector<float> x(static_cast<size_t>(n));
  for (int i = 0; i < n; ++i)
    x[static_cast<size_t>(i)] = kAmp * static_cast<float>(std::sin(2.0 * kPi * hz * i / cv::kSampleRate));
  return x;
}

// Runs the whole signal through one half in kBlock chunks.
std::vector<float> run(cv::Polish& e, Half half, const std::vector<float>& in, const cv::PolishParams& p) {
  std::vector<float> out(in.size());
  const int total = static_cast<int>(in.size());
  for (int i = 0; i < total; i += cv::kBlock) {
    const int n = total - i < cv::kBlock ? total - i : cv::kBlock;
    gInProcess = true;
    if (half == Half::Front) {
      e.processFront(&in[static_cast<size_t>(i)], &out[static_cast<size_t>(i)], n, p);
    } else {
      e.processBack(&in[static_cast<size_t>(i)], &out[static_cast<size_t>(i)], n, p);
    }
    gInProcess = false;
  }
  return out;
}

double rmsOf(const std::vector<float>& x, int from, int to) {
  double s = 0.0;
  for (int i = from; i < to; ++i) s += static_cast<double>(x[static_cast<size_t>(i)]) * x[static_cast<size_t>(i)];
  return std::sqrt(s / (to - from));
}

bool report(const char* name, bool ok, const char* fmt, double a, double b = 0.0, double c = 0.0,
            double d = 0.0, double e = 0.0, double f = 0.0) {
  char detail[240];
  std::snprintf(detail, sizeof detail, fmt, a, b, c, d, e, f);
  std::printf("%s %s  %s\n", ok ? "PASS" : "FAIL", name, detail);
  return ok;
}

static cv::Polish gEq;

// Gain in dB of a sine through one half, RMS over seconds 1-2.
double gainDb(Half half, double hz, const cv::PolishParams& p) {
  const std::vector<float> in = sine(hz, 2 * kSecond);
  gEq.reset();
  const std::vector<float> out = run(gEq, half, in, p);
  return 20.0 * std::log10(rmsOf(out, kSecond, 2 * kSecond) / rmsOf(in, kSecond, 2 * kSecond));
}

// Returns max|out - in| for `half` switched off. It reads after the first 100 ms from reset, and
// after the first 400 ms when the half was on before, because the 20 ms smoother snaps to off in
// about 280 ms.
float offError(Half half, bool afterOn) {
  const std::vector<float> in = sine(1000.0, kSecond);
  cv::PolishParams on;
  cv::PolishParams off;
  off.on = false;
  gEq.reset();
  if (afterOn) run(gEq, half, in, on);
  const std::vector<float> out = run(gEq, half, in, off);
  float maxDiff = 0.0f;
  for (int i = (afterOn ? 4 : 1) * cv::kSampleRate / 10; i < kSecond; ++i) {
    const float d = std::fabs(out[static_cast<size_t>(i)] - in[static_cast<size_t>(i)]);
    if (d > maxDiff) maxDiff = d;
  }
  return maxDiff;
}

bool testPassthrough() {
  const float f0 = offError(Half::Front, false), b0 = offError(Half::Back, false);
  const float f1 = offError(Half::Front, true), b1 = offError(Half::Back, true);
  return report("passthrough", f0 == 0.0f && b0 == 0.0f && f1 == 0.0f && b1 == 0.0f,
                "on=false max|out-in|, front/back from reset after 100 ms %.3g / %.3g, after being on after 400 ms %.3g / %.3g (0)",
                f0, b0, f1, b1);
}

// The de-esser sits out of the front response, which is the two filters alone.
cv::PolishParams filtersOnly() {
  cv::PolishParams p;
  p.tuning.deEsser.maxCutDb = 0.0f;
  return p;
}

bool testFront() {
  const cv::PolishParams p = filtersOnly();
  const double g50 = gainDb(Half::Front, 50.0, p), g70 = gainDb(Half::Front, 70.0, p);
  const double g97 = gainDb(Half::Front, 97.0, p), g1k = gainDb(Half::Front, 1000.0, p);
  const double g6k = gainDb(Half::Front, 6000.0, p), g10k = gainDb(Half::Front, 10000.0, p);
  const bool ok = g50 <= -11.0 && std::fabs(g70 + 3.0) <= 0.3 && std::fabs(g97) <= 0.5 && std::fabs(g1k) <= 0.1 &&
                  std::fabs(g6k + 3.0) <= 0.3 && g10k <= -20.0;
  return report("front response", ok,
                "50/70/97/1k/6k/10k Hz = %.2f / %.2f / %.2f / %.3f / %.2f / %.2f dB (<=-11, -3, 0, 0, -3, <=-20)",
                g50, g70, g97, g1k, g6k, g10k);
}

// ON runs the front half without the cuts: only the 40 Hz subsonic high-pass takes anything off the low
// end, so 97 Hz and 10 kHz pass, with the de-esser off and live.
bool testFrontNoCuts() {
  cv::PolishParams filters = filtersOnly();
  filters.cuts = false;
  cv::PolishParams voiced;
  voiced.cuts = false;
  const double f97 = gainDb(Half::Front, 97.0, filters), f10k = gainDb(Half::Front, 10000.0, filters);
  const double v97 = gainDb(Half::Front, 97.0, voiced), v10k = gainDb(Half::Front, 10000.0, voiced);
  const bool ok = std::fabs(f97) <= 0.05 && std::fabs(f10k) <= 0.05 && std::fabs(v97) <= 0.1 && std::fabs(v10k) <= 0.5;
  return report("front no cuts", ok,
                "cuts off, 97 Hz / 10 kHz: de-esser out %.3f / %.3f dB, live %.3f / %.3f dB (0 +-0.05, 0 +-0.1 and 0 +-0.5)",
                f97, f10k, v97, v10k);
}

// The subsonic high-pass at ON: 4th-order Butterworth at 40 Hz, so 3 dB down at 40 Hz and 20 dB or more
// down at 20 Hz, under any note Adam sings. It takes less than 1 dB off 50 Hz, the octave below his G2.
bool testSubsonic() {
  cv::PolishParams p = filtersOnly();
  p.cuts = false;
  const double g20 = gainDb(Half::Front, 20.0, p), g40 = gainDb(Half::Front, 40.0, p);
  const double g50 = gainDb(Half::Front, 50.0, p);
  const bool ok = g20 <= -20.0 && std::fabs(g40 + 3.0) <= 0.3 && g50 >= -1.0 && g50 <= -0.3;
  return report("subsonic", ok, "cuts off, 20/40/50 Hz = %.2f / %.2f / %.2f dB (<=-20, -3, -0.3 to -1)", g20, g40, g50);
}

// Steps from ON to ON+CUTS and back on a 40 Hz sine, which the high-pass takes 19 dB off, and measures the
// largest sample-to-sample step of the output around each switch against the sine's own largest step.
bool testCutsFade() {
  const int half = kSecond;
  std::vector<float> in = sine(40.0, 3 * half);
  cv::PolishParams on, cuts;
  on.cuts = false;
  cuts.cuts = true;
  gEq.reset();
  std::vector<float> out(in.size());
  const cv::PolishParams* settings[3] = {&on, &cuts, &on};
  for (int part = 0; part < 3; ++part) {
    for (int i = part * half; i < (part + 1) * half; i += cv::kBlock) {
      const int n = std::min(cv::kBlock, (part + 1) * half - i);
      gInProcess = true;
      gEq.processFront(&in[static_cast<size_t>(i)], &out[static_cast<size_t>(i)], n, *settings[part]);
      gInProcess = false;
    }
  }
  const double natural = kAmp * 2.0 * kPi * 40.0 / cv::kSampleRate;
  double worst = 0.0;
  for (int i = half - 100; i < 3 * half; ++i) {
    worst = std::max(worst, static_cast<double>(std::fabs(out[static_cast<size_t>(i)] - out[static_cast<size_t>(i - 1)])));
  }
  const double settled = rmsOf(out, half * 2 - 4800, half * 2) / rmsOf(in, half * 2 - 4800, half * 2);
  return report("cuts fade", worst <= 2.0 * natural && settled < 0.2,
                "ON to ON+CUTS to ON on 40 Hz: largest step %.5f against the sine's %.5f, ON+CUTS level %.3f of the input (<= 2x, < 0.2)",
                worst, natural, settled);
}

// Both halves in series on 40 Hz + 300 Hz + 6.5 kHz, stepped OFF, ON, ON+CUTS, OFF for 10 ms and back,
// OFF, and ON+CUTS again. Every switch fades the whole stage: the output's largest second difference, the
// change in its slope from one sample to the next, stays within 2x the input's own. The tones' own second
// difference stays small (the 6.5 kHz tone sits 34 dB under the others), so a hard switch in either half
// shows past the bound. The last switch-on comes after 750 ms OFF, and from it the output equals a fresh
// stage switched on at that sample, bit for bit, so settling OFF left no filter state behind.
std::vector<float> stageRun(cv::Polish& e, const std::vector<float>& in, int from, bool stepped) {
  struct Step {
    int at;
    bool on;
    bool cuts;
  };
  const Step steps[] = {{0, false, false},     {24000, true, false},  {48000, true, true},   {60000, false, true},
                        {60480, true, true},   {84000, false, false}, {120000, true, true}};
  const int total = static_cast<int>(in.size());
  std::vector<float> out(static_cast<size_t>(total - from));
  cv::PolishParams p;
  for (int i = from; i < total; i += cv::kBlock) {
    const int n = std::min(cv::kBlock, total - i);
    p.on = true;
    p.cuts = true;
    if (stepped) {
      for (const Step& s : steps) {
        if (i >= s.at) {
          p.on = s.on;
          p.cuts = s.cuts;
        }
      }
    }
    float* o = &out[static_cast<size_t>(i - from)];
    gInProcess = true;
    e.processFront(&in[static_cast<size_t>(i)], o, n, p);
    e.processBack(o, o, n, p);
    gInProcess = false;
  }
  return out;
}

bool testStageFade() {
  constexpr int kLastOn = 120000;
  std::vector<float> in(static_cast<size_t>(3 * kSecond));
  for (size_t i = 0; i < in.size(); ++i) {
    const double t = static_cast<double>(i) / cv::kSampleRate;
    in[i] = static_cast<float>(0.25 * (std::sin(2.0 * kPi * 40.0 * t) + std::sin(2.0 * kPi * 300.0 * t)) +
                               0.005 * std::sin(2.0 * kPi * 6500.0 * t));
  }
  gEq.reset();
  const std::vector<float> out = stageRun(gEq, in, 0, true);
  const auto bend = [](const std::vector<float>& x, size_t i) {
    return std::fabs(static_cast<double>(x[i]) - 2.0 * x[i - 1] + x[i - 2]);
  };
  double natural = 0.0, worst = 0.0;
  for (size_t i = 2; i < in.size(); ++i) {
    natural = std::max(natural, bend(in, i));
    worst = std::max(worst, bend(out, i));
  }
  cv::Polish fresh;
  const std::vector<float> clean = stageRun(fresh, in, kLastOn, false);
  bool same = true;
  for (size_t i = 0; i < clean.size(); ++i) same = same && out[static_cast<size_t>(kLastOn) + i] == clean[i];
  return report("stage fade", worst <= 2.0 * natural && same,
                "OFF/ON/ON+CUTS/OFF 10 ms/OFF/ON+CUTS: largest second difference %.5f against the input's %.5f (<= 2x); "
                "switch-on after OFF matches a fresh stage: %.0f (1)",
                worst, natural, same ? 1.0 : 0.0);
}

bool testFrontDeEsserQuiet() {
  const cv::PolishParams p;  // the de-esser at its tuning
  const double g97 = gainDb(Half::Front, 97.0, p), g1k = gainDb(Half::Front, 1000.0, p);
  return report("front voiced", std::fabs(g97) <= 0.5 && std::fabs(g1k) <= 0.1,
                "de-esser live: 97 Hz = %.3f dB, 1 kHz = %.3f dB (0 +-0.5, 0 +-0.1)", g97, g1k);
}

// A 194 Hz voice at -20 dBFS RMS under noise-like sibilance spread over 4.5 to 7 kHz at -26 dBFS RMS,
// made of 56 sines with fixed phases.
std::vector<float> voiceWithEss(int n) {
  std::vector<float> x = sine(194.0, n);
  const float voiceScale = 0.1f / kAmp * 1.41421356f;  // peak 0.1414, -20 dBFS RMS
  for (float& v : x) v *= voiceScale;
  uint32_t seed = 12345u;
  const int parts = 56;
  const double amp = std::pow(10.0, -26.0 / 20.0) * std::sqrt(2.0 / parts);
  for (int k = 0; k < parts; ++k) {
    seed = seed * 1664525u + 1013904223u;
    const double phase = 2.0 * kPi * (seed >> 8) / 16777216.0;
    const double hz = 4500.0 + 2500.0 * (k + 0.5) / parts;
    for (int i = 0; i < n; ++i)
      x[static_cast<size_t>(i)] += static_cast<float>(amp * std::sin(2.0 * kPi * hz * i / cv::kSampleRate + phase));
  }
  return x;
}

// RMS of what is left above 3 kHz, from two Butterworth high-pass sections, over seconds 1-2 of the signal.
double sibilantRms(const std::vector<float>& x) {
  cv::Biquad a, b;
  a.setHighPass(3000.0f, 0.5412f);
  b.setHighPass(3000.0f, 1.3066f);
  std::vector<float> hi(x.size());
  for (size_t i = 0; i < x.size(); ++i) hi[i] = b.run(a.run(x[i]));
  return rmsOf(hi, kSecond, 2 * kSecond);
}

// The de-esser as the box runs it, behind the 6 kHz low-pass, which takes 2 to 6 dB off the top of its
// band before the detector reads it. Sibilance still cuts by about 4.5 dB.
bool testFrontEss() {
  const std::vector<float> in = voiceWithEss(2 * kSecond);
  gEq.reset();
  const std::vector<float> off = run(gEq, Half::Front, in, filtersOnly());
  gEq.reset();
  const std::vector<float> on = run(gEq, Half::Front, in, cv::PolishParams{});
  const double cut = 20.0 * std::log10(sibilantRms(off) / sibilantRms(on));
  return report("front ess", cut >= 3.5 && cut <= 5.5, "band noise over a voice: the 3 kHz and up content cuts %.2f dB (4.5 +-1)", cut);
}

// The same voice at ON, where the de-esser reads the band at full level because the 6 kHz low-pass is out.
bool testFrontEssNoCuts() {
  const std::vector<float> in = voiceWithEss(2 * kSecond);
  cv::PolishParams ref = filtersOnly();
  ref.cuts = false;
  cv::PolishParams voiced;
  voiced.cuts = false;
  gEq.reset();
  const std::vector<float> off = run(gEq, Half::Front, in, ref);
  gEq.reset();
  const std::vector<float> on = run(gEq, Half::Front, in, voiced);
  const double cut = 20.0 * std::log10(sibilantRms(off) / sibilantRms(on));
  return report("front ess no cuts", cut >= 3.5 && cut <= 7.0, "same voice at ON: the 3 kHz and up content cuts %.2f dB (3.5 to 7)", cut);
}

// A call of any size gives the back half the same samples, because it holds no block state.
bool testBackCallSizes() {
  const std::vector<float> in = voiceWithEss(6 * cv::kBlock);
  const cv::PolishParams p;
  cv::Polish ref;
  std::vector<float> want(in.size());
  for (size_t i = 0; i < in.size(); i += cv::kBlock) ref.processBack(&in[i], &want[i], cv::kBlock, p);
  bool same = true;
  for (const int size : {1, 7, 63, 100}) {
    cv::Polish e;
    std::vector<float> got(in.size());
    for (size_t i = 0; i < in.size();) {
      const size_t n = std::min<size_t>(static_cast<size_t>(size), in.size() - i);
      e.processBack(&in[i], &got[i], static_cast<int>(n), p);
      i += n;
    }
    same = same && got == want;
  }
  return report("back call sizes", same, "calls of 1, 7, 63 and 100 samples match kBlock calls=%.0f (1)", same ? 1.0 : 0.0);
}

// The default settings sit inside the ranges, and a setting outside them or a NaN is refused.
bool testRanges() {
  cv::PolishParams p;
  bool ok = cv::polishParamsInRange(p);
  const float nan = std::nanf("");
  const auto refused = [](float cv::PolishParams::*field, float value) {
    cv::PolishParams q;
    q.*field = value;
    return !cv::polishParamsInRange(q);
  };
  ok = ok && refused(&cv::PolishParams::dipHz, 1000.0f) && refused(&cv::PolishParams::dipDb, nan) &&
       refused(&cv::PolishParams::presenceHz, 100.0f) && refused(&cv::PolishParams::presenceDb, 20.0f);
  p.tuning.trimDb = 400.0f;
  ok = ok && !cv::polishParamsInRange(p);
  p = cv::PolishParams{};
  p.tuning.deEsser.thresholdDb = -1000.0f;
  ok = ok && !cv::polishParamsInRange(p);
  return report("ranges", ok, "the default is in range and each setting is refused outside it=%.0f (1)", ok ? 1.0 : 0.0);
}

bool testBack() {
  // The shape is pinned without the level-rule trim.
  cv::PolishParams p;
  p.tuning.trimDb = 0.0f;
  const double g300 = gainDb(Half::Back, 300.0, p), g1k = gainDb(Half::Back, 1000.0, p);
  const double g35 = gainDb(Half::Back, 3500.0, p);
  const bool ok = std::fabs(g300 + 2.5) <= 0.4 && std::fabs(g1k) <= 0.4 && std::fabs(g35 - 1.5) <= 0.4;
  return report("back response", ok, "300/1k/3.5k Hz = %.2f / %.2f / %.2f dB (-2.5, 0, +1.5, each +-0.4)", g300,
                g1k, g35);
}

bool testFlat() {
  cv::PolishParams p;
  p.dipDb = 0.0f;
  p.presenceDb = 0.0f;
  p.tuning.trimDb = 0.0f;
  const double g = gainDb(Half::Back, 1000.0, p);
  return report("flat", std::fabs(g) <= 0.1, "back half, all gains 0: 1 kHz = %.3f dB (+-0.1)", g);
}

// A call of any n is the same as kBlock calls, because the front half splits at kBlock.
bool testSplit() {
  const std::vector<float> in = sine(5800.0, 4 * cv::kBlock + 8);
  const cv::PolishParams p;
  std::vector<float> whole(in.size()), parts(in.size());
  cv::Polish a, b;
  a.processFront(in.data(), whole.data(), static_cast<int>(in.size()), p);
  int done = 0;
  while (done < static_cast<int>(in.size())) {
    const int n = static_cast<int>(in.size()) - done < cv::kBlock ? static_cast<int>(in.size()) - done : cv::kBlock;
    b.processFront(&in[static_cast<size_t>(done)], &parts[static_cast<size_t>(done)], n, p);
    done += n;
  }
  float worst = 0.0f;
  for (size_t i = 0; i < in.size(); ++i) worst = std::fmax(worst, std::fabs(whole[i] - parts[i]));
  return report("split", worst == 0.0f, "one call against kBlock calls: max diff %.3g (0)", worst);
}

// In place gives the same samples as out of place.
bool testInPlace() {
  const std::vector<float> in = sine(700.0, 8 * cv::kBlock);
  const cv::PolishParams p;
  std::vector<float> apart(in.size()), inplace = in;
  cv::Polish a, b;
  for (int i = 0; i < static_cast<int>(in.size()); i += cv::kBlock) {
    a.processFront(&in[static_cast<size_t>(i)], &apart[static_cast<size_t>(i)], cv::kBlock, p);
    b.processFront(&inplace[static_cast<size_t>(i)], &inplace[static_cast<size_t>(i)], cv::kBlock, p);
    a.processBack(&apart[static_cast<size_t>(i)], &apart[static_cast<size_t>(i)], cv::kBlock, p);
    b.processBack(&inplace[static_cast<size_t>(i)], &inplace[static_cast<size_t>(i)], cv::kBlock, p);
  }
  return report("in place", apart == inplace, "both halves, in place against out of place: %.0f (1 = equal)",
                apart == inplace ? 1.0 : 0.0);
}

bool testNoAlloc() {
  const std::vector<float> in = sine(500.0, cv::kBlock * 8);
  gEq.reset();
  run(gEq, Half::Front, in, cv::PolishParams{});  // aborts on any new/delete inside process()
  run(gEq, Half::Back, in, cv::PolishParams{});
  return report("no-alloc", true, "%.0f allocations in processFront and processBack", 0.0);
}

}  // namespace

int main() {
  bool ok = true;
  ok &= testPassthrough();
  ok &= testFront();
  ok &= testFrontNoCuts();
  ok &= testSubsonic();
  ok &= testCutsFade();
  ok &= testStageFade();
  ok &= testFrontDeEsserQuiet();
  ok &= testFrontEss();
  ok &= testFrontEssNoCuts();
  ok &= testBackCallSizes();
  ok &= testRanges();
  ok &= testBack();
  ok &= testFlat();
  ok &= testSplit();
  ok &= testInPlace();
  ok &= testNoAlloc();
  return ok ? 0 : 1;
}
