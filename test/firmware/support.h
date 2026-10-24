// The firmware host test files share these helpers. Each file defines one entry point, and
// main.cpp calls the entry points in the order declared here.

#pragma once

#include "core/canvas.h"
#include "core/registry.h"

// Counts the failed check() calls.
extern int gFailures;

// Prints the message and counts a failure when `ok` is false.
void check(bool ok, const char* what);

// Casts a table index to a Card without a range check.
Card card(int i);

// Clears the dirty flag of every canvas page.
void clearAllDirty(Canvas& canvas);

void checkRegistry();          // core_registry.cpp
void checkLayoutAndCompose();  // core_layout.cpp
void checkProtocol();          // core_protocol.cpp
void checkTx();                // core_tx_queue.cpp
void checkFade();              // core_layout_fade.cpp
void checkFuzz();              // core_protocol_fuzz.cpp
void checkInputLogic();        // core_input.cpp
void checkMenu();              // ui_menu.cpp
void checkCanvas();            // core_canvas.cpp
void checkPanelTest();         // core_panel_test.cpp
void checkRecords();           // core_record.cpp
void checkCrc32();             // core_crc32.cpp
void checkBootHealth();        // core_boot_health.cpp
void checkSplash();            // ui_splash.cpp
void checkBootDecide();        // boot_decide.cpp
