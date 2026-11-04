// Formant follower checks: settled off is a plain copy, the drive reads a loud note near 1 and a note
// 15 dB softer near 0.55 at any input level, the voice's SHIFT follows DEPTH times the drive between
// whole semitones, the output stays finite on Adam's clips at every knob corner, and the shifted voice
// alone at MIX 100 % holds the level rule on them. The card is set through the registry, as the box
// sets it. The clip rows skip when samples/ is absent. No window, no audio device.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#include "core/chain_params.h"
#include "core/registry.h"
#include "engine/formant_follower.h"
#include "engine/pitch_fx.h"

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr int kRate = cv::kSampleRate;

using Signal = std::vector<float>;

bool report(const char* name, bool ok, const char* fmt, double a = 0.0, double b = 0.0, double c = 0.0) {
  char detail[200];
  std::snprintf(detail, sizeof detail, fmt, a, b, c);
  std::printf("%s %s  %s\n", ok ? "PASS" : "FAIL", name, detail);
  return ok;
}

double dbOf(double ratio) { return 20.0 * std::log10(ratio); }

double rmsRange(const Signal& x, size_t from, size_t to) {
  double s = 0.0;
  for (size_t i = from; i < to; ++i) s += static_cast<double>(x[i]) * x[i];
  return std::sqrt(s / static_cast<double>(to - from));
}

// A 150 Hz sawtooth, harmonics at 1/k up to 8 kHz, at peak `peak`.
Signal sawtooth(int n, float peak) {
  Signal x(static_cast<size_t>(n));
  for (int i = 0; i < n; ++i) {
    double s = 0.0;
    for (int k = 1; 150.0 * k < 8000.0; ++k) s += std::sin(2.0 * kPi * 150.0 * k * i / kRate) / k;
    x[static_cast<size_t>(i)] = static_cast<float>(0.55 * peak * s);
  }
  return x;
}

// The card placed and switched on with its pots at `depth` and `mix` (0..1), as compose sets it.
ChainParams boxFollower(float depth, float mix, bool on = true) {
  ChainParams chain;
  applyPlacement(Card::FormantFollower, chain);
  applyToggle(Card::FormantFollower, on, chain);
  applyKnob(Card::FormantFollower, 0, depth, chain);
  applyKnob(Card::FormantFollower, 1, mix, chain);
  return chain;
}

// The pitch stage and the follower ahead of it, as the chain runs them, on the heap: the ring is large.
struct Stage {
  cv::PitchFx pitch;
  cv::FormantFollower follower;
  Stage() {
    pitch.reset();
    follower.reset();
  }

  // One call of n samples.
  void run(const float* in, float* out, int n, const ChainParams& p) {
    pitch.process(in, out, n, follower.follow(in, n, p.pitchFx, p.follower));
  }
};

// The signal through a fresh stage in kBlock calls. `each` sees the stage after every call.
template <typename Each>
Signal through(const Signal& in, const ChainParams& p, Each each) {
  auto s = std::make_unique<Stage>();
  Signal out(in.size());
  const int total = static_cast<int>(in.size());
  for (int i = 0; i < total; i += cv::kBlock) {
    const int n = total - i < cv::kBlock ? total - i : cv::kBlock;
    s->run(&in[static_cast<size_t>(i)], &out[static_cast<size_t>(i)], n, p);
    each(*s, i + n);
  }
  return out;
}

Signal through(const Signal& in, const ChainParams& p) {
  return through(in, p, [](const Stage&, int) {});
}

