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
//   coop.route.note(domain, outcome[, script])
//     counts a call: outcome "ran" | "skipped" | "error"; script names a skip for "the most skipped" list.
//   coop.route.none_functions() -> {"module.function", ...}
//     [route_none_functions] of the ltx files (debug tools replaced by a message on a client, coop_main.script).
//
// Overrides: configs\coop_route.ltx, then every coop_route_*.ltx and coop_compat_*.ltx of $game_config$ (root only) by
// name, a later file winning for the same key; sections [route_scripts], [route_callbacks], [route_callbacks_forced],
// [route_pairs] (coop_routes.h parse_section) and [route_none_functions]. Read once per process, on the first of() or
// none_functions(). A rule for a script that is not installed never matches, so coop_compat_<mod>.ltx of a missing
// mod changes nothing (MOD_LAYER §2.5).
//
// Threading: Lua calls come from one Lua thread at a time (main or the GameThread worker); print_status comes from the
// console. One lock guards the table, the cache and the counters.
#pragma once

struct lua_State;

namespace coop
{
namespace router
{
// Once per Manager: no overrides loaded yet (the next Lua access reads them), counters zero.
void install();
// Session started or ended: the call counters start again. The table stays.
void reset();
// coop_status: the rule files, call counters by domain and the ten most skipped scripts.
void print_status();
// Reads the ltx overrides unless done already. Called by the Lua functions; safe to call again.
void load_overrides();
// Adds the `route` table to the `coop` namespace of this Lua state (coop_script.cpp register_raw).
void register_lua(lua_State* L);
} // namespace router
} // namespace coop
