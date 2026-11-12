// Anomaly Coop (live 2026-11, owner: a universal protection, not one Lua guard per mod): a Lua error in an engine->Lua call
// (a UI functor, a dialog precondition, a script callback, a binder method) is survived while a co-op session is active and
// the console variable coop_lua_errors is 1 (the default): it is logged with its Lua stack once per place, repeats only as a
// counter, and the call returns a safe value (nothing / false / 0 / "" / nullptr / nil). Outside a session, or with 0, the
// stock fatal, unchanged. The bookkeeping is xrCoop/coop_lua_errors.h; luabind's call proxies ask take_token (error.hpp).
#pragma once

#include <cstdint>

struct lua_State;

extern int g_coop_lua_errors; // console coop_lua_errors (0..1, default 1)

namespace coop
{
namespace lua_errors
{
// In a co-op session with coop_lua_errors 1.
bool survive_now();
// CScriptEngine::lua_pcall_failed (luabind's pcall errfunc), first thing: true = logged and survived, the error object stays
// on top as the errfunc's result; false = the stock path (fatal).
bool on_pcall_error(lua_State* L);
// luabind's survive callback, asked after the failed pcall: true when the error on top is the one on_pcall_error survived on
// this thread (it takes the mark).
bool take_token(lua_State* L);
// The errors survived on this thread so far: a call site that must tell a survived error from a real false compares it
// before and after the call (CScriptBinderObjectWrapper::net_Spawn).
uint64_t survived_on_this_thread();

void register_console(); // coop_lua_errors
void print_status(); // coop_status: "[coop] lua errors: ..." and the places with the most errors
} // namespace lua_errors
} // namespace coop
