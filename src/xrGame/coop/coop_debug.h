// Anomaly Coop — the server's debug in the engine (docs/DEBUG_DESIGN.md §B, work package 1; protocol 29).
//
// Debug is a right of the server, as sv_cheats. The world server (the host's process, or the dedicated host) keeps the state: on or off,
// who switched it, each player's personal flags. The key -dbg of the process that starts hosting switches it on (Core.isDebug, an exact
// key, not a substring of the command line); the console command coop_debug on|off of that process switches it any time (the host's F7
// button runs the same command). The state goes to every player in Msg::DebugState, the host player too over the LocalLink, and to every
// map server, at every change, at the end of each join and of each map server's.
//
// What the engine does with it:
//   - coop.sync requests named coop.dbg.* are refused while the debug is off, all but coop.dbg.view (coop_sync_api.cpp on_request);
//   - god: the authority of a player's level drops the PlayerHit of a player with the flag (coop_requests.cpp stalker_hit); his own
//     machine sets AF_GODMODE on its actor (and AF_UNLIMITEDAMMO for the ammo flag) from DebugState, and takes back only what it set;
//   - invisible (work package 4): the AI of the authority of a player's level leaves him out as the stock ai_ignore_actor leaves out the
//     actor (coop_ai.cpp npc_ai::debug_unseen: memory, enemy choice); a monster's stamina blow asks the god of the player it lands on
//     (npc_ai::player_god), not the process's GodMode();
//   - the engine's own cheats of the -dbg key (g_god, run_string, ...) are refused on a player's machine in a session; the world server's
//     g_god / g_unlimitedammo point at the personal flags (console_commands.cpp, refuse_local_cheat);
//   - coop_dbg <command> ... in the world server's console (needs the debug on) runs coop_debug_server.console(line, parsed) in its Lua.
//
// Lua (coop.debug, on every machine):
//   coop.debug.state() -> { on = bool, source = "off" | "key" | "console", serial = n, operator = bool, mine = bits,
//                           flags = { [peer] = bits, ... } }      the world server's own state there, the last DebugState elsewhere
//   coop.debug.flags(peer) -> bits (1 god, 2 ammo, 4 invisible; 0: none, or debug off)
//   coop.debug.set_flags(peer, bits | {god = bool, ammo = bool, invis = bool}) -> true | false, why     the world server only; a key that
//                           is absent keeps its bit; refused while the debug is off ("debug is off") and for a peer that is no player
//   coop.debug.is_operator() -> bool    this process is the world server of a session (its console switches the debug)
//   coop.debug.say(line)                a line to the log and to the dedicated host's window
//   coop.debug.unseen(id) -> bool       the object `id` (this machine's numbering) is a player the AI here leaves out (flag invisible,
//                                       this process runs his level's AI; npc_ai::debug_unseen); xr_combat_ignore.is_enemy asks it
// Callback: SendScriptCallback("coop_debug_changed", on, source) on every machine at every new state (a serial newer than the last);
// source is "off" | "key" | "console". In the host's process once (from the world server's half).
#pragma once

#include "xrCoop/coop_debug_rules.h"

struct lua_State;

namespace coop
{
class Session;
class Endpoint;

namespace debug
{
// The handler tables (Msg::DebugState: the client's; the world server has none to take).
void install_server(Session& s);
void install_client(Endpoint& e);

// The world server, Manager::start_host (sync::on_session_started): the state of a new session: on with the key -dbg, else off.
void on_session_started();
// Any process, the session ended or this client's world was replaced (sync::reset): the state goes, the flags this machine set on its
// actor are taken back.
void reset();
// The world server: a player left, his flags go.
void on_peer_left(PeerId peer);
// The world server: a player (or a map server's bridge) finished loading the world: the state now (sync::host_end_join).
void host_on_peer_in_world(PeerId peer);
// Every frame (sync::frame): the world server sends a changed state.
void frame();
void print_status();

// The state this process acts on: the world server's own, elsewhere the last DebugState.
bool on();
uint8_t flags(PeerId peer);
// Some player has `flag` in the state this process acts on: the cheap test before a per-object one (the AI asks for all it sees).
bool anyone_has(uint8_t flag);
// The authority of a player's level (stalker_hit): true if he has god; counted as a dropped hit.
bool host_drops_hit(PeerId peer);
// coop_sync_api.cpp on_request (the world server): true if this request is refused because the debug is off (kRefusedOff).
bool refuses_request(const std::string& name);
// console_commands.cpp: the engine's cheats of the -dbg key; true = refused (and said in the log), the command must not run.
bool refuse_local_cheat(const char* command);

// coop_debug, coop_dbg; from coop::register_console_commands.
void register_console();
// coop.debug.*; from the coop script registrator (coop_script.cpp register_raw).
void register_lua(lua_State* L);
} // namespace debug
} // namespace coop
