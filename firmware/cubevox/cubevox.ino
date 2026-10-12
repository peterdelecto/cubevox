// cubevox firmware, STM32H743VIT6. Panel + layout + settings -> ChainSnapshot -> audio interrupt.

#include <Arduino.h>

#include "board/audio.h"
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
static BenchOverrides gBench;
static bool gOledFound = false;
static bool gAudioRunning = false;

// Boot facts for the first-article checklist, printed whenever a terminal opens the port.
static void printBootReport() {
  Serial.println("[boot] cubevox firmware");
  faultReport(Serial);
  clockReport(Serial);
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
static void publishSnapshot() {
  audioPublish(composeSnapshot(gLayout, controlsReading(), gSettings, gBench));
}

static void printStatsNow() {
  const AudioStats s = audioStats();
  Serial.print("[audio] blocks ");
  Serial.print(s.blocks);
  Serial.print(" late ");
  Serial.print(s.late);
  Serial.print(" overruns ");
  Serial.print(s.overruns);
  Serial.print(" avg ");
  Serial.print(s.avgCycles);
  Serial.print(" worst ");
  Serial.print(s.maxCycles);
  Serial.print(" of ");
  Serial.print(s.budgetCycles);
  Serial.print(" cycles, slots off 0x");
  Serial.println(gBench.slotsOff, HEX);
  Serial.print("[audio] per slot");
  for (int i = 0; i <= kSlotCount; ++i) {
    Serial.print(' ');
    Serial.print(s.slotAvg[i]);
  }
  Serial.println(" (last is EQ)");
  Serial.print("[audio] slot worst");
  for (int i = 0; i <= kSlotCount; ++i) {
    Serial.print(' ');
    Serial.print(s.slotWorst[i]);
  }
  Serial.println();
}

static void printStats(uint32_t nowMs) {
  static uint32_t lastMs = 0;
  if (nowMs - lastMs < kStatsMs) return;
  lastMs = nowMs;
  printStatsNow();
}

// Single-letter bench commands from a terminal. Roadmap step 3 replaces these with the JSON
// protocol; until then they are how the first-article fault checks are driven.
static void pollBenchCommands() {
  while (Serial.available() > 0) {
    const int c = Serial.read();
    if (c >= '1' && c <= '8') {
      gBench.slotsOff ^= static_cast<uint8_t>(1u << (c - '1'));
      publishSnapshot();
      printStatsNow();
      continue;
    }
    switch (c) {
      case 'r': printBootReport(); break;
      case 'p': printStatsNow(); break;
      case '0':
        gBench.slotsOff = 0;
        publishSnapshot();
        printStatsNow();
        break;
      case 'f':
        Serial.println("[bench] forcing a bus fault");
        Serial.flush();
        (void)*reinterpret_cast<volatile uint32_t*>(0xDEADBEE0);
        break;
      case 'w':
        Serial.println("[bench] spinning; the watchdog resets in 2 s");
        Serial.flush();
        for (;;) {}
      case 'a':
        Serial.println("[bench] stopping audio DMA");
        audioStopForBench();
        break;
      case 'm':
        audioResetPeaks();
        Serial.println("[bench] audio peaks reset");
        break;
      case 'k': {
        gBench.knobsPinned = !gBench.knobsPinned;
        publishSnapshot();
        Serial.println(gBench.knobsPinned ? "[bench] knobs pinned at top" : "[bench] knobs live");
        break;
      }
      case 'c':
        gBench.reverbChasm = !gBench.reverbChasm;
        publishSnapshot();
        Serial.println(gBench.reverbChasm ? "[bench] reverb slot runs Chasm" : "[bench] reverb slot runs its card's engine");
        break;
      case 'v': {
        static bool clipOn = false;
        clipOn = audioBenchClip(!clipOn);
        Serial.println(clipOn ? "[bench] vocal clip on" : "[bench] vocal clip off (or not compiled in)");
        break;
      }
      case 'd':
        Serial.println("[bench] entering DFU");
        Serial.flush();
        muteForReset();
        platformEnterDfu();
        break;
      default: break;
    }
  }
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
  muteHold(gJack.holdMute());
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
  pollBenchCommands();

  controlsPoll(nowMs, gLayout);
  mutePoll(nowMs);

  const bool settingsChanged = applyAction(gMenu.update(inputPoll(nowMs), nowMs));
  runJackSwitch(nowMs);
  if (controlsTakeChanged() || settingsChanged) publishSnapshot();

  const UiContext ctx{gLayout, controlsReading(), gSettings, muteActive()};
  gMenu.draw(ctx, gCanvas);
  gDisplay->flushStep(gCanvas);
  heartbeat(nowMs);
  printStats(nowMs);
}
