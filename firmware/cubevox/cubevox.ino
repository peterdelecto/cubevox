// cubevox firmware, STM32H743VIT6. Panel -> ChainParams -> audio interrupt.

#include <Arduino.h>

#include "board/audio.h"
#include "board/sysclock.h"
#include "board/controls.h"
#include "board/display_sh1106.h"
#include "board/fault.h"
#include "ui/menu.h"
#include "board/mute.h"
#include "board/pins.h"
#include "board/slots.h"
#include "board/usb_link.h"

constexpr uint32_t kHeartbeatMs = 500;
constexpr uint32_t kStatsMs = 5000;

static ChainParams gParams;
static bool gOledFound = false;
static bool gAudioRunning = false;

// Boot facts for the first-article checklist, printed whenever a terminal opens the port.
static void printBootReport() {
  Serial.println("[boot] cubevox firmware");
  faultReport(Serial);
  clockReport(Serial);
  Serial.println(gOledFound ? "[boot] OLED at 0x3C" : "[WARN] OLED not found, serial display");
  if (gAudioRunning) {
    Serial.println("[boot] audio running, SAI1 48 kHz");
  } else {
    Serial.print("[ERROR] audio: ");
    Serial.println(audioError());
  }
}

// Rebuilds the full parameter set from the panel and the menu, then hands it to the audio interrupt.
static void publishParams() {
  controlsApply(gParams);
  gParams.reverb.engine = menuSettings().reverbEngine;
  gParams.inputGainDb = menuInputGainDb();
  gParams.outputGainDb = menuOutputGainDb();
  audioPublish(gParams);
}

static void printStats(uint32_t nowMs) {
  static uint32_t lastMs = 0;
  if (nowMs - lastMs < kStatsMs) return;
  lastMs = nowMs;
  const AudioStats s = audioStats();
  Serial.print("[audio] blocks ");
  Serial.print(s.blocks);
  Serial.print(" overruns ");
  Serial.print(s.overruns);
  Serial.print(" worst ");
  Serial.print(s.maxCycles);
  Serial.print(" of ");
  Serial.print(s.budgetCycles);
  Serial.println(" cycles");
}

static void heartbeat(uint32_t nowMs) {
  static uint32_t lastMs = 0;
  if (nowMs - lastMs < kHeartbeatMs) return;
  lastMs = nowMs;
  digitalToggle(pins::kUserLed);
}

void setup() {
  muteInit();  // MUTE_N and XSMT low before anything slow
  faultInit();
  pinMode(pins::kUserLed, OUTPUT);
  usbLinkInit();  // USB attaches from loop() once VBUS is seen

  controlsInit();
  gOledFound = sh1106Present();
  menuInit(gOledFound ? sh1106Display() : stubDisplay());

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

  controlsPoll(nowMs);
  mutePoll(nowMs);

  const bool panelChanged = controlsTakeChanged();
  const bool menuChanged = menuTakeSettingsChanged();
  if (panelChanged || menuChanged) publishParams();

  menuPoll(nowMs, muteActive());
  heartbeat(nowMs);
  printStats(nowMs);
}
