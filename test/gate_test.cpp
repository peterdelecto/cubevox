// Gate checks: passthrough, closing, attack/release timing, hysteresis, the applied gain against
// libm, no-alloc, and the slot-1 Input gate's tuning (burst timing, noise stability, the pot ranges
// and the detector peak). No window, no audio device.

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <vector>

#include "core/chain_params.h"
#include "core/registry.h"
#include "engine/gate.h"

namespace {

std::atomic<bool> gInProcess{false};

void guardAlloc() {
  if (gInProcess.load()) std::abort();
}

}  // namespace

// Allocation in process() aborts the test binary.
void* operator new(std::size_t n) {
  guardAlloc();
  void* p = std::malloc(n);
  if (!p) std::abort();
  return p;
}
void* operator new[](std::size_t n) { return operator new(n); }
void operator delete(void* p) noexcept {
  guardAlloc();
  std::free(p);
}
void operator delete[](void* p) noexcept { operator delete(p); }
void operator delete(void* p, std::size_t) noexcept { operator delete(p); }
void operator delete[](void* p, std::size_t) noexcept { operator delete(p); }

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr int kMs = cv::kSampleRate / 1000;

float dbfs(double db) { return static_cast<float>(std::pow(10.0, db / 20.0)); }

// Feeds n samples one at a time. amp 0 is silence. Records the gain after each sample.
// Phase runs on across calls so a tone has no discontinuity.
class Rig {
 public:
  cv::Gate gate;
  long pos = 0;

  std::vector<float> feed(int n, double hz, float amp, const cv::GateParams& p,
                          std::vector<float>* in = nullptr, std::vector<float>* out = nullptr) {
    std::vector<float> g(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i, ++pos) {
      const float x = amp * static_cast<float>(std::sin(2.0 * kPi * hz * static_cast<double>(pos) / cv::kSampleRate));
      float y = 0.0f;
      gInProcess = true;
      gate.process(&x, &y, 1, p);
      gInProcess = false;
      g[static_cast<size_t>(i)] = gate.gainDb();
      if (in) in->push_back(x);
      if (out) out->push_back(y);
    }
    return g;
  }
};

// The physics checks run on fixed tuning so they do not follow default changes.
cv::GateParams fixedParams() {
  cv::GateParams p;
  p.thresholdDb = -40.0f;
  p.tuning.holdMs = 60.0f;
  p.tuning.releaseMs = 120.0f;
  p.tuning.rangeDb = -80.0f;
  p.tuning.kneeDb = 6.0f;
  p.tuning.hysteresisDb = 3.0f;
  return p;
}

bool report(const char* name, bool ok, const char* fmt, double a, double b = 0.0, double c = 0.0,
            double d = 0.0, double e = 0.0) {
  char detail[220];
  std::snprintf(detail, sizeof detail, fmt, a, b, c, d, e);
  std::printf("%s %s  %s\n", ok ? "PASS" : "FAIL", name, detail);
  return ok;
}

bool testPassthrough() {
  cv::GateParams off;
  off.on = false;
  Rig a;
  a.gate.reset();
  std::vector<float> in, out;
  a.feed(cv::kSampleRate, 1000.0, dbfs(-60.0), off, &in, &out);
  float offDiff = 0.0f;
  for (size_t i = static_cast<size_t>(100 * kMs); i < in.size(); ++i)
    offDiff = std::fmax(offDiff, std::fabs(out[i] - in[i]));

  cv::GateParams on;
  on.thresholdDb = -40.0f;
  Rig b;
  b.gate.reset();
  in.clear();
  out.clear();
  b.feed(cv::kSampleRate, 1000.0, dbfs(-30.0), on, &in, &out);
  float onDiff = 0.0f;
  for (size_t i = static_cast<size_t>(100 * kMs); i < in.size(); ++i)
    onDiff = std::fmax(onDiff, std::fabs(out[i] - in[i]));
  return report("passthrough", offDiff == 0.0f && onDiff < 1e-4f,
                "off max|out-in|=%.3g (0); on, +10 dB over threshold, max|out-in|=%.3g (<1e-4)", offDiff,
                onDiff);
}

bool testCloses() {
  cv::GateParams p = fixedParams();
  Rig r;
  r.gate.reset();
  const std::vector<float> g = r.feed(2 * cv::kSampleRate, 1000.0, dbfs(-60.0), p);
  const float last = g.back();
  return report("closes", last <= -70.0f, "1 kHz -60 dBFS, thr -40: steady gain=%.2f dB (<=-70)", last);
}

