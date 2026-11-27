// Anomaly Coop — callback router (MOD_LAYER.md §2.2). Decides on which machine each Lua handler
// of a callback runs. The table and its rules are xrCoop/coop_routes.h; this module loads the ltx overrides,
// answers Lua and keeps the counters for coop_status.
//
// Anomaly's callback dispatcher is Lua (axr_main.callback_set / make_callback), so a thin Lua shim stays:
// scripts\coop_route.script wraps every handler once, when it is registered (RegisterScriptCallback, AddUniqueCall,
// CreateTimeEvent), with the domain coop.route.of gives, and asks coop.route.runs on every call.
//
// Lua (`coop.route`, registered with the `coop` namespace for every Lua state):
//   coop.route.of(callback, func) -> domain, script
//     domain "host" | "owner" | "view" | "none" | "level"; script is the key of the file func comes from
//     (routes::script_key: "script_name" for a .script, "some_mod.patches.script_name" for a .lua under
//     scripts\some_mod\patches; "[C]" for a C function, "?" when unknown). Nil when callback is not a string.
//   coop.route.runs(domain) -> bool
//     outside a session every domain runs (single player is untouched); in a session host and level run on the host,
//     owner and view everywhere, none nowhere (the host too). An unknown name runs (as owner).
//   coop.route.note(domain, outcome[, script[, callback]])
//     counts a call: outcome "ran" | "skipped" | "error"; script and callback name a skip for the per-pair lists
//     ("most skipped", "latest new skips"). of("", func) only names the script: it is neither counted nor cached.
//   coop.route.rule_of(callback, func) -> domain, script
//     what of() answers, without a cache entry and without a count: a script asking its own rule.
//   coop.route.note_batch({ {domain, outcome, count[, script[, callback]]}, ... }) -> rows counted
//     what `count` calls of note() count, for many rows under one lock (coop_route.script hands its counts over once a second).
//   coop.route.none_functions() -> {"module.function", ...}
//     [route_none_functions] of the ltx files (debug tools replaced by a message on a client, coop_main.script).
//   coop.route.scoped(wrapper, domain), coop.route.scope() -> domain
//     the code scope of the writer gate: the domain of the innermost registered wrapper on the Lua stack.
//
// Overrides: configs\coop_route.ltx, then every coop_route_*.ltx and coop_compat_*.ltx of $game_config$ (root only) by
// name, a later file winning for the same key; sections [route_scripts], [route_callbacks], [route_callbacks_forced],
// [route_pairs] (coop_routes.h parse_section) and [route_none_functions]. Read once per process, on the first of() or
// none_functions(). A rule for a script that is not installed never matches, so coop_compat_<mod>.ltx of a missing
// mod changes nothing (MOD_LAYER §2.5).
//
// Binders: the methods the engine itself calls on a script binder (CScriptBinder: update on every shedule tick,
// net_spawn, save ...) are not callbacks. configs\coop_route_binders.ltx ([binder_host]; any coop_route*.ltx or
// coop_compat_*.ltx may hold it) names, by the script_binding of the object's section, the methods a client does not call
// (routes::BinderAction, coop_routes.h: Run or HostOnly). The host and single player always Run. script_binder.cpp asks
// binder_action() before each call. What must survive a Lua error is not here: Release has no C++ exceptions,
// so only a Lua pcall around the method helps, scripts\coop_client_binders.script [binder_lua_guard].
//
// Threading: Lua calls come from one Lua thread at a time (main or the GameThread worker); print_status comes from the
// console. One lock guards the table, the cache and the counters.
#pragma once

#include "xrCoop/coop_routes.h"
#include "xrCoop/coop_writer_gate.h"

#include <string>
#include <vector>

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
// The console command coop_writer_gate = log | enforce (coop_console.cpp register_console_commands, before user.ltx is read).
void register_console();

// The files that hold the overrides of the router and of the call gate (callgate::load_overrides): coop_route.ltx first, then
// coop_route_*.ltx, then coop_compat_*.ltx, each group by name, from the root of $game_config$ (file_list_open, not
// file_list_open_ex: a checked rescan drops the subfolders of configs). have_main: coop_route.ltx is there.
std::vector<std::string> route_files(bool& have_main);

// --- the writer gate (xrCoop/coop_writer_gate.h) ---

// This process's coop_writer_gate setting: Log until the console command says otherwise.
calls::WriterGate writer_gate();
// Asked by the gated script writes after the call gate (alife_simulator_script.cpp, level_script.cpp): true when the write is refused
// here (only under coop_writer_gate = enforce, on the world server or a map server). Every other answer is false: it counts what it
// would refuse, ask or order (the census in coop_status); the co-op host, a player's machine and single player never refuse.
bool writer_refuses(const char* call, writer::WriteOp op, const calls::WriteTarget& target);

// --- engine-invoked binder methods ---

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

// Whether this machine calls `method` of the binder created by `binding` (the script_binding of its section, e.g.
// "bind_camp.bind"; null for a binder Lua attached by hand). Run on the host, in single player, outside a session and
// for a binding no rule names. A HostOnly answer is a skipped call and is counted as one.
routes::BinderAction binder_action(const char* binding, BinderMethod method);
} // namespace router
} // namespace coop
