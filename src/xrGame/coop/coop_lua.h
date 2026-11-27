// Anomaly Coop: the Lua side of the coop.* API in C++. The one registrar of a table of C functions (fill, fill_global), the readers
// of the arguments a C function gets (they refuse, never raise: a wrong call from an addon gets nil / false instead of a Lua error,
// which this engine makes fatal) and the setters of a result table's fields. Header only, over the Lua C API: include it after the
// Lua headers. The Lua test runner (tests/lua/native_coop.cpp, tests/lua/test_lua_helpers.lua) tests it on the engine's LuaJIT.
#pragma once

#include <cmath>
#include <cstdint>
#include <string>

namespace coop
{
namespace lua_api
{
// ---------------------------------------------------------------- tables of functions

// Sets the functions of `fns` (ended by {nullptr, nullptr}) on the table at the stack top.
inline void set_functions(lua_State* L, const luaL_Reg* fns)
{
	for (const luaL_Reg* f = fns; f->name; ++f)
	{
		lua_pushcfunction(L, f->func);
		lua_setfield(L, -2, f->name);
	}
}

// Sets `fns` on coop.<name> (made here when it is not there), or on coop itself when `name` is null. -> false, nothing set, when the
// global table coop is not there (the caller says so in the log, or leaves it to the registrar that does).
inline bool fill(lua_State* L, const char* name, const luaL_Reg* fns)
{
	lua_pushstring(L, "coop");
	lua_rawget(L, LUA_GLOBALSINDEX);
	if (!lua_istable(L, -1))
	{
		lua_pop(L, 1);
		return false;
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
	set_functions(L, fns);
	lua_pop(L, pops);
	return true;
}

// Sets `fns` on the global table `name`, made here when it is not there.
inline void fill_global(lua_State* L, const char* name, const luaL_Reg* fns)
{
	lua_pushstring(L, name);
	lua_rawget(L, LUA_GLOBALSINDEX);
	if (!lua_istable(L, -1))
	{
		lua_pop(L, 1);
		lua_newtable(L);
		lua_pushstring(L, name);
		lua_pushvalue(L, -2);
		lua_rawset(L, LUA_GLOBALSINDEX);
	}
	set_functions(L, fns);
	lua_pop(L, 1);
}

// ---------------------------------------------------------------- arguments
// Each reader answers false for anything but a number of its kind (a string, nil, NaN, an infinity) and leaves `out` as it was then.
// The kinds differ on purpose, each is the rule of the functions that use it: a whole number, a number cut toward zero, any number in
// a range. Which one a function takes is part of its API.

// A finite number.
inline bool arg_number(lua_State* L, int idx, double& out)
{
	if (lua_type(L, idx) != LUA_TNUMBER)
		return false;
	const double v = double(lua_tonumber(L, idx));
	if (!std::isfinite(v))
		return false;
	out = v;
	return true;
}

// A whole number within [lo, hi] (a fraction is refused).
inline bool arg_whole(lua_State* L, int idx, double lo, double hi, double& out)
{
	double v = 0.0;
	if (!arg_number(L, idx, v) || v != std::floor(v) || v < lo || v > hi)
		return false;
	out = v;
	return true;
}

// A whole number 0 .. max.
inline bool arg_whole_u32(lua_State* L, int idx, double max, uint32_t& out)
{
	double v = 0.0;
	if (!arg_whole(L, idx, 0.0, max, v))
		return false;
	out = uint32_t(v);
	return true;
}

// An object id: a whole number 0 .. 0xfffe (0xffff is "no object").
inline bool arg_object_id(lua_State* L, int idx, uint16_t& out)
{
	uint32_t v = 0;
	if (!arg_whole_u32(L, idx, double(0xfffe), v))
		return false;
	out = uint16_t(v);
	return true;
}

// A number cut toward zero (as the engine's int setters cut it: give_money), the whole part within [lo, hi].
inline bool arg_trunc(lua_State* L, int idx, double lo, double hi, int64_t& out)
{
	double v = 0.0;
	if (!arg_number(L, idx, v))
		return false;
	const double whole = std::trunc(v);
	if (whole < lo || whole > hi)
		return false;
	out = int64_t(whole);
	return true;
}

// Any number within [lo, hi], fractions kept (the caller decides what a fraction means).
inline bool arg_number_in(lua_State* L, int idx, double lo, double hi, double& out)
{
	double v = 0.0;
	if (!arg_number(L, idx, v) || v < lo || v > hi)
		return false;
	out = v;
	return true;
}

// ---------------------------------------------------------------- fields of the table at the stack top

inline void set_bool(lua_State* L, const char* name, bool v)
{
	lua_pushboolean(L, v ? 1 : 0);
	lua_setfield(L, -2, name);
}

inline void set_num(lua_State* L, const char* name, double v)
{
	lua_pushnumber(L, lua_Number(v));
	lua_setfield(L, -2, name);
}

// A C string; null is set as "".
inline void set_str(lua_State* L, const char* name, const char* v)
{
	lua_pushstring(L, v ? v : "");
	lua_setfield(L, -2, name);
}

// Every byte of `v`, zeros included.
inline void set_str(lua_State* L, const char* name, const std::string& v)
{
	lua_pushlstring(L, v.data(), v.size());
	lua_setfield(L, -2, name);
}
} // namespace lua_api
} // namespace coop
