// Ring mod card checks: off is a bit-exact copy, the carrier sits at RATIO times the note nearest the tracked
// pitch (read from the card and from the output's spectrum), vibrato of 0.4 semitone leaves it still, a note
// step moves it in the block the tracker reports it, a keyed card never lands on a note outside the scale, it
// rests at 220 Hz times RATIO before the first voiced frame and holds through unvoiced frames, the output
// stays finite at every knob corner, and the level at the defaults (RATIO 1.5, MIX 50 %) on Adam's clips.
// Each run feeds a PitchFx first, so the
// tracker runs as the chain runs it. `ringmod_test --bench` prints the default card's cost. The clip rows
// skip when samples/ is absent. No window, no audio device.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
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
  int key = -1;            // the Autotune KEY index the card sits on, or -1 for every semitone

  // Called after each block with the block's first sample, the tracker and the carrier's frequency.
  std::function<void(size_t, const cv::PitchFx&, float)> each;

  template <typename ParamsAt>
  Signal run(const Signal& in, ParamsAt at) {
    Signal out(in.size());
    for (size_t i = 0; i < in.size(); i += cv::kBlock) {
      const int n = static_cast<int>(in.size() - i < cv::kBlock ? in.size() - i : cv::kBlock);
      const cv::RingModParams p = at(i);
      cv::PitchFxParams pp;
      pp.ringModOn = p.on;
      pitch->process(&in[i], &out[i], n, pp);
      mod->process(&out[i], &out[i], n, p, *pitch, key);
      carrierHz = mod->carrierHz();
      if (each) each(i, *pitch, carrierHz);
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

// The same harmonic tone with its pitch swung `depthSemis` either side of `hz` at `lfoHz`.
Signal vibrato(double hz, double seconds, double dbfs, double depthSemis, double lfoHz) {
  const size_t n = static_cast<size_t>(seconds * kRate);
  Signal x(n);
  double sum = 0.0;
  double ph = 0.0;
  for (size_t i = 0; i < n; ++i) {
    const double t = static_cast<double>(i) / kRate;
    const double f = hz * std::pow(2.0, depthSemis * std::sin(2.0 * kPi * lfoHz * t) / 12.0);
    ph += 2.0 * kPi * f / kRate;
    double v = 0.0;
    for (int k = 1; k * hz < 5000.0; ++k) v += std::sin(k * ph) / k;
    x[i] = static_cast<float>(v);
    sum += v * v;
  }
  const double g = std::pow(10.0, dbfs / 20.0) / std::sqrt(sum / static_cast<double>(n));
  for (float& v : x) v = static_cast<float>(v * g);
  return x;
}

double rms(const Signal& x, size_t from, size_t to) { return testutil::rmsOf(x, from, to); }

// The tracker's pitch in MIDI semitones, or -1 on an unvoiced frame.
float trackedMidi(const cv::PitchFx& pitch) {
  const cv::PitchResult& pr = pitch.pitch();
  if (!pr.voiced || pr.period <= 0.0f) return -1.0f;
  return cv::midiOf(cv::kSampleRate / cv::PitchTracker::clampPeriod(pr.period));
}

constexpr float kA3Hz = 220.0f;
constexpr float kC4Hz = 261.6256f;
constexpr float kD4Hz = 293.6648f;

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

// A tone at 261.6 Hz, unkeyed, RATIO X1: the carrier sits on C4 to 0.01 Hz.
bool checkNote() {
  Rig rig;
  rig.run(tone(kC4Hz, 1.0, -20.0), modAt(1.0f, 1.0f));
  const double err = std::fabs(static_cast<double>(rig.carrierHz) - kC4Hz);
  char detail[96];
  std::snprintf(detail, sizeof detail, "carrier %.3f Hz for a 261.6 Hz tone (261.626 +- 0.01)", static_cast<double>(rig.carrierHz));
  return report("carrier is the nearest note", err < 0.01, detail);
}

// Vibrato of 0.4 semitone at 5 Hz around A3 leaves the carrier on 220 Hz for the whole second.
bool checkVibrato() {
  Rig rig;
  double worst = 0.0;
  rig.each = [&worst](size_t, const cv::PitchFx&, float hz) {
    worst = std::fmax(worst, std::fabs(static_cast<double>(hz) - kA3Hz));
  };
  rig.run(vibrato(kA3Hz, 1.0, -20.0, 0.4, 5.0), modAt(1.0f, 1.0f));
  char detail[96];
  std::snprintf(detail, sizeof detail, "largest carrier move %.3f Hz over 1 s (0.01)", worst);
  return report("vibrato leaves it still", worst < 0.01, detail);
}

// A step from A3 to C4: the carrier is on C4 by the first block in which the tracker reports C4.
bool checkStep() {
  Signal in = tone(kA3Hz, 0.5, -20.0);
  const Signal next = tone(kC4Hz, 0.5, -20.0);
  in.insert(in.end(), next.begin(), next.end());
  const size_t stepAt = in.size() / 2;
  Rig rig;
  long trackerBlock = -1;
  long carrierBlock = -1;
  rig.each = [&](size_t i, const cv::PitchFx& pitch, float hz) {
    const long block = static_cast<long>(i / cv::kBlock);
    const float m = trackedMidi(pitch);
    if (i >= stepAt && trackerBlock < 0 && std::fabs(m - 60.0f) < 0.1f) trackerBlock = block;
    if (i >= stepAt && carrierBlock < 0 && std::fabs(hz - kC4Hz) < 0.01f) carrierBlock = block;
  };
  rig.run(in, modAt(1.0f, 1.0f));
  char detail[112];
  std::snprintf(detail, sizeof detail, "tracker reports C4 in block %ld, carrier on C4 in block %ld (carrier no later)",
                trackerBlock, carrierBlock);
  return report("a note step jumps at once", trackerBlock >= 0 && carrierBlock >= 0 && carrierBlock <= trackerBlock, detail);
}

// Keyed to C major, a sung C#4 sits on C4 or D4, never on C#4.
bool checkKeyed() {
  const float cSharp = 277.1826f;
  Rig rig;
  rig.key = 0;
  float nearest = 1e9f;
  rig.each = [&nearest, cSharp](size_t i, const cv::PitchFx&, float hz) {
    if (i >= static_cast<size_t>(kRate / 10)) nearest = std::fmin(nearest, std::fabs(hz - cSharp));
  };
  rig.run(tone(cSharp, 1.0, -20.0), modAt(1.0f, 1.0f));
  const bool onScale = std::fabs(rig.carrierHz - kC4Hz) < 0.01f || std::fabs(rig.carrierHz - kD4Hz) < 0.01f;
  char detail[112];
  std::snprintf(detail, sizeof detail, "carrier %.2f Hz (C4 %.2f or D4 %.2f); closest it came to C#4: %.1f Hz away",
                static_cast<double>(rig.carrierHz), static_cast<double>(kC4Hz), static_cast<double>(kD4Hz),
                static_cast<double>(nearest));
  return report("keyed card stays in the scale", onScale && nearest > 1.0f, detail);
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
  for (const int key : {-1, 0}) {
    for (const float ratio : {0.5f, 4.0f}) {
      for (const float mix : {0.0f, 1.0f}) {
        for (const Signal* in : {&note, &noise, &silence}) {
          Rig rig;
          rig.key = key;
          const Signal out = rig.run(*in, modAt(ratio, mix));
          ok = ok && allFinite(out);
          peak = std::fmax(peak, testutil::peakOf(out));
        }
      }
    }
  }
  char detail[128];
  std::snprintf(detail, sizeof detail, "ratios 0.5 and 4, MIX 0 and 100 %%, unkeyed and keyed, note, noise, silence; peak %.2f (< 2)",
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
        rig.mod->process(&out[i], &out[i], cv::kBlock, p, *rig.pitch, -1);
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
  ok &= checkNote();
  ok &= checkVibrato();
  ok &= checkStep();
  ok &= checkKeyed();
  ok &= checkRestAndHold();
  ok &= checkFinite();
  ok &= checkLevel();
  return ok ? 0 : 1;
}
