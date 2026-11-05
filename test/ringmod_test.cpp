// Ringer card checks: off is a bit-exact copy, the carrier sits at RATIO times the note nearest the tracked
// pitch (read from the card and from the output's spectrum), vibrato of 0.4 semitone leaves it still, a note
// step moves it 60 ms after the tracker reports it, a flip shorter than that leaves it still, no change on
// Adam's clips follows another within 60 ms, a keyed card never lands on a note outside the scale, it
// rests at 220 Hz times RATIO before the first voiced frame and holds through unvoiced frames, the output
// stays finite at every knob corner, and the level at the defaults (RATIO 1.5, MIX 50 %) on Adam's clips.
// A note that starts after an unvoiced gap scoops into its pitch: the wet ducks for the gap, the carrier moves at
// most once in the first 250 ms and only while the wet is down (a replay of the worst start in Adam's clips),
// and no note start on the clips changes the carrier twice.
// The card tracks the pitch of its own input, so an Octave stage at +12 st ahead of it moves the note 12
// semitones up on the clips. `ringmod_test --bench` prints the default card's cost with its tracker. The
// clip rows skip when samples/ is absent. No window, no audio device.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <vector>

#include "engine/pitch_fx.h"
#include "engine/psola.h"
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

// One Ringer card, which tracks the pitch of the signal it is given.
struct Rig {
  std::unique_ptr<cv::RingMod> mod = std::make_unique<cv::RingMod>();
  float carrierHz = 0.0f;  // after the last block
  int key = -1;            // the Autotune KEY index the card sits on, or -1 for every semitone

  // Called after each block with the block's first sample, the card and the carrier's frequency.
  std::function<void(size_t, const cv::RingMod&, float)> each;

