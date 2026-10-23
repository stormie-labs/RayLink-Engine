// Anomaly Coop — callback router (MOD_LAYER.md §2.2, F2 T10, review R5 I4). Decides on which machine each Lua handler
// of a callback runs. The table and its rules are xrCoop/coop_routes.h (F2 T9); this module loads the ltx overrides,
// answers Lua and keeps the counters for coop_status.
//
// Anomaly's callback dispatcher is Lua (axr_main.callback_set / make_callback), so a thin Lua shim stays:
// scripts\coop_route.script wraps every handler once, when it is registered (RegisterScriptCallback, AddUniqueCall,
// CreateTimeEvent), with the domain coop.route.of gives, and asks coop.route.runs on every call.
//
// Lua (`coop.route`, registered with the `coop` namespace for every Lua state):
//   coop.route.of(callback, func) -> domain, script
//     domain "host" | "owner" | "view" | "none" | "level"; script is the key of the file func comes from
//     (routes::script_key: "xr_combat_ignore" for a .script, "illish.patches.xr_combat_ignore" for a .lua under
//     scripts\illish\patches; "[C]" for a C function, "?" when unknown). Nil when callback is not a string.
//   coop.route.runs(domain) -> bool
//     outside a session every domain runs (single player is untouched); in a session host and level run on the host,
//     owner and view everywhere, none nowhere (the host too). An unknown name runs (as owner).
//   coop.route.note(domain, outcome[, script[, callback]])
//     counts a call: outcome "ran" | "skipped" | "error"; script and callback name a skip for the per-pair lists
//     ("most skipped", "latest new skips"). of("", func) only names the script: it is neither counted nor cached.
//   coop.route.none_functions() -> {"module.function", ...}
//     [route_none_functions] of the ltx files (debug tools replaced by a message on a client, coop_main.script).
//
// Overrides: configs\coop_route.ltx, then every coop_route_*.ltx and coop_compat_*.ltx of $game_config$ (root only) by
// name, a later file winning for the same key; sections [route_scripts], [route_callbacks], [route_callbacks_forced],
// [route_pairs] (coop_routes.h parse_section) and [route_none_functions]. Read once per process, on the first of() or
// none_functions(). A rule for a script that is not installed never matches, so coop_compat_<mod>.ltx of a missing
// mod changes nothing (MOD_LAYER §2.5).
//
// Binders (F2 T11): the methods the engine itself calls on a script binder (CScriptBinder: update on every shedule tick,
// net_spawn, save ...) are not callbacks. configs\coop_route_binders.ltx ([binder_host], [binder_keep]; any coop_route*.ltx
// or coop_compat_*.ltx may hold them) names, by the script_binding of the object's section, what a client does with each
// method (routes::BinderAction, coop_routes.h): Run, HostOnly (not called) or KeepOnError (called, a Lua error is logged
// once per binder and method and the binder stays). The host and single player always Run. script_binder.cpp asks
// binder_action() before each call.
//
// Threading: Lua calls come from one Lua thread at a time (main or the GameThread worker); print_status comes from the
// console. One lock guards the table, the cache and the counters.
#pragma once

#include "xrCoop/coop_routes.h"

struct lua_State;

namespace coop
{
namespace router
{
// Once per Manager: no overrides loaded yet (the next Lua access reads them), counters zero.
void install();
// Session started or ended: the call counters start again. The table stays.
void reset();
// coop_status: the rule files, call counters by domain, the ten most skipped (callback, script) pairs and the ten
// pairs skipped for the first time most recently.
void print_status();
// Reads the ltx overrides unless done already. Called by the Lua functions; safe to call again.
void load_overrides();
// Adds the `route` table to the `coop` namespace of this Lua state (coop_script.cpp register_raw).
void register_lua(lua_State* L);

// --- F2 T11: engine-invoked binder methods ---

// The methods CScriptBinder calls on its Lua object; the order is routes::kBinderMethodNames.
enum class BinderMethod : u8
{
	Reinit,
	Reload,
	NetSpawn,
	NetDestroy,
	Update,
	Save,
	Load,
	NetSaveRelevant
};

// What this machine does with `method` of the binder created by `binding` (the script_binding of its section, e.g.
// "bind_camp.bind"; null for a binder Lua attached by hand). Run on the host, in single player, outside a session and
// for a binding no rule names. A HostOnly answer is a skipped call and is counted as one.
routes::BinderAction binder_action(const char* binding, BinderMethod method);

// An exception reached CScriptBinder's catch (exception builds). True: the action is KeepOnError, the binder stays and
// the error is counted and logged (the first one per binder and method, `logged` is the binder's own bit mask); false:
// the caller drops the binder as the stock engine does.
bool binder_kept(routes::BinderAction action, const char* binding, BinderMethod method, u16& logged, const char* what);

// While alive, a Lua error inside a call the engine makes into Lua (luabind) does not end the game: the Release engine
// raises a fatal error from the luabind pcall handler (CScriptEngine::lua_pcall_failed) and has no C++ exceptions for
// the catch branches to catch. The scope puts a logging handler in its place and restores the engine's on exit, so a
// failed call returns at once like an empty method (bool methods answer what the Lua object would not have changed:
// net_spawn true, net_save_relevant false, see failed()). `active` false: does nothing. In an exception build (Debug)
// luabind throws and the catch branches handle it, the scope does nothing there either.
// Errors of any luabind call made inside the scope are kept, nested binder calls included.
class BinderKeepScope
{
public:
	BinderKeepScope(bool active, const char* binding, BinderMethod method, u16& logged);
	~BinderKeepScope();
	BinderKeepScope(const BinderKeepScope&) = delete;
	BinderKeepScope& operator=(const BinderKeepScope&) = delete;

	// A Lua error was kept inside the scope.
	bool failed() const { return m_failed; }
	// Called by the handlers of the scope in force.
	void on_lua_error(lua_State* L);

private:
	const char* m_binding;
	BinderMethod m_method;
	u16* m_logged;
	bool m_active = false;
	bool m_failed = false;
	BinderKeepScope* m_outer = nullptr;
	int (*m_prev_pcall)(lua_State*) = nullptr;
	void (*m_prev_error)(lua_State*) = nullptr;
};
} // namespace router
} // namespace coop
