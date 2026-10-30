// Checks the levels behind the voice screen on the host. Covers block energy, the input bar's dB
// mapping and its instant attack and 120 ms release, and the output ribbon's window, which rises at
// once, falls 10 dB a second, never sits under -30 dBFS and spans 44 dB.

#include "support.h"

#include <cmath>
#include <cstdint>
#include <limits>

#include "core/level.h"

namespace {

constexpr float kPi = 3.14159265358979f;
constexpr int kBlock = 64;
constexpr float kTickMs = 40.0f;

float dbToLinear(float db) { return std::pow(10.0f, db / 20.0f); }

// A full-scale sine of the given peak, four whole cycles in a 64-sample block, so one block holds
// whole cycles and its RMS is exactly peak / sqrt(2).
void sineBlock(float peak, float* out) {
  for (int i = 0; i < kBlock; ++i) out[i] = peak * std::sin(2.0f * kPi * 4.0f * static_cast<float>(i) / kBlock);
}

level::InputEnergy energyOfRms(float rms) {
  level::InputEnergy e;
  e.samples = 1920;  // one tick of audio at 48 kHz
  e.sumSquares = rms * rms * static_cast<float>(e.samples);
  return e;
}

level::InputEnergy energyOfDb(float db) { return energyOfRms(dbToLinear(db)); }

void checkEnergy() {
  float block[kBlock];
  sineBlock(0.5f, block);
  level::InputEnergy e;
  for (int b = 0; b < 30; ++b) {
    e.sumSquares += level::sumSquares(block, kBlock);
    e.samples += kBlock;
  }
  check(e.samples == 1920, "level: thirty blocks make one tick of samples");
  check(std::fabs(level::rms(e) - 0.5f / std::sqrt(2.0f)) < 1e-4f, "level: the RMS of a sine is its peak over root two");

  const float ramp[4] = {1.0f, -1.0f, 0.5f, 0.0f};
  check(std::fabs(level::sumSquares(ramp, 4) - 2.25f) < 1e-6f && level::sumSquares(ramp, 0) == 0.0f,
        "level: sumSquares adds the squares and an empty block adds nothing");
  check(level::rms(level::InputEnergy()) == 0.0f, "level: no samples read as silence");
  level::InputEnergy bad;
  bad.samples = 10;
  bad.sumSquares = std::numeric_limits<float>::quiet_NaN();
  check(level::rms(bad) == 0.0f, "level: a NaN sum reads as silence");
}

void checkDbfs() {
  check(std::fabs(level::toDbfs(1.0f)) < 1e-6f && std::fabs(level::toDbfs(0.1f) + 20.0f) < 1e-4f,
        "level: toDbfs reads full scale as 0 and a tenth as -20");
  check(level::toDbfs(0.0f) == level::kNoSignalDbfs && level::toDbfs(-1.0f) == level::kNoSignalDbfs &&
            level::toDbfs(std::numeric_limits<float>::quiet_NaN()) == level::kNoSignalDbfs,
        "level: silence, a negative amplitude and NaN read as no signal");
}

void checkMapping() {
  check(level::kFullDbfs == 0.0f && level::kFloorDbfs == -50.0f, "level: the input bar runs from -50 to 0 dBFS");
  check(std::fabs(level::displayLevel(dbToLinear(level::kFullDbfs)) - 1.0f) < 1e-4f, "level: 0 dBFS is full");
  check(level::displayLevel(dbToLinear(level::kFloorDbfs)) < 1e-3f && level::displayLevel(dbToLinear(-49.0f)) > 0.0f,
        "level: -50 dBFS is the floor");
  check(level::displayLevel(dbToLinear(-60.0f)) == 0.0f && level::displayLevel(0.0f) == 0.0f,
        "level: below -50 dBFS and silence are zero");
  check(level::displayLevel(1.0f) == 1.0f && level::displayLevel(8.0f) == 1.0f, "level: above 0 dBFS stays full");
  check(level::displayLevel(std::numeric_limits<float>::quiet_NaN()) == 0.0f, "level: NaN reads as zero");

  // The map is linear in dB with no curve, so each quarter of the span is a quarter of the level.
  check(std::fabs(level::displayLevel(dbToLinear(-25.0f)) - 0.5f) < 1e-4f, "level: the dB midpoint is 0.5");
  check(std::fabs(level::displayLevel(dbToLinear(-37.5f)) - 0.25f) < 1e-4f &&
            std::fabs(level::displayLevel(dbToLinear(-12.5f)) - 0.75f) < 1e-4f,
        "level: the map is linear in dB");

  bool rising = true;
  float prev = 0.0f;
  for (float db = -60.0f; db <= 6.0f; db += 0.5f) {
    const float now = level::displayLevel(dbToLinear(db));
    rising = rising && now >= prev;
    prev = now;
  }
  check(rising, "level: the level never falls as the input rises");
}

void checkMeterAttackAndRelease() {
  level::Meter silent;
  check(silent.update(level::InputEnergy(), kTickMs) == 0.0f, "meter: starts silent");
  level::Meter meter;
  const float loud = meter.update(energyOfDb(0.0f), kTickMs);
  check(std::fabs(loud - 1.0f) < 1e-4f, "meter: a loud tick shows at once");

  // After one tick of silence the level falls by exp(-40 / 120).
  const float after1 = meter.update(level::InputEnergy(), kTickMs);
  check(std::fabs(after1 - std::exp(-kTickMs / level::kReleaseMs)) < 1e-4f, "meter: release falls by exp(-dt / 120 ms) per tick");
  meter.update(level::InputEnergy(), kTickMs);
  const float after3 = meter.update(level::InputEnergy(), kTickMs);
  check(std::fabs(after3 - std::exp(-1.0f)) < 1e-3f, "meter: 120 ms after the peak the level is 1 / e");

  int ticks = 3;
  float shown = after3;
  while (shown > 0.0f && ticks < 100) {
    shown = meter.update(level::InputEnergy(), kTickMs);
    ++ticks;
  }
  check(shown == 0.0f && ticks == 14, "meter: a decayed level snaps to zero once under the silence mark");

  // A quieter tick after a loud one releases toward the new level, a louder tick takes it at once.
  meter.update(energyOfDb(0.0f), kTickMs);
  const float quiet = level::displayLevel(dbToLinear(-30.0f));
  const float eased = meter.update(energyOfDb(-30.0f), kTickMs);
  const float want = quiet + (1.0f - quiet) * std::exp(-kTickMs / level::kReleaseMs);
  check(std::fabs(eased - want) < 1e-3f, "meter: a quieter tick releases toward its own level");
  const float up = meter.update(energyOfDb(-3.0f), kTickMs);
  check(std::fabs(up - level::displayLevel(dbToLinear(-3.0f))) < 1e-4f, "meter: a louder tick is taken at once");
}

// The tick length scales the release, so a late tick falls further.
void checkMeterTickLength() {
  level::Meter fast;
  level::Meter slow;
  fast.update(energyOfRms(1.0f), kTickMs);
  slow.update(energyOfRms(1.0f), kTickMs);
  const float a = fast.update(level::InputEnergy(), 20.0f);
  const float b = slow.update(level::InputEnergy(), 80.0f);
  check(a > b && std::fabs(b - std::exp(-80.0f / level::kReleaseMs)) < 1e-4f, "meter: a longer tick falls further");
}

// ---------------------------------------------------------------------------------------------
// The output window

bool near(float a, float b, float tolerance = 1e-3f) { return std::fabs(a - b) < tolerance; }

void checkWindowConstants() {
  check(level::kWindowSpanDb == 44.0f && level::kWindowMinTopDbfs == -30.0f && level::kWindowFallDbPerS == 10.0f,
        "window: 44 dB span, -30 dBFS minimum top, 10 dB a second");
  level::OutputMeter fresh;
  check(fresh.top() == -30.0f, "window: the top starts at the minimum");
}

void checkWindowRisesAtOnce() {
  level::OutputMeter meter;
  const float shown = meter.update(energyOfDb(-10.0f), kTickMs);
  check(near(meter.top(), -10.0f) && near(shown, 1.0f), "window: a tick above the top moves the top to it at once, and reads full");
  meter.update(energyOfDb(-3.0f), kTickMs);
  check(near(meter.top(), -3.0f), "window: a louder tick moves the top again");
  meter.update(energyOfDb(0.0f), kTickMs);
  check(near(meter.top(), 0.0f), "window: the top reaches 0 dBFS");

  // A tick equal to the top does not raise it.
  level::OutputMeter edge;
  edge.update(energyOfDb(-30.0f), kTickMs);
  check(near(edge.top(), -30.0f), "window: a tick at the minimum top leaves it there");
}

void checkWindowFalls() {
  level::OutputMeter meter;
  meter.update(energyOfDb(-10.0f), kTickMs);
  meter.update(energyOfDb(-50.0f), kTickMs);
  check(near(meter.top(), -10.4f), "window: a quieter tick of 40 ms lowers the top by 0.4 dB");
  for (int i = 0; i < 24; ++i) meter.update(energyOfDb(-50.0f), kTickMs);
  check(near(meter.top(), -20.0f), "window: the top falls 10 dB in a second");

  // The fall runs on real time, so one 200 ms tick falls as far as five of 40 ms.
  level::OutputMeter late;
  late.update(energyOfDb(-10.0f), kTickMs);
  late.update(energyOfDb(-50.0f), 200.0f);
  check(near(late.top(), -12.0f), "window: a late tick of 200 ms lowers the top by 2 dB");

  // Silence falls the same way.
  level::OutputMeter hushed;
  hushed.update(energyOfDb(-10.0f), kTickMs);
  hushed.update(level::OutputEnergy(), 500.0f);
  check(near(hushed.top(), -15.0f), "window: silence lowers the top at the same rate");
}

void checkWindowFloor() {
  level::OutputMeter meter;
  meter.update(energyOfDb(-10.0f), kTickMs);
  for (int i = 0; i < 100; ++i) meter.update(energyOfDb(-70.0f), kTickMs);
  check(meter.top() == -30.0f, "window: the top stops falling at -30 dBFS");
  meter.update(level::OutputEnergy(), 10000.0f);
  check(meter.top() == -30.0f, "window: a long silence leaves the top at -30 dBFS");

  // At the floor the window is -74 to -30 dBFS.
  level::OutputMeter low;
  check(near(low.update(energyOfDb(-52.0f), kTickMs), std::pow(0.5f, level::kCurve)), "window: -52 dBFS sits at the middle of the floor window");
  level::OutputMeter edge;
  check(near(edge.update(energyOfDb(-30.0f), kTickMs), 1.0f), "window: -30 dBFS fills the floor window");
}

void checkWindowSpan() {
  // Fresh meters sit at the -30 dBFS top, so the window is -74 to -30 dBFS.
  level::OutputMeter bottom;
  check(bottom.update(energyOfDb(-74.0f), kTickMs) == 0.0f, "window: 44 dB under the top reads zero");
  level::OutputMeter under;
  check(under.update(energyOfDb(-90.0f), kTickMs) == 0.0f, "window: under the window reads zero");
  level::OutputMeter quarter;
  check(near(quarter.update(energyOfDb(-63.0f), kTickMs), std::pow(0.25f, level::kCurve)), "window: a quarter of the way up takes the 0.6 curve");
  level::OutputMeter half;
  check(near(half.update(energyOfDb(-52.0f), kTickMs), std::pow(0.5f, level::kCurve)), "window: the middle takes the 0.6 curve");

  // A raised top moves the window up with it. After a -10 dBFS tick and a 40 ms fall the top is
  // -10.4 dBFS, so the window is -54.4 to -10.4 dBFS. The previous level releases toward the new one.
  level::OutputMeter raised;
  raised.update(energyOfDb(-10.0f), kTickMs);
  const float shown = raised.update(energyOfDb(-32.4f), kTickMs);
  const float target = std::pow(0.5f, level::kCurve);
  const float want = target + (1.0f - target) * std::exp(-kTickMs / level::kReleaseMs);
  check(near(raised.top(), -10.4f) && near(shown, want), "window: a raised top lifts the whole window with it");

  bool rising = true;
  float prev = 0.0f;
  for (float db = -90.0f; db <= -30.0f; db += 0.5f) {
    level::OutputMeter m;
    const float now = m.update(energyOfDb(db), kTickMs);
    rising = rising && now >= prev;
    prev = now;
  }
  check(rising, "window: inside the floor window a louder tick is never lower");
}

void checkWindowSilenceAndRelease() {
  level::OutputMeter silent;
  check(silent.update(level::OutputEnergy(), kTickMs) == 0.0f, "window: starts silent");

  level::OutputMeter meter;
  check(near(meter.update(energyOfDb(-6.0f), kTickMs), 1.0f), "window: a loud tick shows at once");
  const float after1 = meter.update(level::OutputEnergy(), kTickMs);
  check(near(after1, std::exp(-kTickMs / level::kReleaseMs), 1e-4f), "window: release falls by exp(-dt / 120 ms) per tick");

  int ticks = 1;
  float shown = after1;
  while (shown > 0.0f && ticks < 100) {
    shown = meter.update(level::OutputEnergy(), kTickMs);
    ++ticks;
  }
  check(shown == 0.0f && ticks == 14, "window: a decayed level snaps to zero once under the silence mark");

  // A tick in the middle of a hush after a loud one releases toward its own level.
  level::OutputMeter after;
  after.update(energyOfDb(-6.0f), kTickMs);
  const float eased = after.update(energyOfDb(-16.0f), kTickMs);
  const float position = (-16.0f - (after.top() - level::kWindowSpanDb)) / level::kWindowSpanDb;
  const float target = std::pow(position, level::kCurve);
  check(near(eased, target + (1.0f - target) * std::exp(-kTickMs / level::kReleaseMs)), "window: a quieter tick releases toward its own level");
}

void checkWindowBadEnergy() {
  level::OutputMeter meter;
  level::OutputEnergy bad;
  bad.samples = 10;
  bad.sumSquares = std::numeric_limits<float>::quiet_NaN();
  check(meter.update(bad, kTickMs) == 0.0f && meter.top() == -30.0f, "window: a NaN sum reads as silence");
}

}  // namespace

void checkLevel() {
  checkEnergy();
  checkDbfs();
  checkMapping();
  checkMeterAttackAndRelease();
  checkMeterTickLength();
  checkWindowConstants();
  checkWindowRisesAtOnce();
  checkWindowFalls();
  checkWindowFloor();
  checkWindowSpan();
  checkWindowSilenceAndRelease();
  checkWindowBadEnergy();
}
