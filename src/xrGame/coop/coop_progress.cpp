// Ядро: без проверок режима (docs/MODES.md)
// Anomaly Coop — a player's own progress (rank, reputation, community) checked where his record is kept. The API is
// coop_progress.h, the rules xrCoop/coop_progress_rules.h.
//
// Who calls what:
//   - coop_story.cpp handle_host, Msg::Progress: host_check(from, proxy, ...) before the report is stored (g_reported), put on the
//     player's character (apply_progress) and sent on to the others (host_broadcast without the sender);
//   - coop_story.cpp progress_frame, the put-back of the stored values every second: host_check on the stored values (the
//     reputation the keeper's own fights added since, host_add_reported_reputation, becomes part of the base there; a repeat of
//     the stored values checks nothing);
//   - sync::frame -> frame(): drops the players who left; sync::print_status -> print_status(); coop_script -> register_lua().
// Both modes run the same code: the co-op host and the world server keep the record (story's handle_host runs there). The co-op
// host has the player's body and the events; the world server has the saved character only and its budget comes from the map
// servers' tallies (coop_progress_check merges them). The map servers only apply the rebroadcast (handle_client).
//
// The budget: coop_progress_check.rank_budget(character) (the points the keeper saw the player earn since its window was last
// closed), asked only when a report raises rank or reputation; after a stored rise the window is closed with
// rank_budget(character, true). The community: coop.progress.community_ok(peer, community, index, character), a function a script
// of the keeper sets (the coop_faction record, a disguise); none set: the community is not checked.
//
// coop_trust (gamedata/configs/mod_system_coop_trust.ltx, [coop_trust]: progress_rank, progress_reputation, progress_community,
// else default, else "log"): "log" keeps the values as told and writes "[coop-trust] <name> would refuse ..."; "refuse" stores the
// clamped values, puts them on the character and sends them on, and writes "[coop-trust] <name> refused ..."; "off" does not
// check. The player's own actor keeps his values in every mode: apply_progress never touches id 0 and the rebroadcast skips the
// sender (send_progress_all too).
#include "stdafx.h"
#include "pch_script.h"
#include "coop_progress.h"
#include "coop_manager.h"
#include "coop_join.h"
#include "xrCoop/coop_progress_rules.h"
#include "xrCoop/coop_fight_fact.h" // the bounds of a fight fact
#include "xrCoop/coop_mode.h" // only the process that decides the world applies a checked fight
#include "xrCoop/coop_lua_fn.h"
#include "coop_lua.h"

#include <cmath>

#include "Level.h"
#include "InventoryOwner.h"
#include "character_community.h"
#include "ai_space.h"
#include "alife_simulator.h"
#include "alife_object_registry.h"
#include "xrServer_Objects_ALife_Monsters.h"
#include "script_engine.h"

