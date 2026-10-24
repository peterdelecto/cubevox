// Fuzzes the protocol on the host. Feeds the request parser, the layout resolve and the line
// framer mutated and random input.

#include "support.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

#include "core/layout.h"
#include "core/protocol.h"

namespace {

// Deterministic generator so a failure reproduces.
struct Lcg {
  uint32_t s;
  uint32_t next() {
    s = s * 1664525u + 1013904223u;
    return s >> 8;
  }
};

// Runs a line through the release-build parse, whose result is not needed (it only has to stay
// safe), then through the bench-build parse and the layout resolve.
void parseAndResolve(const char* line) {
  protocol::Request req;
  char err[protocol::kErrorBytes];
  protocol::parseRequest(line, req, err, sizeof err, false);
  if (protocol::parseRequest(line, req, err, sizeof err, true) && req.cmd == protocol::Command::SetLayout) {
    Layout l = defaultLayout();
    protocol::resolveLayout(req, l, err, sizeof err);
  }
}

void checkFuzzParser() {
  const std::string valid =
      "{\"id\":3,\"cmd\":\"set_layout\",\"name\":\"Stage\",\"slots\":[\"input_gate\",\"autotune\",\"octave\","
      "\"unison\",\"slapback\",\"distortion\",\"gate\",\"reverb_chasm\"],\"extra\":{\"a\":[1,2,{\"b\":null}]}}";
  Lcg rng{12345};

  // every prefix of a valid line, and the line with each single byte replaced
  for (size_t n = 0; n <= valid.size(); ++n) parseAndResolve(valid.substr(0, n).c_str());
  for (size_t i = 0; i < valid.size(); ++i) {
    std::string s = valid;
    for (int k = 0; k < 8; ++k) {
      s[i] = static_cast<char>(rng.next());
      if (s[i] == '\0') s[i] = '?';
      parseAndResolve(s.c_str());
    }
  }

  // random JSON-flavored soup and raw bytes
  const char soup[] = "{}[]\",:\\ \t0123456789-+.eEtrufalsn\"idcmdnameslots";
  for (int i = 0; i < 20000; ++i) {
    std::string s;
    const size_t len = rng.next() % 200;
    for (size_t k = 0; k < len; ++k) {
      const bool raw = i % 4 == 0;
      char c = raw ? static_cast<char>(rng.next()) : soup[rng.next() % (sizeof soup - 1)];
      if (c == '\0') c = ' ';
      s.push_back(c);
    }
    parseAndResolve(s.c_str());
  }

  // deep nesting and long strings must not blow the stack or the buffers
  parseAndResolve(("{\"x\":" + std::string(5000, '[') + std::string(5000, ']') + "}").c_str());
  parseAndResolve(("{\"x\":" + std::string(5000, '{') + "}").c_str());
  parseAndResolve(("{\"id\":1,\"cmd\":\"" + std::string(5000, 'x') + "\"}").c_str());
}

// The framer sees every byte value in any order.
void checkFuzzFramer() {
  Lcg rng{777};
  protocol::Framer f;
  size_t lines = 0;
  for (int i = 0; i < 200000; ++i) {
    const uint8_t byte = (i % 7 == 0) ? '\n' : (i % 11 == 0 ? '{' : static_cast<uint8_t>(rng.next()));
    const protocol::Framer::Kind k = f.feed(byte);
    if (k == protocol::Framer::Kind::Line) {
      ++lines;
      check(std::strlen(f.line()) <= protocol::kMaxLine, "fuzz: framed line fits the limit");
      parseAndResolve(f.line());
    }
  }
  check(lines > 100, "fuzz: the framer produced lines");
  check(f.strayBytes() > 1000, "fuzz: bytes outside lines are counted, never acted on");
}

}  // namespace

void checkFuzz() {
  checkFuzzParser();
  checkFuzzFramer();
}
