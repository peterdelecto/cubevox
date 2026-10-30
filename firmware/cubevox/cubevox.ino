// cubevox firmware, STM32H743VIT6. Panel + layout + settings -> ChainSnapshot -> audio interrupt.

#include <Arduino.h>

#include "board/audio.h"
#include "board/bench.h"
#include "board/bench_panel.h"
#include "board/boot_health.h"
#include "board/boot_report.h"
#include "board/build_kind.h"
#include "board/controls.h"
#include "board/device_serial.h"
#include "board/dfu.h"
#include "board/diag_service.h"
#include "board/display_sh1106.h"
#include "board/fault.h"
#include "board/input.h"
#include "board/input_jack.h"
#include "board/mute.h"
#include "board/pins.h"
#include "board/planned_reset.h"
#include "board/power_sense.h"
#include "board/record_service.h"
#include "board/serial_link.h"
#include "board/usb_link.h"
#include "core/compose.h"
#include "core/gate_floor.h"
#include "core/jack_switch.h"
#include "core/layout.h"
#include "core/loop_policy.h"
#include "core/protocol.h"
#include "core/settings.h"
#include "core/timing.h"
#include "core/version.h"
#include "ui/actions.h"
#include "ui/face.h"
#include "ui/gui.h"
#include "ui/menu.h"
#include "ui/splash.h"