namespace coop
{
namespace progress
{
namespace
{
constexpr u64 kSweepMs = 1000;
constexpr u32 kLuaErrorLines = 8;

struct Counter
{
	u64 checked = 0, failed = 0, refused = 0;
};

struct State
{
	Book book;
	bool modes_read = false;
	Modes modes;
	Counter rank, reputation, community;
	u64 reports = 0, repeats = 0, baselines = 0, no_record = 0, no_world = 0, no_budget = 0, commits = 0;
	u64 worlds = 0; // the book forgot an old world (Book::on_world: a load or a level change)
	u64 community_unchecked = 0, lua_errors = 0;
	u64 fights_applied = 0, fights_refused = 0, fights_unhandled = 0, fights_apply_refused = 0; // map servers' fight facts
	u64 last_sweep = 0;
} g;

const char* cs(const shared_str& s) { return s.c_str() ? s.c_str() : ""; }

// ------------------------------------------------------------------ coop_trust settings

Trust read_trust(LPCSTR name)
{
	Trust t = Trust::Log;
	if (!pSettings || !pSettings->section_exist(kTrustSection))
		return t;
	if (pSettings->line_exist(kTrustSection, kTrustDefault))
		trust_of(pSettings->r_string(kTrustSection, kTrustDefault), t);
	if (pSettings->line_exist(kTrustSection, name))
		trust_of(pSettings->r_string(kTrustSection, name), t);
	return t;
}

// Read once per process, as coop_trust.script reads them once per Lua state.
const Modes& modes()
{
	if (!g.modes_read)
	{
		g.modes_read = true;
		g.modes.rank = read_trust(kTrustRank);
		g.modes.reputation = read_trust(kTrustReputation);
		g.modes.community = read_trust(kTrustCommunity);
	}
	return g.modes;
}

// ------------------------------------------------------------------ the character as the keeper has him

bool community_valid(int c) { return c >= 0 && c <= int(CHARACTER_COMMUNITY::GetMaxIndex()); }

shared_str community_id(int c)
{
	return community_valid(c) ? CHARACTER_COMMUNITY::IndexToId(CHARACTER_COMMUNITY_INDEX(c), shared_str(), true) : shared_str();
}

// The record: the online object (the co-op host has the player's body), else the alife object (the world server, or a character
// that is offline). The keeper's own changes are on it: the put-back writes the stored values, its fights add reputation.
bool record_of(u16 id, Values& out)
{
	if (CInventoryOwner* io = smart_cast<CInventoryOwner*>(Level().Objects.net_Find(id)))
	{
		out.rank = io->Rank();
		out.reputation = io->Reputation();
		out.community = io->Community();
		return true;
	}
	CSE_ALifeTraderAbstract* tr = smart_cast<CSE_ALifeTraderAbstract*>(ai().alife().objects().object(id, true));
	if (!tr)
		return false;
	out.rank = tr->m_rank;
	out.reputation = tr->m_reputation;
	out.community = tr->m_community_index;
	return true;
}

// ------------------------------------------------------------------ Lua: the ledger and the community answer

void lua_error_line(LPCSTR what, LPCSTR text)
{
	if (++g.lua_errors <= kLuaErrorLines)
		::Msg("! [coop] progress: %s failed: %s", what, text ? text : "?");
}

// coop_progress_check.rank_budget(character [, true]) -> rank points, reputation points. False: no such module or function, an error
// (logged), or not two numbers.
bool lua_window(u16 character, bool commit, Budget& out)
{
	bool known = false;
	std::string error;
	const luafn::Fail f = luafn::call_named(ai().script_engine().lua(), "coop_progress_check", "rank_budget", nullptr,
	    [&](lua_State* L)
	    {
		    lua_pushinteger(L, int(character));
		    lua_pushboolean(L, commit ? 1 : 0);
		    return 2;
	    },
	    2,
	    [&](lua_State* L)
	    {
		    if (lua_type(L, -2) != LUA_TNUMBER || lua_type(L, -1) != LUA_TNUMBER)
			    return;
		    out.rank = points_of(double(lua_tonumber(L, -2)));
		    out.reputation = points_of(double(lua_tonumber(L, -1)));
		    known = true;
	    },
	    error);
	if (f == luafn::Fail::LuaError)
		lua_error_line("rank_budget", error.c_str());
	return known;
}

// coop.progress.community_ok(peer, community, index, character) -> true | false; 'unknown': no such function or no answer.
constexpr LPCSTR kLuaCommunity = "local peer, name, index, c = ...\n"
								 "local ok, f = pcall(function() local p = coop and coop.progress; return type(p) == 'table' and p.community_ok or nil end)\n"
								 "if not ok or type(f) ~= 'function' then return 'none' end\n"
								 "local good, v = pcall(f, peer, name, index, c)\n"
								 "if not good then return 'error', tostring(v) end\n"
								 "if v == true then return 'yes' end\n"
								 "if v == false then return 'no' end\n"
								 "return 'unknown'\n";

Faction lua_community_ok(PeerId from, u16 character, int index, const shared_str& name)
{
	lua_State* L = ai().script_engine().lua();
	if (!L)
		return Faction::Unknown;
	const int top = lua_gettop(L);
	Faction f = Faction::Unknown;
	if (luaL_loadbuffer(L, kLuaCommunity, xr_strlen(kLuaCommunity), "@coop_progress_community") == 0)
	{
		lua_pushnumber(L, lua_Number(from));
		lua_pushstring(L, cs(name));
		lua_pushinteger(L, index);
		lua_pushinteger(L, int(character));
		if (lua_pcall(L, 4, 2, 0))
			lua_error_line("community_ok", lua_tostring(L, -1));
		else if (lua_type(L, -2) == LUA_TSTRING)
		{
			LPCSTR a = lua_tostring(L, -2);
			if (!xr_strcmp(a, "yes"))
				f = Faction::Allowed;
			else if (!xr_strcmp(a, "no"))
				f = Faction::NotAllowed;
			else if (!xr_strcmp(a, "error"))
				lua_error_line("community_ok", lua_tostring(L, -1));
			else if (!xr_strcmp(a, "none"))
				++g.community_unchecked;
		}
	}
	else
		lua_error_line("the community_ok chunk", lua_tostring(L, -1));
	lua_settop(L, top);
	return f;
}

Faction faction_answer(PeerId from, u16 character, int base, int told, const Report& r)
{
	const shared_str t = community_id(told);
	if (same_faction(cs(community_id(base)), cs(t)) ||
	    (r.record_known && same_faction(cs(community_id(r.record.community)), cs(t))))
		return Faction::Same; // the NPC record says X, the player's character actor_X (coop_join.cpp role swap)
	return lua_community_ok(from, character, told, t);
}

// ------------------------------------------------------------------ counters and lines

void count(Counter& c, uint8_t bit, const Outcome& o)
{
	if (o.checked & bit)
		++c.checked;
	if (o.failed & bit)
		++c.failed;
	if (o.refused & bit)
		++c.refused;
}

// Only fixed text and numbers reach the console: the names are this file's constants, the communities the engine's table.
void say_points(const Counter& c, LPCSTR name, LPCSTR what, Trust mode, PeerId from, u16 character, s64 told, s64 base,
    s64 budget, s64 kept, bool baseline)
{
	if (!log_due(c.failed))
		return;
	LPCSTR join = baseline ? " (the first report after a join, from the saved record)" : "";
	if (mode == Trust::Refuse)
		::Msg("[coop-trust] %s refused: peer %u, character %u: %s %lld -> %lld (+%lld), budget %lld%s: kept %lld (%llu of %llu "
		      "failed)",
		    name, u32(from), u32(character), what, base, told, told - base, budget, join, kept, c.failed, c.checked);
	else
		::Msg("[coop-trust] %s would refuse: peer %u, character %u: %s %lld -> %lld (+%lld), budget %lld%s: let through (%llu "
		      "of %llu failed)",
		    name, u32(from), u32(character), what, base, told, told - base, budget, join, c.failed, c.checked);
}

void say_community(const Counter& c, Trust mode, PeerId from, u16 character, int base, int told)
{
	if (!log_due(c.failed))
		return;
	const shared_str b = community_id(base), t = community_id(told);
	if (mode == Trust::Refuse)
		::Msg("[coop-trust] %s refused: peer %u, character %u: community %s -> %s without the keeper's allowance: kept %s (%llu "
		      "of %llu failed)",
		    kTrustCommunity, u32(from), u32(character), cs(b), cs(t), cs(b), c.failed, c.checked);
	else
		::Msg("[coop-trust] %s would refuse: peer %u, character %u: community %s -> %s without the keeper's allowance: let "
		      "through (%llu of %llu failed)",
		    kTrustCommunity, u32(from), u32(character), cs(b), cs(t), c.failed, c.checked);
}

// coop.progress.on_verified_fight(epoch, fact, from, character, delta) -> true | false: the keeper's script checks a map
// server's fight fact and applies it; 'none': no such function (no keeper script here).
constexpr LPCSTR kLuaFight = "local e, q, from, c, delta = ...\n"
							 "local ok, f = pcall(function() local p = coop and coop.progress; return type(p) == 'table' and p.on_verified_fight or nil end)\n"
							 "if not ok or type(f) ~= 'function' then return 'none' end\n"
							 "local good, v = pcall(f, e, q, from, c, delta)\n"
							 "if not good then return 'error', tostring(v) end\n"
							 "return v == true and 'yes' or 'no'\n";

// coop.progress.apply_verified_fight(peer, character, delta) -> true | false: the keeper's script checked a map
// server's fight fact (coop_srv_progress.script); the delta goes into the base of his record and to his machine. Only where the world is
// decided, for a whole peer whose character it is, a whole delta within a fact's bounds (xrCoop/coop_fight_fact.h).
int l_apply_verified_fight(lua_State* L)
{
	bool ok = false;
	if (mgr().active() && mode().decides_world() && lua_type(L, 1) == LUA_TNUMBER && lua_type(L, 2) == LUA_TNUMBER &&
	    lua_type(L, 3) == LUA_TNUMBER)
	{
		const double p = double(lua_tonumber(L, 1)), c = double(lua_tonumber(L, 2)), d = double(lua_tonumber(L, 3));
		const bool whole = p == std::floor(p) && c == std::floor(c) && d == std::floor(d);
		if (whole && p >= 0.0 && p < double(kInvalidPeer) && c >= 1.0 && c < double(kNoId) && d != 0.0 &&
		    d >= -double(fight::kMaxDelta) && d <= double(fight::kMaxDelta) && join::proxy_of_peer(PeerId(p)) == u16(c))
			ok = host_apply_fight(PeerId(p), u16(c), int(d));
		// The record here (the world server's alife object) never sees the fight: the keeper's credit carries it into
		// the budget of the put-back and of his next report, as the co-op host's record carries its own fights
		if (ok)
			g.book.credit_keeper(u32(p), u16(c), int64_t(d), mgr().now_ms());
	}
	++(ok ? g.fights_applied : g.fights_apply_refused);
	lua_pushboolean(L, ok ? 1 : 0);
	return 1;
}
} // namespace

void register_lua(lua_State* L)
{
	// community_ok and on_verified_fight are the keeper's script's to set; no table coop: coop_script logs it
	static const luaL_Reg kFns[] = {{"apply_verified_fight", &l_apply_verified_fight}, {nullptr, nullptr}};
	lua_api::fill(L, "progress", kFns);
}

bool host_fight_fact(PeerId from, u32 epoch, u32 fact, u16 character, int delta)
{
	lua_State* L = ai().script_engine().lua();
	if (!L || !mgr().active())
	{
		++g.fights_unhandled;
		return false;
	}
	const int top = lua_gettop(L);
	bool applied = false;
	if (luaL_loadbuffer(L, kLuaFight, xr_strlen(kLuaFight), "@coop_progress_fight") == 0)
	{
		lua_pushnumber(L, lua_Number(epoch));
		lua_pushnumber(L, lua_Number(fact));
		lua_pushnumber(L, lua_Number(from));
		lua_pushinteger(L, int(character));
		lua_pushinteger(L, delta);
		if (lua_pcall(L, 5, 2, 0))
			lua_error_line("on_verified_fight", lua_tostring(L, -1));
		else if (lua_type(L, -2) == LUA_TSTRING)
		{
			LPCSTR a = lua_tostring(L, -2);
			if (!xr_strcmp(a, "yes"))
				applied = true;
			else if (!xr_strcmp(a, "none"))
				++g.fights_unhandled;
			else if (!xr_strcmp(a, "error"))
				lua_error_line("on_verified_fight", lua_tostring(L, -1));
		}
	}
	else
		lua_error_line("the on_verified_fight chunk", lua_tostring(L, -1));
	lua_settop(L, top);
	if (!applied)
		++g.fights_refused;
	return applied;
}

bool host_check(PeerId from, u16 character, int& rank, int& reputation, int& community)
{
	if (from == kInvalidPeer || character == kNoId)
		return true;
	if (!world_up())
	{
		++g.no_world; // nothing to measure from: as told (story's handlers wait for the world, ready())
		return true;
	}
	const Modes& m = modes();
	Report r;
	r.told.rank = rank;
	r.told.reputation = reputation;
	r.told.community = community;
	r.told_community_valid = community_valid(community);
	r.record_known = record_of(character, r.record);
	auto window = [character](Budget& out) { return lua_window(character, false, out); };
	auto faction = [from, character, &r](int32_t base, int32_t told) { return faction_answer(from, character, base, told, r); };
	if (g.book.on_world(join::host_world_epoch()))
		++g.worlds; // a load or a level change: the values stored in the old world are no base here
	const Outcome o = g.book.check(u32(from), character, r, m, mgr().now_ms(), window, faction);
	++g.reports;
	rank = o.out.rank;
	reputation = o.out.reputation;
	community = o.out.community;
	if (o.repeat)
	{
		++g.repeats;
		return true;
	}
	if (o.baseline)
		++g.baselines;
	if (o.no_record)
		++g.no_record;
	if (!o.budget_known)
		++g.no_budget;
	count(g.rank, kRank, o);
	count(g.reputation, kReputation, o);
	count(g.community, kCommunity, o);
	if (o.failed & kRank)
		say_points(g.rank, kTrustRank, "rank", m.rank, from, character, r.told.rank, o.base.rank, o.budget.rank, o.out.rank,
		    o.baseline);
	if (o.failed & kReputation)
		say_points(g.reputation, kTrustReputation, "reputation", m.reputation, from, character, r.told.reputation,
		    o.base.reputation, o.budget.reputation, o.out.reputation, o.baseline);
	if (o.failed & kCommunity)
		say_community(g.community, m.community, from, character, o.base.community, r.told.community);
	if (o.commit)
	{
		++g.commits;
		Budget closed;
		lua_window(character, true, closed); // the ledger's window closes: the next rise is measured from here
	}
	return o.failed == 0;
}

void frame()
{
	if (g.book.empty())
		return;
	if (g.book.on_world(join::host_world_epoch()))
	{
		++g.worlds; // a load or a level change: the values stored in the old world are no base here
		return;
	}
	const u64 now = mgr().now_ms();
	if (now - g.last_sweep < kSweepMs)
		return;
	g.last_sweep = now;
	g.book.sweep([](u32 peer) -> u16 { return join::proxy_of_peer(PeerId(peer)); }, now);
}

void reset()
{
	g = State(); // the book, its world and the counters are one session's (the settings are read again)
}

void print_status()
{
	const Modes& m = modes();
	::Msg("[coop] progress: %s [%s]: checked %llu, failed %llu, refused %llu | %s [%s]: checked %llu, failed %llu, refused %llu "
	      "| %s [%s]: checked %llu, failed %llu, refused %llu",
	    kTrustRank, trust_word(m.rank), g.rank.checked, g.rank.failed, g.rank.refused, kTrustReputation,
	    trust_word(m.reputation), g.reputation.checked, g.reputation.failed, g.reputation.refused, kTrustCommunity,
	    trust_word(m.community), g.community.checked, g.community.failed, g.community.refused);
	::Msg("[coop] progress: players %u, checks asked %llu (reports and put-backs; the stored values again %llu, first after a "
	      "join %llu, no record %llu, no world %llu), "
	      "no budget source %llu, windows closed %llu, community not checked (no coop.progress.community_ok) %llu, Lua errors "
	      "%llu, old worlds forgotten %llu",
	    u32(g.book.size()), g.reports, g.repeats, g.baselines, g.no_record, g.no_world, g.no_budget, g.commits,
	    g.community_unchecked, g.lua_errors, g.worlds);
	::Msg("[coop] progress: fight facts of map servers applied %llu, refused %llu (no keeper script %llu), applications refused %llu",
	    g.fights_applied, g.fights_refused, g.fights_unhandled, g.fights_apply_refused);
}
} // namespace progress
} // namespace coop
