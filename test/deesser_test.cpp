// De-esser checks: detector band, trigger and cap, release, floor, silence, call splitting and the
// setting ranges, no-alloc. No window, no audio device.

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "engine/deesser.h"

#include "alloc_guard.h"
#include "signal_util.h"

using testutil::dbToLin;

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr int kSecond = cv::kSampleRate;

struct Tone {
  double hz;
  double dbfs;  // peak level
};

// A sum of tones, `n` samples from sample `start`.
std::vector<float> tones(const std::vector<Tone>& parts, int start, int n) {
  std::vector<float> x(static_cast<size_t>(n), 0.0f);
  for (const Tone& t : parts) {
    const double amp = dbToLin(t.dbfs);
    for (int i = 0; i < n; ++i) {
      x[static_cast<size_t>(i)] +=
          static_cast<float>(amp * std::sin(2.0 * kPi * t.hz * (start + i) / cv::kSampleRate));
    }
  }
  return x;
}

// What one run saw: the cut in dB averaged and at its peak over the measured stretch, and the RMS
// gain of the de-esser over the same stretch.
struct Run {
  double meanCut = 0.0;
  double maxCut = 0.0;
  double gainDb = 0.0;
};

// Runs `seconds` of the tones through a fresh de-esser in kBlock chunks and measures the last
// second. `tuning` goes to every call.
Run runTones(const std::vector<Tone>& parts, const cv::DeEsserTuning& tuning, int seconds = 2) {
  static cv::DeEsser d;
  d.reset();
  const int total = seconds * kSecond;
  const int from = total - kSecond;
  const std::vector<float> in = tones(parts, 0, total);
  std::vector<float> out = in;
  Run r;
  double sumIn = 0.0, sumOut = 0.0, sumCut = 0.0;
  int blocks = 0;
  for (int i = 0; i < total; i += cv::kBlock) {
    gInProcess = true;
    d.process(&out[static_cast<size_t>(i)], cv::kBlock, tuning);
    gInProcess = false;
    if (i < from) continue;
    sumCut += d.cutDb();
    if (d.cutDb() > r.maxCut) r.maxCut = d.cutDb();
    ++blocks;
    for (int k = i; k < i + cv::kBlock; ++k) {
      sumIn += static_cast<double>(in[static_cast<size_t>(k)]) * in[static_cast<size_t>(k)];
      sumOut += static_cast<double>(out[static_cast<size_t>(k)]) * out[static_cast<size_t>(k)];
    }
  }
  r.meanCut = sumCut / blocks;
  r.gainDb = 10.0 * std::log10(sumOut / sumIn);
  return r;
}

bool report(const char* name, bool ok, const char* fmt, double a = 0.0, double b = 0.0, double c = 0.0) {
  char detail[200];
  std::snprintf(detail, sizeof detail, fmt, a, b, c);
  std::printf("%s %s  %s\n", ok ? "PASS" : "FAIL", name, detail);
  return ok;
}

// The band-pass gain at `hz`. A threshold far below the signal and an uncapped, instant cut turn
// the cut into the band level minus the full level plus 100 dB, which for one tone is the band's gain.
double bandGainDb(double hz) {
  cv::DeEsserTuning t;
  t.thresholdDb = -100.0f;
  t.maxCutDb = 200.0f;
  t.releaseMs = 0.1f;
  return runTones({{hz, -20.0}}, t).meanCut - 100.0;
}

// The band edges sit at -3 dB and the centre at 0 dB. The tones at 2 kHz and 12 kHz stay loud
// enough after the band-pass to read above the -60 dBFS floor.
bool testBand() {
  const double g45 = bandGainDb(4500.0), g58 = bandGainDb(5800.0), g70 = bandGainDb(7000.0);
  const double g2k = bandGainDb(2000.0), g12k = bandGainDb(12000.0);
  const bool ok = std::fabs(g45 + 3.0) <= 0.3 && std::fabs(g70 + 3.0) <= 0.3 && std::fabs(g58) <= 0.3 &&
                  g2k <= -20.0 && g12k <= -25.0;
  char detail[200];
  std::snprintf(detail, sizeof detail,
                "4.5k/5.8k/7k = %.2f / %.2f / %.2f dB (-3, 0, -3), 2k %.1f, 12k %.1f dB (<=-20, <=-25)", g45, g58,
                g70, g2k, g12k);
  std::printf("%s band  %s\n", ok ? "PASS" : "FAIL", detail);
  return ok;
}

