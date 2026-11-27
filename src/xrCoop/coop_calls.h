// Anomaly Coop - call router table (MOD_LAYER.md §2.3). Every engine export a Lua script can reach in the
// sources below has a class that decides what a call does on a client:
//   world - changes the world everybody shares (spawn, release, NPCs and their AI, story, relations, NPC money, server
//           objects, cars and helicopters, time). The host runs it; a client sends it to the host where the engine has
//           a request path (ReqSpawn, story, ReqTransfer, the money journal, TimeRequest) and refuses it otherwise.
//   owner - changes what one player owns: the own actor (health, needs, boosters, slots, movement) and own items. Runs
//           on the owner's machine; on another's object it is a world call.
//   view  - picture, sound and UI of this machine (HUD, effectors, particles, map spots, the look of the current
//           weather, script plumbing). Runs locally.
//   read  - reads (positions, alife():object, time, info portions) and script-local values. Runs locally: on a client
//           against the mirror.
// A name is "<namespace>:<name>": the Lua class (alife = alife(), game_object, CCar, CHelicopter, CPhysicObject,
// CConsole, hanging_lamp, CZoneCampfire, cse_* server objects, CTime, ray_pick ...) or module (level, actor_stats,
// debug_render, game, hud_adjust, relation_registry, weather; _G for globals), then the method, field or property. A
// property or a getter + setter pair has one row with the setter's class (reading is always read). A name the table
// does not know is read. Letter case does not matter (an ltx reader may lowercase keys).
//
// The sources: tools/list_exports.py (its docstring lists them: alife_simulator_script.cpp, script_game_object_script*,
// level_script.cpp, MosquitoBald_script.cpp, HangingLamp.cpp, helicopter_script.cpp, CarScript.cpp,
// PhysicObject_script.cpp, console_registrator_script.cpp, xrServerEntities/xrServer_Objects*_script*.cpp). Every
// .def, def, .def_readwrite, .def_readonly and .property there has a row in kCalls; the virtual methods a server
// object class gets from its luabind_class_* macro (STATE_Read, on_register, switch_online ...) are classified once,
// for every cse_* class, by kMacroVirtuals. Other engine exports (vectors, ini files, net packets, UI, sounds, the
// physics shell, particles, planners ...) are outside the table and read (MOD_LAYER §6). tools/list_exports.py --check
// (run by tests/test_calls.cpp on every unit-test run) fails on a row for no export, an unclassified export and a
// read-only export (def_readonly, a property without a setter) that is not read.
//
// Overrides: [call_classes] `namespace:name = world|owner|view|read` in configs\coop_route*.ltx and coop_compat_*.ltx
// (parse_overrides, Classes). An override can move a function to any class; the request paths stay the engine's
// (kRequestPaths).
//
// Only the engine functions with a gate (xrGame/coop/coop_callgate.h client_gate) consult the class at run time;
// the others run as in single player whatever their row says. The gate never decides how often: the host limits what
// a request path sends (coop_requests.cpp buckets, the session limits, coop.sync.limit).
//
// The table itself is coop_calls_table.inl (kCalls, kMacroVirtuals: data tools/list_exports.py checks row by row). The writer gate
// that follows this gate for the script writes is coop_writer_gate.h.
//
// Pure C++17, header-only, no engine dependencies: used by the engine and the unit-test exe.
#pragma once

