// Anomaly Coop (live 2026-11, owner: a universal protection, not one patch per mod): Lua errors survived in a co-op session.
// See coop_lua_errors.h here and xrCoop/coop_lua_errors.h (the bookkeeping, the lines, the unit tests).
// The errfunc of luabind's pcall (CScriptEngine::lua_pcall_failed) runs while the erroring stack still exists: on_pcall_error
// logs there and marks the error object (a token: the thread's lua_State and the error value). After lua_pcall returns, the
// call proxy asks take_token (luabind's survive callback): the same object on top = survive, the proxy drops it and returns a
// safe value. A failure that did not pass through this errfunc (an out-of-memory error, an error in the errfunc, a coroutine
// resume, an uninitialised functor) finds no token and goes the stock way (CScriptEngine::lua_error -> fatal).
// Script calls may run on the GameThread worker: the ledger is under g_lock (never held across a call out), the token is per
// thread.
#include "stdafx.h"
#include "pch_script.h"
#include "coop_lua_errors.h"
#include "coop_manager.h"
#include "xrCoop/coop_lua_errors.h"

#include "script_engine.h"

#include "../../xrEngine/XR_IOConsole.h"
#include "../../xrEngine/xr_ioc_cmd.h"

#include <string>

int g_coop_lua_errors = 1; // console coop_lua_errors: 1 = survive Lua errors in a co-op session, 0 = stock (fatal)

namespace coop
{
namespace lua_errors
{
namespace
{
xrCriticalSection g_lock;
Ledger g_ledger;

struct Token
{
	lua_State* L = nullptr;
	int type = LUA_TNONE;
	const void* value = nullptr;
};
thread_local Token t_token;
thread_local uint64_t t_survived = 0;

// The identity of the value at `index`: a string by its interned text, a table / function / userdata by its address.
const void* identity(lua_State* L, int index)
{
	if (lua_type(L, index) == LUA_TSTRING)
		return lua_tostring(L, index);
	return lua_topointer(L, index);
}

// The place of the error: the first Lua frame of the erroring stack.
std::string place_of(lua_State* L)
{
	lua_Debug ar;
	for (int i = 0; lua_getstack(L, i, &ar); ++i)
	{
		lua_getinfo(L, "Sl", &ar);
		if (ar.what && (!xr_strcmp(ar.what, "Lua") || !xr_strcmp(ar.what, "main")))
			return place_key(ar.short_src, ar.currentline);
	}
	return kNoLuaFrame;
}

// The frames of the erroring stack as get_lua_stack (script_engine.cpp) writes them, at most kMaxStackLines.
void frames_of(lua_State* L, xr_vector<xr_string>& frames)
{
	lua_Debug ar;
	for (int i = 0; frames.size() < kMaxStackLines && lua_getstack(L, i, &ar); ++i)
	{
		lua_getinfo(L, "nSlu", &ar);
		if (!ar.name)
			frames.push_back(make_string("%2d : [%s] %s(%d) : %s", i, ar.what, ar.short_src, ar.currentline, "").c_str());
		else if (!xr_strcmp(ar.what, "C"))
			frames.push_back(make_string("%2d : [C  ] %s", i, ar.name).c_str());
		else
			frames.push_back(
			    make_string("%2d : [%s] %s(%d) : %s", i, ar.what, ar.short_src, ar.currentline, ar.name).c_str());
	}
}
} // namespace

bool survive_now() { return survive(mgr().active(), g_coop_lua_errors); }

bool on_pcall_error(lua_State* L)
{
	if (!L || !survive_now())
		return false;
	const std::string place = place_of(L);
	const char* message = lua_isstring(L, -1) ? lua_tostring(L, -1) : nullptr;

	Note note;
	{
		xrCriticalSectionGuard guard(g_lock);
		note = g_ledger.note(place, GetTickCount64());
	}
	if (note.log == Log::Full)
	{
		xr_vector<xr_string> frames; // only for the first error of a place: a callback failing every frame costs no strings
		frames_of(L, frames);
		CScriptEngine::print_output(L, "", LUA_ERRRUN); // "! [SCRIPT ERROR]: <error>" as the stock path writes it
		::Msg("! [LUA] SCRIPT RUNTIME ERROR, stack (survived in a co-op session, coop_lua_errors 1):");
		for (const xr_string& f : frames)
			::Msg("%s", stack_line(f.c_str()).c_str());
		::Msg("%s", survived_line(note, message).c_str());
	}
	else if (note.log == Log::Summary)
		::Msg("%s", survived_line(note, message).c_str());

	++t_survived;
	t_token.L = L;
	t_token.type = lua_type(L, -1);
	t_token.value = identity(L, -1);
	return true;
}

bool take_token(lua_State* L)
{
	const Token t = t_token;
	t_token = Token();
	if (!L || t.L != L || lua_gettop(L) < 1)
		return false;
	return lua_type(L, -1) == t.type && identity(L, -1) == t.value;
}

uint64_t survived_on_this_thread() { return t_survived; }

void register_console()
{
	// 1 = a Lua error in an engine->Lua call is logged and survived while a co-op session is active (host, client, map
	// server); 0 = the stock fatal. Outside a session it is always the stock fatal.
	CMD4(CCC_Integer, "coop_lua_errors", &g_coop_lua_errors, 0, 1);
}

void print_status()
{
	std::string head;
	std::vector<Top> top;
	{
		xrCriticalSectionGuard guard(g_lock);
		head = status_line(g_ledger, g_coop_lua_errors, mgr().active());
		top = g_ledger.top(5);
	}
	::Msg("%s", head.c_str());
	for (const Top& t : top)
		::Msg("%s", status_place_line(t).c_str());
}
} // namespace lua_errors
} // namespace coop