namespace {

constexpr uint32_t kHeartbeatMs = 500;
constexpr uint32_t kStatsMs = 5000;

Layout gLayout = defaultLayout();
Settings gSettings;
Menu gMenu;
Face gFace;
Splash gSplash;
JackSwitch gJack{Settings().inputJack};
Display* gDisplay = nullptr;
Canvas gCanvas;
bool gAudioRunning = false;
PublishGate gPublish;
FallingEdge gMenuClosed;
Every gHeartbeat{kHeartbeatMs};
Every gStats{kStatsMs};
Every gLoadRefresh{kUiTickMs};  // the DEBUG figure moves once per UI tick
int gLoadPercent = 0;

// FPSCR and FPDSCR bit 24: flush subnormal results and operands to zero. The input filters decay
// toward silence, and a subnormal state would cost the FPU extra cycles on every sample.
constexpr uint32_t kFlushToZero = 1u << 24;

// FPSCR covers the main loop. The FPU loads FPDSCR into FPSCR on exception entry, so FPDSCR covers
// the audio interrupt that runs the filters.
void flushDenormalsToZero() {
  __set_FPSCR(__get_FPSCR() | kFlushToZero);
  FPU->FPDSCR |= kFlushToZero;
}

void reportBoot() { bootReport(sh1106Present()); }

// Rebuilds the snapshot from the layout, panel and settings, then hands it to the audio interrupt.
void publishSnapshot() { audioPublish(composeSnapshot(gLayout, controlsReading(), gSettings, benchOverrides())); }

// The encoder's turns and clicks for the menu. While a planned reset waits on the flash the menu
// takes none, so a turn cannot leave the confirm screen behind an update that is already starting.
UiInput menuInput(uint32_t nowMs) {
  const UiInput input = inputPoll(nowMs);
  return plannedResetPending() ? UiInput{} : input;
}

// Applies what the menu asked for. Returns true when a setting or the layout changed. A new layout
// takes the same path as one from the app: the snapshot goes out stale, the audio fades and resets
// the engines whose card changed, and the save writes the layout record.
bool applyAction(UiAction action, uint32_t nowMs) {
  switch (ui::applyAction(action, gSettings, gLayout)) {
    case ui::ApplyResult::SettingsChanged: return true;
    case ui::ApplyResult::LayoutChanged:
      gPublish.markStale();
      return true;
    case ui::ApplyResult::EnterDfu: enterDfu(nowMs); break;
    case ui::ApplyResult::None: break;
  }
  return false;
}

// A settings change saves kSaveDebounceMs after the last click, or at once when the menu closes,
// so the last click is in flash by the time Adam plays.
void saveSettings(bool changed, uint32_t nowMs) {
  if (changed) recordRequestSave(gLayout, gSettings, nowMs);
  if (gMenuClosed.update(gMenu.open())) recordSkipDebounce();
}

const char* layoutName() { return layoutDisplayName(recordName()); }

void replyStatus(uint32_t id) {
  const AudioLoad load = audioTakeLoad();
  protocol::Status status;
  status.cpuMeanPercent = load.meanPercent;
  status.cpuWorstPercent = load.worstPercent;
  status.late = load.late;
  status.dryBlocks = load.dryBlocks;
  status.muted = muteActive();
  status.seq = recordSequence();
  status.recordErrors = recordErrors();
  status.serialDropped = serialDropped();
  status.strayBytes = serialStrayBytes();
  status.bootFails = bootHealthFails();
  status.updateReason = bootHealthLastReason();
  const AudioHealth health = audioHealth();
  status.overruns = health.overruns;
  status.saiErrors = health.errors.count;
  status.restarts = health.restarts;
  status.nanBlocks = health.nanBlocks;
  status.audioLatched = health.latched;
  status.stalled = faultAudioStalled();
  status.powerTrips = powerSenseTrips();
  status.floorDbfs = peakDbfs(audioTakeGatePeak());
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
  protocol::applySettings(req, gSettings);  // only the fields the request carried
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
      protocol::replyHello(out, req.id, kFirmwareVersion, layoutName(), gLayout, kBenchBuild, deviceSerial(), &gSettings);
      break;
    case protocol::Command::GetLayout: protocol::replyLayout(out, req.id, layoutName(), gLayout, gSettings); break;
    case protocol::Command::SetLayout: replySetLayout(req, nowMs); break;
    case protocol::Command::Status: replyStatus(req.id); break;
    case protocol::Command::EnterDfu:
      protocol::replyOk(out, req.id);
      enterDfu(nowMs);
      break;
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

// The face owns the canvas except while the boot splash or the bench OLED pattern is up; it
// dissolves in from what they left once either ends. It ticks at 25 frames a second and only
// after the panel has taken the last frame. Without a panel the pages stay dirty, so the face waits.
void drawFace(const UiContext& ctx, uint32_t nowMs) {
  switch (gSplash.draw(nowMs, kFirmwareVersion, gCanvas)) {
    case SplashStep::Up: return;
    case SplashStep::Ended: gFace.invalidate(); break;
    case SplashStep::Off: break;
  }
  switch (benchPanelPattern(gCanvas)) {
    case BenchPattern::Up: return;
    case BenchPattern::Ended: gFace.invalidate(); break;
    case BenchPattern::Off: break;
  }
  if (gFace.due(nowMs, gCanvas)) {
    gFace.tick(nowMs, ctx, audioTakeInputEnergy(), audioTakeOutputEnergy(), gMenu, gCanvas);
  }
}

}  // namespace

// The bootloader starts the watchdog at 16 s (CV_WATCHDOG_BOOT_MS) right before the jump, and
// faultWatchdogStart() sets the box's own 2 s at the end. The refresh after recordBoot splits the
// time between into two stretches, each well inside 16 s:
//   1. Up to the end of the ECC recovery, at most 10.3 s: the clock setup's waits (0.2 s), the
//      power sense ADC (tens of ms), two erases at the driver's 5 s bound and eight program words
//      at 5 ms each.
//   2. From there to faultWatchdogStart(), well under 1 s: the panel's start (five tries 20 ms
//      apart, each a bus recovery of about 0.3 ms and one polled wait that gives up within about
//      15 ms, so 0.2 s at the most) and the HAL's bounded ADC, PLL3, SAI and DMA waits (tens of ms).
void setup() {
  flushDenormalsToZero();  // before the audio interrupt can run
  muteInit();  // MUTE_N and XSMT low before anything slow
  powerSenseInit();  // needs the mute pins; a failure prints in the boot report
  inputJackInit();  // PAD_EN low = MIC
  faultInit();  // before recordBoot: the ECC recovery reads the fault record this loads
  recordBoot(gLayout, gSettings, millis());  // waits on the ECC recovery's erases, if any
  faultWatchdogKick();  // ends stretch 1 above
  gJack = JackSwitch(gSettings.inputJack);  // output is still muted, so no dip
  inputJackApply(gSettings.inputJack);
  pinMode(pins::kUserLed, OUTPUT);
  usbLinkInit();  // USB attaches from loop() once VBUS is seen

  controlsInit();
  gDisplay = &sh1106Display();  // starts the panel; one that does not answer is retried once a second
  inputInit();

  gAudioRunning = audioInit();
  if (gAudioRunning) muteClocksRunning();
  faultWatchdogStart();  // every init is behind us; from here loop() must keep running
  bootHealthInit();  // after the watchdog: a start only counts as healthy with both running
  diagInit();  // after recordBoot (ECC recovery) and bootHealthInit (failed starts, bootloader reason)
  gSplash.start(millis());
}

void loop() {
  const uint32_t nowMs = millis();
  faultWatchdogKick();
  faultAudioWatch(nowMs, gAudioRunning);
  if (audioPoll(nowMs) != AudioEvent::None) diagNoteAudioFault();
  bootHealthPoll(nowMs);

  usbLinkPoll(nowMs);
  if (usbLinkTerminalOpened()) {
    serialLinkSessionStart();
    reportBoot();
  }
  serialLinkPoll(nowMs, onRequest);

  controlsPoll(nowMs, gLayout);
  benchPanelPoll(nowMs);
  powerSensePoll(nowMs);  // before mutePoll, so a trip it clears starts the release this pass
  mutePoll(nowMs, gSettings.topSwitch);

  const bool settingsChanged = applyAction(gMenu.update(menuInput(nowMs), nowMs), nowMs);
  saveSettings(settingsChanged, nowMs);
  recordPoll(nowMs);
  diagPoll();
  plannedResetPoll(nowMs);  // after both stores took their step this pass
  runJackSwitch(nowMs);
  // The first publish carries the real panel; mutePoll sees the hold lift on the next pass.
  if (gPublish.take(controlsSeeded(), controlsTakeChanged() || settingsChanged)) publishSnapshot();

  const bool switchOn = topSwitchEngaged(gSettings.topSwitch, muteActive(), muteBypassOn());
  if (gLoadRefresh.due(nowMs)) gLoadPercent = gSettings.debug ? audioWorstLoadPercent() : 0;
  const UiContext ctx{gLayout, controlsReading(), gSettings, switchOn, gLoadPercent};
  LastMoved touch;
  if (controlsTakeTouch(touch)) gFace.knobMoved(gLayout, ctx.panel, touch, nowMs);
  ToggleFlip flip;
  if (controlsTakeToggle(flip)) gFace.toggleFlipped(gLayout, flip, nowMs);
  drawFace(ctx, nowMs);
  gDisplay->flushStep(gCanvas);
  if (gHeartbeat.due(nowMs)) digitalToggle(pins::kUserLed);
  if (gStats.due(nowMs)) benchReportStats();
}
