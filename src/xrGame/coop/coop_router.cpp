// Anomaly Coop — callback router (F2 T10, review R5 I4) and binder router (F2 T11). See coop_router.h and
// xrCoop/coop_routes.h.
#include "stdafx.h"
#include "pch_script.h"
#include "coop_router.h"
#include "coop_manager.h"
#include "xrCoop/coop_routes.h"

#include <algorithm>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace coop
{
namespace router
{
namespace
{
using routes::Domain;
using routes::Outcome;

constexpr size_t kCacheMax = 4096;   // routed (callback, script) pairs remembered; the cache starts again when full
constexpr u32 kErrorsLogged = 20;    // ltx errors written to the log one by one, the rest only counted
constexpr size_t kTopSkipped = 10;
constexpr const char* kNoneSection = "route_none_functions";
constexpr const char* kRouteSections[] = {"route_scripts", "route_callbacks", "route_callbacks_forced", "route_pairs"};
constexpr size_t kTopBinders = 10;
static_assert(size_t(BinderMethod::NetSaveRelevant) + 1 == routes::kBinderMethodCount,
	"BinderMethod follows routes::kBinderMethodNames");

// A binding (script_binding value) the binder router has been asked about: its resolved actions and call counters.
struct BinderSlot
{
	shared_str binding; // pinned: the cache is keyed by the address of this string, which stays valid while it lives
	routes::BinderAction action[routes::kBinderMethodCount] = {};
	u64 skipped[routes::kBinderMethodCount] = {}; // HostOnly answers, by method
};

struct State
{
	bool loaded = false;
	routes::Table table;
	std::vector<std::string> none_functions;
	std::vector<std::string> files; // read, in order
	u32 rules = 0;
	u32 binder_rules = 0; // (binding, method) rules of [binder_host]
	u32 errors = 0;
	// Table::route lowercases both names and scans the prefixes: a handler is routed once when it is registered, and
	// a pair seen before (one script's many time events) is answered from here.
	std::unordered_map<std::string, Domain> cache; // callback '\1' script, as Lua gave them
	u64 cache_flushes = 0;
	u64 routed[routes::Counters::kDomains] = {}; // of() answers by domain since install
	routes::Counters counters;
	// Binders: one slot per distinct script_binding string; the binder asks with the address of its shared_str, which
	// the slot pins, so the second ask is a pointer lookup.
	std::vector<BinderSlot> binder_slots;
	std::unordered_map<std::string, size_t> binder_by_name; // binding as written -> slot
	std::unordered_map<const char*, size_t> binder_by_ptr;  // address of the binding string -> slot
};

State g;
xrCriticalSection g_lock;

bool iends_with(std::string_view s, std::string_view suffix)
{
	return s.size() >= suffix.size() && routes::detail::iequals(s.substr(s.size() - suffix.size()), suffix);
}

// coop_route.ltx first, then coop_route_*.ltx, then coop_compat_*.ltx, each group by name. file_list_open, not
// file_list_open_ex: a checked rescan drops the subfolders of configs (coop_main.script used to note the same).
std::vector<std::string> route_files(bool& have_main)
{
	std::vector<std::string> extra, compat;
	have_main = false;
	xr_vector<LPSTR>* list = FS.file_list_open("$game_config$", FS_ListFiles | FS_RootOnly);
	if (list)
	{
		for (LPSTR name : *list)
		{
			if (!name)
				continue;
			const std::string n = routes::detail::lowered(name);
			if (n == "coop_route.ltx")
				have_main = true;
			else if (routes::detail::istarts_with(n, "coop_route_") && iends_with(n, ".ltx"))
				extra.push_back(n);
			else if (routes::detail::istarts_with(n, "coop_compat_") && iends_with(n, ".ltx"))
				compat.push_back(n);
		}
		FS.file_list_close(list);
	}
	std::sort(extra.begin(), extra.end());
	std::sort(compat.begin(), compat.end());
	std::vector<std::string> files;
	if (have_main)
		files.emplace_back("coop_route.ltx");
	files.insert(files.end(), extra.begin(), extra.end());
	files.insert(files.end(), compat.begin(), compat.end());
	return files;
}

using IniLines = std::vector<std::pair<std::string, std::string>>;

// The `name = value` lines of a section, in file order; empty when the section is not there.
IniLines section_lines(CInifile& ini, const char* sec)
{
	IniLines lines;
	if (!ini.section_exist(sec))
		return lines;
	const u32 n = ini.line_count(sec);
	for (u32 i = 0; i < n; ++i)
	{
		LPCSTR key = nullptr;
		LPCSTR value = nullptr;
		if (ini.r_line(sec, int(i), &key, &value) && key)
			lines.emplace_back(key, value ? value : "");
	}
	return lines;
}

// One file's route sections into the table, its none functions into the list. Under g_lock.
void read_file(const std::string& name)
{
	string_path path;
	FS.update_path(path, "$game_config$", name.c_str());
	CInifile ini(path, TRUE, TRUE, FALSE); // DLTX: mod_coop_route_*.ltx add to coop_route.ltx
	for (const char* sec : kRouteSections)
	{
		if (!ini.section_exist(sec))
			continue;
		const IniLines lines = section_lines(ini, sec);
		std::vector<std::string> errors;
		for (const routes::Override& o : routes::parse_section(sec, lines, &errors))
		{
			g.table.add(o);
			++g.rules;
		}
		for (const std::string& e : errors)
			if (g.errors++ < kErrorsLogged)
				::Msg("! [coop] configs\\%s: %s", name.c_str(), e.c_str());
	}
	// F2 T11: [binder_host], `script_binding = method, method`
	if (ini.section_exist(routes::kBinderHostSection))
	{
		std::vector<std::string> errors;
		for (const routes::BinderRule& r :
			routes::parse_binder_section(routes::kBinderHostSection, section_lines(ini, routes::kBinderHostSection), &errors))
		{
			g.table.add(r);
			g.binder_rules += u32(r.methods.size());
		}
		for (const std::string& e : errors)
			if (g.errors++ < kErrorsLogged)
				::Msg("! [coop] configs\\%s: %s", name.c_str(), e.c_str());
	}
	if (ini.section_exist(kNoneSection))
	{
		const u32 n = ini.line_count(kNoneSection);
		for (u32 i = 0; i < n; ++i)
		{
			LPCSTR key = nullptr;
			LPCSTR value = nullptr;
			if (!ini.r_line(kNoneSection, int(i), &key, &value) || !key || !*key)
				continue;
			const std::string fn(routes::detail::trimmed(key));
			if (!fn.empty() && std::find(g.none_functions.begin(), g.none_functions.end(), fn) == g.none_functions.end())
				g.none_functions.push_back(fn);
		}
	}
	g.files.push_back(name);
}

// Under g_lock.
Domain route_of(std::string_view callback, const std::string& script)
{
	std::string key;
	key.reserve(callback.size() + 1 + script.size());
	key.append(callback.data(), callback.size()).append(1, '\1').append(script);
	const auto it = g.cache.find(key);
	if (it != g.cache.end())
		return it->second;
	const Domain d = g.table.route(callback, script);
	if (g.cache.size() >= kCacheMax)
	{
		g.cache.clear();
		++g.cache_flushes;
	}
	g.cache.emplace(std::move(key), d);
	return d;
}

// Outside a session nothing is co-op routed: single player runs every handler as the stock game does.
bool runs_now(Domain d)
{
	const Manager& m = mgr();
	return !m.active() || routes::runs_here(d, m.is_host());
}

bool arg_string(lua_State* L, int idx, std::string_view& out)
{
	if (lua_type(L, idx) != LUA_TSTRING)
		return false;
	size_t len = 0;
	const char* s = lua_tolstring(L, idx, &len);
	out = std::string_view(s, len);
	return true;
}

// The functions below never raise a Lua error on bad arguments (R-11.4): a wrong call from an addon gets nil / true /
// nothing, like the rest of the coop namespace.

// coop.route.of(callback, func) -> domain, script
int l_of(lua_State* L)
{
	std::string_view callback;
	if (!arg_string(L, 1, callback))
	{
		lua_pushnil(L);
		return 1;
	}
	load_overrides();
	std::string script = "?";
	if (lua_type(L, 2) == LUA_TFUNCTION)
	{
		lua_Debug ar;
		lua_pushvalue(L, 2);
		if (lua_getinfo(L, ">S", &ar)) // pops the function
			script = ar.what && !xr_strcmp(ar.what, "C") ? std::string("[C]") : routes::script_key(ar.source ? ar.source : "");
	}
	Domain d = Domain::Owner;
	{
		xrCriticalSectionGuard guard(g_lock);
		if (callback.empty()) // review M4: coop_route.script_of only asks the script; not a handler, not cached
			d = g.table.route(callback, script);
		else
		{
			d = route_of(callback, script);
			++g.routed[size_t(d)];
		}
	}
	lua_pushstring(L, routes::domain_name(d));
	lua_pushlstring(L, script.data(), script.size());
	return 2;
}

// coop.route.runs(domain) -> bool
int l_runs(lua_State* L)
{
	Domain d = Domain::Owner; // an unknown name runs, as owner
	std::string_view name;
	if (arg_string(L, 1, name))
		routes::parse_domain(name, d);
	lua_pushboolean(L, runs_now(d) ? 1 : 0);
	return 1;
}

// coop.route.note(domain, outcome[, script])
int l_note(lua_State* L)
{
	std::string_view name, outcome, script, callback;
	Domain d = Domain::Owner;
	Outcome o = Outcome::Ran;
	if (!arg_string(L, 1, name) || !arg_string(L, 2, outcome) || !routes::parse_domain(name, d) ||
		!routes::parse_outcome(outcome, o))
		return 0;
	arg_string(L, 3, script);
	arg_string(L, 4, callback); // review M2: skips are kept per (callback, script)
	xrCriticalSectionGuard guard(g_lock);
	g.counters.note(d, o, script, callback);
	return 0;
}

// coop.route.none_functions() -> {"module.function", ...}
int l_none_functions(lua_State* L)
{
	load_overrides();
	std::vector<std::string> list;
	{
		xrCriticalSectionGuard guard(g_lock);
		list = g.none_functions;
	}
	lua_createtable(L, int(list.size()), 0);
	for (size_t i = 0; i < list.size(); ++i)
	{
		lua_pushlstring(L, list[i].data(), list[i].size());
		lua_rawseti(L, -2, int(i + 1));
	}
	return 1;
}

// --- F2 T11: binders ---

// Under g_lock. The slot of a binding: made on the first ask, with the actions resolved against the table. A reference
// into g.binder_slots lives until the next call of this function.
BinderSlot& binder_slot(const char* binding)
{
	const auto by_ptr = g.binder_by_ptr.find(binding);
	if (by_ptr != g.binder_by_ptr.end())
		return g.binder_slots[by_ptr->second];
	size_t index = 0;
	const auto by_name = g.binder_by_name.find(binding);
	if (by_name != g.binder_by_name.end())
		index = by_name->second;
	else
	{
		BinderSlot slot;
		slot.binding = binding; // docks the string: the same str_value the binder holds, kept alive by the slot
		for (size_t i = 0; i < routes::kBinderMethodCount; ++i)
			slot.action[i] = g.table.binder_action(binding, routes::kBinderMethodNames[i]);
		index = g.binder_slots.size();
		g.binder_slots.push_back(std::move(slot));
		g.binder_by_name.emplace(binding, index);
	}
	g.binder_by_ptr.emplace(binding, index);
	return g.binder_slots[index];
}
} // namespace

void install()
{
	xrCriticalSectionGuard guard(g_lock);
	g.loaded = false;
	g.table.clear();
	g.none_functions.clear();
	g.files.clear();
	g.rules = 0;
	g.binder_rules = 0;
	g.errors = 0;
	g.cache.clear();
	g.cache_flushes = 0;
	for (u64& n : g.routed)
		n = 0;
	g.counters.clear();
	g.binder_slots.clear();
	g.binder_by_name.clear();
	g.binder_by_ptr.clear();
}

void reset()
{
	xrCriticalSectionGuard guard(g_lock);
	g.counters.clear();
	for (BinderSlot& b : g.binder_slots)
		std::fill(std::begin(b.skipped), std::end(b.skipped), u64(0));
}

void load_overrides()
{
	xrCriticalSectionGuard guard(g_lock);
	if (g.loaded)
		return;
	g.loaded = true;
	bool have_main = false;
	for (const std::string& name : route_files(have_main))
		read_file(name);
	if (!have_main)
		::Msg("! [coop] configs\\coop_route.ltx not found: callbacks route by their defaults and the other files");
	g.cache.clear();
	g.binder_slots.clear(); // resolved against the table just loaded
	g.binder_by_name.clear();
	g.binder_by_ptr.clear();
	::Msg("[coop] callback routes: %u rule(s) and %u binder rule(s) from %u file(s), %u none function(s), %u ltx error(s)",
		g.rules, g.binder_rules, u32(g.files.size()), u32(g.none_functions.size()), g.errors);
}

void register_lua(lua_State* L)
{
	static const luaL_Reg fns[] = {
		{"of", &l_of},
		{"runs", &l_runs},
		{"note", &l_note},
		{"none_functions", &l_none_functions},
		{nullptr, nullptr},
	};
	lua_pushstring(L, "coop");
	lua_rawget(L, LUA_GLOBALSINDEX);
	if (!lua_istable(L, -1))
	{
		lua_pop(L, 1);
		return; // coop_script logs it
	}
	lua_createtable(L, 0, 4);
	for (const luaL_Reg* f = fns; f->name; ++f)
	{
		lua_pushcfunction(L, f->func);
		lua_setfield(L, -2, f->name);
	}
	lua_setfield(L, -2, "route");
	lua_pop(L, 1);
}

routes::BinderAction binder_action(const char* binding, BinderMethod method)
{
	if (!binding || !*binding || !mgr().is_client()) // the host and single player run the stock code
		return routes::BinderAction::Run;
	const size_t i = size_t(method);
	if (i >= routes::kBinderMethodCount)
		return routes::BinderAction::Run;
	load_overrides();
	xrCriticalSectionGuard guard(g_lock);
	BinderSlot& slot = binder_slot(binding);
	if (slot.action[i] == routes::BinderAction::HostOnly)
		++slot.skipped[i];
	return slot.action[i];
}

void print_status()
{
	xrCriticalSectionGuard guard(g_lock);
	if (!g.loaded)
	{
		::Msg("[coop] routes: no handler routed yet (coop_route*.ltx is read on the first registration)");
		return;
	}
	std::string files;
	for (const std::string& f : g.files)
		files += (files.empty() ? "" : ", ") + f;
	::Msg("[coop] routes: %u rule(s), %u none function(s), %u ltx error(s) from %s", g.rules,
		u32(g.none_functions.size()), g.errors, files.empty() ? "no file" : files.c_str());
	const u64* r = g.routed;
	::Msg("[coop] routes: handlers routed host %llu, owner %llu, view %llu, none %llu, level %llu | cache %u pair(s), "
		  "%llu flush(es)",
		r[size_t(Domain::Host)], r[size_t(Domain::Owner)], r[size_t(Domain::View)], r[size_t(Domain::None)],
		r[size_t(Domain::Level)], u32(g.cache.size()), g.cache_flushes);
	for (const Domain d : {Domain::Host, Domain::Owner, Domain::View, Domain::None, Domain::Level})
	{
		const routes::Counters::Totals& t = g.counters.totals(d);
		::Msg("[coop] routes:   %-5s ran %llu, skipped %llu, errors %llu", routes::domain_name(d), t.ran, t.skipped,
			t.errors);
	}
	// Review M2: per (callback, script). A handler skipped every frame tops the first list; a rare one (a use, a key
	// press) shows in the second, the pairs skipped for the first time most recently.
	const std::vector<std::pair<std::string, u64>> top = g.counters.top_skipped(kTopSkipped);
	std::string line;
	for (const std::pair<std::string, u64>& s : top)
		line += (line.empty() ? "" : ", ") + s.first + " " + std::to_string(s.second);
	::Msg("[coop] routes: most skipped %s%s", line.empty() ? "(none)" : line.c_str(),
		g.counters.pairs_dropped() ? " (more pairs than the table keeps)" : "");
	line.clear();
	for (const std::string& s : g.counters.latest_new_skips(kTopSkipped))
		line += (line.empty() ? "" : ", ") + s;
	::Msg("[coop] routes: latest new skips %s (%u pair(s) skipped so far)", line.empty() ? "(none)" : line.c_str(),
		u32(g.counters.pairs_tracked()));

	// F2 T11: the binder rules. skipped = host-only calls a client did not make (the Lua guards of
	// coop_client_binders.script count their errors in their own [coop-binders] lines).
	std::vector<std::pair<std::string, u64>> rows;
	u64 skipped = 0;
	for (const BinderSlot& b : g.binder_slots)
		for (size_t i = 0; i < routes::kBinderMethodCount; ++i)
		{
			skipped += b.skipped[i];
			if (b.skipped[i])
				rows.emplace_back(std::string(b.binding.c_str()) + ":" + std::string(routes::kBinderMethodNames[i]), b.skipped[i]);
		}
	std::sort(rows.begin(), rows.end(), [](const std::pair<std::string, u64>& a, const std::pair<std::string, u64>& b)
		{ return a.second != b.second ? a.second > b.second : a.first < b.first; });
	::Msg("[coop] binders: %u host-only rule(s) (%u binding(s) asked on this client), client calls skipped %llu",
		g.binder_rules, u32(g.binder_slots.size()), skipped);
	line.clear();
	for (size_t i = 0; i < rows.size() && i < kTopBinders; ++i)
		line += (line.empty() ? "" : ", ") + rows[i].first + " " + std::to_string(rows[i].second);
	::Msg("[coop] binders: most skipped %s", line.empty() ? "(none)" : line.c_str());
}
} // namespace router
} // namespace coop