// Settled off, the card placed with its toggle off passes the input bit for bit. Switched on and then
// off again, the stage ends on a plain copy once the toggle's fade settles.
bool testOffExact() {
  const Signal in = sawtooth(kRate, 0.5f);
  const Signal off = through(in, boxFollower(1.0f, 1.0f, false));
  bool ok = report("off: placed with its toggle off, a plain copy", off == in, "");

  auto s = std::make_unique<Stage>();
  const ChainParams on = boxFollower(1.0f, 1.0f, true);
  const ChainParams offAgain = boxFollower(1.0f, 1.0f, false);
  Signal out(in.size());
  bool heard = false;
  bool copy = true;
  for (int pass = 0; pass < 2; ++pass) {
    for (size_t i = 0; i < in.size(); i += cv::kBlock) {
      s->run(&in[i], &out[i], cv::kBlock, pass == 0 ? on : offAgain);
      for (int j = 0; j < cv::kBlock; ++j) {
        if (pass == 0) heard = heard || out[i + static_cast<size_t>(j)] != in[i + static_cast<size_t>(j)];
        if (pass == 1 && i >= in.size() / 2) copy = copy && out[i + static_cast<size_t>(j)] == in[i + static_cast<size_t>(j)];
      }
    }
  }
  ok &= report("off: on for 1 s, then off, ends on a plain copy", heard && copy, "");
  return ok;
}

// A loud note, then the same note 15 dB softer, each 0.5 s: the drive at the end of each.
struct Drives {
  float loud;
  float soft;
  float rest;  // after 1 s of silence, which lets the release and the glide settle
};

Drives drivesAt(float peak) {
  const int half = kRate / 2;
  Signal in = sawtooth(4 * half, peak);
  const float soft = static_cast<float>(std::pow(10.0, -15.0 / 20.0));
  for (int i = half; i < 2 * half; ++i) in[static_cast<size_t>(i)] *= soft;
  for (int i = 2 * half; i < 4 * half; ++i) in[static_cast<size_t>(i)] = 0.0f;
  Drives d{0.0f, 0.0f, 0.0f};
  through(in, boxFollower(1.0f, 1.0f), [&](const Stage& s, int done) {
    if (done == half) d.loud = s.follower.drive();
    if (done == 2 * half) d.soft = s.follower.drive();
    if (done == 4 * half) d.rest = s.follower.drive();
  });
  return d;
}

// The drive reads the loud note at 1 and the soft one 15 dB under it at (30 - 15 + 1.5) / 30 = 0.55, the
// peak having fallen 1.5 dB over the soft note. The same notes 20 dB quieter read the same drives.
bool testDriveRanges() {
  const Drives hot = drivesAt(0.5f);
  const Drives cold = drivesAt(0.05f);
  bool ok = report("drive: a loud note reads 1", hot.loud > 0.99f && cold.loud > 0.99f, "%.3f, 20 dB down %.3f", hot.loud,
                   cold.loud);
  ok &= report("drive: a note 15 dB softer reads 0.55", std::fabs(hot.soft - 0.55f) < 0.03f, "%.3f", hot.soft);
  ok &= report("drive: the same notes 20 dB down read the same", std::fabs(hot.soft - cold.soft) < 0.01f, "%.3f against %.3f",
               cold.soft, hot.soft);
  ok &= report("drive: silence reads 0", hot.rest == 0.0f && cold.rest == 0.0f, "%.3f, %.3f", hot.rest, cold.rest);
  return ok;
}

// With the drive at 1, the voice's glided SHIFT sits on DEPTH: on +7 st exactly, and between whole
// semitones (+3.5 st at 75 %) within 0.06 st of it on average and 0.1 st at every call.
bool testShiftFollowsDepth() {
  const Signal in = sawtooth(kRate, 0.5f);
  bool ok = true;
  for (const float position : {1.0f, 0.75f, 0.0f}) {
    const float want = -cv::kFollowerMaxDepthSt + 2.0f * cv::kFollowerMaxDepthSt * position;
    double sum = 0.0;
    double worst = 0.0;
    int count = 0;
    through(in, boxFollower(position, 1.0f), [&](const Stage& s, int done) {
      if (done < kRate / 2) return;
      const double err = static_cast<double>(s.pitch.shiftSemis()) - want;
      sum += err;
      worst = std::max(worst, std::fabs(err));
      ++count;
    });
    char name[64];
    std::snprintf(name, sizeof name, "shift: DEPTH %+.1f st on a loud note", static_cast<double>(want));
    ok &= report(name, std::fabs(sum / count) < 0.06 && worst < 0.1, "mean error %+.3f st, worst %.3f st", sum / count, worst);
  }
  return ok;
}

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
        int16_t v;
        std::memcpy(&v, &b[pos + 8 + 2 * i], 2);
        x[i] = static_cast<float>(v) / 32768.0f;
      }
      return x;
    }
    pos += 8 + len + (len & 1);
  }
  return {};
}