// First sample index in g at or past `from` where pred holds, else -1.
template <class F>
int firstWhere(const std::vector<float>& g, int from, F pred) {
  for (size_t i = static_cast<size_t>(from); i < g.size(); ++i)
    if (pred(g[i])) return static_cast<int>(i);
  return -1;
}

bool testTiming() {
  cv::GateParams p = fixedParams();
  const cv::GateTuning& t = p.tuning;
  Rig r;
  r.gate.reset();
  r.feed(2 * cv::kSampleRate, 1000.0, 0.0f, p);  // settle closed

  // Attack: tone at -20 dBFS.
  const std::vector<float> up = r.feed(500 * kMs, 1000.0, dbfs(-20.0), p);
  const int tAtt = firstWhere(up, 0, [](float g) { return g >= -1.0f; });
  const double attMs = tAtt < 0 ? -1.0 : static_cast<double>(tAtt) / kMs;
  const bool attOk = tAtt >= 0 && attMs <= static_cast<double>(t.attackMs) + 5.0;

  // Release: back to silence. The detector falls from -20 dBFS to the close
  // level before the hold timer starts, so that fall is part of the expected delay.
  const std::vector<float> dn = r.feed(2 * cv::kSampleRate, 1000.0, 0.0f, p);
  const int tLeave = firstWhere(dn, 0, [](float g) { return g < -1.0f; });
  const double holdMs = tLeave < 0 ? -1.0 : static_cast<double>(tLeave) / kMs;
  const double detFallMs = 20.0 * std::log(std::pow(10.0, ((p.thresholdDb - t.hysteresisDb) - -20.0) / -20.0));
  const double lo = static_cast<double>(t.holdMs) - 10.0;
  const double hi = static_cast<double>(t.holdMs) + detFallMs + 10.0;
  const bool holdOk = tLeave >= 0 && holdMs >= lo && holdMs <= hi;
  const int tRel = firstWhere(dn, 0, [&](float g) { return g <= t.rangeDb + 6.0f; });
  const double relMs = tRel < 0 ? -1.0 : static_cast<double>(tRel) / kMs - holdMs;
  const bool relOk = tRel >= 0 && relMs <= 3.0 * static_cast<double>(t.releaseMs);
  report("timing attack", attOk, "gain >= -1 dB after %.2f ms (<= attack+5 = %.1f)", attMs, t.attackMs + 5.0);
  report("timing hold", holdOk, "gain < -1 dB after %.1f ms (hold %.0f -10 .. +detector fall %.1f +10 = %.1f)",
         holdMs, t.holdMs, detFallMs, hi);
  return report("timing release", relOk, "range+6 dB reached %.1f ms after the drop (<= 3*release = %.0f)",
                relMs, 3.0 * t.releaseMs) && attOk && holdOk;
}

bool testHysteresis() {
  cv::GateParams p = fixedParams();  // threshold -40, hysteresis 3
  Rig r;
  r.gate.reset();
  r.feed(500 * kMs, 1000.0, dbfs(-20.0), p);  // open
  const std::vector<float> stay = r.feed(1000 * kMs, 1000.0, dbfs(-41.0), p);
  float minStay = 0.0f;
  for (float g : stay) minStay = std::fmin(minStay, g);
  const std::vector<float> shut = r.feed(1500 * kMs, 1000.0, dbfs(-44.0), p);
  const float endShut = shut.back();
  return report("hysteresis", minStay > -1.0f && endShut < -10.0f,
                "-41 dBFS after open: min gain %.2f dB (> -1); -44 dBFS: gain %.2f dB (< -10)", minStay,
                endShut);
}

bool testNoAlloc() {
  cv::GateParams p;
  Rig r;
  r.gate.reset();
  r.feed(cv::kBlock * 8, 500.0, dbfs(-30.0), p);  // aborts on any new/delete inside process()
  return report("no-alloc", true, "%.0f allocations in process()", 0.0);
}

