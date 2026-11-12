// Anomaly Coop — the dedicated host in the engine (docs/DEDICATED_HOST.md, plan §12). The key -coop_dedicated: the world server without a
// window and without a player, in a console window. Same exe, same headless start as a map server (HEADLESS_AUDIT.md). The process is a
// host whose player stands on the service level fake_start for good and plays nothing: its actor is the ghost of a map server, its seat
// does not play (levels::Seat::plays), split levels are on, so every level with players has a map server and everybody is a client.
//
// Here: the keys (the world to start, the port), the lines of the console window, the end of the process, and the commands of the operator.
// The commands work on any host: coop_players, coop_kick, coop_time, coop_restore. coop_world only before a world exists, coop_console_log
// only in the window.
//
// The pure half is xrCoop/coop_dedicated_rules.h, the window is xrEngine/coop_host_console.cpp.
#pragma once

#include "xrCoop/coop_dedicated_rules.h"
#include "xrCoop/coop_protocol.h" // PeerId

#include <string>

class CALifeObjectRegistry;

namespace coop
{
namespace dedicated
{
// This process was started with -coop_dedicated.
bool on();
// The world being created is this process's new one, not a save: Lua sets the game up for it.
bool new_world();
// The operator, the launcher's pipe or the window asked for the end: the world is being saved and torn down.
bool quitting();
// Its key, parsed once. The port of its session.
const Args& args();

// Manager::frame, every frame from the first one: the world, the lines of the window, the quit.
void frame();
// on_alife_objects_loaded: the actor of the save, whatever level it stands on, is the ghost and goes to the service level. False: this
// game has no such level.
bool place_ghost(CALifeObjectRegistry& objects);
// The commands; from coop::register_console_commands.
void register_console();

// Plan 2026-11-12 §2.3: a line of the operator of a world server (Session::on_operator_command: it passed the rate, the sender, the length
// and op::allowed). Checked again here (still joined, still the operator, op::command_verdict), then decided by what it means for him:
// coop_kick never kicks himself, coop_dbg runs as his peer's request and never a command of the server's own console, coop_game_set is
// not run (no table of game values on the server yet). The rest goes the way of a line of the server's own pipe, not recorded in the
// console's history. What the command says through the answers below goes back to him as the ScriptEvent "coop.op.answer"
// ({ cmd = <first word>, text = <lines joined by "\n">, ok = bool }). Host, main thread.
void run_for(PeerId from, const std::string& line);
// A line a command of this process says to its operator: kept while run_for runs a command, nothing otherwise.
void answer_line(const char* line);
} // namespace dedicated
} // namespace coop
