#include "stdafx.h"
#include "pch_script.h"
#include "coop_levels.h"
#include "coop_manager.h"

#include "../../xrEngine/XR_IOConsole.h"
#include "../../xrEngine/xr_ioc_cmd.h"
#include "ai_space.h"
#include "game_graph.h"
#include "alife_simulator.h"
#include "alife_object_registry.h"
#include "alife_graph_registry.h"
#include "xrServer_Objects_ALife.h"

#include <cmath>

// F3a split levels, the skeleton (T1): the switches, the questions "which level" and "who is on it" with the answers of a
// session without split levels, the console and the Lua side. The registry itself is T10, see coop_levels.h.

namespace coop
{
namespace levels
{
int g_coop_split_levels = 1;
int g_coop_story_together = 1;
int g_coop_max_map_servers = 3;
int g_coop_mapserver_linger = 60;

bool split() { return g_coop_split_levels != 0; }

bool story_together() { return g_coop_story_together != 0; }

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
	if (!ai().get_alife())
		return wire::kNoLevel;
	const CSE_ALifeDynamicObject* e = ai().alife().objects().object(id, true);
	return e ? level_of_vertex(e->m_tGraphID) : wire::kNoLevel;
}

LevelId level_of_player(PeerId peer)
{
	if (!is_player())
		return wire::kNoLevel;
	// the host is a player of its own session; the others are the peers the session knows
	if (peer != kHostPeer && !mgr().session().peer(peer))
		return wire::kNoLevel;
	return my_level(); // skeleton: one world, one level for everybody
}

bool led_by_host_process(LevelId level)
{
	return level != wire::kNoLevel && level == my_level();
}

u32 players_on(LevelId level)
{
	if (!is_player() || level == wire::kNoLevel || level != my_level())
		return 0;
	// the peers() of the host are the clients, of a client the host and the others: this machine's own player is one more
	return 1 + u32(mgr().session().peers().size());
}

bool online_at_server(u16)
{
	return false; // the marks come with the transfers (T10/T11, coop_levels_xfer.cpp)
}

void install(Session&) {}

void reset() {}

void on_peer_left(PeerId) {}

void frame() {}

void print_status()
{
	::Msg("[coop] levels: split %s, story together %s, map servers up to %d, linger %d s | this level %u, players on it %u "
	      "(skeleton, the registry comes with F3a T10)",
	    split() ? "on" : "off", story_together() ? "on" : "off", g_coop_max_map_servers, g_coop_mapserver_linger,
	    u32(my_level()), players_on(my_level()));
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
		::Msg("! [coop] levels audit: the auditor of ownership comes with F3a T11, nothing to check yet");
	}
	void Info(TInfo& I) override { xr_strcpy(I, "split levels: check now that every object has exactly one owner"); }
};
} // namespace

void register_console()
{
	CMD4(CCC_Integer, "coop_split_levels", &g_coop_split_levels, 0, 1); // game rule, applies at the world's start
	CMD4(CCC_Integer, "coop_story_together", &g_coop_story_together, 0, 1); // game rule
	CMD4(CCC_Integer, "coop_max_map_servers", &g_coop_max_map_servers, 0, 7); // machine setting
	CMD4(CCC_Integer, "coop_mapserver_linger", &g_coop_mapserver_linger, 0, 3600); // machine setting, seconds
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
	if (!arg_whole(L, 1, double(kMaxPlayers - 1), peer))
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
int l_online_at_server(lua_State* L)
{
	u32 id = 0;
	lua_pushboolean(L, arg_whole(L, 1, double(0xfffe), id) && online_at_server(u16(id)));
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
		{"is_mirror", &l_is_mirror},
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
		{nullptr, nullptr},
	};
	fill(L, nullptr, roles);
	fill(L, "levels", registry);
}
} // namespace levels
} // namespace coop
