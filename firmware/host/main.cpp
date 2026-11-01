// cubevox-emulator: the box's firmware code on the Mac.
//   cubevox-emulator                          opens the window
//   cubevox-emulator --check                  headless check, no window and no audio device
//   cubevox-emulator --proof f.ppm [--no-mic] [--slot7 gate|formant|lofi|bloom]
//                                             one frame of the window as a P6 PPM, no audio device,
//                                             with that card in slot 7 (Gate when omitted)

#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "audio_block.h"
#include "box.h"
#include "clock.h"
#include "core/costs.h"
#include "core/layout.h"
#include "core/level.h"
#include "core/registry.h"
#include "file_menu.h"
#include "source.h"
#include "ui/gui.h"
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
constexpr uint32_t kProofMs = 2500;        // the splash is over and the ribbon has history
constexpr uint32_t kPassMs = 10;  // one scan of the 16 pots takes about this long on the board
constexpr int kMaxCatchUpPasses = 50;
constexpr int kCheckMinDbfs = -40;
constexpr int kMobSlot = 3;    // slot 4, counted from 0
constexpr int kSlot7 = 6;      // slot 7
constexpr int kChasmSlot = 7;  // slot 8
constexpr uint32_t kCheckSlot7Ms = 1500;  // after a pick: the layout fade, then more than a second of audio

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
};

// What the default layout costs, from the cost table's own rows, and what it costs with MOB in
// slot 4 and Chasm in slot 8. The tracker is paid once, because Autotune is placed. Each "rest" is
// every row but slot 7's, which is Gate in the default layout and one of four cards after a pick.
constexpr costs::Cost kDefaultRest = costs::kInputGate + costs::kAutotune + costs::kOctave + costs::kUnison +
                                     costs::kSlapback + costs::kDistortion + costs::kReverbSpring + costs::kFixed +
                                     costs::kTracker;
constexpr costs::Cost kSwappedRest = costs::kInputGate + costs::kAutotune + costs::kOctave + costs::kCrowd +
                                     costs::kSlapback + costs::kDistortion + costs::kReverbChasm + costs::kFixed +
                                     costs::kTracker;
constexpr costs::Cost kDefaultSum = kDefaultRest + costs::kGate;
constexpr costs::Cost kSwappedSum = kSwappedRest + costs::kGate;

// The slot 7 cards in the menu's cycle order, with the row each costs.
struct Slot7Row {
  Card card;
  const char* id;
  const char* name;
  costs::Cost cost;
};
constexpr std::array<Slot7Row, 4> kSlot7Rows = {{{Card::Formant, "formant", "FORMANT", costs::kFormant},
                                                 {Card::Lofi, "lofi", "LO-FI", costs::kLofi},
                                                 {Card::Bloom, "bloom", "BLOOM", costs::kBloom},
                                                 {Card::Gate, "gate", "GATE", costs::kGate}}};

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

// The four slot 7 cards through the box's card switch, round the menu's cycle and back to Gate. After
// each pick the layout holds the card in slot 7, its cost is the rest of the layout's rows plus the
// card's own, and the audio through the box is finite and above the floor. `swapped` puts MOB in
// slot 4 and Chasm in slot 8 first, the heaviest layout the cards share. A card outside the cycle
// leaves the layout as it was.
void checkSlot7(const char* argv0, bool swapped, std::vector<std::string>& failed) {
  const char* const where = swapped ? "MOB and Chasm" : "default layout";
  const costs::Cost rest = swapped ? kSwappedRest : kDefaultRest;
  const auto expect = [&](bool ok, const std::string& what) {
    if (!ok) failed.push_back(std::string(where) + ": " + what);
  };

  Rig rig;
  expect(rig.source.findClip(argv0), "the clip loads");
  if (swapped) {
    rig.box.swapUnisonMob();
    rig.box.swapReverb();
  }
  rig.run(kCheckSlot7Ms, true);
  expect(cardIs(rig.box.layout(), kSlot7, "gate") && costIs(rig.box.cost(), rest + costs::kGate),
         "slot 7 starts on Gate at the sum of its rows");

  for (const Slot7Row& row : kSlot7Rows) {
    const size_t from = rig.output.size();
    rig.box.pickSlot7Card(row.card);
    rig.run(kCheckSlot7Ms, true);
    const LayoutCost cost = rig.box.cost();
    bool finite = true;
    for (size_t i = from; i < rig.output.size(); ++i) finite = finite && std::isfinite(rig.output[i]);
    const float dbfs = lastSecondDbfs(rig.output);
    const std::string name = std::string("slot 7 ") + row.name;
    expect(cardIs(rig.box.layout(), kSlot7, row.id), name + " is in slot 7 after the pick");
    expect(costIs(cost, rest + row.cost), name + " costs the sum of its rows, kFixed and kTracker");
    expect(layoutValid(rig.box.layout()).ok(), name + " layout passes the placement rules");
    expect(finite, name + " output is finite");
    expect(dbfs > static_cast<float>(kCheckMinDbfs), name + " output RMS of the last second is above -40 dBFS");
    std::printf("%-14s slot 7 %-8s cost %d / %d, output %.1f dBFS\n", where, row.name, cost.avg, cost.worst,
                static_cast<double>(dbfs));
  }

  const Layout kept = rig.box.layout();
  rig.box.pickSlot7Card(Card::Octave);
  expect(rig.box.layout() == kept, "a card outside the cycle leaves the layout as it was");
}

