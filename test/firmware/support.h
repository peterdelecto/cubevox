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

// Writes the canvas as a binary PGM at 4x, lit pixels white, to build/<name>.
void writePgm(const Canvas& canvas, const char* name);

// True when the canvas holds the golden frame test/firmware/golden/<name>.bin byte for byte. Those
// are the page-layout bytes the OLED proof renderer (tools/oled-proof/oled_motion.cpp) drew, cut out
// by tools/oled_golden.py. A mismatch writes the canvas to build/oled-mismatch-<name>.pgm.
bool matchesGolden(const Canvas& canvas, const char* name);

void checkRegistry();          // core_registry.cpp
void checkLayoutAndCompose();  // core_layout.cpp
void checkProtocol();          // core_protocol.cpp
void checkTx();                // core_tx_queue.cpp
void checkFade();              // core_layout_fade.cpp
void checkChainRun();          // core_chain_run.cpp
void checkFuzz();              // core_protocol_fuzz.cpp
void checkInputLogic();        // core_input.cpp
void checkMenu();              // ui_menu.cpp
void checkGui();               // ui_gui.cpp
void checkScreens();           // ui_screens.cpp
void checkFace();              // ui_face.cpp
void checkLevel();             // core_level.cpp
void checkLoadWindow();        // core_load.cpp
void checkCanvas();            // core_canvas.cpp
void checkPanelTest();         // core_panel_test.cpp
void checkRecords();           // core_record.cpp
void checkEntryLog();          // core_entry_log.cpp
void checkCrc32();             // core_crc32.cpp
void checkBootHealth();        // core_boot_health.cpp
void checkSplash();            // ui_splash.cpp
void checkBootDecide();        // boot_decide.cpp
