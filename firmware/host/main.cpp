// cubevox-emulator: the box's firmware code on the Mac.
//   cubevox-emulator                          opens the window
//   cubevox-emulator --check                  headless check, no window and no audio device
//   cubevox-emulator --proof f.ppm [--no-mic] [--slot3 octave|formant|apraxia] [--slot6 distortion|growl]
//                                  [--slot7 gate|lofi|chatter] [--slot8 spring|chasm]
//                                             one frame of the window as a P6 PPM, no audio device,
//                                             with those cards in slots 3, 6, 7 and 8 (Octave, Distortion,
//                                             Gate and Spring when omitted; a card id also picks its card, `bloom`
//                                             Chatter and `formant_follow` Apraxia)

#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "audio_block.h"
#include "box.h"
#include "check_clip.h"
#include "clock.h"
#include "core/brightness.h"
#include "core/costs.h"
#include "core/layout.h"
#include "core/level.h"
#include "core/registry.h"
#include "file_menu.h"
#include "source.h"
#include "theme.h"
#include "ui/gui.h"
#include "ui/screen_dim.h"
#include "window.h"

namespace {

using namespace host;

constexpr uint32_t kStartMs = 1000;  // the fake clock's power-on time
constexpr uint32_t kTickMs = kUiTickMs;
constexpr int kBlocksPerTick = static_cast<int>(kTickMs) * cv::kSampleRate / 1000 / cv::kBlock;
static_assert(static_cast<uint32_t>(kBlocksPerTick * cv::kBlock) * 1000 == kTickMs * cv::kSampleRate,
              "a UI tick must hold a whole number of blocks");

constexpr uint32_t kCheckBeforeMs = 3000;  // the default layout
constexpr uint32_t kCheckAfterMs = 2000;   // after MOB and Chasm
constexpr uint32_t kCheckOpenMs = 2000;    // the second clip, after the open
constexpr uint32_t kCheckKeptMs = 1000;    // after a load that fails
constexpr const char* kSecondClip = "adam_vox_sample_3s.wav";
constexpr const char* kMissingClip = "/nonexistent/cubevox-missing.wav";
constexpr uint32_t kProofMs = 2500;        // the splash is over and the scope has audio
constexpr uint32_t kPassMs = 10;  // one scan of the 16 pots takes about this long on the board
constexpr int kMaxCatchUpPasses = 50;
constexpr int kCheckMinDbfs = -40;
constexpr int kSlot3 = 2;      // slot 3, counted from 0
constexpr int kMobSlot = 3;    // slot 4
constexpr int kSlot6 = 5;      // slot 6
constexpr int kSlot7 = 6;      // slot 7
constexpr int kReverbSlot = 7;  // slot 8, the reverb slot
constexpr uint32_t kCheckSwapMs = 1500;  // after a pick: the layout fade, then more than a second of audio

// A box and its audio run on the main thread with no device: each UI tick, one box pass, then the
// blocks that fill the tick pulled from the clip.
struct Rig {
  std::unique_ptr<AudioBlock> audio = std::make_unique<AudioBlock>();
  Box box;
  Source source;
  PanelInput panel;  // knobs at mid travel, every slot toggle on, MUTE off
  uint32_t nowMs = kStartMs;
  std::vector<float> output;

  Rig() : box(*audio, kStartMs) { box.audioStarted(); }

  void run(uint32_t ms, bool capture) {
    std::array<float, cv::kBlock> in;
    std::array<float, cv::kBlock> out;
    for (uint32_t t = 0; t < ms; t += kTickMs) {
      box.tick(panel, nowMs);
      for (int b = 0; b < kBlocksPerTick; ++b) {
        source.readClip(in.data(), cv::kBlock);
        audio->process(in.data(), out.data(), cv::kBlock);
        if (capture) output.insert(output.end(), out.begin(), out.end());
      }
      nowMs += kTickMs;
    }
  }

  // Ticks the box with no audio, which is a silent input, so minutes of idling cost little.
  void idle(uint32_t ms) {
    for (uint32_t t = 0; t < ms; t += kTickMs) {
      box.tick(panel, nowMs);
      nowMs += kTickMs;
    }
  }

  // One tick with the MENU turned `detents` steps and clicked when `click`.
  void turnMenu(int32_t detents, bool click) {
    panel.detents = detents;
    panel.click = click;
    idle(kTickMs);
    panel.detents = 0;
    panel.click = false;
  }