// Adam's three clips, each after 0.4 s of silence so the enable fade settles. Empty when samples/ is
// absent; `missing` is set when the folder is there and a clip in it is missing or empty.
struct Clip {
  std::string name;
  Signal padded;
};
constexpr size_t kPad = kRate * 2 / 5;

std::vector<Clip> clips(bool& missing) {
  std::vector<Clip> out;
  missing = false;
  std::error_code ec;
  const bool folder = std::filesystem::is_directory(CUBEVOX_SAMPLES_DIR, ec);
  for (const char* name : {"adam_vox_sample_3s", "adam_vox_sample2_4s", "adam_vox_sample3_5s"}) {
    const Signal clip = readWav(std::string(CUBEVOX_SAMPLES_DIR) + "/" + name + ".wav");
    if (clip.empty()) {
      missing = missing || folder;
      continue;
    }
    Signal padded(kPad, 0.0f);
    padded.insert(padded.end(), clip.begin(), clip.end());
    out.push_back({name, padded});
  }
  return out;
}

// Every knob corner, DEPTH -7 st and +7 st by MIX 0 and 100 %, and DEPTH 0 at noon, on each clip.
bool testClipCorners(const std::vector<Clip>& all) {
  bool finite = true;
  int runs = 0;
  for (const Clip& c : all) {
    for (const float depth : {0.0f, 0.5f, 1.0f}) {
      for (const float mix : {0.0f, 1.0f}) {
        const Signal out = through(c.padded, boxFollower(depth, mix));
        for (const float v : out) finite = finite && std::isfinite(v);
        ++runs;
      }
    }
  }
  return report("clips: finite at every knob corner", finite, "%.0f runs", runs);
}

// The shifted voice alone at MIX 100 %, at DEPTH -7, +3 and +7 st: each clip in the mid band (-1.5 to
// +1 dB), as the Formant card's SHIFT rows hold it, and the mean of the three in the rule, 0 to +0.5 dB
// with 0.1 dB of slack.
bool testLevel(const std::vector<Clip>& all) {
  bool ok = true;
  for (const float st : {-7.0f, 3.0f, 7.0f}) {
    const float position = (st + cv::kFollowerMaxDepthSt) / (2.0f * cv::kFollowerMaxDepthSt);
    double sum = 0.0;
    bool band = true;
    for (const Clip& c : all) {
      const Signal out = through(c.padded, boxFollower(position, 1.0f));
      const double db = dbOf(rmsRange(out, kPad, out.size()) / rmsRange(c.padded, kPad, c.padded.size()));
      std::printf("NOTE level DEPTH %+.0f st mix 1 %-22s %+6.2f dB\n", static_cast<double>(st), c.name.c_str(), db);
      band = band && db >= -1.5 && db <= 1.0;
      sum += db;
    }
    const double mean = sum / static_cast<double>(all.size());
    char name[64];
    std::snprintf(name, sizeof name, "level: DEPTH %+.0f st MIX 100 %%", static_cast<double>(st));
    ok &= report(name, band && mean >= -0.1 && mean <= 0.6, "mean %+.2f dB (want -0.1..+0.6, each clip -1.5..+1.0)", mean);
  }
  return ok;
}

}  // namespace

int main() {
  bool ok = testOffExact();
  ok &= testDriveRanges();
  ok &= testShiftFollowsDepth();
  bool missing = false;
  const std::vector<Clip> all = clips(missing);
  if (missing) {
    std::printf("FAIL clips: samples/ is here and a clip in it is missing or empty\n");
    ok = false;
  } else if (all.empty()) {
    std::printf("SKIP clips: samples/ is not in this checkout\n");
  } else {
    ok &= testClipCorners(all);
    ok &= testLevel(all);
  }
  std::printf("%s formant_follower\n", ok ? "PASS" : "FAIL");
  return ok ? 0 : 1;
}
