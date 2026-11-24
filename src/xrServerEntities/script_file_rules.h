#pragma once
// Anomaly Coop (TZ R74, section 6.16): script files of zero bytes. Pure rules, no engine types (tests/test_script_file_rules.cpp).
// GAMMA removes a script by shipping an empty file over it, and axr_main.on_game_start skips files of size 0. A script that
// asks for such a module by name (the _G.__index loader, CScriptEngine::process_file_if_exists) loaded the empty file into a
// namespace: the header was copied into a buffer of header + body bytes, one short of the terminator, and strncpy_s stopped
// the game (invalid parameter, Stable 60). An empty file is no module, like a missing one; a loaded buffer always has room
// for the terminator.
#include <cstddef>
#include <cstdint>

namespace script_file_rules
{
// Does a script file of this size define a module? An empty one does not: it is how mods remove a script.
inline bool is_module(std::uint32_t size_real) { return size_real > 0; }

// Bytes for the namespace header, the script body and the terminator the header copy writes.
inline std::size_t buffer_size(std::size_t header_len, std::size_t body_len) { return header_len + body_len + 1; }
} // namespace script_file_rules
