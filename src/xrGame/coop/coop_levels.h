// Anomaly Coop — split levels (F3a, docs/plans/2026-10-24-f3a-split-levels.md): the registry of who stands on which level.
//
// Every player may be on another map. The host's process is the world server: it holds the world, leads its own level and
// plays the host. A background map server (Mk) of the host's PC leads each other level that has players. This module answers
// "which level is this?" and "who is on it?" for the rest of the coop layer; the roles of a process (who may act as the world
// server, as the authority of a map, as a player, as a mirror) are in coop_manager.h (is_world_server() ...).
//
// T1 is the skeleton. Until the registry (T10) every player stands on the level of the one world, so the answers below are
// those of a session without split levels, and the switches already work: the game rules coop_split_levels and
// coop_story_together and the machine settings coop_max_map_servers and coop_mapserver_linger (plan §3). The registry
// (LevelMap, the table of servers and gates), the marks "online at a map server" (coop_levels_alife.cpp), the transfers and the
// auditor (coop_levels_xfer.cpp), the map-server processes (coop_mapserver.cpp) and the bridge (coop_trunk.cpp) fill in
// the other tasks; gate_of() comes with the gates (T10, the Gate enum is coop_levels_rules.h).
//
// Lua: coop.levels.split(), story_together(), my_level(), level_of_player(peer), players_on(level), led_by_host_process(level),
// online_at_server(id); coop.is_world_server(), is_map_authority(), is_player(), is_mirror() (the roles, old meanings: the
// host is the world server, the authority of its map and a player; a client is a player and a mirror).
// Console: coop_split_levels, coop_story_together, coop_max_map_servers, coop_mapserver_linger, coop_levels_status,
// coop_levels_audit.
#pragma once

#include "xrCoop/coop_levels_wire.h"
#include "xrCoop/coop_protocol.h"

struct lua_State;

namespace coop
{
class Session;
class Endpoint;

namespace levels
{
// A game graph level id (CGameGraph level_id(), a u8 in the engine) as the protocol carries it (u16, xrCoop/coop_levels_wire.h).
// "No level" is wire::kNoLevel (0xffff); coop_levels_rules.h has a kNoLevel of its own in this namespace, so this one is
// not repeated here.
using LevelId = u16;

// Game rules and machine settings (console variables; the rules are set per game by coop_game_schema, plan §3).
extern int g_coop_split_levels; // 1 = every player on his own map (default), 0 = one map for the group, as in F3
extern int g_coop_story_together; // 1 = the story levels are entered together (default), 0 = apart as any level
extern int g_coop_max_map_servers; // 0..7, default 3: how many background map servers the host's PC may run
extern int g_coop_mapserver_linger; // seconds a map server stays after its last player left, default 60

bool split(); // split levels are on
bool story_together(); // the story levels are entered together (meaningful with split())

// The level this process has loaded: wire::kNoLevel outside a loaded level.
LevelId my_level();
// The level a player stands on (a peer of the session; the host is kHostPeer): wire::kNoLevel if he is none of ours or his level is
// not known. Skeleton: every player is on my_level().
LevelId level_of_player(PeerId peer);
// The level of a game graph vertex / of an object of the world (by its id in the world server's numbering):
// wire::kNoLevel when unknown.
LevelId level_of_vertex(u16 game_vertex);
LevelId level_of_object(u16 id);
// The host's process leads `level` (it is the level the host stands on). Skeleton: my_level().
bool led_by_host_process(LevelId level);
// How many players of the session stand on `level` (the host among them); 0 outside a session.
u32 players_on(LevelId level);
// The object is online at a map server (the world server holds it online without a game object). Always false until the
// marks of T10/T11.
bool online_at_server(u16 id);

// Module lifecycle, as every coop module (sync::install_server / install_client ...).
void install_server(Session& s); // F3a T4: the world server's handlers (on the hosting session)
void install_client(Endpoint& e); // F3a T4: the client's (a remote client's session, the host player's LocalLink)
void reset(); // session started / ended (sync::on_session_started, sync::reset)
void on_peer_left(PeerId id); // sync::on_peer_left
void frame(); // main thread, every frame (sync::frame)
void print_status(); // coop_status section (sync::print_status), console coop_levels_status
void register_console(); // from coop::register_console_commands
// Fills coop.levels and the role functions of coop.*; called by script::register_raw once the `coop` table exists.
void register_lua(lua_State* L);
} // namespace levels
} // namespace coop
