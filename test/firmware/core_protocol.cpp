// Checks the USB JSON protocol on the host. Covers request parsing and its errors, reply
// formatting, the line framer and the bench ops.

#include "support.h"

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

#include "core/bench_ops.h"
#include "core/layout.h"
#include "core/protocol.h"
#include "core/registry.h"

namespace {

// ---- serial protocol -----------------------------------------------------------------------

class StringWriter : public protocol::Writer {
 public:
  void write(const char* text, size_t len) override {
    out.append(text, len);
    ++calls;
  }
  std::string out;
  int calls = 0;
};

// Strict JSON reader: true when `s` is one whole valid value. Enough to prove the replies parse.
class JsonReader {
 public:
  explicit JsonReader(const std::string& s) : s_(s) {}
  bool valid() {
    ws();
    return value(0) && (ws(), i_ == s_.size() || (s_[i_] == '\n' && i_ + 1 == s_.size()));
  }

 private:
  void ws() { while (i_ < s_.size() && (s_[i_] == ' ' || s_[i_] == '\t' || s_[i_] == '\r')) ++i_; }
  bool lit(const char* w) {
    const size_t n = std::strlen(w);
    if (s_.compare(i_, n, w) != 0) return false;
    i_ += n;
    return true;
  }
  bool str() {
    if (i_ >= s_.size() || s_[i_] != '"') return false;
    for (++i_; i_ < s_.size(); ++i_) {
      const unsigned char c = static_cast<unsigned char>(s_[i_]);
      if (c == '"') { ++i_; return true; }
      if (c < 0x20) return false;
      if (c == '\\') {
        ++i_;
        if (i_ >= s_.size() || !std::strchr("\"\\/bfnrtu", s_[i_])) return false;
        if (s_[i_] == 'u') {
          for (int k = 0; k < 4; ++k) {
            if (++i_ >= s_.size() || !std::isxdigit(static_cast<unsigned char>(s_[i_]))) return false;
          }
        }
      }
    }
    return false;
  }
  bool num() {
    const size_t start = i_;
    while (i_ < s_.size() && std::strchr("+-0123456789.eE", s_[i_])) ++i_;
    return i_ > start;
  }
  bool seq(char close, bool pairs, int depth) {
    ++i_;
    ws();
    if (i_ < s_.size() && s_[i_] == close) { ++i_; return true; }
    for (;;) {
      ws();
      if (pairs && (!str() || (ws(), i_ >= s_.size() || s_[i_++] != ':'))) return false;
      if (!value(depth + 1)) return false;
      ws();
      if (i_ < s_.size() && s_[i_] == ',') { ++i_; continue; }
      if (i_ < s_.size() && s_[i_] == close) { ++i_; return true; }
      return false;
    }
  }
  bool value(int depth) {
    ws();
    if (depth > 8 || i_ >= s_.size()) return false;
    switch (s_[i_]) {
      case '{': return seq('}', true, depth);
      case '[': return seq(']', false, depth);
      case '"': return str();
      case 't': return lit("true");
      case 'f': return lit("false");
      case 'n': return lit("null");
      default: return num();
    }
  }
  const std::string& s_;
  size_t i_ = 0;
};

bool has(const std::string& hay, const std::string& needle) { return hay.find(needle) != std::string::npos; }

bool parses(const char* line, protocol::Request& req, std::string& err, bool benchBuild = true) {
  char buf[protocol::kErrorBytes];
  const bool ok = protocol::parseRequest(line, req, buf, sizeof buf, benchBuild);
  err = ok ? "" : buf;
  return ok;
}

void checkProtocolCommands() {
  protocol::Request r;
  std::string err;
  check(parses("{\"id\":7,\"cmd\":\"hello\"}", r, err) && r.id == 7 && r.cmd == protocol::Command::Hello, "proto: hello parses");
  check(parses("{\"cmd\":\"get_layout\",\"id\":8}", r, err) && r.id == 8 && r.cmd == protocol::Command::GetLayout,
        "proto: key order is free");
  check(parses(" { \"id\" : 9 , \"cmd\" : \"status\" } ", r, err) && r.cmd == protocol::Command::Status, "proto: whitespace is free");
  check(parses("{\"id\":10,\"cmd\":\"enter_dfu\"}", r, err) && r.cmd == protocol::Command::EnterDfu, "proto: enter_dfu parses");
  check(parses("{\"id\":4294967295,\"cmd\":\"hello\"}", r, err) && r.id == 4294967295u, "proto: largest id parses");
  check(parses("{\"id\":1,\"cmd\":\"hello\",\"x\":[1,{\"a\":[true,null,\"s\\\"\"]},-2.5e3],\"y\":{}}", r, err),
        "proto: unknown keys with nested values are skipped");

  const char* set =
      "{\"id\":3,\"cmd\":\"set_layout\",\"name\":\"Stage \\u0041\\\"x\",\"slots\":[\"input_gate\",\"autotune\","
      "\"octave\",\"unison\",\"slapback\",\"distortion\",\"gate\",\"reverb_chasm\"]}";
  check(parses(set, r, err) && r.cmd == protocol::Command::SetLayout && r.slotCount == kSlotCount, "proto: set_layout parses");
  check(std::strcmp(r.name, "Stage A\"x") == 0, "proto: escapes in the name decode");
  Layout l;
  check(protocol::resolveLayout(r, l, nullptr, 0) && l.card[7] == Card::ReverbChasm && l.card[0] == Card::InputGate,
        "proto: set_layout resolves to cards");
}

void checkRequestErrors() {
  protocol::Request r;
  std::string err;
  check(!parses("{\"id\":1,\"cmd\":\"hello\"", r, err) && err == "malformed json", "proto: unterminated object");
  check(!parses("not json", r, err) && err == "malformed json", "proto: not json");
  check(!parses("{\"id\":1,\"cmd\":\"hello\"} x", r, err) && err == "malformed json", "proto: trailing text");
  check(!parses("{\"id\":1,}", r, err) && err == "malformed json", "proto: trailing comma");
  check(!parses("{\"id\":5,\"cmd\":\"hel", r, err) && r.id == 5, "proto: a cut-off line still echoes the id it read");
  check(!parses("{\"cmd\":\"hello\"}", r, err) && err == "missing id" && r.id == 0, "proto: missing id");
  check(!parses("{\"id\":\"1\",\"cmd\":\"hello\"}", r, err) && err == "id must be a non-negative integer", "proto: string id");
  check(!parses("{\"id\":-1,\"cmd\":\"hello\"}", r, err) && err == "id must be a non-negative integer", "proto: negative id");
  check(!parses("{\"id\":1.5,\"cmd\":\"hello\"}", r, err) && err == "id must be a non-negative integer", "proto: fractional id");
  check(!parses("{\"id\":4294967296,\"cmd\":\"hello\"}", r, err) && err == "id must be a non-negative integer", "proto: id over 32 bits");
  check(!parses("{\"id\":2}", r, err) && err == "missing cmd" && r.id == 2, "proto: missing cmd echoes the id");
  check(!parses("{\"id\":2,\"cmd\":4}", r, err) && err == "cmd must be a string", "proto: numeric cmd");
  check(!parses("{\"id\":2,\"cmd\":\"fly\"}", r, err) && err == "unknown cmd \"fly\"" && r.id == 2, "proto: unknown cmd");
}

void checkJsonNumbers() {
  protocol::Request r;
  std::string err;
  for (const char* good : {"0", "-0", "7", "-12", "0.5", "10.25", "1e5", "1E5", "2e+3", "-4.5e-2", "0e0"}) {
    const std::string line = std::string("{\"id\":1,\"cmd\":\"hello\",\"x\":") + good + "}";
    check(parses(line.c_str(), r, err), good);
  }
  for (const char* bad : {"01", "-01", "00", "1.", ".5", "+1", "-", "1e", "1e+", "--1", "1.2.3", "0x10", "1e5.5"}) {
    const std::string line = std::string("{\"id\":1,\"cmd\":\"hello\",\"x\":") + bad + "}";
    check(!parses(line.c_str(), r, err) && err == "malformed json", bad);
  }
  check(parses("{\"id\":0,\"cmd\":\"hello\"}", r, err) && r.id == 0, "proto: id 0 parses");
  check(!parses("{\"id\":01,\"cmd\":\"hello\"}", r, err) && err == "malformed json", "proto: an id with a leading zero is malformed");
  check(!parses("{\"id\":1e2,\"cmd\":\"hello\"}", r, err) && err == "id must be a non-negative integer", "proto: an exponent id is not an integer");
}

void checkSetLayoutNameRules() {
  protocol::Request r;
  std::string err;
  check(!parses("{\"id\":2,\"cmd\":\"set_layout\",\"slots\":[]}", r, err) && err == "missing name", "proto: set_layout needs a name");
  check(!parses("{\"id\":2,\"cmd\":\"set_layout\",\"name\":\"a\"}", r, err) && err == "missing slots", "proto: set_layout needs slots");
  check(!parses("{\"id\":2,\"cmd\":\"set_layout\",\"name\":3,\"slots\":[]}", r, err) && err == "name must be a string",
        "proto: numeric name");
  const std::string nameRule = "name must be 1 to " + std::to_string(kLayoutNameBytes - 1) + " printable ASCII characters";
  const std::string head = "{\"id\":2,\"cmd\":\"set_layout\",\"name\":\"";
  const std::string tailSlots = "\",\"slots\":[]}";
  const std::string slotsRule = "slots needs " + std::to_string(kSlotCount) + " card ids";
  check(!parses((head + "0123456789012345678901234567890" + "1" + tailSlots).c_str(), r, err) && err == nameRule,
        "proto: name of 32 characters is rejected");
  check(!parses((head + "0123456789012345678901234567890" + tailSlots).c_str(), r, err) && err == slotsRule,
        "proto: name of 31 characters passes the name rule");
  check(!parses((head + tailSlots).c_str(), r, err) && err == nameRule, "proto: empty name is rejected");
  check(!parses((head + "a\\nb" + tailSlots).c_str(), r, err) && err == nameRule, "proto: control character in the name");
  check(!parses((head + "caf\\u00e9" + tailSlots).c_str(), r, err) && err == nameRule, "proto: non-ASCII name is rejected");
  check(!parses((head + "caf\xC3\xA9" + tailSlots).c_str(), r, err) && err == nameRule, "proto: raw UTF-8 name is rejected");
  check(nameRule == "name must be 1 to 31 printable ASCII characters" && slotsRule == "slots needs 8 card ids",
        "proto: the messages follow the constants");
}

void checkSetLayoutSlotRules() {
  protocol::Request r;
  std::string err;
  const std::string slotsRule = "slots needs " + std::to_string(kSlotCount) + " card ids";
  check(!parses("{\"id\":2,\"cmd\":\"set_layout\",\"name\":\"a\",\"slots\":\"x\"}", r, err) &&
            err == "slots must be an array of card ids", "proto: slots not an array");
  check(!parses("{\"id\":2,\"cmd\":\"set_layout\",\"name\":\"a\",\"slots\":[1,2,3,4,5,6,7,8]}", r, err) &&
            err == "slots must be an array of card ids", "proto: slot ids must be strings");
  check(!parses("{\"id\":2,\"cmd\":\"set_layout\",\"name\":\"a\",\"slots\":[\"gate\"]}", r, err) && err == slotsRule,
        "proto: too few slots");
  check(!parses("{\"id\":2,\"cmd\":\"set_layout\",\"name\":\"a\",\"slots\":[\"a\",\"a\",\"a\",\"a\",\"a\",\"a\",\"a\",\"a\",\"a\"]}",
                r, err) && err == slotsRule, "proto: too many slots");
}

// Resolves a set_layout line made from `ids`; returns the resolveLayout result and its message.
bool resolveIds(const char* ids, Layout& l, char* why, size_t whySize) {
  std::string line = std::string("{\"id\":1,\"cmd\":\"set_layout\",\"name\":\"n\",\"slots\":[") + ids + "]}";
  protocol::Request q;
  char e[protocol::kErrorBytes];
  check(protocol::parseRequest(line.c_str(), q, e, sizeof e, true), "proto: layout line parses");
  why[0] = '\0';
  return protocol::resolveLayout(q, l, why, whySize);
}

void checkResolveErrors() {
  Layout l;
  char why[protocol::kErrorBytes];
  const char* tail = "\"gate\",\"slapback\",\"unison\",\"octave\",\"autotune\",\"distortion\"";
  check(!resolveIds((std::string("\"input_gate\",\"foo\",") + tail).c_str(), l, why, sizeof why) &&
            std::strcmp(why, "unknown card \"foo\" in slot 2") == 0, "proto: unknown card names the id and slot");
  check(!resolveIds((std::string("\"reverb_spring\",\"reverb_chasm\",") + tail).c_str(), l, why, sizeof why) &&
            std::strcmp(why, "two reverb cards") == 0, "proto: two reverbs");
  check(!resolveIds((std::string("\"slapback\",\"input_gate\",") + tail).c_str(), l, why, sizeof why) &&
            std::strcmp(why, "same card twice: slapback") == 0, "proto: duplicate card");
  check(resolveIds("\"empty\",\"empty\",\"empty\",\"empty\",\"empty\",\"empty\",\"empty\",\"empty\"", l, why, sizeof why) &&
            l.card[3] == Card::Empty, "proto: all-empty layout is accepted");
}

// An id longer than the clip width is cut short, and the cut text never matches a real card.
void checkIdClipping() {
  Layout l;
  char why[protocol::kErrorBytes];
  const std::string tail = "\"gate\",\"slapback\",\"unison\",\"octave\",\"autotune\",\"distortion\",\"input_gate\"";
  const std::string longId(40, 'x');
  check(!resolveIds(("\"" + longId + "\"," + tail).c_str(), l, why, sizeof why) &&
            std::string(why) == "unknown card \"" + longId.substr(0, protocol::kIdBytes - 1) + "\" in slot 1",
        "proto: an over-long id is clipped in the unknown-card reply");
  check(!resolveIds(("\"reverb_spring_and_then_some_more\"," + tail).c_str(), l, why, sizeof why) &&
            std::strncmp(why, "unknown card \"", 14) == 0, "proto: a real id with a long tail is not a match");
  check(protocol::kIdBytes == kCardIdBytes, "proto: the id buffer is the card id width");
}

void checkProtocolErrors() {
  checkRequestErrors();
  checkJsonNumbers();
  checkSetLayoutNameRules();
  checkSetLayoutSlotRules();
  checkResolveErrors();
  checkIdClipping();
}

// A set_layout request with `extra` (a comma and more fields, or nothing) before the closing brace.
std::string setLayoutWith(const std::string& extra) {
  return std::string("{\"id\":5,\"cmd\":\"set_layout\",\"name\":\"n\",\"slots\":[\"input_gate\",\"autotune\",\"octave\","
                     "\"unison\",\"slapback\",\"distortion\",\"gate\",\"reverb_spring\"]") + extra + "}";
}

bool settingsAre(const Settings& s, bool comp, bool eq, TopSwitch top) {
  return s.polishComp == comp && s.polishEq == eq && s.topSwitch == top;
}

// The settings after a set_layout request carrying `extra` is applied to `start`. `given` gets the
// request's settingsGiven bits.
Settings appliedTo(const Settings& start, const std::string& extra, uint8_t& given) {
  protocol::Request r;
  std::string err;
  check(parses(setLayoutWith(extra).c_str(), r, err), extra.c_str());
  Settings s = start;
  protocol::applySettings(r, s);
  given = r.settingsGiven;
  return s;
}

// The "settings" object: each field optional, and a field the request leaves out stays as the box has it.
void checkSettingsRequest() {
  protocol::Request r;
  std::string err;
  Settings usual;  // comp on, EQ on, MUTE
  Settings flipped;
  flipped.polishComp = false;
  flipped.polishEq = false;
  flipped.topSwitch = TopSwitch::Bypass;
  Settings mixed;
  mixed.polishEq = false;
  uint8_t given = 0xFF;

  check(settingsAre(appliedTo(flipped, "", given), false, false, TopSwitch::Bypass) && given == 0,
        "settings: absent leaves every current value");
  check(settingsAre(appliedTo(usual, "", given), true, true, TopSwitch::Mute) && given == 0,
        "settings: absent leaves the defaults too");
  check(settingsAre(appliedTo(flipped, ",\"settings\":{}", given), false, false, TopSwitch::Bypass) && given == 0,
        "settings: an empty object leaves every current value");
  check(settingsAre(appliedTo(usual, ",\"settings\":{\"polish_comp\":false,\"polish_eq\":false,\"top_switch\":\"bypass\"}", given),
                    false, false, TopSwitch::Bypass) &&
            given == (protocol::kGivenPolishComp | protocol::kGivenPolishEq | protocol::kGivenTopSwitch),
        "settings: all three fields in");
  check(settingsAre(appliedTo(mixed, ",\"settings\":{\"top_switch\":\"bypass\"}", given), true, false, TopSwitch::Bypass) &&
            given == protocol::kGivenTopSwitch,
        "settings: top_switch alone changes only the switch");
  check(settingsAre(appliedTo(usual, ",\"settings\":{\"polish_eq\":false}", given), true, false, TopSwitch::Mute) &&
            given == protocol::kGivenPolishEq,
        "settings: polish_eq alone changes only the EQ");
  check(settingsAre(appliedTo(flipped, ",\"settings\":{\"top_switch\":\"mute\",\"polish_comp\":true}", given), true, false,
                    TopSwitch::Mute) &&
            given == (protocol::kGivenPolishComp | protocol::kGivenTopSwitch),
        "settings: a value equal to the default still applies, and the field left out stays");
  check(settingsAre(appliedTo(usual, " , \"settings\" : { \"polish_comp\" : false , \"x\" : [1,{\"a\":null}] , \"polish_eq\":false } ", given),
                    false, false, TopSwitch::Mute) &&
            given == (protocol::kGivenPolishComp | protocol::kGivenPolishEq),
        "settings: whitespace is free and unknown keys with nested values are skipped");
  check(parses("{\"id\":6,\"cmd\":\"hello\",\"settings\":{\"polish_comp\":false}}", r, err) && r.cmd == protocol::Command::Hello,
        "settings: any command may carry the object");

  struct Bad {
    const char* extra;
    const char* why;
  };
  const Bad bad[] = {
      {",\"settings\":{\"polish_comp\":1}", "polish_comp must be true or false"},
      {",\"settings\":{\"polish_comp\":\"true\"}", "polish_comp must be true or false"},
      {",\"settings\":{\"polish_eq\":null}", "polish_eq must be true or false"},
      {",\"settings\":{\"top_switch\":\"loud\"}", "top_switch must be \"mute\" or \"bypass\""},
      {",\"settings\":{\"top_switch\":\"bypassX\"}", "top_switch must be \"mute\" or \"bypass\""},
      {",\"settings\":{\"top_switch\":\"\"}", "top_switch must be \"mute\" or \"bypass\""},
      {",\"settings\":{\"top_switch\":true}", "top_switch must be \"mute\" or \"bypass\""},
      {",\"settings\":true", "settings must be an object"},
      {",\"settings\":null", "settings must be an object"},
      {",\"settings\":[{\"polish_eq\":false}]", "settings must be an object"},
  };
  for (const Bad& b : bad) {
    check(!parses(setLayoutWith(b.extra).c_str(), r, err) && err == b.why, b.extra);
  }
  for (const char* torn : {",\"settings\":{\"polish_comp\":tru}", ",\"settings\":{\"polish_comp\":false", ",\"settings\":{\"top_switch\"}",
                           ",\"settings\":{\"polish_eq\":false,}"}) {
    check(!parses(setLayoutWith(torn).c_str(), r, err) && err == "malformed json", torn);
  }

  // A line cut anywhere is refused and read no further than its end (the sanitizers watch the reads).
  const std::string whole = setLayoutWith(",\"settings\":{\"polish_comp\":false,\"polish_eq\":true,\"top_switch\":\"bypass\"}");
  bool everyCutRefused = true;
  for (size_t cut = 0; cut < whole.size(); ++cut) everyCutRefused = everyCutRefused && !parses(whole.substr(0, cut).c_str(), r, err);
  check(everyCutRefused && parses(whole.c_str(), r, err), "settings: every cut of a settings request is refused, the whole line parses");
}

void checkSimpleReplies() {
  StringWriter w;
  protocol::replyError(w, 12, "unknown card \"foo\" in slot 3\\");
  check(JsonReader(w.out).valid() && has(w.out, "\"id\":12,\"ok\":false,\"error\":\"unknown card \\\"foo\\\" in slot 3\\\\\""),
        "proto: error reply escapes and parses");
  w = StringWriter();
  protocol::replyOk(w, 13);
  check(w.out == "{\"id\":13,\"ok\":true}\n", "proto: ok reply");

  w = StringWriter();
  protocol::replyLayout(w, 14, "Rig \"A\"", defaultLayout(), Settings());
  check(JsonReader(w.out).valid() && has(w.out, "\"layout\":{\"name\":\"Rig \\\"A\\\"\",\"slots\":[\"input_gate\",\"autotune\"") &&
            has(w.out, "\"reverb_spring\"]}"), "proto: layout reply");

  w = StringWriter();
  protocol::Status s;
  s.cpuMeanPercent = 54.66f;
  s.cpuWorstPercent = 71.0f;
  s.muted = true;
  s.seq = 9;
  s.recordErrors = 2;
  s.serialDropped = 5;
  s.strayBytes = 8;
  protocol::replyStatus(w, 15, s);
  check(JsonReader(w.out).valid() &&
            w.out == "{\"id\":15,\"ok\":true,\"cpu_mean\":54.66,\"cpu_worst\":71,\"late\":0,\"muted\":true,\"seq\":9,\"rec_errors\":2,\"serial_dropped\":5,\"stray_bytes\":8,\"boot_fails\":0,\"update_reason\":\"none\"}\n",
        "proto: status reply");

  w = StringWriter();
  s.bootFails = 2;
  s.updateReason = "BootFailed";
  protocol::replyStatus(w, 17, s);
  check(JsonReader(w.out).valid() && has(w.out, "\"stray_bytes\":8,\"boot_fails\":2,\"update_reason\":\"BootFailed\"}\n"),
        "proto: status carries the boot fails and the update reason");
}

// Every counter at its widest, so a reply piece that outgrows the writer's buffer shows up.
void checkStatusWidest() {
  StringWriter w;
  protocol::Status s;
  s.cpuMeanPercent = 100.0f;
  s.cpuWorstPercent = 100.0f;
  s.late = s.seq = s.recordErrors = s.serialDropped = s.strayBytes = s.bootFails = 0xFFFFFFFFu;
  s.updateReason = "ImageUnreadable";
  protocol::replyStatus(w, 0xFFFFFFFFu, s);
  check(JsonReader(w.out).valid() && w.overflows() == 0 && has(w.out, "\"update_reason\":\"ImageUnreadable\"}\n"),
        "proto: the widest status reply fits every piece");
}

void checkHelloReply() {
  StringWriter w;
  protocol::replyHello(w, 16, "v2.00.04", "Default", defaultLayout(), false);
  check(JsonReader(w.out).valid(), "proto: hello parses as JSON");
  check(w.calls > 20 && w.overflows() == 0, "proto: hello streams in pieces without overflow");
  check(has(w.out, "\"proto\":1,\"fw\":\"v2.00.04\""), "proto: hello carries proto and fw");
  for (int i = 0; i < kCardCount; ++i) {
    const CardDesc& d = cardDesc(card(i));
    check(has(w.out, std::string("{\"id\":\"") + d.id + "\",\"name\":\"" + d.name + "\""), d.id);
    for (int k = 0; k < d.knobCount; ++k) check(has(w.out, std::string("{\"name\":\"") + d.knob[k].name + "\""), d.knob[k].name);
  }
  check(has(w.out, "\"cost\":{\"avg\":14,\"worst\":21,\"measured\":true}"), "proto: hello carries costs");
  check(has(w.out, "\"kind\":\"stepped\",\"positions\":25,\"min\":-12,\"max\":12,\"unit\":\"st\",\"curve\":\"linear\""),
        "proto: hello carries the octave knob");
  check(has(w.out, "\"curve\":\"log\"") && has(w.out, "\"curve\":\"log_reversed\""), "proto: hello carries curves");
  check(has(w.out, "\"group\":\"pitch\"") && has(w.out, "\"group\":\"reverb\""), "proto: hello carries groups");
  check(has(w.out, "\"layout\":{\"name\":\"Default\""), "proto: hello carries the layout");
}

void checkHelloSerial() {
  StringWriter w;
  protocol::replyHello(w, 3, "v2.00.10", "Default", defaultLayout(), false, "0123456789ABCDEF01234567");
  check(JsonReader(w.out).valid() && w.overflows() == 0, "proto: hello with a serial parses as JSON");
  check(has(w.out, "\"fw\":\"v2.00.10\",\"serial\":\"0123456789ABCDEF01234567\",\"build\":\"release\""),
        "proto: hello carries the serial between fw and build");

  StringWriter none;
  protocol::replyHello(none, 3, "v2.00.10", "Default", defaultLayout(), false);
  check(!has(none.out, "serial"), "proto: hello leaves the serial out when the caller has none");
}

void checkReplyEscapes() {
  StringWriter w;
  protocol::replyLayout(w, 1, "caf\xC3\xA9\x01\x7F", defaultLayout(), Settings());
  check(JsonReader(w.out).valid() && has(w.out, "\"name\":\"caf\\u00c3\\u00a9\\u0001\\u007f\""),
        "proto: non-ASCII and control bytes in a reply string are escaped");
}

const char* kDefaultSettingsText = "\"settings\":{\"polish_comp\":true,\"polish_eq\":true,\"top_switch\":\"mute\"}";

// The settings object leaves the box in every reply that carries the layout, and its text is
// what a set_layout request takes back.
void checkSettingsReplies() {
  StringWriter w;
  protocol::replyLayout(w, 2, "Default", defaultLayout(), Settings());
  check(JsonReader(w.out).valid() && w.overflows() == 0 && has(w.out, std::string("\"reverb_spring\"]},") + kDefaultSettingsText + "}\n"),
        "settings: the layout reply ends with the default settings");

  Settings changed;
  changed.polishComp = false;
  changed.polishEq = false;
  changed.topSwitch = TopSwitch::Bypass;
  StringWriter c;
  protocol::replyLayout(c, 3, "Default", defaultLayout(), changed);
  const std::string text = "\"settings\":{\"polish_comp\":false,\"polish_eq\":false,\"top_switch\":\"bypass\"}";
  check(JsonReader(c.out).valid() && has(c.out, text + "}\n"), "settings: the layout reply carries the changed settings");

  const size_t at = c.out.find("\"settings\":");
  const std::string back = c.out.substr(at, c.out.size() - 2 - at);  // the field, without the reply's closing brace and newline
  uint8_t given = 0;
  check(settingsAre(appliedTo(Settings(), "," + back, given), false, false, TopSwitch::Bypass) && given == 7,
        "settings: a set_layout request takes the reply's settings text back");

  StringWriter hello;
  protocol::replyHello(hello, 4, "v", "Default", defaultLayout(), false, nullptr, &changed);
  check(JsonReader(hello.out).valid() && hello.overflows() == 0 && has(hello.out, text + "}\n") &&
            has(hello.out, "\"reverb_spring\"]},\"settings\""),
        "settings: hello carries the settings after the layout");
  StringWriter withSerial;
  protocol::replyHello(withSerial, 4, "v", "Default", defaultLayout(), false, "0123456789ABCDEF01234567", &changed);
  check(JsonReader(withSerial.out).valid() && has(withSerial.out, "\"serial\":\"0123456789ABCDEF01234567\"") &&
            has(withSerial.out, text + "}\n"),
        "settings: hello carries both the serial and the settings");
  StringWriter none;
  protocol::replyHello(none, 4, "v", "Default", defaultLayout(), false);
  check(!has(none.out, "settings"), "settings: hello leaves the settings out when the caller has none");
}

void checkWriterFormat() {
  StringWriter w;
  check(w.format("%d-%s", 42, "ab") && w.out == "42-ab" && w.overflows() == 0, "writer: a short format is written");
  const int fits = static_cast<int>(protocol::Writer::kFormatBytes) - 1;
  check(w.format("%0*d", fits, 1) && w.out.size() == 5 + static_cast<size_t>(fits), "writer: the longest format that fits is written whole");
  const std::string kept = w.out;
  check(!w.format("%0*d", fits + 1, 1) && w.out == kept && w.overflows() == 1,
        "writer: a format that overflows writes nothing, returns false and is counted");
  check(!w.format("%0*d", fits + 50, 1) && w.overflows() == 2, "writer: every overflow is counted");
}

void checkProtocolReplies() {
  checkSimpleReplies();
  checkStatusWidest();
  checkHelloReply();
  checkHelloSerial();
  checkReplyEscapes();
  checkSettingsReplies();
  checkWriterFormat();
}

void checkFramerBasics() {
  protocol::Framer f;
  using K = protocol::Framer::Kind;
  check(f.feed('p') == K::None && f.strayBytes() == 1, "framer: a bare byte is ignored and counted");
  check(f.feed('\n') == K::None && f.strayBytes() == 2, "framer: a newline outside a line is not a line");
  const char* line = "{\"id\":1}";
  K k = K::None;
  for (const char* c = line; *c; ++c) {
    k = f.feed(static_cast<uint8_t>(*c));
    check(k == K::None, "framer: line bytes are held");
  }
  check(f.strayBytes() == 2, "framer: line bytes are not stray");
  check(f.feed('\n') == K::Line && std::strcmp(f.line(), line) == 0, "framer: newline ends the line");
  check(f.feed('q') == K::None && f.strayBytes() == 3, "framer: stray bytes after a line are counted");

  // letters inside a line belong to the line
  f.feed('{');
  check(f.feed('p') == K::None && f.feed('\n') == K::Line && std::strcmp(f.line(), "{p") == 0, "framer: letters inside a line stay in it");
}

void checkFramerLimits() {
  protocol::Framer f;
  using K = protocol::Framer::Kind;
  // 768 bytes pass, 769 do not
  f.feed('{');
  for (size_t i = 1; i < protocol::kMaxLine; ++i) f.feed('a');
  check(f.feed('\n') == K::Line && std::strlen(f.line()) == protocol::kMaxLine, "framer: a 768-byte line passes");
  f.feed('{');
  for (size_t i = 1; i < protocol::kMaxLine; ++i) f.feed('a');
  check(f.feed('a') == K::TooLong, "framer: byte 769 is too long");
  check(f.feed('b') == K::None && f.feed('{') == K::None && f.feed('\n') == K::None, "framer: the rest of a long line is dropped");
  const uint32_t before = f.strayBytes();
  check(f.feed('m') == K::None && f.strayBytes() == before + 1, "framer: stray bytes count again after a dropped line");
}

void checkFramerReset() {
  protocol::Framer f;
  using K = protocol::Framer::Kind;
  const char* line = "{\"id\":1}";
  // a host that left mid-line does not leak into the next session
  for (const char* c = "{\"id\":1,\"cm"; *c; ++c) f.feed(static_cast<uint8_t>(*c));
  f.reset();
  const uint32_t reset = f.strayBytes();
  check(f.feed('d') == K::None && f.strayBytes() == reset + 1, "framer: a new session starts outside a line");
  for (const char* c = line; *c; ++c) f.feed(static_cast<uint8_t>(*c));
  check(f.feed('\n') == K::Line && std::strcmp(f.line(), line) == 0, "framer: the first line after a reset is whole");
  f.feed('{');
  for (size_t i = 1; i <= protocol::kMaxLine; ++i) f.feed('a');
  f.reset();
  check(f.feed('q') == K::None && f.strayBytes() == reset + 2, "framer: a reset ends a dropped line");
}

// A NUL inside a line would end the C string early, so the line is rejected whole.
void checkFramerNul() {
  protocol::Framer f;
  using K = protocol::Framer::Kind;
  check(f.feed('\0') == K::None && f.strayBytes() == 1, "framer: a NUL outside a line is stray");
  f.feed('{');
  f.feed('"');
  check(f.feed('\0') == K::BadByte, "framer: a NUL inside a line is reported once");
  check(f.feed('i') == K::None && f.feed('}') == K::None && f.feed('\n') == K::None, "framer: the rest of that line is dropped");
  check(f.strayBytes() == 1, "framer: the dropped line adds no stray bytes");
  const char* line = "{\"id\":2,\"cmd\":\"status\"}";
  for (const char* c = line; *c; ++c) f.feed(static_cast<uint8_t>(*c));
  check(f.feed('\n') == K::Line && std::strcmp(f.line(), line) == 0, "framer: the next line frames normally");
}

// Bytes that would have been bench letters do nothing, and the next line still frames.
void checkFramerStray() {
  protocol::Framer f;
  using K = protocol::Framer::Kind;
  for (const char* c = "faewdx1k"; *c; ++c) check(f.feed(static_cast<uint8_t>(*c)) == K::None, "framer: letters give no event");
  check(f.strayBytes() == 8, "framer: eight stray letters counted");
  const char* line = "{\"id\":2,\"cmd\":\"status\"}";
  for (const char* c = line; *c; ++c) f.feed(static_cast<uint8_t>(*c));
  check(f.feed('\n') == K::Line && std::strcmp(f.line(), line) == 0, "framer: a line after stray letters is whole");
  check(f.strayBytes() == 8, "framer: the line added no stray bytes");
}

void checkFramer() {
  checkFramerBasics();
  checkFramerLimits();
  checkFramerReset();
  checkFramerNul();
  checkFramerStray();
}

// ---- bench ops -----------------------------------------------------------------------------

std::string allOps(bool benchBuild) {
  std::string out = "[";
  for (int i = 0; i < kBenchOpCount; ++i) {
    if (!opAllowed(benchOp(i), benchBuild)) continue;
    if (out.size() > 1) out += ",";
    out += std::string("\"") + opName(benchOp(i)) + "\"";
  }
  return out + "]";
}

void checkOpTable() {
  protocol::Request r;
  std::string err;
  int queries = 0;
  for (int i = 0; i < kBenchOpCount; ++i) {
    const BenchOp op = benchOp(i);
    BenchOp back;
    check(static_cast<int>(op) == i, "ops: table index matches the enum");
    check(opFromName(opName(op), back) && back == op, opName(op));
    const std::string line = std::string("{\"id\":4,\"cmd\":\"bench\",\"op\":\"") + opName(op) + "\"" +
                             (op == BenchOp::SlotOff ? ",\"slot\":3" : "") + "}";
    check(parses(line.c_str(), r, err, true) && r.cmd == protocol::Command::Bench && r.op == op, "ops: every op round-trips in a bench build");
    const bool query = opClass(op) == OpClass::Query;
    queries += query ? 1 : 0;
    check(opAllowed(op, true), "ops: bench build allows every op");
    check(opAllowed(op, false) == query, "ops: release build allows queries only");
    check(parses(line.c_str(), r, err, false) == query &&
              (query || err == std::string("op not in this build: ") + opName(op)),
          "ops: release build parse follows the class");
  }
  check(queries == 3, "ops: three query ops");
  check(kBenchOpCount == static_cast<int>(BenchOp::Count) && kMaxBenchSlot == kSlotCount, "ops: counts derive from their sources");
  BenchOp none;
  check(!opFromName("nope", none) && !opFromName("", none) && !opFromName("Report", none), "ops: lookup is exact");
}

void checkOpBounds() {
  check(benchOp(-1) == BenchOp::Count && benchOp(kBenchOpCount) == BenchOp::Count && benchOp(1000) == BenchOp::Count,
        "ops: an index out of range gives the Count sentinel");
  check(std::strcmp(opName(BenchOp::Count), "") == 0, "ops: the sentinel has no name");
  check(opClass(BenchOp::Count) == OpClass::Bench && !opAllowed(BenchOp::Count, false), "ops: the sentinel is never a query");
}

void checkOpSlotParsing() {
  protocol::Request r;
  std::string err;
  check(parses("{\"id\":4,\"cmd\":\"bench\",\"op\":\"slot_off\",\"slot\":8}", r, err) && r.benchSlot == 8, "ops: slot 8 parses");
  check(parses("{\"id\":4,\"cmd\":\"bench\",\"op\":\"slot_off\",\"slot\":1}", r, err) && r.benchSlot == 1, "ops: slot 1 parses");
  check(!parses("{\"id\":4,\"cmd\":\"bench\"}", r, err) && err == "missing op" && r.id == 4, "ops: missing op");
  check(!parses("{\"id\":4,\"cmd\":\"bench\",\"op\":7}", r, err) && err == "op must be a string", "ops: numeric op");
  check(!parses("{\"id\":4,\"cmd\":\"bench\",\"op\":\"zap\"}", r, err) && err == "unknown op: zap", "ops: unknown op");
  check(!parses("{\"id\":4,\"cmd\":\"bench\",\"op\":\"zap\"}", r, err, false) && err == "unknown op: zap", "ops: unknown op in release");
  check(!parses("{\"id\":4,\"cmd\":\"bench\",\"op\":\"fault\"}", r, err, false) && err == "op not in this build: fault",
        "ops: bench op in release");
  check(!parses("{\"id\":4,\"cmd\":\"bench\",\"op\":\"slot_off\"}", r, err) && err == "missing slot", "ops: slot_off needs a slot");
  check(!parses("{\"id\":4,\"cmd\":\"bench\",\"op\":\"slot_off\",\"slot\":0}", r, err) && err == "slot must be 1 to 8", "ops: slot 0");
  check(!parses("{\"id\":4,\"cmd\":\"bench\",\"op\":\"slot_off\",\"slot\":9}", r, err) && err == "slot must be 1 to 8", "ops: slot 9");
  check(!parses("{\"id\":4,\"cmd\":\"bench\",\"op\":\"slot_off\",\"slot\":99999999999}", r, err) && err == "slot must be an integer",
        "ops: slot over 32 bits");
  check(!parses("{\"id\":4,\"cmd\":\"bench\",\"op\":\"slot_off\",\"slot\":4000000000}", r, err) && err == "slot must be 1 to 8",
        "ops: a huge slot is out of range");
  check(!parses("{\"id\":4,\"cmd\":\"bench\",\"op\":\"slot_off\",\"slot\":\"3\"}", r, err) && err == "slot must be an integer",
        "ops: string slot");
  check(!parses("{\"id\":4,\"cmd\":\"bench\",\"op\":\"slot_off\",\"slot\":-1}", r, err) && err == "slot must be an integer",
        "ops: negative slot");
  check(!parses("{\"id\":4,\"cmd\":\"bench\",\"op\":\"slot_off\",\"slot\":2.5}", r, err) && err == "slot must be an integer",
        "ops: fractional slot");
  check(parses("{\"id\":4,\"cmd\":\"bench\",\"op\":\"stats\",\"slot\":0}", r, err) && r.op == BenchOp::Stats,
        "ops: other ops ignore the slot");
}

void checkHelloOps() {
  for (const bool bench : {false, true}) {
    StringWriter w;
    protocol::replyHello(w, 1, "v", "Default", defaultLayout(), bench);
    check(has(w.out, std::string("\"build\":\"") + (bench ? "bench" : "release") + "\""), "ops: hello names the build");
    check(has(w.out, "\"bench_ops\":" + allOps(bench) + ","), "ops: hello lists exactly the allowed ops");
  }
  StringWriter rel;
  protocol::replyHello(rel, 1, "v", "Default", defaultLayout(), false);
  check(has(rel.out, "\"bench_ops\":[\"report\",\"stats\",\"record_status\"],") && !has(rel.out, "\"fault\""),
        "ops: release hello lists the three queries only");
}

void checkBenchOps() {
  checkOpTable();
  checkOpBounds();
  checkOpSlotParsing();
  checkHelloOps();
}

}  // namespace

void checkProtocol() {
  checkProtocolCommands();
  checkProtocolErrors();
  checkSettingsRequest();
  checkBenchOps();
  checkProtocolReplies();
  checkFramer();
}
