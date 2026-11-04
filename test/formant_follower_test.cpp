// Formant follower checks: settled off is a plain copy; at SENSITIVITY 50 % the drive reads about 1 on a
// -21 dBFS note, 0.5 on a -27 dBFS note and 0 on a -33 dBFS note, and the full point moves with the pot;
// the follower's attack and release measured on a step; the voice's SHIFT follows RANGE times the drive
// between whole semitones; pluck sweeps a held note from the drive to 0 and fires again on the next note;
// the output stays finite on Adam's clips at every knob corner; and the shifted voice holds the level
// rule on them. The card is set through the registry, as the box sets it. The clip rows skip when
// samples/ is absent. No window, no audio device.

#include <algorithm>
#include <array>
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

// A 150 Hz sine whose peak sits at `peakDbfs`.
Signal sine(int n, double peakDbfs) {
  Signal x(static_cast<size_t>(n));
  const double a = std::pow(10.0, peakDbfs / 20.0);
  for (int i = 0; i < n; ++i) x[static_cast<size_t>(i)] = static_cast<float>(a * std::sin(2.0 * kPi * 150.0 * i / kRate));
  return x;
}

// The card placed and switched on with its pots at `sens` and `range` (0..1), as compose sets it, and
// the pluck switch at `pluck`.
ChainParams boxFollower(float sens, float range, bool on = true, bool pluck = false) {
  ChainParams chain;
  applyPlacement(Card::FormantFollower, chain);
  applyToggle(Card::FormantFollower, on, chain);
  applyKnob(Card::FormantFollower, 0, sens, chain);
  applyKnob(Card::FormantFollower, 1, range, chain);
  chain.follower.tuning.pluck = pluck;
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

// The drive's lowest and highest over the last 0.25 s of a 0.5 s sine note at `peakDbfs`, then after
// 0.5 s of silence, at SENSITIVITY `sens`.
struct Reading {
  float low;
  float high;
  float rest;
};

Reading driveOn(double peakDbfs, float sens) {
  Signal in = sine(kRate / 2, peakDbfs);
  in.resize(kRate, 0.0f);
  Reading r{1.0f, 0.0f, 1.0f};
  through(in, boxFollower(sens, 1.0f), [&](const Stage& s, int done) {
    if (done > kRate / 4 && done <= kRate / 2) {
      r.low = std::min(r.low, s.follower.drive());
      r.high = std::max(r.high, s.follower.drive());
    }
    if (done == kRate) r.rest = s.follower.drive();
  });
  return r;
}

// The full point is -6 - 0.30 x SENSITIVITY % dBFS and the drive rises over the 12 dB under it, so at
// 50 % it reads 1 at -21 dBFS, 0.5 at -27 dBFS and 0 at -33 dBFS. At 0 % the full point is -6 dBFS and
// at 100 % it is -36 dBFS. Silence reads 0.
bool testDriveReadings() {
  bool ok = true;
  struct Row {
    float sens;
    double db;
    float want;
  };
  for (const Row& row : {Row{0.5f, -21.0, 1.0f}, Row{0.5f, -27.0, 0.5f}, Row{0.5f, -33.0, 0.0f}, Row{0.0f, -6.0, 1.0f},
                         Row{0.0f, -18.0, 0.0f}, Row{1.0f, -36.0, 1.0f}, Row{1.0f, -48.0, 0.0f}}) {
    const Reading r = driveOn(row.db, row.sens);
    char name[96];
    std::snprintf(name, sizeof name, "drive: SENSITIVITY %.0f %%, a %.0f dBFS note reads %.1f", 100.0 * row.sens, row.db,
                  static_cast<double>(row.want));
    const float slack = row.want == 0.0f ? 0.0f : 0.04f;
    ok &= report(name, r.low >= row.want - slack && r.high <= row.want + 0.01f && r.rest == 0.0f,
                 "%.3f to %.3f, silence after %.3f", r.low, r.high, r.rest);
  }
  return ok;
}

// The time a one-pole's distance to its target takes to fall to 1/e, read between block ends on the log
// of the distance, which a one-pole stepped once a block follows in a straight line.
double timeToOneOverE(const std::vector<double>& distance) {
  const double ms = 1000.0 * cv::kBlock / kRate;
  double before = 1.0;
  for (size_t k = 0; k < distance.size(); ++k) {
    if (distance[k] <= std::exp(-1.0)) {
      const double t0 = ms * static_cast<double>(k);
      return t0 + ms * (-1.0 - std::log(before)) / (std::log(distance[k]) - std::log(before));
    }
    before = distance[k];
  }
  return -1.0;
}

// A step from silence to a held level of 0.25 for 100 ms, then back to silence for 300 ms, straight into
// the follower: it covers 1 - 1/e of the rise in 3 ms and of the fall in 70 ms.
bool testAttackRelease() {
  auto f = std::make_unique<cv::FormantFollower>();
  f->reset();
  cv::FormantFollowerParams p;
  p.placed = true;
  constexpr float kStep = 0.25f;
  std::array<float, cv::kBlock> held;
  held.fill(kStep);
  const std::array<float, cv::kBlock> quiet{};
  std::vector<double> rise;
  std::vector<double> fall;
  for (int b = 0; b < 75; ++b) {
    f->shiftFor(held.data(), cv::kBlock, p);
    rise.push_back(1.0 - f->level() / kStep);
  }
  const double top = f->level();
  for (int b = 0; b < 225; ++b) {
    f->shiftFor(quiet.data(), cv::kBlock, p);
    fall.push_back(f->level() / top);
  }
  const double attack = timeToOneOverE(rise);
  const double release = timeToOneOverE(fall);
  bool ok = report("follower: attack 3 ms on a step", std::fabs(attack - 3.0) < 0.1, "%.2f ms", attack);
  ok &= report("follower: release 70 ms on a step", std::fabs(release - 70.0) < 1.0, "%.2f ms", release);
  return ok;
}

// With the drive at 1, the voice's glided SHIFT sits on RANGE: on +7 st exactly, and between whole
// semitones (+3.5 st at 75 %) within 0.06 st of it on average and 0.1 st at every call.
bool testShiftFollowsRange() {
  const Signal in = sawtooth(kRate, 0.5f);
  bool ok = true;
  for (const float position : {1.0f, 0.75f, 0.0f}) {
    const float want = -cv::kFollowerMaxRangeSt + 2.0f * cv::kFollowerMaxRangeSt * position;
    double sum = 0.0;
    double worst = 0.0;
    int count = 0;
    through(in, boxFollower(0.5f, position), [&](const Stage& s, int done) {
      if (done < kRate / 2) return;
      const double err = static_cast<double>(s.pitch.shiftSemis()) - want;
      sum += err;
      worst = std::max(worst, std::fabs(err));
      ++count;
    });
    char name[64];
    std::snprintf(name, sizeof name, "shift: RANGE %+.1f st on a loud note", static_cast<double>(want));
    ok &= report(name, std::fabs(sum / count) < 0.06 && worst < 0.1, "mean error %+.3f st, worst %.3f st", sum / count, worst);
  }
  return ok;
}

// Pluck on at SENSITIVITY 50 % and RANGE +7 st: a -15 dBFS note held 0.6 s, 0.4 s of silence, and the
// note again. Each onset fires a sweep that opens near the drive, about 1, falls through the middle by
// 150 ms and reaches 0 by 300 ms, and stays at 0 while the note holds. SHIFT follows it through the
// stage's 20 ms glide, which rounds the peak off below +7 st.
bool testPluck() {
  constexpr int kNote = kRate * 6 / 10;
  constexpr int kGap = kRate * 4 / 10;
  Signal in = sawtooth(2 * kNote + kGap, 0.2f);
  for (int i = kNote; i < kNote + kGap; ++i) in[static_cast<size_t>(i)] = 0.0f;
  float open[2] = {0.0f, 0.0f};
  float middle[2] = {0.0f, 0.0f};
  float held[2] = {0.0f, 0.0f};
  float shiftOpen[2] = {0.0f, 0.0f};
  float shiftHeld[2] = {0.0f, 0.0f};
  const int starts[2] = {0, kNote + kGap};
  through(in, boxFollower(0.5f, 1.0f, true, true), [&](const Stage& s, int done) {
    for (int k = 0; k < 2; ++k) {
      const int t = done - starts[k];
      if (t <= 0 || t > kNote) continue;
      const float e = s.follower.pluckEnvelope();
      const float shift = s.pitch.shiftSemis();
      if (t <= kRate * 3 / 100) open[k] = std::max(open[k], e);
      if (t <= kRate / 10) shiftOpen[k] = std::max(shiftOpen[k], shift);
      if (std::abs(t - kRate * 15 / 100) < cv::kBlock) middle[k] = e;
      if (t >= kRate * 3 / 10) held[k] = std::max(held[k], e);
      if (t >= kRate * 35 / 100) shiftHeld[k] = std::max(shiftHeld[k], std::fabs(shift));
    }
  });
  bool ok = true;
  for (int k = 0; k < 2; ++k) {
    char name[96];
    std::snprintf(name, sizeof name, "pluck: note %d opens near 1, halfway by 150 ms, closed from 300 ms", k + 1);
    ok &= report(name, open[k] > 0.9f && middle[k] > 0.35f && middle[k] < 0.65f && held[k] == 0.0f,
                 "open %.3f, at 150 ms %.3f, held %.3f", open[k], middle[k], held[k]);
    std::snprintf(name, sizeof name, "pluck: note %d SHIFT opens past +5.5 st and rests at 0", k + 1);
    ok &= report(name, shiftOpen[k] > 5.5f && shiftHeld[k] < 0.1f, "peak %+.2f st, held within %.3f st", shiftOpen[k],
                 shiftHeld[k]);
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

// Every knob corner, SENSITIVITY 0 and 100 % by RANGE -7, 0 and +7 st, each with pluck off and on, on
// each clip.
bool testClipCorners(const std::vector<Clip>& all) {
  bool finite = true;
  int runs = 0;
  for (const Clip& c : all) {
    for (const float sens : {0.0f, 1.0f}) {
      for (const float range : {0.0f, 0.5f, 1.0f}) {
        for (const bool pluck : {false, true}) {
          const Signal out = through(c.padded, boxFollower(sens, range, true, pluck));
          for (const float v : out) finite = finite && std::isfinite(v);
          ++runs;
        }
      }
    }
  }
  return report("clips: finite at every knob corner", finite, "%.0f runs", runs);
}

// The shifted voice alone at SENSITIVITY 50 %, at RANGE -7, +3 and +7 st: each clip in the mid band (-1.5
// to +1 dB), as the Formant card's SHIFT rows hold it, and the mean of the three in the rule, 0 to +0.5 dB
// with 0.1 dB of slack.
bool testLevel(const std::vector<Clip>& all) {
  bool ok = true;
  for (const float st : {-7.0f, 3.0f, 7.0f}) {
    const float position = (st + cv::kFollowerMaxRangeSt) / (2.0f * cv::kFollowerMaxRangeSt);
    double sum = 0.0;
    bool band = true;
    for (const Clip& c : all) {
      const Signal out = through(c.padded, boxFollower(0.5f, position));
      const double db = dbOf(rmsRange(out, kPad, out.size()) / rmsRange(c.padded, kPad, c.padded.size()));
      std::printf("NOTE level RANGE %+.0f st %-22s %+6.2f dB\n", static_cast<double>(st), c.name.c_str(), db);
      band = band && db >= -1.5 && db <= 1.0;
      sum += db;
    }
    const double mean = sum / static_cast<double>(all.size());
    char name[64];
    std::snprintf(name, sizeof name, "level: RANGE %+.0f st", static_cast<double>(st));
    ok &= report(name, band && mean >= -0.1 && mean <= 0.6, "mean %+.2f dB (want -0.1..+0.6, each clip -1.5..+1.0)", mean);
  }
  return ok;
}

}  // namespace

int main() {
  bool ok = testOffExact();
  ok &= testDriveReadings();
  ok &= testAttackRelease();
  ok &= testShiftFollowsRange();
  ok &= testPluck();
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
