// Chasm checks: the Reverb block's off and on edges, decay across the TIME knob, chirp direction,
// wobble, wobble level, stability at the box's gain and top TIME, out-of-range tuning,
// the size bound and no-alloc. No window, no audio device. Every limit sits about twice the measured
// value, and the measured value is in the report line.

#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <vector>

#include "engine/chasm.h"
#include "engine/reverb.h"

#include "alloc_guard.h"
#include "signal_util.h"

namespace {

using namespace testutil;

constexpr int kSr = cv::kSampleRate;

size_t at(int i) { return static_cast<size_t>(i); }
int ms(double m) { return static_cast<int>(m * kSr / 1000.0); }

bool report(const char* name, bool pass, const char* fmt, ...) {
  std::printf("%s %-14s ", pass ? "PASS" : "FAIL", name);
  va_list args;
  va_start(args, fmt);
  std::vfprintf(stdout, fmt, args);
  va_end(args);
  std::printf("\n");
  return pass;
}

// What the panel's TIME knob gives the engine, and the box's tuning for everything else.
cv::ChasmParams atTime(float time) {
  cv::ReverbParams r;
  r.engine = cv::kReverbChasm;
  r.time = time;
  cv::applyKnobs(r);
  return r.chasm;
}

// Runs the input through an engine or the Reverb block in kBlock chunks, with the allocation guard on.
template <class Engine, class Params>
Signal run(Engine& e, const Signal& in, const Params& p) {
  Signal out(in.size());
  const int total = static_cast<int>(in.size());
  for (int i = 0; i < total; i += cv::kBlock) {
    const int n = total - i < cv::kBlock ? total - i : cv::kBlock;
    gInProcess = true;
    e.process(&in[at(i)], &out[at(i)], n, p);
    gInProcess = false;
  }
  return out;
}

// The wet signal of a fresh engine.
Signal wet(const Signal& in, const cv::ChasmParams& p) {
  auto chasm = std::make_unique<cv::Chasm>();
  return run(*chasm, in, p);
}

Signal impulseWet(const cv::ChasmParams& p, size_t n) { return wet(impulse(n), p); }

cv::ReverbParams reverbParams(bool on, float wetMix) {
  cv::ReverbParams p;
  p.engine = cv::kReverbChasm;
  p.on = on;
  p.wet = wetMix;
  return p;
}

// 1. The block's edges. Switched off from the start, and at MIX 0 with the engine running, the output
// is the input on every sample. Running to off ends as an exact copy. Off for 2 s then on, with
// silence in, is silent. Off for 5 ms then on keeps the running tail.
bool testEdges() {
  const Signal in = noise(kSr / 2, 0.25f);
  const Signal quiet(at(kSr), 0.0f);
  const Signal gap(at(ms(5.0)), 0.0f);
  const auto mismatches = [](const Signal& a, const Signal& b, size_t from) {
    size_t bad = 0;
    for (size_t i = from; i < a.size(); ++i) bad += a[i] != b[i];
    return bad;
  };

  size_t offBad = 0;
  for (int engine = 0; engine < 2; ++engine) {
    auto rv = std::make_unique<cv::Reverb>();
    cv::ReverbParams p = reverbParams(false, 0.15f);
    p.engine = engine;
    offBad += mismatches(run(*rv, in, p), in, 0);
  }

  auto rv = std::make_unique<cv::Reverb>();
  const size_t mixBad = mismatches(run(*rv, in, reverbParams(true, 0.0f)), in, 0);

  // Running, then off: the crossfade snaps to 0 within 0.3 s, and the copy is exact from there.
  rv = std::make_unique<cv::Reverb>();
  const Signal long2 = noise(2 * kSr, 0.25f);
  Signal head(long2.begin(), long2.begin() + kSr);
  Signal tail(long2.begin() + kSr, long2.end());
  run(*rv, head, reverbParams(true, 0.6f));
  const Signal settled = run(*rv, tail, reverbParams(false, 0.6f));
  const size_t settleBad = mismatches(settled, tail, at(kSr / 2));

  rv = std::make_unique<cv::Reverb>();
  run(*rv, in, reverbParams(true, 0.6f));
  run(*rv, quiet, reverbParams(false, 0.6f));
  run(*rv, quiet, reverbParams(false, 0.6f));
  const float coldPeak = peakOf(run(*rv, quiet, reverbParams(true, 0.6f)));

  rv = std::make_unique<cv::Reverb>();
  run(*rv, in, reverbParams(true, 0.6f));
  run(*rv, gap, reverbParams(false, 0.6f));
  const float keptPeak = peakOf(run(*rv, Signal(at(ms(100.0)), 0.0f), reverbParams(true, 0.6f)));

  constexpr float kMinKept = 0.25f;
  const bool ok = offBad == 0 && mixBad == 0 && settleBad == 0 && coldPeak == 0.0f && keptPeak > kMinKept;
  return report("edges", ok,
                "off %zu, MIX 0 %zu, running to off %zu differing samples (all 0); off 2 s then on peak %.3g (0); "
                "off 5 ms then on keeps peak %.3g (>%.2f)",
                offBad, mixBad, settleBad, static_cast<double>(coldPeak), static_cast<double>(keptPeak),
                static_cast<double>(kMinKept));
}

// 2. The late tail over the early tail falls further at TIME 0 than at the top of the knob.
bool testDecay() {
  constexpr double kMinGap = 20.0;
  double tail[2];
  const float times[2] = {0.0f, 1.0f};
  for (int k = 0; k < 2; ++k) {
    const Signal w = impulseWet(atTime(times[k]), at(ms(1500)));
    tail[k] = db(std::pow(rmsOf(w, at(ms(1000)), at(ms(1500))) / rmsOf(w, 0, at(ms(500))), 2.0));
  }
  return report("decay", tail[1] - tail[0] >= kMinGap,
                "late/early RMS TIME 0 %.1f dB, TIME 100 %% %.1f dB, gap %.1f dB (>=%.0f)", tail[0], tail[1],
                tail[1] - tail[0], kMinGap);
}

// Seconds for the tail to fall 60 dB: twice the time from -5 to -35 dB on the backward-integrated
// energy of the impulse response.
double t60Seconds(const cv::ChasmParams& p) {
  const Signal ir = impulseWet(p, at(8 * kSr));
  std::vector<double> edc(ir.size());
  double acc = 0.0;
  for (size_t i = ir.size(); i-- > 0;) edc[i] = acc += static_cast<double>(ir[i]) * ir[i];
  const double total = edc[0];
  size_t t5 = 0, t35 = 0;
  for (size_t i = 0; i < edc.size(); ++i) {
    if (t5 == 0 && edc[i] <= total * std::pow(10.0, -0.5)) t5 = i;
    if (t35 == 0 && edc[i] <= total * std::pow(10.0, -3.5)) t35 = i;
  }
  return 2.0 * static_cast<double>(t35 - t5) / kSr;
}

// 3. The tail lasts as long as the box's TIME says at three knob points. The pins are the times the
// loop with four allpasses measures.
bool testReverbTime() {
  constexpr double kTol = 0.12;
  constexpr double kPin[3] = {0.77, 0.95, 1.79};  // seconds at TIME 0, 50 and 100 %
  const float times[3] = {0.0f, 0.5f, 1.0f};
  double t60[3];
  bool ok = true;
  for (int k = 0; k < 3; ++k) {
    t60[k] = t60Seconds(atTime(times[k]));
    ok = ok && std::fabs(t60[k] / kPin[k] - 1.0) <= kTol;
  }
  return report("reverb time", ok, "T60 at TIME 0 / 50 / 100 %%: %.2f / %.2f / %.2f s (want %.2f / %.2f / %.2f, +-12 %%)",
                t60[0], t60[1], t60[2], kPin[0], kPin[1], kPin[2]);
}

// 4. The chirp runs down: 4 kHz arrives before 500 Hz. The eight-unit chirp measures 11.5 ms.
bool testChirp() {
  constexpr double kMinLeadMs = 5.5;
  const Signal w = impulseWet(atTime(0.5f), at(ms(60)));
  const long lo = firstPeak(envelope(bandPass(w, 500.0, 4.0), 0.010), at(ms(60)));
  const long hi = firstPeak(envelope(bandPass(w, 4000.0, 4.0), 0.010), at(ms(60)));
  const double loMs = static_cast<double>(lo) * 1000.0 / kSr, hiMs = static_cast<double>(hi) * 1000.0 / kSr;
  return report("chirp-down", lo >= 0 && hi >= 0 && loMs - hiMs >= kMinLeadMs,
                "first peak 4 kHz %.2f ms, 500 Hz %.2f ms, lead %.2f ms (>=%.1f)", hiMs, loMs, loMs - hiMs, kMinLeadMs);
}

// Energy in 900-1100 Hz, excluding 1 kHz +- 5 Hz.
double sideband(const std::vector<double>& p, size_t size) {
  double e = 0.0;
  for (size_t k = 0; k < p.size(); ++k) {
    const double hz = static_cast<double>(k) * kSr / static_cast<double>(size);
    if (hz >= 900.0 && hz < 1100.0 && std::fabs(hz - 1000.0) > 5.0) e += p[k];
  }
  return e;
}

// 5. The wobble puts sidebands round a steady tone.
bool testWobble() {
  constexpr size_t kSize = 32768;
  constexpr double kMinRise = 20.0;
  const Signal in = sine(at(2 * kSr), 1000.0, 0.25f);
  double side[2];
  for (int k = 0; k < 2; ++k) {
    cv::ChasmParams p = atTime(0.5f);
    if (k == 0) p.tuning.wobbleDepth = 0.0f;
    side[k] = sideband(spectrum(wet(in, p), at(kSr / 2), at(2 * kSr), kSize), kSize);
  }
  return report("wobble", db(side[1] / side[0]) >= kMinRise,
                "900-1100 Hz minus 1 kHz +-5 Hz: depth 0 %.1f dB, box depth %.1f dB, rise %.1f (>=%.0f)", db(side[0]),
                db(side[1]), db(side[1] / side[0]), kMinRise);
}

// 6. Changing the wobble depth moves the wet level little. The box's depth is the reference.
bool testWobbleLevel() {
  constexpr double kMaxDb = 0.4;
  const Signal in = noise(3 * kSr, 0.25f);
  const cv::ChasmParams box = atTime(0.5f);
  const double ref = rmsOf(wet(in, box), at(kSr / 2), at(3 * kSr));
  const float scales[3] = {0.25f, 0.5f, 2.0f};
  double d[3];
  bool ok = true;
  for (int k = 0; k < 3; ++k) {
    cv::ChasmParams p = box;
    p.tuning.wobbleDepth = box.tuning.wobbleDepth * scales[k];
    d[k] = db(std::pow(rmsOf(wet(in, p), at(kSr / 2), at(3 * kSr)) / ref, 2.0));
    ok = ok && std::fabs(d[k]) <= kMaxDb;
  }
  return report("wobble-level", ok,
                "noise wet RMS re the box depth: x0.25 %.2f dB, x0.5 %.2f dB, x2 %.2f dB (+-%.1f)", d[0], d[1], d[2],
                kMaxDb);
}

// 8. Burst of loud noise at the top of the TIME knob and the box's gain: finite, bounded, and the
// tail dies away.
bool testStable() {
  constexpr float kMaxPeak = 4.0f;
  constexpr double kMinFall = 38.0;
  Signal in = noise(at(3 * kSr), 0.5f);
  in.resize(at(6 * kSr), 0.0f);
  const Signal w = wet(in, atTime(1.0f));
  const bool finite = [&] {
    for (const float v : w)
      if (!std::isfinite(v)) return false;
    return true;
  }();
  const float peak = peakOf(w);
  const double first = rmsOf(w, at(3 * kSr), at(3 * kSr + kSr / 2));
  const double last = rmsOf(w, at(6 * kSr - kSr / 2), at(6 * kSr));
  const double fall = db(std::pow(first / last, 2.0));
  return report("stable", finite && peak < kMaxPeak && fall >= kMinFall,
                "peak |wet| %.3f (<%.0f), silence RMS first 0.5 s %.3g, last 0.5 s %.3g, fall %.1f dB (>=%.0f)",
                static_cast<double>(peak), static_cast<double>(kMaxPeak), first, last, fall, kMinFall);
}

// 9. A tuning value far outside its range stays finite, because the engine clamps it.
bool testExtremeTuning() {
  constexpr float kBigPeak = 100.0f;
  using Set = void (*)(cv::ChasmTuning&);
  const Set cases[] = {
      [](cv::ChasmTuning& t) { t.bassCutHz = 0.0f; },         [](cv::ChasmTuning& t) { t.bassCutHz = -5.0f; },
      [](cv::ChasmTuning& t) { t.bassCutHzTop = 0.0f; },      [](cv::ChasmTuning& t) { t.bassCutHzTop = 1e9f; },
      [](cv::ChasmTuning& t) { t.wobbleDepth = 1e6f; },       [](cv::ChasmTuning& t) { t.wobbleDepth = -5.0f; },
      [](cv::ChasmTuning& t) { t.wobbleRateHz = -3.0f; },     [](cv::ChasmTuning& t) { t.wobbleRateHz = 1e6f; },
      [](cv::ChasmTuning& t) { t.inputTrim = 1e6f; },         [](cv::ChasmTuning& t) { t.inputTrim = -1e6f; },
      [](cv::ChasmTuning& t) { t.wetDb = 400.0f; },           [](cv::ChasmTuning& t) { t.wetDb = -1e6f; },
  };
  const Signal x = noise(kSr, 0.5f);
  int bad = 0;
  float worst = 0.0f;
  const auto check = [&](const cv::ChasmParams& p) {
    const Signal y = wet(x, p);
    bool ok = true;
    for (const float v : y) ok = ok && std::isfinite(v) && std::fabs(v) < kBigPeak;
    bad += ok ? 0 : 1;
    worst = std::fmax(worst, peakOf(y));
  };
  for (const Set set : cases) {
    cv::ChasmParams p = atTime(1.0f);
    set(p.tuning);
    check(p);
  }
  // The feedback is a parameter, and the engine holds it to its ceiling.
  for (const float feedback : {5.0f, -1.0f, 0.97f}) {
    cv::ChasmParams p = atTime(1.0f);
    p.feedback = feedback;
    check(p);
  }
  return report("extreme tuning", bad == 0, "%zu out-of-range values stay finite and under %.0f, largest output %.1f",
                sizeof cases / sizeof cases[0] + 3, static_cast<double>(kBigPeak), static_cast<double>(worst));
}

// 10. The Reverb block never allocates with Chasm selected from its first pass.
bool testNoAlloc() {
  const Signal in = noise(cv::kBlock * 8, 0.25f);
  auto rv = std::make_unique<cv::Reverb>();
  cv::ReverbParams p = reverbParams(true, 0.15f);
  p.chasm = atTime(0.7f);
  run(*rv, in, p);  // aborts on any new/delete inside process()
  p.on = false;
  run(*rv, Signal(at(cv::kBlock * 64), 0.0f), p);
  return report("no-alloc", true, "no allocation inside process() (the guard aborts on one)");
}

// 11. The Chasm object stays inside its RAM bound.
bool testSize() {
  constexpr size_t kMaxBytes = 49152;
  return report("size", sizeof(cv::Chasm) <= kMaxBytes, "sizeof(cv::Chasm) = %zu bytes (<= %zu), sizeof(cv::Reverb) = %zu",
                sizeof(cv::Chasm), kMaxBytes, sizeof(cv::Reverb));
}

}  // namespace

int main() {
  bool ok = true;
  ok &= testEdges();
  ok &= testDecay();
  ok &= testReverbTime();
  ok &= testChirp();
  ok &= testWobble();
  ok &= testWobbleLevel();
  ok &= testStable();
  ok &= testExtremeTuning();
  ok &= testNoAlloc();
  ok &= testSize();
  std::printf("%s chasm\n", ok ? "PASS" : "FAIL");
  return ok ? 0 : 1;
}