// The gain runs through dbToGainFast. Through an open, a hold and a close to the -80 dB range,
// every output sample sits within 1e-6 relative of the input times libm's 10^(gainDb / 20).
bool testGainMatchesLibm() {
  cv::GateParams p = fixedParams();
  Rig r;
  r.gate.reset();
  std::vector<float> in, out;
  std::vector<float> g = r.feed(500 * kMs, 1000.0, dbfs(-20.0), p, &in, &out);
  const std::vector<float> shut = r.feed(2 * cv::kSampleRate, 1000.0, dbfs(-60.0), p, &in, &out);
  g.insert(g.end(), shut.begin(), shut.end());
  double worst = 0.0;
  for (size_t i = 0; i < in.size(); ++i) {
    const double want = static_cast<double>(in[i]) * std::pow(10.0, static_cast<double>(g[i]) / 20.0);
    if (want != 0.0) worst = std::fmax(worst, std::fabs(static_cast<double>(out[i]) - want) / std::fabs(want));
  }
  const float lowest = *std::min_element(g.begin(), g.end());
  return report("gain vs libm", worst <= 1e-6 && lowest <= -79.0f,
                "worst relative error %.3g (<= 1e-6) with the gain down to %.1f dB (<= -79)", worst, lowest);
}

// ---- The slot-1 Input gate ------------------------------------------------------------------------

// The Input gate as the box sets it: the registry's pot mapping on top of the chain defaults.
cv::GateParams boxInputGate(float thresholdPos, float decayPos) {
  ChainParams chain;
  applyKnob(Card::InputGate, 0, thresholdPos, chain);
  applyKnob(Card::InputGate, 1, decayPos, chain);
  return chain.inputGate;
}

// One gate fed a sample at a time.
class Probe {
 public:
  cv::Gate gate;

  float step(float x, const cv::GateParams& p) {
    float y = 0.0f;
    gInProcess = true;
    gate.process(&x, &y, 1, p);
    gInProcess = false;
    return y;
  }

  // Silence until the gain sits at the closed range.
  bool settleClosed(const cv::GateParams& p) {
    for (int i = 0; i < 40 * cv::kSampleRate; ++i) {
      if (gate.gainDb() <= -79.9f) return true;
      step(0.0f, p);
    }
    return false;
  }
};

const char* kPotName[] = {"DECAY 0", "DECAY 0.5", "DECAY 1"};

// The pots: THRESHOLD -80 to -30 dBFS shown in whole dB, DECAY 20 ms to 2 s log, and the tuning the
// pots do not reach.
bool testInputGatePots() {
  const KnobDesc& thr = *knobDesc(Card::InputGate, 0);
  const KnobDesc& dec = *knobDesc(Card::InputGate, 1);
  const double lo = cv::kInputGateDecayMinMs;
  const double hi = cv::kInputGateDecayMaxMs;
  bool decayOk = true;
  double mapped[3] = {};
  const float pos[3] = {0.0f, 0.5f, 1.0f};
  for (int i = 0; i < 3; ++i) {
    ChainParams chain;
    applyKnob(Card::InputGate, 1, pos[i], chain);
    mapped[i] = static_cast<double>(chain.inputGate.tuning.releaseMs);
    const double want = lo * std::pow(hi / lo, static_cast<double>(pos[i]));
    decayOk = decayOk && std::fabs(mapped[i] - want) <= 1e-3 * want && std::fabs(static_cast<double>(knobValue(dec, pos[i])) - want) <= 1e-3 * want;
  }
  report("input gate DECAY", decayOk && std::fabs(mapped[0] - 20.0) < 0.05 && std::fabs(mapped[2] - 2000.0) < 5.0,
         "DECAY at 0, 0.5, 1 maps to %.1f, %.1f, %.1f ms (20, 200, 2000 log)", mapped[0], mapped[1], mapped[2]);

  const bool thresholdOk = knobShown(thr, 0.0f) == -80 && knobShown(thr, 1.0f) == -30 && knobShown(thr, 0.5f) == -55 &&
                           thr.decimals == 0 && thr.curve == Curve::Linear;
  ChainParams chain;
  applyKnob(Card::InputGate, 0, 0.0f, chain);
  const double atMin = static_cast<double>(chain.inputGate.thresholdDb);
  applyKnob(Card::InputGate, 0, 1.0f, chain);
  const double atMax = static_cast<double>(chain.inputGate.thresholdDb);
  report("input gate THRESHOLD", thresholdOk && std::fabs(atMin + 80.0) < 1e-4 && std::fabs(atMax + 30.0) < 1e-4,
         "THRESHOLD maps %.1f to %.1f dBFS, linear, shown in whole dB", atMin, atMax);

  const cv::GateTuning t = boxInputGate(0.5f, 0.5f).tuning;
  const bool tuningOk = t.rangeDb == -80.0f && t.kneeDb == 0.0f && t.hysteresisDb == 3.0f && t.holdMs == 50.0f &&
                        t.attackMs == 0.5f && t.detectorAttackMs == 0.1f && t.detectorReleaseMs == 5.0f &&
                        t.detectorHpHz == 0.0f && t.travelTimes;
  report("input gate tuning", tuningOk,
         "range %.0f dB, hysteresis %.0f dB, hold %.0f ms, attack %.1f ms (hard range, 3 dB, 50 ms, 0.5 ms)",
         static_cast<double>(t.rangeDb), static_cast<double>(t.hysteresisDb), static_cast<double>(t.holdMs),
         static_cast<double>(t.attackMs));

  ChainParams fresh;
  const cv::GateTuning& g = fresh.gate.tuning;
  const cv::GateTuning def;
  const bool panelOk = g.kneeDb == def.kneeDb && g.holdMs == def.holdMs && g.rangeDb == def.rangeDb &&
                       g.hysteresisDb == def.hysteresisDb && g.detectorHpHz == 120.0f && g.detectorAttackMs == 0.0f &&
                       g.detectorReleaseMs == 20.0f && !g.travelTimes && t.rangeDb != g.rangeDb;
  return report("panel gate tuning", panelOk, "the panel GATE keeps its soft knee %.0f dB and its range %.0f dB",
                static_cast<double>(g.kneeDb), static_cast<double>(g.rangeDb)) &&
         decayOk && thresholdOk && tuningOk;
}

