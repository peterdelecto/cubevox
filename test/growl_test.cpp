// Growl card checks: off is a bit-exact copy, the voice drops an octave, the wave's depth falls without
// a pitch, the throat follows each syllable and rests in silence, MIX 100 % is the plain card and MIX 0 is
// the input at the voice's lag, the output stays finite at every knob corner, the level at the default
// (GROWL 75 %, MIX 100 %) on Adam's clips, and the fold's alias level at DRIVE 100 % on a sung note. Each run feeds a
// PitchFx first, so the ring is written and the tracker runs, as the chain does. The clip rows skip
// when samples/ is absent. No window, no audio device.

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#include "engine/growl.h"

#include "ringed_tracker.h"
#include "signal_util.h"

namespace {

using testutil::Signal;
constexpr double kPi = testutil::kPi;
constexpr int kRate = cv::kSampleRate;

bool report(const char* name, bool ok, const char* detail) {
  std::printf("%s %-28s %s\n", ok ? "PASS" : "FAIL", name, detail);
  return ok;
}

// The chain's order for a Growl card that makes the pitch call: the pitch stage writes the ring and
// feeds the tracker, then the card runs on the stage's output.
struct Rig {
  std::unique_ptr<cv::PitchFx> pitch = std::make_unique<cv::PitchFx>();
  std::unique_ptr<cv::Growl> growl = std::make_unique<cv::Growl>();

  // in through the stage and the card, with the card's params from `at(sample)` per block.
  template <typename ParamsAt>
  Signal run(const Signal& in, ParamsAt at) {
    Signal out(in.size());
    for (size_t i = 0; i < in.size(); i += cv::kBlock) {
      const int n = static_cast<int>(in.size() - i < cv::kBlock ? in.size() - i : cv::kBlock);
      const cv::GrowlParams g = at(i);
      cv::PitchFxParams pp;
      pp.growlOn = g.on;
      pitch->process(&in[i], &out[i], n, pp);
      growl->process(&out[i], &out[i], n, g, *pitch);
    }
    return out;
  }

