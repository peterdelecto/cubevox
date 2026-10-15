// Prints the firmware's `hello` reply for the default layout to stdout. The app's fake box
// reads app/fixtures/hello.json, and the hello_fixture ctest keeps that file equal to this output.

#include <stdio.h>

#include "core/layout.h"
#include "core/protocol.h"

namespace {

class StdoutWriter : public protocol::Writer {
 public:
  void write(const char* text, size_t len) override { fwrite(text, 1, len, stdout); }
};

}  // namespace

int main() {
  StdoutWriter out;

  protocol::replyHello(out, 1, "fixture", "Default", defaultLayout(), false);

  return 0;
}