bool testVoiced() {
  const Run r = runTones({{1000.0, -20.0}}, cv::DeEsserTuning{});
  return report("voiced", r.maxCut < 0.1 && std::fabs(r.gainDb) < 0.1,
                "1 kHz at -20 dBFS: peak cut %.3f dB, gain %.3f dB (<0.1)", r.maxCut, r.gainDb);
}

bool testEss() {
  const Run r = runTones({{5800.0, -20.0}}, cv::DeEsserTuning{});
  return report("ess alone", std::fabs(r.meanCut - 6.0) <= 0.3 && std::fabs(r.gainDb + 6.0) <= 0.3,
                "5.8 kHz at -20 dBFS: cut %.2f dB, gain %.2f dB (6.0 +-0.3)", r.meanCut, r.gainDb);
}

// The band is 10.4 dB under the full level, so the static cut is 1.59 dB. A block of 64 samples holds
// 1.33 cycles of the 1 kHz tone, so its block RMS ripples by about 0.5 dB, and the instant attack
// with a 40 ms release holds the cut near the top of that ripple. The bound runs from 1.1 dB to 2.2 dB.
bool testEssOverVoice() {
  const Run r = runTones({{5800.0, -30.0}, {1000.0, -20.0}}, cv::DeEsserTuning{});
  return report("ess over voice", r.meanCut >= 1.1 && r.meanCut <= 2.2,
                "5.8 kHz at -30 dBFS over 1 kHz at -20 dBFS: cut %.2f dB (static 1.59, bound 1.1 to 2.2)", r.meanCut);
}

// Half a second of ess, then `after` playing on. Returns the cut `afterMs` after the ess ended.
double cutAfterEss(const std::vector<Tone>& after, int afterMs) {
  cv::DeEsser d;
  const cv::DeEsserTuning t;
  const int essLen = kSecond / 2;
  const int total = essLen + afterMs * kSecond / 1000;
  std::vector<float> x = tones({{5800.0, -20.0}}, 0, essLen);
  const std::vector<float> tail = tones(after, essLen, total - essLen);
  x.insert(x.end(), tail.begin(), tail.end());
  x.resize((x.size() + cv::kBlock - 1) / cv::kBlock * cv::kBlock, 0.0f);
  for (int i = 0; i < static_cast<int>(x.size()); i += cv::kBlock) {
    d.process(&x[static_cast<size_t>(i)], cv::kBlock, t);
  }
  return d.cutDb();
}

bool testRelease() {
  const double silent = cutAfterEss({}, 100);
  const double voiced = cutAfterEss({{1000.0, -20.0}}, 100);
  return report("release", silent < 1.0 && voiced < 1.0,
                "cut 100 ms after the ess: silence %.2f dB, 1 kHz voice %.2f dB (<1)", silent, voiced);
}

bool testFloor() {
  const Run r = runTones({{5800.0, -70.0}}, cv::DeEsserTuning{});
  return report("floor", r.maxCut == 0.0, "5.8 kHz at -70 dBFS alone: peak cut %.3f dB (0)", r.maxCut);
}

bool finiteAndNormal(const std::vector<float>& x) {
  for (float v : x) {
    if (std::isnan(v) || std::fpclassify(v) == FP_SUBNORMAL) return false;
  }
  return true;
}

bool testSilence() {
  cv::DeEsser d;
  const cv::DeEsserTuning t;
  std::vector<float> zeros(static_cast<size_t>(cv::kBlock), 0.0f);
  bool ok = true;
  for (int i = 0; i < 400; ++i) {
    d.process(zeros.data(), cv::kBlock, t);
    ok = ok && finiteAndNormal(zeros) && d.cutDb() == 0.0f;
  }
  // After a burst and then silence, the filters ring down without a subnormal and the cut ends at 0.
  std::vector<float> burst = tones({{5800.0, -20.0}}, 0, kSecond / 10);
  bool tailOk = true;
  for (int i = 0; i + cv::kBlock <= static_cast<int>(burst.size()); i += cv::kBlock) {
    d.process(&burst[static_cast<size_t>(i)], cv::kBlock, t);
  }
  for (int i = 0; i < 1000; ++i) {
    zeros.assign(static_cast<size_t>(cv::kBlock), 0.0f);
    d.process(zeros.data(), cv::kBlock, t);
    tailOk = tailOk && finiteAndNormal(zeros);
  }
  return report("silence", ok && tailOk && d.cutDb() == 0.0f,
                "zeros stay zero, the ring-down has no NaN or subnormal, final cut %.3f dB (0)", d.cutDb());
}