// A 1 kHz burst 10 dB over the threshold: the gain is within 1 dB of open inside 1 ms of the onset,
// the output peak of the last half cycle inside that millisecond is within 1 dB of the input's, and
// after the burst the gain is under -60 dB within hold + release + 20 ms.
bool testInputGateBurst() {
  bool ok = true;
  double worstOpenMs = 0.0;
  double worstOutDb = 0.0;
  double worstCloseOverMs = -1e9;
  for (const float thrPos : {0.1f, 0.52f, 0.9f}) {
    for (int d = 0; d < 3; ++d) {
      const float decayPos[3] = {0.0f, 0.5f, 1.0f};
      const cv::GateParams p = boxInputGate(thrPos, decayPos[d]);
      Probe r;
      r.gate.reset();
      ok = ok && r.settleClosed(p);
      const double amp = static_cast<double>(dbfs(static_cast<double>(p.thresholdDb) + 10.0));
      int open = -1;
      double outPeak = 0.0;
      float gainEnd = -80.0f;
      const int burst = 300 * kMs;
      for (int n = 0; n < burst; ++n) {
        const float x = static_cast<float>(amp * std::sin(2.0 * kPi * 1000.0 * n / cv::kSampleRate));
        const float y = r.step(x, p);
        if (open < 0 && r.gate.gainDb() >= -1.0f) open = n;
        if (n >= kMs / 2 && n < kMs) outPeak = std::fmax(outPeak, std::fabs(static_cast<double>(y)));
        gainEnd = r.gate.gainDb();
      }
      int below = -1;
      const double limitMs = static_cast<double>(p.tuning.holdMs) + static_cast<double>(p.tuning.releaseMs) + 20.0;
      for (int n = 0; n < static_cast<int>((limitMs + 3000.0) * kMs) && below < 0; ++n) {
        r.step(0.0f, p);
        if (r.gate.gainDb() <= -60.0f) below = n;
      }
      const double openMs = open < 0 ? 1e9 : static_cast<double>(open) / kMs;
      const double outDb = 20.0 * std::log10(outPeak / amp);
      const double closeMs = below < 0 ? 1e9 : static_cast<double>(below) / kMs;
      worstOpenMs = std::fmax(worstOpenMs, openMs);
      worstOutDb = std::fmin(worstOutDb, outDb);
      worstCloseOverMs = std::fmax(worstCloseOverMs, closeMs - limitMs);
      ok = ok && open >= 0 && openMs <= 1.0 && outDb >= -1.0 && gainEnd > -0.01f && below >= 0 && closeMs <= limitMs;
      std::printf("TABLE burst thr %+5.0f dB %-9s within 1 dB of open at %.3f ms, last half cycle %.2f dB, under -60 dB %.1f ms after the burst (limit %.0f)\n",
                  static_cast<double>(p.thresholdDb), kPotName[d], openMs, outDb, closeMs, limitMs);
    }
  }
  return report("input gate burst", ok,
                "worst: within 1 dB at %.3f ms (<= 1), output %.2f dB (>= -1), under -60 dB %.1f ms inside the limit",
                worstOpenMs, worstOutDb, -worstCloseOverMs);
}