  Signal run(const Signal& in, const cv::GrowlParams& g) {
    return run(in, [&g](size_t) { return g; });
  }
};

// The engine's rasp and fold, not the pot. The pot map is tested with the registry.
cv::GrowlParams growlAt(float growl, float drive, bool on = true, float mix = 1.0f) {
  cv::GrowlParams g;
  g.on = on;
  g.growl = growl;
  g.drive = drive;
  g.mix = mix;
  return g;
}

// The engine at a GROWL pot position and a MIX, through the same map the registry applies.
cv::GrowlParams potAt(float pot, float mix) {
  return growlAt(pot * cv::kGrowlRaspTop, pot * cv::kGrowlDriveTop, true, mix);
}

// A sung note: harmonics at 1/k up to 5 kHz, a 5 Hz vibrato of +-20 cents and a phrase envelope.
Signal sungNote(double hz, double seconds, double dbfs) {
  const size_t n = static_cast<size_t>(seconds * kRate);
  Signal x(n);
  double phase = 0.0;
  double sum = 0.0;
  for (size_t i = 0; i < n; ++i) {
    const double t = static_cast<double>(i) / kRate;
    phase += 2.0 * kPi * hz * std::pow(2.0, 20.0 * std::sin(2.0 * kPi * 5.0 * t) / 1200.0) / kRate;
    double v = 0.0;
    for (int k = 1; k * hz < 5000.0; ++k) v += std::sin(k * phase) / k;
    x[i] = static_cast<float>(v);
    sum += v * v;
  }
  const double g = std::pow(10.0, dbfs / 20.0) / std::sqrt(sum / static_cast<double>(n));
  for (float& v : x) v = static_cast<float>(v * g);
  return x;
}

double rms(const Signal& x, size_t from, size_t to) { return testutil::rmsOf(x, from, to); }

using testutil::readWav;

// Adam's three clips, each behind 0.4 s of silence so the card's crossfade settles first. Empty when
// samples/ is absent.
constexpr size_t kPad = kRate * 2 / 5;
std::vector<Signal> clips(bool& whole) {
  const char* names[] = {"adam_vox_sample_3s", "adam_vox_sample2_4s", "adam_vox_sample3_5s"};
  std::vector<Signal> out;
  whole = true;
  std::error_code ec;
  if (!std::filesystem::is_directory(CUBEVOX_SAMPLES_DIR, ec)) return out;
  for (const char* name : names) {
    const Signal clip = readWav(std::string(CUBEVOX_SAMPLES_DIR) + "/" + name + ".wav");
    if (clip.empty()) {
      whole = false;
      continue;
    }
    Signal padded(kPad, 0.0f);
    padded.insert(padded.end(), clip.begin(), clip.end());
    out.push_back(padded);
  }
  return out;
}

// Off, the card is the stage's output bit for bit, and so it is again once a switch-off has settled.
bool checkOff() {
  const Signal in = sungNote(220.0, 2.0, -20.0);
  Rig rig;
  const Signal off = rig.run(in, growlAt(0.5f, 0.5f, false));
  bool exact = off == in;
  Rig offMixed;
  exact = exact && offMixed.run(in, growlAt(0.5f, 0.5f, false, 0.3f)) == in;

  Rig flip;
  const size_t offAt = in.size() / 2;
  const Signal out = flip.run(in, [offAt](size_t i) { return growlAt(1.0f, 1.0f, i < offAt); });
  const size_t settled = offAt + kRate / 10;  // the 20 ms crossfade with room to spare
  size_t differ = 0;
  for (size_t i = settled; i < in.size(); ++i) differ += out[i] != in[i] ? 1 : 0;
  exact = exact && differ == 0;
  char detail[96];
  std::snprintf(detail, sizeof detail, "never on and 100 ms after a switch-off: %zu samples differ (0)", differ);
  return report("off is a copy", exact, detail);
}

// The voice sits an octave under the note: the tracker reads twice the input's period on the output at
// GROWL 0 and DRIVE 0.
bool checkDrop() {
  const Signal in = sungNote(220.0, 2.0, -20.0);
  Rig rig;
  const Signal out = rig.run(in, growlAt(0.0f, 0.0f));
  RingedTracker heard;
  double sum = 0.0;
  int frames = 0;
  for (size_t i = 0; i + cv::kBlock <= out.size(); i += cv::kBlock) {
    heard.push(&out[i], cv::kBlock);
    if (i > static_cast<size_t>(kRate) && heard.result().voiced) {
      sum += heard.result().hz;
      ++frames;
    }
  }
  const double hz = frames > 0 ? sum / frames : 0.0;
  char detail[96];
  std::snprintf(detail, sizeof detail, "220 Hz in reads %.1f Hz out over %d voiced frames (110 +- 3)", hz, frames);
  return report("octave down", frames > 0 && std::fabs(hz - 110.0) < 3.0, detail);
}

// The wave's depth falls to 0 within 20 ms of the tracker losing the pitch, and is 1 on a note.
bool checkDepthFade() {
  Signal in = sungNote(220.0, 1.0, -20.0);
  const Signal noise = testutil::noise(static_cast<size_t>(kRate / 2), 0.05f);
  in.insert(in.end(), noise.begin(), noise.end());
  Rig rig;
  float onNote = 0.0f;
  bool fellInTime = true;
  size_t lostAt = 0;
  Signal out(in.size());
  for (size_t i = 0; i < in.size(); i += cv::kBlock) {
    const cv::GrowlParams g = growlAt(1.0f, 0.5f);
    cv::PitchFxParams pp;
    pp.growlOn = true;
    rig.pitch->process(&in[i], &out[i], cv::kBlock, pp);
    rig.growl->process(&out[i], &out[i], cv::kBlock, g, *rig.pitch);
    if (i == static_cast<size_t>(kRate) - cv::kBlock) onNote = rig.growl->depthFade();
    if (i >= static_cast<size_t>(kRate) && lostAt == 0 && !rig.pitch->pitch().voiced) lostAt = i;
    if (lostAt > 0 && i >= lostAt + static_cast<size_t>(20 * kRate / 1000) + cv::kBlock) {
      fellInTime = fellInTime && rig.growl->depthFade() == 0.0f;
    }
  }
  char detail[96];
  std::snprintf(detail, sizeof detail, "on the note %.2f (1), 0 within 20 ms of the pitch going: %s", onNote,
                lostAt > 0 && fellInTime ? "yes" : "no");
  return report("depth fade", onNote == 1.0f && lostAt > 0 && fellInTime, detail);
}

// Every knob corner, on the synthetic note and on Adam's clips, gives finite output.
bool checkCorners(const std::vector<Signal>& voices) {
  std::vector<Signal> inputs = voices;
  inputs.push_back(sungNote(180.0, 2.0, -18.0));
  size_t bad = 0;
  for (const Signal& in : inputs) {
    for (const float pot : {0.0f, 1.0f}) {
      for (const float mix : {0.0f, 0.5f, 1.0f}) {
        Rig rig;
        for (const float v : rig.run(in, potAt(pot, mix))) bad += std::isfinite(v) ? 0 : 1;
      }
    }
    // The engine's own corners, rasp and fold apart, and a MIX that flips every block.
    for (const float growl : {0.0f, 1.0f}) {
      for (const float drive : {0.0f, 1.0f}) {
        Rig rig;
        for (const float v : rig.run(in, growlAt(growl, drive))) bad += std::isfinite(v) ? 0 : 1;
      }
    }
    Rig flip;
    const auto flipMix = [](size_t i) { return potAt(1.0f, (i / cv::kBlock) % 2 == 0 ? 0.0f : 1.0f); };
    for (const float v : flip.run(in, flipMix)) bad += std::isfinite(v) ? 0 : 1;
  }
  char detail[112];
  std::snprintf(detail, sizeof detail, "%zu inputs at the pot and MIX corners and a flipping MIX: %zu samples not finite (0)",
                inputs.size(), bad);
  return report("finite at the corners", bad == 0, detail);
}

// The mean level over Adam's clips of the output against the input, over the clip alone, in dB.
double meanLevelDb(const std::vector<Signal>& voices, float pot, float mix, const char* label) {
  double sum = 0.0;
  for (const Signal& in : voices) {
    Rig rig;
    const Signal out = rig.run(in, potAt(pot, mix));
    const double db = 20.0 * std::log10(rms(out, kPad, out.size()) / rms(in, kPad, in.size()));
    std::printf("NOTE level %-12s clip           %+6.2f dB\n", label, db);
    sum += db;
  }
  return sum / static_cast<double>(voices.size());
}

// The level rule at the default, GROWL 75 % and MIX 100 %: the mean over Adam's clips of the output
// against the input, over the clip alone, lands 0 to +0.5 dB. GROWL 100 % and MIX 50 % are reported
// beside it.
bool checkLevel(const std::vector<Signal>& voices, bool whole) {
  if (!whole) return report("level 75/100", false, "a clip in samples/ is missing or empty");
  if (voices.empty()) return report("level 75/100", true, "SKIP, samples/ is not in this checkout");
  const double mean = meanLevelDb(voices, cv::kGrowlDefaultPot, 1.0f, "75/100");
  const double top = meanLevelDb(voices, 1.0f, 1.0f, "100/100");
  const double half = meanLevelDb(voices, cv::kGrowlDefaultPot, 0.5f, "75/50");
  std::printf("NOTE level GROWL 100 %% MIX 100 %%: mean %+.2f dB\n", top);
  std::printf("NOTE level GROWL 75 %% MIX 50 %%: mean %+.2f dB\n", half);
  char detail[96];
  std::snprintf(detail, sizeof detail, "mean of %zu clips %+.2f dB (0 to +0.5)", voices.size(), mean);
  return report("level 75/100", mean >= 0.0 && mean <= 0.5, detail);
}

// What the card's throat did over one run, a point per block.
struct ThroatTrace {
  std::vector<float> target;  // the throat the sweep asked for, st
  std::vector<float> glided;  // the throat the voice played, st
  std::vector<float> sweep;   // the syllable sweep, 0 to 1
};

ThroatTrace traceThroat(const Signal& in, const cv::GrowlParams& g) {
  Rig rig;
  ThroatTrace t;
  Signal out(in.size());
  for (size_t i = 0; i + cv::kBlock <= in.size(); i += cv::kBlock) {
    cv::PitchFxParams pp;
    pp.growlOn = g.on;
    rig.pitch->process(&in[i], &out[i], cv::kBlock, pp);
    rig.growl->process(&out[i], &out[i], cv::kBlock, g, *rig.pitch);
    t.target.push_back(rig.growl->throatTargetSt());
    t.glided.push_back(rig.growl->throatSt());
    t.sweep.push_back(rig.growl->sweep());
  }
  return t;
}

// 0.3 s of silence, a loud sung note, then silence.
constexpr double kNoteSec = 1.5;
constexpr double kRestSec = 1.2;

Signal noteThenSilence() {
  Signal in(static_cast<size_t>(0.3 * kRate), 0.0f);
  const Signal note = sungNote(220.0, kNoteSec, -14.0);
  in.insert(in.end(), note.begin(), note.end());
  in.insert(in.end(), static_cast<size_t>(kRestSec * kRate), 0.0f);
  return in;
}

// GROWL 0: the throat stays at -5 st through a note. GROWL 100 %: an onset drops the throat to within
// 0.05 st of -12 st, and it is back at -5 st within DECAY while the note holds. Both are the throat the
// sweep asks for. The voice plays it through the 20 ms glide, which comes within 0.05 st of -5 st again
// 100 ms after the return.
bool checkThroat() {
  const Signal in = noteThenSilence();
  const size_t noteEnd = static_cast<size_t>((0.3 + kNoteSec) * kRate) / cv::kBlock;
  const float rest = cv::GrowlTuning{}.formantSt;
  const float decayBlocks = cv::GrowlTuning{}.decayMs * cv::kSmpPerMs / cv::kBlock;

  const ThroatTrace flat = traceThroat(in, growlAt(0.0f, 0.5f));
  float flatWorst = 0.0f;
  for (size_t b = 0; b < noteEnd; ++b) {
    if (flat.sweep[b] > 0.0f || flat.glided[b] != 0.0f) flatWorst = std::fmax(flatWorst, std::fabs(flat.glided[b] - rest));
  }
  const bool flatOk = flatWorst < 1e-4f;

  const ThroatTrace deep = traceThroat(in, growlAt(1.0f, 0.5f));
  float low = 0.0f, lowGlided = 0.0f;
  size_t lowAt = 0;
  for (size_t b = 0; b < noteEnd; ++b) {
    if (deep.target[b] < low) {
      low = deep.target[b];
      lowAt = b;
    }
    lowGlided = std::fmin(lowGlided, deep.glided[b]);
  }
  size_t backAt = 0;
  for (size_t b = lowAt; b < noteEnd; ++b) {
    if (deep.target[b] > rest - 0.05f) {
      backAt = b;
      break;
    }
  }
  const float returnBlocks = static_cast<float>(backAt - lowAt);
  const size_t holdFrom = std::min(noteEnd, backAt + static_cast<size_t>(100 * cv::kSmpPerMs / cv::kBlock) + 1);
  float heldWorst = 0.0f;
  for (size_t b = holdFrom; b < noteEnd; ++b) heldWorst = std::fmax(heldWorst, std::fabs(deep.glided[b] - rest));
  char detail[200];
  std::snprintf(detail, sizeof detail,
                "GROWL 0 holds %.4f st off -5; GROWL 100 %% drops to %.3f st (glided %.2f), back at -5 after %.0f ms (<= %.0f), "
                "then %.3f st off -5",
                static_cast<double>(flatWorst), static_cast<double>(low), static_cast<double>(lowGlided),
                static_cast<double>(returnBlocks * cv::kBlock / cv::kSmpPerMs),
                static_cast<double>((decayBlocks + 1.0f + 15.0f * cv::kSmpPerMs / cv::kBlock) * cv::kBlock / cv::kSmpPerMs),
                static_cast<double>(heldWorst));
  const bool deepOk = low < -12.0f + 0.05f && lowGlided > -12.05f && backAt > lowAt &&
                      returnBlocks <= decayBlocks + 1.0f + 15.0f * cv::kSmpPerMs / cv::kBlock && heldWorst < 0.05f;
  return report("throat follows syllable", flatOk && deepOk, detail);
}

// The depth at a knob setting: the deepest throat the sweep asks for on the note, measured for the report.
bool checkThroatDepth() {
  const Signal in = noteThenSilence();
  const size_t noteEnd = static_cast<size_t>((0.3 + kNoteSec) * kRate) / cv::kBlock;
  char detail[200] = "";
  bool ok = true;
  size_t used = 0;
  for (const float growl : {0.25f, 0.5f, 1.0f}) {
    const ThroatTrace t = traceThroat(in, growlAt(growl, 0.5f));
    float low = 0.0f;
    for (size_t b = 0; b < noteEnd; ++b) low = std::fmin(low, t.target[b]);
    const float want = -5.0f - 7.0f * growl;
    ok = ok && std::fabs(low - want) < 0.05f;
    used += static_cast<size_t>(std::snprintf(detail + used, sizeof detail - used, "GROWL %.0f %%: %.2f st (%.2f); ",
                                              static_cast<double>(growl * 100.0f), static_cast<double>(low),
                                              static_cast<double>(want)));
  }
  return report("throat depth per GROWL", ok, detail);
}

// After the note the throat rests at -5 st and the sweep reads 0. Room noise 50 dB under full scale
// fires no onset either, so the sweep stays 0 through it.
bool checkThroatSilence() {
  Signal in = noteThenSilence();
  const size_t quietFrom = static_cast<size_t>((0.3 + kNoteSec + 0.6) * kRate);
  const Signal hiss = testutil::noise(in.size() - quietFrom, 0.003f);
  std::copy(hiss.begin(), hiss.end(), in.begin() + static_cast<std::ptrdiff_t>(quietFrom));
  const ThroatTrace t = traceThroat(in, growlAt(1.0f, 0.5f));
  const float rest = cv::GrowlTuning{}.formantSt;
  float sweepMax = 0.0f, offRest = 0.0f;
  for (size_t b = quietFrom / cv::kBlock; b < t.target.size(); ++b) {
    sweepMax = std::fmax(sweepMax, t.sweep[b]);
    offRest = std::fmax(offRest, std::fabs(t.glided[b] - rest));
  }
  char detail[120];
  std::snprintf(detail, sizeof detail, "after the note and through noise: sweep peak %.4f (0), throat %.4f st off -5 (<= 0.05)",
                static_cast<double>(sweepMax), static_cast<double>(offRest));
  return report("throat rests in silence", sweepMax == 0.0f && offRest < 0.05f, detail);
}

// The sweep's extremes (SENSITIVITY and DECAY at their bounds, the throat drop at both signs) give finite output.
bool checkThroatTuning() {
  const Signal in = noteThenSilence();
  size_t bad = 0;
  int runs = 0;
  for (const float sens : {0.0f, 1.0f}) {
    for (const float decay : {cv::kSweepMinDecayMs, cv::kSweepMaxDecayMs}) {
      for (const float drop : {-7.0f, -12.0f, 7.0f}) {
        cv::GrowlParams g = growlAt(1.0f, 1.0f);
        g.tuning.sensitivity = sens;
        g.tuning.decayMs = decay;
        g.tuning.throatDropTop = drop;
        Rig rig;
        for (const float v : rig.run(in, g)) bad += std::isfinite(v) ? 0 : 1;
        ++runs;
      }
    }
  }
  char detail[96];
  std::snprintf(detail, sizeof detail, "%d tuning corners at GROWL 100 %%, DRIVE 100 %%: %zu samples not finite (0)", runs, bad);
  return report("finite at the sweep corners", bad == 0, detail);
}

// The fold at DRIVE 100 % on a sung note, the A3 Adam sings dropped an octave: harmonics at 1/k up to
// 8 kHz at -20 dBFS RMS. The note repeats exactly every 8192 samples (19 periods of 111.3 Hz), so its
// harmonics sit on every 19th bin of an 8192-point FFT with no leakage, and so does all that the fold
// makes from them in continuous time. A harmonic past 24 kHz folds onto a bin off that grid. The alias
// level is the power off the grid against the power on it.
bool checkAlias() {
  constexpr size_t kN = 8192;
  constexpr int kPeriods = 19;
  const double f0 = static_cast<double>(kPeriods) * kRate / kN;
  const size_t total = kRate + kN;
  Signal x(total);
  for (size_t i = 0; i < total; ++i) {
    double v = 0.0;
    for (int k = 1; k * f0 < 8000.0; ++k) {
      v += std::sin(2.0 * kPi * k * kPeriods * static_cast<double>(i % kN) / kN + 0.3 * k * k) / k;
    }
    x[i] = static_cast<float>(v);
  }
  const double g = std::pow(10.0, -20.0 / 20.0) / rms(x, 0, kN);
  for (float& v : x) v = static_cast<float>(v * g);

  cv::growl_detail::Fold fold;
  fold.setTuning(cv::GrowlTuning{});
  const cv::growl_detail::FoldGains gains = fold.gainsAt(1.0f);
  for (size_t i = 0; i < total; i += cv::kBlock) fold.run(&x[i], cv::kBlock, gains, cv::growl_detail::FoldGains{0, 0, 0, 0});

  std::vector<std::complex<double>> spec(kN);
  for (size_t i = 0; i < kN; ++i) spec[i] = x[total - kN + i];
  testutil::fft(spec);
  double on = 0.0, off = 0.0, peakOff = 0.0, fundamental = 0.0;
  for (size_t k = 1; k <= kN / 2; ++k) {
    const double p = std::norm(spec[k]);
    if (k % kPeriods == 0) {
      on += p;
      if (k == kPeriods) fundamental = p;
    } else {
      off += p;
      peakOff = std::fmax(peakOff, p);
    }
  }
  const double aliasDb = 10.0 * std::log10(off / on);
  const double peakDb = 10.0 * std::log10(peakOff / fundamental);
  std::printf("NOTE fold alias at DRIVE 100 %%: %+.1f dB of the note's power, the strongest alias line %+.1f dB under "
              "the fundamental (%.1f Hz)\n",
              aliasDb, peakDb, f0);
  char detail[96];
  std::snprintf(detail, sizeof detail, "%.1f dB, measured and finite", aliasDb);
  return report("fold alias", std::isfinite(aliasDb) && on > 0.0, detail);
}

// FNV-1a over the samples' bit patterns.
uint64_t hashOf(const Signal& x) {
  uint64_t h = 1469598103934665603ull;
  for (const float v : x) {
    uint32_t bits;
    std::memcpy(&bits, &v, sizeof bits);
    for (int s = 0; s < 32; s += 8) h = (h ^ ((bits >> s) & 0xffu)) * 1099511628211ull;
  }
  return h;
}

constexpr uint64_t kFullMixHash = 0x29f1217abb29799aull;

// MIX at its default 100 % leaves the card as it was before the knob existed. The hash is the output of
// that engine on a sung note at GROWL 0.80 and DRIVE 0.65.
bool checkFullMix() {
  const Signal in = sungNote(220.0, 2.0, -20.0);
  Rig rig;
  const uint64_t h = hashOf(rig.run(in, growlAt(0.80f, 0.65f)));
  std::printf("NOTE full-mix hash %016llx\n", static_cast<unsigned long long>(h));
  char detail[96];
  std::snprintf(detail, sizeof detail, "output hash %016llx", static_cast<unsigned long long>(h));
  return report("MIX 100 % is the plain card", h == kFullMixHash, detail);
}

// MIX 0 is the card's input delayed by the voice's lag. A mirror ring of the input is read at the
// delay the card reports, over every block where that delay held still, and the delay itself must be the
// one the voice plays at (the grain delay plus a period, clamped as the voice clamps it).
bool checkMixZero() {
  const Signal in = sungNote(220.0, 2.0, -20.0);
  Rig rig;
  std::vector<float> mirror(static_cast<size_t>(cv::kVoiceRingLen), 0.0f);
  Signal out(in.size());
  double worst = 0.0;
  size_t compared = 0, blocks = 0;
  float period = 0.0f;
  float lag = 0.0f;
  bool lagHeld = false;
  for (size_t i = 0; i + cv::kBlock <= in.size(); i += cv::kBlock) {
    const cv::GrowlParams g = potAt(1.0f, 0.0f);
    cv::PitchFxParams pp;
    pp.growlOn = true;
    const float lagBefore = rig.growl->dryLagSamples();
    rig.pitch->process(&in[i], &out[i], cv::kBlock, pp);
    rig.growl->process(&out[i], &out[i], cv::kBlock, g, *rig.pitch);
    for (size_t j = 0; j < static_cast<size_t>(cv::kBlock); ++j) mirror[(i + j) % mirror.size()] = in[i + j];
    if (rig.pitch->pitch().period > 0.0f) period = rig.pitch->pitch().period;
    lag = rig.growl->dryLagSamples();
    lagHeld = lag == lagBefore;
    ++blocks;
    if (i < static_cast<size_t>(kRate) || !lagHeld || rig.growl->wet() != 1.0f) continue;
    for (size_t j = 0; j < static_cast<size_t>(cv::kBlock); ++j) {
      const float want = cv::readRing<cv::kVoiceRingLen>(mirror.data(), static_cast<int>((i + j) % mirror.size()), lag);
      worst = std::fmax(worst, std::fabs(static_cast<double>(out[i + j] - want)));
      ++compared;
    }
  }
  const float voiceLag = cv::formant_detail::clampLag(
      static_cast<float>(cv::PsolaVoice::kGrainDelay) + cv::PitchTracker::clampPeriod(period), period);
  char detail[128];
  std::snprintf(detail, sizeof detail, "%zu samples in %zu blocks differ by %.2e (1e-6); lag %.2f vs the voice's %.2f", compared,
                blocks, worst, static_cast<double>(lag), static_cast<double>(voiceLag));
  return report("MIX 0 is the delayed input", compared > 20000 && worst < 1e-6 && std::fabs(lag - voiceLag) < 0.01f,
                detail);
}

}  // namespace

int main() {
  bool whole = true;
  const std::vector<Signal> voices = clips(whole);
  bool ok = true;
  ok &= checkOff();
  ok &= checkDrop();
  ok &= checkDepthFade();
  ok &= checkThroat();
  ok &= checkThroatDepth();
  ok &= checkThroatSilence();
  ok &= checkThroatTuning();
  ok &= checkCorners(voices);
  ok &= checkLevel(voices, whole);
  ok &= checkAlias();
  ok &= checkFullMix();
  ok &= checkMixZero();
  std::printf("%s growl\n", ok ? "PASS" : "FAIL");
  return ok ? 0 : 1;
}
