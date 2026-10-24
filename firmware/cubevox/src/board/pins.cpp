#include "board/pins.h"

namespace pins {

void report(Print& out) {
  struct Named {
    const char* name;
    uint32_t pin;
  };
  const Named list[] = {{"XSMT", kXsmt},     {"MUTE_N", kMuteN}, {"SAI", kSaiPins[0]}, {"", kSaiPins[1]},
                        {"", kSaiPins[2]},   {"", kSaiPins[3]},  {"", kSaiPins[4]},    {"OLED", kOledScl},
                        {"", kOledSda}};
  out.print("[boot] pins");
  for (const Named& n : list) {
    if (n.name[0] != '\0') {
      out.print(' ');
      out.print(n.name);
    }
    out.print(' ');
    out.print(static_cast<char>('A' + portIndexOf(n.pin)));
    out.print(static_cast<int>(__builtin_ctz(maskOf(n.pin))));
  }
  out.println();
}

}  // namespace pins
