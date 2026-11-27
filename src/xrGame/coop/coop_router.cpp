// Anomaly Coop — callback router and binder router. See coop_router.h and xrCoop/coop_routes.h.
//
// The code scope and the writer gate of script writes, next to runs_now:
//   - Scope: coop_route.script registers every wrapper it makes with its domain (coop.route.scoped(wrapper, domain)); the table is
//     kept in the Lua registry, weak-keyed. The scope of the running code is the domain of the innermost registered wrapper on the
//     live Lua stack (coop.route.scope(); writer_refuses below), so a Lua error leaves nothing stale.
//   - Gate: writer_refuses(call, op, target) is what alife_simulator_script.cpp and level_script.cpp ask after the call gate. The
//     co-op host, a player's machine and single player get false at once (calls::decide_writer_gate: Run, Mirror). The world server
//     and a map server compute the target's root (writing::root_facts_of), its writer (Mode::writer_of), the scope, and the pure
//     verdict (writer::gate); Refuse, Ask and Order are counted per (call, script) with one log line each (the census, coop_status),
//     and so is a world server's create that runs into a led map from code that may run on its map server as well
//     (calls::census_note).
//     Only under coop_writer_gate = enforce is a Refuse refused; the default is log, which refuses nothing. Ask and Order have no
//     engine path: they keep the call gate's answer in both settings (counted).
//   - Setting: the console command coop_writer_gate = log | enforce (register_console; log by default).
#include "stdafx.h"
#include "pch_script.h"
#include "coop_router.h"
#include "coop_manager.h"
#include "coop_net.h"
#include "coop_writer.h" // writing::root_facts_of
#include "coop_levels.h"
#include "coop_lua.h" // lua_api::arg_whole_u32
#include "xrCoop/coop_lua_errors.h" // lua_errors::LogBudget
#include "xrCoop/coop_mode.h"
#include "xrCoop/coop_text.h"

#include "ai_space.h"
#include "script_engine.h"
#include "alife_simulator.h"
#include "alife_object_registry.h"
#include "xrServer_Objects_ALife_Monsters.h"
#include "../../xrEngine/XR_IOConsole.h"
#include "../../xrEngine/xr_ioc_cmd.h"

#include <algorithm>
#include <atomic>
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
	// Set under g_lock once the ltx files are read, cleared by install(); read without the lock first (load_overrides, every binder
	// update on a mirror): a true there means the table is complete.
	std::atomic<bool> loaded{false};
	routes::Table table;
	std::vector<std::string> none_functions;
	std::vector<std::string> files; // read, in order
	u32 rules = 0;
	u32 binder_rules = 0; // (binding, method) rules of [binder_host]
	u32 errors = 0;
	// Table::rule lowercases both names and scans the prefixes: a handler is routed once when it is registered, and
	// a pair seen before (one script's many time events) is answered from here.
	std::unordered_map<std::string, Domain> cache; // callback '\1' script, as Lua gave them
	u64 cache_flushes = 0;
	u64 routed[routes::Counters::kDomains] = {}; // of() answers by domain since install
	routes::Counters counters;
	lua_errors::LogBudget batch_refusals{8}; // note_batch's lines of refused rows: the first ones, then one in a hundred
	// Binders: one slot per distinct script_binding string; the binder asks with the address of its shared_str, which
	// the slot pins, so the second ask is a pointer lookup.
	std::vector<BinderSlot> binder_slots;
	std::unordered_map<std::string, size_t> binder_by_name; // binding as written -> slot
	std::unordered_map<const char*, size_t> binder_by_ptr;  // address of the binding string -> slot
	// What the writer gate refused, asked or ordered (or would have, under log), and the world's creates that ran into a led map, per
	// (kind, call, script)
	calls::WriterCensus census;
	// The kind of a new object of a class (section_kind), per factory class; a constant of the process (the classes are
	// registered once), so install and reset keep it
	std::unordered_map<CLASS_ID, calls::Target> class_kinds;
};

