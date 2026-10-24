#include "core/protocol.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "core/registry.h"

namespace protocol {

namespace {

constexpr size_t kKeyBytes = 16;
constexpr size_t kCmdBytes = 16;
constexpr size_t kOpBytes = kOpNameBytes;
constexpr int kMaxSkipDepth = 4;
constexpr uint32_t kSlotClamp = 255;  // keeps a huge slot number out of int range; it still fails the check

struct CommandName {
  const char* text;
  Command cmd;
};

constexpr std::array<CommandName, 6> kCommands = {{
    {"hello", Command::Hello},       {"get_layout", Command::GetLayout}, {"set_layout", Command::SetLayout},
    {"status", Command::Status},     {"enter_dfu", Command::EnterDfu},   {"bench", Command::Bench},
}};

// A command name clipped to the buffer must not be able to equal a real one.
constexpr bool commandNamesFit() {
  for (const CommandName& c : kCommands) {
    if (__builtin_strlen(c.text) + 2 > kCmdBytes) return false;
  }
  return true;
}
static_assert(commandNamesFit(), "command names must be shorter than kCmdBytes");

// ---- reading -------------------------------------------------------------------------------

void skipWs(const char*& p) {
  while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') ++p;
}

bool eat(const char*& p, char c) {
  skipWs(p);
  if (*p != c) return false;
  ++p;
  return true;
}

// Destination for a decoded string. A null buffer discards the text.
struct StrOut {
  char* buf;
  size_t cap;
  size_t len = 0;
  bool clipped = false;

