// Anomaly Coop — call router gate (F2 T12, MOD_LAYER.md §2.3). See coop_callgate.h and xrCoop/coop_calls.h.
#include "stdafx.h"
#include "coop_callgate.h"
#include "coop_manager.h"
#include "coop_events.h"
#include "coop_entities.h" // client_applying
#include "coop_requests.h" // lua_client
#include "coop_story.h" // applying
#include "xrCoop/coop_validate.h"
#include "xrServer_Objects_ALife_Monsters.h"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace coop
{
namespace callgate
{
namespace
{
using calls::CallClass;

constexpr u16 kNoId = 0xffff;
constexpr u16 kActor = 0; // the local actor on every machine (the join swap keeps it at 0)
constexpr u32 kErrorsLogged = 20; // ltx errors written to the log one by one, the rest only counted
constexpr size_t kTopRefused = 10;
constexpr const char* kSection = "call_classes";

struct State
{
	bool loaded = false;
	calls::Classes classes;
	std::vector<std::string> files; // read, in order
	u32 rules = 0;
	u32 unknown = 0; // overrides of names kCalls does not have (a modded exe's exports, or a typo)
	u32 errors = 0;
	calls::Counters counters;
};

State g;
xrCriticalSection g_lock;

bool iends_with(std::string_view s, std::string_view suffix)
{
	return s.size() >= suffix.size() && calls::detail::iequals(s.substr(s.size() - suffix.size()), suffix);
}

// coop_route.ltx first, then coop_route_*.ltx, then coop_compat_*.ltx, each group by name: the router's files and
// order (coop_router.cpp route_files), so one file can hold both a mod's callback routes and its call classes.
std::vector<std::string> override_files()
{
	std::vector<std::string> extra, compat;
	bool have_main = false;
	xr_vector<LPSTR>* list = FS.file_list_open("$game_config$", FS_ListFiles | FS_RootOnly);
	if (list)
	{
		for (LPSTR name : *list)
		{
			if (!name)
				continue;
			const std::string n = calls::detail::lowered(name);
			if (n == "coop_route.ltx")
				have_main = true;
			else if (calls::detail::istarts_with(n, "coop_route_") && iends_with(n, ".ltx"))
				extra.push_back(n);
			else if (calls::detail::istarts_with(n, "coop_compat_") && iends_with(n, ".ltx"))
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

// One file's [call_classes] into the table. Under g_lock.
void read_file(const std::string& name)
{
	string_path path;
	FS.update_path(path, "$game_config$", name.c_str());
	CInifile ini(path, TRUE, TRUE, FALSE); // DLTX: mod_coop_route_*.ltx add to coop_route.ltx
	if (!ini.section_exist(kSection))
		return;
	std::vector<std::pair<std::string, std::string>> lines;
	const u32 n = ini.line_count(kSection);
	for (u32 i = 0; i < n; ++i)
	{
		LPCSTR key = nullptr;
		LPCSTR value = nullptr;
		if (ini.r_line(kSection, int(i), &key, &value) && key)
			lines.emplace_back(key, value ? value : "");
	}
	std::vector<std::string> errors;
	for (const calls::Override& o : calls::parse_overrides(lines, &errors))
	{
		const CallClass before = g.classes.class_of(o.name);
		const bool known = calls::has_row(o.name);
		g.classes.add(o);
		++g.rules;
		if (!known)
			++g.unknown;
		// visible: an override that moves a function changes what clients do with it
		::Msg("[coop] calls: configs\\%s: %s %s -> %s%s", name.c_str(), o.name.c_str(), calls::class_name(before),
			calls::class_name(o.cls), known ? "" : " (not an export of this engine)");
	}
	for (const std::string& e : errors)
		if (g.errors++ < kErrorsLogged)
			::Msg("! [coop] configs\\%s: %s", name.c_str(), e.c_str());
	g.files.push_back(name);
}

// Counts the decision and logs a refusal while the name's budget lasts (the first kLoggedPerName of each name, review
// M7). Under g_lock.
void note(const char* name, Gate gate, const char* why, u16 target, const char* what)
{
	g.counters.note(name, gate);
	if (gate != Gate::Refuse || !g.counters.take_log_line(name))
		return;
	string64 on;
	if (target != kNoId)
		xr_sprintf(on, " on object %u", target);
	else
		on[0] = 0;
	const bool last = g.counters.logged(name) >= calls::Counters::kLoggedPerName;
	::Msg("[coop] calls: %s%s%s%s%s refused on a client: %s%s", name, what && *what ? " [" : "", what ? what : "",
		what && *what ? "]" : "", on, why ? why : "refused", last ? " (its last line; the rest only count in coop_status)" : "");
}

const char* why_refused(CallClass cls, bool own)
{
	if (cls == CallClass::Owner && !own)
		return "another's object (owner call); the host's world is only mirrored here";
	return "a world call with no request path; the host's world is only mirrored here";
}

// own: decided by the caller; target: for the log only (0xffff = none). The decision itself is calls::decide.
Gate decide(const char* name, bool own, u16 target, const Args& args)
{
	// Review M5: one condition for every gate, the alife() ones' before: a client in the host's world (LoadingWorld,
	// Live). The host, single player and a client whose own world still runs while the host's downloads run everything.
	if (!name || !requests::lua_client())
		return Gate::Run;
	load_overrides();
	// P8 on the client side too: a bad number is not run here nor sent to the host
	string128 bad;
	bad[0] = 0;
	if (args.pos && !validate::vec_finite(*args.pos))
		xr_strcpy(bad, "the position is not a number");
	else if (args.id != kNoId && !events::find_entity(args.id))
		xr_sprintf(bad, "object %u is not in this world", args.id);
	// a host command being applied (an engine apply scope, the host's story) runs: it is the host's change
	const bool applying = args.runs_while_applying && (entities::client_applying() || story::applying());
	xrCriticalSectionGuard guard(g_lock);
	const CallClass cls = g.classes.class_of(name);
	const Gate gate = calls::decide(cls, true, applying, own, calls::has_request_path(name, own), !bad[0]);
	note(name, gate, gate != Gate::Refuse ? nullptr : (bad[0] ? bad : why_refused(cls, own)), target, args.what);
	return gate;
}
} // namespace

bool is_own(u16 id) { return id != kNoId && (id == kActor || events::root_of(id) == kActor); }

bool own_destroy(u16 id)
{
	CSE_Abstract* e = events::find_entity(id);
	const bool free_item = e && e->ID_Parent == kNoId && !smart_cast<CSE_ALifeCreatureAbstract*>(e);
	return calls::own_destroy(e != nullptr, is_own(id), free_item);
}

Gate client_gate(const char* name, CObject* target, const Args& args)
{
	if (!requests::lua_client())
		return Gate::Run;
	const u16 id = target ? target->ID() : kNoId;
	return decide(name, !target || is_own(id), id, args);
}

Gate client_gate_id(const char* name, u16 target, const Args& args)
{
	if (!requests::lua_client())
		return Gate::Run;
	return decide(name, target == kNoId || is_own(target), target, args);
}

Gate client_gate_own(const char* name, bool own_target, const Args& args)
{
	if (!requests::lua_client())
		return Gate::Run;
	return decide(name, own_target, kNoId, args);
}

void note_refused(const char* name, const char* why)
{
	if (!name || !requests::lua_client())
		return;
	xrCriticalSectionGuard guard(g_lock);
	note(name, Gate::Refuse, why ? why : "refused by its request path", kNoId, nullptr);
}

void install()
{
	xrCriticalSectionGuard guard(g_lock);
	g.loaded = false;
	g.classes.clear();
	g.files.clear();
	g.rules = 0;
	g.unknown = 0;
	g.errors = 0;
	g.counters.clear();
}

void reset()
{
	xrCriticalSectionGuard guard(g_lock);
	g.counters.clear();
}

void load_overrides()
{
	xrCriticalSectionGuard guard(g_lock);
	if (g.loaded)
		return;
	g.loaded = true;
	for (const std::string& name : override_files())
		read_file(name);
	::Msg("[coop] call classes: %u export(s) in the table, %u override(s) from %u file(s) (%u for unknown names), "
		  "%u ltx error(s)",
		u32(calls::call_count()), g.rules, u32(g.files.size()), g.unknown, g.errors);
}

void print_status()
{
	xrCriticalSectionGuard guard(g_lock);
	const calls::Counters::Totals& t = g.counters.totals();
	::Msg("[coop] calls: refused %llu, requested %llu, ran %llu (gated Lua calls on this client; a host runs them all) "
		  "| %u override(s), %u ltx error(s)%s",
		t.refused, t.requested, t.ran, g.rules, g.errors, g.loaded ? "" : ", call_classes not read yet");
	const std::vector<std::pair<std::string, u64>> top = g.counters.top_refused(kTopRefused);
	std::string line;
	for (const std::pair<std::string, u64>& s : top)
		line += (line.empty() ? "" : ", ") + s.first + " " + std::to_string(s.second);
	::Msg("[coop] calls: most refused %s%s", line.empty() ? "(none)" : line.c_str(),
		g.counters.names_dropped() ? " (more names than the table keeps)" : "");
}
} // namespace callgate
} // namespace coop
