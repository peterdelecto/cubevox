#pragma once

#include <string>

// Native "Open..." file panel. Returns the chosen path, or an empty string
// if the user cancelled. Must be called from the main thread.
std::string openFilePanel();

// Native directory panel, for SAVE/LOAD's preset directory: lets the user
// pick an existing folder or make a new one (canCreateDirectories). Same
// panel serves both; SAVE overwrites into whatever is chosen. Returns the
// chosen path, or an empty string if the user cancelled. Main thread only.
std::string chooseDirectoryPanel();