  void put(char c) {
    if (!buf) return;
    if (len + 1 < cap) {
      buf[len++] = c;
    } else {
      clipped = true;
    }
  }
  void end() {
    if (buf && cap > 0) buf[len] = '\0';
  }
};

bool readHex4(const char*& p, unsigned& v) {
  v = 0;
  for (int i = 0; i < 4; ++i) {
    const char c = *p;
    unsigned d;
    if (c >= '0' && c <= '9') d = static_cast<unsigned>(c - '0');
    else if (c >= 'a' && c <= 'f') d = static_cast<unsigned>(c - 'a' + 10);
    else if (c >= 'A' && c <= 'F') d = static_cast<unsigned>(c - 'A' + 10);
    else return false;
    v = v * 16 + d;
    ++p;
  }
  return true;
}

void putCodePoint(StrOut& out, unsigned cp) {
  if (cp >= 0xD800 && cp <= 0xDFFF) cp = '?';  // a lone surrogate half
  if (cp == 0) cp = 1;                          // a NUL would end the string early
  if (cp < 0x80) {
    out.put(static_cast<char>(cp));
  } else if (cp < 0x800) {
    out.put(static_cast<char>(0xC0 | (cp >> 6)));
    out.put(static_cast<char>(0x80 | (cp & 0x3F)));
  } else {
    out.put(static_cast<char>(0xE0 | (cp >> 12)));
    out.put(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.put(static_cast<char>(0x80 | (cp & 0x3F)));
  }
}

bool readEscape(const char*& p, StrOut& out) {
  const char e = *p++;
  switch (e) {
    case '"': case '\\': case '/': out.put(e); return true;
    case 'b': out.put('\b'); return true;
    case 'f': out.put('\f'); return true;
    case 'n': out.put('\n'); return true;
    case 'r': out.put('\r'); return true;
    case 't': out.put('\t'); return true;
    case 'u': {
      unsigned cp;
      if (!readHex4(p, cp)) return false;
      putCodePoint(out, cp);
      return true;
    }
    default: return false;
  }
}

bool readString(const char*& p, StrOut& out) {
  skipWs(p);
  if (*p != '"') return false;
  ++p;
  for (;;) {
    const unsigned char c = static_cast<unsigned char>(*p);
    if (c == 0 || c < 0x20) return false;
    ++p;
    if (c == '"') {
      out.end();
      return true;
    }
    if (c != '\\') {
      out.put(static_cast<char>(c));
    } else if (!readEscape(p, out)) {
      return false;
    }
  }
}

bool isDigit(char c) { return c >= '0' && c <= '9'; }

// Consumes digits; false when there is none.
bool skipDigits(const char*& q) {
  if (!isDigit(*q)) return false;
  while (isDigit(*q)) ++q;
  return true;
}

// The JSON number grammar: -? (0 | [1-9][0-9]*) (. [0-9]+)? ([eE] [+-]? [0-9]+)?
// On failure `p` is left where it was.
bool skipNumber(const char*& p) {
  const char* q = p;
  if (*q == '-') ++q;
  if (*q == '0') {
    ++q;  // no leading zeros: "01" stops after the 0
  } else if (!skipDigits(q)) {
    return false;
  }
  if (*q == '.') {
    ++q;
    if (!skipDigits(q)) return false;
  }
  if (*q == 'e' || *q == 'E') {
    ++q;
    if (*q == '+' || *q == '-') ++q;
    if (!skipDigits(q)) return false;
  }
  p = q;
  return true;
}

// A plain non-negative integer that fits in 32 bits: a JSON number with no sign, fraction or exponent.
bool readUint(const char*& p, uint32_t& v) {
  skipWs(p);
  const char* end = p;
  if (!skipNumber(end)) return false;
  uint64_t acc = 0;
  for (const char* d = p; d < end; ++d) {
    if (!isDigit(*d)) return false;
    acc = acc * 10 + static_cast<uint64_t>(*d - '0');
    if (acc > 0xFFFFFFFFull) return false;
  }
  p = end;
  v = static_cast<uint32_t>(acc);
  return true;
}

bool skipValue(const char*& p, int depth);

bool skipContainer(const char*& p, int depth, char close, bool pairs) {
  if (depth > kMaxSkipDepth) return false;
  if (eat(p, close)) return true;
  for (;;) {
    if (pairs) {
      StrOut none{nullptr, 0};
      if (!readString(p, none) || !eat(p, ':')) return false;
    }
    if (!skipValue(p, depth + 1)) return false;
    if (eat(p, ',')) continue;
    return eat(p, close);
  }
}

bool skipLiteral(const char*& p, const char* word) {
  const size_t n = strlen(word);
  if (strncmp(p, word, n) != 0) return false;
  p += n;
  return true;
}

bool skipValue(const char*& p, int depth) {
  skipWs(p);
  switch (*p) {
    case '"': {
      StrOut none{nullptr, 0};
      return readString(p, none);
    }
    case '{': ++p; return skipContainer(p, depth, '}', true);
    case '[': ++p; return skipContainer(p, depth, ']', false);
    case 't': return skipLiteral(p, "true");
    case 'f': return skipLiteral(p, "false");
    case 'n': return skipLiteral(p, "null");
    default: return skipNumber(p);
  }
}

// ---- request fields ------------------------------------------------------------------------

struct Fields {
  Request& req;
  char cmd[kCmdBytes] = {};
  char op[kOpBytes] = {};
  bool hasId = false;
  bool hasCmd = false;
  bool hasOp = false;
  bool hasSlot = false;
  bool hasName = false;
  bool hasSlots = false;
  bool nameClipped = false;
  const char* typeError = nullptr;  // first wrong-typed field
  char problemText[kErrorBytes] = {};  // backs a problem message built from constants

  explicit Fields(Request& r) : req(r) {}
  void typeFail(const char* msg) {
    if (!typeError) typeError = msg;
  }
};

bool parseId(const char*& p, Fields& f) {
  const char* start = p;
  uint32_t v;
  if (readUint(p, v)) {
    f.req.id = v;
    f.hasId = true;
    return true;
  }
  p = start;
  f.typeFail("id must be a non-negative integer");
  return skipValue(p, 0);
}

bool parseBenchSlot(const char*& p, Fields& f) {
  const char* start = p;
  uint32_t v;
  if (readUint(p, v)) {
    f.req.benchSlot = static_cast<int>(v < kSlotClamp ? v : kSlotClamp);
    f.hasSlot = true;
    return true;
  }
  p = start;
  f.typeFail("slot must be an integer");
  return skipValue(p, 0);
}

// Reads a string field into `out`, or notes the type error and skips the value.
bool parseText(const char*& p, Fields& f, StrOut& out, const char* typeMsg, bool& present) {
  skipWs(p);
  if (*p != '"') {
    f.typeFail(typeMsg);
    return skipValue(p, 0);
  }
  present = true;
  return readString(p, out);
}

bool parseSlots(const char*& p, Fields& f) {
  skipWs(p);
  if (*p != '[') {
    f.typeFail("slots must be an array of card ids");
    return skipValue(p, 0);
  }
  ++p;
  f.hasSlots = true;
  f.req.slotCount = 0;
  if (eat(p, ']')) return true;
  for (;;) {
    skipWs(p);
    const int i = f.req.slotCount++;
    if (*p != '"') {
      f.typeFail("slots must be an array of card ids");
      if (!skipValue(p, 1)) return false;
    } else {
      StrOut out{i < kSlotCount ? f.req.slot[i].data() : nullptr, kIdBytes};
      if (!readString(p, out)) return false;
    }
    if (eat(p, ',')) continue;
    return eat(p, ']');
  }
}

bool parseField(const char*& p, const char* key, Fields& f) {
  if (strcmp(key, "id") == 0) return parseId(p, f);
  if (strcmp(key, "cmd") == 0) {
    StrOut out{f.cmd, sizeof f.cmd};
    return parseText(p, f, out, "cmd must be a string", f.hasCmd);
  }
  if (strcmp(key, "op") == 0) {
    StrOut out{f.op, sizeof f.op};
    return parseText(p, f, out, "op must be a string", f.hasOp);
  }
  if (strcmp(key, "slot") == 0) return parseBenchSlot(p, f);
  if (strcmp(key, "name") == 0) {
    StrOut out{f.req.name, kNameBytes};  // one byte under the buffer: 31 characters fit
    const bool ok = parseText(p, f, out, "name must be a string", f.hasName);
    f.nameClipped = out.clipped;
    return ok;
  }
  if (strcmp(key, "slots") == 0) return parseSlots(p, f);
  return skipValue(p, 0);
}

bool parseObject(const char*& p, Fields& f) {
  if (!eat(p, '{')) return false;
  if (!eat(p, '}')) {
    for (;;) {
      char key[kKeyBytes];
      StrOut k{key, sizeof key};
      if (!readString(p, k) || !eat(p, ':') || !parseField(p, key, f)) return false;
      if (eat(p, ',')) continue;
      if (!eat(p, '}')) return false;
      break;
    }
  }
  skipWs(p);
  return *p == '\0';
}

// True for 1 or more characters, all printable ASCII.
bool printableName(const char* s) {
  if (*s == '\0') return false;
  for (; *s; ++s) {
    const unsigned char c = static_cast<unsigned char>(*s);
    if (c < 0x20 || c > 0x7E) return false;
  }
  return true;
}

// The reason a request lacks what every command needs, or null.
const char* commonProblem(const Fields& f) {
  if (f.typeError) return f.typeError;
  if (!f.hasId) return "missing id";
  if (!f.hasCmd) return "missing cmd";
  return nullptr;
}

// The reason a set_layout request lacks its own fields, or null.
const char* setLayoutProblem(Fields& f) {
  if (!f.hasName) return "missing name";
  if (f.nameClipped || !printableName(f.req.name)) {
    snprintf(f.problemText, sizeof f.problemText, "name must be 1 to %d printable ASCII characters",
             static_cast<int>(kNameBytes) - 1);
    return f.problemText;
  }
  if (!f.hasSlots) return "missing slots";
  if (f.req.slotCount != kSlotCount) {
    snprintf(f.problemText, sizeof f.problemText, "slots needs %d card ids", kSlotCount);
    return f.problemText;
  }
  return nullptr;
}

// Resolves the bench op and checks the build carries it and slot_off has its target. On failure
// writes the reason to `err`.
bool resolveBenchOp(Fields& f, bool benchBuild, char* err, size_t errSize) {
  if (!f.hasOp) {
    snprintf(err, errSize, "missing op");
    return false;
  }
  if (!opFromName(f.op, f.req.op)) {
    snprintf(err, errSize, "unknown op: %s", f.op);
    return false;
  }
  if (!opAllowed(f.req.op, benchBuild)) {
    snprintf(err, errSize, "op not in this build: %s", f.op);
    return false;
  }
  if (f.req.op != BenchOp::SlotOff) return true;
  if (!f.hasSlot) {
    snprintf(err, errSize, "missing slot");
    return false;
  }
  if (f.req.benchSlot < kMinBenchSlot || f.req.benchSlot > kMaxBenchSlot) {
    snprintf(err, errSize, "slot must be %d to %d", kMinBenchSlot, kMaxBenchSlot);
    return false;
  }
  return true;
}

bool findCommand(const char* text, Command& out) {
  for (const CommandName& c : kCommands) {
    if (strcmp(text, c.text) == 0) {
      out = c.cmd;
      return true;
    }
  }
  return false;
}

// ---- writing -------------------------------------------------------------------------------

// Quoted JSON string. Quote, backslash, control and non-ASCII bytes are escaped, so any bytes
// (a corrupt record name, say) still give valid JSON.
void writeString(Writer& w, const char* s) {
  w.write("\"", 1);
  const char* run = s;
  for (; *s; ++s) {
    const unsigned char c = static_cast<unsigned char>(*s);
    if (c >= 0x20 && c < 0x7F && c != '"' && c != '\\') continue;
    if (s > run) w.write(run, static_cast<size_t>(s - run));
    if (c == '"') {
      w.write("\\\"", 2);
    } else if (c == '\\') {
      w.write("\\\\", 2);
    } else {
      w.format("\\u%04x", static_cast<unsigned>(c));
    }
    run = s + 1;
  }
  if (s > run) w.write(run, static_cast<size_t>(s - run));
  w.write("\"", 1);
}

// Up to two decimals, trailing zeros dropped; no float printf needed.
void formatNumber(float v, char* buf, size_t size) {
  const long scaled = lroundf(fabsf(v) * 100.0f);
  const char* sign = v < 0.0f && scaled > 0 ? "-" : "";
  const long whole = scaled / 100;
  const int frac = static_cast<int>(scaled % 100);
  if (frac == 0) {
    snprintf(buf, size, "%s%ld", sign, whole);
  } else if (frac % 10 == 0) {
    snprintf(buf, size, "%s%ld.%d", sign, whole, frac / 10);
  } else {
    snprintf(buf, size, "%s%ld.%02d", sign, whole, frac);
  }
}

void writeLayout(Writer& w, const char* name, const Layout& layout) {
  w.write("{\"name\":", 8);
  writeString(w, name);
  w.write(",\"slots\":[", 10);
  for (int s = 0; s < kSlotCount; ++s) {
    if (s > 0) w.write(",", 1);
    writeString(w, cardDesc(layout.card[s]).id);
  }
  w.write("]}", 2);
}

const char* groupName(Group g) {
  switch (g) {
    case Group::None: return "none";
    case Group::Pitch: return "pitch";
    case Group::Reverb: return "reverb";
  }
  return "none";
}

const char* curveName(Curve c) {
  switch (c) {
    case Curve::Linear: return "linear";
    case Curve::Log: return "log";
    case Curve::LogReversed: return "log_reversed";
  }
  return "linear";
}

void writeKnob(Writer& w, const KnobDesc& k) {
  char lo[16];
  char hi[16];
  formatNumber(k.min, lo, sizeof lo);
  formatNumber(k.max, hi, sizeof hi);
  w.write("{\"name\":", 8);
  writeString(w, k.name);
  w.format(",\"kind\":\"%s\",\"positions\":%d,\"min\":%s,\"max\":%s,\"unit\":",
           k.kind == KnobKind::Stepped ? "stepped" : "continuous", k.positions, lo, hi);
  writeString(w, k.unit);
  w.format(",\"curve\":\"%s\"}", curveName(k.curve));
}

void writeCard(Writer& w, const CardDesc& d) {
  w.write("{\"id\":", 6);
  writeString(w, d.id);
  w.write(",\"name\":", 8);
  writeString(w, d.name);
  w.format(",\"group\":\"%s\",\"cost\":{\"avg\":%d,\"worst\":%d,\"measured\":%s},\"knobs\":[", groupName(d.group),
           d.cost.avg, d.cost.worst, d.cost.measured ? "true" : "false");
  for (int k = 0; k < d.knobCount; ++k) {
    if (k > 0) w.write(",", 1);
    writeKnob(w, d.knob[k]);
  }
  w.write("]}", 2);
}

}  // namespace

bool Writer::format(const char* fmt, ...) {
  std::array<char, kFormatBytes> buf;
  va_list ap;
  va_start(ap, fmt);
  const int n = vsnprintf(buf.data(), buf.size(), fmt, ap);
  va_end(ap);
  if (n < 0 || static_cast<size_t>(n) >= buf.size()) {
    ++overflows_;
    return false;
  }
  if (n > 0) write(buf.data(), static_cast<size_t>(n));
  return true;
}

Framer::Kind Framer::feed(uint8_t byte) {
  if (dropping_) {
    if (byte == '\n') dropping_ = false;
    return Kind::None;
  }
  if (!inLine_) {
    if (byte != '{') {
      ++stray_;
      return Kind::None;
    }
    inLine_ = true;
    len_ = 0;
  }
  if (byte == '\0') {
    inLine_ = false;
    dropping_ = true;
    return Kind::BadByte;
  }
  if (byte == '\n') {
    line_[len_] = '\0';
    inLine_ = false;
    return Kind::Line;
  }
  if (len_ >= kMaxLine) {
    inLine_ = false;
    dropping_ = true;
    return Kind::TooLong;
  }
  line_[len_++] = static_cast<char>(byte);
  return Kind::None;
}

bool parseRequest(const char* line, Request& req, char* err, size_t errSize, bool benchBuild) {
  req = Request();
  Fields f(req);
  const char* p = line;
  if (!parseObject(p, f)) {
    snprintf(err, errSize, "malformed json");
    return false;
  }
  const char* problem = commonProblem(f);
  if (!problem && !findCommand(f.cmd, req.cmd)) {
    snprintf(err, errSize, "unknown cmd \"%s\"", f.cmd);
    return false;
  }
  if (!problem && req.cmd == Command::SetLayout) problem = setLayoutProblem(f);
  if (!problem && req.cmd == Command::Bench) return resolveBenchOp(f, benchBuild, err, errSize);
  if (problem) {
    snprintf(err, errSize, "%s", problem);
    return false;
  }
  return true;
}

bool resolveLayout(const Request& req, Layout& out, char* err, size_t errSize) {
  for (int s = 0; s < kSlotCount; ++s) {
    if (!cardFromId(req.slot[s].data(), out.card[s])) {
      snprintf(err, errSize, "unknown card \"%s\" in slot %d", req.slot[s].data(), s + 1);
      return false;
    }
  }
  const LayoutCheck check = layoutValid(out);
  if (check.ok()) return true;
  layoutReason(check, err, errSize);
  return false;
}

void replyError(Writer& w, uint32_t id, const char* reason) {
  w.format("{\"id\":%lu,\"ok\":false,\"error\":", static_cast<unsigned long>(id));
  writeString(w, reason);
  w.write("}\n", 2);
}

void replyOk(Writer& w, uint32_t id) { w.format("{\"id\":%lu,\"ok\":true}\n", static_cast<unsigned long>(id)); }

namespace {

void writeBenchOps(Writer& w, bool benchBuild) {
  w.write("[", 1);
  bool first = true;
  for (int i = 0; i < kBenchOpCount; ++i) {
    const BenchOp op = benchOp(i);
    if (!opAllowed(op, benchBuild)) continue;
    if (!first) w.write(",", 1);
    first = false;
    writeString(w, opName(op));
  }
  w.write("]", 1);
}

}  // namespace

void replyHello(Writer& w, uint32_t id, const char* fw, const char* layoutName, const Layout& layout, bool benchBuild,
                const char* serial) {
  w.format("{\"id\":%lu,\"ok\":true,\"proto\":%d,\"fw\":", static_cast<unsigned long>(id), kVersion);
  writeString(w, fw);
  if (serial) {
    w.write(",\"serial\":", 10);
    writeString(w, serial);
  }
  w.format(",\"build\":\"%s\",\"bench_ops\":", benchBuild ? "bench" : "release");
  writeBenchOps(w, benchBuild);
  w.write(",\"cards\":[", 10);
  for (int c = 0; c < kCardCount; ++c) {
    if (c > 0) w.write(",", 1);
    writeCard(w, cardDesc(static_cast<Card>(c)));
  }
  w.write("],\"layout\":", 11);
  writeLayout(w, layoutName, layout);
  w.write("}\n", 2);
}

void replyLayout(Writer& w, uint32_t id, const char* layoutName, const Layout& layout) {
  w.format("{\"id\":%lu,\"ok\":true,\"layout\":", static_cast<unsigned long>(id));
  writeLayout(w, layoutName, layout);
  w.write("}\n", 2);
}

void replyStatus(Writer& w, uint32_t id, const Status& s) {
  char mean[16];
  char worst[16];
  formatNumber(s.cpuMeanPercent, mean, sizeof mean);
  formatNumber(s.cpuWorstPercent, worst, sizeof worst);
  w.format("{\"id\":%lu,\"ok\":true,\"cpu_mean\":%s,\"cpu_worst\":%s,\"late\":%lu,\"muted\":%s,\"seq\":%lu,",
           static_cast<unsigned long>(id), mean, worst, static_cast<unsigned long>(s.late),
           s.muted ? "true" : "false", static_cast<unsigned long>(s.seq));
  w.format("\"rec_errors\":%lu,\"serial_dropped\":%lu,\"stray_bytes\":%lu,\"boot_fails\":%lu,\"update_reason\":",
           static_cast<unsigned long>(s.recordErrors), static_cast<unsigned long>(s.serialDropped),
           static_cast<unsigned long>(s.strayBytes), static_cast<unsigned long>(s.bootFails));
  writeString(w, s.updateReason);
  w.write("}\n", 2);
}

}  // namespace protocol