State g;
xrCriticalSection g_lock;
// This process's coop_writer_gate: log until the console command says otherwise (CCC_WriterGate). Read by the script gate and by
// the id steps of xrServer (writer_gate()).
std::atomic<calls::WriterGate> g_writer_gate{calls::WriterGate::Log};

// --- the code scope ---

constexpr int kScopeDepth = 64; // Lua frames looked at for the innermost wrapper and the calling script
char g_scope_key = 0; // its address keys the wrapper -> domain table in the Lua registry

// Pushes the wrapper -> domain table of L; false (nothing pushed) when this state has none.
bool push_scopes(lua_State* L)
{
	lua_pushlightuserdata(L, &g_scope_key);
	lua_rawget(L, LUA_REGISTRYINDEX);
	if (lua_istable(L, -1))
		return true;
	lua_pop(L, 1);
	return false;
}

// The helpers whose frames do not name the maker of a write: the stock _g.script (alife_create_item, alife_release ...), axr_main,
// coop_route.script and the utils_*.script (as coop_core_world.script's HELPER_SCRIPTS for news).
bool helper_script(const std::string& key)
{
	return key == "_g" || key == "axr_main" || key == "coop_route" || key.compare(0, 6, "utils_") == 0;
}

// What the Lua stack of L says about the running code: the domain of the innermost registered wrapper (empty: no wrapper), and, when
// asked for (with_script), the script that made the engine call: the innermost Lua function that is no helper's, else the innermost
// one ("?" when there is none or when not asked for). Without the script the walk reads no frame's source.
struct StackFacts
{
	std::string domain;
	std::string script = "?";
};

StackFacts stack_facts(lua_State* L, bool with_script)
{
	StackFacts out;
	if (!L)
		return out;
	const int top = lua_gettop(L);
	const bool have_table = push_scopes(L);
	if (!have_table && !with_script)
		return out;
	const int base = lua_gettop(L);
	bool script_found = false;
	std::string innermost;
	for (int level = 0; level < kScopeDepth; ++level)
	{
		lua_Debug ar;
		if (!lua_getstack(L, level, &ar))
			break;
		if (!lua_getinfo(L, with_script ? "Sf" : "f", &ar)) // pushes the function of that frame
			continue;
		if (with_script && !script_found && ar.what && xr_strcmp(ar.what, "C") && ar.source && *ar.source)
		{
			std::string key = routes::script_key(ar.source);
			if (innermost.empty())
				innermost = key;
			if (!helper_script(key))
			{
				out.script = std::move(key);
				script_found = true;
			}
		}
		bool found = false;
		if (have_table)
		{
			lua_rawget(L, base); // the function -> its domain
			if (lua_type(L, -1) == LUA_TSTRING)
			{
				size_t n = 0;
				const char* d = lua_tolstring(L, -1, &n);
				out.domain.assign(d, n);
				found = true;
			}
		}
		lua_settop(L, base);
		if (found)
			break;
	}
	lua_settop(L, top);
	if (!script_found && !innermost.empty())
		out.script = innermost;
	return out;
}

// --- the writer gate ---

const char* root_kind_name(writer::RootKind k)
{
	switch (k)
	{
	case writer::RootKind::Character: return "a character";
	case writer::RootKind::Unit: return "a unit";
	case writer::RootKind::Resident: return "a resident";
	case writer::RootKind::None: return "no map's object";
	}
	return "no map's object";
}

// The kind of a new object of the factory class of `e` (calls::new_kind_of_class: a squad or a monster is a unit).
calls::Target class_kind(CSE_Abstract* e)
{
	return calls::new_kind_of_class(smart_cast<CSE_ALifeOnlineOfflineGroup*>(e) != nullptr, smart_cast<CSE_ALifeMonsterAbstract*>(e) != nullptr);
}

