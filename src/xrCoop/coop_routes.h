// Anomaly Coop - callback route table (MOD_LAYER.md §2.2, F2 T9). Decides on which machine a Lua callback handler
// runs: the key is the pair (callback name, script of the handler), the stock callbacks have defaults by callback, and
// ltx sections override them.
//   host  - only on the host (world events, spawn, story, emission, NPC death, saving)
//   owner - on the machine that owns the object: a player for their own character and items; runs everywhere, the
//           calls it makes into the world are gated by the call router (§2.3)
//   view  - everywhere (HUD, sound, particles, UI, data that every machine reads)
//   none  - nowhere in co-op (mechanics the engine replaced, debug tools); `blocked` is the old name
//   level - reserved for F3a (split levels); behaves as host until then
// Names (callbacks and scripts) compare case-insensitively: an ltx reader may lowercase keys, Lua sends the real case.
// F2 T10 adds what the router needs besides the table: script_key (a function's script from its Lua source) and the
// call counters. F2 T11 adds the binder rules: whether a client calls a method the engine calls on a script binder.
// Pure C++17, header-only, no engine dependencies: used by the engine, the unit-test exe and the Lua test runner.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace coop
{
namespace routes
{
enum class Domain : uint8_t
{
	Host,
	Owner,
	View,
	None,
	Level
};

inline const char* domain_name(Domain d)
{
	switch (d)
	{
	case Domain::Host: return "host";
	case Domain::Owner: return "owner";
	case Domain::View: return "view";
	case Domain::None: return "none";
	case Domain::Level: return "level";
	}
	return "owner";
}

namespace detail
{
inline char lower(char c) { return (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c; }

inline bool iequals(std::string_view a, std::string_view b)
{
	if (a.size() != b.size())
		return false;
	for (size_t i = 0; i < a.size(); ++i)
		if (lower(a[i]) != lower(b[i]))
			return false;
	return true;
}

inline bool istarts_with(std::string_view s, std::string_view prefix)
{
	return s.size() >= prefix.size() && iequals(s.substr(0, prefix.size()), prefix);
}

inline std::string lowered(std::string_view s)
{
	std::string r(s);
	for (char& c : r)
		c = lower(c);
	return r;
}

inline std::string_view trimmed(std::string_view s)
{
	const std::string_view ws = " \t\r\n";
	const size_t b = s.find_first_not_of(ws);
	if (b == std::string_view::npos)
		return {};
	const size_t e = s.find_last_not_of(ws);
	return s.substr(b, e - b + 1);
}
} // namespace detail

// Accepts host, owner, view, none, blocked (= none), level; surrounding blanks and letter case are ignored.
inline bool parse_domain(std::string_view text, Domain& out)
{
	const std::string_view t = detail::trimmed(text);
	struct Name
	{
		std::string_view name;
		Domain domain;
	};
	static constexpr Name kNames[] = {{"host", Domain::Host}, {"owner", Domain::Owner}, {"view", Domain::View},
		{"none", Domain::None}, {"blocked", Domain::None}, {"level", Domain::Level}};
	for (const Name& n : kNames)
		if (detail::iequals(t, n.name))
		{
			out = n.domain;
			return true;
		}
	return false;
}

// Pseudo-callback names the Lua shim uses for AddUniqueCall and CreateTimeEvent. They have no callback of their own:
// the handler's script decides, owner by default: a timer follows its script like the script's callbacks, so a client
// mod is never half on (fix round 2, review I2: a callback that starts an action and a time event that ends it). World
// scripts (tasks_*, xr_effects, simulation_objects ...) are host by their generated rules (configs\coop_route_stock.ltx
// / coop_route_gamma.ltx, Ruling F2-29), whatever registers them.
inline constexpr std::string_view kUniqueCall = "unique_call";
inline constexpr std::string_view kTimeEvent = "time_event";

struct StockRoute
{
	std::string_view callback;
	Domain domain;
};

// Every callback of axr_main.script's `intercepts` table (engine gamedata, lines 49-233), group by group in the file's
// order: 140 live rows and the 7 rows the stock file has commented out (a mod that adds one of those gets its group's
// domain). Then the names the brief adds. tests/test_routes.cpp checks this list against the file.
inline constexpr StockRoute kStockRoutes[] = {
	// Player: the local actor -> owner
	{"on_before_level_changing", Domain::Owner},
	{"on_level_changing", Domain::Owner},
	{"actor_on_before_death", Domain::Owner},
	{"actor_on_net_destroy", Domain::Owner},
	{"actor_on_first_update", Domain::Owner},
	{"actor_on_update", Domain::Owner},
	{"actor_on_weapon_fired", Domain::Owner},
	{"actor_on_weapon_jammed", Domain::Owner},
	{"actor_on_weapon_no_ammo", Domain::Owner},
	{"actor_on_weapon_lower", Domain::Owner},
	{"actor_on_weapon_raise", Domain::Owner},
	{"actor_on_weapon_reload", Domain::Owner},
	{"actor_on_weapon_zoom_in", Domain::Owner},
	{"actor_on_weapon_zoom_out", Domain::Owner},
	{"actor_on_item_take", Domain::Owner},
	{"actor_on_item_take_from_box", Domain::Owner},
	{"actor_on_item_put_in_box", Domain::Owner},
	{"actor_on_item_drop", Domain::Owner},
	{"actor_on_item_use", Domain::Owner},
	{"actor_on_item_before_use", Domain::Owner},
	{"actor_on_item_before_pickup", Domain::Owner},
	{"actor_item_to_belt", Domain::Owner},
	{"actor_item_to_ruck", Domain::Owner},
	{"actor_item_to_slot", Domain::Owner},
	{"actor_on_trade", Domain::Owner},
	{"actor_on_init", Domain::Owner},
	{"actor_on_reinit", Domain::Owner},
	{"actor_on_torch_enabled", Domain::Owner},  // commented out in stock
	{"actor_on_torch_disabled", Domain::Owner}, // commented out in stock
	{"actor_on_info_callback", Domain::Owner},
	{"actor_on_hit_callback", Domain::Owner},
	{"actor_on_attach_vehicle", Domain::Owner},
	{"actor_on_detach_vehicle", Domain::Owner},
	{"actor_on_use_vehicle", Domain::Owner},
	{"actor_on_hud_animation_play", Domain::Owner},
	{"actor_on_hud_animation_end", Domain::Owner},
	{"actor_on_hud_animation_mark", Domain::Owner},
	{"actor_on_sleep", Domain::Owner},
	{"actor_on_foot_step", Domain::Owner},
	{"actor_on_interaction", Domain::Owner},
	{"actor_on_before_hit", Domain::Owner},
	{"actor_on_before_hit_belt", Domain::Owner},
	{"actor_on_weapon_before_fire", Domain::Owner},
	{"actor_on_feeling_anomaly", Domain::Owner},
	{"actor_on_leave_dialog", Domain::Owner},
	{"actor_on_stash_create", Domain::Owner},
	{"actor_on_stash_remove", Domain::Owner},
	{"actor_on_frequency_change", Domain::Owner},
	{"actor_on_achievement_earned", Domain::Owner},
	{"actor_on_movement_changed", Domain::Owner},
	{"actor_on_footstep", Domain::Owner},
	{"actor_on_jump", Domain::Owner},
	{"actor_on_land", Domain::Owner},
	{"actor_on_before_throwable_select", Domain::Owner},

	// NPCs: AI and world -> host. Except the player's own use action (here and in Mutants, Physical objects): it fires
	// on the machine whose actor pressed "use" (bind_monster.script:291 use_callback; GAMMA ui_mutant_loot.script:172
	// sends monster_on_loot_init for that player's loot window), and only a CPhysicObject use is moved to the host
	// (client_intercept_use, coop_objects.cpp), where owner runs it as well. Host would drop the NPC and mutant ones
	// for a client, so owner; world logic in those handlers stays on the host through script rules (treasure_manager,
	// grok_stashes_on_corpses = host).
	{"npc_on_use", Domain::Owner},
	{"npc_on_choose_weapon", Domain::Host},
	{"npc_on_item_take", Domain::Host},
	{"npc_on_item_take_from_box", Domain::Host},
	{"npc_on_item_drop", Domain::Host},
	{"npc_on_net_spawn", Domain::Host},
	{"npc_on_net_destroy", Domain::Host},
	{"npc_on_update", Domain::Host},
	{"npc_on_before_hit", Domain::Host},
	{"npc_on_hit_callback", Domain::Host},
	{"npc_on_death_callback", Domain::Host},
	{"npc_on_fighting_actor", Domain::Host},
	{"npc_on_weapon_strapped", Domain::Host},
	{"npc_on_weapon_unstrapped", Domain::Host},
	{"npc_on_weapon_drop", Domain::Host},
	{"npc_on_hear_callback", Domain::Host},
	{"npc_on_get_all_from_corpse", Domain::Host},
	{"npc_on_eval_danger", Domain::Host},
	{"npc_shot_dispersion", Domain::Host},
	{"anomaly_on_before_activate", Domain::Host},

	// Mutants -> host
	{"monster_on_update", Domain::Host},
	{"monster_on_before_hit", Domain::Host},
	{"monster_on_hit_callback", Domain::Host},
	{"monster_on_net_spawn", Domain::Host},
	{"monster_on_net_destroy", Domain::Host},
	{"monster_on_death_callback", Domain::Host},
	{"monster_on_actor_use_callback", Domain::Owner}, // the player's own use, see NPCs
	{"monster_on_loot_init", Domain::Owner},          // the player's own loot window, see NPCs
	{"burer_on_before_weapon_drop", Domain::Host},

	// Physical objects -> host
	{"physic_object_on_hit_callback", Domain::Host},
	{"physic_object_on_use_callback", Domain::Owner}, // the player's own use, see NPCs

	// Vehicles -> host
	{"heli_on_hit_callback", Domain::Host},
	{"vehicle_on_death_callback", Domain::Host},

	// Squads: simulation -> host
	{"squad_on_npc_creation", Domain::Host},
	{"squad_on_enter_smart", Domain::Host},
	{"squad_on_leave_smart", Domain::Host},
	{"squad_on_npc_death", Domain::Host},
	{"squad_on_update", Domain::Host},
	{"squad_on_first_update", Domain::Host},
	{"squad_on_add_npc", Domain::Host},
	{"squad_on_register", Domain::Host},   // commented out in stock
	{"squad_on_unregister", Domain::Host}, // commented out in stock
	{"squad_on_after_game_vertex_change", Domain::Host},
	{"squad_on_after_level_change", Domain::Host},

	// smart_terrain.script -> host
	{"smart_terrain_on_update", Domain::Host},
	{"on_try_respawn", Domain::Host},

	// Server objects: ALife -> host
	{"server_entity_on_register", Domain::Host},
	{"server_entity_on_unregister", Domain::Host},
	{"fill_start_position", Domain::Host},
	{"se_stalker_on_spawn", Domain::Host},
	{"se_actor_on_register", Domain::Host},   // commented out in stock
	{"se_actor_on_unregister", Domain::Host}, // commented out in stock
	{"se_actor_on_STATE_Write", Domain::Host},
	{"se_actor_on_STATE_Read", Domain::Host},

	// GUI -> view
	{"ActorMenu_on_before_init_mode", Domain::View},
	{"ActorMenu_on_mode_changed", Domain::View},
	{"ActorMenu_on_item_drag_drop", Domain::View},
	{"ActorMenu_on_item_focus_receive", Domain::View},
	{"ActorMenu_on_item_focus_lost", Domain::View},
	{"ActorMenu_on_item_before_move", Domain::View},
	{"ActorMenu_on_item_after_move", Domain::View},
	{"ActorMenu_on_trade_started", Domain::View},
	{"ActorMenu_on_trade_closed", Domain::View},
	{"GUI_on_show", Domain::View},
	{"GUI_on_hide", Domain::View},
	{"map_spot_menu_add_property", Domain::View},
	{"map_spot_menu_property_clicked", Domain::View},
	{"main_menu_on_keyboard", Domain::View},
	{"main_menu_on_init", Domain::View},
	{"main_menu_on_init_callbacks", Domain::View}, // commented out in stock
	{"main_menu_on_quit", Domain::View},
	{"on_screen_resolution_changed", Domain::View},

	// Technical: input, options, console, loading -> view
	{"on_game_load", Domain::View},
	{"on_key_press", Domain::View},
	{"on_key_release", Domain::View},
	{"on_key_hold", Domain::View},
	{"on_before_key_press", Domain::View},
	{"on_before_key_release", Domain::View},
	{"on_before_key_hold", Domain::View},
	{"on_option_change", Domain::View},
	{"on_localization_change", Domain::View},
	{"on_console_execute", Domain::View},
	{"on_before_save_input", Domain::View},
	{"on_before_load_input", Domain::View},

	// Files: the host saves, everybody reads what was loaded
	{"save_state", Domain::Host},
	{"load_state", Domain::View},
	{"on_pstor_save_all", Domain::Host},
	{"on_pstor_load_all", Domain::View},

	// Others: world AI and weather -> host; a price query and bullet impact effects -> view (every machine shows
	// prices and impacts; world calls from the handlers are gated by the call router)
	{"on_enemy_eval", Domain::Host},
	{"on_before_surge", Domain::Host},
	{"on_before_psi_storm", Domain::Host},
	{"on_get_item_cost", Domain::View},
	{"bullet_on_hit", Domain::View},

	// Not in intercepts, named by the brief
	{"on_xml_read", Domain::View}, // modxml_core
	{kUniqueCall, Domain::Owner}, // see kUniqueCall
	{kTimeEvent, Domain::Owner},
};

// Domains for callback names that are not in the list above (callbacks mods and the monolith add themselves).
inline constexpr StockRoute kPrefixRoutes[] = {
	{"actor_", Domain::Owner},
	{"npc_", Domain::Host},
	{"monster_", Domain::Host},
	{"squad_", Domain::Host},
	{"smart_terrain_", Domain::Host},
	{"server_entity_", Domain::Host},
	{"server_object_", Domain::Host}, // monolith callbacks_gameobject.script: server_object_on_register/unregister
	{"heli_", Domain::Host},
	{"vehicle_", Domain::Host},
	{"physic_object_", Domain::Host},
	{"GUI_", Domain::View},
	{"ActorMenu_", Domain::View},
	{"on_key_", Domain::View},
};

inline size_t stock_count() { return sizeof(kStockRoutes) / sizeof(kStockRoutes[0]); }

// The explicit entry of a known callback; false for a name only the prefixes or the fallback cover.
inline bool stock_domain(std::string_view callback, Domain& out)
{
	for (const StockRoute& r : kStockRoutes)
		if (detail::iequals(r.callback, callback))
		{
			out = r.domain;
			return true;
		}
	return false;
}

// The callback's own domain: its explicit entry, else its prefix, else owner. An unknown callback (a mod's own
// AddScriptCallback name, e.g. idiots_on_start) is owner, so it runs on every machine: skipping it on a client
// leaves the mod half-initialised there.
inline Domain default_of(std::string_view callback)
{
	Domain d = Domain::Owner;
	if (stock_domain(callback, d))
		return d;
	for (const StockRoute& r : kPrefixRoutes)
		if (detail::istarts_with(callback, r.callback))
			return r.domain;
	return Domain::Owner;
}

// One ltx override.
//   Script   [route_scripts]          script = domain; a trailing * makes `script` a prefix
//   Callback [route_callbacks]        callback = domain; the callback's default, weaker than any script rule
//   Forced   [route_callbacks_forced] callback = domain; stronger than script rules, except scripts routed to none
//   Pair     [route_pairs]            callback@script = domain; stronger than everything
struct Override
{
	enum Kind : uint8_t
	{
		Script,
		Callback,
		Forced,
		Pair
	} kind = Script;
	std::string callback, script;
	Domain domain = Domain::Owner;
};

inline const char* section_of(Override::Kind kind)
{
	switch (kind)
	{
	case Override::Script: return "route_scripts";
	case Override::Callback: return "route_callbacks";
	case Override::Forced: return "route_callbacks_forced";
	case Override::Pair: return "route_pairs";
	}
	return "route_scripts";
}

// Parses one ltx section of `name = value` lines. A bad line is skipped and reported in `errors` (may be null) as
// "<section>: <name> = <value>: <why>"; the other lines still parse. An unknown section is one error and no rules.
inline std::vector<Override> parse_section(std::string_view section,
	const std::vector<std::pair<std::string, std::string>>& lines, std::vector<std::string>* errors)
{
	std::vector<Override> out;
	const std::string_view sec = detail::trimmed(section);
	Override::Kind kind = Override::Script;
	bool known = false;
	for (const Override::Kind k : {Override::Script, Override::Callback, Override::Forced, Override::Pair})
		if (detail::iequals(sec, section_of(k)))
		{
			kind = k;
			known = true;
		}
	if (!known)
	{
		if (errors)
			errors->push_back(std::string(sec) +
				": unknown section (route_scripts, route_callbacks, route_callbacks_forced, route_pairs)");
		return out;
	}

	for (const std::pair<std::string, std::string>& line : lines)
	{
		const std::string_view key = detail::trimmed(line.first);
		const std::string_view value = detail::trimmed(line.second);
		const char* why = nullptr;
		Override o;
		o.kind = kind;
		if (!parse_domain(value, o.domain))
			why = "unknown domain (host, owner, view, none, blocked, level)";
		else if (key.empty())
			why = "empty name";
		else if (kind == Override::Pair)
		{
			const size_t at = key.find('@');
			if (at == std::string_view::npos || key.find('@', at + 1) != std::string_view::npos)
				why = "expected callback@script";
			else
			{
				o.callback = std::string(detail::trimmed(key.substr(0, at)));
				o.script = std::string(detail::trimmed(key.substr(at + 1)));
				if (o.callback.empty() || o.script.empty())
					why = "expected callback@script";
				else if (o.callback.find('*') != std::string::npos || o.script.find('*') != std::string::npos)
					why = "a pair takes exact names, no *";
			}
		}
		else if (key.find('@') != std::string_view::npos)
			why = "callback@script belongs in [route_pairs]";
		else if (kind == Override::Script)
		{
			const size_t star = key.find('*');
			if (key == "*")
				why = "a bare * would route every script";
			else if (star != std::string_view::npos && star != key.size() - 1)
				why = "* only at the end of a script name";
			else
				o.script = std::string(key);
		}
		else if (key.find('*') != std::string_view::npos)
			why = "callback names are exact, no *";
		else
			o.callback = std::string(key);

		if (why)
		{
			if (errors)
				errors->push_back(std::string(sec) + ": " + std::string(key) + " = " + std::string(value) + ": " + why);
			continue;
		}
		out.push_back(std::move(o));
	}
	return out;
}

// --- F2 T11: the methods the engine itself calls on a script binder (CScriptBinder, xrGame/script_binder.cpp) ---
// Not callbacks: Anomaly's binders are Lua classes derived from object_binder, and the engine calls their methods by name
// (update every shedule tick, net_spawn, save ...). The section of the object names the class's creator in
// `script_binding = bind_camp.bind` (script.function); that string is the key here, so the engine decides whether a client
// calls a method before it enters Lua. Lua-called methods (a binder's own helpers) cannot be reached from here and stay in
// scripts\coop_client_binders.script, and so does every "keep running through a Lua error" rule: the Release engine has no C++
// exceptions, a Lua error there is fatal, and only a Lua pcall around the method survives it (Ruling F2-35).
//   Run      - call it, as the stock game does: the host, single player, a binding no rule names
//   HostOnly - do not call it on a client: world logic only the host runs ([binder_host]); a client keeps the C++ default
enum class BinderAction : uint8_t
{
	Run,
	HostOnly
};

inline const char* binder_action_name(BinderAction a)
{
	switch (a)
	{
	case BinderAction::Run: return "run";
	case BinderAction::HostOnly: return "host";
	}
	return "run";
}

// The Lua names of the methods the engine calls, in the order of the engine enum coop::router::BinderMethod.
// (net_Relcase is not among them: CScriptBinderObjectWrapper::net_Relcase does not call Lua, and the engine asks it of
// every binder for every destroyed object.)
inline constexpr std::string_view kBinderMethodNames[] = {"reinit", "reload", "net_spawn", "net_destroy", "update", "save",
	"load", "net_save_relevant"};
inline constexpr size_t kBinderMethodCount = sizeof(kBinderMethodNames) / sizeof(kBinderMethodNames[0]);

inline constexpr const char* kBinderHostSection = "binder_host";

// One line of [binder_host]: `script.function = method, method`.
struct BinderRule
{
	BinderAction action = BinderAction::Run;
	std::string binding;              // the script_binding value, as written
	std::vector<std::string> methods; // lowercase names from kBinderMethodNames
};

namespace detail
{
// script.function: parts of letters, digits and '_' joined by dots, at least two (a mod's nested module is allowed).
inline bool is_binding_name(std::string_view s)
{
	size_t parts = 0, len = 0;
	for (const char c : s)
	{
		if (c == '.')
		{
			if (len == 0)
				return false;
			++parts;
			len = 0;
		}
		else if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')
			++len;
		else
			return false;
	}
	return len != 0 && parts >= 1;
}

inline int binder_method_index(std::string_view name)
{
	for (size_t i = 0; i < kBinderMethodCount; ++i)
		if (iequals(name, kBinderMethodNames[i]))
			return int(i);
	return -1;
}
} // namespace detail

// Parses the [binder_host] section of `binding = method, method` lines (methods separated by commas and blanks). A bad
// line is skipped and reported in `errors` (may be null) as "<section>: <name> = <value>: <why>"; the other lines still
// parse. Any other section is one error and no rules.
inline std::vector<BinderRule> parse_binder_section(std::string_view section,
	const std::vector<std::pair<std::string, std::string>>& lines, std::vector<std::string>* errors)
{
	std::vector<BinderRule> out;
	const std::string_view sec = detail::trimmed(section);
	if (!detail::iequals(sec, kBinderHostSection))
	{
		if (errors)
			errors->push_back(std::string(sec) + ": unknown section (" + kBinderHostSection + ")");
		return out;
	}

	for (const std::pair<std::string, std::string>& line : lines)
	{
		const std::string_view key = detail::trimmed(line.first);
		const std::string_view value = detail::trimmed(line.second);
		BinderRule rule;
		rule.action = BinderAction::HostOnly;
		rule.binding = std::string(key);
		std::string why;
		if (!detail::is_binding_name(key))
			why = "expected script.function (the script_binding of the object's section)";
		else
		{
			size_t pos = 0;
			while (pos < value.size())
			{
				const size_t end = value.find_first_of(", \t", pos);
				const std::string_view token = value.substr(pos, end == std::string_view::npos ? end : end - pos);
				pos = end == std::string_view::npos ? value.size() : end + 1;
				if (token.empty())
					continue;
				const int m = detail::binder_method_index(token);
				if (m < 0)
				{
					why = "unknown binder method '" + std::string(token) + "' (reinit, reload, net_spawn, net_destroy, update, save, load, "
						"net_save_relevant)";
					break;
				}
				rule.methods.emplace_back(kBinderMethodNames[size_t(m)]);
			}
			if (why.empty() && rule.methods.empty())
				why = "expected a list of binder methods";
		}
		if (!why.empty())
		{
			if (errors)
				errors->push_back(std::string(sec) + ": " + std::string(key) + " = " + std::string(value) + ": " + why);
			continue;
		}
		out.push_back(std::move(rule));
	}
	return out;
}

// Overrides by kind; route() answers from strongest to weakest:
//   1. pair callback@script
//   2. forced callback, unless the script's own rule is none
//   3. exact script
//   4. longest script prefix
//   5. callback rule from ltx
//   6. default_of(callback)
// A later rule for the same key replaces the earlier one.
class Table
{
public:
	// Rules with an empty name are ignored (parse_section never makes them).
	void add(const Override& o)
	{
		switch (o.kind)
		{
		case Override::Pair:
			if (!o.callback.empty() && !o.script.empty())
				m_pairs[pair_key(o.callback, o.script)] = o.domain;
			break;
		case Override::Forced:
			if (!o.callback.empty())
				m_forced[detail::lowered(o.callback)] = o.domain;
			break;
		case Override::Callback:
			if (!o.callback.empty())
				m_callbacks[detail::lowered(o.callback)] = o.domain;
			break;
		case Override::Script:
			if (o.script.empty())
				break;
			if (o.script.back() == '*')
				add_prefix(detail::lowered(std::string_view(o.script).substr(0, o.script.size() - 1)), o.domain);
			else
				m_scripts[detail::lowered(o.script)] = o.domain;
			break;
		}
	}

	// F2 T11: a binder rule gives each of its methods the rule's action. Rules without a binding or a method are ignored
	// (parse_binder_section never makes them).
	void add(const BinderRule& r)
	{
		if (r.binding.empty())
			return;
		for (const std::string& m : r.methods)
			if (!m.empty())
				m_binders[detail::lowered(r.binding)][detail::lowered(m)] = r.action;
	}

	// Whether a client calls `method` of the binder created by `binding` (the object's script_binding, script.function).
	BinderAction binder_action(std::string_view binding, std::string_view method) const
	{
		if (m_binders.empty() || binding.empty() || method.empty())
			return BinderAction::Run;
		const auto b = m_binders.find(detail::lowered(binding));
		if (b == m_binders.end())
			return BinderAction::Run;
		const auto m = b->second.find(detail::lowered(method));
		return m == b->second.end() ? BinderAction::Run : m->second;
	}

	// Binder rules, one per (binding, method); size() counts the callback rules only.
	size_t binder_size() const
	{
		size_t n = 0;
		for (const auto& b : m_binders)
			n += b.second.size();
		return n;
	}

	void clear()
	{
		m_pairs.clear();
		m_forced.clear();
		m_callbacks.clear();
		m_scripts.clear();
		m_prefixes.clear();
		m_binders.clear();
	}

	size_t size() const
	{
		return m_pairs.size() + m_forced.size() + m_callbacks.size() + m_scripts.size() + m_prefixes.size();
	}

	Domain route(std::string_view callback, std::string_view script) const
	{
		const std::string cb = detail::lowered(callback);
		const std::string sc = detail::lowered(script);

		if (!m_pairs.empty())
		{
			const auto it = m_pairs.find(pair_key(cb, sc));
			if (it != m_pairs.end())
				return it->second;
		}

		Domain by_script = Domain::Owner;
		const bool has_script = script_rule(sc, by_script);

		if (!(has_script && by_script == Domain::None))
		{
			const auto it = m_forced.find(cb);
			if (it != m_forced.end())
				return it->second;
		}
		if (has_script)
			return by_script;

		const auto it = m_callbacks.find(cb);
		if (it != m_callbacks.end())
			return it->second;
		return default_of(cb);
	}

private:
	struct Prefix
	{
		std::string prefix;
		Domain domain;
	};

	static std::string pair_key(std::string_view callback, std::string_view script)
	{
		std::string key = detail::lowered(callback);
		key += '@';
		key += detail::lowered(script);
		return key;
	}

	void add_prefix(std::string prefix, Domain d)
	{
		for (Prefix& p : m_prefixes)
			if (p.prefix == prefix)
			{
				p.domain = d;
				return;
			}
		m_prefixes.push_back({std::move(prefix), d});
	}

	// Exact script name first, then the longest matching prefix. `script` is lowercase.
	bool script_rule(const std::string& script, Domain& out) const
	{
		const auto it = m_scripts.find(script);
		if (it != m_scripts.end())
		{
			out = it->second;
			return true;
		}
		const Prefix* best = nullptr;
		for (const Prefix& p : m_prefixes)
			if (script.size() >= p.prefix.size() && script.compare(0, p.prefix.size(), p.prefix) == 0 &&
				(!best || p.prefix.size() > best->prefix.size()))
				best = &p;
		if (!best)
			return false;
		out = best->domain;
		return true;
	}

	std::map<std::string, Domain> m_pairs;     // "callback@script", lowercase
	std::map<std::string, Domain> m_forced;    // callback, lowercase
	std::map<std::string, Domain> m_callbacks; // callback, lowercase
	std::map<std::string, Domain> m_scripts;   // exact script, lowercase
	std::vector<Prefix> m_prefixes;            // script prefix without the *, lowercase
	std::map<std::string, std::map<std::string, BinderAction>> m_binders; // binding -> method -> action, lowercase
};

// Host and level run only on the host; owner and view run everywhere; none runs nowhere.
inline bool runs_here(Domain d, bool is_host)
{
	switch (d)
	{
	case Domain::Host:
	case Domain::Level: return is_host;
	case Domain::Owner:
	case Domain::View: return true;
	case Domain::None: return false;
	}
	return false;
}

// --- F2 T10: what the router (xrGame/coop/coop_router.cpp) needs besides the table ---

// The key of the script a Lua function comes from, given its lua_Debug::source:
//   "@D:\...\gamedata\scripts\xr_combat_ignore.script"          -> "xr_combat_ignore"
//     a .script by its base name: the engine loads them from the scripts root by name;
//   "@.\gamedata\scripts\illish\patches\xr_combat_ignore.lua"   -> "illish.patches.xr_combat_ignore"
//     a .lua by its path under the scripts folder, folders joined by dots (the name require gives it). Never its bare
//     base name: GAMMA's patch of a stock script is its own script, not the stock one (live bug 2026-10-23: the old
//     Lua filter took illish\patches\xr_combat_ignore.lua for the stock host-only xr_combat_ignore). The folder is
//     the part after the last "\gamedata\scripts\", else after the last "\scripts\", else the path itself without a
//     drive and leading ".\" / "..\";
//   anything else (no .script / .lua: a string chunk, "=[C]", "@console_command") -> "?".
// Lowercase; '/' and '\' are both separators; a leading '@' or '=' is dropped.
inline std::string script_key(std::string_view source)
{
	if (!source.empty() && (source.front() == '@' || source.front() == '='))
		source.remove_prefix(1);
	std::string p = "\\" + detail::lowered(source); // a leading separator: "scripts\x.lua" matches "\scripts\" too
	for (char& c : p)
		if (c == '/')
			c = '\\';

	const auto strip = [&p](std::string_view ext)
	{
		if (p.size() < ext.size() || p.compare(p.size() - ext.size(), ext.size(), ext) != 0)
			return false;
		p.resize(p.size() - ext.size());
		return true;
	};
	std::string key;
	if (strip(".script"))
		key = p.substr(p.find_last_of('\\') + 1);
	else if (strip(".lua"))
	{
		size_t from = std::string::npos;
		for (const std::string_view folder : {std::string_view("\\gamedata\\scripts\\"), std::string_view("\\scripts\\")})
		{
			const size_t at = p.rfind(folder);
			if (at != std::string::npos)
			{
				from = at + folder.size();
				break;
			}
		}
		if (from == std::string::npos)
		{
			from = 0;
			const size_t drive = p.find(':');
			if (drive != std::string::npos && drive < p.find('\\', 1))
				from = drive + 1;
			for (bool more = true; more;)
			{
				more = false;
				for (const std::string_view lead : {std::string_view("\\"), std::string_view(".\\"), std::string_view("..\\")})
					if (p.compare(from, lead.size(), lead) == 0)
					{
						from += lead.size();
						more = true;
						break;
					}
			}
		}
		key = p.substr(from);
		for (char& c : key)
			if (c == '\\')
				c = '.';
	}
	else
		return "?";
	return key.empty() ? std::string("?") : key;
}

// What happened to one call of a routed handler.
enum class Outcome : uint8_t
{
	Ran,     // called (a call that then failed is counted here and under Error)
	Skipped, // not called on this machine
	Error    // called on a client under pcall and raised a Lua error
};

// Accepts ran, skipped, error; surrounding blanks and letter case are ignored.
inline bool parse_outcome(std::string_view text, Outcome& out)
{
	const std::string_view t = detail::trimmed(text);
	if (detail::iequals(t, "ran"))
		out = Outcome::Ran;
	else if (detail::iequals(t, "skipped"))
		out = Outcome::Skipped;
	else if (detail::iequals(t, "error"))
		out = Outcome::Error;
	else
		return false;
	return true;
}

// Call counters of the router: per domain (ran, skipped, errors) and skips per (callback, script) pair, named like a
// [route_pairs] key ("actor_on_update@guards_spawner"; the script alone when the callback is not given).
// Review M2: a handler skipped every frame tops "the most skipped"; a rare one (a use, a key press) would never get
// there, so the pairs skipped for the first time most recently are kept too ("latest new skips").
// The pair table is bounded (kMaxPairs); a pair past that counts in its domain only.
class Counters
{
public:
	static constexpr size_t kDomains = size_t(Domain::Level) + 1;
	static constexpr size_t kMaxPairs = 1024;

	struct Totals
	{
		uint64_t ran = 0, skipped = 0, errors = 0;
	};

	void note(Domain d, Outcome o, std::string_view script, std::string_view callback = {})
	{
		Totals& t = m_totals[size_t(d) < kDomains ? size_t(d) : 0];
		switch (o)
		{
		case Outcome::Ran: ++t.ran; break;
		case Outcome::Error: ++t.errors; break;
		case Outcome::Skipped:
			++t.skipped;
			if (!script.empty())
				note_skip(script, callback);
			break;
		}
	}

	const Totals& totals(Domain d) const { return m_totals[size_t(d) < kDomains ? size_t(d) : 0]; }

	// The pairs with the most skips, most first (a tie by name), at most n.
	std::vector<std::pair<std::string, uint64_t>> top_skipped(size_t n) const
	{
		std::vector<Row> all = rows();
		const auto more = [](const Row& a, const Row& b)
		{ return a.count != b.count ? a.count > b.count : a.name < b.name; };
		const size_t k = n < all.size() ? n : all.size();
		std::partial_sort(all.begin(), all.begin() + std::ptrdiff_t(k), all.end(), more);
		std::vector<std::pair<std::string, uint64_t>> out;
		for (size_t i = 0; i < k; ++i)
			out.emplace_back(std::move(all[i].name), all[i].count);
		return out;
	}

	// The pairs skipped for the first time most recently, the latest first, at most n.
	std::vector<std::string> latest_new_skips(size_t n) const
	{
		std::vector<Row> all = rows();
		const auto later = [](const Row& a, const Row& b) { return a.first > b.first; };
		const size_t k = n < all.size() ? n : all.size();
		std::partial_sort(all.begin(), all.begin() + std::ptrdiff_t(k), all.end(), later);
		std::vector<std::string> out;
		for (size_t i = 0; i < k; ++i)
			out.push_back(std::move(all[i].name));
		return out;
	}

	size_t pairs_tracked() const { return m_pairs; }
	uint64_t pairs_dropped() const { return m_dropped; } // skips of pairs past kMaxPairs

	void clear()
	{
		for (Totals& t : m_totals)
			t = Totals();
		m_skips.clear();
		m_pairs = 0;
		m_dropped = 0;
		m_seq = 0;
	}

private:
	struct Skip
	{
		uint64_t count = 0;
		uint64_t first = 0; // order of its first skip
	};
	struct Row
	{
		std::string name;
		uint64_t count, first;
	};
	using Callbacks = std::map<std::string, Skip, std::less<>>; // std::less<>: find() by string_view, no allocation

	void note_skip(std::string_view script, std::string_view callback)
	{
		auto it = m_skips.find(script);
		if (it != m_skips.end())
		{
			const auto jt = it->second.find(callback);
			if (jt != it->second.end())
			{
				++jt->second.count;
				return;
			}
		}
		if (m_pairs >= kMaxPairs)
		{
			++m_dropped;
			return;
		}
		if (it == m_skips.end())
			it = m_skips.emplace(std::string(script), Callbacks()).first;
		it->second.emplace(std::string(callback), Skip{1, ++m_seq});
		++m_pairs;
	}

	std::vector<Row> rows() const
	{
		std::vector<Row> all;
		all.reserve(m_pairs);
		for (const auto& s : m_skips)
			for (const auto& c : s.second)
				all.push_back(Row{c.first.empty() ? s.first : c.first + "@" + s.first, c.second.count, c.second.first});
		return all;
	}

	Totals m_totals[kDomains];
	std::map<std::string, Callbacks, std::less<>> m_skips; // script -> callback -> skips
	size_t m_pairs = 0;
	uint64_t m_dropped = 0;
	uint64_t m_seq = 0;
};
} // namespace routes
} // namespace coop
