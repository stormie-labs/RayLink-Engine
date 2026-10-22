#include "stdafx.h"
#include "pch_script.h"
#include "coop_checkpoints.h"
#include "coop_rest.h"

#include <cmath>

// Lua side of the F3 modules: coop.checkpoint (here, F3 Tasks 6 and 7) and coop.rest (coop_rest.cpp, F3 Task 11).
// Functions with arguments are plain C functions, as in coop_script.cpp: a wrong argument from an addon returns
// nil / false instead of a luabind "no matching overload" error, which is fatal in this engine (R-11.4).

namespace coop
{
namespace checkpoints
{
namespace
{
constexpr size_t kMaxReason = 128; // a story reason is a string id ("st_save_..."), only logged

// A character id from argument `idx`: a whole number 0..0xfffe (0xffff is "no object").
bool arg_character(lua_State* L, int idx, u16& out)
{
	if (lua_type(L, idx) != LUA_TNUMBER)
		return false;
	const double v = double(lua_tonumber(L, idx));
	if (!std::isfinite(v) || v < 0.0 || v >= double(0xffff) || v != std::floor(v))
		return false;
	out = u16(v);
	return true;
}

// coop.checkpoint.get(local_id) -> nil | {kind = "campfire"|"rest"|"tent"|"arrival"|"story"|"mod", level, x, y, z,
// source = local id | nil}: this player's own checkpoint (0 = me) or another one the host shares.
int l_get(lua_State* L)
{
	u16 id = 0;
	checkpoint::Record r;
	if (!arg_character(L, 1, id) || !get(id, r) || !r.valid())
	{
		lua_pushnil(L);
		return 1;
	}
	lua_createtable(L, 0, 6);
	lua_pushstring(L, checkpoint::kind_name(r.kind));
	lua_setfield(L, -2, "kind");
	lua_pushstring(L, r.level.c_str());
	lua_setfield(L, -2, "level");
	lua_pushnumber(L, lua_Number(r.pos[0]));
	lua_setfield(L, -2, "x");
	lua_pushnumber(L, lua_Number(r.pos[1]));
	lua_setfield(L, -2, "y");
	lua_pushnumber(L, lua_Number(r.pos[2]));
	lua_setfield(L, -2, "z");
	if (r.source != checkpoint::kNoSource)
	{
		lua_pushnumber(L, lua_Number(r.source));
		lua_setfield(L, -2, "source");
	}
	return 1;
}

// coop.checkpoint.story([reason]) -> bool: host, a story checkpoint for every present living player (false on a
// client, without a session, with a reason that is not a string of at most 128 bytes, or when nobody got one).
int l_story(lua_State* L)
{
	const char* reason = "";
	const int t = lua_type(L, 1);
	if (t == LUA_TSTRING)
	{
		size_t n = 0;
		reason = lua_tolstring(L, 1, &n);
		if (n > kMaxReason)
		{
			lua_pushboolean(L, 0);
			return 1;
		}
	}
	else if (t != LUA_TNONE && t != LUA_TNIL)
	{
		lua_pushboolean(L, 0);
		return 1;
	}
	lua_pushboolean(L, host_story(reason) ? 1 : 0);
	return 1;
}
} // namespace

// Fills coop.checkpoint (made here if no one made it yet: F3 Task 7 adds its functions to the same table).
void register_lua(lua_State* L)
{
	static const luaL_Reg fns[] = {
		{"get", &l_get},
		{"story", &l_story},
		{nullptr, nullptr},
	};
	lua_pushstring(L, "coop");
	lua_rawget(L, LUA_GLOBALSINDEX);
	if (!lua_istable(L, -1))
	{
		lua_pop(L, 1);
		return; // the registrator logs it
	}
	lua_pushstring(L, "checkpoint");
	lua_rawget(L, -2);
	if (!lua_istable(L, -1))
	{
		lua_pop(L, 1);
		lua_newtable(L);
		lua_pushstring(L, "checkpoint");
		lua_pushvalue(L, -2);
		lua_rawset(L, -4); // coop.checkpoint = the new table
	}
	for (const luaL_Reg* f = fns; f->name; ++f)
	{
		lua_pushcfunction(L, f->func);
		lua_setfield(L, -2, f->name);
	}
	lua_pop(L, 2); // coop.checkpoint, coop
}
} // namespace checkpoints
} // namespace coop

#pragma optimize("s", on)
void coop_checkpoints_script_registrator::script_register(lua_State* L)
{
	// export_classes runs this right after coop_script_registrator, which made the `coop` table.
	lua_pushstring(L, "coop");
	lua_rawget(L, LUA_GLOBALSINDEX);
	const bool have_coop = lua_istable(L, -1);
	lua_pop(L, 1);
	if (!have_coop)
	{
		::Msg("! [coop] script namespace 'coop' was not created, coop.checkpoint and coop.rest unavailable");
		return;
	}
	coop::checkpoints::register_lua(L);
	coop::rest::register_lua(L);
}
