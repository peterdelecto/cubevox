// cubevox firmware, STM32H743VIT6. Panel + layout + settings -> ChainSnapshot -> audio interrupt.

#include <Arduino.h>

#include "board/audio.h"
#include "board/bench.h"
#include "board/sysclock.h"
#include "board/controls.h"
#include "board/display_sh1106.h"
#include "board/fault.h"
#include "board/display_stub.h"
#include "board/input.h"
#include "board/input_jack.h"
#include "board/mute.h"
#include "board/pins.h"
#include "board/platform.h"
#include "board/slots.h"
#include "board/usb_link.h"
#include "core/compose.h"
#include "core/jack_switch.h"
#include "core/layout.h"
#include "core/registry.h"
#include "core/settings.h"
#include "ui/menu.h"

constexpr uint32_t kHeartbeatMs = 500;
constexpr uint32_t kStatsMs = 5000;

static Layout gLayout = defaultLayout();
static Settings gSettings;
static Menu gMenu;
static JackSwitch gJack;
static Display* gDisplay = nullptr;
static Canvas gCanvas;
static bool gOledFound = false;
static bool gAudioRunning = false;
static bool gSnapshotStale = false;  // a change is waiting for the panel to be read

// Boot facts for the first-article checklist, printed whenever a terminal opens the port.
static void printBootReport() {
  Serial.println("[boot] cubevox firmware");
  faultReport(Serial);
  clockReport(Serial);
  pins::report(Serial);
  Serial.print("[boot] icache ");
  Serial.print((SCB->CCR & SCB_CCR_IC_Msk) ? "on" : "OFF");
  Serial.print(", dcache ");
  Serial.print((SCB->CCR & SCB_CCR_DC_Msk) ? "on" : "OFF");
  Serial.print(", fpu ");
  Serial.println((SCB->CPACR & (0xFu << 20)) == (0xFu << 20) ? "on" : "OFF");
  Serial.println(gOledFound ? "[boot] OLED at 0x3C" : "[WARN] OLED not found, serial display");
  if (gAudioRunning) {
    Serial.println("[boot] audio running, SAI1 48 kHz");
  } else {
    Serial.print("[ERROR] audio: ");
    Serial.println(audioError());
  }
}

// Rebuilds the snapshot from the layout, panel and settings, then hands it to the audio interrupt.
// Before the panel is read the knobs would be zero, so the request waits in gSnapshotStale.
static void publishSnapshot() {
  if (!controlsSeeded()) {
    gSnapshotStale = true;
    return;
  }
  gSnapshotStale = false;
  audioPublish(composeSnapshot(gLayout, controlsReading(), gSettings, benchOverrides()));
}

static void printStats(uint32_t nowMs) {
  static uint32_t lastMs = 0;
  if (nowMs - lastMs < kStatsMs) return;
  lastMs = nowMs;
  benchPrintStats();
}

// Applies what the menu asked for. Returns true when a setting changed.
static bool applyAction(UiAction action) {
  switch (action) {
    case UiAction::ToggleOutput:
      gSettings.output = gSettings.output == OutputLevel::Line ? OutputLevel::Instrument : OutputLevel::Line;
      return true;
    case UiAction::ToggleInputJack:
      gSettings.inputJack = gSettings.inputJack == InputJack::Mic ? InputJack::Pedal : InputJack::Mic;
      return true;
    case UiAction::EnterDfu:
      muteForReset();
      platformEnterDfu();  // does not return
      break;
    case UiAction::None: break;
  }
  return false;
}

// The relay switches only while the output is fully muted; the mute is held until it settles.
static void runJackSwitch(uint32_t nowMs) {
  gJack.update(nowMs, gSettings.inputJack, muteFullyMuted());
  muteHold(gJack.holdMute() || !controlsSeeded());  // unseeded knobs read 0, so stay muted
  inputJackApply(gJack.relay());
}

static void heartbeat(uint32_t nowMs) {
  static uint32_t lastMs = 0;
  if (nowMs - lastMs < kHeartbeatMs) return;
  lastMs = nowMs;
  digitalToggle(pins::kUserLed);
}

void setup() {
  muteInit();  // MUTE_N and XSMT low before anything slow
  inputJackInit();  // PAD_EN low = MIC
  faultInit();
  pinMode(pins::kUserLed, OUTPUT);
  usbLinkInit();  // USB attaches from loop() once VBUS is seen

  controlsInit();
  gOledFound = sh1106Present();
  inputInit();
  gDisplay = gOledFound ? &sh1106Display() : &stubDisplay();

  gAudioRunning = audioInit();
  if (gAudioRunning) muteClocksRunning(millis());
  faultWatchdogStart();  // every init is behind us; from here loop() must keep running
}

void loop() {
  const uint32_t nowMs = millis();
  faultWatchdogKick();
  faultAudioWatch(nowMs, gAudioRunning);

  usbLinkPoll(nowMs);
  if (usbLinkTerminalOpened()) printBootReport();
  if (benchPoll(printBootReport)) publishSnapshot();

  controlsPoll(nowMs, gLayout);
  mutePoll(nowMs);

  const bool settingsChanged = applyAction(gMenu.update(inputPoll(nowMs), nowMs));
  runJackSwitch(nowMs);
  // The first publish carries the real panel; mutePoll sees the hold lift on the next pass.
  if (controlsTakeChanged() || settingsChanged || gSnapshotStale) publishSnapshot();

  const UiContext ctx{gLayout, controlsReading(), gSettings, muteActive()};
  gMenu.draw(ctx, gCanvas);
  gDisplay->flushStep(gCanvas);
  heartbeat(nowMs);
  printStats(nowMs);
}
