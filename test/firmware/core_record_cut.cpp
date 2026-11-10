// Cuts the power at every flash step of a save, an erase and the boot's ECC recovery, then powers on
// again. Every power-on has to end in a start that loads a valid layout, at least as new as the
// newest record written whole, before the bootloader's failed-start limit sends the box to update
// mode.
//
// The flash faults on a read of an entry that holds a bad word, the way a double ECC error
// bus-faults on the board. The start loop runs the bootloader's own decision (boot_decide.c) with
// its failed-start count, and each start runs the store's boot path.

#include "support.h"

#include <setjmp.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "board/boot_block.h"
#include "boot_decide.h"
#include "core/layout.h"
#include "core/record.h"
#include "core/record_store.h"

namespace {

constexpr uint32_t kPowerOnFlags = CV_RSR_PORRSTF | CV_RSR_BORRSTF;
constexpr uint32_t kFaultResetFlags = CV_RSR_SFTRSTF;
constexpr int kJumpFault = 1;
constexpr int kJumpPowerLost = 2;
constexpr int kMaxStarts = 12;

// What a power cut leaves of the flash step it hits.
enum class Cut : uint8_t {
  Before,       // the step never started
  EccWord,      // a program left part of its bits, and the word reads as a double ECC error
  QuietWord,    // a program left part of its bits, and the word reads back without an error
  EraseBad,     // an erase left the sector reading as a double ECC error from its first word
  EraseKeeps,   // an erase left the first entries as they were, a double ECC error after them
  EraseClears,  // an erase blanked the first entries and left the rest as they were
};

const char* cutName(Cut c) {
  switch (c) {
    case Cut::Before: return "before";
    case Cut::EccWord: return "ecc-word";
    case Cut::QuietWord: return "quiet-word";
    case Cut::EraseBad: return "erase-bad";
    case Cut::EraseKeeps: return "erase-keeps";
    case Cut::EraseClears: return "erase-clears";
  }
  return "?";
}

constexpr size_t kKeptBytes = 3 * kRecordBytes;  // what an EraseKeeps or EraseClears cut leaves alone

struct BadWord {
  int sector;
  uint32_t offset;
};

enum class Step : uint8_t { Erase, Program };

class CutFlash : public FlashIo {
 public:
  CutFlash() {
    for (std::vector<uint8_t>& m : mem) m.assign(kRecordSectorBytes, 0xFF);
  }

  const uint8_t* read(int sector) const override { return mem[sector].data(); }

  const uint8_t* view(int sector, uint32_t offset, size_t bytes) const override {
    for (const BadWord& b : bad) {
      if (b.sector != sector || b.offset < offset || b.offset >= offset + bytes) continue;
      if (jump == nullptr) std::abort();
      faultSector = sector;
      faultOffset = b.offset;
      longjmp(*jump, kJumpFault);
    }
    return mem[sector].data() + offset;
  }

  void eraseStart(int sector) override {
    if (cutNow(Step::Erase)) {
      cutErase(sector);
      longjmp(*jump, kJumpPowerLost);
    }
    erasing_ = sector;
  }

  bool eraseBusy() override {
    if (erasing_ < 0) return false;
    mem[erasing_].assign(kRecordSectorBytes, 0xFF);
    dropBad(erasing_, 0, kRecordSectorBytes);
    erasing_ = -1;
    return false;
  }

  void programWord(int sector, uint32_t offset, const uint8_t* word) override {
    if (cutNow(Step::Program)) {
      cutProgram(sector, offset, word);
      longjmp(*jump, kJumpPowerLost);
    }
    for (size_t i = 0; i < kFlashWordBytes; ++i) mem[sector][offset + i] &= word[i];
    if ((offset + kFlashWordBytes) % kRecordBytes != 0) return;
    RecordData rec;
    const uint32_t entry = offset / kRecordBytes * kRecordBytes;
    if (decodeRecord(mem[sector].data() + entry, rec)) committed = std::max(committed, rec.sequence);
  }

  bool takeError() override { return false; }

