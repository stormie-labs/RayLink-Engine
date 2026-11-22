// Сервер: только сервер мира и серверы карт (docs/MODES.md)
// Anomaly Coop — the Lua table srv.* (docs/MODES.md section 4 "Lua"; step S4 of docs/plans/2026-11-18-two-modes.md, unit A-S4H): the
// predicates of the server's mode, for the server's layer (scripts\coop_srv_mode.script) and the server's scripts:
//   srv.is_dedicated()          this process was started with -coop_dedicated: the world server, from its start
//   srv.dedicated_new_world()   that world server makes a new world
//   srv.is_map_server()         this process is a map server (-coop_mapserver), in its session
//   srv.is_world_server()       this process is the world server (-coop_dedicated) and decides the world of its session; a co-op host
//                               decides its world too (its roles hold WorldServer) and is answered false (review A-REV-E-1, finding 5)
//   srv.split()                 the session is the server's: its players stand on different maps
// They answer as their copies in coop.* (coop.is_dedicated, coop.dedicated_new_world, coop.is_map_server, coop.is_world_server,
// coop.levels.split: coop_levels.cpp), which stay until step S8 (A-S8) and go once no script reads them; is_world_server is the copy's
// answer in a -coop_dedicated process only. In a co-op process and in single player the table is there too and answers false: the
// server's layer asks it in every process.
// No argument is read: an addon that passes one gets the same answer. Plain C functions, as in coop_levels.cpp: no luabind overload
// errors (R-11.4).
// Made every time the script engine makes its Lua state, with the Core's coop.* (coop_script.cpp), by the server's layer
// (srv/coop_srv_mode.cpp ServerLayer::register_lua, through the switchboard: coop_mode.cpp register_layers_lua).
#include "stdafx.h"
#include "pch_script.h"
#include "coop/coop_manager.h"
#include "coop/srv/coop_dedicated.h"
#include "coop/coop_levels.h"

namespace coop
{
namespace srv
{
namespace
{
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
int l_is_map_server(lua_State* L)
{
	lua_pushboolean(L, is_map_server());
	return 1;
}
int l_is_world_server(lua_State* L)
{
	lua_pushboolean(L, dedicated::on() && is_world_server());
	return 1;
}
int l_split(lua_State* L)
{
	lua_pushboolean(L, levels::split());
	return 1;
}
} // namespace

void register_lua(lua_State* L)
{
	static const luaL_Reg fns[] = {
		{"is_dedicated", &l_is_dedicated},
		{"dedicated_new_world", &l_dedicated_new_world},
		{"is_map_server", &l_is_map_server},
		{"is_world_server", &l_is_world_server},
		{"split", &l_split},
		{nullptr, nullptr},
	};
	// the global table srv, made here when it is not there
	lua_pushstring(L, "srv");
	lua_rawget(L, LUA_GLOBALSINDEX);
	if (!lua_istable(L, -1))
	{
		lua_pop(L, 1);
		lua_newtable(L);
		lua_pushstring(L, "srv");
		lua_pushvalue(L, -2);
		lua_rawset(L, LUA_GLOBALSINDEX);
	}
	for (const luaL_Reg* f = fns; f->name; ++f)
	{
		lua_pushcfunction(L, f->func);
		lua_setfield(L, -2, f->name);
	}
	lua_pop(L, 1);
}
} // namespace srv
} // namespace coop
