// Checks the screen dimming on the host with a fake clock: the brightness table, the ten-minute idle
// rule, each kind of activity that wakes the screen, the voice threshold and the millisecond counter
// wrapping.

#include "support.h"

#include <cmath>
#include <cstdint>

#include "core/brightness.h"
#include "core/level.h"
#include "core/settings.h"
#include "ui/screen_dim.h"

namespace {

constexpr uint32_t kIdleMs = 600000;
constexpr uint8_t kDimmed = 8;
constexpr uint8_t kHigh = 207;

// The input energy of one UI tick (40 ms at 48 kHz, 1920 samples) whose RMS sits at `dbfs`.
level::InputEnergy tickAt(float dbfs) {
  constexpr uint32_t kSamples = 1920;
  const float amplitude = std::pow(10.0f, dbfs / 20.0f);
  level::InputEnergy energy;
  energy.samples = kSamples;
  energy.sumSquares = amplitude * amplitude * static_cast<float>(kSamples);
  return energy;
}

// A screen that started at `startMs` and has sat idle past the dimming time.
ScreenDim dimmedFrom(uint32_t startMs) {
  ScreenDim dim;
  dim.start(startMs);
  dim.contrast(startMs + kIdleMs, Brightness::High);
  return dim;
}

void checkTable() {
  check(kBrightnessLevels.size() == 4, "brightness: four levels");
  check(brightnessLevel(Brightness::Low).contrast == 48 && brightnessLevel(Brightness::Mid).contrast == 128 &&
            brightnessLevel(Brightness::High).contrast == kHigh && brightnessLevel(Brightness::Max).contrast == 255,
        "brightness: LOW, MID, HIGH and MAX are contrast 48, 128, 207 and 255");
  check(kDimmedContrast == kDimmed, "brightness: the dimmed contrast is 8");
  check(Settings().brightness == Brightness::High, "brightness: the default is HIGH, the contrast the panel starts at");
  check(nextBrightness(Brightness::Low) == Brightness::Mid && nextBrightness(Brightness::Mid) == Brightness::High &&
            nextBrightness(Brightness::High) == Brightness::Max && nextBrightness(Brightness::Max) == Brightness::Low,
        "brightness: a step goes LOW, MID, HIGH, MAX and wraps to LOW");
}

void checkIdleRule() {
  ScreenDim dim;
  dim.start(5000);
  check(dim.contrast(5000, Brightness::High) == kHigh, "dim: awake at start");
  check(dim.contrast(5000 + kIdleMs - 1, Brightness::High) == kHigh, "dim: still awake at 599,999 ms after the last activity");
  check(dim.contrast(5000 + kIdleMs, Brightness::High) == kDimmed, "dim: dimmed at 600,000 ms after the last activity");
  check(dim.contrast(5000 + kIdleMs + 1000, Brightness::Max) == kDimmed, "dim: stays dimmed at every brightness");
}

void checkLevels() {
  for (const BrightnessLevel& row : kBrightnessLevels) {
    ScreenDim dim;
    dim.start(0);
    const Brightness b = static_cast<Brightness>(&row - kBrightnessLevels.data());
    check(dim.contrast(1000, b) == row.contrast, "dim: each brightness level gives its contrast while awake");
    check(dim.contrast(kIdleMs, b) == kDimmed, "dim: each brightness level gives 8 once dimmed");
  }
}

void checkWake() {
  // A knob move, a slot toggle flip, or a MENU turn or press: all call noteActivity.
  ScreenDim dim = dimmedFrom(0);
  check(dim.contrast(kIdleMs + 50, Brightness::Mid) == kDimmed, "dim: dimmed before the activity");
  dim.noteActivity(kIdleMs + 50);
  check(dim.contrast(kIdleMs + 50, Brightness::Mid) == 128, "dim: activity wakes the screen in the same pass, at the saved brightness");
  check(dim.contrast(kIdleMs + 50 + kIdleMs - 1, Brightness::Mid) == 128 &&
            dim.contrast(kIdleMs + 50 + kIdleMs, Brightness::Mid) == kDimmed,
        "dim: the idle time counts from the activity");

  // The top switch: the first call records the position, a change is activity, the same level is not.
  ScreenDim sw = dimmedFrom(0);
  sw.noteTopSwitch(false, kIdleMs + 10);
  check(sw.contrast(kIdleMs + 10, Brightness::High) == kDimmed, "dim: the first top switch reading is not activity");
  sw.noteTopSwitch(false, kIdleMs + 20);
  check(sw.contrast(kIdleMs + 20, Brightness::High) == kDimmed, "dim: a top switch that stays put is not activity");
  sw.noteTopSwitch(true, kIdleMs + 30);
  check(sw.contrast(kIdleMs + 30, Brightness::High) == kHigh, "dim: the top switch changing position wakes the screen");
  ScreenDim back = dimmedFrom(0);
  back.noteTopSwitch(true, 100);
  back.noteTopSwitch(false, kIdleMs + 40);
  check(back.contrast(kIdleMs + 40, Brightness::High) == kHigh, "dim: the top switch changing back wakes the screen too");

  // A voice: above the input meter's floor of -50 dBFS wakes it, below does not.
  check(level::kFloorDbfs == -50.0f, "dim: the voice threshold is the input meter's floor, -50 dBFS");
  ScreenDim quiet = dimmedFrom(0);
  quiet.noteVoice(tickAt(-51.0f), kIdleMs + 10);
  check(quiet.contrast(kIdleMs + 10, Brightness::High) == kDimmed, "dim: a voice at -51 dBFS does not wake the screen");
  quiet.noteVoice(tickAt(-49.0f), kIdleMs + 20);
  check(quiet.contrast(kIdleMs + 20, Brightness::High) == kHigh, "dim: a voice at -49 dBFS wakes the screen");
  quiet.noteVoice(level::InputEnergy(), kIdleMs + 30);
  check(quiet.contrast(kIdleMs + 30, Brightness::High) == kHigh, "dim: an empty tick leaves it awake");

  ScreenDim awake;
  awake.start(0);
  awake.noteVoice(tickAt(-51.0f), kIdleMs - 1);
  check(awake.contrast(kIdleMs, Brightness::High) == kDimmed, "dim: a voice under the floor does not hold the screen awake");
  ScreenDim talking;
  talking.start(0);
  talking.noteVoice(tickAt(-49.0f), kIdleMs - 1);
  check(talking.contrast(kIdleMs, Brightness::High) == kHigh, "dim: a voice over the floor restarts the idle time");
}

void checkWrap() {
  const uint32_t startMs = 0xFFFFFFFFu - 1000u;  // 1,001 ms before the counter wraps
  ScreenDim dim;
  dim.start(startMs);
  check(dim.contrast(startMs + kIdleMs - 1, Brightness::High) == kHigh, "dim: awake at 599,999 ms across the counter wrap");
  check(dim.contrast(startMs + kIdleMs, Brightness::High) == kDimmed, "dim: dimmed at 600,000 ms across the counter wrap");
  dim.noteActivity(startMs + kIdleMs + 10);
  check(dim.contrast(startMs + kIdleMs + 10, Brightness::High) == kHigh, "dim: wakes across the counter wrap");

  ScreenDim lapped = dimmedFrom(7);
  check(lapped.contrast(7, Brightness::High) == kDimmed, "dim: a screen dimmed for a whole lap of the counter stays dimmed");
}

}  // namespace

void checkScreenDim() {
  checkTable();
  checkIdleRule();
  checkLevels();
  checkWake();
  checkWrap();
}
