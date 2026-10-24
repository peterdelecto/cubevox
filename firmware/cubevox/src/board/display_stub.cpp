#include "board/display_stub.h"

namespace {

class StubDisplay : public Display {
 public:
  void flushStep(Canvas&) override {}
};

}  // namespace

Display& stubDisplay() {
  static StubDisplay display;
  return display;
}
