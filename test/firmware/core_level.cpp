// Checks the input level behind the voice screen on the host. Covers block energy and the input
// bar's dB mapping and its instant attack and 120 ms release.

#include "support.h"

#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <limits>

#include "core/level.h"
#include "engine/common.h"

namespace {

constexpr float kPi = 3.14159265358979f;
constexpr float kTickMs = 40.0f;

float dbToLinear(float db) { return std::pow(10.0f, db / 20.0f); }

// A full-scale sine of the given peak, four whole cycles in a 64-sample block, so one block holds
// whole cycles and its RMS is exactly peak / sqrt(2).
void sineBlock(float peak, float* out) {
  for (int i = 0; i < cv::kBlock; ++i) out[i] = peak * std::sin(2.0f * kPi * 4.0f * static_cast<float>(i) / cv::kBlock);
}

level::InputEnergy energyOfRms(float rms) {
  level::InputEnergy e;
  e.samples = 1920;  // one tick of audio at 48 kHz
  e.sumSquares = rms * rms * static_cast<float>(e.samples);
  return e;
}

level::InputEnergy energyOfDb(float db) { return energyOfRms(dbToLinear(db)); }

void checkEnergy() {
  float block[cv::kBlock];
  sineBlock(0.5f, block);
  level::InputEnergy e;
  for (int b = 0; b < 30; ++b) {
    e.sumSquares += level::sumSquares(block, cv::kBlock);
    e.samples += cv::kBlock;
  }
  check(e.samples == 1920, "level: thirty blocks make one tick of samples");
  check(std::fabs(level::rms(e) - 0.5f / std::sqrt(2.0f)) < 1e-4f, "level: the RMS of a sine is its peak over root two");

  const float ramp[4] = {1.0f, -1.0f, 0.5f, 0.0f};
  check(std::fabs(level::sumSquares(ramp, 4) - 2.25f) < 1e-6f && level::sumSquares(ramp, 0) == 0.0f,
        "level: sumSquares adds the squares and an empty block adds nothing");

  // The four running sums give the total to float rounding for any length, the odd tail included.
  float odd[67];
  for (int i = 0; i < 67; ++i) odd[i] = 0.3f * std::sin(0.7f * static_cast<float>(i));
  bool lanes = true;
  for (const int n : {1, 3, 4, 5, 63, 64, 67}) {
    double want = 0.0;
    for (int i = 0; i < n; ++i) want += static_cast<double>(odd[i]) * static_cast<double>(odd[i]);
    lanes = lanes && std::fabs(static_cast<double>(level::sumSquares(odd, n)) - want) <= 1e-6 * (want + 1e-9);
  }
  check(lanes, "level: sumSquares matches the double sum for every length around the four lanes");
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

}  // namespace

void checkLevel() {
  checkEnergy();
  checkDbfs();
  checkMapping();
  checkMeterAttackAndRelease();
  checkMeterTickLength();
}