// Binary white noise (every sample +-1) read by a peak detector shows its amplitude exactly, so a
// level in dB is the level the gate sees. A slow wobble of +-1.5 dB sits on it, from three sines that
// do not repeat inside the run.
class WobbleNoise {
 public:
  explicit WobbleNoise(double centerDb) : centerDb_(centerDb) {}

  float next() {
    s_ = s_ * 6364136223846793005ULL + 1442695040888963407ULL;
    const double t = static_cast<double>(n_++) / cv::kSampleRate;
    const double w = (std::sin(2.0 * kPi * 3.1 * t) + std::sin(2.0 * kPi * 7.3 * t + 1.0) + std::sin(2.0 * kPi * 11.7 * t + 2.0)) / 3.0;
    const double db = centerDb_ + 1.5 * w;
    return static_cast<float>(((s_ >> 40) & 1ULL ? 1.0 : -1.0) * std::pow(10.0, db / 20.0));
  }

 private:
  double centerDb_;
  unsigned long long s_ = 0x9E3779B97F4A7C15ULL;
  long n_ = 0;
};

struct Excursion {
  float minGain = 0.0f;
  float maxGain = -80.0f;
  int risesToOpen = 0;  // crossings of -40 dB upward
  int fallsToClosed = 0;
};

// Feeds 10 s of wobbling noise and records how the gain moved.
Excursion runNoise(Probe& r, const cv::GateParams& p, double centerDb) {
  WobbleNoise noise(centerDb);
  Excursion e;
  e.minGain = r.gate.gainDb();
  e.maxGain = r.gate.gainDb();
  bool wasOpen = r.gate.gainDb() > -40.0f;
  for (int n = 0; n < 10 * cv::kSampleRate; ++n) {
    r.step(noise.next(), p);
    const float g = r.gate.gainDb();
    e.minGain = std::fmin(e.minGain, g);
    e.maxGain = std::fmax(e.maxGain, g);
    const bool open = g > -40.0f;
    if (open && !wasOpen) ++e.risesToOpen;
    if (!open && wasOpen) ++e.fallsToClosed;
    wasOpen = open;
  }
  return e;
}

// Noise whose level wobbles +-1.5 dB, which is the width of the 3 dB hysteresis, leaves the gate
// where it was for 10 s, open or closed. The same noise 2 dB over the threshold opens it once and it
// stays open.
bool testInputGateNoise() {
  const cv::GateParams p = boxInputGate(0.52f, 0.0f);  // -54 dBFS, the shortest release
  const double thr = static_cast<double>(p.thresholdDb);
  bool ok = true;

  Probe open;
  open.gate.reset();
  for (int n = 0; n < cv::kSampleRate / 2; ++n) open.step(static_cast<float>(dbfs(thr + 6.0)), p);  // 6 dB over: open
  const Excursion held = runNoise(open, p, thr);
  report("input gate noise open", held.minGain >= -0.001f && held.fallsToClosed == 0,
         "open, noise at threshold +-1.5 dB for 10 s: gain min %.4f dB, %.0f closings (0)", static_cast<double>(held.minGain), static_cast<double>(held.fallsToClosed));
  ok = ok && held.minGain >= -0.001f && held.fallsToClosed == 0;

  Probe closed;
  closed.gate.reset();
  ok = ok && closed.settleClosed(p);
  const Excursion shut = runNoise(closed, p, thr - 2.0);
  report("input gate noise closed", shut.maxGain <= -79.9f && shut.risesToOpen == 0,
         "closed, noise at threshold -2 dB +-1.5 dB for 10 s: gain max %.3f dB, %.0f openings (0); a hard range never lets a partial gain through",
         static_cast<double>(shut.maxGain), static_cast<double>(shut.risesToOpen));
  ok = ok && shut.maxGain <= -79.9f && shut.risesToOpen == 0;

  Probe over;
  over.gate.reset();
  ok = ok && over.settleClosed(p);
  const Excursion up = runNoise(over, p, thr + 2.0);
  report("input gate noise over", up.risesToOpen == 1 && up.fallsToClosed == 0 && over.gate.gainDb() > -0.001f,
         "noise at threshold +2 dB +-1.5 dB: %.0f opening, %.0f closings, final gain %.4f dB (1, 0, 0)",
         static_cast<double>(up.risesToOpen), static_cast<double>(up.fallsToClosed), static_cast<double>(over.gate.gainDb()));
  ok = ok && up.risesToOpen == 1 && up.fallsToClosed == 0 && over.gate.gainDb() > -0.001f;
  return ok;
}