bool testEdges() {
  cv::DeEsser d;
  const cv::DeEsserTuning t;
  std::vector<float> x = tones({{5800.0, -20.0}}, 0, 80);
  d.process(x.data(), 0, t);
  d.process(x.data(), 40, t);
  const float open = d.cutDb();  // the window is 40 samples in, so nothing has moved
  d.process(x.data() + 40, 40, t);  // the second 40 samples close the window
  return report("short call", finiteAndNormal(x) && open == 0.0f && d.cutDb() > 0.0f,
                "40 samples leave the cut at %.2f dB, 80 samples move it to %.2f dB", open, d.cutDb());
}

// Ess over voice for 20 blocks, voice alone for 20, twice. The cut rises and falls.
std::vector<float> essBursts() {
  std::vector<float> x;
  for (int rep = 0; rep < 2; ++rep) {
    const std::vector<float> loud = tones({{5800.0, -20.0}, {1000.0, -20.0}}, static_cast<int>(x.size()), 20 * cv::kBlock);
    x.insert(x.end(), loud.begin(), loud.end());
    const std::vector<float> voice = tones({{1000.0, -20.0}}, static_cast<int>(x.size()), 20 * cv::kBlock);
    x.insert(x.end(), voice.begin(), voice.end());
  }
  return x;
}

// The detector windows follow the stream, so the cut after each 64 samples is the same whether the
// caller makes one call of 64 or two calls that split the block.
bool testCallSplit() {
  const std::vector<float> in = essBursts();
  const cv::DeEsserTuning t;
  const int blocks = static_cast<int>(in.size()) / cv::kBlock;
  std::vector<float> want;
  {
    cv::DeEsser d;
    std::vector<float> x = in;
    for (int b = 0; b < blocks; ++b) {
      d.process(&x[static_cast<size_t>(b * cv::kBlock)], cv::kBlock, t);
      want.push_back(d.cutDb());
    }
  }
  bool equal = true;
  double top = 0.0;
  for (const float c : want) top = std::fmax(top, c);
  for (const int first : {1, 7, 32, 63}) {
    cv::DeEsser d;
    std::vector<float> x = in;
    for (int b = 0; b < blocks; ++b) {
      float* p = &x[static_cast<size_t>(b * cv::kBlock)];
      d.process(p, first, t);
      d.process(p + first, cv::kBlock - first, t);
      equal = equal && d.cutDb() == want[static_cast<size_t>(b)];
    }
  }
  return report("call split", equal && top > 1.0,
                "the cut after every block is the same for calls split 1/63, 7/57, 32/32, 63/1 (=%.0f), peak cut %.2f dB (>1)",
                equal ? 1.0 : 0.0, top);
}

// The tuning default sits inside the ranges, and each field fails alone outside them or as a NaN.
bool testRanges() {
  const cv::DeEsserTuning base;
  bool ok = cv::deEsserTuningInRange(base);
  const float nan = std::nanf("");
  const auto bad = [&](float cv::DeEsserTuning::*field, float value) {
    cv::DeEsserTuning t;
    t.*field = value;
    return !cv::deEsserTuningInRange(t);
  };
  ok = ok && bad(&cv::DeEsserTuning::bandLoHz, 5.0f) && bad(&cv::DeEsserTuning::bandHiHz, 30000.0f) &&
       bad(&cv::DeEsserTuning::thresholdDb, -1000.0f) && bad(&cv::DeEsserTuning::maxCutDb, 1000.0f) &&
       bad(&cv::DeEsserTuning::releaseMs, 0.0f) && bad(&cv::DeEsserTuning::notchHz, nan) &&
       bad(&cv::DeEsserTuning::notchQ, 0.0f);
  return report("ranges", ok, "the default is in range and each setting is refused outside it=%.0f (1)", ok ? 1.0 : 0.0);
}

bool testNoAlloc() {
  cv::DeEsser d;
  std::vector<float> x = tones({{5800.0, -20.0}}, 0, cv::kBlock * 8);
  for (int i = 0; i < 8; ++i) {
    gInProcess = true;
    d.process(&x[static_cast<size_t>(i * cv::kBlock)], cv::kBlock, cv::DeEsserTuning{});
    gInProcess = false;
  }
  return report("no-alloc", true, "%.0f allocations in process()", 0.0);
}

}  // namespace

int main() {
  bool ok = true;
  ok &= testBand();
  ok &= testVoiced();
  ok &= testEss();
  ok &= testEssOverVoice();
  ok &= testRelease();
  ok &= testFloor();
  ok &= testSilence();
  ok &= testEdges();
  ok &= testCallSplit();
  ok &= testRanges();
  ok &= testNoAlloc();
  return ok ? 0 : 1;
}
