// Ring mod card checks: off is a bit-exact copy, the carrier sits at RATIO times the tracked pitch of a
// 220 Hz tone (read from the card and from the output's spectrum), it rests at 220 Hz times RATIO before the
// first voiced frame and holds through unvoiced frames, the output stays finite at every knob corner, and
// the level at the defaults (RATIO 1.5, MIX 50 %) on Adam's clips. Each run feeds a PitchFx first, so the
// tracker runs as the chain runs it. `ringmod_test --bench` prints the default card's cost. The clip rows
// skip when samples/ is absent. No window, no audio device.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

#include "engine/ringmod.h"

#include "clip_util.h"
#include "dft_util.h"
#include "signal_util.h"

namespace {

using testutil::Signal;
constexpr double kPi = testutil::kPi;
constexpr int kRate = cv::kSampleRate;

bool report(const char* name, bool ok, const char* detail) {
  std::printf("%s %-28s %s\n", ok ? "PASS" : "FAIL", name, detail);
  return ok;
}

// The chain's order for a Ring mod card that makes the pitch call: the pitch stage writes the ring and feeds
// the tracker, then the card runs on the stage's output.
struct Rig {
  std::unique_ptr<cv::PitchFx> pitch = std::make_unique<cv::PitchFx>();
  std::unique_ptr<cv::RingMod> mod = std::make_unique<cv::RingMod>();
  float carrierHz = 0.0f;  // after the last block

  template <typename ParamsAt>
  Signal run(const Signal& in, ParamsAt at) {
    Signal out(in.size());
    for (size_t i = 0; i < in.size(); i += cv::kBlock) {
      const int n = static_cast<int>(in.size() - i < cv::kBlock ? in.size() - i : cv::kBlock);
      const cv::RingModParams p = at(i);
      cv::PitchFxParams pp;
      pp.ringModOn = p.on;
      pitch->process(&in[i], &out[i], n, pp);
      mod->process(&out[i], &out[i], n, p, *pitch);
      carrierHz = mod->carrierHz();
    }
    return out;
  }

