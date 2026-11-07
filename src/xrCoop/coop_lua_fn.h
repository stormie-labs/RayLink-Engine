#pragma once
// Anomaly Coop: Lua functions C++ keeps between calls (coop.sync handlers, on_change watches, effects, request callbacks) and the
// protected call of one of them that says why it did not run. Header only, over the Lua C API: include it after the Lua headers
// (lua.hpp). Used by xrGame/coop/coop_sync_api.cpp and by the Lua test runner (tests/lua/native_coop.cpp), on the engine's LuaJIT.

#include <cstddef>
#include <string>

namespace coop
{
namespace luafn
{
// The kept functions live in a table of their own, registry[kStoreKey], never in the registry's integer slots: luabind
// (3rd party/luabind/luabind/src/ref.cpp ref / unref, behind every luabind::object and functor the engine makes from Lua) hands
// those out with its own free list (registry[1]) and its own count (sizes[registry]), LuaJIT's luaL_ref with registry[0] and
// lua_objlen. The two gave one slot to two owners: 2026-11-07 a luabind object took the slots of the coop.dbg.self and
// coop.dbg.spawn handlers, whose calls then found a table and answered "handler error" without a line in the log.
constexpr const char* kStoreKey = "coop.lua_fn.kept";

// Pushes the store (made on first use) -> its index on the stack.
inline int push_store(lua_State* L)
{
	lua_pushstring(L, kStoreKey);
	lua_rawget(L, LUA_REGISTRYINDEX);
	if (!lua_istable(L, -1))
	{
		lua_pop(L, 1);
		lua_newtable(L);
		lua_pushstring(L, kStoreKey);
		lua_pushvalue(L, -2);
		lua_rawset(L, LUA_REGISTRYINDEX);
	}
	return lua_gettop(L);
}

// The function at `idx` kept: its reference, LUA_NOREF for anything but a function.
inline int keep(lua_State* L, int idx)
{
	if (lua_type(L, idx) != LUA_TFUNCTION)
		return LUA_NOREF;
	if (idx < 0 && idx > LUA_REGISTRYINDEX)
		idx = lua_gettop(L) + idx + 1;
	const int store = push_store(L);
	lua_pushvalue(L, idx);
	const int ref = luaL_ref(L, store);
	lua_pop(L, 1); // the store
	return ref;
}

// Pushes what `ref` holds now (nil for nothing).
inline void push(lua_State* L, int ref)
{
	const int store = push_store(L);
	lua_rawgeti(L, store, ref);
	lua_remove(L, store);
}

// The function kept as `ref` let go.
inline void drop(lua_State* L, int ref)
{
	if (!L || ref == LUA_NOREF || ref == LUA_REFNIL)
		return;
	const int store = push_store(L);
	luaL_unref(L, store, ref);
	lua_pop(L, 1);
}

// Is the value at `idx` the one kept as `ref`?
inline bool same(lua_State* L, int ref, int idx)
{
	if (idx < 0 && idx > LUA_REGISTRYINDEX)
		idx = lua_gettop(L) + idx + 1;
	push(L, ref);
	const bool yes = lua_rawequal(L, -1, idx) != 0;
	lua_pop(L, 1);
	return yes;
}

// Why a kept function was not called, or its call failed.
enum class Fail : unsigned char
{
	None,
	NoState, // no Lua state (no level yet)
	NoReference, // nothing was kept
	StackFull, // lua_checkstack refused
	NotFunction, // the reference holds no function any more
	LuaError, // the call raised an error
};
constexpr size_t kFailKinds = 6;
constexpr size_t kMaxReason = 160; // bytes of a reason an answer carries

inline const char* fail_text(Fail f)
{
	switch (f)
	{
	case Fail::None: return "ran";
	case Fail::NoState: return "no Lua state";
	case Fail::NoReference: return "no function kept";
	case Fail::StackFull: return "the Lua stack is full";
	case Fail::NotFunction: return "not a function";
	case Fail::LuaError: return "lua error";
	}
	return "?";
}

// The first line of a Lua error without the folders of its script ("coop_x.script:12: attempt to ..."), at most `max` bytes.
inline std::string first_line(const std::string& error, size_t max = kMaxReason)
{
	std::string s = error.substr(0, error.find_first_of("\r\n"));
	const size_t colon = s.find(": ");
	const size_t sep = s.find_last_of("\\/", colon == std::string::npos ? std::string::npos : colon);
	if (sep != std::string::npos && colon != std::string::npos)
		s.erase(0, sep + 1);
	while (!s.empty() && (s.back() == ' ' || s.back() == '\t'))
		s.pop_back();
	if (s.size() > max)
	{
		s.resize(max > 3 ? max - 3 : 0);
		s += "...";
	}
	return s.empty() ? std::string("?") : s;
}

// What the answer of a request says after "handler error: ": "not a function (number)", the first line of the Lua error, ...
inline std::string reason(Fail f, const std::string& error)
{
	switch (f)
	{
	case Fail::None: return std::string();
	case Fail::NotFunction: return std::string(fail_text(f)) + " (" + error + ")";
	case Fail::LuaError: return first_line(error);
	default: return fail_text(f);
	}
}

// Calls the function kept as `ref` with what push(L) pushed (it returns how many), protected: `errfunc` makes the message (0: the
// bare one), `nres` results for read(L) on success. The stack is left as it was. -> Fail::None, or why not; `error` gets the Lua
// error (with what errfunc added) or the type the reference held.
template <class Push, class Read>
Fail call(lua_State* L, int ref, lua_CFunction errfunc, Push push_args, int nres, Read read, std::string& error)
{
	error.clear();
	if (!L)
		return Fail::NoState;
	if (ref == LUA_NOREF || ref == LUA_REFNIL)
		return Fail::NoReference;
	const int top = lua_gettop(L);
	if (!lua_checkstack(L, 24))
		return Fail::StackFull;
	int ef = 0;
	if (errfunc)
	{
		lua_pushcfunction(L, errfunc);
		ef = lua_gettop(L);
	}
	push(L, ref);
	if (!lua_isfunction(L, -1))
	{
		error = lua_typename(L, lua_type(L, -1));
		lua_settop(L, top);
		return Fail::NotFunction;
	}
	const int nargs = push_args(L);
	if (lua_pcall(L, nargs, nres, ef) != 0)
	{
		const char* e = lua_tostring(L, -1);
		error = e ? e : "(error object is not a string)";
		lua_settop(L, top);
		return Fail::LuaError;
	}
	read(L);
	lua_settop(L, top);
	return Fail::None;
}
} // namespace luafn
} // namespace coop
