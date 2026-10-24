// Firmware core and ui tiers on the host: registry, layout, compose, protocol, record store, input
// logic, canvas and menu. Compiles firmware/cubevox/src/core/*.cpp and src/ui/*.cpp; a file that
// pulls in Arduino breaks this build. Also compiles the bootloader's decision policy as C99 and
// checks the boot block contract.
//
// Each file in this directory covers one subject. main() runs their entry points in a fixed
// order and reports the failure count.

#include <cstdio>

#include "core/version.h"
#include "support.h"

int main() {
  checkRegistry();
  checkLayoutAndCompose();
  checkProtocol();
  checkTx();
  checkFade();
  checkFuzz();
  checkInputLogic();
  checkMenu();
  checkCanvas();
  checkPanelTest();
  checkRecords();
  checkBootDecide();
  std::printf("firmware_test: version %s, %d failures\n", kFirmwareVersion, gFailures);
  return gFailures == 0 ? 0 : 1;
}
