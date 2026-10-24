// Serial protocol: line framing, request parsing and reply formatting.
// One flat JSON object per line, hand-written (spec 2026-10-12-serial-protocol-design.md).

#pragma once

#include <stddef.h>
#include <stdint.h>

#include <array>

#include "core/bench_ops.h"
#include "core/layout.h"
#include "core/registry.h"
#include "core/settings.h"
#include "core/slots.h"

namespace protocol {

constexpr int kVersion = 1;
constexpr size_t kMaxLine = 768;
constexpr size_t kIdBytes = kCardIdBytes;  // a longer card id is clipped to this
constexpr size_t kNameBytes = kLayoutNameBytes;
constexpr size_t kErrorBytes = 96;

// Sink for reply text. `hello` streams through it in pieces.
class Writer {
 public:
  // Longest text one format() call can hold.
  static constexpr size_t kFormatBytes = 160;

  virtual ~Writer() = default;
  virtual void write(const char* text, size_t len) = 0;

  // Writes printf-formatted text. Text that does not fit kFormatBytes is not written at all; the
  // call returns false and counts an overflow, so a lost piece of a reply shows in overflows().
  bool format(const char* fmt, ...) __attribute__((format(printf, 2, 3)));

  // Pieces of replies lost to format() overflow since boot.
  uint32_t overflows() const { return overflows_; }

 private:
  uint32_t overflows_ = 0;
};

// Splits the byte stream into JSON lines. A `{` outside a line starts one, and it runs to the next
// '\n'. Any other byte outside a line is ignored and counted.
class Framer {
 public:
  enum class Kind : uint8_t { None, Line, TooLong, BadByte };

  // `Line` leaves the NUL-terminated text in line(). `TooLong` fires once; the rest of the line is
  // dropped. `BadByte` fires once on a NUL inside a line, which would cut the text short; the rest
  // of that line is dropped too.
  Kind feed(uint8_t byte);
  const char* line() const { return line_.data(); }

  // Bytes seen outside a line since boot.
  uint32_t strayBytes() const { return stray_; }

  // Forgets a half-received line, for when a new host session starts.
  void reset() {
    len_ = 0;
    inLine_ = false;
    dropping_ = false;
  }

 private:
  std::array<char, kMaxLine + 1> line_ = {};
  size_t len_ = 0;
  bool inLine_ = false;
  bool dropping_ = false;
  uint32_t stray_ = 0;
};

enum class Command : uint8_t { Hello, GetLayout, SetLayout, Status, EnterDfu, Bench };

struct Request {
  uint32_t id = 0;
  Command cmd = Command::Hello;
  BenchOp op = BenchOp::Report;  // Bench only
  int benchSlot = 0;             // Bench only; the slot_off target, 1 to kMaxBenchSlot
  char name[kNameBytes + 1] = {};
  int slotCount = 0;  // ids present in "slots"
  std::array<std::array<char, kIdBytes>, kSlotCount> slot = {};

  // The "settings" object: polish_comp, polish_eq and top_switch. `settingsGiven` has a kGiven bit
  // for each field the request carried, and `settings` holds those values. A field the request
  // leaves out stays as the box has it. The output and input settings are not on the wire.
  Settings settings;
  uint8_t settingsGiven = 0;
};

constexpr uint8_t kGivenPolishComp = 1u << 0;
constexpr uint8_t kGivenPolishEq = 1u << 1;
constexpr uint8_t kGivenTopSwitch = 1u << 2;

// Copies the settings fields the request carried onto `current`; the others stay as they are.
void applySettings(const Request& req, Settings& current);

// Parses and checks one request. On failure returns false with a readable reason in `err`; `req.id`
// holds the request's id when the parser got far enough to read it, otherwise 0. A bench op the
// build does not carry (`benchBuild` false) is an error.
bool parseRequest(const char* line, Request& req, char* err, size_t errSize, bool benchBuild);

// Maps the request's card ids onto a layout and checks it with layoutValid.
bool resolveLayout(const Request& req, Layout& out, char* err, size_t errSize);

// Fill the fields by name. The constructor is explicit and non-aggregate on purpose, so a positional
// initializer cannot swap the four adjacent counters.
struct Status {
  float cpuMeanPercent = 0.0f;
  float cpuWorstPercent = 0.0f;
  uint32_t late = 0;
  bool muted = false;
  uint32_t seq = 0;
  uint32_t recordErrors = 0;
  uint32_t serialDropped = 0;
  uint32_t strayBytes = 0;
  uint32_t bootFails = 0;  // earlier starts that never reported healthy
  const char* updateReason = "none";  // why the bootloader last stayed in update mode
  uint32_t overruns = 0;    // blocks the audio interrupt never started before their half came round again
  uint32_t saiErrors = 0;   // SAI and DMA errors the HAL reported
  uint32_t restarts = 0;    // audio stream restarts after a fault
  uint32_t nanBlocks = 0;   // blocks discarded before the polish stages for a NaN or an infinity
  uint32_t powerTrips = 0;  // power-fail mutes, 5VA under its trip level
  bool stalled = false;       // the stall watch latched the mute
  bool audioLatched = false;  // a fault past the restart budget latched the mute

  explicit Status() = default;
};

void replyError(Writer& w, uint32_t id, const char* reason);
void replyOk(Writer& w, uint32_t id);

// Lists the build ("release" or "bench") and the ops it accepts. `serial` is the box's 24-character
// hex id and `settings` its current settings; the reply leaves either field out when it is null,
// which is how the host tools that have no board call it.
void replyHello(Writer& w, uint32_t id, const char* fw, const char* layoutName, const Layout& layout, bool benchBuild,
                const char* serial = nullptr, const Settings* settings = nullptr);
void replyLayout(Writer& w, uint32_t id, const char* layoutName, const Layout& layout, const Settings& settings);

// Also reports `reply_overflows`, the reply pieces `w` has lost to its format buffer since boot.
void replyStatus(Writer& w, uint32_t id, const Status& s);

}  // namespace protocol
