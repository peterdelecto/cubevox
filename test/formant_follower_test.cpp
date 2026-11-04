// Yawn checks: settled off is a plain copy; RANGE maps 0 st at the pot's bottom and -7 st at its top,
// the default position reads -5 st, and SHIFT never leaves 0 to -7 st; at SENSITIVITY 50 % the drive
// reads about 1 on a -21 dBFS note, 0.5 on a -27 dBFS note and 0 on a -33 dBFS note, and the full point
// moves with the pot; the follower's attack and release measured on a step; each onset fires a sweep
// that opens at the drive, halves at half the decay and rests at 0 while the note holds, and the next
// note fires it again; silence rests at 0; the output stays finite on Adam's clips at every knob corner;
// and the card at its default holds the level rule on them. The card is set through the registry, as the
// box sets it. The clip rows skip when samples/ is absent. No window, no audio device.

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

bool report(const char* name, bool ok, const char* fmt, double a = 0.0, double b = 0.0, double c = 0.0,
            double d = 0.0) {
  char detail[200];
  std::snprintf(detail, sizeof detail, fmt, a, b, c, d);
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

// The RANGE pot's position for `st`, 0 st at 0 and -7 st at 1.
float rangePosition(float st) { return st / cv::kFollowerMinRangeSt; }

// The card placed and switched on with its pots at `sens` and `range` (0..1), as compose sets it, and
// its sweep falling over `decayMs`.
ChainParams boxYawn(float sens, float range, bool on = true, float decayMs = cv::kFollowerDefaultDecayMs) {
  ChainParams chain;
  applyPlacement(Card::FormantFollower, chain);
  applyToggle(Card::FormantFollower, on, chain);
  applyKnob(Card::FormantFollower, 0, sens, chain);
  applyKnob(Card::FormantFollower, 1, range, chain);
  chain.follower.tuning.decayMs = decayMs;
  return chain;
}

// The card as it ships: SENSITIVITY 40 %, RANGE -5 st, decay 300 ms.
ChainParams boxDefault() {
  return boxYawn(cv::kFollowerDefaultSensitivity, rangePosition(cv::kFollowerDefaultRangeSt));
}

// The pitch stage and the card ahead of it, as the chain runs them, on the heap: the ring is large.
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
  bool ok = report("off: placed with its toggle off, a plain copy", through(in, boxYawn(1.0f, 1.0f, false)) == in, "");

  auto s = std::make_unique<Stage>();
  const ChainParams on = boxYawn(1.0f, 1.0f, true);
  const ChainParams offAgain = boxYawn(1.0f, 1.0f, false);
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

// The glided SHIFT's lowest and highest over a loud 0.5 s note with RANGE set straight on the engine.
struct Span {
  float low;
  float high;
};

Span shiftSpan(float rangeSt) {
  ChainParams p = boxYawn(0.5f, 0.0f);
  p.follower.rangeSt = rangeSt;
  Span s{0.0f, 0.0f};
  through(sawtooth(kRate / 2, 0.5f), p, [&](const Stage& st, int) {
    s.low = std::min(s.low, st.pitch.shiftSemis());
    s.high = std::max(s.high, st.pitch.shiftSemis());
  });
  return s;
}

// The RANGE pot hands the card 0 st at its bottom and -7 st at its top in a straight line, and its
// default position, 5/7 of the travel, reads the default of -5 st. RANGE 0 st leaves SHIFT at 0. A RANGE
// past -7 st or above 0 set straight on the engine still keeps SHIFT within 0 to -7 st.
bool testRangeMap() {
  const float bottom = boxYawn(0.5f, 0.0f).follower.rangeSt;
  const float noon = boxYawn(0.5f, 0.5f).follower.rangeSt;
  const float top = boxYawn(0.5f, 1.0f).follower.rangeSt;
  const float position = rangePosition(cv::kFollowerDefaultRangeSt);
  const float dflt = boxYawn(0.5f, position).follower.rangeSt;
  bool ok = report("range: the pot gives 0, -3.5 and -7 st at 0, 50 and 100 %", bottom == 0.0f && noon == -3.5f && top == -7.0f,
                   "%+.2f, %+.2f, %+.2f st", bottom, noon, top);
  ok &= report("range: the default position reads -5 st, the engine's default",
               std::fabs(dflt + 5.0f) < 1e-5f && cv::FormantFollowerParams().rangeSt == -5.0f,
               "%.1f %% of the travel reads %+.4f st", 100.0 * position, dflt);

  const Span none = shiftSpan(0.0f);
  const Span deep = shiftSpan(-12.0f);
  const Span up = shiftSpan(3.0f);
  ok &= report("range: 0 st leaves SHIFT at 0 on a loud note", none.low == 0.0f && none.high == 0.0f, "%+.3f to %+.3f st",
               none.low, none.high);
  ok &= report("range: -12 st on the engine shifts -7 st at most, +3 st shifts nothing",
               deep.low >= -7.0f && deep.low < -6.9f && up.low == 0.0f && up.high == 0.0f,
               "deepest %+.3f st; +3 st spans %+.3f to %+.3f st", deep.low, up.low, up.high);
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
  through(in, boxYawn(sens, 1.0f), [&](const Stage& s, int done) {
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

// What one note of the sweep test read. Times are in samples from the note's start, each call read at
// its end.
struct NoteReading {
  float open = 0.0f;       // the sweep's highest over the first 30 ms
  int lastOnset = 0;       // the last call in the first 60 ms whose sweep sat on the drive, the last onset
  float half = -1.0f;      // the sweep at half the decay after that call
  float at150 = -1.0f;     // the sweep 150 ms into the note
  float held = 0.0f;       // the sweep's highest from one call past the decay's end to the note's end
  float shiftPeak = 0.0f;  // the glided SHIFT's deepest over the first 100 ms, in st
  float shiftRest = 0.0f;  // its furthest from 0 from 100 ms past the decay's end, five glide times, in st
};

// Reads one note from its calls' sweeps, drives and SHIFTs. An onset sets the ramp to 1, so the sweep of
// an onset call equals the drive.
NoteReading readNote(const std::vector<float>& sweep, const std::vector<float>& drive, const std::vector<float>& shift,
                     int decay) {
  NoteReading r;
  const auto timeOf = [](size_t b) { return static_cast<int>(b + 1) * cv::kBlock; };
  for (size_t b = 0; b < sweep.size(); ++b) {
    const int t = timeOf(b);
    if (t <= kRate * 6 / 100 && drive[b] > 0.0f && sweep[b] == drive[b]) r.lastOnset = t;
    if (t <= kRate * 3 / 100) r.open = std::max(r.open, sweep[b]);
    if (std::abs(t - kRate * 15 / 100) <= cv::kBlock / 2) r.at150 = sweep[b];
    if (t <= kRate / 10) r.shiftPeak = std::min(r.shiftPeak, shift[b]);
  }
  for (size_t b = 0; b < sweep.size(); ++b) {
    const int t = timeOf(b);
    if (std::abs(t - (r.lastOnset + decay / 2)) <= cv::kBlock / 2) r.half = sweep[b];
    if (t > r.lastOnset + decay + cv::kBlock) r.held = std::max(r.held, sweep[b]);
    if (t >= r.lastOnset + decay + kRate / 10) r.shiftRest = std::max(r.shiftRest, std::fabs(shift[b]));
  }
  return r;
}

// The sweep at SENSITIVITY 50 % and RANGE -7 st over `decayMs`: a -14 dBFS note held 0.6 s, 0.4 s of
// silence, and the note again. The rise detector refires the onset for the first 10 ms or so of each
// note, so the ramp starts from the last of those calls. Also reads the silence between the notes from
// 300 ms on.
bool testSweep(float decayMs) {
  constexpr int kNote = kRate * 6 / 10;
  constexpr int kGap = kRate * 4 / 10;
  Signal in = sawtooth(2 * kNote + kGap, 0.2f);
  for (int i = kNote; i < kNote + kGap; ++i) in[static_cast<size_t>(i)] = 0.0f;
  const int starts[2] = {0, kNote + kGap};
  std::vector<float> sweep[2];
  std::vector<float> drive[2];
  std::vector<float> shift[2];
  float restSweep = 0.0f;
  float restDrive = 0.0f;
  float restShift = 0.0f;
  through(in, boxYawn(0.5f, 1.0f, true, decayMs), [&](const Stage& s, int done) {
    if (done > kNote + kRate * 3 / 10 && done <= kNote + kGap) {
      restSweep = std::max(restSweep, s.follower.sweep());
      restDrive = std::max(restDrive, s.follower.drive());
      restShift = std::max(restShift, std::fabs(s.pitch.shiftSemis()));
    }
    for (int k = 0; k < 2; ++k) {
      const int t = done - starts[k];
      if (t <= 0 || t > kNote) continue;
      sweep[k].push_back(s.follower.sweep());
      drive[k].push_back(s.follower.drive());
      shift[k].push_back(s.pitch.shiftSemis());
    }
  });
  const int decay = static_cast<int>(decayMs * cv::kSmpPerMs);
  const NoteReading note[2] = {readNote(sweep[0], drive[0], shift[0], decay), readNote(sweep[1], drive[1], shift[1], decay)};

  bool ok = true;
  for (int k = 0; k < 2; ++k) {
    const NoteReading& r = note[k];
    char name[128];
    std::snprintf(name, sizeof name, "sweep: decay %.0f ms, note %d opens at 1, halves at half the decay, 0 from its end",
                  static_cast<double>(decayMs), k + 1);
    ok &= report(name, r.open > 0.999f && std::fabs(r.half - 0.5f) < 0.02f && r.held == 0.0f,
                 "open %.3f, last onset at %.1f ms, at half the decay %.3f, held %.3f", r.open,
                 1000.0 * r.lastOnset / kRate, r.half, r.held);

    // The ramp is a straight line from the last onset: 1 - (150 ms - onset) / decay at 150 ms, 0.456 at
    // the owner's 250 ms pick.
    const float line = 1.0f - static_cast<float>(kRate * 15 / 100 - r.lastOnset) / static_cast<float>(decay);
    std::snprintf(name, sizeof name, "sweep: decay %.0f ms, note %d falls in a straight line", static_cast<double>(decayMs),
                  k + 1);
    ok &= report(name, std::fabs(r.at150 - line) < 0.01f, "at 150 ms %.3f, the line %.3f", r.at150, line);
    std::snprintf(name, sizeof name, "sweep: decay %.0f ms, note %d SHIFT opens past -5.5 st and rests within 0.01 st of 0",
                  static_cast<double>(decayMs), k + 1);
    ok &= report(name, r.shiftPeak < -5.5f && r.shiftRest < 0.01f, "peak %+.2f st, rests within %.4f st", r.shiftPeak,
                 r.shiftRest);
  }
  char name[96];
  std::snprintf(name, sizeof name, "sweep: decay %.0f ms, silence from 300 ms reads 0 and SHIFT rests at 0",
                static_cast<double>(decayMs));
  ok &= report(name, restSweep == 0.0f && restDrive == 0.0f && restShift < 0.01f, "sweep %.3f, drive %.3f, SHIFT within %.4f st",
               restSweep, restDrive, restShift);
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

// Every knob corner, SENSITIVITY 0 and 100 % by RANGE 0 and -7 st by decay 10 and 2000 ms, on each clip.
bool testClipCorners(const std::vector<Clip>& all) {
  bool finite = true;
  int runs = 0;
  for (const Clip& c : all) {
    for (const float sens : {0.0f, 1.0f}) {
      for (const float range : {0.0f, 1.0f}) {
        for (const float decayMs : {cv::kFollowerMinDecayMs, cv::kFollowerMaxDecayMs}) {
          const Signal out = through(c.padded, boxYawn(sens, range, true, decayMs));
          for (const float v : out) finite = finite && std::isfinite(v);
          ++runs;
        }
      }
    }
  }
  return report("clips: finite at every knob corner", finite, "%.0f runs", runs);
}

// The card at its default on each clip, out/in RMS after the pad.
double levelDb(const Clip& c, const ChainParams& p) {
  const Signal out = through(c.padded, p);
  return dbOf(rmsRange(out, kPad, out.size()) / rmsRange(c.padded, kPad, c.padded.size()));
}

// The level rule: the card at its default reads 0 to +0.5 dB over its input on each clip. The owner's
// three picks print beside it.
bool testLevel(const std::vector<Clip>& all) {
  bool ok = true;
  for (const Clip& c : all) {
    const double db = levelDb(c, boxDefault());
    char name[96];
    std::snprintf(name, sizeof name, "level: the default on %s", c.name.c_str());
    ok &= report(name, db >= 0.0 && db <= 0.5, "%+.2f dB (want 0..+0.5)", db);
  }
  struct Pick {
    float sens;
    float decayMs;
  };
  for (const Pick& pick : {Pick{0.5f, 250.0f}, Pick{0.5f, 500.0f}, Pick{0.25f, 250.0f}}) {
    for (const Clip& c : all) {
      std::printf("NOTE level SENSITIVITY %.0f %% RANGE -7 st decay %.0f ms %-22s %+6.2f dB\n", 100.0 * pick.sens,
                  static_cast<double>(pick.decayMs), c.name.c_str(), levelDb(c, boxYawn(pick.sens, 1.0f, true, pick.decayMs)));
    }
  }
  return ok;
}

}  // namespace

int main() {
  bool ok = testOffExact();
  ok &= testRangeMap();
  ok &= testDriveReadings();
  ok &= testAttackRelease();
  ok &= testSweep(250.0f);
  ok &= testSweep(cv::kFollowerDefaultDecayMs);
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
