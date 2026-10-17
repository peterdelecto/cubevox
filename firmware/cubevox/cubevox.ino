// cubevox firmware, STM32H743VIT6. Panel + layout + settings -> ChainSnapshot -> audio interrupt.

#include <Arduino.h>

#include "board/audio.h"
#include "board/bench.h"
#include "board/bench_panel.h"
#include "board/boot_report.h"
#include "board/build_kind.h"
#include "board/controls.h"
#include "board/dfu.h"
#include "board/display_sh1106.h"
#include "board/display_stub.h"
#include "board/fault.h"
#include "board/input.h"
#include "board/input_jack.h"
#include "board/mute.h"
#include "board/pins.h"
#include "board/record_service.h"
#include "board/serial_link.h"
#include "board/usb_link.h"
#include "core/compose.h"
#include "core/jack_switch.h"
#include "core/layout.h"
#include "core/loop_policy.h"
#include "core/protocol.h"
#include "core/settings.h"
#include "core/timing.h"
#include "core/version.h"
#include "ui/actions.h"
#include "ui/menu.h"

namespace {

constexpr uint32_t kHeartbeatMs = 500;
constexpr uint32_t kStatsMs = 5000;

Layout gLayout = defaultLayout();
Settings gSettings;
Menu gMenu;
JackSwitch gJack{Settings().inputJack};
Display* gDisplay = nullptr;
Canvas gCanvas;
bool gOledFound = false;
bool gAudioRunning = false;
PublishGate gPublish;
Every gHeartbeat{kHeartbeatMs};
Every gStats{kStatsMs};

void reportBoot() { bootReport(gOledFound); }

// Rebuilds the snapshot from the layout, panel and settings, then hands it to the audio interrupt.
void publishSnapshot() { audioPublish(composeSnapshot(gLayout, controlsReading(), gSettings, benchOverrides())); }

// Applies what the menu asked for. Returns true when a setting changed.
bool applyAction(UiAction action) {
  switch (ui::applyAction(action, gSettings)) {
    case ui::ApplyResult::SettingsChanged: return true;
    case ui::ApplyResult::EnterDfu: enterDfu();
    case ui::ApplyResult::None: break;
  }
  return false;
}

const char* layoutName() { return layoutDisplayName(recordName()); }

void replyStatus(uint32_t id) {
  const AudioLoad load = audioTakeLoad();
  protocol::Status status;
  status.cpuMeanPercent = load.meanPercent;
  status.cpuWorstPercent = load.worstPercent;
  status.late = load.late;
  status.muted = muteActive();
  status.seq = recordSequence();
  status.recordErrors = recordErrors();
  status.serialDropped = serialDropped();
  status.strayBytes = serialStrayBytes();
  protocol::replyStatus(serialWriter(), id, status);
}

// Validates and adopts a layout from the app. The save runs after the usual debounce.
void replySetLayout(const protocol::Request& req, uint32_t nowMs) {
  Layout next = defaultLayout();
  char err[protocol::kErrorBytes];
  if (!protocol::resolveLayout(req, next, err, sizeof err)) {
    protocol::replyError(serialWriter(), req.id, err);
    return;
  }
  gLayout = next;
  recordSetName(req.name);
  recordRequestSave(gLayout, gSettings, nowMs);
  gPublish.markStale();  // goes out after controlsPoll() re-seeds the knobs for the new cards
  protocol::replyOk(serialWriter(), req.id);
}

// Replies after the op runs, so a failure reaches the host. Fault and spin never return, so
// they reply first and flush inside the drill before they act.
void replyBench(const protocol::Request& req) {
  protocol::Writer& out = serialWriter();
  if (req.op == BenchOp::Fault || req.op == BenchOp::Spin) protocol::replyOk(out, req.id);  // the drill flushes it
  const BenchResult result = benchRun(req.op, req.benchSlot, reportBoot);
  if (result.republish) gPublish.markStale();
  if (result.error) {
    protocol::replyError(out, req.id, result.error);
  } else {
    protocol::replyOk(out, req.id);
  }
}

void onRequest(const char* line, uint32_t nowMs) {
  protocol::Writer& out = serialWriter();
  protocol::Request req;
  char err[protocol::kErrorBytes];
  if (!protocol::parseRequest(line, req, err, sizeof err, kBenchBuild)) {
    protocol::replyError(out, req.id, err);
    return;
  }
  switch (req.cmd) {
    case protocol::Command::Hello:
      protocol::replyHello(out, req.id, kFirmwareVersion, layoutName(), gLayout, kBenchBuild);
      break;
    case protocol::Command::GetLayout: protocol::replyLayout(out, req.id, layoutName(), gLayout); break;
    case protocol::Command::SetLayout: replySetLayout(req, nowMs); break;
    case protocol::Command::Status: replyStatus(req.id); break;
    case protocol::Command::EnterDfu:
      protocol::replyOk(out, req.id);
      enterDfu();
    case protocol::Command::Bench: replyBench(req); break;
  }
}

// The relay switches only while the output is fully muted; the mute is held until it settles.
void runJackSwitch(uint32_t nowMs) {
  gJack.update(nowMs, benchJack(gSettings.inputJack), muteFullyMuted());
  // Unseeded knobs read 0, so stay muted; the panel test can hold the mute too.
  muteHold(gJack.holdMute() || !controlsSeeded() || benchMuteHeld());
  inputJackApply(gJack.relay());
}

// The menu owns the canvas except while the bench OLED pattern is up; it repaints once that ends.
void drawFace(const UiContext& ctx) {
  switch (benchPanelPattern(gCanvas)) {
    case BenchPattern::Up: return;
    case BenchPattern::Ended: gMenu.invalidate(); break;
    case BenchPattern::Off: break;
  }
  gMenu.draw(ctx, gCanvas);
}

}  // namespace

void setup() {
  muteInit();  // MUTE_N and XSMT low before anything slow
  inputJackInit();  // PAD_EN low = MIC
  faultInit();  // before recordBoot: the ECC recovery reads the fault record this loads
  recordBoot(gLayout, gSettings, millis());  // blocking ECC erase is safe: the watchdog starts at the end of setup()
  gJack = JackSwitch(gSettings.inputJack);  // output is still muted, so no dip
  inputJackApply(gSettings.inputJack);
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
  if (usbLinkTerminalOpened()) {
    serialLinkSessionStart();
    reportBoot();
  }
  serialLinkPoll(nowMs, onRequest);

  controlsPoll(nowMs, gLayout);
  benchPanelPoll(nowMs);
  mutePoll(nowMs);

  const bool settingsChanged = applyAction(gMenu.update(inputPoll(nowMs), nowMs));
  if (settingsChanged) recordRequestSave(gLayout, gSettings, nowMs);
  recordPoll(nowMs);
  runJackSwitch(nowMs);
  // The first publish carries the real panel; mutePoll sees the hold lift on the next pass.
  if (gPublish.take(controlsSeeded(), controlsTakeChanged() || settingsChanged)) publishSnapshot();

  const UiContext ctx{gLayout, controlsReading(), gSettings, muteActive()};
  drawFace(ctx);
  gDisplay->flushStep(gCanvas);
  if (gHeartbeat.due(nowMs)) digitalToggle(pins::kUserLed);
  if (gStats.due(nowMs)) benchReportStats();
}