// A create with no parent whose kind is read from its section: a unit as the writer rule names one. The section's `class` names one
// factory class, so the kind is the class's, kept per class (g.class_kinds). It is read from a live object of that class in this world,
// not from a scratch object: building one runs a script class's constructor (sim_squad_scripted:__init -> init_squad evaluates the
// squad's relationship condlist), and the create that follows builds the real object again. Only a class with no object in this world
// is built once as a scratch, logged. Asked only where the kind changes the writer (writer_refuses).
calls::Target section_kind(const char* section)
{
	if (!section || !*section || !pSettings->section_exist(section) || !pSettings->line_exist(section, "class"))
		return calls::Target::NewResident; // the stock create fails on it itself
	const CLASS_ID cls = pSettings->r_clsid(section, "class");
	{
		xrCriticalSectionGuard guard(g_lock);
		const auto kept = g.class_kinds.find(cls);
		if (kept != g.class_kinds.end())
			return kept->second;
	}
	calls::Target kind = calls::Target::NewResident;
	bool read = false;
	for (const auto& it : ai().alife().objects().objects())
		if (it.second && it.second->m_tClassID == cls)
		{
			kind = class_kind(it.second);
			read = true;
			break;
		}
	if (!read)
	{
		CSE_Abstract* e = F_entity_Create(section);
		if (e)
		{
			kind = class_kind(e);
			F_entity_Destroy(e);
		}
		::Msg("[coop] writer gate: the kind of [%s] (%s) was read from a scratch object: no object of its class in this world (once per "
		      "class)", section, kind == calls::Target::NewUnit ? "a unit" : "a resident");
	}
	xrCriticalSectionGuard guard(g_lock);
	g.class_kinds.emplace(cls, kind);
	return kind;
}

// The census line of a counted write (once per kind, call and script). `who`: the writer the verdict used (after new_root_writer);
// `root_writer`: the writer of the root, or of the new root's map, as Mode::writer_of said. Under g_lock.
void census_line(const char* call, writer::WriteOp op, const calls::WriteTarget& t, const writer::RootFacts& root,
    const writer::Writer& who, const writer::Writer& root_writer, writer::CodeScope scope, const std::string& script,
    calls::CensusKind kind, const calls::WriteDecision& d, calls::WriterGate setting)
{
	string256 what;
	if (t.kind == calls::Target::Object)
		xr_sprintf(what, " on object %u (root %u, %s of map %u)", u32(t.id), u32(root.root), root_kind_name(root.kind), u32(root.level));
	else
		xr_sprintf(what, " with no parent (a new root on map %u)", u32(t.level));
	string64 writer_text;
	if (who.is == writer::WriterIs::Other)
		xr_sprintf(writer_text, "peer %u", u32(who.peer));
	else
		xr_strcpy(writer_text, writer::writer_name(who.is));
	string64 map_text = "";
	if (root_writer != who && root_writer.is == writer::WriterIs::Other)
		xr_sprintf(map_text, " (the root's writer: peer %u)", u32(root_writer.peer));
	const char* outcome = kind == calls::CensusKind::RunIntoLed
	    ? "ran in every setting: a create into a map a map server leads, from code that may run there as well (a double: a route line)"
	    : d.act == calls::WriteAct::Refuse                                     ? "refused"
	    : d.act == calls::WriteAct::KeepCallGate && setting == calls::WriterGate::Log ? "counted only (coop_writer_gate log)"
	                                                                           : "kept: no engine path for an ask or an order yet";
	::Msg("[coop] writer gate: %s %s%s%s%s%s of %s: %s, verdict %s, written by %s%s, %s, from %s; %s (the rest of this call and script "
	      "only count in coop_status)",
	    call, writer::op_name(op), t.what && *t.what ? " [" : "", t.what ? t.what : "", t.what && *t.what ? "]" : "", what,
	    writer::kind_name(writer::process_kind(is_mirror(), mode().decides_world(), mode().leads_level())), calls::census_name(kind),
	    writer::verdict_name(d.verdict), writer_text, map_text, calls::scope_name(scope), script.c_str(), outcome);
}

