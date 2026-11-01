// The File menu of the Mac's menu bar, with one item, Open.... It chooses the clip the emulator loops.

#pragma once

#include <functional>

namespace host {

// Adds "File" to the main menu right after the application menu, with "Open..." on Cmd+O. The item
// shows an open panel for wav, aiff, aif and mp3, and calls `onOpen` with the chosen path on the main
// thread. Call it once the GLFW window exists, so GLFW's own menu bar is already there.
void installFileMenu(std::function<void(const char*)> onOpen);

}  // namespace host
