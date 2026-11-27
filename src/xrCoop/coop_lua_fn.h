#pragma once
// Anomaly Coop: how C++ calls Lua. Lua functions C++ keeps between calls (coop.sync handlers, on_change watches, effects, request
// callbacks), the protected call of a kept one (call) or of <module>.<fn> by name (call_named), both saying why they did not run, and
// the one message handler of those calls (traceback). Header only, over the Lua C API: include it after the Lua headers (lua.hpp).
// Used by xrGame/coop/coop_sync_api.cpp and by the Lua test runner (tests/lua/native_coop.cpp), on the engine's LuaJIT.

#include <cstddef>
#include <string>

namespace coop
{
namespace luafn
{
// The kept functions live in a table of their own, registry[kStoreKey], never in the registry's integer slots: luabind
// (3rd party/luabind/luabind/src/ref.cpp ref / unref, behind every luabind::object and functor the engine makes from Lua) hands
// those out with its own free list (registry[1]) and its own count (sizes[registry]), LuaJIT's luaL_ref with registry[0] and
// lua_objlen. The two would give one slot to two owners: a luabind object could take the slot of a kept handler, whose
// call then finds another value and fails without a line in the log.
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
	Missing, // call_named: no such module, or no such function in it
};
constexpr size_t kFailKinds = 7;
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
	case Fail::Missing: return "missing";
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
	case Fail::NotFunction:
	case Fail::Missing: return std::string(fail_text(f)) + " (" + error + ")";
	case Fail::LuaError: return first_line(error);
	default: return fail_text(f);
	}
}

// The message handler of these calls: the error with debug.traceback of the erroring stack (from its level 2), the bare error when
// debug.traceback is not there.
inline int traceback(lua_State* L)
{
	const char* msg = lua_tostring(L, 1);
	if (!msg)
		msg = "(error object is not a string)";
	lua_pushstring(L, "debug");
	lua_rawget(L, LUA_GLOBALSINDEX);
	if (!lua_istable(L, -1))
	{
		lua_pop(L, 1);
		lua_pushstring(L, msg);
		return 1;
	}
	lua_getfield(L, -1, "traceback");
	if (!lua_isfunction(L, -1))
	{
		lua_pop(L, 2);
		lua_pushstring(L, msg);
		return 1;
	}
	lua_pushstring(L, msg);
	lua_pushinteger(L, 2);
	lua_call(L, 2, 1);
	return 1;
}

namespace detail
{
constexpr int kCallSlots = 24; // stack slots a call reserves: the handler, the function, its arguments

// Under lua_pcall: arg 1 the module's name, arg 2 the function's name -> the function; or nil and what was found instead
// ("<module>: <type>", "<module>.<fn>: <type>"). The module is read through _G, whose __index loads gamedata/scripts/<module>.script
// on first use, so a script that fails to load raises here, protected.
inline int find_named(lua_State* L)
{
	const char* module = lua_tostring(L, 1);
	const char* fn = lua_tostring(L, 2);
	lua_getglobal(L, module);
	if (!lua_istable(L, -1))
	{
		lua_pushnil(L);
		lua_pushfstring(L, "%s: %s", module, luaL_typename(L, -2));
		return 2;
	}
	lua_getfield(L, -1, fn);
	if (!lua_isfunction(L, -1))
	{
		lua_pushnil(L);
		lua_pushfstring(L, "%s.%s: %s", module, fn, luaL_typename(L, -2));
		return 2;
	}
	return 1;
}

// The function is on the stack top, the handler (if any) at `ef`: pushes the arguments, calls, reads, puts the stack back to `top`.
template <class Push, class Read>
Fail run(lua_State* L, int top, int ef, Push& push_args, int nres, Read& read, std::string& error)
{
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

// Pushes `errfunc` (nothing for 0) -> its index, 0 for none.
inline int push_handler(lua_State* L, lua_CFunction errfunc)
{
	if (!errfunc)
		return 0;
	lua_pushcfunction(L, errfunc);
	return lua_gettop(L);
}
} // namespace detail

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
	if (!lua_checkstack(L, detail::kCallSlots))
		return Fail::StackFull;
	const int ef = detail::push_handler(L, errfunc);
	push(L, ref);
	if (!lua_isfunction(L, -1))
	{
		error = lua_typename(L, lua_type(L, -1));
		lua_settop(L, top);
		return Fail::NotFunction;
	}
	return detail::run(L, top, ef, push_args, nres, read, error);
}

// Calls <module>.<fn> (the global table `module`, a script of gamedata/scripts loaded on first use) as call() calls a kept function.
// -> Fail::Missing when there is no such module or function (`error`: what was found instead, "coop_x: nil", "coop_x.fn: nil"),
// Fail::LuaError when the call raised, or the module's script raised while it loaded.
template <class Push, class Read>
Fail call_named(lua_State* L, const char* module, const char* fn, lua_CFunction errfunc, Push push_args, int nres, Read read,
    std::string& error)
{
	error.clear();
	if (!L)
		return Fail::NoState;
	const int top = lua_gettop(L);
	if (!lua_checkstack(L, detail::kCallSlots))
		return Fail::StackFull;
	const int ef = detail::push_handler(L, errfunc);
	lua_pushcfunction(L, &detail::find_named);
	lua_pushstring(L, module);
	lua_pushstring(L, fn);
	if (lua_pcall(L, 2, 2, ef) != 0)
	{
		const char* e = lua_tostring(L, -1);
		error = e ? e : "(error object is not a string)";
		lua_settop(L, top);
		return Fail::LuaError;
	}
	if (!lua_isfunction(L, -2))
	{
		const char* found = lua_tostring(L, -1);
		error = found ? found : "?";
		lua_settop(L, top);
		return Fail::Missing;
	}
	lua_pop(L, 1); // the function stays on top
	return detail::run(L, top, ef, push_args, nres, read, error);
}
} // namespace luafn
} // namespace coop
