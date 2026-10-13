// Serial protocol: line framing, request parsing and reply formatting.
// One flat JSON object per line, hand-written (spec 2026-10-12-serial-protocol-design.md).

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "core/layout.h"
#include "core/record.h"

namespace protocol {

constexpr int kVersion = 1;
constexpr size_t kMaxLine = 768;
constexpr size_t kIdBytes = 24;  // a longer card id is clipped; no real id comes near it
constexpr size_t kNameBytes = kRecordNameBytes;  // storage; a name holds 1 to kNameBytes - 1 printable ASCII characters
constexpr size_t kErrorBytes = 96;

// Sink for reply text. `hello` streams through it in pieces.
class Writer {
 public:
  virtual ~Writer() = default;
  virtual void write(const char* text, size_t len) = 0;
};

// Splits the byte stream into JSON lines and bench letters. A `{` outside a line starts one, and it
// runs to the next '\n'. Any other byte outside a line is a bench letter.
class Framer {
 public:
  enum class Kind : uint8_t { None, Bench, Line, TooLong };

  // `Line` leaves the NUL-terminated text in line(). `TooLong` fires once; the rest of the line is dropped.
  Kind feed(uint8_t byte);
  const char* line() const { return line_; }
  char bench() const { return bench_; }

  // Forgets a half-received line, for when a new host session starts.
  void reset() {
    len_ = 0;
    inLine_ = false;
    dropping_ = false;
  }

 private:
  char line_[kMaxLine + 1] = {};
  size_t len_ = 0;
  bool inLine_ = false;
  bool dropping_ = false;
  char bench_ = 0;
};

enum class Command : uint8_t { Hello, GetLayout, SetLayout, Status, EnterDfu };

struct Request {
  uint32_t id = 0;
  Command cmd = Command::Hello;
  char name[kNameBytes + 1] = {};
  int slotCount = 0;  // ids present in "slots"
  char slot[kSlotCount][kIdBytes] = {};
};

// Parses and checks one request. On failure returns false with a readable reason in `err`; `req.id`
// holds the request's id when the parser got far enough to read it, otherwise 0.
bool parseRequest(const char* line, Request& req, char* err, size_t errSize);

// Maps the request's card ids onto a layout and checks it with layoutValid.
bool resolveLayout(const Request& req, Layout& out, char* err, size_t errSize);

struct Status {
  float cpuMeanPercent;
  float cpuWorstPercent;
  uint32_t late;
  bool muted;
  uint32_t seq;
  uint32_t recordErrors;
  uint32_t serialDropped;
};

void replyError(Writer& w, uint32_t id, const char* reason);
void replyOk(Writer& w, uint32_t id);
void replyHello(Writer& w, uint32_t id, const char* fw, const char* layoutName, const Layout& layout);
void replyLayout(Writer& w, uint32_t id, const char* layoutName, const Layout& layout);
void replyStatus(Writer& w, uint32_t id, const Status& s);

}  // namespace protocol
