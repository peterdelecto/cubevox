// cubevox firmware, STM32H743VIT6. Panel -> ChainParams -> audio interrupt.

#include <Arduino.h>

#include "audio.h"
#include "controls.h"
#include "display_sh1106.h"
#include "menu.h"
#include "mute.h"
#include "pins.h"
#include "slots.h"

constexpr uint32_t kHeartbeatMs = 500;
constexpr uint32_t kStatsMs = 5000;

static ChainParams gParams;

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
  pinMode(pins::kUserLed, OUTPUT);
  Serial.begin(115200);

  controlsInit();
  if (sh1106Present()) {
    menuInit(sh1106Display());
  } else {
    Serial.println("[WARN] OLED not found, serial display");
    menuInit(stubDisplay());
  }

  if (audioInit()) {
    muteClocksRunning(millis());
  } else {
    Serial.print("[ERROR] audio: ");
    Serial.println(audioError());
  }
}

void loop() {
  const uint32_t nowMs = millis();

  controlsPoll(nowMs);
  mutePoll(nowMs);

  const bool panelChanged = controlsTakeChanged();
  const bool menuChanged = menuTakeSettingsChanged();
  if (panelChanged || menuChanged) publishParams();

  menuPoll(nowMs, muteActive());
  heartbeat(nowMs);
  printStats(nowMs);
}
