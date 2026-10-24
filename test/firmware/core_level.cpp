// Checks the input level behind the voice ribbon on the host. Covers block energy, the dB
// mapping with its anchors and curve, and the meter's instant attack and 120 ms release.

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

void checkMapping() {
  check(std::fabs(level::displayLevel(dbToLinear(level::kFullDbfs)) - 1.0f) < 1e-4f, "level: -6 dBFS is full height");
  check(level::displayLevel(dbToLinear(level::kFloorDbfs + 0.5f)) > 0.0f && level::displayLevel(dbToLinear(level::kFloorDbfs)) < 1e-3f,
        "level: -50 dBFS is the floor");
  check(level::displayLevel(dbToLinear(-60.0f)) == 0.0f && level::displayLevel(0.0f) == 0.0f,
        "level: below -50 dBFS and silence are zero");
  check(level::displayLevel(1.0f) == 1.0f && level::displayLevel(8.0f) == 1.0f, "level: above -6 dBFS stays at full height");
  check(level::displayLevel(std::numeric_limits<float>::quiet_NaN()) == 0.0f, "level: NaN reads as zero");

  // The middle of the dB range sits at 0.5 before the curve.
  const float mid = level::displayLevel(dbToLinear((level::kFullDbfs + level::kFloorDbfs) / 2.0f));
  check(std::fabs(mid - std::pow(0.5f, level::kCurve)) < 1e-3f, "level: the dB midpoint takes the 0.6 curve");

  bool rising = true;
  float prev = 0.0f;
  for (float db = -60.0f; db <= 0.0f; db += 0.5f) {
    const float now = level::displayLevel(dbToLinear(db));
    rising = rising && now >= prev;
    prev = now;
  }
  check(rising, "level: the level never falls as the input rises");
}

void checkMeterAttackAndRelease() {
  level::Meter meter;
  check(meter.value() == 0.0f, "meter: starts silent");
  const float loud = meter.update(energyOfRms(dbToLinear(-6.0f)), kTickMs);
  check(std::fabs(loud - 1.0f) < 1e-4f, "meter: a loud tick shows at once");

  // After one tick of silence the level falls by exp(-40 / 120).
  const float after1 = meter.update(level::InputEnergy(), kTickMs);
  check(std::fabs(after1 - std::exp(-kTickMs / level::kReleaseMs)) < 1e-4f, "meter: release falls by exp(-dt / 120 ms) per tick");
  meter.update(level::InputEnergy(), kTickMs);
  const float after3 = meter.update(level::InputEnergy(), kTickMs);
  check(std::fabs(after3 - std::exp(-1.0f)) < 1e-3f, "meter: 120 ms after the peak the level is 1 / e");

  int ticks = 3;
  while (meter.value() > 0.0f && ticks < 100) {
    meter.update(level::InputEnergy(), kTickMs);
    ++ticks;
  }
  check(meter.value() == 0.0f && ticks == 14, "meter: a decayed level snaps to zero once under the silence mark");

  // A quieter tick after a loud one releases toward the new level, a louder tick takes it at once.
  meter.update(energyOfRms(dbToLinear(-6.0f)), kTickMs);
  const float quiet = level::displayLevel(dbToLinear(-30.0f));
  const float eased = meter.update(energyOfRms(dbToLinear(-30.0f)), kTickMs);
  const float want = quiet + (1.0f - quiet) * std::exp(-kTickMs / level::kReleaseMs);
  check(std::fabs(eased - want) < 1e-3f, "meter: a quieter tick releases toward its own level");
  const float up = meter.update(energyOfRms(dbToLinear(-12.0f)), kTickMs);
  check(std::fabs(up - level::displayLevel(dbToLinear(-12.0f))) < 1e-4f, "meter: a louder tick is taken at once");
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

}  // namespace

void checkLevel() {
  checkEnergy();
  checkMapping();
  checkMeterAttackAndRelease();
  checkMeterTickLength();
}