#include "coop_text.h"
#include "coop_writer_gate.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace coop
{
namespace calls
{
enum class CallClass : uint8_t
{
	World,
	Owner,
	View,
	Read
};

inline const char* class_name(CallClass c)
{
	switch (c)
	{
	case CallClass::World: return "world";
	case CallClass::Owner: return "owner";
	case CallClass::View: return "view";
	case CallClass::Read: return "read";
	}
	return "read";
}

namespace detail
{
// "namespace:function": two non-empty runs of letters, digits and '_' around one ':'.
inline bool valid_name(std::string_view n)
{
	const size_t colon = n.find(':');
	if (colon == std::string_view::npos || colon == 0 || colon + 1 == n.size())
		return false;
	for (size_t i = 0; i < n.size(); ++i)
		if (i != colon && !ascii::ident_char(n[i]))
			return false;
	return true;
}
} // namespace detail

// Accepts world, owner, view, read; surrounding blanks and letter case are ignored.
inline bool parse_class(std::string_view text, CallClass& out)
{
	const std::string_view t = ascii::trim(text, ascii::space_or_eol);
	for (const CallClass c : {CallClass::World, CallClass::Owner, CallClass::View, CallClass::Read})
		if (ascii::iequals(t, class_name(c)))
		{
			out = c;
			return true;
		}
	return false;
}

struct Call
{
	std::string_view name;
	CallClass cls;
};

#include "coop_calls_table.inl"

// Client calls the engine sends to the host instead of running them (MOD_LAYER §2.3). own_only: only for a target
// the client owns (its actor, an item under it); another's target has no path.
struct RequestPath
{
	std::string_view name;
	bool own_only;
};

inline constexpr RequestPath kRequestPaths[] = {
	// ReqSpawn (coop_requests.cpp lua_create / lua_clone / lua_register_spawn, the event router's filter_spawn): an item
	// into our own inventory only (client_forward_spawn); own = the parent is our actor or under it (own_spawn).
	{"alife:create", true},
	{"alife:create_ammo", true},
	{"alife:clone_weapon", true},
	{"alife:register", true}, // a temporary object of create(..., false) / clone_weapon(..., false)
	{"level:spawn_item", true},
	{"alife:release", true},               // client_destroy of an own item (lua_release, own_release)
	{"game_object:destroy_object", true}, // the event router's on_destroy (own_destroy)
	// story (inventory_owner_info.cpp -> coop_story on_info): our actor's infos only (owner id 0); an NPC's info stays
	// in this machine's mirror, counted as requested
	{"game_object:give_info_portion", false},
	{"game_object:disable_info_portion", false},
	{"alife:give_info", true}, // id 0 = our actor: story::on_info; another's registry is the host's
	{"alife:disable_info", true},
	// story Relation (relation_registry.cpp -> coop_story): a relation with a player; between NPCs it stays in this
	// machine's mirror, counted as requested
	{"game_object:set_relation", false},
	{"game_object:set_goodwill", false},
	{"game_object:force_set_goodwill", false},
	{"game_object:change_goodwill", false},
	{"game_object:set_community_goodwill", false},
	{"relation_registry:set_community_goodwill", false},
	{"relation_registry:change_community_goodwill", false},
	{"relation_registry:set_community_relation", false},
	// story tasks (GameTaskManager / GameTask hooks: on_give_task, on_task_state; personal tasks stay local)
	{"game_object:give_task", false},
	{"game_object:set_task_state", false},
	{"game_object:transfer_item", true}, // ReqTransfer / ReqTake / ReqDrop (coop_events.cpp); own: own_move
	{"game_object:take_item", true},     // the same
	{"game_object:give_money", true},    // our actor: the money journal (GE_MONEY -> money::client_on_actor_money)
	{"game_object:transfer_money", true}, // our actor pays: its side goes to the journal, the receiver's is the host's
	{"level:change_game_time", false},    // TimeRequest (coop_worldstate.cpp intercept_time_jump)
};

namespace detail
{
// kCalls by lowercase name, built once.
inline const std::unordered_map<std::string, CallClass>& table()
{
	static const std::unordered_map<std::string, CallClass> t = []
	{
		std::unordered_map<std::string, CallClass> m;
		m.reserve(call_count() * 2);
		for (const Call& c : kCalls)
			m.emplace(ascii::lowered(c.name), c.cls);
		return m;
	}();
	return t;
}

// The class kMacroVirtuals gives `name` (a cse_* namespace and a macro virtual), false if none.
inline bool macro_virtual(std::string_view name, CallClass& out)
{
	const size_t colon = name.find(':');
	if (colon == std::string_view::npos || !ascii::istarts_with(name.substr(0, colon), kMacroPrefix))
		return false;
	const std::string_view fn = name.substr(colon + 1);
	for (const Call& v : kMacroVirtuals)
		if (ascii::iequals(v.name, fn))
		{
			out = v.cls;
			return true;
		}
	return false;
}
} // namespace detail

// Whether the table classifies `name`: a kCalls row, or a server object's macro virtual.
inline bool has_row(std::string_view name)
{
	if (detail::table().count(ascii::lowered(name)) != 0)
		return true;
	CallClass unused = CallClass::Read;
	return detail::macro_virtual(name, unused);
}

// The built-in class of a function: its row, else its macro virtual, else read.
inline CallClass class_of(std::string_view name)
{
	const auto& t = detail::table();
	const auto it = t.find(ascii::lowered(name));
	if (it != t.end())
		return it->second;
	CallClass c = CallClass::Read;
	detail::macro_virtual(name, c);
	return c;
}

// Whether the engine has a request path for this call on a client, for a target it owns or not.
inline bool has_request_path(std::string_view name, bool own_target)
{
	for (const RequestPath& p : kRequestPaths)
		if (ascii::iequals(p.name, name))
			return own_target || !p.own_only;
	return false;
}

// The class matrix (MOD_LAYER §2.3):
//   host (or no session)  run everything;
//   client  read, view    run;
//           owner         run on an own target; on another's target as world;
//           world         a request where the engine has a path, else refused.
inline Gate gate(CallClass cls, bool is_client, bool own_target, bool has_request_path)
{
	if (!is_client)
		return Gate::Run;
	switch (cls)
	{
	case CallClass::Read:
	case CallClass::View: return Gate::Run;
	case CallClass::Owner:
		if (own_target)
			return Gate::Run;
		break;
	case CallClass::World: break;
	}
	return has_request_path ? Gate::Request : Gate::Refuse;
}

// The whole decision of the engine's gate (coop_callgate.cpp):
//   not a client in the host's world   run (the host and single player run everything, bad arguments included);
//   bad arguments (NaN, a missing id)   refused, even while applying: a bad number neither runs nor goes to the host;
//   applying a host command             run: an engine apply scope (entities::ClientApply) or the host's story being
//                                       applied (story::applying) is the host's change (the event router still drops
//                                       echoes); our Lua mirrors are NOT under it:
//                                       coop_npc_meet / coop_npc_world get the host's events through
//                                       coop_script deliver_inbox, outside both scopes, so a world
//                                       function they call (enable_talk, explode ...) must stay without a gate until
//                                       they run under an apply scope;
//   else                                the class matrix (gate).
inline Gate decide(CallClass cls, bool is_client, bool applying, bool own_target, bool has_request_path, bool args_ok)
{
	if (!is_client)
		return Gate::Run;
	if (!args_ok)
		return Gate::Refuse;
	if (applying)
		return Gate::Run;
	return gate(cls, true, own_target, has_request_path);
}

// A map server leads its level. The events of its objects run there and what the world must know goes up (coop_events filter_event,
// coop_levels_xfer), so a call on an object of its level runs as on the host. Refused as on a client, no trader would have any stock,
// no NPC would give back what he ate and nobody would come to his place. What is not here stays as on a client: the clock, the
// difficulty, the story and the relations have their own ways to the world server.
inline constexpr std::string_view kLevelCalls[] = {
	"alife:disable_info",
	"alife:give_info",
	"game_object:bleeding",
	"game_object:buy_supplies",
	"game_object:change_health",
	"game_object:change_morale",
	"game_object:change_power",
	"game_object:change_psy_health",
	"game_object:change_radiation",
	"game_object:change_satiety",
	"game_object:destroy_object",
	"game_object:enable_level_changer",
	"game_object:give_money",
	"game_object:health",
	"game_object:kill",
	"game_object:morale",
	"game_object:power",
	"game_object:psy_health",
	"game_object:radiation",
	"game_object:satiety",
	"game_object:set_health_ex",
	"game_object:set_npc_position",
	"game_object:take_item",
	"game_object:transfer_item",
	"game_object:transfer_money",
	"hanging_lamp:turn_off",
	"hanging_lamp:turn_on",
	"level:spawn_item",
	"level:spawn_phantom",
};

inline bool is_level_call(std::string_view name)
{
	for (std::string_view call : kLevelCalls)
		if (ascii::iequals(call, name))
			return true;
	return false;
}

// The answer on a map server: `client` is what decide() says. A request keeps its path, a bad number runs nowhere.
inline Gate decide_leading(std::string_view name, Gate client, bool args_ok)
{
	return client == Gate::Refuse && args_ok && is_level_call(name) ? Gate::Run : client;
}

// The "own target" of the gated call sites, the same as the engine paths behind them:
// transfer_item / take_item: our actor is a side of the move, the item is ours or it comes to us (the
// host checks owner, reach and the sides' locks in host_move).
inline bool own_move(bool item_ours, bool other_side_ours) { return item_ours || other_side_ours; }
// alife:create / create_ammo / clone_weapon / register, level:spawn_item: registered now only into our own inventory
// (client_forward_spawn); a temporary object (register = false) passes, its alife():register is gated, so Lua keeps
// getting the temporary object it got before.
inline bool own_spawn(bool registers_now, bool parent_ours) { return !registers_now || parent_ours; }
// alife:release: an item under our actor, never our actor itself.
inline bool own_release(bool is_our_actor, bool ours) { return ours && !is_our_actor; }
// destroy_object: as the event router's on_destroy: ours and free items (no parent, not a creature) go to the host
// (client_destroy), an object outside the host's world is local; anything else is the host's.
inline bool own_destroy(bool in_host_world, bool ours, bool free_item) { return !in_host_world || ours || free_item; }

// One [call_classes] line.
struct Override
{
	std::string name;
	CallClass cls = CallClass::Read;
};

// Parses the `name = class` lines of a [call_classes] section. A bad line is skipped and reported in `errors` (may be
// null) as "call_classes: <name> = <value>: <why>"; the other lines still parse. A name the table does not know is
// accepted (a modded exe's export).
inline std::vector<Override> parse_overrides(const std::vector<std::pair<std::string, std::string>>& lines,
	std::vector<std::string>* errors)
{
	std::vector<Override> out;
	for (const std::pair<std::string, std::string>& line : lines)
	{
		const std::string_view key = ascii::trim(line.first, ascii::space_or_eol);
		const std::string_view value = ascii::trim(line.second, ascii::space_or_eol);
		Override o;
		const char* why = nullptr;
		if (!detail::valid_name(key))
			why = "expected namespace:function (letters, digits, _)";
		else if (!parse_class(value, o.cls))
			why = "unknown class (world, owner, view, read)";
		if (why)
		{
			if (errors)
				errors->push_back("call_classes: " + std::string(key) + " = " + std::string(value) + ": " + why);
			continue;
		}
		o.name = std::string(key);
		out.push_back(std::move(o));
	}
	return out;
}

// kCalls with the ltx overrides on top; a later override of a name replaces the earlier one.
class Classes
{
public:
	void add(const Override& o)
	{
		if (detail::valid_name(o.name))
			m_overrides[ascii::lowered(o.name)] = o.cls;
	}
	void clear() { m_overrides.clear(); }
	size_t size() const { return m_overrides.size(); }
	bool overridden(std::string_view name) const { return m_overrides.count(ascii::lowered(name)) != 0; }

	CallClass class_of(std::string_view name) const
	{
		if (!m_overrides.empty())
		{
			const auto it = m_overrides.find(ascii::lowered(name));
			if (it != m_overrides.end())
				return it->second;
		}
		return calls::class_of(name);
	}

private:
	std::map<std::string, CallClass> m_overrides; // lowercase name
};

// Gated calls on this machine for coop_status: totals, refusals per name for "the most refused" list, and the log
// budget: the first kLoggedPerName refusals of each name get a log line (one noisy name, the weather of
// level_weathers, must not hide the others), the rest are only counted. The per-name table is bounded: a name past
// kMaxNames counts in the totals only and is not logged.
class Counters
{
public:
	static constexpr size_t kMaxNames = 512;
	static constexpr size_t kLoggedPerName = 10;

	struct Totals
	{
		uint64_t ran = 0, requested = 0, refused = 0;
	};

	void note(std::string_view name, Gate g)
	{
		switch (g)
		{
		case Gate::Run: ++m_totals.ran; break;
		case Gate::Request: ++m_totals.requested; break;
		case Gate::Refuse:
			++m_totals.refused;
			if (const auto it = m_refused.find(name); it != m_refused.end())
				++it->second.refused;
			else if (m_refused.size() < kMaxNames)
				m_refused.emplace(std::string(name), Name{1, 0});
			else
				++m_dropped;
			break;
		}
	}

	const Totals& totals() const { return m_totals; }

	// The names with the most refusals, most first (a tie by name), at most n.
	std::vector<std::pair<std::string, uint64_t>> top_refused(size_t n) const
	{
		std::vector<std::pair<std::string, uint64_t>> all;
		all.reserve(m_refused.size());
		for (const auto& [name, counts] : m_refused)
			all.emplace_back(name, counts.refused);
		const auto more = [](const std::pair<std::string, uint64_t>& a, const std::pair<std::string, uint64_t>& b)
		{ return a.second != b.second ? a.second > b.second : a.first < b.first; };
		const size_t k = n < all.size() ? n : all.size();
		std::partial_sort(all.begin(), all.begin() + std::ptrdiff_t(k), all.end(), more);
		all.resize(k);
		return all;
	}

	// After note(name, Refuse): true for the first kLoggedPerName refusals of `name` since clear(), that refusal gets
	// its own log line. False for a name the table does not track (no refusal noted, or past kMaxNames).
	bool take_log_line(std::string_view name)
	{
		const auto it = m_refused.find(name);
		if (it == m_refused.end() || it->second.logged >= kLoggedPerName)
			return false;
		++it->second.logged;
		return true;
	}

	// Log lines taken for `name` since clear().
	size_t logged(std::string_view name) const
	{
		const auto it = m_refused.find(name);
		return it == m_refused.end() ? 0 : it->second.logged;
	}

	size_t names_tracked() const { return m_refused.size(); }
	uint64_t names_dropped() const { return m_dropped; }

	void clear()
	{
		m_totals = Totals();
		m_refused.clear();
		m_dropped = 0;
	}

private:
	struct Name
	{
		uint64_t refused = 0;
		size_t logged = 0;
	};

	Totals m_totals;
	std::map<std::string, Name, std::less<>> m_refused; // std::less<>: find() by string_view
	uint64_t m_dropped = 0;
};

} // namespace calls
} // namespace coop