int runCheck(const char* argv0) {
  Rig rig;
  std::vector<std::string> failed;
  const auto expect = [&failed](bool ok, const char* what) {
    if (!ok) failed.push_back(what);
  };

  expect(rig.source.findClip(argv0), "the clip loads");
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
  const std::string second = rig.source.locate(argv0, kSecondClip);
  expect(!second.empty() && rig.source.loadClip(second.c_str()), "the second clip loads through loadClip");
  rig.run(kCheckOpenMs, true);
  bool openFinite = true;
  for (size_t i = openedAt; i < rig.output.size(); ++i) openFinite = openFinite && std::isfinite(rig.output[i]);
  const float openDbfs = lastSecondDbfs(rig.output);
  expect(std::strcmp(rig.source.clipName(), kSecondClip) == 0, "clipName reads the second clip's file name");
  expect(openFinite, "every output sample after the open is finite");
  expect(openDbfs > static_cast<float>(kCheckMinDbfs), "the output RMS of the second clip's last second is above -40 dBFS");
  expect(!rig.source.loadClip(kMissingClip), "loadClip on a missing path returns false");
  rig.run(kCheckKeptMs, true);
  const float keptDbfs = lastSecondDbfs(rig.output);
  expect(std::strcmp(rig.source.clipName(), kSecondClip) == 0, "a failed load keeps the clip name");
  expect(keptDbfs > static_cast<float>(kCheckMinDbfs), "the previous clip keeps playing after a failed load");

  expect(finite, "every output sample is finite");
  expect(dbfs > static_cast<float>(kCheckMinDbfs), "the output RMS of the last second is above -40 dBFS");
  expect(costIs(before, kDefaultSum), "the default layout costs the sum of its rows, kFixed and kTracker");
  expect(costIs(after, kSwappedSum), "the swapped layout costs the sum with kCrowd and kReverbChasm");
  expect(cardIs(layout, kMobSlot, "crowd"), "slot 4 reads crowd after the swap");
  expect(cardIs(layout, kChasmSlot, "reverb_chasm"), "slot 8 reads reverb_chasm after the swap");
  expect(rig.box.splashEnded(), "the splash ended");
  expect(anyLit(rig.box.pixels()), "the face rendered (the canvas is not blank)");

  checkSlot7(argv0, false, failed);
  checkSlot7(argv0, true, failed);

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

int runProof(const char* argv0, const char* path, bool micAvailable, Card slot7) {
  Rig rig;
  if (!rig.source.findClip(argv0)) return 1;
  rig.box.pickSlot7Card(slot7);
  rig.run(kProofMs, false);
  Window window(false);
  if (!window.ok()) return 1;
  Panel panel;
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
               "usage: cubevox-emulator [--check | --proof file.ppm [--no-mic] [--slot7 gate|formant|lofi|bloom]]\n");
  return 2;
}

// The slot 7 card with this id, from the same rows --check walks.
bool slot7FromId(const char* id, Card& out) {
  for (const Slot7Row& row : kSlot7Rows) {
    if (std::strcmp(id, row.id) == 0) {
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
    Card slot7 = Card::Gate;
    for (int i = 3; i < argc; ++i) {
      if (std::strcmp(argv[i], "--no-mic") == 0) {
        noMic = true;
      } else if (std::strcmp(argv[i], "--slot7") == 0 && i + 1 < argc && slot7FromId(argv[i + 1], slot7)) {
        ++i;
      } else {
        return usage();
      }
    }
    return runProof(argv[0], argv[2], !noMic, slot7);
  }
  return usage();
}