  void put(int sector, int entry, uint32_t sequence) {
    RecordData rec;
    rec.sequence = sequence;
    std::snprintf(rec.name, sizeof rec.name, "seq %u", static_cast<unsigned>(sequence));
    const RecordBytes b = encodeRecord(rec);
    std::memcpy(&mem[sector][static_cast<size_t>(entry) * kRecordBytes], b.data(), kRecordBytes);
    committed = std::max(committed, sequence);
  }

  void arm(int at, Cut how) {
    cutAt = at;
    cut = how;
    steps = 0;
    trace.clear();
  }

  std::vector<uint8_t> mem[kRecordSectors];
  std::vector<BadWord> bad;
  jmp_buf* jump = nullptr;
  mutable int faultSector = -1;
  mutable uint32_t faultOffset = 0;
  uint32_t committed = 0;   // the newest sequence written whole
  int cutAt = -1;           // the step the power cut hits, counted from arm(); -1 for none
  Cut cut = Cut::Before;
  int steps = 0;
  std::vector<Step> trace;  // the steps since arm()

 private:
  bool cutNow(Step step) {
    trace.push_back(step);
    return steps++ == cutAt;
  }

  void dropBad(int sector, size_t from, size_t to) {
    bad.erase(std::remove_if(bad.begin(), bad.end(),
                             [&](const BadWord& b) { return b.sector == sector && b.offset >= from && b.offset < to; }),
              bad.end());
  }

  void cutErase(int sector) {
    std::vector<uint8_t>& m = mem[sector];
    switch (cut) {
      case Cut::EraseBad: bad.push_back({sector, 0}); break;
      case Cut::EraseKeeps:
        std::fill(m.begin() + kKeptBytes, m.end(), 0xFF);
        dropBad(sector, kKeptBytes, kRecordSectorBytes);
        bad.push_back({sector, static_cast<uint32_t>(kKeptBytes)});
        break;
      case Cut::EraseClears:
        std::fill(m.begin(), m.begin() + kKeptBytes, 0xFF);
        dropBad(sector, 0, kKeptBytes);
        break;
      default: break;
    }
  }

  void cutProgram(int sector, uint32_t offset, const uint8_t* word) {
    if (cut == Cut::Before) return;
    for (size_t i = 0; i < kFlashWordBytes / 2; ++i) mem[sector][offset + i] &= word[i];
    if (cut == Cut::EccWord) bad.push_back({sector, offset});
  }

  int erasing_ = -1;
};

// Runs `fn` until it returns, faults on a bad word or loses power. Returns 0, kJumpFault or
// kJumpPowerLost.
template <typename Fn>
int guarded(CutFlash& flash, Fn fn) {
  jmp_buf env;
  flash.jump = &env;
  int code = 0;
  switch (setjmp(env)) {
    case 0: fn(); break;
    case kJumpFault: code = kJumpFault; break;
    default: code = kJumpPowerLost; break;
  }
  flash.jump = nullptr;
  return code;
}

// What lives across starts: the flash, the boot block, and what the last start left in RAM.
struct Box {
  CutFlash flash;
  CvBootBlock block{};
  RecordReadLimits limits{};  // .cv_noinit
  bool faulted = false;  // the last start ended in a bank 2 fault
  int faultSector = 0;
  uint32_t faultOffset = 0;
  int saves = 0;
  uint32_t nowMs = 0;

