// Anomaly Coop — Lua errors survived in a co-op session. Live 2026-11 (G.A.M.M.A., hundreds of mods): a Lua error inside
// an engine->Lua call (a UI functor, a dialog precondition, a script callback, a binder method) went to luabind's pcall
// callback CScriptEngine::lua_pcall_failed -> Debug.fatal and ended the session for that player (ui_options.script:912,
// ssfx_weapons_dof.script:269, _g_patches.script:817, tasks_guide.script:447, ...). In a session, with the console variable
// coop_lua_errors 1 (the default), the error is logged and the call returns a safe value instead (xrGame/coop/coop_lua_errors.cpp,
// luabind's call proxies: functor.hpp, detail/call_member.hpp, detail/call_function.hpp, object.hpp).
//
// This file is the bookkeeping: the place of an error is the file:line of the first Lua frame of the erroring stack. The first
// error of a place is logged in full (the error text, the Lua stack, one "survived" line); later ones only count, and one
// summary line per place comes at most once per kRepeatMs. At most kMaxPlaces places are kept apart; the rest share one.
// Pure C++17, header-only, no engine dependencies: tests/test_lua_errors.cpp tests it.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

namespace coop
{
namespace lua_errors
{
constexpr uint64_t kRepeatMs = 60000; // one summary line per place per minute at most
constexpr size_t kMaxPlaces = 200; // places kept apart; any further one goes to kOtherPlaces
constexpr size_t kMaxMessage = 400; // characters of the error text in a "survived" line
constexpr size_t kMaxStackLines = 48; // Lua frames logged with the first error of a place
constexpr const char* kOtherPlaces = "(other places)";
constexpr const char* kNoLuaFrame = "(no Lua frame)"; // the engine called a value that is not a function, say

// Survive the error instead of the stock fatal: only in a co-op session (host, client, map server) with the switch on.
inline bool survive(bool session_active, int coop_lua_errors) { return session_active && coop_lua_errors != 0; }

// The place of a Lua frame: "...\\gamedata\\scripts\\ui_options.script", 912 -> "ui_options.script:912".
inline std::string place_key(const char* src, int line)
{
	if (!src)
		return kNoLuaFrame;
	const char* name = src;
	for (const char* p = src; *p; ++p)
		if (*p == '\\' || *p == '/')
			name = p + 1;
	if (!*name)
		return kNoLuaFrame;
	return std::string(name) + ":" + std::to_string(line);
}

// The error text on one line ("\n" -> " | "), at most kMaxMessage characters (then "...").
inline std::string clip(const char* message)
{
	if (!message)
		return "(not a string)";
	std::string out;
	for (const char* p = message; *p; ++p)
	{
		if (*p == '\r')
			continue;
		if (*p == '\n')
			out += " | ";
		else
			out += *p;
		if (out.size() > kMaxMessage)
			break;
	}
	if (out.size() > kMaxMessage)
	{
		out.resize(kMaxMessage);
		out += "...";
	}
	return out;
}

enum class Log
{
	Full, // the first error of its place: the error text, the stack, the "survived" line
	Count, // only counted
	Summary // a "survived" line with the count since the last line of the place
};

struct Note
{
	Log log = Log::Count;
	uint64_t number = 0; // this error's number among all survived ones (#N)
	uint64_t since_line = 0; // Summary: the errors of the place since its last line, this one included
	uint64_t at_place = 0; // the errors of the place in all, this one included
	uint64_t elapsed_ms = 0; // Summary: the time since the last line of the place
	std::string place; // the place it was booked at (kOtherPlaces past the cap)
};

struct Top
{
	std::string place;
	uint64_t count = 0;
};

class Ledger
{
public:
	Note note(const std::string& place, uint64_t now_ms)
	{
		Note n;
		n.number = ++m_total;
		n.place = place;
		auto it = m_places.find(place);
		if (it == m_places.end())
		{
			if (m_places.size() >= kMaxPlaces)
			{
				n.place = kOtherPlaces;
				it = m_places.find(n.place);
			}
			if (it == m_places.end())
				it = m_places.emplace(n.place, Place()).first;
		}
		Place& p = it->second;
		n.at_place = ++p.count;
		if (p.count == 1)
		{
			n.log = Log::Full;
			p.last_line_ms = now_ms;
			p.since_line = 0;
			return n;
		}
		++p.since_line;
		const uint64_t elapsed = now_ms >= p.last_line_ms ? now_ms - p.last_line_ms : 0;
		if (elapsed < kRepeatMs)
		{
			n.log = Log::Count;
			return n;
		}
		n.log = Log::Summary;
		n.since_line = p.since_line;
		n.elapsed_ms = elapsed;
		p.since_line = 0;
		p.last_line_ms = now_ms;
		return n;
	}

	uint64_t total() const { return m_total; }
	size_t places() const { return m_places.size(); }

	// The places with the most errors first (a tie: by name).
	std::vector<Top> top(size_t n) const
	{
		std::vector<Top> all;
		all.reserve(m_places.size());
		for (const auto& it : m_places)
			all.push_back({it.first, it.second.count});
		std::stable_sort(all.begin(), all.end(), [](const Top& a, const Top& b) { return a.count > b.count; });
		if (all.size() > n)
			all.resize(n);
		return all;
	}

private:
	struct Place
	{
		uint64_t count = 0;
		uint64_t last_line_ms = 0;
		uint64_t since_line = 0;
	};
	std::map<std::string, Place> m_places;
	uint64_t m_total = 0;
};

// Full:    "! [coop] Lua error survived (#12, ui_options.script:912): <error text>"
// Summary: "! [coop] Lua error survived (#57, ui_options.script:912): 43 more in 60 s, 44 at this place: <error text>"
inline std::string survived_line(const Note& n, const char* message)
{
	std::string s = "! [coop] Lua error survived (#" + std::to_string(n.number) + ", " + n.place + "): ";
	if (n.log == Log::Summary)
		s += std::to_string(n.since_line) + " more in " + std::to_string(n.elapsed_ms / 1000) + " s, " +
		    std::to_string(n.at_place) + " at this place: ";
	return s + clip(message);
}

// A frame of the Lua stack as the engine's script log writes it with an error ("! [LUA] " + the frame).
inline std::string stack_line(const std::string& frame) { return "! [LUA] " + frame; }

// coop_status: "[coop] lua errors: coop_lua_errors 1 (survive, in a session now), survived 5 at 2 place(s)"
inline std::string status_line(const Ledger& l, int coop_lua_errors, bool session_active)
{
	std::string mode;
	if (!coop_lua_errors)
		mode = "stock: fatal";
	else if (session_active)
		mode = "survive, in a session now";
	else
		mode = "survive, not in a session now: fatal";
	return "[coop] lua errors: coop_lua_errors " + std::to_string(coop_lua_errors) + " (" + mode + "), survived " +
	    std::to_string(l.total()) + " at " + std::to_string(l.places()) + " place(s)";
}

// "[coop]   lua errors x4 at b.script:2"
inline std::string status_place_line(const Top& t)
{
	return "[coop]   lua errors x" + std::to_string(t.count) + " at " + t.place;
}
} // namespace lua_errors
} // namespace coop
