// Anomaly Coop - the game starts, watches and stops its own world server (docs/plans/2026-11-12-split-levels-native.md section 3.3,
// stage 6a). The world server is this same exe with -coop_dedicated -coop_hidden: its standard input and output are pipes of this
// module, its end of input means "save the world and quit" (coop_host_console.cpp), its output carries the @rl lines
// (xrCoop/coop_world_server_rules.h). The pure part (command line, environment, fsgame, the state of the pipe) is in that header.
//
// Nothing here runs until Lua calls coop.world_server.start: a game that never starts a server makes no thread, pipe or file.
//
// Lua (coop.world_server, coop.machine_memory):
//   coop.world_server.start{ world = "new" | "load", save = <stem | "last">, port = <n>, stage = <file>, nick = <name> }
//       -> true | false, why
//       world: required; save: required for "load"; port: the host's port of the game's settings (default 27960), the next free one
//       if it is taken (state().port_moved); stage: the staging file of stage 5 for a new world (-coop_stage), passed on as it is;
//       nick: the world's name for -coop_nick (one word made of it). Refused while a server of this game runs or in a co-op session.
//       stage = true: the file Lua wrote at stage_file(); the server is given its name alone and reads it from its own data folder
//       (a profile's path may hold blanks, which the command line cannot carry). A save's name goes to the server as hex.
//   coop.world_server.stage_file() -> the path where Lua writes the new world's staging file (xrCoop/coop_world_stage.h), or nil
//   coop.world_server.state() (also .status()) -> nil before the first start, else a table:
//       state       "starting" | "ready" | "failed" | "stopping" | "down" | "crashed"   (Summary in the rules header)
//       phase       the last "@rl phase": "none" | "starting" | "world" | "session" | "ready" | "saving" | "down" | "unknown"
//       port        the port it listens on (0 until "@rl ready"); asked_port: the one it was started with; port_moved: they differ
//       wait, wait_text     the last "@rl wait" (cleared at ready)
//       players     "@rl players"; mapservers = { [level] = "loading" | "ready" | "serving" }
//       error, error_text   the last "@rl error" (gave_up, bad_args, no_save), or this module's: ready_timeout, killed
//       exited, exit_code, crashed (ended without "@rl phase down"), killed (terminated after the stop's 60 s)
//       pid, elapsed_ms, lines = the last 20 log lines (cp1251)
//   coop.world_server.lines(n) -> the last n log lines (n <= 200, default 20), oldest first, cp1251
//   coop.world_server.running() -> bool: its process runs
//   coop.world_server.stop() -> bool: its input is closed (it saves and quits); 60 s later it is terminated. false: nothing runs
//   coop.machine_memory() -> { phys_total_mb, phys_free_mb, commit_total_mb, commit_free_mb, load }   (GlobalMemoryStatusEx)
//
// The operator's token never reaches Lua. Lua connects with coop.connect("127.0.0.1:" .. state().port); Manager::start_client asks
// operator_token_for() for that port and presents the token in Hello (the line the lead adds there: see the report of stage 6a).
#pragma once

#include "xrCoop/coop_operator.h"

#include <cstdint>

struct lua_State;

namespace coop
{
namespace world_server
{
// The operator's token of the world server this game started, if one of its processes runs and listens on `port`. For
// Manager::start_client (a loopback address only). The token stays in this process's memory, nowhere else.
bool operator_token_for(uint16_t port, op::Token& out);

// A world server of this game runs.
bool running();

// coop.world_server.* and coop.machine_memory; from the coop script registrator (coop_script.cpp register_raw), after coop exists.
void register_lua(lua_State* L);
} // namespace world_server
} // namespace coop