  template <typename ParamsAt>
  Signal run(const Signal& in, ParamsAt at) {
    Signal out(in.size());
    for (size_t i = 0; i < in.size(); i += cv::kBlock) {
      const int n = static_cast<int>(in.size() - i < cv::kBlock ? in.size() - i : cv::kBlock);
      const cv::RingModParams p = at(i);
      mod->process(&in[i], &out[i], n, p, key);
      carrierHz = mod->carrierHz();
      if (each) each(i, *mod, carrierHz);
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
float trackedMidi(const cv::RingMod& mod) {
  const cv::PitchResult& pr = mod.pitch();
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
  rig.each = [&worst](size_t, const cv::RingMod&, float hz) {
    worst = std::fmax(worst, std::fabs(static_cast<double>(hz) - kA3Hz));
  };
  rig.run(vibrato(kA3Hz, 1.0, -20.0, 0.4, 5.0), modAt(1.0f, 1.0f));
  char detail[96];
  std::snprintf(detail, sizeof detail, "largest carrier move %.3f Hz over 1 s (0.01)", worst);
  return report("vibrato leaves it still", worst < 0.01, detail);
}

// A step from A3 to C4: the carrier is on C4 60 ms (45 blocks) after the first block in which the tracker
// reports C4, give or take one block.
bool checkStep() {
  Signal in = tone(kA3Hz, 0.5, -20.0);
  const Signal next = tone(kC4Hz, 0.5, -20.0);
  in.insert(in.end(), next.begin(), next.end());
  const size_t stepAt = in.size() / 2;
  Rig rig;
  long trackerBlock = -1;
  long carrierBlock = -1;
  rig.each = [&](size_t i, const cv::RingMod& mod, float hz) {
    const long block = static_cast<long>(i / cv::kBlock);
    const float m = trackedMidi(mod);
    if (i >= stepAt && trackerBlock < 0 && std::fabs(m - 60.0f) < 0.1f) trackerBlock = block;
    if (i >= stepAt && carrierBlock < 0 && std::fabs(hz - kC4Hz) < 0.01f) carrierBlock = block;
  };
  rig.run(in, modAt(1.0f, 1.0f));
  char detail[112];
  const long settle = static_cast<long>(60.0f * cv::kSmpPerMs) / cv::kBlock;
  const long lag = carrierBlock - trackerBlock + 1;
  std::snprintf(detail, sizeof detail, "tracker reports C4 in block %ld, carrier on C4 in block %ld (%ld to %ld blocks counted)",
                trackerBlock, carrierBlock, settle, settle + 1);
  return report("a note step settles", trackerBlock >= 0 && carrierBlock >= 0 && lag >= settle && lag <= settle + 1, detail);
}

// A tracker frame as the card reads it: voiced at `hz`, or unvoiced when `hz` is 0.
cv::PitchResult frameAt(double hz) {
  cv::PitchResult pr;
  if (hz > 0.0) {
    pr.voiced = true;
    pr.hz = static_cast<float>(hz);
    pr.period = static_cast<float>(kRate / hz);
  }
  return pr;
}

// Drives the card's note tracker with a scripted reading, one block at a time, and records the held note.
struct Script {
  cv::RingMod mod;
  cv::RingModTuning tuning;
  int moves = 0;
  float last = 0.0f;

  // Feeds `ms` of reading at `hz` and returns the block count at which the held note moved, or -1.
  int feed(double hz, double ms) {
    const int blocks = static_cast<int>(ms * cv::kSmpPerMs) / cv::kBlock;
    const cv::PitchResult pr = frameAt(hz);
    int movedAt = -1;
    for (int b = 0; b < blocks; ++b) {
      mod.trackNote(pr, -1, tuning, cv::kBlock);
      if (std::fabs(mod.heldHz() - last) > 0.01f) {
        last = mod.heldHz();
        ++moves;
        movedAt = b + 1;
      }
    }
    return movedAt;
  }
};

// A reading that flips to another note for 40 ms and back, or wanders through other notes in 40 ms stretches,
// leaves the held note still. An unvoiced gap pauses the count without clearing it.
bool checkFlip() {
  Script s;
  s.feed(kA3Hz, 100.0);
  const int first = s.moves;
  s.feed(kC4Hz, 40.0);
  s.feed(kA3Hz, 100.0);
  s.feed(kD4Hz, 40.0);
  s.feed(kC4Hz, 40.0);
  s.feed(kD4Hz, 40.0);
  s.feed(kA3Hz, 100.0);
  const bool still = first == 1 && s.moves == 1 && std::fabs(s.mod.heldHz() - kA3Hz) < 0.01f;

  Script gap;
  gap.feed(kA3Hz, 100.0);
  gap.feed(kC4Hz, 30.0);
  gap.feed(0.0, 200.0);
  const bool heldThrough = gap.moves == 1;
  gap.feed(kC4Hz, 40.0);
  const bool paused = gap.moves == 2 && std::fabs(gap.mod.heldHz() - kC4Hz) < 0.01f;

  char detail[112];
  std::snprintf(detail, sizeof detail, "40 ms flips: %d move (1, the first note); 30 + 40 ms around an unvoiced gap moves: %s",
                s.moves, heldThrough && paused ? "yes" : "no");
  return report("a short flip leaves it still", still && heldThrough && paused, detail);
}

// A real change held 100 ms moves the held note once, 60 ms (45 blocks) in, and the first note sets it at once.
bool checkSettle() {
  Script s;
  const int firstAt = s.feed(kA3Hz, 100.0);
  s.moves = 0;
  const int movedAt = s.feed(kC4Hz, 100.0);
  const long settle = static_cast<long>(60.0f * cv::kSmpPerMs) / cv::kBlock;
  const bool once = s.moves == 1 && std::fabs(s.mod.heldHz() - kC4Hz) < 0.01f;
  char detail[112];
  std::snprintf(detail, sizeof detail, "first note in block %d (1); change moved %d time(s) in block %d (%ld to %ld)", firstAt,
                s.moves, movedAt, settle, settle + 1);
  return report("a held change moves once", firstAt == 1 && once && movedAt >= settle && movedAt <= settle + 1, detail);
}

// Keyed to C major, a sung C#4 sits on C4 or D4, never on C#4.
bool checkKeyed() {
  const float cSharp = 277.1826f;
  Rig rig;
  rig.key = 0;
  float nearest = 1e9f;
  rig.each = [&nearest, cSharp](size_t i, const cv::RingMod&, float hz) {
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

// Carrier note changes on the three clips, unkeyed and keyed to C. A change is a block whose carrier differs
// from the last block's. The first jump out of the rest pitch counts. A short change is one the next change
// follows within 60 ms.
struct ChangeStats {
  int changes = 0;
  int shorter = 0;
  double seconds = 0.0;
};

ChangeStats countChanges(const std::vector<Signal>& sets, int key) {
  ChangeStats s;
  for (const Signal& padded : sets) {
    Rig rig;
    rig.key = key;
    float last = 0.0f;
    long lastChange = -1;
    rig.each = [&](size_t i, const cv::RingMod&, float hz) {
      if (std::fabs(hz - last) > 0.01f) {
        if (last > 0.0f && i >= clips::kPad) {
          ++s.changes;
          if (lastChange >= 0 && static_cast<double>(i) - static_cast<double>(lastChange) < 0.060 * kRate) ++s.shorter;
          lastChange = static_cast<long>(i);
        }
        last = hz;
      }
    };
    rig.run(padded, modAt(1.0f, 1.0f));
    s.seconds += static_cast<double>(padded.size() - clips::kPad) / kRate;
  }
  return s;
}

// On Adam's clips no carrier change follows another within 60 ms. The rates print for the record.
bool checkClipChanges() {
  bool whole = true;
  const std::vector<Signal> sets = clips::load(CUBEVOX_SAMPLES_DIR, whole);
  if (sets.empty()) {
    std::printf("SKIP clip change rows  samples/ is not in this checkout\n");
    return true;
  }
  const ChangeStats open = countChanges(sets, -1);
  const ChangeStats keyed = countChanges(sets, 0);
  std::printf("NOTE ringmod changes unkeyed: %d in %.1f s = %.2f a second, %d under 60 ms\n", open.changes, open.seconds,
              open.changes / open.seconds, open.shorter);
  std::printf("NOTE ringmod changes keyed C: %d in %.1f s = %.2f a second, %d under 60 ms\n", keyed.changes, keyed.seconds,
              keyed.changes / keyed.seconds, keyed.shorter);
  char detail[96];
  std::snprintf(detail, sizeof detail, "changes under 60 ms: %d unkeyed, %d keyed (0)", open.shorter, keyed.shorter);
  return report("no change under 60 ms", whole && open.shorter == 0 && keyed.shorter == 0, detail);
}

double midiHz(double midi) { return 440.0 * std::pow(2.0, (midi - 69.0) / 12.0); }

// A tracker over a ring of `len` floats that takes every call, tracked or idle, as the card does.
class RingTracker : public cv::PitchTracker {
 public:
  explicit RingTracker(int len) : len_(len), ring_(static_cast<size_t>(len), 0.0f) {}

  void run(const float* in, int n, bool active, bool fine) {
    const int first = head_;
    for (int i = 0; i < n; ++i) {
      ring_[static_cast<size_t>(head_)] = in[i];
      if (++head_ == len_) head_ = 0;
    }
    if (active) {
      push(in, n, true, fine, History{ring_.data(), len_, first});
    } else {
      idle(n);
    }
  }

 private:
  int len_;
  std::vector<float> ring_;
  int head_ = 0;
};

// The card's ring holds what the tracker reads: through tracked stretches, idle stretches of 5 to 50 blocks and
// the replays that follow them, the tracker over kRingNeed floats reports the same period, bit for bit, as one
// over the 51 kB pitch ring, with and without the fine pass.
bool checkRingHoldsTrackerReach() {
  bool whole = true;
  const std::vector<Signal> sets = clips::load(CUBEVOX_SAMPLES_DIR, whole);
  if (sets.empty()) {
    std::printf("SKIP ring reach row  samples/ is not in this checkout\n");
    return true;
  }
  const int idleBlocks[] = {5, 13, 29, 50, 8, 21};
  int blocks = 0;
  int differ = 0;
  for (const bool fine : {false, true}) {
    for (const Signal& padded : sets) {
      RingTracker tight(cv::PitchTracker::kRingNeed);
      RingTracker wide(cv::kVoiceRingLen);
      int idle = 0;
      bool active = true;
      int left = 150;
      for (size_t i = 0; i + cv::kBlock <= padded.size(); i += cv::kBlock, ++blocks) {
        if (left-- == 0) {
          active = !active;
          idle = active ? idle : (idle + 1) % 6;
          left = active ? 150 : idleBlocks[idle];
        }
        tight.run(&padded[i], cv::kBlock, active, fine);
        wide.run(&padded[i], cv::kBlock, active, fine);
        differ += tight.result().period != wide.result().period || tight.result().voiced != wide.result().voiced;
      }
    }
  }
  char detail[112];
  std::snprintf(detail, sizeof detail, "blocks whose period or voicing differs from the wide ring's: %d of %d (0)", differ,
                blocks);
  return report("the ring holds the tracker's reach", whole && differ == 0, detail);
}

// The held note (MIDI) the card has after each block of a clip that passes through an Octave stage ahead of it,
// or -1 before the card has a note. Octave runs at +12 st and MIX 100 %, or is off.
std::vector<float> heldAfterOctave(const Signal& padded, bool octaveUp) {
  auto pitch = std::make_unique<cv::PitchFx>();
  Rig rig;
  cv::PitchFxParams pp;
  pp.octave.on = octaveUp;
  pp.octave.semitones = 12;
  pp.octave.mix = 1.0f;
  const cv::RingModParams p = modAt(1.0f, 1.0f);
  Signal shifted(padded.size()), out(padded.size());
  std::vector<float> held;
  for (size_t i = 0; i + cv::kBlock <= padded.size(); i += cv::kBlock) {
    pitch->process(&padded[i], &shifted[i], cv::kBlock, pp);
    rig.mod->process(&shifted[i], &out[i], cv::kBlock, p, -1);
    held.push_back(rig.mod->heldHz() > 0.0f ? cv::midiOf(rig.mod->heldHz()) : -1.0f);
  }
  return held;
}

// With Octave at +12 st and MIX 100 % ahead, the card's note follows the octave-up voice: 12 semitones above
// the note it holds with Octave off, give or take the semitone the snapping can add, in nearly every block of
// Adam's clips where both hold one. A carrier on the raw voice's pitch would read 0.
bool checkOctaveAhead() {
  bool whole = true;
  const std::vector<Signal> sets = clips::load(CUBEVOX_SAMPLES_DIR, whole);
  if (sets.empty()) {
    std::printf("SKIP octave-ahead row  samples/ is not in this checkout\n");
    return true;
  }
  int both = 0;
  int near = 0;
  std::vector<float> diffs;
  for (const Signal& padded : sets) {
    const std::vector<float> off = heldAfterOctave(padded, false);
    const std::vector<float> up = heldAfterOctave(padded, true);
    for (size_t b = clips::kPad / cv::kBlock; b < off.size(); ++b) {
      if (off[b] < 0.0f || up[b] < 0.0f) continue;
      ++both;
      diffs.push_back(up[b] - off[b]);
      near += std::fabs(up[b] - off[b] - 12.0f) <= 1.0f ? 1 : 0;
    }
  }
  std::sort(diffs.begin(), diffs.end());
  const float median = diffs.empty() ? 0.0f : diffs[diffs.size() / 2];
  const double share = both > 0 ? static_cast<double>(near) / both : 0.0;
  char detail[112];
  std::snprintf(detail, sizeof detail, "median %.1f st above the Octave-off note, %.1f %% of %d blocks within 1 st of 12 (>= 90 %%)",
                median, 100.0 * share, both);
  return report("a carrier follows the Octave voice", whole && median == 12.0f && share >= 0.9, detail);
}

// The worst note start in Adam's clips (clip 3 at 2.0 s): the voice scoops down 2.9 semitones in the first
// 230 ms from the tracked pitch, after a 190 ms gap that follows a note at 41.73. Knots are the clip's own
// pitch in MIDI semitones, in ms after the voice returns. The carrier used to step from 42 to 44 and on to 43.
struct Knot {
  double ms;
  double midi;
};
constexpr Knot kScoop[] = {{0, 45.64}, {11, 45.18}, {21, 44.69}, {32, 44.34}, {43, 44.15}, {53, 44.07}, {64, 44.01},
                           {75, 43.93}, {85, 43.78}, {96, 43.66}, {107, 43.58}, {117, 43.51}, {128, 43.43}, {139, 43.30},
                           {149, 43.20}, {160, 43.17}, {171, 43.21}, {181, 43.26}, {192, 43.33}, {203, 43.23},
                           {213, 43.03}, {224, 42.89}, {235, 42.80}};

double scoopMidi(double ms) {
  constexpr size_t kCount = sizeof kScoop / sizeof kScoop[0];
  if (ms <= kScoop[0].ms) return kScoop[0].midi;
  for (size_t i = 1; i < kCount; ++i) {
    if (ms <= kScoop[i].ms) {
      const double f = (ms - kScoop[i - 1].ms) / (kScoop[i].ms - kScoop[i - 1].ms);
      return kScoop[i - 1].midi + f * (kScoop[i].midi - kScoop[i - 1].midi);
    }
  }
  return kScoop[kCount - 1].midi;
}

// A note at 41.73 for 0.5 s, 190 ms of silence, then the scoop for 260 ms. `onset` is the scoop's first sample.
Signal scoopStart(size_t& onset) {
  Signal x = tone(midiHz(41.73), 0.5, -20.0);
  x.insert(x.end(), static_cast<size_t>(0.19 * kRate), 0.0f);
  onset = x.size();
  const size_t n = static_cast<size_t>(0.26 * kRate);
  Signal v(n);
  double ph = 0.0;
  double sum = 0.0;
  for (size_t i = 0; i < n; ++i) {
    const double f = midiHz(scoopMidi(1000.0 * static_cast<double>(i) / kRate));
    ph += 2.0 * kPi * f / kRate;
    double s = 0.0;
    for (int k = 1; k * f < 5000.0; ++k) s += std::sin(k * ph) / k;
    v[i] = static_cast<float>(s);
    sum += s * s;
  }
  const double g = std::pow(10.0, -20.0 / 20.0) / std::sqrt(sum / static_cast<double>(n));
  const size_t ramp = static_cast<size_t>(0.008 * kRate);
  for (size_t i = 0; i < n; ++i) {
    const double attack = i < ramp ? static_cast<double>(i) / static_cast<double>(ramp) : 1.0;
    const double release = i + ramp > n ? static_cast<double>(n - i) / static_cast<double>(ramp) : 1.0;
    v[i] = static_cast<float>(v[i] * g * attack * release);
  }
  x.insert(x.end(), v.begin(), v.end());
  return x;
}

// After the replay's gap the wet stays down while the voice scoops, the carrier changes at most once in the first
// 250 ms, and the wet heard before that change sits under -30 dB of the settled wet (MIX 100 %, so 1 is settled).
bool checkScoopStart() {
  size_t onset = 0;
  const Signal in = scoopStart(onset);
  Rig rig;
  std::vector<float> carrier;
  std::vector<float> wet;
  rig.each = [&](size_t, const cv::RingMod&, float hz) {
    carrier.push_back(hz);
    wet.push_back(rig.mod->wetLevel());
  };
  rig.run(in, modAt(1.5f, 1.0f));
  const size_t first = onset / cv::kBlock;
  const size_t last = std::min(carrier.size(), first + static_cast<size_t>(0.25 * kRate) / cv::kBlock);
  int changes = 0;
  size_t firstChange = last;
  for (size_t b = first + 1; b < last; ++b) {
    if (std::fabs(carrier[b] - carrier[b - 1]) > 0.01f) {
      if (changes == 0) firstChange = b;
      ++changes;
    }
  }
  float before = 0.0f;
  for (size_t b = first; b < firstChange; ++b) before = std::fmax(before, wet[b]);
  const float landed = static_cast<float>(1.5 * midiHz(43.0));
  const bool onNote = std::fabs(carrier[last - 1] - landed) < 0.01f;
  const float settled = wet[last - 1];
  char detail[144];
  std::snprintf(detail, sizeof detail, "%d carrier change(s) in 250 ms (at most 1), wet before it %.1f dB (< -30), wet %.2f at 250 ms, carrier %s",
                changes, 20.0 * std::log10(std::fmax(static_cast<double>(before), 1e-6)), static_cast<double>(settled),
                onNote ? "on 43" : "off 43");
  return report("a scoop lands once", changes <= 1 && before < 0.0316f && settled > 0.9f && onNote, detail);
}

// Note starts on the clips: a tracker voiced frame that follows 40 ms or more unvoiced, on input above -45 dBFS,
// after the clip's pad. The count is the starts whose carrier changes more than once in the next 250 ms.
struct StartStats {
  int starts = 0;
  int multi = 0;
};

StartStats countStarts(const std::vector<Signal>& sets, int key) {
  StartStats s;
  for (const Signal& padded : sets) {
    Rig rig;
    rig.key = key;
    std::vector<char> voiced;
    std::vector<float> carrier;
    rig.each = [&](size_t, const cv::RingMod& mod, float hz) {
      voiced.push_back(mod.pitch().voiced ? 1 : 0);
      carrier.push_back(hz);
    };
    rig.run(padded, modAt(1.5f, 1.0f));
    const size_t window = static_cast<size_t>(0.25 * kRate) / cv::kBlock;
    const size_t gap = static_cast<size_t>(0.04 * kRate) / cv::kBlock;
    size_t unvoiced = gap;
    for (size_t b = 0; b + window < voiced.size(); ++b) {
      if (!voiced[b]) {
        ++unvoiced;
        continue;
      }
      const bool start = unvoiced >= gap && b * cv::kBlock >= clips::kPad;
      unvoiced = 0;
      if (!start) continue;
      const size_t end = std::min(padded.size(), (b + 1) * cv::kBlock);
      const size_t from = end > 480 ? end - 480 : 0;
      double energy = 0.0;
      for (size_t i = from; i < end; ++i) energy += static_cast<double>(padded[i]) * padded[i];
      if (10.0 * std::log10(energy / static_cast<double>(end - from) + 1e-12) <= -45.0) continue;
      int changes = 0;
      for (size_t k = b + 1; k <= b + window; ++k) changes += std::fabs(carrier[k] - carrier[k - 1]) > 0.01f ? 1 : 0;
      ++s.starts;
      s.multi += changes > 1 ? 1 : 0;
    }
  }
  return s;
}

// On Adam's clips no note start changes the carrier twice in its first 250 ms (21 starts, 1 such start before the gate).
bool checkClipStarts() {
  bool whole = true;
  const std::vector<Signal> sets = clips::load(CUBEVOX_SAMPLES_DIR, whole);
  if (sets.empty()) {
    std::printf("SKIP clip start rows  samples/ is not in this checkout\n");
    return true;
  }
  const StartStats open = countStarts(sets, -1);
  const StartStats keyed = countStarts(sets, 0);
  char detail[112];
  std::snprintf(detail, sizeof detail, "starts with more than one change in 250 ms: %d of %d unkeyed, %d of %d keyed (0)", open.multi,
                open.starts, keyed.multi, keyed.starts);
  return report("starts change the carrier once", whole && open.multi == 0 && keyed.multi == 0, detail);
}

// The wet ducks after 20 ms unvoiced and not before, a voice that comes back on the held note lands without moving
// the carrier, and a scoop through other notes moves the carrier once, to the note it lands on.
bool checkGate() {
  Script s;
  s.feed(kA3Hz, 100.0);
  s.feed(0.0, 15.0);
  const bool shortOpen = !s.mod.ducked();
  s.feed(0.0, 20.0);
  const bool longDucked = s.mod.ducked();
  s.feed(kA3Hz, 8.0);
  const bool stillDucked = s.mod.ducked();
  s.feed(kA3Hz, 20.0);
  const bool backOnHeld = !s.mod.ducked() && s.moves == 1;

  Script scoop;
  scoop.feed(kA3Hz, 100.0);
  scoop.feed(0.0, 100.0);
  scoop.feed(293.66, 25.0);
  scoop.feed(277.18, 25.0);
  const bool downDuring = scoop.mod.ducked() && scoop.moves == 1;
  scoop.feed(kC4Hz, 60.0);
  const bool landed = !scoop.mod.ducked() && scoop.moves == 2 && std::fabs(scoop.mod.heldHz() - kC4Hz) < 0.01f;

  const bool ok = shortOpen && longDucked && stillDucked && backOnHeld && downDuring && landed;
  char detail[112];
  std::snprintf(detail, sizeof detail, "15 ms open %d, 35 ms ducked %d, 8 ms back still ducked %d, 28 ms back open %d; scoop %d %d", shortOpen,
                longDucked, stillDucked, backOnHeld, downDuring, landed);
  return report("the gate ducks and lands", ok, detail);
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
      const cv::RingModParams p = modAt(1.5f, 0.5f);
      double spent = 0.0;
      double slowest = 0.0;
      for (size_t i = 0; i + cv::kBlock <= padded.size(); i += cv::kBlock) {
        const auto t0 = std::chrono::steady_clock::now();
        rig.mod->process(&padded[i], &out[i], cv::kBlock, p, -1);
        const double ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count();
        spent += ns;
        slowest = std::fmax(slowest, ns);
      }
      best = std::fmin(best, spent / static_cast<double>(padded.size()));
      worst = std::fmin(worst, slowest);
    }
  }
  std::printf("ringmod bench: %.2f ns a sample, %.0f ns the costliest block, at RATIO 1.5, MIX 50 %% (card with its own tracker, "
              "minimum of 9 passes per clip)\n", best, worst);
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
  ok &= checkFlip();
  ok &= checkSettle();
  ok &= checkKeyed();
  ok &= checkRestAndHold();
  ok &= checkFinite();
  ok &= checkLevel();
  ok &= checkClipChanges();
  ok &= checkOctaveAhead();
  ok &= checkRingHoldsTrackerReach();
  ok &= checkScoopStart();
  ok &= checkClipStarts();
  ok &= checkGate();
  return ok ? 0 : 1;
}
