// Anomaly Coop — the map servers in the engine (F3a, docs/plans/2026-10-24-f3a-split-levels.md §1.1, §1.3, §1.6, T9). The owner's decision
// of 2026-10-24: every map is computed on the host's PC, a client never leads one. A level with players and without the host is led
// by a background process of the same exe (Mk), started and stopped by the host's process.
//
// The host's process (coop_mapserver.cpp): one levels::ServerLife per level, the process and its fsgame, the Hello of its bridge, what
// the process reports. Its world comes as a client's does (JoinRequest, the blob), its transfers are coop_levels_xfer.cpp, the Keeper over
// bridges instead of session slots.
//
// A map server (the same file): the key -coop_mapserver, its ghost actor, Ready, Stop. For the world it is a client: alife_frozen(),
// offline runs on the world server alone, what it asks of the world are requests. For its level it is the host: the modules' host half
// asks is_map_authority(), the players are virtual peers on Manager::session(), and the code of a level is the code of the host's process.
//
// The trunk (coop_trunk.cpp): what the host's process does with a message of a player on such a level, Msg::Tunnel both ways, and the
// virtual peers as the world server's LevelMap names them.
//
// The pure half is xrCoop/coop_mapserver_rules.h.
#pragma once

#include "xrCoop/coop_levels_wire.h"
#include "xrCoop/coop_mapserver_rules.h"
#include "xrCoop/coop_session.h"
#include "xrCoop/coop_soft_move.h" // host_verdict

namespace coop
{
namespace mapserver
{
// How long a map server waits for the host's process to answer its Hello: a level load of the host does not pump the session.
constexpr u32 kConnectTimeoutMs = 5 * 60 * 1000;

// The key of this process (Core.Params), parsed once. True: this process is a map server.
bool parsed_key();
const Args& key();

// The tables: the host's process (Tunnel, what a map server reports), a map server (Tunnel, the orders of the world server).
void install_server(Session& s);
void install_client(Endpoint& e);

// --- the host's process
void host_frame(); // levels::frame
// A process is there for `level`, or on its way, and it was not given up on.
bool host_serves(u16 level);
// It loaded its world: a player may enter.
bool host_ready(u16 level);
// A player may stand on `level`: it has a server, or one can be started.
bool host_can_serve(u16 level);
// How many count against coop_max_map_servers (ServerLife::running).
u32 host_running();
// What LevelMap says of it.
levels::wire::ServerState host_state_of(u16 level);
// The bridges of the processes that run: connected, or on their way (a world change for them waits, sync::host_begin_join).
const std::vector<PeerId>& host_bridges();
// The connected bridge of `level`, kInvalidPeer if there is none. The level of a bridge, wire::kNoLevel for anybody else.
PeerId bridge_of(u16 level);
u16 level_of_bridge(PeerId bridge);
u32 host_highest_number(); // the highest number a map server has now, 0 with none
// The host moves (plan §1.5): every map server hands everything to the world server and exits; none is started until
// host_launch_now. A server that is still loading is ended.
void host_stop_all();
// The same for a move that may leave them running (xrCoop/coop_soft_move.h): a server that leads a level other than `target` stays when
// `soft` is on; the others go as above.
void host_stop_for_move(u16 target, bool soft);
// True when every server that does not stay is gone.
bool host_all_stopped();
// The servers that stay through this move, connected.
std::vector<Session::SoftServer> host_kept();
bool host_keeps(u16 level);
// Why the server of `level` was kept or not at the last host_stop_for_move; None: the level has no server.
softmove::ServerVerdict host_verdict(u16 level);
// The move is over, or off: nobody is kept. What happened to a server that stayed is looked at now: a process lost meanwhile had
// nobody to tell (its players were parked), they reload now. `world_is_new`: this ends a move, in the world it brought, which is up.
void host_soft_over(bool world_is_new = false);
// The save of the host's move is written: the servers its seats need start now, for world epoch `epoch`.
void host_launch_now(u32 epoch);
// A new world on the host's process. Every server not started for it ends.
void host_world_replaced(u32 epoch);
// A message of a player (Session::set_in_filter): tunnels it if the map server of his level is to act on it, and says whether it runs
// here too (route_incoming). coop_trunk.cpp.
bool host_incoming(PeerId from, Msg type, Lane lane, const u8* packet, size_t size);
void trunk_host_on_tunnel(PeerId from, Reader& r);

// --- a map server
// The world server's table came: the virtual peers follow it. coop_trunk.cpp.
void server_on_map(const levels::wire::LevelMapMsg& m);
void trunk_server_on_tunnel(PeerId from, Reader& r);
VirtualPeers& trunk_peers(); // who stands on its level
// The join's half of what the table said runs with the next frame (coop_mapserver.cpp Me::came).
void server_visitor_came(const Visitor& v);
void server_visitor_in_world(PeerId peer);
void server_frame(); // levels::frame
bool server_leads(); // it told the world server its level is up: the world server hears what it says of the level
// Plan §1.6: the actor of this process is nobody, for a map server and for the dedicated host. A world that is up, every frame.
void ghost_frame();
// Its connection to the world server is over: nothing is left to do here. A process that is no map server: no effect.
void server_session_over();

// --- both
void reset();
void print_status();
} // namespace mapserver
} // namespace coop