// The console command coop_writer_gate = log | enforce (saved with the machine's settings).
class CCC_WriterGate : public IConsole_Command
{
public:
	CCC_WriterGate(LPCSTR name) : IConsole_Command(name) {}
	void Execute(LPCSTR args) override
	{
		calls::WriterGate setting = g_writer_gate.load();
		if (!calls::parse_writer_gate(args ? args : "", setting))
		{
			InvalidSyntax();
			return;
		}
		g_writer_gate.store(setting);
		::Msg("[coop] coop_writer_gate %s", calls::writer_gate_name(setting));
	}
	void Status(TStatus& S) override { xr_strcpy(S, calls::writer_gate_name(g_writer_gate.load())); }
	void Info(TInfo& I) override
	{
		xr_strcpy(I, "the writer gate of script writes: log (counts in coop_status what it would refuse, ask or order) | enforce");
	}
	void fill_tips(vecTips& tips, u32 /*mode*/) override
	{
		tips.push_back("log");
		tips.push_back("enforce");
	}
};

bool iends_with(std::string_view s, std::string_view suffix)
{
	return s.size() >= suffix.size() && ascii::iequals(s.substr(s.size() - suffix.size()), suffix);
}
} // namespace

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
			const std::string n = ascii::lowered(name);
			if (n == "coop_route.ltx")
				have_main = true;
			else if (ascii::istarts_with(n, "coop_route_") && iends_with(n, ".ltx"))
				extra.push_back(n);
			else if (ascii::istarts_with(n, "coop_compat_") && iends_with(n, ".ltx"))
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

namespace
{
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
	// the behaviour of online objects, the keys of two sections (routes::Table::level_behaviour)
	for (const auto& line : section_lines(ini, "level_scripts"))
		g.table.add_level_script(line.first);
	for (const auto& line : section_lines(ini, "level_callbacks"))
		g.table.add_level_callback(line.first);
	// [binder_host], `script_binding = method, method`
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
			const std::string fn(ascii::trim(key, ascii::space_or_eol));
			if (!fn.empty() && std::find(g.none_functions.begin(), g.none_functions.end(), fn) == g.none_functions.end())
				g.none_functions.push_back(fn);
		}
	}
	g.files.push_back(name);
}