  Signal run(const Signal& in, const cv::RingModParams& p) {
    return run(in, [&p](size_t) { return p; });
  }
};

cv::RingModParams modAt(float ratio, float mix, bool on = true) {
  cv::RingModParams p;
  p.on = on;
  p.ratio = ratio;
  p.mix = mix;
  return p;
}

// A harmonic tone: partials at 1/k up to 5 kHz, normalised to dbfs RMS.
Signal tone(double hz, double seconds, double dbfs) {
  const size_t n = static_cast<size_t>(seconds * kRate);
  Signal x(n);
  double sum = 0.0;
  for (size_t i = 0; i < n; ++i) {
    double v = 0.0;
    for (int k = 1; k * hz < 5000.0; ++k) v += std::sin(2.0 * kPi * k * hz * static_cast<double>(i) / kRate) / k;
    x[i] = static_cast<float>(v);
    sum += v * v;
  }
  const double g = std::pow(10.0, dbfs / 20.0) / std::sqrt(sum / static_cast<double>(n));
  for (float& v : x) v = static_cast<float>(v * g);
  return x;
}

double rms(const Signal& x, size_t from, size_t to) { return testutil::rmsOf(x, from, to); }

bool checkOff() {
  const Signal in = tone(220.0, 2.0, -20.0);
  Rig rig;
  const bool never = rig.run(in, modAt(1.5f, 1.0f, false)) == in;

  Rig flip;
  const size_t offAt = in.size() / 2;
  const Signal out = flip.run(in, [offAt](size_t i) { return modAt(2.5f, 1.0f, i < offAt); });
  size_t differ = 0;
  for (size_t i = offAt + static_cast<size_t>(kRate / 10); i < in.size(); ++i) differ += out[i] != in[i] ? 1 : 0;
  char detail[96];
  std::snprintf(detail, sizeof detail, "never on and 100 ms after a switch-off: %zu samples differ (0)", differ);
  return report("off is a copy", never && differ == 0, detail);
}

// RATIO times the pitch of a steady 220 Hz tone, read from the card's carrier and from the output's spectrum.
// A pure 220 Hz sine through MIX 100 % leaves only the sum and difference tones, so the strongest peak above
// 220 Hz is the sum tone at (RATIO + 1) times 220 Hz, and the carrier follows from it.
bool checkCarrier() {
  const Signal sine = testutil::sine(static_cast<size_t>(2 * kRate), 220.0, 0.1f);
  const Signal harmonic = tone(220.0, 2.0, -20.0);
  bool ok = true;
  double worstCard = 0.0;
  double worstSpectrum = 0.0;
  for (int i = 0; i < cv::kRingModRatioCount; ++i) {
    const float ratio = cv::kRingModRatio[static_cast<size_t>(i)];
    Rig a;
    a.run(harmonic, modAt(ratio, 1.0f));
    const double card = std::fabs(static_cast<double>(a.carrierHz) / (ratio * 220.0) - 1.0);
    Rig b;
    const Signal out = b.run(sine, modAt(ratio, 1.0f));
    const double sum = (ratio + 1.0) * 220.0;
    const double heard = dft::peakHz(out, static_cast<size_t>(kRate), out.size(), sum * 0.97, sum * 1.03, 1.0);
    const double spectrum = std::fabs((heard - 220.0) / (ratio * 220.0) - 1.0);
    worstCard = std::fmax(worstCard, card);
    worstSpectrum = std::fmax(worstSpectrum, spectrum);
    ok = ok && card < 0.005 && spectrum < 0.005;
  }
  char detail[112];
  std::snprintf(detail, sizeof detail, "worst error over the 7 ratios: card %.3f %%, spectrum %.3f %% (0.5)",
                100.0 * worstCard, 100.0 * worstSpectrum);
  return report("carrier is RATIO x pitch", ok, detail);
}

// Silence has no pitch, so the carrier rests at 220 Hz times RATIO. After a voiced note it keeps that note's
// frequency through noise, which the tracker calls unvoiced.
bool checkRestAndHold() {
  Rig quiet;
  quiet.run(Signal(static_cast<size_t>(kRate / 2), 0.0f), modAt(2.0f, 1.0f));
  const bool rests = std::fabs(quiet.carrierHz - 440.0f) < 0.01f;

  Signal in = tone(330.0, 1.0, -20.0);
  const Signal noise = testutil::noise(static_cast<size_t>(kRate), 0.05f);
  in.insert(in.end(), noise.begin(), noise.end());
  Rig held;
  held.run(in, modAt(2.0f, 1.0f));
  const bool holds = std::fabs(held.carrierHz / 660.0f - 1.0f) < 0.02f;
  char detail[112];
  std::snprintf(detail, sizeof detail, "silence: %.1f Hz (440); after noise: %.1f Hz (660 +- 2 %%)",
                static_cast<double>(quiet.carrierHz), static_cast<double>(held.carrierHz));
  return report("rests and holds", rests && holds, detail);
}

bool allFinite(const Signal& x) {
  for (const float v : x) {
    if (!std::isfinite(v)) return false;
  }
  return true;
}

// Every knob corner on a note, on noise and in silence stays finite and inside a sane peak.
bool checkFinite() {
  const Signal note = tone(220.0, 1.0, -6.0);
  const Signal noise = testutil::noise(static_cast<size_t>(kRate), 1.0f);
  const Signal silence(static_cast<size_t>(kRate), 0.0f);
  bool ok = true;
  float peak = 0.0f;
  for (const float ratio : {0.5f, 4.0f}) {
    for (const float mix : {0.0f, 1.0f}) {
      for (const Signal* in : {&note, &noise, &silence}) {
        Rig rig;
        const Signal out = rig.run(*in, modAt(ratio, mix));
        ok = ok && allFinite(out);
        peak = std::fmax(peak, testutil::peakOf(out));
      }
    }
  }
  char detail[96];
  std::snprintf(detail, sizeof detail, "ratios 0.5 and 4, MIX 0 and 100 %%, note, noise, silence; peak %.2f (< 2)",
                static_cast<double>(peak));
  return report("finite at the corners", ok && peak < 2.0f, detail);
}

// The level on Adam's clips. The wet path alone (MIX 100 %) is held to the rule over the mean of the clips,
// the default (MIX 50 %) to the rule on every clip.
bool checkLevel() {
  bool whole = true;
  const std::vector<Signal> sets = clips::load(CUBEVOX_SAMPLES_DIR, whole);
  if (sets.empty()) {
    std::printf("SKIP level rows  samples/ is not in this checkout\n");
    return true;
  }
  bool ok = whole;
  double wetSum = 0.0;
  double midSum = 0.0;
  for (const Signal& padded : sets) {
    Rig wet;
    const Signal w = wet.run(padded, modAt(1.5f, 1.0f));
    Rig mid;
    const Signal m = mid.run(padded, modAt(1.5f, 0.5f));
    const double in = rms(padded, clips::kPad, padded.size());
    const double wetDb = 20.0 * std::log10(rms(w, clips::kPad, w.size()) / in);
    const double midDb = 20.0 * std::log10(rms(m, clips::kPad, m.size()) / in);
    wetSum += wetDb;
    midSum += midDb;
    ok = ok && midDb >= -1.5 && midDb <= 1.0;
    std::printf("NOTE ringmod level clip: wet only %+.2f dB, default %+.2f dB (default held to -1.5..+1.0)\n", wetDb, midDb);
  }
  const double wetMean = wetSum / static_cast<double>(sets.size());
  const double midMean = midSum / static_cast<double>(sets.size());
  ok = ok && wetMean >= 0.0 && wetMean <= 0.5 && midMean >= 0.0 && midMean <= 0.5;
  char detail[112];
  std::snprintf(detail, sizeof detail, "mean of %zu clips: wet only %+.2f dB, default %+.2f dB (0..+0.5)", sets.size(), wetMean,
                midMean);
  return report("level at the defaults", ok, detail);
}

int bench() {
  bool whole = true;
  const std::vector<Signal> sets = clips::load(CUBEVOX_SAMPLES_DIR, whole);
  if (sets.empty()) return 1;
  double best = 1e30;
  double worst = 1e30;
  for (int pass = 0; pass < 9; ++pass) {
    for (const Signal& padded : sets) {
      Rig rig;
      Signal out(padded.size());
      cv::PitchFxParams pp;
      pp.ringModOn = true;
      const cv::RingModParams p = modAt(1.5f, 0.5f);
      double spent = 0.0;
      double slowest = 0.0;
      for (size_t i = 0; i + cv::kBlock <= padded.size(); i += cv::kBlock) {
        rig.pitch->process(&padded[i], &out[i], cv::kBlock, pp);
        const auto t0 = std::chrono::steady_clock::now();
        rig.mod->process(&out[i], &out[i], cv::kBlock, p, *rig.pitch);
        const double ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count();
        spent += ns;
        slowest = std::fmax(slowest, ns);
      }
      best = std::fmin(best, spent / static_cast<double>(padded.size()));
      worst = std::fmin(worst, slowest);
    }
  }
  std::printf("ringmod bench: %.2f ns a sample, %.0f ns the costliest block, at RATIO 1.5, MIX 50 %% (card alone, minimum of 9 "
              "passes per clip)\n", best, worst);
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc > 1 && std::strcmp(argv[1], "--bench") == 0) return bench();
  bool ok = true;
  ok &= checkOff();
  ok &= checkCarrier();
  ok &= checkRestAndHold();
  ok &= checkFinite();
  ok &= checkLevel();
  return ok ? 0 : 1;
}
