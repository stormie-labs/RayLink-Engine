// Anomaly Coop — "Game settings", the engine's part (docs/GAME_SETTINGS.md §3.4, §3.5; xrCoop/coop_game_values.h).
// A host's game carries its own values; the engine gives Lua what it cannot do alone:
//   - a hook before the world's scripts start (before_world_start): the group of a new world, then the Lua call
//     coop_game_settings.before_world_start(mode, save), which enables the game's values before any on_game_start;
//   - the new game that waits in memory between the "new game" screen and that hook;
//   - a layer over the machine's console: the game's values for the 17 co-op rules are set, the machine's are remembered
//     and given back when the level is left, so user.ltx keeps the machine's values.
//
// Lua (namespace coop.game, registered from coop_script.cpp):
//   coop.game.stage_new(payload, faction) -> bool     the record of a new game (<= 256 KiB) and the world's group (a
//                                                     character name); replaces the one staged before; false (and the old
//                                                     one dropped) when refused
//   coop.game.take_new() -> payload|nil, faction|nil  the staged game, once; nil, nil when none is staged
//   coop.game.set_cvars({name = value}) -> applied, refused
//                                                     white list of 17 (xrCoop/coop_game_values.h kGameCvars). A value is
//                                                     a string, a finite number or a boolean (true = "1"). applied: how
//                                                     many console commands were run; refused: { name = reason }, the
//                                                     reason "not_allowed" (not a game rule), "bad_value" (not a plain
//                                                     token), "unknown" (the console has no such command or shows a value
//                                                     that cannot be written back)
//   coop.game.release_cvars() -> restored             the machine's values back (also done when the level is left)
//   coop.game.machine_cvar(name) -> string|nil        the machine's value while a game value is set, else nil
//   coop.game.set_world_faction(f) -> bool            false (unchanged) when f is not a character name
//   coop.game.world_faction() -> string               "" when the world has no group
//
// Threading: Lua calls come from one Lua thread at a time; the hooks run on the main thread. No state is shared with
// the network thread, so there is no lock.
#pragma once

#include <string>

struct lua_State;

namespace coop
{
namespace game
{
// CALifeSimulator::CALifeSimulator, after the Lua state is rebuilt and before the stock start_game_callback (the first
// on_game_start of every script). Reads the start mode of g_pGamePersistent->m_game_params, settles the world's group and
// the staged game (on_world_start), then calls coop_game_settings.before_world_start(mode, save) in a protected call
// ("save" is m_game_or_spawn: the save's name for a load, the spawn name for a new game). A missing script or function is
// a no-op, an error is logged and not fatal: a single-player game without the co-op scripts is not changed.
void before_world_start();

// CLevel::net_Stop: the machine's console values come back. Does nothing when no game value is set.
void on_level_exit();

// The world's group (docs §8): set from the staged game at a new world, by Lua at a load. "" when none.
const std::string& world_faction();

// Fills coop.game; called from coop_script.cpp register_raw (the `coop` table exists then).
void register_lua(lua_State* L);
} // namespace game
} // namespace coop