// Under g_lock. routes::Table::rule through the cache: a handler is routed once when it is registered, and a pair seen before (one
// script's many time events) is answered from here. The cache holds only the table's answers and is cleared whenever the table is
// loaded again.
Domain route_of(std::string_view callback, const std::string& script)
{
	std::string key;
	key.reserve(callback.size() + 1 + script.size());
	key.append(callback.data(), callback.size()).append(1, '\1').append(script);
	const auto it = g.cache.find(key);
	if (it != g.cache.end())
		return it->second;
	const Domain d = g.table.rule(callback, script);
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
	return net::route_runs_here(d, m.active(), mode());
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

// The functions below never raise a Lua error on bad arguments: a wrong call from an addon gets nil / true /
// nothing, like the rest of the coop namespace.

// The script key of the Lua function at `idx` ("[C]" for a C function), "?" for anything else.
std::string script_of(lua_State* L, int idx)
{
	if (lua_type(L, idx) != LUA_TFUNCTION)
		return "?";
	lua_Debug ar;
	lua_pushvalue(L, idx);
	if (!lua_getinfo(L, ">S", &ar)) // pops the function
		return "?";
	return ar.what && !xr_strcmp(ar.what, "C") ? std::string("[C]") : routes::script_key(ar.source ? ar.source : "");
}

// coop.route.of(callback, func) and coop.route.rule_of(callback, func) -> domain, script: both answer routes::Table::rule for the same
// input.
// of (Traced) routes a handler as it is registered: the answer goes through the route cache and counts in the routed totals of
// coop_status. rule_of is a script asking its own rule: no cache entry, no count. An empty callback asks for the script alone (its key
// and its rule), which is no handler: of neither caches nor counts it either.
template <bool Traced>
int l_rule(lua_State* L)
{
	std::string_view callback;
	if (!arg_string(L, 1, callback))
	{
		lua_pushnil(L);
		return 1;
	}
	load_overrides();
	const std::string script = script_of(L, 2);
	Domain d = Domain::Owner;
	{
		xrCriticalSectionGuard guard(g_lock);
		const bool traced = Traced && !callback.empty();
		if (traced)
		{
			d = route_of(callback, script);
			++g.routed[size_t(d)];
		}
		else
			d = g.table.rule(callback, script);
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
	arg_string(L, 4, callback); // skips are kept per (callback, script)
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

// coop.route.scoped(wrapper, domain): the wrapper's code runs in that domain ("host", "owner", "view", "level", "none", or
// calls::kUnknownScope). Anything else is ignored.
int l_scoped(lua_State* L)
{
	std::string_view name;
	routes::Domain unused = routes::Domain::Owner;
	if (lua_type(L, 1) != LUA_TFUNCTION || !arg_string(L, 2, name) ||
		(name != calls::kUnknownScope && !routes::parse_domain(name, unused)))
		return 0;
	if (!push_scopes(L))
		return 0;
	lua_pushvalue(L, 1);
	lua_pushlstring(L, name.data(), name.size());
	lua_rawset(L, -3);
	lua_pop(L, 1);
	return 0;
}

// coop.route.scope() -> the domain of the innermost registered wrapper on this Lua stack, nil outside any.
int l_scope(lua_State* L)
{
	const StackFacts f = stack_facts(L, false);
	if (f.domain.empty())
		lua_pushnil(L);
	else
		lua_pushlstring(L, f.domain.data(), f.domain.size());
	return 1;
}

// coop.route.note_batch bounds: one call takes at most kNoteBatchRows rows, a row counts at most kNoteBatchMax calls. A batch is what
// one frame of wrapped handlers ran, far below both; the bound keeps the router lock to kNoteBatchRows single additions.
constexpr int kNoteBatchRows = 256;
constexpr double kNoteBatchMax = double(1u << 20);

// coop.route.note_batch({ {domain, outcome, count[, script[, callback]]}, ... }) -> rows counted. A row counts what `count` calls of
// coop.route.note(domain, outcome, script, callback) count, all rows under one lock. A row that is not of that shape (an unknown
// domain or outcome, a count that is no whole number 1 .. kNoteBatchMax) and every row past the first kNoteBatchRows are refused:
// counted and logged, not noted.
// No allocation: the rows are read into a fixed array whose names are views of the Lua strings, which the argument table holds for
// the whole call (LuaJIT does not move strings).
int l_note_batch(lua_State* L)
{
	struct Row
	{
		Domain d = Domain::Owner;
		Outcome o = Outcome::Ran;
		uint32_t count = 0;
		std::string_view script, callback;
	};
	Row rows[kNoteBatchRows];
	int noted = 0;
	int refused = 0;
	if (lua_type(L, 1) == LUA_TTABLE)
	{
		const int n = int(lua_objlen(L, 1));
		const int read = n < kNoteBatchRows ? n : kNoteBatchRows;
		refused = n - read;
		for (int i = 1; i <= read; ++i)
		{
			lua_rawgeti(L, 1, i);
			const int row = lua_gettop(L);
			bool ok = false;
			if (lua_type(L, row) == LUA_TTABLE)
			{
				for (int k = 1; k <= 5; ++k)
					lua_rawgeti(L, row, k);
				std::string_view name, outcome;
				Row& r = rows[noted];
				r = Row();
				if (arg_string(L, row + 1, name) && arg_string(L, row + 2, outcome) && routes::parse_domain(name, r.d) &&
					routes::parse_outcome(outcome, r.o) && lua_api::arg_whole_u32(L, row + 3, kNoteBatchMax, r.count) && r.count > 0)
				{
					arg_string(L, row + 4, r.script);
					arg_string(L, row + 5, r.callback);
					++noted;
					ok = true;
				}
			}
			lua_settop(L, row - 1);
			if (!ok)
				++refused;
		}
	}
	bool log = false;
	u64 refusals = 0;
	{
		xrCriticalSectionGuard guard(g_lock);
		for (int i = 0; i < noted; ++i)
		{
			const Row& r = rows[i];
			g.counters.note(r.d, r.o, r.script, r.callback, r.count);
		}
		if (refused > 0)
		{
			log = g.batch_refusals.allow();
			refusals = g.batch_refusals.count();
		}
	}
	if (log)
		::Msg("! [coop] routes: note_batch refused %d row(s) (malformed, or past %d per call) (#%llu)", refused, kNoteBatchRows,
			refusals);
	lua_pushnumber(L, lua_Number(noted));
	return 1;
}

// --- binders ---

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
	g.loaded.store(false);
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
	g.census.clear();
}

void reset()
{
	xrCriticalSectionGuard guard(g_lock);
	g.counters.clear();
	for (BinderSlot& b : g.binder_slots)
		std::fill(std::begin(b.skipped), std::end(b.skipped), u64(0));
	g.census.clear(); // a session's census, its lines again
}

void load_overrides()
{
	if (g.loaded.load(std::memory_order_acquire))
		return; // the hot path (every binder update on a mirror): no lock once read; every reader of the table takes g_lock itself
	xrCriticalSectionGuard guard(g_lock);
	if (g.loaded.load(std::memory_order_relaxed))
		return;
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
	g.loaded.store(true, std::memory_order_release);
}

void register_lua(lua_State* L)
{
	static const luaL_Reg fns[] = {
		{"of", &l_rule<true>},
		{"runs", &l_runs},
		{"note", &l_note},
		{"none_functions", &l_none_functions},
		{"scoped", &l_scoped},
		{"scope", &l_scope},
		{"rule_of", &l_rule<false>},
		{"note_batch", &l_note_batch},
		{nullptr, nullptr},
	};
	// The wrapper -> domain table of this state, weak-keyed (a wrapper goes with its handler); made once per state, so a second
	// registration keeps the marks of the wrappers made before it
	if (!push_scopes(L))
	{
		lua_pushlightuserdata(L, &g_scope_key);
		lua_newtable(L);
		lua_createtable(L, 0, 1);
		lua_pushstring(L, "k");
		lua_setfield(L, -2, "__mode");
		lua_setmetatable(L, -2);
		lua_rawset(L, LUA_REGISTRYINDEX);
	}
	else
		lua_pop(L, 1);
	lua_pushstring(L, "coop");
	lua_rawget(L, LUA_GLOBALSINDEX);
	if (!lua_istable(L, -1))
	{
		lua_pop(L, 1);
		return; // coop_script logs it
	}
	lua_createtable(L, 0, 6);
	for (const luaL_Reg* f = fns; f->name; ++f)
	{
		lua_pushcfunction(L, f->func);
		lua_setfield(L, -2, f->name);
	}
	lua_setfield(L, -2, "route");
	lua_pop(L, 1);
}

bool writer_refuses(const char* call, writer::WriteOp op, const calls::WriteTarget& target)
{
	if (!call || !mgr().active())
		return false; // single player: the stock game writes everything
	const writer::ProcessKind kind = writer::process_kind(is_mirror(), mode().decides_world(), mode().leads_level());
	if (kind == writer::ProcessKind::Whole || kind == writer::ProcessKind::Mirror)
		return false; // the co-op host writes everything; a player's machine follows the call gate's verdict
	if (!ai().get_alife())
		return false;
	const u16 own = levels::my_level();
	writer::RootFacts root{writer::RootKind::None, writer::kNoId, writer::kNoLevel};
	if (target.kind == calls::Target::Object)
	{
		if (target.id == writer::kNoId || !ai().alife().objects().object(target.id, true))
			return false; // no such object: the stock code says so itself
		root = writing::root_facts_of(target.id);
	}
	else
		root = writer::RootFacts{writer::RootKind::Resident, writer::kNoId, target.level}; // a create with no parent: its new root
	const StackFacts stack = stack_facts(ai().script_engine().lua(), true);
	const writer::CodeScope scope = stack.domain.empty() ? writer::CodeScope::Unknown : calls::scope_of_domain(stack.domain);
	const bool on_own_level = kind == writer::ProcessKind::Level && root.level != writer::kNoLevel && root.level == own;
	const calls::WriterGate setting = g_writer_gate.load();
	const writer::Writer root_writer = mode().writer_of(root);
	writer::Writer who = root_writer;
	const bool on_other_level = kind == writer::ProcessKind::Level && root.level != writer::kNoLevel && root.level != own;
	calls::WriteFacts facts{kind, who, scope, on_own_level, calls::is_ghost(root, mode().actor_is_player()), on_other_level,
	    target.kind != calls::Target::Object};
	calls::WriteDecision d = calls::decide_writer_gate(facts, op, calls::Gate::Run, setting);
	// a new root that is a unit is this process's own (new_root_writer): asked only where the resident's answer would not run, so the
	// section is read only then
	if (d.verdict != writer::GateVerdict::Run && target.kind != calls::Target::Object && target.kind != calls::Target::NewResident)
	{
		const calls::Target made = target.kind == calls::Target::NewOfSection ? section_kind(target.what) : target.kind;
		who = calls::new_root_writer(kind, made, on_own_level, root_writer);
		facts.writer = who;
		d = calls::decide_writer_gate(facts, op, calls::Gate::Run, setting);
	}
	// the census: what the gate does not run here, and the world's creates into a led map from code that may run there as well
	// (counted with the root's writer from before new_root_writer: a level-code squad on a led map is the world's own unit, yet that
	// map's server runs the same code)
	const calls::CensusNote note = calls::census_note(kind, root_writer, op, scope, d.verdict);
	if (!note.counted)
		return false;
	{
		xrCriticalSectionGuard guard(g_lock);
		if (g.census.note(note.kind, call, stack.script))
			census_line(call, op, target, root, who, root_writer, scope, stack.script, note.kind, d, setting);
	}
	return d.gate == calls::Gate::Refuse;
}

calls::WriterGate writer_gate()
{
	return g_writer_gate.load();
}

void register_console()
{
	CMD1(CCC_WriterGate, "coop_writer_gate");
}

routes::BinderAction binder_action(const char* binding, BinderMethod method)
{
	if (!binding || !*binding || !is_mirror()) // the host, single player and a map server run the stock code
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
	if (!g.loaded.load())
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
	// Per (callback, script). A handler skipped every frame tops the first list; a rare one (a use, a key
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

	// The binder rules. skipped = host-only calls a client did not make (the Lua guards of
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

	// The census of the writer gate (what the stand reads before coop_writer_gate enforce)
	const calls::WriterCensus::Totals& c = g.census.totals();
	::Msg("[coop] writer gate: coop_writer_gate %s | refuse %llu, ask %llu, order %llu, run into led %llu (%u call/script pair(s)%s)",
		calls::writer_gate_name(g_writer_gate.load()), c.refuse, c.ask, c.order, c.led, u32(g.census.keys()),
		g.census.dropped() ? ", more than the table keeps" : "");
	line.clear();
	for (const std::pair<std::string, u64>& s : g.census.top(kTopSkipped))
		line += (line.empty() ? "" : ", ") + s.first + " " + std::to_string(s.second);
	::Msg("[coop] writer gate: most counted %s", line.empty() ? "(none)" : line.c_str());
}
} // namespace router
} // namespace coop
