#include "stdafx.h"
#include "pch_script.h"
#include "coop_levels.h"
#include "coop_entities.h"
#include "coop_manager.h"
#include "coop_dedicated.h"
#include "coop_join.h"
#include "coop_mapserver.h"
#include "coop_money.h"
#include "coop_net.h"
#include "xrCoop/coop_scope.h"
#include "xrCoop/coop_roster.h" // who is a player (plan 2026-11-12 stage 3)
#include "coop_levels_map.h" // the rows of the LevelMap

#include "../../xrEngine/XR_IOConsole.h"
#include "../../xrEngine/xr_ioc_cmd.h"
#include "ai_space.h"
#include "game_graph.h"
#include "alife_simulator.h"
#include "alife_object_registry.h"
#include "alife_graph_registry.h"
#include "xrServer_Objects_ALife.h"
#include "xrServer_Objects_ALife_Items.h"

#include <cmath>
#include <map>
#include <string>

// F3a split levels: the switches, the questions "which level" and "who is on it", the host's table and LevelMap, the gates, the
// console and the Lua side. See coop_levels.h.

namespace coop
{
namespace levels
{
int g_coop_split_levels = 0;
int g_coop_story_together = 1;
int g_coop_world_server = 0; // owner 2026-11-13: the new mode is a choice of its own; Lua reads it, the engine keeps it
int g_coop_max_map_servers = int(kDefaultMaxMapServers); // build 50: the ceiling of the PC, the cap itself follows the players
int g_coop_mapserver_linger = int(kDefaultLingerMs / 1000);
int g_coop_soft_host_move = 1;
// Both settings above are saved in user.ltx, so a new default in the code never reaches a machine that ran an earlier build: the
// owner's own world server kept 3 servers and 60 s (seen 2026-11-14). The old defaults move to the new ones once a machine; this
// number is saved with them, and a value somebody sets after that is his.
int g_coop_map_servers_rev = 0;

void settle_machine_settings()
{
	if (g_coop_map_servers_rev >= 1)
		return;
	g_coop_map_servers_rev = 1;
	if (g_coop_max_map_servers == 3)
	{
		g_coop_max_map_servers = int(kDefaultMaxMapServers);
		::Msg("[coop] levels: coop_max_map_servers was the old default 3: it is %d now (build 50, the cap follows the players)",
		    g_coop_max_map_servers);
	}
	if (g_coop_mapserver_linger == 60)
	{
		g_coop_mapserver_linger = int(kDefaultLingerMs / 1000);
		::Msg("[coop] levels: coop_mapserver_linger was the old default 60 s: it is %d s now (build 50, for the map emptied last)",
		    g_coop_mapserver_linger);
	}
}

namespace
{
constexpr u32 kMapEveryMs = 5000; // the table goes out when it changes and at least this often

struct GateRow
{
	Gate gate = Gate::Free;
	std::string reason;
};

struct State
{
	// host
	Table table{kHostPeer};
	bool map_due = false;
	u64 map_sent_at = 0;
	bool host_split = false; // the game rule as the world took it: it applies at the world's start (plan §3)
	LevelId host_going = wire::kNoLevel; // host_going_to
	std::map<std::string, GateRow> gates; // by level name, from Lua (coop_levels.script)
	// client: the host's last table
	wire::LevelMapMsg view;
	bool view_known = false;
	u32 maps_bad = 0;
	LevelId echo = wire::kNoLevel; // EchoOf
} g;

// The game rule. A dedicated host has no level to play on: every level with players has a map server.
bool game_split() { return g_coop_split_levels != 0 || dedicated::on(); }

// The name of a level id of the game graph, nullptr when it has none.
const char* level_name_of(LevelId level)
{
	const CGameGraph* gg = ai().get_game_graph();
	if (!gg || level == wire::kNoLevel)
		return nullptr;
	const auto it = gg->header().levels().find(GameGraph::_LEVEL_ID(level));
	return it == gg->header().levels().end() ? nullptr : it->second.name().c_str();
}

LevelId level_by_name(const char* name)
{
	const CGameGraph* gg = ai().get_game_graph();
	if (!gg || !name)
		return wire::kNoLevel;
	for (const auto& it : gg->header().levels())
		if (!xr_strcmp(it.second.name(), name))
			return LevelId(it.first);
	return wire::kNoLevel;
}

void host_seat_self()
{
	const LevelId mine = my_level();
	if (mine == wire::kNoLevel || g.host_going != wire::kNoLevel)
		return; // the host's level is being replaced: the seat keeps what it has until the new world is up
	const Seat* s = g.table.find(kHostPeer);
	if (s && s->level == mine)
		return;
	Seat seat;
	seat.peer = kHostPeer;
	seat.level = mine;
	seat.in_world = true;
	seat.plays = !dedicated::on();
	g.table.set(seat);
	g.map_due = true;
}

// The new world's own level comes with host_seat_self; until then, on the way, this one.
void host_seat_going(LevelId level)
{
	Seat seat;
	seat.peer = kHostPeer;
	seat.level = level;
	seat.in_world = true;
	seat.plays = !dedicated::on();
	g.table.set(seat);
	g.map_due = true;
}

void host_seat(PeerId peer, LevelId level, bool in_world)
{
	Seat seat;
	seat.peer = peer;
	seat.level = level;
	seat.in_world = in_world;
	g.table.set(seat);
	g.map_due = true;
}

void host_send_map()
{
	wire::LevelMapMsg m;
	m.flags = u8((split() ? wire::LevelMapMsg::Split : 0) | (story_together() ? wire::LevelMapMsg::StoryTogether : 0));
	// plan 2026-11-12 stage 3: a seat that plays no character (the dedicated host's own) is on no map: no row, no server row for its level
	for (const Seat& s : map_rows::listed_seats(g.table.seats()))
	{
		wire::LevelMapMsg::Player p;
		p.peer = s.peer;
		p.level = s.level;
		p.in_world = s.in_world ? 1 : 0;
		p.proxy = s.peer == kHostPeer ? u16(0) : join::proxy_of_peer(s.peer);
		p.money = s.peer == kHostPeer ? 0 : money::host_balance(s.peer);
		m.players.push_back(p);
	}
	for (u16 level : map_rows::levels_of(g.table.seats()))
	{
		wire::LevelMapMsg::Server row;
		row.level = level;
		const bool host_process = g.table.server_of(level) == Server::HostProcess;
		row.server = host_process ? wire::LevelMapMsg::HostProcess : wire::LevelMapMsg::MapServerProcess;
		row.state = u8(host_process ? wire::ServerState::Ready : mapserver::host_state_of(level));
		m.servers.push_back(row);
	}
	for (const auto& it : g.gates)
	{
		if (it.second.gate == Gate::Free)
			continue;
		const LevelId level = level_by_name(it.first.c_str());
		if (level == wire::kNoLevel)
			continue;
		wire::LevelMapMsg::Gate row;
		row.level = level;
		row.gate = u8(it.second.gate);
		row.reason = it.second.reason;
		m.gates.push_back(row);
	}
	Writer w(64 + m.players.size() * 8 + m.servers.size() * 4 + m.gates.size() * 32);
	if (!m.write(w))
		return; // over the wire's limits: cannot happen with kMaxPlayers seats; the next change tries again
	// Not through the join queue: LevelMap needs no world (coop_world_gate.h Free), and a map server takes its players from it.
	for (const PeerInfo& p : mgr().session().peers())
		if (p.state == PeerState::Joined)
			mgr().session().send(p.id, Lane::Reliable, Msg::LevelMap, w);
	for (const PeerId bridge : mgr().session().bridges())
		mgr().session().send(bridge, Lane::Reliable, Msg::LevelMap, w);
	g.map_due = false;
	g.map_sent_at = mgr().now_ms();
}

void client_on_level_map(PeerId from, Reader& r)
{
	if (from != kHostPeer || !mgr().is_client())
		return;
	wire::LevelMapMsg m;
	if (!m.read(r))
	{
		if (++g.maps_bad <= 3)
			::Msg("! [coop] levels: a malformed LevelMap from the host");
		return;
	}
	g.view = m;
	g.view_known = true;
	mapserver::server_on_map(g.view);
}
} // namespace

const char* level_name(LevelId level) { return level_name_of(level); }

bool split()
{
	if (mgr().is_client())
		return g.view_known && (g.view.flags & wire::LevelMapMsg::Split) != 0;
	return mgr().is_host() ? g.host_split : game_split();
}

// A dedicated host has no group on its level to board an exit with: "together" is "better together" there.
bool story_together() { return g_coop_story_together != 0 && !dedicated::on(); }

bool leads_my_level() { return is_map_server(); }

LevelId my_level()
{
	// the simulator and the level graph of a level that is not up are not there (coop_manager.h world_up)
	if (!world_up() || !ai().get_alife())
		return wire::kNoLevel;
	return LevelId(ai().alife().graph().level().level_id());
}

LevelId level_of_vertex(u16 game_vertex)
{
	const CGameGraph* gg = ai().get_game_graph();
	if (!gg || !gg->valid_vertex_id(game_vertex))
		return wire::kNoLevel;
	return LevelId(gg->vertex(GameGraph::_GRAPH_ID(game_vertex))->level_id());
}

LevelId level_of_object(u16 id)
{
	// like my_level(): the simulator of a level that is being built or torn down answers nothing safely
	if (!world_up() || !ai().get_alife())
		return wire::kNoLevel;
	const CSE_ALifeDynamicObject* e = ai().alife().objects().object(id, true);
	return e ? level_of_vertex(e->m_tGraphID) : wire::kNoLevel;
}

LevelId level_of_player(PeerId peer)
{
	if (is_map_server())
	{
		for (const wire::LevelMapMsg::Player& p : g.view.players)
			if (p.peer == peer)
				return p.level;
		return wire::kNoLevel;
	}
	if (!is_player() && !is_world_server())
		return wire::kNoLevel;
	// the host is a player of its own session, so is this machine whatever its slot (a client's own slot is not among
	// the peers its session lists); the others are the peers the session knows
	if (peer != kHostPeer && peer != mgr().self() && !mgr().session().peer(peer))
		return wire::kNoLevel;
	if (!split())
		return my_level(); // one world, one level for everybody
	if (peer == mgr().self() && mgr().is_client())
		return my_level();
	if (mgr().is_host())
	{
		if (peer == kHostPeer)
			return my_level();
		const Seat* s = g.table.find(peer);
		return s ? s->level : wire::kNoLevel;
	}
	for (const wire::LevelMapMsg::Player& p : g.view.players)
		if (p.peer == peer)
			return p.level;
	return wire::kNoLevel;
}

bool led_by_host_process(LevelId level)
{
	if (level == wire::kNoLevel)
		return false;
	if (split() && mgr().is_client())
		return level == level_of_player(kHostPeer);
	return level == my_level();
}

u32 players_on(LevelId level)
{
	if ((!is_player() && !is_map_server() && !is_world_server()) || level == wire::kNoLevel)
		return 0;
	if (split())
	{
		u32 n = 0;
		if (mgr().is_host())
			return u32(g.table.population(level));
		for (const wire::LevelMapMsg::Player& p : g.view.players)
			n += p.level == level ? 1u : 0u;
		return n;
	}
	if (level != my_level())
		return 0;
	// the peers() of the host are the clients, of a client the host and the others: this machine's own player is one more, unless it is
	// a host whose seat plays no character; a seat that does not play is nobody (coop_roster.h)
	const u32 self = mgr().is_host() && dedicated::on() ? 0u : 1u;
	return self + roster::player_count(mgr().session().peers());
}

bool on_host_level(PeerId peer)
{
	if (!mgr().is_host() || !split() || peer == kHostPeer)
		return true; // a map server: every peer of its session is on its level
	const Seat* s = g.table.find(peer);
	const LevelId mine = my_level();
	// not seated, or the host's world is being replaced: nothing to tell him apart from the group by
	return !s || mine == wire::kNoLevel || s->level == mine;
}

bool host_character_elsewhere(u16 character)
{
	if (!mgr().is_host() || !split() || character == 0)
		return false;
	const PeerId peer = join::peer_of_proxy(character);
	return peer != kInvalidPeer && !on_host_level(peer);
}

bool passes_to(Msg type, PeerId peer)
{
	if (g.echo != wire::kNoLevel && mgr().is_host() && level_of_player(peer) == g.echo)
		return false;
	return level_passes(type, split(), on_host_level(peer));
}

EchoOf::EchoOf(LevelId level) { g.echo = level; }
EchoOf::~EchoOf() { g.echo = wire::kNoLevel; }

bool host_player_loads(PeerId peer, LevelId level)
{
	if (!mgr().is_host() || !split() || level == wire::kNoLevel)
		return false;
	host_seat_self();
	const bool stays = g.table.server_of(level) == Server::HostProcess || mapserver::host_can_serve(level, host_map_cap(peer), peer);
	if (stays)
		host_seat(peer, level, false);
	return stays;
}

void host_player_in_world(PeerId peer)
{
	if (!mgr().is_host() || !split())
		return;
	const Seat* s = g.table.find(peer);
	const LevelId level = s ? s->level : my_level();
	host_seat(peer, level, true);
}

MoveVerdict host_judge_move(PeerId peer, LevelId target)
{
	host_seat_self();
	Gate gate = effective_gate(gate_of_level(target), split(), story_together());
	if (gate == Gate::Together && g.table.all_on(target, peer))
		gate = Gate::Free; // the others are in there already: nobody is left to wait for, he walks in
	const bool new_server = g.table.server_of(target) != Server::HostProcess && !mapserver::host_serves(target);
	std::vector<uint16_t> stop;
	const MoveVerdict verdict = decide_move(gate, new_server, mapserver::host_slots(), host_map_cap(peer), stop);
	if (verdict == MoveVerdict::Go && !stop.empty())
		mapserver::host_stop_for_room(stop, target, peer); // a Stopping server holds no place: the new one starts without waiting
	return verdict;
}

u32 host_map_players_now() { return g.table.players(); }

u32 host_map_cap(PeerId mover)
{
	u32 players = g.table.players();
	const Seat* s = mover != kInvalidPeer ? g.table.find(mover) : nullptr;
	if (mover != kInvalidPeer && (!s || !s->plays || s->level == kNoLevel))
		++players; // a join: he is not in the table until he may stay (host_player_loads)
	// "maps = players + 1" counts the map of a host who plays: this process leads it, no map server does
	const Seat* host = g.table.find(kHostPeer);
	if (host && host->plays && host->level != kNoLevel && players > 0)
		--players;
	return map_server_cap(players, u32(_max(0, g_coop_max_map_servers)));
}

void host_player_moves(PeerId peer, LevelId target)
{
	if (!mgr().is_host() || !split())
		return;
	host_seat(peer, target, false);
}

std::vector<LevelId> host_map_levels()
{
	std::vector<LevelId> out;
	for (const uint16_t level : g.table.map_server_levels())
		out.push_back(level);
	return out;
}

u32 host_map_players(LevelId level) { return g.table.map_players(level); }

std::vector<u32> host_players_on(LevelId level) { return g.table.on(level); }

std::vector<u32> host_players_of(LevelId level)
{
	std::vector<u32> out;
	for (const Seat& s : g.table.seats())
		if (s.level == level && s.peer != kHostPeer)
			out.push_back(s.peer);
	return out;
}

u32 host_servers_if_host_on(LevelId level)
{
	std::vector<LevelId> seen;
	for (const Seat& s : g.table.seats())
		if (s.peer != kHostPeer && s.level != wire::kNoLevel && s.level != level &&
		    std::find(seen.begin(), seen.end(), s.level) == seen.end())
			seen.push_back(s.level);
	return u32(seen.size());
}

bool host_goes_alone(LevelId level)
{
	if (!mgr().is_host() || !split())
		return false;
	// a level entered together waits for the others, unless they are in there already
	return effective_gate(gate_of_level(level), split(), story_together()) != Gate::Together || g.table.all_on(level, kHostPeer);
}

void host_player_reloads(PeerId peer)
{
	const Seat* s = g.table.find(peer);
	if (mgr().is_host() && split() && s)
		host_seat(peer, s->level, false);
}

void host_map_changed() { g.map_due = true; }

void host_world_replaced()
{
	if (!mgr().is_host())
		return;
	const bool was = g.host_split;
	g.host_split = game_split();
	g.host_going = wire::kNoLevel;
	if (!was && !g.host_split)
		return;
	if (!join::host_soft_window())
		xfer_reset(); // the simulator that had the marks is gone; the map servers connect anew when they are Ready
	mapserver::host_world_replaced(join::host_world_epoch());
	g.map_due = true;
}

void host_going_to(LevelId level)
{
	if (!mgr().is_host() || !split())
		return;
	g.host_going = level;
	if (level != wire::kNoLevel)
		host_seat_going(level);
}

void host_send_map_now()
{
	if (mgr().is_host() && split())
		host_send_map();
}

Gate gate_of_level(LevelId level)
{
	if (mgr().is_client())
	{
		for (const wire::LevelMapMsg::Gate& row : g.view.gates)
			if (row.level == level)
				return Gate(row.gate);
		return Gate::Free;
	}
	const char* name = level_name_of(level);
	const auto it = name ? g.gates.find(name) : g.gates.end();
	return it == g.gates.end() ? Gate::Free : it->second.gate;
}

namespace
{
// The session's last word on a message of a player (Session::set_in_filter). A map server's session has no filter: every peer of it
// stands on its level.
bool host_takes(PeerId from, Msg type, Lane lane, const uint8_t* packet, size_t size)
{
	return !is_world_server() || mapserver::host_incoming(from, type, lane, packet, size);
}
} // namespace

void install_server(Session& s)
{
	s.set_in_filter(&host_takes);
	xfer_install_server(s);
}

void install_client(Endpoint& e)
{
	e.on(Msg::LevelMap, &client_on_level_map);
	xfer_install_client(e);
}

void reset()
{
	g.table.clear();
	g.map_due = false;
	g.map_sent_at = 0;
	g.host_split = game_split();
	g.host_going = wire::kNoLevel;
	g.gates.clear();
	g.view = wire::LevelMapMsg();
	g.view_known = false;
	mapserver::reset();
	xfer_reset();
}

void on_peer_left(PeerId id)
{
	if (!mgr().is_host())
		return;
	g.table.remove(id);
	if (split())
		g.map_due = true;
}

void frame()
{
	settle_machine_settings(); // user.ltx is read by now
	if (!split())
		return;
	if (mgr().is_host())
	{
		host_seat_self();
		mapserver::host_frame();
		const u64 now = mgr().now_ms();
		if (g.map_due || now - g.map_sent_at >= kMapEveryMs)
			host_send_map();
		xfer_host_frame();
	}
	else if (leads_my_level())
	{
		xfer_client_frame();
		mapserver::server_frame();
	}
}

void print_status()
{
	::Msg("[coop] levels: split %s, story together %s | this level %u, players on it %u, this process %s", split() ? "on" : "off",
	    story_together() ? "on" : "off", u32(my_level()), players_on(my_level()),
	    mgr().is_host() ? "hosts" : (leads_my_level() ? "is a map server" : "mirrors"));
	if (mgr().is_host())
		for (const Seat& s : g.table.seats())
			::Msg("[coop] levels:   #%u on level %u [%s]%s%s", s.peer, u32(s.level), level_name_of(s.level) ? level_name_of(s.level) : "?",
			    s.in_world ? "" : " loading", g.table.server_of(s.level) == Server::HostProcess ? ", the host's process leads it" : ", a map server leads it");
	if (mgr().is_host() && split())
		::Msg("[coop] levels: map servers cap %u = min(players %u + 1, coop_max_map_servers %d, %u), linger %d s for the map emptied last, "
		      "at most %u s for the others",
		    host_map_cap(), host_map_players_now(), g_coop_max_map_servers, u32(mapserver::kMaxBridges), g_coop_mapserver_linger,
		    kColdLingerMs / 1000);
	mapserver::print_status();
	xfer_print_status();
}

namespace
{
class CCC_CoopLevelsStatus : public IConsole_Command
{
public:
	CCC_CoopLevelsStatus(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = true; }
	void Execute(LPCSTR) override { print_status(); }
	void Info(TInfo& I) override { xr_strcpy(I, "split levels: the switches, this level, who is on it"); }
};

class CCC_CoopLevelsAudit : public IConsole_Command
{
public:
	CCC_CoopLevelsAudit(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = true; }
	void Execute(LPCSTR) override
	{
		xfer_audit_now();
	}
	void Info(TInfo& I) override { xr_strcpy(I, "split levels: check now that every object has exactly one owner"); }
};
} // namespace

void register_console()
{
	CMD4(CCC_Integer, "coop_split_levels", &g_coop_split_levels, 0, 1); // game rule, applies at the world's start
	CMD4(CCC_Integer, "coop_story_together", &g_coop_story_together, 0, 1); // game rule
	// game rule (xrCoop/coop_game_values.h): a new world with split levels runs as a world server and its creator's game joins it. The
	// new game screen and Continue read it; nothing in the engine does (a world server is split by being one: game_split)
	CMD4(CCC_Integer, "coop_world_server", &g_coop_world_server, 0, 1);
	// machine setting, default 6: the most map servers this PC can carry. The cap in force is min(players + 1, this, 7) (host_map_cap)
	CMD4(CCC_Integer, "coop_max_map_servers", &g_coop_max_map_servers, 0, 7);
	// machine setting, seconds, default 600: how long the map emptied last stays; every other empty map at most 60 s, none with < 2 GB free
	CMD4(CCC_Integer, "coop_mapserver_linger", &g_coop_mapserver_linger, 0, 3600);
	CMD4(CCC_Integer, "coop_map_servers_rev", &g_coop_map_servers_rev, 0, 1); // saved: the two above were moved to build 50's defaults
	CMD4(CCC_Integer, "coop_soft_host_move", &g_coop_soft_host_move, 0, 1); // the way back for a live check: not a rule of the game
	CMD1(CCC_CoopLevelsStatus, "coop_levels_status");
	CMD1(CCC_CoopLevelsAudit, "coop_levels_audit");
}

// ---------------------------------------------------------------- Lua
// Plain C functions, as in coop_script.cpp: a wrong argument from an addon returns nil / false instead of a luabind "no
// matching overload" error, which is fatal in this engine (R-11.4).
namespace
{
// A whole number 0..max from argument `idx`.
bool arg_whole(lua_State* L, int idx, double max, u32& out)
{
	if (lua_type(L, idx) != LUA_TNUMBER)
		return false;
	const double v = double(lua_tonumber(L, idx));
	if (!std::isfinite(v) || v < 0.0 || v > max || v != std::floor(v))
		return false;
	out = u32(v);
	return true;
}

void push_level(lua_State* L, LevelId level)
{
	if (level == wire::kNoLevel)
		lua_pushnil(L);
	else
		lua_pushnumber(L, lua_Number(level));
}

// The roles (coop_manager.h): the old meanings until a module is moved.
int l_is_world_server(lua_State* L)
{
	lua_pushboolean(L, is_world_server());
	return 1;
}
int l_is_map_authority(lua_State* L)
{
	lua_pushboolean(L, is_map_authority());
	return 1;
}
int l_is_player(lua_State* L)
{
	lua_pushboolean(L, is_player());
	return 1;
}
int l_is_mirror(lua_State* L)
{
	lua_pushboolean(L, is_mirror());
	return 1;
}
int l_is_map_server(lua_State* L)
{
	lua_pushboolean(L, is_map_server());
	return 1;
}

int l_is_dedicated(lua_State* L)
{
	lua_pushboolean(L, dedicated::on());
	return 1;
}
int l_dedicated_new_world(lua_State* L)
{
	lua_pushboolean(L, dedicated::new_world());
	return 1;
}

int l_split(lua_State* L)
{
	lua_pushboolean(L, split());
	return 1;
}
int l_story_together(lua_State* L)
{
	lua_pushboolean(L, story_together());
	return 1;
}
int l_my_level(lua_State* L)
{
	push_level(L, my_level());
	return 1;
}
int l_level_of_player(lua_State* L)
{
	u32 peer = 0;
	if (!arg_whole(L, 1, double(kMaxPeerSlots - 1), peer))
		lua_pushnil(L);
	else
		push_level(L, level_of_player(PeerId(peer)));
	return 1;
}
int l_players_on(lua_State* L)
{
	u32 level = 0;
	lua_pushnumber(L, arg_whole(L, 1, double(0xfffe), level) ? lua_Number(players_on(LevelId(level))) : lua_Number(0));
	return 1;
}
int l_led_by_host_process(lua_State* L)
{
	u32 level = 0;
	lua_pushboolean(L, arg_whole(L, 1, double(0xfffe), level) && led_by_host_process(LevelId(level)));
	return 1;
}
// coop.levels.level_name(level id): "l01_escape"; nil for no level
int l_level_name(lua_State* L)
{
	u32 level = 0;
	const char* name = arg_whole(L, 1, double(0xfffe), level) ? level_name_of(LevelId(level)) : nullptr;
	if (name)
		lua_pushstring(L, name);
	else
		lua_pushnil(L);
	return 1;
}
// coop.levels.on_host_level(peer): he stands on the host's level (the host's process; elsewhere always true)
int l_on_host_level(lua_State* L)
{
	u32 peer = 0;
	lua_pushboolean(L, !arg_whole(L, 1, double(kMaxPeerSlots - 1), peer) || on_host_level(PeerId(peer)));
	return 1;
}
// coop.levels.squads_at(bridge) -> the host's process: the ids of the squads that map server holds
int l_squads_at(lua_State* L)
{
	u32 bridge = 0;
	lua_newtable(L);
	if (arg_whole(L, 1, double(0xffff), bridge))
	{
		int n = 0;
		for (const u16 id : squads_at(PeerId(bridge)))
		{
			lua_pushnumber(L, lua_Number(id));
			lua_rawseti(L, -2, ++n);
		}
	}
	return 1;
}
// coop.levels.carried(id) -> { { id =, section =, condition = }, ... }: what this process's ALife has in that object: all there is of the
// inventory of a player on another level (every item that changes hands there is reported, coop_levels_xfer.cpp up_parent)
int l_carried(lua_State* L)
{
	u32 id = 0;
	lua_newtable(L);
	const CSE_ALifeDynamicObject* e = arg_whole(L, 1, double(0xfffe), id) && ai().get_alife() ? ai().alife().objects().object(u16(id), true) : nullptr;
	if (!e)
		return 1;
	int n = 0;
	for (const u16 child : e->children)
	{
		CSE_ALifeDynamicObject* c = ai().alife().objects().object(child, true);
		const CSE_ALifeInventoryItem* item = c ? smart_cast<CSE_ALifeInventoryItem*>(c) : nullptr;
		if (!item)
			continue;
		lua_newtable(L);
		lua_pushnumber(L, lua_Number(child));
		lua_setfield(L, -2, "id");
		lua_pushstring(L, c->s_name.c_str());
		lua_setfield(L, -2, "section");
		lua_pushnumber(L, lua_Number(item->m_fCondition));
		lua_setfield(L, -2, "condition");
		lua_rawseti(L, -2, ++n);
	}
	return 1;
}
// coop.levels.logic_put(bytes), coop.levels.logic_bytes(): coop_levels_brain.logic_of and logic_in, the engine between them
int l_logic_put(lua_State* L)
{
	size_t size = 0;
	const char* bytes = lua_tolstring(L, 1, &size);
	logic_put(bytes ? bytes : "", bytes ? size : 0);
	return 0;
}
int l_logic_bytes(lua_State* L)
{
	const std::string& bytes = logic_bytes();
	lua_pushlstring(L, bytes.data(), bytes.size());
	return 1;
}
// coop.levels.map_servers() -> the host's process: { { bridge = peer, level = id }, ... }, those that are connected
int l_map_servers(lua_State* L)
{
	lua_newtable(L);
	int n = 0;
	if (mgr().is_host())
		for (const PeerId bridge : mapserver::host_bridges())
		{
			lua_newtable(L);
			lua_pushnumber(L, lua_Number(bridge));
			lua_setfield(L, -2, "bridge");
			lua_pushnumber(L, lua_Number(mapserver::level_of_bridge(bridge)));
			lua_setfield(L, -2, "level");
			lua_rawseti(L, -2, ++n);
		}
	return 1;
}
// coop.levels.bridge_of_player(peer) -> the host's process: the bridge of the map server he stands under, nil on the host's level
int l_bridge_of_player(lua_State* L)
{
	u32 peer = 0;
	const PeerId bridge = arg_whole(L, 1, double(kMaxPeerSlots - 1), peer) && mgr().is_host() && !on_host_level(PeerId(peer)) ?
	    mapserver::bridge_of(level_of_player(PeerId(peer))) :
	    kInvalidPeer;
	if (bridge == kInvalidPeer)
		lua_pushnil(L);
	else
		lua_pushnumber(L, lua_Number(bridge));
	return 1;
}
// coop.levels.talking(peer) -> the host's process: he holds a talk lock on a map server's level, as that server last said of his body
// (LevelBodyFacts); false on the host's level, and when it has said nothing for a while
int l_talking(lua_State* L)
{
	u32 peer = 0;
	BodyFacts facts;
	const bool told = arg_whole(L, 1, double(kMaxPeerSlots - 1), peer) && mgr().is_host() && body_facts(join::proxy_of_peer(PeerId(peer)), facts);
	lua_pushboolean(L, told && facts.talking);
	return 1;
}
// coop.levels.body_stays(id): this object dies on the host now and its body stands on a map server: no release of it
int l_body_stays(lua_State* L)
{
	lua_pushboolean(L, host_body_stays(u16(lua_tonumber(L, 1))));
	return 1;
}

// coop.levels.far_place(id, x, y, z, level vertex, game vertex): a client's record of a player on another level, which nothing else moves
int l_far_place(lua_State* L)
{
	u32 id = 0, node = 0, graph = 0;
	if (!mgr().is_client() || leads_my_level() || !ai().get_alife() || !lua_isnumber(L, 2) || !lua_isnumber(L, 3) || !lua_isnumber(L, 4))
		return 0;
	if (!arg_whole(L, 1, double(0xfffe), id) || !arg_whole(L, 5, double(0xffffffff), node) || !arg_whole(L, 6, double(0xfffe), graph))
		return 0;
	CSE_ALifeDynamicObject* e = ai().alife().objects().object(u16(id), true);
	if (!e || e->m_bOnline || !ai().game_graph().valid_vertex_id(u16(graph)))
		return 0;
	Fvector pos;
	pos.set(float(lua_tonumber(L, 2)), float(lua_tonumber(L, 3)), float(lua_tonumber(L, 4)));
	entities::place_offline(e, pos, e->o_Angle, node, u16(graph));
	return 0;
}
int l_online_at_server(lua_State* L)
{
	u32 id = 0;
	lua_pushboolean(L, arg_whole(L, 1, double(0xfffe), id) && online_at_server(u16(id)));
	return 1;
}
// coop.levels.holder_of(id) -> the host's process: the bridge of the map server that holds the object or its squad, nil when none does
int l_holder_of(lua_State* L)
{
	u32 id = 0;
	const PeerId bridge = arg_whole(L, 1, double(0xfffe), id) ? holder_of(u16(id)) : kInvalidPeer;
	if (bridge == kInvalidPeer)
		lua_pushnil(L);
	else
		lua_pushnumber(L, lua_Number(bridge));
	return 1;
}
// coop.levels.recall(squad id[, ms]) -> a map server: the world server recalled the squad (its Lua checked who said so): offline now and
// home, pinned for ms (kRecallPinMs by default, at most kRecallPinMaxMs), 0 lifts the pin; false anywhere else
int l_recall(lua_State* L)
{
	u32 id = 0, ms = kRecallPinMs;
	const bool args = arg_whole(L, 1, double(0xfffe), id) && (lua_isnoneornil(L, 2) || arg_whole(L, 2, double(kRecallPinMaxMs), ms));
	lua_pushboolean(L, args && recall(u16(id), ms));
	return 1;
}
int l_logic_look(lua_State* L)
{
	lua_pushboolean(L, logic_looking());
	return 1;
}
// coop.levels.set_gate(level name, "free"|"suggest"|"together", reason id): the world server, coop_levels.script
int l_set_gate(lua_State* L)
{
	Gate gate = Gate::Free;
	const char* name = lua_type(L, 1) == LUA_TSTRING ? lua_tostring(L, 1) : nullptr;
	const char* word = lua_type(L, 2) == LUA_TSTRING ? lua_tostring(L, 2) : nullptr;
	const char* reason = lua_type(L, 3) == LUA_TSTRING ? lua_tostring(L, 3) : "";
	if (!name || !word || !parse_gate(word, gate) || !mgr().is_host())
	{
		lua_pushboolean(L, 0);
		return 1;
	}
	GateRow& row = g.gates[name];
	const bool changed = row.gate != gate || row.reason != reason;
	row.gate = gate;
	row.reason = reason;
	g.map_due = g.map_due || changed;
	lua_pushboolean(L, 1);
	return 1;
}
// coop.levels.gate_of(level name): the gate as the two game rules fold it, a word; nil for a level that does not exist
int l_gate_of(lua_State* L)
{
	const LevelId level = lua_type(L, 1) == LUA_TSTRING ? level_by_name(lua_tostring(L, 1)) : wire::kNoLevel;
	if (level == wire::kNoLevel)
		lua_pushnil(L);
	else
		lua_pushstring(L, gate_name(effective_gate(gate_of_level(level), true, story_together())));
	return 1;
}

// Sets `fns` on the table `name` of coop (made here if it is not there), or on coop itself when `name` is null.
void fill(lua_State* L, const char* name, const luaL_Reg* fns)
{
	lua_pushstring(L, "coop");
	lua_rawget(L, LUA_GLOBALSINDEX);
	if (!lua_istable(L, -1))
	{
		lua_pop(L, 1);
		return; // the registrator logs it
	}
	int pops = 1; // coop
	if (name)
	{
		lua_pushstring(L, name);
		lua_rawget(L, -2);
		if (!lua_istable(L, -1))
		{
			lua_pop(L, 1);
			lua_newtable(L);
			lua_pushstring(L, name);
			lua_pushvalue(L, -2);
			lua_rawset(L, -4); // coop.<name> = the new table
		}
		++pops;
	}
	for (const luaL_Reg* f = fns; f->name; ++f)
	{
		lua_pushcfunction(L, f->func);
		lua_setfield(L, -2, f->name);
	}
	lua_pop(L, pops);
}
} // namespace

void register_lua(lua_State* L)
{
	static const luaL_Reg roles[] = {
		{"is_world_server", &l_is_world_server},
		{"is_map_authority", &l_is_map_authority},
		{"is_player", &l_is_player},
		{"is_dedicated", &l_is_dedicated},
		{"dedicated_new_world", &l_dedicated_new_world},
		{"is_mirror", &l_is_mirror},
		{"is_map_server", &l_is_map_server},
		{nullptr, nullptr},
	};
	static const luaL_Reg registry[] = {
		{"split", &l_split},
		{"story_together", &l_story_together},
		{"my_level", &l_my_level},
		{"level_of_player", &l_level_of_player},
		{"players_on", &l_players_on},
		{"led_by_host_process", &l_led_by_host_process},
		{"online_at_server", &l_online_at_server},
		{"holder_of", &l_holder_of},
		{"recall", &l_recall},
		{"far_place", &l_far_place},
		{"body_stays", &l_body_stays},
		{"level_name", &l_level_name},
		{"on_host_level", &l_on_host_level},
		{"map_servers", &l_map_servers},
		{"squads_at", &l_squads_at},
		{"carried", &l_carried},
		{"logic_put", &l_logic_put},
		{"logic_bytes", &l_logic_bytes},
		{"logic_look", &l_logic_look},
		{"bridge_of_player", &l_bridge_of_player},
		{"talking", &l_talking},
		{"set_gate", &l_set_gate},
		{"gate_of", &l_gate_of},
		{nullptr, nullptr},
	};
	fill(L, nullptr, roles);
	fill(L, "levels", registry);
}
} // namespace levels
} // namespace coop