// Gaussian white noise: the detector peak over 2 s is the floor, and a threshold 8 dB above it
// keeps the gate shut for the next 10 s. The peak window restarts on each take.
bool testInputGateFloorMargin() {
  class Gauss {
   public:
    float next() {
      const double a = u();
      const double b = u();
      return static_cast<float>(std::sqrt(-2.0 * std::log(a > 1e-12 ? a : 1e-12)) * std::cos(2.0 * kPi * b));
    }

   private:
    double u() {
      s_ = s_ * 6364136223846793005ULL + 1442695040888963407ULL;
      return static_cast<double>(s_ >> 11) / 9007199254740992.0;
    }
    unsigned long long s_ = 0x2545F4914F6CDD1DULL;
  };

  const float rms = dbfs(-62.0);
  Gauss noise;
  cv::GateParams p = boxInputGate(1.0f, 0.0f);
  p.thresholdDb = 0.0f;  // the measuring pass: never opens
  Probe r;
  r.gate.reset();
  for (int n = 0; n < 2 * cv::kSampleRate; ++n) r.step(rms * noise.next(), p);
  const float peak = r.gate.takeDetectorPeak();
  const float again = r.gate.takeDetectorPeak();
  const double floorDb = 20.0 * std::log10(static_cast<double>(peak));
  p.thresholdDb = static_cast<float>(floorDb + 8.0);
  float maxGain = -80.0f;
  for (int n = 0; n < 10 * cv::kSampleRate; ++n) {
    r.step(rms * noise.next(), p);
    maxGain = std::fmax(maxGain, r.gate.gainDb());
  }
  const double second = 20.0 * std::log10(static_cast<double>(r.gate.takeDetectorPeak()));
  const bool ok = peak > 0.0f && again == 0.0f && floorDb > -62.0 && floorDb < -45.0 && maxGain < -79.9f && second < floorDb + 8.0;
  return report("input gate floor margin", ok,
                "white noise at -62 dB rms: floor %.1f dBFS over 2 s, threshold floor + 8 keeps the gate shut (max gain %.1f dB), the next 10 s peak %.1f dBFS",
                floorDb, static_cast<double>(maxGain), second);
}

// The detector peak is a plain linear value the interrupt can keep: no division, restarted on a take.
bool testDetectorPeakWindow() {
  Probe r;
  r.gate.reset();
  cv::GateParams p = boxInputGate(0.5f, 0.0f);
  for (int n = 0; n < 480; ++n) r.step(static_cast<float>(0.25 * std::sin(2.0 * kPi * 1000.0 * n / cv::kSampleRate)), p);
  const float first = r.gate.takeDetectorPeak();
  for (int n = 0; n < 480; ++n) r.step(0.0f, p);
  const float second = r.gate.takeDetectorPeak();
  const bool ok = first > 0.2f && first <= 0.25f && second < first && second > 0.0f && r.gate.takeDetectorPeak() == 0.0f;
  return report("detector peak", ok, "a 0.25 sine reads a peak of %.3f, the next window %.3f, then 0", static_cast<double>(first),
                static_cast<double>(second));
}

}  // namespace

int main() {
  bool ok = true;
  ok &= testPassthrough();
  ok &= testCloses();
  ok &= testTiming();
  ok &= testHysteresis();
  ok &= testGainMatchesLibm();
  ok &= testNoAlloc();
  ok &= testInputGatePots();
  ok &= testInputGateBurst();
  ok &= testInputGateNoise();
  ok &= testInputGateFloorMargin();
  ok &= testDetectorPeakWindow();
  return ok ? 0 : 1;
}
