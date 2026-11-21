#include "stdafx.h"
#include "pch_script.h"
#include "coop_levels.h"
#include "coop_entities.h"
#include "coop_manager.h"
#include "srv/coop_dedicated.h"
#include "coop_join.h"
#include "srv/coop_mapserver.h"
#include "coop_money.h"
#include "coop_net.h"
#include "xrCoop/srv/coop_scope.h"
#include "xrCoop/coop_roster.h" // who is a player (plan 2026-11-12 stage 3)
#include "xrCoop/coop_mode.h" // coop.levels.online_at_server asks the mode's hook
#include "srv/coop_levels_map.h" // the rows of the LevelMap

#include "../../xrEngine/XR_IOConsole.h"
#include "../../xrEngine/xr_ioc_cmd.h"
#include "ai_space.h"
#include "game_graph.h"
#include "script_engine.h"
#include "alife_simulator.h"
#include "alife_object_registry.h"
#include "alife_graph_registry.h"
#include "xrServer_Objects_ALife.h"
#include "xrServer_Objects_ALife_Items.h"

#include <cmath>
#include <limits>
#include <map>
#include <string>

// F3a split levels: the switches, the questions "which level" and "who is on it", the host's table and LevelMap, the gates, the
// console and the Lua side. See coop_levels.h.

