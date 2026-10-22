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
// Pure C++17, header-only, no engine dependencies: used by the engine, the unit-test exe and the Lua test runner.
#pragma once

#include <cstddef>
#include <cstdint>
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
// the handler's script decides, owner by default.
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
	{kUniqueCall, Domain::Owner},
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

	void clear()
	{
		m_pairs.clear();
		m_forced.clear();
		m_callbacks.clear();
		m_scripts.clear();
		m_prefixes.clear();
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
} // namespace routes
} // namespace coop
