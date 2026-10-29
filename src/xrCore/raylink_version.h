// RayLink Engine: the one place that names the engine and its build (owner, 2026-10-29: everywhere in the menus, in
// the core and in the game it must say which engine this is - "RayLink Engine" and its version).
//
// The number comes from raylink_build.h (RAYLINK_BUILD, 0 = development build). Shown by: the log header (xrCore.cpp,
// x_ray.cpp), the window title (Device_Initialize.cpp), the crash dialogs (xrDebugNew.cpp), the main menu version line
// (string_table.cpp appends it to ui_st_game_version), the console command `version` (xr_ioc_cmd.cpp), Lua
// raylink_engine_version() / raylink_engine_build() / get_modded_exes_name() (script_ini_file_script.cpp) and the
// co-op build id of the handshake (coop_manager.cpp). RayLink is a fork of X-Ray Monolith (modded exes): the credit
// strings below stay with it, as do the GSC Game World and Anomaly Team copyright notices of the sources.
// No engine headers: the unit tests include this file as it is (anomaly-coop tests/test_raylink_version.cpp).
#pragma once

#include <string>

#include "raylink_build.h"

#ifndef RAYLINK_BUILD
#define RAYLINK_BUILD 0
#endif

#define RAYLINK_ENGINE_NAME "RayLink Engine"
#define RAYLINK_ENGINE_BASE "X-Ray Monolith"              // the engine RayLink is a fork of: credit, keep it
#define RAYLINK_GAME_NAME "S.T.A.L.K.E.R. Anomaly 1.5.3" // the game this engine runs (G.A.M.M.A. runs on it too)

#define RAYLINK_STRINGIZE_(x) #x
#define RAYLINK_STRINGIZE(x) RAYLINK_STRINGIZE_(x)
#if RAYLINK_BUILD > 0
#define RAYLINK_BUILD_LABEL "b" RAYLINK_STRINGIZE(RAYLINK_BUILD)
#else
#define RAYLINK_BUILD_LABEL "dev"
#endif

// "RayLink Engine b28" / "RayLink Engine dev". One string literal: tools/package.ps1 finds it in the exe (followed by
// its terminating zero) and compares it with the package version.
#define RAYLINK_ENGINE_VERSION RAYLINK_ENGINE_NAME " " RAYLINK_BUILD_LABEL

namespace raylink
{
// What a build number reads as: "b28"; 0 (or less) is a development build, "dev".
inline std::string build_label(int build)
{
	return build > 0 ? "b" + std::to_string(build) : std::string("dev");
}

// "RayLink Engine b28"
inline std::string engine_version(int build)
{
	return std::string(RAYLINK_ENGINE_NAME) + " " + build_label(build);
}

// The game's version line with the engine after it: "S.T.A.L.K.E.R. G.A.M.M.A. 0.9.5 | RayLink Engine b28". The game's
// text stays first and unchanged: mods look for "G.A.M.M.A." in it (Catspaw's utils, Devices of Anomaly Redone). A line
// that already names this engine is left as it is; an empty one becomes the engine alone.
inline std::string game_line_with_engine(const std::string& game_line, const std::string& engine)
{
	if (engine.empty() || game_line.find(engine) != std::string::npos)
		return game_line;
	if (game_line.empty())
		return engine;
	return game_line + " | " + engine;
}
} // namespace raylink
