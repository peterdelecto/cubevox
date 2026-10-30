// Checks the levels behind the voice screen on the host. Covers block energy, the input bar's dB
// mapping and its instant attack and 120 ms release, and the output ribbon's window. The window top
// rises at once, falls 10 dB a second and never sits under -30 dBFS. The ribbon reads each tick's
// amplitude over the top's with no release. A floor follows the quietest tick, and a tick under the
// floor plus 6 dB reads as silence.

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

// Reads one tick of `tickDb` against a top that a tick at `topDb` has just set. The tick lasts 0 ms,
// so the top and the floor have not moved when it is read.
float readAgainst(float topDb, float tickDb) {
  level::OutputMeter meter;
  meter.update(energyOfDb(topDb), kTickMs);
  return meter.update(energyOfDb(tickDb), 0.0f);
}

void checkWindowConstants() {
  check(level::kWindowMinTopDbfs == -30.0f && level::kWindowFallDbPerS == 10.0f, "window: -30 dBFS minimum top, 10 dB a second");
  check(level::kWindowFloorDbfs == -60.0f && level::kWindowFloorRiseDbPerS == 3.0f,
        "window: the floor never drops under -60 dBFS and rises 3 dB a second");
  check(level::kWindowFloorUnderTopDb == 12.0f && level::kWindowCutoffMarginDb == 6.0f,
        "window: the floor stays 12 dB under the top and the cutoff sits 6 dB over the floor");

  level::OutputMeter fresh;
  check(fresh.top() == -30.0f && fresh.floor() == -60.0f && fresh.bottom() == -54.0f,
        "window: a fresh meter has its top at -30, its floor at -60 and its cutoff at -54 dBFS");
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

void checkWindowTopMinimum() {
  level::OutputMeter meter;
  meter.update(energyOfDb(-10.0f), kTickMs);
  for (int i = 0; i < 100; ++i) meter.update(energyOfDb(-70.0f), kTickMs);
  check(meter.top() == -30.0f, "window: the top stops falling at -30 dBFS");
  meter.update(level::OutputEnergy(), 10000.0f);
  check(meter.top() == -30.0f, "window: a long silence leaves the top at -30 dBFS");
}

void checkWindowAmplitudeMap() {
  // The level is the tick's amplitude over the top's, so each 6 dB under the top halves it.
  check(near(readAgainst(-10.0f, -10.0f), 1.0f), "window: a tick at the top reads 1.0");
  check(near(readAgainst(-10.0f, -16.0f), 0.501f), "window: a tick 6 dB under the top reads 0.501");
  check(near(readAgainst(-10.0f, -30.0f), 0.1f), "window: a tick 20 dB under the top reads 0.1");
  check(near(readAgainst(0.0f, -40.0f), 0.01f), "window: a tick 40 dB under the top reads 0.01");

  // A fresh meter has its top at -30 dBFS, so a tick under it reads the same way.
  level::OutputMeter fresh;
  check(near(fresh.update(energyOfDb(-36.0f), kTickMs), 0.501f), "window: -36 dBFS under the minimum top reads 0.501");

  // Above the cutoff a louder tick is never lower, and none passes 1.
  bool rising = true;
  float prev = 0.0f;
  for (float db = -53.0f; db <= 0.0f; db += 0.5f) {
    const float now = readAgainst(0.0f, db);
    rising = rising && now >= prev && now <= 1.0f;
    prev = now;
  }
  check(rising, "window: a louder tick is never lower and never over 1");
}

void checkWindowFloorFalls() {
  // 100 ticks at -20 dBFS raise the floor from -60 to -48 dBFS.
  level::OutputMeter meter;
  for (int i = 0; i < 100; ++i) meter.update(energyOfDb(-20.0f), kTickMs);
  check(near(meter.floor(), -48.0f), "window: a steady tick at -20 dBFS raises the floor to -48 dBFS in 100 ticks");

  meter.update(energyOfDb(-55.0f), kTickMs);
  check(near(meter.floor(), -55.0f), "window: a quieter tick takes the floor down to it at once");
  meter.update(energyOfDb(-80.0f), kTickMs);
  check(meter.floor() == -60.0f, "window: the floor never drops under -60 dBFS");

  level::OutputMeter hushed;
  for (int i = 0; i < 100; ++i) hushed.update(energyOfDb(-20.0f), kTickMs);
  hushed.update(level::OutputEnergy(), kTickMs);
  check(hushed.floor() == -60.0f, "window: silence takes the floor to -60 dBFS");
}

void checkWindowFloorRises() {
  // 25 ticks of 40 ms are one second, which lifts the floor by 3 dB.
  level::OutputMeter meter;
  for (int i = 0; i < 25; ++i) meter.update(energyOfDb(-20.0f), kTickMs);
  check(near(meter.floor(), -57.0f, 0.01f), "window: the floor rises 3 dB in a second of louder ticks");

  // The rise runs on real time, so one 1000 ms tick lifts it as far as 25 of 40 ms.
  level::OutputMeter late;
  late.update(energyOfDb(-20.0f), 1000.0f);
  check(near(late.floor(), -57.0f), "window: a late tick of 1000 ms lifts the floor by 3 dB");

  // The rise is the tick being louder than the floor, so a tick on the floor lifts it too.
  level::OutputMeter onFloor;
  onFloor.update(energyOfDb(-60.0f), 1000.0f);
  check(near(onFloor.floor(), -57.0f), "window: a tick at the floor lifts it");
}

void checkWindowFloorUnderTop() {
  // A steady tick at 0 dBFS lifts the floor 0.12 dB a tick toward the top. The top wavers by 0.4 dB
  // as the tick meets it, so the cap does too.
  level::OutputMeter meter;
  bool capped = true;
  for (int i = 0; i < 600; ++i) {
    meter.update(energyOfDb(0.0f), kTickMs);
    capped = capped && meter.floor() <= meter.top() - 12.0f + 1e-3f;
  }
  check(capped, "window: the floor never rises over 12 dB under the top");
  check(near(meter.floor(), meter.top() - 12.0f, 0.5f), "window: a steady tick at 0 dBFS holds the floor at the cap");
  check(near(meter.update(energyOfDb(0.0f), kTickMs), 1.0f), "window: the loudest recent tick still reads full");

  // A falling top drags the floor down with it.
  level::OutputMeter drag;
  for (int i = 0; i < 600; ++i) drag.update(energyOfDb(0.0f), kTickMs);
  drag.update(energyOfDb(-20.0f), 1000.0f);
  check(near(drag.top(), -10.0f, 0.5f) && near(drag.floor(), drag.top() - 12.0f),
        "window: a falling top lowers the floor to 12 dB under it");
}

void checkWindowCutoff() {
  level::OutputMeter edge;
  edge.update(energyOfDb(-70.0f), kTickMs);
  check(near(edge.floor(), -60.0f) && near(edge.bottom(), edge.floor() + 6.0f), "window: the cutoff is 6 dB over the floor");

  // A fresh meter has its floor at -60, so after one 40 ms tick the cutoff is -53.88 dBFS.
  level::OutputMeter fresh;
  check(fresh.update(energyOfDb(-54.5f), kTickMs) == 0.0f, "window: a tick under the floor plus 6 dB reads 0");
  level::OutputMeter above;
  check(near(above.update(energyOfDb(-53.0f), kTickMs), std::pow(10.0f, -23.0f / 20.0f)), "window: a tick over the floor plus 6 dB reads its amplitude");
  level::OutputMeter hiss;
  check(hiss.update(energyOfDb(-60.0f), kTickMs) == 0.0f && hiss.update(energyOfDb(-70.0f), kTickMs) == 0.0f,
        "window: the idle hiss at -60 and -70 dBFS reads 0");

  // With the floor raised to -48 dBFS the cutoff sits at -42 dBFS.
  level::OutputMeter raised;
  for (int i = 0; i < 100; ++i) raised.update(energyOfDb(-20.0f), kTickMs);
  check(raised.update(energyOfDb(-44.0f), kTickMs) == 0.0f, "window: a raised floor raises the cutoff");
}

void checkWindowSteadyTone() {
  // A tick at -45 dBFS under a top of -30 reads 0.178 while the floor is low. The floor rises
  // 0.12 dB a tick from -60, and the cutoff (floor + 6) passes -45 when the floor passes -51, which
  // takes (-45 - (-60 + 6)) / 0.12 = 75 ticks.
  constexpr int kTicksToCutoff = 75;
  level::OutputMeter meter;
  const float first = meter.update(energyOfDb(-45.0f), kTickMs);
  check(near(first, 0.178f), "window: a steady tick at -45 dBFS reads 0.178 at first");

  bool held = true;
  for (int i = 2; i < kTicksToCutoff; ++i) held = held && near(meter.update(energyOfDb(-45.0f), kTickMs), 0.178f);
  check(held, "window: it reads 0.178 for the first 74 ticks");

  // Tick 75 puts the cutoff on the tick, which rounding decides, so the check starts at tick 76.
  meter.update(energyOfDb(-45.0f), kTickMs);
  check(meter.update(energyOfDb(-45.0f), kTickMs) == 0.0f && near(meter.floor(), -50.88f, 0.01f),
        "window: it reads 0 once the floor has risen past -51 dBFS, at tick 76");

  bool silent = true;
  for (int i = 0; i < 200; ++i) silent = silent && meter.update(energyOfDb(-45.0f), kTickMs) == 0.0f;
  check(silent, "window: it keeps reading 0 after that");
}

void checkWindowNoRelease() {
  level::OutputMeter silent;
  check(silent.update(level::OutputEnergy(), kTickMs) == 0.0f, "window: starts silent");

  level::OutputMeter meter;
  check(near(meter.update(energyOfDb(-6.0f), kTickMs), 1.0f), "window: a loud tick shows at once");
  check(meter.update(level::OutputEnergy(), kTickMs) == 0.0f, "window: a silent tick after a loud one reads 0 at once");

  // A quieter tick after a loud one reads its own amplitude over the top, with nothing eased in.
  level::OutputMeter after;
  after.update(energyOfDb(-6.0f), kTickMs);
  const float own = after.update(energyOfDb(-26.0f), kTickMs);
  check(near(own, std::pow(10.0f, (-26.0f - after.top()) / 20.0f)) && own < 0.15f, "window: a quieter tick reads its own level");
}

void checkWindowBadEnergy() {
  level::OutputMeter meter;
  level::OutputEnergy bad;
  bad.samples = 10;
  bad.sumSquares = std::numeric_limits<float>::quiet_NaN();
  check(meter.update(bad, kTickMs) == 0.0f && meter.top() == -30.0f && meter.floor() == -60.0f,
        "window: a NaN sum reads as silence and leaves the top and the floor");
  check(meter.update(level::OutputEnergy(), kTickMs) == 0.0f && meter.top() == -30.0f && meter.floor() == -60.0f,
        "window: a tick with no samples reads as silence and leaves the top and the floor");
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
  checkWindowTopMinimum();
  checkWindowAmplitudeMap();
  checkWindowFloorFalls();
  checkWindowFloorRises();
  checkWindowFloorUnderTop();
  checkWindowCutoff();
  checkWindowSteadyTone();
  checkWindowNoRelease();
  checkWindowBadEnergy();
}
