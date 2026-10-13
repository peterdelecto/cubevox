// Panel self-test (spec 2026-10-12 panel test), driven by the panel_* bench ops. Bench builds only;
// the release image gets inline stubs and none of the code.

#pragma once

#include "board/build_kind.h"
#include "core/bench_ops.h"
#include "core/canvas.h"
#include "core/settings.h"

#if CUBEVOX_BENCH

// Runs a panel_* op. panel_test turns the test on or off; the others act only while it is on, so
// leaving it restores the box. Returns an error text, or null on success.
const char* benchPanelRun(BenchOp op);

// Call every loop pass after controlsPoll(); prints moved pots and toggle edges.
void benchPanelPoll(uint32_t nowMs);

// The jack the sketch should drive: `requested`, or the K103 state panel_jack forced. The change
// still goes through the JackSwitch mute dip.
InputJack benchJack(InputJack requested);

// True while panel_mute holds the output muted.
bool benchMuteHeld();

// While the OLED pattern is up, draws it and returns true; the caller skips its own draw.
bool benchDrawPanelPattern(Canvas& canvas);

#else

inline void benchPanelPoll(uint32_t) {}
inline InputJack benchJack(InputJack requested) { return requested; }
inline bool benchMuteHeld() { return false; }
inline bool benchDrawPanelPattern(Canvas&) { return false; }

#endif