  // The start's results.
  std::unique_ptr<RecordStore> store;
  Layout layout = defaultLayout();
  Settings settings;
  bool hasRecord = false;
  uint32_t loaded = 0;
  int starts = 0;
};

// What one start does with the record before the box plays, as recordBoot() does it: the read
// limits from .cv_noinit, the recovery of the last start's fault with its give-back, the load.
void appStart(Box& box, bool fault, bool powerOn) {
  box.store = std::make_unique<RecordStore>(box.flash);
  RecordStore& store = *box.store;
  store.useReadLimits(box.limits, powerOn);
  if (fault) {
    const RecordRecovery r = store.recover(box.faultSector, box.faultOffset);
    cvBootFailsSet(&box.block, failsAfterEccRecovery(cvBootFailsGet(&box.block), r.learned));
  }
  box.layout = defaultLayout();
  box.settings = Settings{};
  const RecordLoad load = loadNewestRecord(store, box.layout, box.settings, box.nowMs);
  box.hasRecord = load.loaded;
  box.loaded = load.sequence;
}

enum class End : uint8_t { Healthy, UpdateMode, PowerLost, Stuck };

// Starts from a power-on until a start runs healthy, the bootloader stays in update mode, or the
// power cut hits.
End powerOn(Box& box) {
  uint32_t flags = kPowerOnFlags;
  box.faulted = false;  // a power-on leaves the fault record random, and its magic fails
  box.starts = 0;
  for (int start = 0; start < kMaxStarts; ++start) {
    cvBootAdopt(&box.block, flags);
    const CvBootInputs in{false, CvImageValid, 0u};
    if (cvBootDecide(&box.block, &in).action != CvActionRunApp) return End::UpdateMode;
    ++box.starts;
    const bool fault = box.faulted;
    const bool cold = flags == kPowerOnFlags;
    box.faulted = false;
    const int code = guarded(box.flash, [&] { appStart(box, fault, cold); });
    if (code == 0) {
      cvBootFailsSet(&box.block, 0u);  // the start ran healthy for CV_HEALTHY_MS
      return End::Healthy;
    }
    if (code == kJumpPowerLost) return End::PowerLost;
    box.faulted = true;
    box.faultSector = box.flash.faultSector;
    box.faultOffset = box.flash.faultOffset;
    flags = kFaultResetFlags;
  }
  return End::Stuck;
}

// A menu change and its save, run to the end. False when the power cut hits it.
bool save(Box& box) {
  char name[kLayoutNameBytes + 1];
  std::snprintf(name, sizeof name, "save %d", ++box.saves);
  return guarded(box.flash, [&] {
           RecordStore& store = *box.store;
           store.setName(name);
           store.request(box.layout, box.settings, box.nowMs);
           box.nowMs += kSaveDebounceMs;
           for (int i = 0; i < 64 && store.status().busy; ++i) store.poll(box.nowMs++);
         }) == 0;
}

// A power-on, then `saves` saves.
End phase(Box& box, int cutAt, Cut cut, int saves) {
  box.flash.arm(cutAt, cut);
  const End end = powerOn(box);
  if (end != End::Healthy) return end;
  for (int i = 0; i < saves; ++i) {
    if (!save(box)) return End::PowerLost;
  }
  return End::Healthy;
}

// The flash and RAM a power cut leaves; a power-on clears the rest.
struct Snapshot {
  CutFlash flash;
  CvBootBlock block{};
  int saves = 0;
  uint32_t nowMs = 0;
};

void restore(Box& box, const Snapshot& s) {
  box.store.reset();
  box.flash.mem[0] = s.flash.mem[0];
  box.flash.mem[1] = s.flash.mem[1];
  box.flash.bad = s.flash.bad;
  box.flash.committed = s.flash.committed;
  box.block = s.block;
  box.saves = s.saves;
  box.nowMs = s.nowMs;
  std::memset(&box.limits, 0xA5, sizeof box.limits);  // a power-on leaves DTCM random
}

Snapshot snapshot(const Box& box) {
  Snapshot s;
  s.flash.mem[0] = box.flash.mem[0];
  s.flash.mem[1] = box.flash.mem[1];
  s.flash.bad = box.flash.bad;
  s.flash.committed = box.flash.committed;
  s.block = box.block;
  s.saves = box.saves;
  s.nowMs = box.nowMs;
  return s;
}

struct Sweep {
  const char* state = "";
  std::string path;  // the cuts so far, for the failure line
  int lives = 0;
  int failures = 0;
  int worstStarts = 0;
};

void fail(Sweep& sweep, const char* what) {
  ++sweep.failures;
  if (sweep.failures <= 8) std::printf("FAIL record cut: %s, %s: %s\n", sweep.state, sweep.path.c_str(), what);
}

// The power-on after the last cut, a save, and one more power-on.
void finalCheck(Sweep& sweep, const Snapshot& s) {
  ++sweep.lives;
  Box box;
  restore(box, s);
  box.flash.arm(-1, Cut::Before);
  const uint32_t committed = box.flash.committed;
  const End end = powerOn(box);
  sweep.worstStarts = std::max(sweep.worstStarts, box.starts);
  if (end != End::Healthy) {
    fail(sweep, end == End::UpdateMode ? "the power-on ends in update mode" : "the power-on never runs healthy");
    return;
  }
  if (!layoutValid(box.layout).ok()) return fail(sweep, "the start loads an invalid layout");
  if (committed != 0 && (!box.hasRecord || box.loaded < committed)) {
    return fail(sweep, "the start loads a record older than the newest one written whole");
  }
  if (!save(box)) return fail(sweep, "a save after the power-on lost power with no cut armed");
  const uint32_t saved = box.flash.committed;
  if (powerOn(box) != End::Healthy || !box.hasRecord || box.loaded != saved) {
    fail(sweep, "the save after the recovery does not load at the next power-on");
  }
}

// Runs one phase from `s` with every cut it allows, and below each cut `depth` more phases.
void sweepFrom(Sweep& sweep, const Snapshot& s, int saves, int depth) {
  Box box;
  restore(box, s);
  const End clean = phase(box, -1, Cut::Before, saves);
  if (clean == End::UpdateMode || clean == End::Stuck) {
    return fail(sweep, clean == End::UpdateMode ? "an uncut power-on ends in update mode" : "an uncut power-on never runs healthy");
  }
  const std::vector<Step> steps = box.flash.trace;
  finalCheck(sweep, snapshot(box));

  const std::string path = sweep.path;
  for (size_t at = 0; at < steps.size(); ++at) {
    const bool erase = steps[at] == Step::Erase;
    const std::vector<Cut> cuts = erase ? std::vector<Cut>{Cut::Before, Cut::EraseBad, Cut::EraseKeeps, Cut::EraseClears}
                                        : std::vector<Cut>{Cut::Before, Cut::EccWord, Cut::QuietWord};
    for (const Cut cut : cuts) {
      restore(box, s);
      sweep.path = path + " cut " + std::to_string(at) + " " + cutName(cut);
      const End end = phase(box, static_cast<int>(at), cut, saves);
      if (end == End::UpdateMode || end == End::Stuck) {
        fail(sweep, end == End::UpdateMode ? "a power-on before the cut ends in update mode" : "a power-on never runs healthy");
        continue;
      }
      if (end != End::PowerLost) {
        fail(sweep, "the cut never hit");
        continue;
      }
      const Snapshot after = snapshot(box);
      if (depth > 0) {
        sweepFrom(sweep, after, 1, depth - 1);
      } else {
        finalCheck(sweep, after);
      }
    }
  }
  sweep.path = path;
}

using Setup = void (*)(CutFlash&);

void blankFlash(CutFlash&) {}

void fewRecords(CutFlash& f) {
  for (int e = 0; e < 5; ++e) f.put(0, e, 1 + static_cast<uint32_t>(e));
}

// The newest sector ends in an entry cut off mid-write that reads without an error, so the log has
// to move. The other sector holds a few older records, so the move erases it.
void tornAndOld(CutFlash& f) {
  for (int e = 0; e < 6; ++e) f.put(0, e, 101 + static_cast<uint32_t>(e));
  std::memset(&f.mem[0][5 * kRecordBytes + 3 * kFlashWordBytes + 8], 0x00, 8);
  f.committed = 105;
  for (int e = 0; e < 5; ++e) f.put(1, e, 1 + static_cast<uint32_t>(e));
}

// As above with the other sector blank, so the move needs no erase.
void tornAndBlank(CutFlash& f) {
  for (int e = 0; e < 6; ++e) f.put(0, e, 101 + static_cast<uint32_t>(e));
  std::memset(&f.mem[0][5 * kRecordBytes + 3 * kFlashWordBytes + 8], 0x00, 8);
  f.committed = 105;
}

// One entry left in the newest sector, so the first save fills it and the second moves the log over
// the older sector.
void oneLeft(CutFlash& f) {
  for (int e = 0; e < 5; ++e) f.put(1, e, 1 + static_cast<uint32_t>(e));
  for (int e = 0; e + 1 < kEntriesPerSector; ++e) f.put(0, e, 1001 + static_cast<uint32_t>(e));
}

// The audit's case: the newest sector ends in a bad word, and a cut erase left older records ahead
// of a bad word in the other sector.
void bothBad(CutFlash& f) {
  for (int e = 0; e < 10; ++e) f.put(0, e, 1000 + static_cast<uint32_t>(e));
  f.bad.push_back({0, 10 * kRecordBytes});
  for (int e = 0; e < 10; ++e) f.put(1, e, 10 + static_cast<uint32_t>(e));
  f.bad.push_back({1, 10 * kRecordBytes + 2 * kFlashWordBytes});
}

void sweepState(const char* name, Setup setup, int depth) {
  Sweep sweep;
  sweep.state = name;
  Box box;
  setup(box.flash);
  sweepFrom(sweep, snapshot(box), 2, depth);
  char line[160];
  std::snprintf(line, sizeof line, "record cut: %s, %d power cuts at every flash step, every power-on loads a valid layout",
                name, sweep.lives);
  check(sweep.failures == 0, line);
  if (sweep.failures > 0) std::printf("  %d of %d lives failed\n", sweep.failures, sweep.lives);
}

// The audit's probe case 1: the recovery's copy loses power at its third word. The next power-on
// faults once in each sector and boots on the newest record.
void checkCutCopy() {
  Box box;
  for (int e = 0; e < 5; ++e) box.flash.put(0, e, 1 + static_cast<uint32_t>(e));
  box.flash.bad.push_back({0, 5 * kRecordBytes + 2 * kFlashWordBytes});
  box.flash.arm(2, Cut::EccWord);
  std::memset(&box.limits, 0xA5, sizeof box.limits);
  check(powerOn(box) == End::PowerLost, "record cut: the recovery's copy loses power at its third word");
  box.flash.arm(-1, Cut::Before);
  std::memset(&box.limits, 0xA5, sizeof box.limits);
  check(powerOn(box) == End::Healthy && box.loaded == 5 && box.starts == 3,
        "record cut: the next power-on boots on the newest record at its third start");
}

// Starts that each end in a bank 2 ECC fault the next start learns from never reach the
// failed-start limit. Without the give-back the bootloader stops them at the limit.
void checkGiveBack() {
  for (const bool learned : {true, false}) {
    CvBootBlock block{};
    uint32_t flags = kPowerOnFlags;
    int ran = 0;
    for (int start = 0; start < 6; ++start) {
      cvBootAdopt(&block, flags);
      const CvBootInputs in{false, CvImageValid, 0u};
      if (cvBootDecide(&block, &in).action != CvActionRunApp) break;
      ++ran;
      if (start > 0) cvBootFailsSet(&block, failsAfterEccRecovery(cvBootFailsGet(&block), learned));
      flags = kFaultResetFlags;  // and this start faults as well
    }
    check(ran == (learned ? 6 : static_cast<int>(CV_BOOT_FAIL_LIMIT)),
          learned ? "record cut: six starts that each learn a bad word all run"
                  : "record cut: starts that learn nothing stop at the failed-start limit");
  }
}

}  // namespace

void checkRecordCuts() {
  checkCutCopy();
  checkGiveBack();
  sweepState("blank flash", blankFlash, 1);
  sweepState("five records", fewRecords, 1);
  sweepState("torn tail, older records in the other", tornAndOld, 1);
  sweepState("torn tail, the other blank", tornAndBlank, 1);
  // A full sector costs a CRC per entry at every scan, so this state cuts its saves only. The
  // recoveries from its cuts take the same steps the states above sweep.
  sweepState("one entry left", oneLeft, 0);
  sweepState("both sectors hold a bad word", bothBad, 1);
}