namespace coop
{
namespace levels
{
int g_coop_max_map_servers = int(kDefaultMaxMapServers); // build 50: the ceiling of the PC, the cap itself follows the players
int g_coop_mapserver_linger = int(kDefaultLingerMs / 1000);
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
static_assert(kMaxLevelId == (std::numeric_limits<GameGraph::_LEVEL_ID>::max)());

struct GateRow
{
	Gate gate = Gate::Free;
	std::string reason;
};

struct State
{
	// host
	Table table;
	bool map_due = false;
	u64 map_sent_at = 0;
	bool host_split = false; // the server mode as the world took it: it applies at the world's start (plan §3)
	std::map<std::string, GateRow> gates; // by level name, from Lua (coop_levels.script)
	// client: the host's last table
	wire::LevelMapMsg view;
	bool view_known = false;
	u32 map_trust_fallbacks = 0;
	LevelId echo = wire::kNoLevel; // EchoOf
} g;

// Split levels are the server mode's (owner 2026-11-18, no hybrid): a dedicated host has no level to play on, every level with
// players has a map server. The co-op never splits.
bool game_split() { return dedicated::on(); }

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

// The world server's own seat: its level (fake_start), and no player on it (it plays nobody, it leads no level).
void host_seat_self()
{
	const LevelId mine = my_level();
	if (mine == wire::kNoLevel)
		return;
	const Seat* s = g.table.find(kHostPeer);
	if (s && s->level == mine)
		return;
	g.table.set({kHostPeer, mine, true, false});
	g.map_due = true;
}

void host_seat(PeerId peer, LevelId level, bool in_world)
{
	g.table.set({peer, level, in_world, true});
	g.map_due = true;
}

void host_send_map()
{
	wire::LevelMapMsg m;
	m.flags = wire::LevelMapMsg::Split; // only split levels have a map (frame, host_send_map_now)
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
		row.state = u8(mapserver::host_state_of(level));
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
	// Not through the join queue: LevelMap needs no world (coop_world_gate.h Free), and a map server takes its players from it. The
	// session's broadcast: every joined player, then every trunk of the session that takes it (a world message: a map server does,
	// SessionPart::trunk_takes); this process has no host player (A-S4Sb: the same peers as the two loops of players and bridges).
	mgr().session().broadcast(Lane::Reliable, Msg::LevelMap, w);
	g.map_due = false;
	g.map_sent_at = mgr().now_ms();
}

const char* level_map_check_name(LevelMapCheck check)
{
	switch (check)
	{
	case LevelMapCheck::Sender: return "level_map_sender";
	case LevelMapCheck::Phase: return "level_map_phase";
	case LevelMapCheck::Payload: return "level_map_payload";
	case LevelMapCheck::LevelIds: return "level_map_level_id";
	}
	return "level_map";
}

// These are data guards. coop_trust accounts for the verdict; its setting cannot let rejected data through.
void account_level_map(PeerId from, LevelMapCheck check, bool ok)
{
	const char* name = level_map_check_name(check);
	lua_State* L = g_ai_space ? ai().script_engine().lua() : nullptr;
	if (L)
	{
		constexpr char chunk[] = "local name, ok, from = ...\n"
		                         "coop_trust.pass(name, ok, 'LevelMap from transport peer %u', from)";
		const int top = lua_gettop(L);
		bool reported = false;
		if (luaL_loadbuffer(L, chunk, sizeof(chunk) - 1, "@coop_level_map_trust") == 0)
		{
			lua_pushstring(L, name);
			lua_pushboolean(L, ok);
			lua_pushnumber(L, lua_Number(from));
			reported = lua_pcall(L, 3, 0, 0) == 0;
		}
		lua_settop(L, top);
		if (reported)
			return;
	}
	if (!ok && ++g.map_trust_fallbacks <= 3)
		::Msg("! [coop-trust] %s: dropped LevelMap from transport peer %u", name, from);
}

void client_on_level_map(PeerId from, Reader& r)
{
	if (!receive_level_map(from, net::client(), mgr().world_session().is_active(), r, g.view,
	        [from](LevelMapCheck check, bool ok) { account_level_map(from, check, ok); }))
		return;
	g.view_known = true;
	mapserver::server_on_map(g.view);
}
} // namespace

const char* level_name(LevelId level) { return level_name_of(level); }

bool split()
{
	if (net::client())
		return mgr().world_session().session_mode() == SessionMode::Server; // S4: the Welcome says it, not the first LevelMap
	return mgr().is_host() ? g.host_split : game_split();
}

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
	if (net::client() && mode().leads_level())
	{
		for (const wire::LevelMapMsg::Player& p : g.view.players)
			if (p.peer == peer)
				return p.level;
		return wire::kNoLevel;
	}
	if (!mgr().active())
		return wire::kNoLevel;
	// the host is a player of its own session, so is this machine whatever its slot (a client's own slot is not among
	// the peers its session lists); the others are the peers the session knows
	if (peer != kHostPeer && peer != mgr().self() && !mgr().session().peer(peer))
		return wire::kNoLevel;
	if (!split())
		return my_level(); // one world, one level for everybody
	if (peer == mgr().self() && net::client())
		return my_level();
	if (mode().decides_world())
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

u32 players_on(LevelId level)
{
	if (!mgr().active() || level == wire::kNoLevel)
		return 0;
	if (split())
	{
		u32 n = 0;
		if (mode().decides_world())
			return u32(g.table.population(level));
		for (const wire::LevelMapMsg::Player& p : g.view.players)
			n += p.level == level ? 1u : 0u;
		return n;
	}
	if (level != my_level())
		return 0;
	// the peers() of the host are the clients, of a client the host and the others: this machine's own player is one more, unless it is
	// a host whose seat plays no character; a seat that does not play is nobody (coop_roster.h)
	const u32 self = mode().decides_world() && !mode().actor_is_player() ? 0u : 1u;
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
	if (g.echo != wire::kNoLevel && mgr().active() && mode().decides_world() && level_of_player(peer) == g.echo)
		return false;
	return level_passes(type, split(), on_host_level(peer));
}

EchoOf::EchoOf(LevelId level) { g.echo = level; }
EchoOf::~EchoOf() { g.echo = wire::kNoLevel; }
LevelId echo_now() { return g.echo; }

bool host_player_loads(PeerId peer, LevelId level)
{
	if (!mgr().is_host() || !split() || level == wire::kNoLevel)
		return false;
	host_seat_self();
	const bool stays = mapserver::host_can_serve(level, host_map_cap(peer), peer);
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

// Called for a LevelMove only, which the host takes with split levels alone (join's host_on_level_move): the gate as split levels
// fold it, nobody is waited for.
MoveVerdict host_judge_move(PeerId peer, LevelId target)
{
	host_seat_self();
	const Gate gate = effective_gate(gate_of_level(target), true);
	const bool new_server = !mapserver::host_serves(target);
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

void host_map_changed() { g.map_due = true; }

void host_world_replaced()
{
	if (!mgr().active() || !mode().decides_world())
		return;
	const bool was = g.host_split;
	g.host_split = game_split();
	if (!was && !g.host_split)
		return;
	xfer_reset(); // the simulator that had the marks is gone; the map servers connect anew when they are Ready
	mapserver::host_world_replaced(join::host_world_epoch());
	g.map_due = true;
}

void host_send_map_now()
{
	if (mgr().is_host() && split())
		host_send_map();
}

Gate gate_of_level(LevelId level)
{
	if (net::client())
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

// S4 (docs/MODES.md section 4, P-E): the server's tables are the server modes' (srv/coop_srv_mode.cpp): the world server's in-filter
// (mapserver::host_incoming) and the transfers' tables (xfer_install_server / xfer_install_client). A co-op process installs none.
void install_server(Session&) {}

void install_client(Endpoint& e) { e.on(Msg::LevelMap, &client_on_level_map); }

void reset()
{
	g.table.clear();
	g.map_due = false;
	g.map_sent_at = 0;
	g.host_split = game_split();
	g.gates.clear();
	g.view = wire::LevelMapMsg();
	g.view_known = false;
	mapserver::reset();
	xfer_reset();
}

void on_peer_left(PeerId id)
{
	if (!mgr().active() || !mode().decides_world())
		return;
	g.table.remove(id);
	if (split())
		g.map_due = true;
}

// The server modes' frame (srv/coop_srv_mode.cpp; S4, A-S4H): a co-op process never runs it. A map server's step waits for its world's
// Welcome itself (mapserver::server_frame).
void frame()
{
	settle_machine_settings(); // user.ltx is read by now
	if (mgr().active() && mode().decides_world())
	{
		host_seat_self();
		mapserver::host_frame();
		const u64 now = mgr().now_ms();
		if (g.map_due || now - g.map_sent_at >= kMapEveryMs)
			host_send_map();
		xfer_host_frame();
	}
	else if (mgr().active() && mode().leads_level())
	{
		xfer_client_frame();
		mapserver::server_frame();
	}
}

void print_status()
{
	::Msg("[coop] levels: split %s | this level %u, players on it %u, this process %s", split() ? "on" : "off",
	    u32(my_level()), players_on(my_level()),
	    mgr().is_host() ? "hosts" : (leads_my_level() ? "is a map server" : "mirrors"));
	if (mgr().is_host())
		for (const Seat& s : g.table.seats())
			::Msg("[coop] levels:   #%u on level %u [%s]%s%s", s.peer, u32(s.level), level_name_of(s.level) ? level_name_of(s.level) : "?",
			    s.in_world ? "" : " loading", s.plays ? ", a map server leads it" : ", plays nobody");
	if (mgr().is_host() && split())
		::Msg("[coop] levels: map servers cap %u = min(players %u + 1, coop_max_map_servers %d, %u), linger %d s for the map emptied last, "
		      "at most %u s for the others",
		    host_map_cap(), host_map_players_now(), g_coop_max_map_servers, u32(mapserver::kMaxBridges), g_coop_mapserver_linger,
		    kColdLingerMs / 1000);
	mapserver::print_status();
	xfer_print_status();
}

// The console commands of the levels (srv_levels_status, srv_levels_audit, srv_max_map_servers, srv_mapserver_linger,
// srv_map_servers_rev and their old coop_* names) are the server's: srv/coop_srv_console.cpp (S4, A-S4H).

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
	lua_pushboolean(L, mgr().active() && mode().decides_world());
	return 1;
}
int l_is_map_authority(lua_State* L)
{
	lua_pushboolean(L, net::host());
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
	lua_pushboolean(L, net::client() && mode().leads_level());
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
// coop.levels.talk_partner(peer) -> the host's process: the id of the NPC he talks to there (the world server's numbering), as that
// server last said (LevelBodyFacts.partner, build 57, protocol 33); nil when he talks to nobody there, or it has said nothing for a while
int push_talk(lua_State* L, bool partner)
{
	u32 peer = 0;
	BodyFacts facts;
	const bool told = arg_whole(L, 1, double(kMaxPeerSlots - 1), peer) && mgr().is_host() && body_facts(join::proxy_of_peer(PeerId(peer)), facts);
	if (!partner)
		lua_pushboolean(L, told && facts.talking);
	else if (told && facts.talking && facts.partner != wire::kNoObject)
		lua_pushnumber(L, lua_Number(facts.partner));
	else
		lua_pushnil(L);
	return 1;
}
int l_talking(lua_State* L) { return push_talk(L, false); }
int l_talk_partner(lua_State* L) { return push_talk(L, true); }
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
	if (!net::client() || mode().leads_level() || !ai().get_alife() || !lua_isnumber(L, 2) || !lua_isnumber(L, 3) || !lua_isnumber(L, 4))
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
// coop.levels.online_at_server(id): the object is online at a map server, nothing of it is here (the mode's hook, as the switch manager
// asks it: S4, A-S4H)
int l_online_at_server(lua_State* L)
{
	u32 id = 0;
	lua_pushboolean(L, arg_whole(L, 1, double(0xfffe), id) && mode().online_elsewhere(u16(id)));
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
	if (!name || !word || !parse_gate(word, gate) || !mgr().active() || !mode().decides_world())
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
// coop.levels.gate_of(level name): the gate as split levels fold it, a word; nil for a level that does not exist
int l_gate_of(lua_State* L)
{
	const LevelId level = lua_type(L, 1) == LUA_TSTRING ? level_by_name(lua_tostring(L, 1)) : wire::kNoLevel;
	if (level == wire::kNoLevel)
		lua_pushnil(L);
	else
		lua_pushstring(L, gate_name(effective_gate(gate_of_level(level), true)));
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
		{"my_level", &l_my_level},
		{"level_of_player", &l_level_of_player},
		{"players_on", &l_players_on},
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
		{"talk_partner", &l_talk_partner},
		{"set_gate", &l_set_gate},
		{"gate_of", &l_gate_of},
		{nullptr, nullptr},
	};
	fill(L, nullptr, roles);
	fill(L, "levels", registry);
}
} // namespace levels
} // namespace coop
