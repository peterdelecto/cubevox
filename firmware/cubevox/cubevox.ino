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
#include "board/platform.h"
#include "board/slots.h"
#include "board/usb_link.h"
#include "core/registry.h"

constexpr uint32_t kHeartbeatMs = 500;
constexpr uint32_t kStatsMs = 5000;

static ChainParams gParams;
static bool gOledFound = false;
static bool gAudioRunning = false;
static uint8_t gBenchOff = 0;  // bit s: slot s forced off by a bench command

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

// Rebuilds the full parameter set from the panel and the menu, then hands it to the audio interrupt.
static void publishParams() {
  controlsApply(gParams);
  for (int s = 0; s < kSlotCount; ++s) {
    if (gBenchOff & (1u << s)) applyToggle(slotCard(s), false, gParams);
  }
  gParams.reverb.engine = menuSettings().reverbEngine;
  gParams.inputGainDb = menuInputGainDb();
  gParams.outputGainDb = menuOutputGainDb();
  audioPublish(gParams);
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
  Serial.println(gBenchOff, HEX);
  Serial.print("[audio] per slot");
  for (int i = 0; i <= kSlotCount; ++i) {
    Serial.print(' ');
    Serial.print(s.slotAvg[i]);
  }
  Serial.println(" (last is EQ)");
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
      gBenchOff ^= static_cast<uint8_t>(1u << (c - '1'));
      publishParams();
      printStatsNow();
      continue;
    }
    switch (c) {
      case 'r': printBootReport(); break;
      case 'p': printStatsNow(); break;
      case '0':
        gBenchOff = 0;
        publishParams();
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
      case 'd':
        Serial.println("[bench] entering DFU");
        Serial.flush();
        platformEnterDfu();
        break;
      default: break;
    }
  }
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
  pollBenchCommands();

  controlsPoll(nowMs);
  mutePoll(nowMs);

  const bool panelChanged = controlsTakeChanged();
  const bool menuChanged = menuTakeSettingsChanged();
  if (panelChanged || menuChanged) publishParams();

  menuPoll(nowMs, muteActive());
  heartbeat(nowMs);
  printStats(nowMs);
}