  // One tick with the MENU held.
  void holdMenu() {
    panel.hold = true;
    idle(kTickMs);
    panel.hold = false;
  }
};

// The clips the check plays. Adam's when samples/ holds the default clip, otherwise two synthetic
// voices written to a temporary directory that `synthetic` removes.
struct CheckClips {
  std::string first;       // the clip every rig loads
  std::string second;      // the clip File > Open loads
  std::string secondName;  // the file name clipName() reports for it
  std::unique_ptr<host::SyntheticClips> synthetic;
};

CheckClips pickCheckClips(const Source& source, const char* argv0) {
  CheckClips clips;
  clips.first = source.locateDefault(argv0);
  if (!clips.first.empty()) {
    clips.second = source.locate(argv0, kSecondClip);
    clips.secondName = kSecondClip;
    std::printf("clip: Adam's clips from samples/, %s then %s\n", clips.first.c_str(),
                clips.second.empty() ? kSecondClip : clips.second.c_str());
    return clips;
  }
  clips.synthetic = std::make_unique<SyntheticClips>();
  clips.first = clips.synthetic->first();
  clips.second = clips.synthetic->second();
  clips.secondName = SyntheticClips::kSecondName;
  std::printf("clip: samples/ is absent, so two synthetic voiced clips play, %s then %s, in %s\n",
              SyntheticClips::kFirstName, SyntheticClips::kSecondName, clips.synthetic->dir().c_str());
  return clips;
}

// What the default layout costs, from the cost table's own rows, and what it costs with MOB in
// slot 4 and Chasm in slot 8. The tracker is paid once, because Autotune is placed. Each "rest" is
// every row but slot 3's and slot 7's, which are Octave and Gate in the default layout and one of two
// cards each after a swap or a pick.
constexpr costs::Cost kDefaultRest = costs::kInputGate + costs::kAutotune + costs::kUnison + costs::kSlapback +
                                     costs::kDistortion + costs::kReverbSpring + costs::kFixed + costs::kTracker;
constexpr costs::Cost kSwappedRest = costs::kInputGate + costs::kAutotune + costs::kCrowd + costs::kSlapback +
                                     costs::kDistortion + costs::kReverbChasm + costs::kFixed + costs::kTracker;
constexpr costs::Cost kDefaultSum = kDefaultRest + costs::kOctave + costs::kGate;
constexpr costs::Cost kSwappedSum = kSwappedRest + costs::kOctave + costs::kGate;

// The slot 3 cards in the menu's swap order, and the slot 7 cards in its cycle order, with the row
// each costs. `id` is the card's registry id; `--proof` takes `arg`, or the id as an alias.
struct CardRow {
  Card card;
  const char* id;
  const char* arg;
  const char* name;
  costs::Cost cost;
};
constexpr std::array<CardRow, 3> kSlot3Rows = {
    {{Card::Formant, "formant", "formant", "FORMANT", costs::kFormant},
     {Card::FormantFollower, "formant_follow", "apraxia", "APRAXIA", costs::kFormantFollower},
     {Card::Octave, "octave", "octave", "OCTAVE", costs::kOctave}}};
constexpr std::array<CardRow, 2> kSlot6Rows = {{{Card::Growl, "growl", "growl", "DEEPTHROAT", costs::kGrowl},
                                                {Card::Distortion, "distortion", "distortion", "DISTORTION",
                                                 costs::kDistortion}}};

// The slot 8 cards in the menu's cycle order, Spring last so the walk ends on the default.
constexpr std::array<CardRow, 2> kSlot8Rows = {{{Card::ReverbChasm, "reverb_chasm", "chasm", "CHASM", costs::kReverbChasm},
                                                {Card::ReverbSpring, "reverb_spring", "spring", "SPRING",
                                                 costs::kReverbSpring}}};
constexpr std::array<CardRow, 3> kSlot7Rows = {{{Card::Lofi, "lofi", "lofi", "LO-FI", costs::kLofi},
                                                {Card::Bloom, "bloom", "chatter", "CHATTER", costs::kBloom},
                                                {Card::Gate, "gate", "gate", "GATE", costs::kGate}}};

// The slot 4 cards picked out of the menu's cycle order, so each pick takes two steps or one, and
// what the default layout costs without slot 4's card.
constexpr std::array<CardRow, 2> kSlot4Rows = {{{Card::Crowd, "crowd", "mob", "MOB", costs::kCrowd},
                                                {Card::Unison, "unison", "unison", "UNISON", costs::kUnison}}};
constexpr costs::Cost kSlot4Rest = costs::kInputGate + costs::kAutotune + costs::kOctave + costs::kSlapback +
                                   costs::kDistortion + costs::kGate + costs::kReverbSpring + costs::kFixed +
                                   costs::kTracker;

bool costIs(const LayoutCost& cost, const costs::Cost& expected) {
  return cost.avg == expected.avg && cost.worst == expected.worst;
}

bool cardIs(const Layout& layout, int slot, const char* id) {
  return std::strcmp(cardDesc(layout.card[static_cast<size_t>(slot)]).id, id) == 0;
}

bool anyLit(const Pixels& pixels) {
  for (const uint8_t p : pixels) {
    if (p != 0) return true;
  }
  return false;
}

float lastSecondDbfs(const std::vector<float>& output) {
  const size_t n = static_cast<size_t>(cv::kSampleRate);
  if (output.size() < n) return level::kNoSignalDbfs;
  level::InputEnergy energy;
  energy.samples = static_cast<uint32_t>(n);
  energy.sumSquares = level::sumSquares(output.data() + (output.size() - n), static_cast<int>(n));
  return level::toDbfs(level::rms(energy));
}

// The slot 3 cycle through the menu's action, to Formant, to Apraxia and back to Octave.
// After each step slot 3 holds the card, the layout's cost is the rest of its rows plus the card's own and Gate's, and the
// audio through the box is finite and above the floor. `swapped` puts MOB in slot 4 and Chasm in
// slot 8 first.
void checkSlot3(const CheckClips& clips, bool swapped, std::vector<std::string>& failed) {
  const char* const where = swapped ? "MOB and Chasm" : "default layout";
  const costs::Cost rest = swapped ? kSwappedRest : kDefaultRest;
  const auto expect = [&](bool ok, const std::string& what) {
    if (!ok) failed.push_back(std::string(where) + ": " + what);
  };

  Rig rig;
  expect(rig.source.loadClip(clips.first.c_str()), "the clip loads");
  if (swapped) {
    rig.box.swapUnisonMob();
    rig.box.swapReverb();
  }
  rig.run(kCheckSwapMs, true);
  expect(cardIs(rig.box.layout(), kSlot3, "octave") && costIs(rig.box.cost(), rest + costs::kOctave + costs::kGate),
         "slot 3 starts on Octave at the sum of its rows");

  for (const CardRow& row : kSlot3Rows) {
    const size_t from = rig.output.size();
    rig.box.swapOctaveFormant();
    rig.run(kCheckSwapMs, true);
    const LayoutCost cost = rig.box.cost();
    bool finite = true;
    for (size_t i = from; i < rig.output.size(); ++i) finite = finite && std::isfinite(rig.output[i]);
    const float dbfs = lastSecondDbfs(rig.output);
    const std::string name = std::string("slot 3 ") + row.name;
    expect(cardIs(rig.box.layout(), kSlot3, row.id), name + " is in slot 3 after the swap");
    expect(costIs(cost, rest + row.cost + costs::kGate), name + " costs the sum of its rows, kFixed and kTracker");
    expect(layoutValid(rig.box.layout()).ok(), name + " layout passes the placement rules");
    expect(finite, name + " output is finite");
    expect(dbfs > static_cast<float>(kCheckMinDbfs), name + " output RMS of the last second is above -40 dBFS");
    std::printf("%-14s slot 3 %-8s cost %d / %d, output %.1f dBFS\n", where, row.name, cost.avg, cost.worst,
                static_cast<double>(dbfs));
  }
}

// The Distortion and Growl swap through the box's switch, to Growl and back. After each swap slot 6
// holds the card, the layout's cost is the rest of its rows less Distortion's plus the card's own,
// Octave's and Gate's, and the audio through the box is finite and above the floor. `swapped` puts MOB
// in slot 4 and Chasm in slot 8 first.
void checkSlot6(const CheckClips& clips, bool swapped, std::vector<std::string>& failed) {
  const char* const where = swapped ? "MOB and Chasm" : "default layout";
  const costs::Cost rest = swapped ? kSwappedRest : kDefaultRest;
  const auto expect = [&](bool ok, const std::string& what) {
    if (!ok) failed.push_back(std::string(where) + ": " + what);
  };
  const auto sumWith = [&rest](const costs::Cost& card) {
    const costs::Cost others = rest + costs::kOctave + costs::kGate + card;
    return costs::Cost{static_cast<uint8_t>(others.avg - costs::kDistortion.avg),
                       static_cast<uint8_t>(others.worst - costs::kDistortion.worst), others.measured};
  };

  Rig rig;
  expect(rig.source.loadClip(clips.first.c_str()), "the clip loads");
  if (swapped) {
    rig.box.swapUnisonMob();
    rig.box.swapReverb();
  }
  rig.run(kCheckSwapMs, true);
  expect(cardIs(rig.box.layout(), kSlot6, "distortion") && costIs(rig.box.cost(), sumWith(costs::kDistortion)),
         "slot 6 starts on Distortion at the sum of its rows");

  for (const CardRow& row : kSlot6Rows) {
    const size_t from = rig.output.size();
    rig.box.swapDistortionGrowl();
    rig.run(kCheckSwapMs, true);
    const LayoutCost cost = rig.box.cost();
    bool finite = true;
    for (size_t i = from; i < rig.output.size(); ++i) finite = finite && std::isfinite(rig.output[i]);
    const float dbfs = lastSecondDbfs(rig.output);
    const std::string name = std::string("slot 6 ") + row.name;
    expect(cardIs(rig.box.layout(), kSlot6, row.id), name + " is in slot 6 after the swap");
    expect(costIs(cost, sumWith(row.cost)), name + " costs the sum of its rows, kFixed and kTracker");
    expect(layoutValid(rig.box.layout()).ok(), name + " layout passes the placement rules");
    expect(finite, name + " output is finite");
    expect(dbfs > static_cast<float>(kCheckMinDbfs), name + " output RMS of the last second is above -40 dBFS");
    std::printf("%-14s slot 6 %-10s cost %d / %d, output %.1f dBFS\n", where, row.name, cost.avg, cost.worst,
                static_cast<double>(dbfs));
  }
}

// The two slot 8 cards through the box's card switch on the default layout, Chasm and back to Spring.
// After each pick slot 8 holds the card, the layout's cost is the rest of its rows less Spring's plus
// the card's own, Octave's and Gate's, and the audio through the box is finite and above the floor.
void checkSlot8(const CheckClips& clips, std::vector<std::string>& failed) {
  const auto expect = [&](bool ok, const std::string& what) {
    if (!ok) failed.push_back("default layout: " + what);
  };
  Rig rig;
  expect(rig.source.loadClip(clips.first.c_str()), "the clip loads");
  rig.run(kCheckSwapMs, true);
  for (const CardRow& row : kSlot8Rows) {
    const size_t from = rig.output.size();
    rig.box.pickSlot8Card(row.card);
    rig.run(kCheckSwapMs, true);
    const LayoutCost cost = rig.box.cost();
    bool finite = true;
    for (size_t i = from; i < rig.output.size(); ++i) finite = finite && std::isfinite(rig.output[i]);
    const float dbfs = lastSecondDbfs(rig.output);
    const std::string name = std::string("slot 8 ") + row.name;
    const costs::Cost others = kDefaultSum + row.cost;
    expect(cardIs(rig.box.layout(), kReverbSlot, row.id), name + " is in slot 8 after the pick");
    expect(cost.avg == others.avg - costs::kReverbSpring.avg && cost.worst == others.worst - costs::kReverbSpring.worst,
           name + " costs the sum of its rows, kFixed and kTracker");
    expect(layoutValid(rig.box.layout()).ok(), name + " layout passes the placement rules");
    expect(finite, name + " output is finite");
    expect(dbfs > static_cast<float>(kCheckMinDbfs), name + " output RMS of the last second is above -40 dBFS");
    std::printf("default layout slot 8 %-8s cost %d / %d, output %.1f dBFS\n", row.name, cost.avg, cost.worst,
                static_cast<double>(dbfs));
  }
}

// The three slot 7 cards through the box's card switch, round the menu's cycle and back to Gate.
// After each pick the layout holds the card in slot 7, its cost is the rest of the layout's rows
// plus Octave's and the card's own, and the audio through the box is finite and above the floor.
// `swapped` puts MOB in slot 4 and Chasm in slot 8 first, the heaviest layout the cards share. A
// card outside the cycle leaves the layout as it was.
void checkSlot7(const CheckClips& clips, bool swapped, std::vector<std::string>& failed) {
  const char* const where = swapped ? "MOB and Chasm" : "default layout";
  const costs::Cost rest = swapped ? kSwappedRest : kDefaultRest;
  const auto expect = [&](bool ok, const std::string& what) {
    if (!ok) failed.push_back(std::string(where) + ": " + what);
  };

  Rig rig;
  expect(rig.source.loadClip(clips.first.c_str()), "the clip loads");
  if (swapped) {
    rig.box.swapUnisonMob();
    rig.box.swapReverb();
  }
  rig.run(kCheckSwapMs, true);
  expect(cardIs(rig.box.layout(), kSlot7, "gate") && costIs(rig.box.cost(), rest + costs::kOctave + costs::kGate),
         "slot 7 starts on Gate at the sum of its rows");

  for (const CardRow& row : kSlot7Rows) {
    const size_t from = rig.output.size();
    rig.box.pickSlot7Card(row.card);
    rig.run(kCheckSwapMs, true);
    const LayoutCost cost = rig.box.cost();
    bool finite = true;
    for (size_t i = from; i < rig.output.size(); ++i) finite = finite && std::isfinite(rig.output[i]);
    const float dbfs = lastSecondDbfs(rig.output);
    const std::string name = std::string("slot 7 ") + row.name;
    expect(cardIs(rig.box.layout(), kSlot7, row.id), name + " is in slot 7 after the pick");
    expect(costIs(cost, rest + costs::kOctave + row.cost), name + " costs the sum of its rows, kFixed and kTracker");
    expect(layoutValid(rig.box.layout()).ok(), name + " layout passes the placement rules");
    expect(finite, name + " output is finite");
    expect(dbfs > static_cast<float>(kCheckMinDbfs), name + " output RMS of the last second is above -40 dBFS");
    std::printf("%-14s slot 7 %-8s cost %d / %d, output %.1f dBFS\n", where, row.name, cost.avg, cost.worst,
                static_cast<double>(dbfs));
  }

  const Layout kept = rig.box.layout();
  rig.box.pickSlot7Card(Card::Formant);
  expect(rig.box.layout() == kept, "a card outside the cycle leaves the layout as it was");
}

// The two slot 4 cards through the box's card switch on the default layout. After each pick the
// layout holds the card in slot 4 at the sum of its rows, and the audio through the box is finite and
// above the floor.
void checkSlot4(const CheckClips& clips, std::vector<std::string>& failed) {
  const auto expect = [&](bool ok, const std::string& what) {
    if (!ok) failed.push_back("default layout: " + what);
  };
  Rig rig;
  expect(rig.source.loadClip(clips.first.c_str()), "the clip loads");
  rig.run(kCheckSwapMs, true);
  for (const CardRow& row : kSlot4Rows) {
    const size_t from = rig.output.size();
    rig.box.pickSlot4Card(row.card);
    rig.run(kCheckSwapMs, true);
    const LayoutCost cost = rig.box.cost();
    bool finite = true;
    for (size_t i = from; i < rig.output.size(); ++i) finite = finite && std::isfinite(rig.output[i]);
    const float dbfs = lastSecondDbfs(rig.output);
    const std::string name = std::string("slot 4 ") + row.name;
    expect(cardIs(rig.box.layout(), kMobSlot, row.id), name + " is in slot 4 after the pick");
    expect(costIs(cost, kSlot4Rest + row.cost), name + " costs the sum of its rows");
    expect(finite, name + " output is finite");
    expect(dbfs > static_cast<float>(kCheckMinDbfs), name + " output RMS of the last second is above -40 dBFS");
    std::printf("%-14s slot 4 %-8s cost %d / %d, output %.1f dBFS\n", "default layout", row.name, cost.avg,
                cost.worst, static_cast<double>(dbfs));
  }
}

// The display's contrast through the box. It starts at HIGH, drops to 8 once the dimming time has
// passed with nothing touched and no voice, and comes back at once on a knob turn, a MENU press, a
// slot toggle flip, the top switch or a voice. A click on OLED BRIGHT in the menu sets the awake
// contrast, and the window draws a lit pixel brighter as the contrast rises.
void checkDim(const CheckClips& clips, std::vector<std::string>& failed) {
  const auto expect = [&failed](bool ok, const char* what) {
    if (!ok) failed.push_back(std::string("dim: ") + what);
  };
  constexpr uint8_t kHigh = 207;
  constexpr uint8_t kMax = 255;
  constexpr uint8_t kLow = 48;
  constexpr uint8_t kDimmed = 8;
  constexpr int32_t kBrightnessItem = 10;  // the OLED BRIGHT row of the menu list, counted from 0
  constexpr uint32_t kSettleMs = 1000;    // long enough for a control to reach the face

  Rig rig;
  expect(rig.source.loadClip(clips.first.c_str()), "the clip loads");
  rig.idle(kSettleMs);
  expect(rig.box.contrast() == kHigh, "the screen starts at the HIGH contrast");

  rig.idle(kDimAfterMs + kSettleMs);
  expect(rig.box.contrast() == kDimmed, "the screen dims after ten minutes with nothing touched");

  rig.panel.knob[0] = 0.9f;
  rig.idle(kSettleMs);
  expect(rig.box.contrast() == kHigh, "a knob turn brings the screen back");

  rig.idle(kDimAfterMs + kSettleMs);
  expect(rig.box.contrast() == kDimmed, "the screen dims again");
  rig.turnMenu(0, true);
  expect(rig.box.contrast() == kHigh && rig.box.menuOpen(), "a MENU press wakes the screen in its own pass and opens the menu");

  rig.idle(kDimAfterMs + kSettleMs);
  expect(rig.box.contrast() == kDimmed, "the screen dims with the menu timed out");
  rig.turnMenu(1, false);
  expect(rig.box.contrast() == kHigh, "a MENU turn wakes the screen");

  rig.idle(kDimAfterMs + kSettleMs);
  rig.panel.toggle[0] = false;
  rig.idle(kSettleMs);
  expect(rig.box.contrast() == kHigh, "a slot toggle flip wakes the screen");
  rig.panel.toggle[0] = true;

  rig.idle(kDimAfterMs + kSettleMs);
  rig.panel.mute = true;
  rig.idle(kSettleMs);
  expect(rig.box.contrast() == kHigh, "the top switch wakes the screen");
  rig.panel.mute = false;

  rig.idle(kDimAfterMs + kSettleMs);
  expect(rig.box.contrast() == kDimmed, "the screen dims before the voice test");
  rig.run(kCheckBeforeMs, false);
  expect(rig.box.contrast() == kHigh, "a voice at the input wakes the screen");

  rig.idle(kDimAfterMs + kSettleMs);
  rig.turnMenu(0, true);
  rig.turnMenu(kBrightnessItem, false);
  rig.turnMenu(0, true);
  expect(rig.box.settings().brightness == Brightness::Max && rig.box.contrast() == kMax,
         "a click on OLED BRIGHT sets MAX and the display takes its contrast");
  rig.turnMenu(0, true);
  expect(rig.box.settings().brightness == Brightness::Low && rig.box.contrast() == kLow,
         "the next click wraps to LOW and the display takes its contrast");
  rig.idle(kDimAfterMs + kSettleMs);
  expect(rig.box.contrast() == kDimmed, "a dimmed screen is at 8 whatever the brightness");

  const auto red = [](ImU32 colour) { return colour & 0xFFu; };
  expect(theme::oledLit(0) == theme::kOledOff && red(theme::oledLit(kDimmed)) < red(theme::oledLit(kLow)) &&
             red(theme::oledLit(kLow)) < red(theme::oledLit(kHigh)) && red(theme::oledLit(kHigh)) < red(theme::oledLit(kMax)),
         "a lit pixel is drawn brighter as the contrast rises");
  expect(theme::oledLit(kHigh) == theme::pack(theme::kOledOnRgb),
         "HIGH draws the ink the screen had before OLED BRIGHT");
}

// A MENU hold closes the open list as the EXIT row does, and a click does not.
void checkMenuHold(const CheckClips& clips, std::vector<std::string>& failed) {
  const auto expect = [&failed](bool ok, const char* what) {
    if (!ok) failed.push_back(std::string("menu hold: ") + what);
  };
  constexpr uint32_t kPastSplashMs = 3000;  // the menu hears nothing until the splash ends

  Rig rig;
  expect(rig.source.loadClip(clips.first.c_str()), "the clip loads");
  rig.idle(kPastSplashMs);
  rig.turnMenu(0, true);
  expect(rig.box.menuOpen(), "a click opens the menu");
  rig.turnMenu(0, true);
  expect(rig.box.menuOpen(), "a click on the first row leaves the menu open");
  rig.holdMenu();
  expect(!rig.box.menuOpen(), "a hold closes the menu");
}

int runCheck(const char* argv0) {
  Rig rig;
  std::vector<std::string> failed;
  const auto expect = [&failed](bool ok, const char* what) {
    if (!ok) failed.push_back(what);
  };

  const CheckClips clips = pickCheckClips(rig.source, argv0);
  expect(rig.source.loadClip(clips.first.c_str()), "the clip loads");
  rig.run(kCheckBeforeMs, true);
  const LayoutCost before = rig.box.cost();
  rig.box.swapUnisonMob();
  rig.box.swapReverb();
  rig.run(kCheckAfterMs, true);
  const LayoutCost after = rig.box.cost();
  const Layout& layout = rig.box.layout();

  bool finite = true;
  for (const float v : rig.output) finite = finite && std::isfinite(v);
  const float dbfs = lastSecondDbfs(rig.output);

  // File > Open: a second clip swaps in and plays, and a path that does not decode leaves it playing.
  const size_t openedAt = rig.output.size();
  expect(!clips.second.empty() && rig.source.loadClip(clips.second.c_str()),
         "the second clip loads through loadClip");
  rig.run(kCheckOpenMs, true);
  bool openFinite = true;
  for (size_t i = openedAt; i < rig.output.size(); ++i) openFinite = openFinite && std::isfinite(rig.output[i]);
  const float openDbfs = lastSecondDbfs(rig.output);
  expect(clips.secondName == rig.source.clipName(), "clipName reads the second clip's file name");
  expect(openFinite, "every output sample after the open is finite");
  expect(openDbfs > static_cast<float>(kCheckMinDbfs), "the output RMS of the second clip's last second is above -40 dBFS");
  expect(!rig.source.loadClip(kMissingClip), "loadClip on a missing path returns false");
  rig.run(kCheckKeptMs, true);
  const float keptDbfs = lastSecondDbfs(rig.output);
  expect(clips.secondName == rig.source.clipName(), "a failed load keeps the clip name");
  expect(keptDbfs > static_cast<float>(kCheckMinDbfs), "the previous clip keeps playing after a failed load");

  expect(finite, "every output sample is finite");
  expect(dbfs > static_cast<float>(kCheckMinDbfs), "the output RMS of the last second is above -40 dBFS");
  expect(costIs(before, kDefaultSum), "the default layout costs the sum of its rows, kFixed and kTracker");
  expect(costIs(after, kSwappedSum), "the swapped layout costs the sum with kCrowd and kReverbChasm");
  expect(cardIs(layout, kMobSlot, "crowd"), "slot 4 reads crowd after the swap");
  expect(cardIs(layout, kReverbSlot, "reverb_chasm"), "slot 8 reads reverb_chasm after the swap");
  expect(rig.box.splashEnded(), "the splash ended");
  expect(anyLit(rig.box.pixels()), "the face rendered (the canvas is not blank)");

  checkSlot3(clips, false, failed);
  checkSlot3(clips, true, failed);
  checkSlot6(clips, false, failed);
  checkSlot6(clips, true, failed);
  checkSlot7(clips, false, failed);
  checkSlot7(clips, true, failed);
  checkSlot4(clips, failed);
  checkSlot8(clips, failed);
  checkDim(clips, failed);
  checkMenuHold(clips, failed);

  std::printf("cost before %d / %d, after %d / %d (mean / worst, %% of a block)\n", before.avg, before.worst, after.avg,
              after.worst);
  std::printf("output RMS of the last second %.1f dBFS over %zu samples\n", static_cast<double>(dbfs), openedAt);
  std::printf("after File > Open of %s %.1f dBFS, after a failed open %.1f dBFS\n", rig.source.clipName(),
              static_cast<double>(openDbfs), static_cast<double>(keptDbfs));
  for (const std::string& what : failed) std::printf("emulator check: FAILED: %s\n", what.c_str());
  if (!failed.empty()) return 1;
  std::printf("emulator check: ok\n");
  return 0;
}

int runProof(const char* argv0, const char* path, bool micAvailable, Card slot3, Card slot6, Card slot7,
             Card slot8) {
  Rig rig;
  Panel panel;
  rig.panel = panel.input;  // the window's start state, every toggle off
  if (!rig.source.findClip(argv0)) return 1;
  rig.box.pickSlot3Card(slot3);
  rig.box.pickSlot6Card(slot6);
  rig.box.pickSlot7Card(slot7);
  rig.box.pickSlot8Card(slot8);
  rig.run(kProofMs, false);
  Window window(false);
  if (!window.ok()) return 1;
  panel.input = rig.panel;
  if (!window.saveProof(rig.box, panel, micAvailable, path)) {
    std::fprintf(stderr, "[ERROR] proof: could not write %s\n", path);
    return 1;
  }
  return 0;
}

int runLive(const char* argv0) {
  Source source;
  source.findClip(argv0);  // a missing clip is reported and the window still opens
  const std::unique_ptr<AudioBlock> audio = std::make_unique<AudioBlock>();
  Box box(*audio, millis());
  if (source.startDevice(*audio)) box.audioStarted();
  Window window(true);
  if (!window.ok()) {
    source.stopDevice();
    return 1;
  }
  Panel panel;
  window.setClip(source.clipName());

  // File > Open picks the clip. A file that decodes plays from its start and sets the source to CLIP;
  // one that does not leaves the old clip playing.
  installFileMenu([&source, &panel, &window](const char* path) {
    if (!source.loadClip(path)) return;
    panel.useMic = false;
    window.setClip(source.clipName());
  });

  uint32_t passMs = millis();
  while (!window.shouldClose()) {
    window.begin();
    window.draw(box, panel, source.micAvailable());
    source.setUseMic(panel.useMic && source.micAvailable());

    // The box takes a pass every kPassMs of host time, however fast the frames come, so the face
    // ticks at its 25 frames a second on a 60 Hz or a 120 Hz screen alike. The encoder's turns and
    // click go into the first pass of the frame only.
    PanelInput input = panel.input;
    const uint32_t nowMs = millis();
    int passes = 0;
    while (nowMs - passMs >= kPassMs && passes < kMaxCatchUpPasses) {
      passMs += kPassMs;
      box.tick(input, passMs);
      input.detents = 0;
      input.click = false;
      input.hold = false;
      ++passes;
    }
    if (nowMs - passMs >= kPassMs) passMs = nowMs;  // the app stalled; drop the backlog
    window.end(box);
  }
  source.stopDevice();
  return 0;
}

int usage() {
  std::fprintf(stderr,
               "usage: cubevox-emulator [--check | --proof file.ppm [--no-mic] [--slot3 octave|formant|apraxia] "
               "[--slot6 distortion|growl] [--slot7 gate|lofi|chatter] [--slot8 spring|chasm]]\n");
  return 2;
}

// The card named by `arg` among `rows`, the same rows --check walks. A row answers to its argument and
// to its registry id.
template <size_t N>
bool cardFromRows(const std::array<CardRow, N>& rows, const char* arg, Card& out) {
  for (const CardRow& row : rows) {
    if (std::strcmp(arg, row.arg) == 0 || std::strcmp(arg, row.id) == 0) {
      out = row.card;
      return true;
    }
  }
  return false;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc == 1) return runLive(argv[0]);
  if (argc == 2 && std::strcmp(argv[1], "--check") == 0) return runCheck(argv[0]);
  if (argc >= 3 && std::strcmp(argv[1], "--proof") == 0) {
    bool noMic = false;
    Card slot3 = Card::Octave;
    Card slot6 = Card::Distortion;
    Card slot7 = Card::Gate;
    Card slot8 = Card::ReverbSpring;
    for (int i = 3; i < argc; ++i) {
      if (std::strcmp(argv[i], "--no-mic") == 0) {
        noMic = true;
      } else if (std::strcmp(argv[i], "--slot3") == 0 && i + 1 < argc && cardFromRows(kSlot3Rows, argv[i + 1], slot3)) {
        ++i;
      } else if (std::strcmp(argv[i], "--slot6") == 0 && i + 1 < argc && cardFromRows(kSlot6Rows, argv[i + 1], slot6)) {
        ++i;
      } else if (std::strcmp(argv[i], "--slot7") == 0 && i + 1 < argc && cardFromRows(kSlot7Rows, argv[i + 1], slot7)) {
        ++i;
      } else if (std::strcmp(argv[i], "--slot8") == 0 && i + 1 < argc && cardFromRows(kSlot8Rows, argv[i + 1], slot8)) {
        ++i;
      } else {
        return usage();
      }
    }
    return runProof(argv[0], argv[2], !noMic, slot3, slot6, slot7, slot8);
  }
  return usage();
}
