#pragma once
// The game's user-facing name. Only text the player sees uses it: the repo, the
// CMake project, the namespace, notita.exe and the save files (notita_world.sav,
// the NOTITASV magic) keep "notita", so nothing that refers to them breaks.
// The exe's version info has its own copy in src/app/notita.rc.

namespace notita::brand {

inline constexpr const char* kName = "Fine Grain";
inline constexpr const char* kVersion = "0.1.0";
inline constexpr const char* kPitch = "a pixel sandbox where every grain is simulated";
// The in-joke, "Whole Grain", is saved for an "every system on, max detail"
// preset, should one ever exist.

} // namespace notita::brand
