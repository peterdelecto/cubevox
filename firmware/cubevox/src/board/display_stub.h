// A Display with no panel behind it. It sends nothing and leaves every page dirty, so the face, which
// waits for the panel to take each frame, never draws.

#pragma once

#include "core/display.h"

Display& stubDisplay();
