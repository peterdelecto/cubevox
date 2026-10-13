#include "core/protocol.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

namespace protocol {

namespace {

constexpr size_t kKeyBytes = 16;
constexpr size_t kCmdBytes = 16;
constexpr int kMaxSkipDepth = 4;

struct CommandName {
  const char* text;
  Command cmd;
};

constexpr CommandName kCommands[] = {
    {"hello", Command::Hello},       {"get_layout", Command::GetLayout}, {"set_layout", Command::SetLayout},
    {"status", Command::Status},     {"enter_dfu", Command::EnterDfu},
};

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

bool isNumberChar(char c) { return (c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.' || c == 'e' || c == 'E'; }

bool skipNumber(const char*& p) {
  if (!isNumberChar(*p)) return false;
  while (isNumberChar(*p)) ++p;
  return true;
}

// A plain non-negative integer that fits in 32 bits, with nothing numeric after it.
bool readUint(const char*& p, uint32_t& v) {
  skipWs(p);
  uint64_t acc = 0;
  int digits = 0;
  const char* q = p;
  while (*q >= '0' && *q <= '9') {
    acc = acc * 10 + static_cast<uint64_t>(*q - '0');
    if (++digits > 10 || acc > 0xFFFFFFFFull) return false;
    ++q;
  }
  if (digits == 0 || isNumberChar(*q)) return false;
  p = q;
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
  bool hasId = false;
  bool hasCmd = false;
  bool hasName = false;
  bool hasSlots = false;
  bool nameClipped = false;
  const char* typeError = nullptr;  // first wrong-typed field

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
      StrOut out{i < kSlotCount ? f.req.slot[i] : nullptr, kIdBytes};
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
const char* setLayoutProblem(const Fields& f) {
  if (!f.hasName) return "missing name";
  if (f.nameClipped || !printableName(f.req.name)) return "name must be 1 to 31 printable ASCII characters";
  if (!f.hasSlots) return "missing slots";
  if (f.req.slotCount != kSlotCount) return "slots needs 8 card ids";
  return nullptr;
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

void emit(Writer& w, const char* fmt, ...) __attribute__((format(printf, 2, 3)));

void emit(Writer& w, const char* fmt, ...) {
  char buf[160];
  va_list ap;
  va_start(ap, fmt);
  const int n = vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  if (n <= 0) return;
  w.write(buf, static_cast<size_t>(n) < sizeof buf ? static_cast<size_t>(n) : sizeof buf - 1);
}

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
      emit(w, "\\u%04x", static_cast<unsigned>(c));
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
  emit(w, ",\"kind\":\"%s\",\"positions\":%d,\"min\":%s,\"max\":%s,\"unit\":",
       k.kind == KnobKind::Stepped ? "stepped" : "continuous", k.positions, lo, hi);
  writeString(w, k.unit);
  emit(w, ",\"curve\":\"%s\"}", curveName(k.curve));
}

void writeCard(Writer& w, const CardDesc& d) {
  w.write("{\"id\":", 6);
  writeString(w, d.id);
  w.write(",\"name\":", 8);
  writeString(w, d.name);
  emit(w, ",\"group\":\"%s\",\"cost\":{\"avg\":%d,\"worst\":%d,\"measured\":%s},\"knobs\":[", groupName(d.group),
       d.cost.avg, d.cost.worst, d.cost.measured ? "true" : "false");
  for (int k = 0; k < d.knobCount; ++k) {
    if (k > 0) w.write(",", 1);
    writeKnob(w, d.knob[k]);
  }
  w.write("]}", 2);
}

}  // namespace

Framer::Kind Framer::feed(uint8_t byte) {
  if (dropping_) {
    if (byte == '\n') dropping_ = false;
    return Kind::None;
  }
  if (!inLine_) {
    if (byte != '{') {
      bench_ = static_cast<char>(byte);
      return Kind::Bench;
    }
    inLine_ = true;
    len_ = 0;
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

bool parseRequest(const char* line, Request& req, char* err, size_t errSize) {
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
  if (problem) {
    snprintf(err, errSize, "%s", problem);
    return false;
  }
  return true;
}

bool resolveLayout(const Request& req, Layout& out, char* err, size_t errSize) {
  for (int s = 0; s < kSlotCount; ++s) {
    int found = -1;
    for (int c = 0; c < kCardCount && found < 0; ++c) {
      if (strcmp(req.slot[s], cardDesc(static_cast<Card>(c)).id) == 0) found = c;
    }
    if (found < 0) {
      snprintf(err, errSize, "unknown card \"%s\" in slot %d", req.slot[s], s + 1);
      return false;
    }
    out.card[s] = static_cast<Card>(found);
  }
  const LayoutCheck check = layoutValid(out);
  if (check.ok()) return true;
  layoutReason(check, err, errSize);
  return false;
}

void replyError(Writer& w, uint32_t id, const char* reason) {
  emit(w, "{\"id\":%lu,\"ok\":false,\"error\":", static_cast<unsigned long>(id));
  writeString(w, reason);
  w.write("}\n", 2);
}

void replyOk(Writer& w, uint32_t id) { emit(w, "{\"id\":%lu,\"ok\":true}\n", static_cast<unsigned long>(id)); }

void replyHello(Writer& w, uint32_t id, const char* fw, const char* layoutName, const Layout& layout) {
  emit(w, "{\"id\":%lu,\"ok\":true,\"proto\":%d,\"fw\":", static_cast<unsigned long>(id), kVersion);
  writeString(w, fw);
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
  emit(w, "{\"id\":%lu,\"ok\":true,\"layout\":", static_cast<unsigned long>(id));
  writeLayout(w, layoutName, layout);
  w.write("}\n", 2);
}

void replyStatus(Writer& w, uint32_t id, const Status& s) {
  char mean[16];
  char worst[16];
  formatNumber(s.cpuMeanPercent, mean, sizeof mean);
  formatNumber(s.cpuWorstPercent, worst, sizeof worst);
  emit(w, "{\"id\":%lu,\"ok\":true,\"cpu_mean\":%s,\"cpu_worst\":%s,\"late\":%lu,\"muted\":%s,\"seq\":%lu,",
       static_cast<unsigned long>(id), mean, worst, static_cast<unsigned long>(s.late),
       s.muted ? "true" : "false", static_cast<unsigned long>(s.seq));
  emit(w, "\"rec_errors\":%lu,\"serial_dropped\":%lu}\n", static_cast<unsigned long>(s.recordErrors),
       static_cast<unsigned long>(s.serialDropped));
}

}  // namespace protocol
