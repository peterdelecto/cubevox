// Growl card checks: off is a bit-exact copy, the voice drops an octave, the wave's depth falls without
// a pitch, the output stays finite at every knob corner, the level at the default (GROWL 50 %, DRIVE
// 50 %) on Adam's clips, and the fold's alias level at DRIVE 100 % on a sung note. Each run feeds a
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

cv::GrowlParams growlAt(float growl, float drive, bool on = true) {
  cv::GrowlParams g;
  g.on = on;
  g.growl = growl;
  g.drive = drive;
  return g;
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

// 16-bit mono PCM WAV, any chunks before data. Empty on a missing or other file.
Signal readWav(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return {};
  std::vector<char> b((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  if (b.size() < 12 || std::memcmp(b.data(), "RIFF", 4) != 0) return {};
  size_t pos = 12;
  while (pos + 8 <= b.size()) {
    uint32_t len;
    std::memcpy(&len, &b[pos + 4], 4);
    if (std::memcmp(&b[pos], "data", 4) == 0) {
      const size_t n = std::min<size_t>(len, b.size() - pos - 8) / 2;
      Signal x(n);
      for (size_t i = 0; i < n; ++i) {
        int16_t s;
        std::memcpy(&s, &b[pos + 8 + 2 * i], 2);
        x[i] = static_cast<float>(s) / 32768.0f;
      }
      return x;
    }
    pos += 8 + len + (len & 1);
  }
  return {};
}

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
    for (const float growl : {0.0f, 1.0f}) {
      for (const float drive : {0.0f, 1.0f}) {
        Rig rig;
        for (const float v : rig.run(in, growlAt(growl, drive))) bad += std::isfinite(v) ? 0 : 1;
      }
    }
  }
  char detail[96];
  std::snprintf(detail, sizeof detail, "%zu inputs at the four corners: %zu samples not finite (0)", inputs.size(), bad);
  return report("finite at the corners", bad == 0, detail);
}

// The level rule at the default, GROWL 50 % and DRIVE 50 %: the mean over Adam's clips of the output
// against the input, over the clip alone, lands 0 to +0.5 dB.
bool checkLevel(const std::vector<Signal>& voices, bool whole) {
  if (!whole) return report("level 50/50", false, "a clip in samples/ is missing or empty");
  if (voices.empty()) return report("level 50/50", true, "SKIP, samples/ is not in this checkout");
  double sum = 0.0;
  for (const Signal& in : voices) {
    Rig rig;
    const Signal out = rig.run(in, growlAt(0.5f, 0.5f));
    const double db = 20.0 * std::log10(rms(out, kPad, out.size()) / rms(in, kPad, in.size()));
    std::printf("NOTE level 50/50 clip              %+6.2f dB\n", db);
    sum += db;
  }
  const double mean = sum / static_cast<double>(voices.size());
  char detail[96];
  std::snprintf(detail, sizeof detail, "mean of %zu clips %+.2f dB (0 to +0.5)", voices.size(), mean);
  return report("level 50/50", mean >= 0.0 && mean <= 0.5, detail);
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

}  // namespace

int main() {
  bool whole = true;
  const std::vector<Signal> voices = clips(whole);
  bool ok = true;
  ok &= checkOff();
  ok &= checkDrop();
  ok &= checkDepthFade();
  ok &= checkCorners(voices);
  ok &= checkLevel(voices, whole);
  ok &= checkAlias();
  std::printf("%s growl\n", ok ? "PASS" : "FAIL");
  return ok ? 0 : 1;
}
