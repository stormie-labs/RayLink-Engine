// Anomaly Coop — the waiting point of a level change (owner 2026-10-23 20:08: "a place where you wait for your allies to
// go to the next level, with the name of the level, and nicer; when we get to split levels it goes away").
//
// Until the split levels of F3a every player is on the host's level and the whole group changes level together. The
// stock exit zone is where the group meets for that: a player who stands in the zone of an exit is READY for it (his
// "claim"); when every player who is in the world is ready for the same exit the group goes, by the path
// coop_accept_transition always took. A player may step back out of the zone and so withdraw. Any level change of the
// host's own (the stock zone, the "go?" dialog, jump_to_level of a guide) goes through the same board while somebody else
// is in the session (coop_join.cpp host_gate_level_change).
//
// The live test of 2026-10-24 (owner: "we wait for the second player as long as it takes while we stand in the transition
// radius; the screen darkens only when we actually start moving; holding a key for a quick transition is fine, but only for
// those who are NOT standing in the transition zone"): no countdown, no "go now" key of the host, the darkening at the go
// only (every moving player's, the host player's too: the board's Going state, coop_hud_transition.script), a key held by a
// player who is not at the exit = he comes along ("consent"), and the stock route exits (sr_teleport restrictors whose drop
// point lies in a level changer) are that changer's exit while a player stands in them (no fade, no teleport).
//
// The rule (docs in the report, tests/test_transition_wait.cpp):
//  - who must be ready: every player in the world and alive. A downed, dead or spectating player does not block (he cannot
//    walk to the exit; he goes with the group). A player who is not in the world yet (loading, choosing a character) holds
//    the group, but at most kLoadWaitMs, counted from the moment everybody else is ready (a clock that stops and starts
//    over whenever somebody is not ready), then the group goes without him (he joins the new world after its SessionReset
//    like everybody);
//  - the exit: players ready for different exits do not add up; the exit whose claim is the oldest leads, the others wait
//    "at another exit";
//  - the go: on the tick nobody is missing, however long that took (State::Going; the host's own load follows kGoFadeMs
//    later while every moving player's screen darkens, coop_join.cpp host_board_step); the chat's /go (force) goes at once
//    through the exit somebody waits at, whoever is missing;
//  - three kinds of claim, the "place" the host measures him against: at the exit (the changer's own zone: the changer), in
//    a route restrictor that leads into it (the restrictor: kRoutePlace | id), or "go with the group" (kNoPlace: no
//    distance at all, only to an exit somebody else stands at - zone or route -, and it ends with the last of them: Board::
//    anchored). A claim of the same exit another way replaces the old one and keeps its age;
//  - a client's claim is kept alive by his refresh every kClaimRefreshMs and lapses kClaimLapseMs after the last one; the
//    host also drops it when the host's own view of the player is further than kLeaveM from its place (he walked away and
//    the cancel got lost); a claim at a place needs him within kReachM of it when he makes it (coop_join.cpp gives the
//    distances, it needs the world);
//  - a guide trip (kGuideGoal: no exit zone, the host's script asked for the move) has the host as its first claim and
//    the others confirm it; it lapses after kGuideLifeMs without everybody's yes.
// Wire (protocol 11): TransitionVote = u8 op + u16 level changer (host numbering; kNoChanger = no exit, a "ready" without
// one confirms the guide trip) + for VoteRoute a u16 route restrictor; TransitionState (host -> all) = StateMsg below, at most
// every kStateEveryMs while the board is not idle, plus once when it goes idle.
//
// F3a T8 (docs/plans/2026-10-24-f3a-split-levels.md §1.2, §1.5, §1.10):
//  - the host player is a client of his own world server over the local link: he is peer 0 of the server's players(), a
//    member like any other, and his claim comes as a TransitionVote that goes through the same rule as a remote client's
//    (apply_vote); his placard reads the TransitionState of the link;
//  - the together entry of split levels: a member who stands on the level the exit leads to is not waited for (on_target),
//    one on another map says yes from there with the exit's id and no distance (other_map), and his row says so;
//  - the simultaneous load of a group transition: on "go" the world server writes the shared save of the transition, gives
//    it the next world epoch and sends every moving client LoadWait {Prepared} and the save BEFORE its own reload; all load
//    at once, each loading screen stays (LoadScreen) until the server says Enter (PreparedLoad over levels::LoadBarrier):
//    everybody is ready, or kLoadWaitMs after the server's world came up for those who are; a laggard enters alone.
// Pure C++17, header-only, no engine dependencies: used by the engine and by the unit-test exe.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "coop_load_barrier.h" // F3a T3: levels::LoadBarrier, kLoadWaitMs
#include "coop_protocol.h" // Writer, Reader, Msg, kMaxPlayers

namespace coop
{
namespace transition
{
constexpr uint16_t kNoChanger = 0xffff;
constexpr uint32_t kGuideGoal = 0x10000; // one guide trip at a time (a goal is a level changer id, or this)
constexpr uint32_t kLoadWaitMs = 60000; // a player who is still loading holds the group this long at most
constexpr uint32_t kGuideLifeMs = 60000; // a guide trip lives this long without everybody's yes
constexpr uint32_t kClaimLapseMs = 20000; // a client's claim lapses this long after his last refresh
constexpr uint32_t kClaimRefreshMs = 5000; // a client says "still ready" this often
constexpr float kReachM = 5.f; // to claim an exit: within this of its nearest shape (the host's view of the player)
constexpr float kLeaveM = 9.f; // the host drops a claim of a player further than this (he left, the cancel got lost)
constexpr uint32_t kIntentSettleMs = 1200; // a client waits this long for the host's board to show what it just sent
constexpr uint32_t kStateEveryMs = 500; // the host's TransitionState while the board is not idle
constexpr uint16_t kUnknownDist = 0xffff;
constexpr size_t kMaxLevelName = 64; // a stock level name is "l01_escape"
// 2026-10-24: the go. The board's Going state reaches every player (the host player over the local link) and his screen
// darkens (sleep_fade.ppe, coop_hud_transition.script); the host's own level change starts this long after it, so that the
// darkening is on the screen before the host's process stops drawing while it writes the transition's save.
constexpr uint32_t kGoFadeMs = 1000;
// A route of sr_teleport_sections.ltx whose drop point is within this of a level changer's shapes drops its traveller into
// that changer (the stock teleport puts the actor there and his body, ~1 m around the point, touches the zone).
constexpr float kRouteDropM = 2.f;
constexpr size_t kMaxRoutes = 64; // route restrictors a machine remembers per world (a level has a handful)
// What the host measures a claim against (its "place"): the exit's own goal (the changer's zone), a route restrictor
// (kRoutePlace | its id), or nothing (kNoPlace: "go with the group" from afar). A guide trip's claims are never measured.
constexpr uint32_t kNoPlace = 0xffffffffu;
constexpr uint32_t kRoutePlace = 0x20000u;

inline uint32_t goal_of_exit(uint16_t changer) { return changer; }
inline bool is_guide(uint32_t goal) { return goal == kGuideGoal; }
inline uint32_t place_of_route(uint16_t restrictor) { return kRoutePlace | restrictor; }
inline bool is_route_place(uint32_t place) { return place != kNoPlace && (place & kRoutePlace) != 0; }
inline uint16_t route_of_place(uint32_t place) { return uint16_t(place & 0xffffu); }

enum class State : uint8_t
{
	Idle = 0, // nobody waits
	Waiting = 1, // somebody is ready, somebody is missing
	Going = 2, // nobody is missing (or the chat's /go): the group goes, the screens darken (was the countdown's value)
};

enum class Mark : uint8_t
{
	Ready = 0, // at the exit that leads
	Away = 1, // in the world, not ready
	Loading = 2, // not in the world yet (loading, choosing a character)
	Down = 3, // wounded / dead / spectating: does not block
	Elsewhere = 4, // ready for another exit
};

// A player as the host sees him this tick (one per entry of the world server's players(): the host player is peer 0).
struct Member
{
	uint32_t id = 0; // peer id, the host player is 0
	bool in_world = true; // connected and has finished loading (join::host_player_in_world)
	bool alive = true; // survival::Life::Alive
	bool remote = true; // a client: his claim lapses without a refresh (the host player's, over the local link, never does)
	bool on_target = false; // F3a §1.10: he stands on the level the exit leads to: neither waited for nor listed
	bool other_map = false; // F3a §1.10: he is on another map: his yes is a remote one (no distance), his row says so
};

// The high bits of a row's mark byte on the wire: the row's player is on another map (F3a), he comes along from afar
// ("go with the group", 2026-10-24). The layout stays 7 bytes a row.
constexpr uint8_t kRowOtherMap = 0x80;
constexpr uint8_t kRowConsent = 0x40;

struct Row
{
	uint32_t id = 0;
	Mark mark = Mark::Away;
	uint16_t dist_m = kUnknownDist; // metres to the leading exit, as the host sees him
	bool other_map = false; // F3a: on another map (Member::other_map)
	bool consent = false; // ready from afar: he comes along (a claim without a place)
};

struct Status
{
	State state = State::Idle;
	uint32_t goal = 0; // the leading exit (a level changer id or kGuideGoal)
	uint32_t ready = 0, total = 0; // total: who counts (everybody but the downed, and the loaders after their minute)
	uint32_t load_ms = 0; // left of the loaders' minute, Waiting for loaders only
	bool go = false; // the group goes now (the claims are gone with it; the state is Going)
	bool expired = false; // a guide trip timed out by this tick
	std::vector<Row> rows; // by player id; empty when Idle
};

// Metres from player `id` to `where`, or < 0 if unknown (the host's view, coop_join.cpp). `where` is a goal (an exit: the
// distance to its changer's shapes; a row's metres) or a claim's place (also kRoutePlace | a route restrictor's id).
using DistFn = std::function<float(uint32_t id, uint32_t where)>;

class Board
{
public:
	// `id` stands at the exit `goal` (its changer's own zone). The same claim again is a refresh (it keeps its age); another
	// goal replaces the old one; the same goal another way (a route, a consent) becomes this one and keeps its age.
	void claim(uint32_t id, uint32_t goal, uint64_t now) { put(id, goal, goal, now); }
	// `id` stands in route restrictor `route`, whose drop point lies in the changer of `goal`: measured against the restrictor.
	void claim_route(uint32_t id, uint32_t goal, uint16_t route, uint64_t now) { put(id, goal, place_of_route(route), now); }
	// `id` comes along to `goal` from wherever he is ("go with the group"): no distance; it ends when nobody stands at that
	// exit any more (tick). apply_vote takes it only for an exit somebody else stands at (anchored).
	void consent(uint32_t id, uint32_t goal, uint64_t now) { put(id, goal, kNoPlace, now); }
	// His claim ends. False if he had none.
	bool release(uint32_t id) { return m_claims.erase(id) != 0; }
	bool claimed(uint32_t id, uint32_t* goal = nullptr, uint32_t* place = nullptr) const
	{
		const auto it = m_claims.find(id);
		if (it == m_claims.end())
			return false;
		if (goal)
			*goal = it->second.goal;
		if (place)
			*place = it->second.place;
		return true;
	}
	// Somebody but `except` stands at exit `goal` (its zone or a route into it), or answers for it from another map: he is
	// what "go with the group" comes along to. A consent is nobody's anchor.
	bool anchored(uint32_t goal, uint32_t except) const
	{
		for (const auto& c : m_claims)
			if (c.first != except && c.second.goal == goal && c.second.place != kNoPlace)
				return true;
		return false;
	}
	void forget(uint32_t id) { m_claims.erase(id); }
	// A guide trip: the host `id` is the first yes; the others confirm with claim(id, kGuideGoal). Already running: kept.
	void start_guide(uint32_t id, uint64_t now)
	{
		if (!m_guide)
		{
			m_guide = true;
			m_guide_since = now;
		}
		claim(id, kGuideGoal, now);
	}
	// The chat's /go (coop_accept_transition) at exit `goal`: the next tick lets the group go at once if somebody claimed
	// that exit, whoever is missing.
	void force(uint32_t goal)
	{
		m_forced = true;
		m_forced_goal = goal;
	}
	// A new world: nothing of the old one counts.
	void clear()
	{
		m_claims.clear();
		m_guide = false;
		m_forced = false;
		m_loading = false;
	}
	bool empty() const { return m_claims.empty(); }

	Status tick(const std::vector<Member>& members, const DistFn& dist, uint64_t now)
	{
		Status st;
		auto member_of = [&members](uint32_t id) -> const Member*
		{
			for (const Member& m : members)
				if (m.id == id)
					return &m;
		    return nullptr;
		};
		// 1. claims that do not hold any more
		for (auto it = m_claims.begin(); it != m_claims.end();)
		{
			const Member* m = member_of(it->first);
			// F3a: one who stands on the level the exit leads to does not go
			bool drop = !m || !m->in_world || !m->alive || m->on_target;
			if (!drop && m->remote && now > it->second.seen && now - it->second.seen > kClaimLapseMs)
				drop = true;
			// F3a: a yes from another map is not near the exit at all: no distance drops it; nor does any drop "go with the
			// group" (no place)
			if (!drop && !m->other_map && !is_guide(it->second.goal) && it->second.place != kNoPlace && dist)
			{
				const float d = dist(it->first, it->second.place);
				if (d >= 0.f && d > kLeaveM)
					drop = true;
			}
			it = drop ? m_claims.erase(it) : std::next(it);
		}
		// 1b. "go with the group" to an exit nobody stands at any more: that wait is over
		for (auto it = m_claims.begin(); it != m_claims.end();)
		{
			const bool lost = it->second.place == kNoPlace && !is_guide(it->second.goal) &&
			    !anchored(it->second.goal, it->first);
			it = lost ? m_claims.erase(it) : std::next(it);
		}
		// 2. the guide trip's life
		if (m_guide)
		{
			bool any = false;
			for (const auto& c : m_claims)
				any = any || is_guide(c.second.goal);
			if (!any)
				m_guide = false;
			else if (now - m_guide_since >= kGuideLifeMs)
			{
				for (auto it = m_claims.begin(); it != m_claims.end();)
					it = is_guide(it->second.goal) ? m_claims.erase(it) : std::next(it);
				m_guide = false;
				st.expired = true;
			}
		}
		// 3. the exit that leads: the oldest claim
		bool have_lead = false;
		uint32_t lead = 0;
		{
			uint64_t best_since = 0;
			uint32_t best_id = 0;
			for (const auto& c : m_claims)
			{
				if (!have_lead || c.second.since < best_since || (c.second.since == best_since && c.first < best_id))
				{
					have_lead = true;
					lead = c.second.goal;
					best_since = c.second.since;
					best_id = c.first;
				}
			}
		}
		const bool forced = m_forced;
		const uint32_t forced_goal = m_forced_goal;
		m_forced = false; // a force is for this tick only
		if (!have_lead)
		{
			m_loading = false;
			return st; // Idle
		}
		st.goal = lead;
		// 4. the rows
		std::vector<Member> sorted(members);
		std::sort(sorted.begin(), sorted.end(), [](const Member& a, const Member& b) { return a.id < b.id; });
		uint32_t blockers = 0, loaders = 0;
		for (const Member& m : sorted)
		{
			if (m.on_target)
				continue; // F3a: already where the exit leads: neither waited for nor listed
			Row r;
			r.id = m.id;
			r.other_map = m.other_map;
			const auto c = m_claims.find(m.id);
			if (c != m_claims.end())
			{
				r.mark = c->second.goal == lead ? Mark::Ready : Mark::Elsewhere;
				r.consent = c->second.place == kNoPlace;
				++(r.mark == Mark::Ready ? st.ready : blockers);
			}
			else if (!m.in_world)
			{
				r.mark = Mark::Loading;
				++loaders;
			}
			else if (!m.alive)
				r.mark = Mark::Down;
			else
			{
				r.mark = Mark::Away;
				++blockers;
			}
			if (m.in_world && !m.other_map && dist)
			{
				const float d = dist(m.id, lead);
				if (d >= 0.f)
					r.dist_m = uint16_t(std::min(65534.f, std::floor(d + 0.5f)));
			}
			st.rows.push_back(r);
		}
		st.total = st.ready + blockers + loaders;
		// 5. the chat's /go
		if (forced && std::any_of(m_claims.begin(), m_claims.end(),
		                         [forced_goal](const auto& c) { return c.second.goal == forced_goal; }))
		{
			st.goal = forced_goal;
			return go(st);
		}
		// 6. who is missing
		if (blockers > 0)
		{
			m_loading = false;
			st.state = State::Waiting;
			return st;
		}
		if (loaders > 0)
		{
			if (!m_loading)
			{
				m_loading = true;
				m_load_since = now;
			}
			const uint64_t waited = now > m_load_since ? now - m_load_since : 0;
			if (waited < kLoadWaitMs)
			{
				st.state = State::Waiting;
				st.load_ms = uint32_t(kLoadWaitMs - waited);
				return st;
			}
			st.total -= loaders; // the minute is over: the group does not wait for them any more
		}
		else
			m_loading = false;
		// 7. nobody is missing: the group goes on this tick (owner 2026-10-24: no waiting timer)
		return go(st);
	}

private:
	struct Claim
	{
		uint32_t goal = 0;
		uint32_t place = 0; // what the host measures it against: the goal, kRoutePlace | a restrictor, kNoPlace
		uint64_t since = 0, seen = 0;
	};
	void put(uint32_t id, uint32_t goal, uint32_t place, uint64_t now)
	{
		auto it = m_claims.find(id);
		if (it != m_claims.end() && it->second.goal == goal)
		{
			it->second.place = place; // the same exit, maybe another way: the claim keeps its age
			it->second.seen = now;
			return;
		}
		m_claims[id] = Claim{goal, place, now, now};
	}
	Status& go(Status& st)
	{
		st.go = true;
		st.state = State::Going;
		m_claims.clear();
		m_guide = false;
		m_loading = false;
		return st;
	}
	std::map<uint32_t, Claim> m_claims;
	bool m_guide = false;
	uint64_t m_guide_since = 0;
	bool m_forced = false;
	uint32_t m_forced_goal = 0;
	bool m_loading = false;
	uint64_t m_load_since = 0;
};

// ---------------------------------------------------------------- messages

// Client -> host (Msg::TransitionVote, protocol 11): u8 op, u16 level changer in HOST numbering, and for VoteRoute a u16
// route restrictor (host numbering). Ready at an exit (the host checks the exit, that the player is in the world and within
// kReachM of it), Ready without an exit = "yes" to the host's guide trip, Cancel = "not any more" (the changer is not read),
// Consent = "go with the group" to that exit from afar (2026-10-24: no distance, only to an exit somebody else stands at),
// Route = he stands in a route restrictor whose drop point lies in that exit's changer (within kReachM of the restrictor).
enum VoteOp : uint8_t
{
	VoteReady = 0,
	VoteCancel = 1,
	VoteConsent = 2,
	VoteRoute = 3,
};

struct VoteMsg
{
	uint8_t op = VoteReady;
	uint16_t changer = kNoChanger;
	uint16_t route = kNoChanger; // VoteRoute only

	static bool is_client_op(uint8_t o) { return o == VoteReady || o == VoteCancel || o == VoteConsent || o == VoteRoute; }
	void write(Writer& w) const
	{
		w.u8(op);
		w.u16(changer);
		if (op == VoteRoute)
			w.u16(route);
	}
	bool read(Reader& r)
	{
		op = r.u8();
		changer = r.u16();
		route = op == VoteRoute ? r.u16() : kNoChanger;
		if (!r.ok() || !is_client_op(op))
			return false;
		// coming along, or a route, always into an exit; a route always names its restrictor
		if ((op == VoteConsent || op == VoteRoute) && changer == kNoChanger)
			return false;
		return op != VoteRoute || route != kNoChanger;
	}
};

// F3a T8: what the world server knows of the sender of a TransitionVote when it takes it (coop_join.cpp reads it from its
// world). The same for every sender, the host player (peer 0, over the local link) included.
struct VoteFacts
{
	bool in_world = false; // his character is in the world the server serves now
	bool exit_known = false; // the exit he names is a level changer of the server's level
	float dist = -1.f; // metres from his character to that exit, < 0 unknown
	bool other_map = false; // he is on another map (the together entry of split levels): a yes from there, no distance
	bool guide_running = false; // a guide trip runs and its starter still says yes
	bool route_known = false; // VoteRoute: the restrictor he names is a space restrictor of the server's level
	float route_dist = -1.f; // VoteRoute: metres from his character to that restrictor, < 0 unknown
};

// What a vote did to the board (the server logs the refusals).
enum class Verdict : uint8_t
{
	Claimed, // he is ready now (a new claim, one for another exit, or the same exit another way)
	Refreshed, // the same claim again: it keeps its age
	Released, // a cancel took his claim
	Nothing, // a cancel of one who had no claim
	NotInWorld, // a ready from one who is not in the world
	NoExit, // a ready at an exit the server's level does not have
	TooFar, // a ready from further than kReachM of the exit (or its route restrictor), or from an unknown distance, on the
	        // exit's map
	NoGuide, // a yes to a guide trip that does not run
	NotWaited, // "go with the group" to an exit nobody else stands at
	NoRoute, // a route restrictor the server's level does not have
};

// The server's rule for a TransitionVote of player `from` (VoteMsg read and checked by the caller).
inline Verdict apply_vote(Board& b, uint32_t from, const VoteMsg& v, const VoteFacts& f, uint64_t now)
{
	if (v.op == VoteCancel)
		return b.release(from) ? Verdict::Released : Verdict::Nothing;
	if (!f.in_world)
		return Verdict::NotInWorld;
	uint32_t goal = kGuideGoal;
	uint32_t place = kGuideGoal;
	if (v.changer == kNoChanger)
	{
		if (!f.guide_running)
			return Verdict::NoGuide;
	}
	else
	{
		if (!f.exit_known)
			return Verdict::NoExit;
		goal = goal_of_exit(v.changer);
		if (v.op == VoteConsent)
		{
			// 2026-10-24: one who is not at the exit comes along, from anywhere; to an exit somebody else stands at
			if (!b.anchored(goal, from))
				return Verdict::NotWaited;
			place = kNoPlace;
		}
		else if (v.op == VoteRoute)
		{
			// 2026-10-24: the route restrictor is the exit's mark (its drop point is in the changer, maybe far behind it): he
			// is measured against the restrictor, as against the zone of the changer itself
			if (!f.route_known)
				return Verdict::NoRoute;
			if (!f.other_map && !(f.route_dist >= 0.f && f.route_dist <= kReachM))
				return Verdict::TooFar;
			place = place_of_route(v.route);
		}
		else
		{
			// T12 review (Ruling 18): on the exit's map only within kReachM of it (an unknown distance is no claim); F3a: a
			// yes from another map is the together entry's remote one, no distance
			if (!f.other_map && !(f.dist >= 0.f && f.dist <= kReachM))
				return Verdict::TooFar;
			place = goal;
		}
	}
	uint32_t before = 0, before_place = 0;
	const bool again = b.claimed(from, &before, &before_place) && before == goal && before_place == place;
	if (place == kNoPlace)
		b.consent(from, goal, now);
	else if (is_route_place(place))
		b.claim_route(from, goal, route_of_place(place), now);
	else
		b.claim(from, goal, now);
	return again ? Verdict::Refreshed : Verdict::Claimed;
}

// Host -> all (Msg::TransitionState): the board as every machine's placard shows it. `level` is the stock name of the level
// the leading exit leads to ("l01_escape"; the Lua side translates it), empty when the exit has none (read: unknown).
//   u8 state, u8 kind (0 exit, 1 guide), u16 changer, str level, u16 ready, u16 total, u32 countdown_ms (always 0 since the
//   countdown is gone, 2026-10-24: the layout of protocol 11 stays), u32 load_ms,
//   u8 n, n x { u32 player, u8 mark (| kRowOtherMap: on another map, F3a; | kRowConsent: comes along), u16 dist_m }
struct StateMsg
{
	State state = State::Idle;
	uint8_t kind = 0;
	uint16_t changer = kNoChanger;
	std::string level;
	uint16_t ready = 0, total = 0;
	uint32_t countdown_ms = 0; // always 0 (the layout of protocol 11)
	uint32_t load_ms = 0;
	std::vector<Row> rows;

	void write(Writer& w) const
	{
		w.u8(uint8_t(state));
		w.u8(kind);
		w.u16(changer);
		w.str(level);
		w.u16(ready);
		w.u16(total);
		w.u32(countdown_ms);
		w.u32(load_ms);
		const size_t n = rows.size() < 255 ? rows.size() : 255;
		w.u8(uint8_t(n));
		for (size_t i = 0; i < n; ++i)
		{
			w.u32(rows[i].id);
			w.u8(uint8_t(uint8_t(rows[i].mark) | (rows[i].other_map ? kRowOtherMap : 0u) | (rows[i].consent ? kRowConsent : 0u)));
			w.u16(rows[i].dist_m);
		}
	}
	bool read(Reader& r)
	{
		const uint8_t s = r.u8();
		kind = r.u8();
		changer = r.u16();
		level = r.str();
		ready = r.u16();
		total = r.u16();
		countdown_ms = r.u32();
		load_ms = r.u32();
		const uint8_t n = r.u8();
		if (!r.ok() || s > uint8_t(State::Going) || kind > 1 || level.size() > kMaxLevelName || n > kMaxPlayers)
			return false;
		state = State(s);
		rows.clear();
		for (uint8_t i = 0; i < n; ++i)
		{
			Row row;
			row.id = r.u32();
			const uint8_t byte = r.u8();
			row.dist_m = r.u16();
			const uint8_t m = uint8_t(byte & ~(kRowOtherMap | kRowConsent));
			if (!r.ok() || m > uint8_t(Mark::Elsewhere))
				return false;
			row.mark = Mark(m);
			row.other_map = (byte & kRowOtherMap) != 0;
			row.consent = (byte & kRowConsent) != 0;
			rows.push_back(row);
		}
		return r.ok();
	}
};

// The board's tick as the message every machine gets.
inline StateMsg state_message(const Status& s, const std::string& level)
{
	StateMsg m;
	m.state = s.state;
	if (s.state == State::Idle)
		return m;
	m.kind = is_guide(s.goal) ? 1 : 0;
	m.changer = is_guide(s.goal) ? kNoChanger : uint16_t(s.goal);
	m.level = level.size() > kMaxLevelName ? level.substr(0, kMaxLevelName) : level;
	m.ready = uint16_t(s.ready);
	m.total = uint16_t(s.total);
	m.load_ms = s.load_ms;
	m.rows = s.rows;
	if (m.rows.size() > kMaxPlayers)
		m.rows.resize(kMaxPlayers);
	return m;
}

// ---------------------------------------------------------------- the hold at the exit

// A player who waits at an exit does not walk off the edge of the level (the live test of 2026-10-23: a client at an exit
// went past the map and the host refused every state of his, "position outside the level"). While he waits, the place he
// stood at last inside the zone and on the map is remembered; if he is off the map (zone or not) he is put back there. A
// step back into the level, out of the zone, is his way to withdraw: nothing holds him there. Not waiting: nothing is held
// (the stock level edge) and the memory is gone.
struct Hold
{
	bool have = false;
	float good[3] = {0.f, 0.f, 0.f};

	// True: put the player at `back`.
	bool step(bool waiting, bool in_zone, bool on_map, const float pos[3], float back[3])
	{
		if (!waiting)
		{
			have = false;
			return false;
		}
		if (in_zone && on_map)
		{
			good[0] = pos[0];
			good[1] = pos[1];
			good[2] = pos[2];
			have = true;
			return false;
		}
		if (!on_map && have)
		{
			back[0] = good[0];
			back[1] = good[1];
			back[2] = good[2];
			return true;
		}
		return false;
	}
	void reset() { have = false; }
};

// ---------------------------------------------------------------- what a client wants against what the host says

enum class Send : uint8_t
{
	Nothing,
	Ready, // TransitionVote Ready (the exit, or none for a guide trip)
	Cancel, // TransitionVote Cancel
};

// The client keeps saying what its player wants until the host's board agrees (a message may be dropped by the host's rate
// row or lost): `want` / `goal` is what the player does (stands in an exit's zone or a route restrictor into it, comes along
// from afar, or confirmed the guide trip), `op` the vote that says it (VoteReady / VoteRoute / VoteConsent), the view is what
// the host's last TransitionState says about this player. The caller paces what it sends (limits::Pacer) and calls sent()
// only when something went out.
struct Intent
{
	uint64_t last = 0; // 0: nothing sent since the last reset
	uint8_t said = VoteReady; // the op of the last ready that went out

	Send wants(bool want, uint32_t goal, bool view_ready, uint32_t view_goal, uint64_t now, uint8_t op = VoteReady) const
	{
		if (last != 0 && now >= last && now - last < kIntentSettleMs)
			return Send::Nothing; // what went out a moment ago is still on its way to the board
		if (!want)
			return view_ready ? Send::Cancel : Send::Nothing;
		if (!view_ready || view_goal != goal)
			return Send::Ready;
		if (last != 0 && op != said)
			return Send::Ready; // the same exit another way (into the zone from afar, from the route into the zone itself)
		if (last != 0 && now >= last && now - last >= kClaimRefreshMs)
			return Send::Ready; // still here: the host's claim of a client lapses without this
		return Send::Nothing;
	}
	void sent(uint64_t now, uint8_t op = VoteReady)
	{
		last = now;
		said = op;
	}
	void reset() { last = 0; }
};

// ---------------------------------------------------------------- route exits (2026-10-24)

// The stock route exits of Anomaly / G.A.M.M.A. are space restrictors (sr_teleport_sections.ltx, logic
// scripts\sr_teleport_logic.ltx: on_actor_inside = %=sr_teleport%) whose effect fades the screen and, 3 s later, puts the
// actor at the route's drop point, which lies inside the real level changer. In a session with company that is no exit of
// the group's: the fade comes when one walks up, and everybody who walks up is put at the same point. Instead the player's
// own machine (scripts\coop_transition_route.script) asks the engine which level changer holds the drop point
// (coop.transition_route); while his actor is inside that restrictor he stands at that changer's exit (coop_join.cpp
// transition_step, VoteRoute), nothing is faded or moved, and the group's level change is the changer's own.

// A level changer of this level and the metres from a route's drop point to its shapes (< 0 or infinite: unknown).
struct ExitDistance
{
	uint16_t changer = kNoChanger;
	float dist = -1.f;
};

// The exit a route drops its traveller into: the nearest within kRouteDropM (a tie: the lower id), kNoChanger if none (a
// route inside the level: the stock teleport stays).
inline uint16_t route_exit(const std::vector<ExitDistance>& exits)
{
	uint16_t best = kNoChanger;
	float best_d = 0.f;
	for (const ExitDistance& e : exits)
	{
		if (e.changer == kNoChanger || !(e.dist >= 0.f && e.dist <= kRouteDropM))
			continue;
		if (best == kNoChanger || e.dist < best_d || (e.dist == best_d && e.changer < best))
		{
			best = e.changer;
			best_d = e.dist;
		}
	}
	return best;
}

// The route restrictors this machine has learned (this machine's numbering), each with the level changer its drop point is
// in; for one world (a new one forgets them). At most kMaxRoutes.
class Routes
{
public:
	struct Entry
	{
		uint16_t restrictor = kNoChanger;
		uint16_t changer = kNoChanger;
	};
	// False: nothing to remember, or full.
	bool add(uint16_t restrictor, uint16_t changer)
	{
		if (restrictor == kNoChanger || changer == kNoChanger)
			return false;
		for (Entry& e : m_entries)
			if (e.restrictor == restrictor)
			{
				e.changer = changer;
				return true;
			}
		if (m_entries.size() >= kMaxRoutes)
			return false;
		m_entries.push_back(Entry{restrictor, changer});
		return true;
	}
	bool forget(uint16_t restrictor)
	{
		const auto it = std::find_if(m_entries.begin(), m_entries.end(),
		    [restrictor](const Entry& e) { return e.restrictor == restrictor; });
		if (it == m_entries.end())
			return false;
		m_entries.erase(it);
		return true;
	}
	uint16_t changer_of(uint16_t restrictor) const
	{
		for (const Entry& e : m_entries)
			if (e.restrictor == restrictor)
				return e.changer;
		return kNoChanger;
	}
	void clear() { m_entries.clear(); }
	const std::vector<Entry>& all() const { return m_entries; }

private:
	std::vector<Entry> m_entries;
};

// ---------------------------------------------------------------- F3a T8: the simultaneous load of a group transition

static_assert(kLoadWaitMs == levels::kLoadWaitMs, "the board's loaders' minute and the load barrier's are the same minute");
constexpr uint32_t kLoadReadyRepeatMs = 3000; // a player on a held screen says LoadReady again this often (a hole may eat it)
constexpr uint32_t kLoadAloneGraceMs = 30000; // after the server's minute, this long more before a held screen lets go alone
constexpr uint32_t kLoadBlobStallMs = 30000; // the server's world is up and the save has not moved this long: it is lost
constexpr uint64_t kBlobChunkHeader = 6; // R1 3: a BlobChunk as the session counts it: u16 type, u32 blob id, then data
constexpr uint64_t kReloadWatchMs = 10000; // R1 5: frames this long after the go's pump and no reload: none is coming

// R1 3: the movers' saves are handed to the network: since the pump began the session sent `chunks` BlobChunk messages of
// `chunk_bytes` in all (headers included), and only the movers' saves stream then (data_total = their sizes together).
// A save can send no more than its own size, so the data reaching their total means every one of them is handed over.
inline bool saves_handed(uint64_t chunk_bytes, uint64_t chunks, uint64_t data_total)
{
	const uint64_t headers = chunks * kBlobChunkHeader;
	return chunk_bytes >= headers && chunk_bytes - headers >= data_total;
}

// R1 5: a prepared load whose reload has not come: `pending` (the next world is the transition's), `since` (the pump
// ended, 0 not yet).
inline bool reload_overdue(bool pending, uint64_t since, uint64_t now)
{
	return pending && since != 0 && now >= since + kReloadWatchMs;
}

// The world server's side (coop_join.cpp): the movers of one transition and the barrier of their loading screens. The
// barrier (levels::LoadBarrier) is started at the "go" so that a LoadReady of the new epoch is counted whenever it comes
// (it waits in the server's world gate until the server's world is up, coop_world_gate.h LoadReady = Hold); its minute is
// started again when the server's world is up, with the readiness kept: nobody can enter a world before it exists, and the
// host's own load does not eat the others' minute. The host player (peer 0, kHostPeer) is a mover: his readiness is the
// server's world (world_up). Enter goes to every mover who is ready once the barrier opens; a laggard enters alone when he is.
class PreparedLoad
{
public:
	// The "go": a transition to world epoch `epoch` for `movers` (the host player 0 among them: his process reloads).
	// Replaces the load that was.
	void start(uint32_t epoch, std::vector<uint32_t> movers, uint64_t now)
	{
		m_active = true;
		m_epoch = epoch;
		m_world_up = false;
		m_movers.clear();
		for (uint32_t p : movers)
			if (std::find(m_movers.begin(), m_movers.end(), p) == m_movers.end())
				m_movers.push_back(p);
		m_entered.clear();
		m_barrier.start(epoch, m_movers, false, now);
	}
	// LoadReady {epoch} from `peer`: true if it made him ready now.
	bool ready(uint32_t peer, uint32_t epoch)
	{
		if (!m_active || epoch != m_epoch || !is_mover(peer) || m_barrier.is_ready(peer))
			return false;
		m_barrier.ready(peer, epoch);
		return m_barrier.is_ready(peer);
	}
	// The server's world is up (and with it the host player): the minute starts now.
	void world_up(uint64_t now)
	{
		if (!m_active || m_world_up)
			return;
		std::vector<uint32_t> were_ready;
		for (uint32_t p : m_movers)
			if (m_barrier.is_ready(p))
				were_ready.push_back(p);
		m_barrier.start(m_epoch, m_movers, false, now);
		for (uint32_t p : were_ready)
			m_barrier.ready(p, m_epoch);
		m_barrier.world_up(m_epoch); // the host player (levels::LoadBarrier's host_peer 0)
		m_world_up = true;
	}
	// A mover left the session, or asked for the world the usual way: nobody waits for him.
	void drop(uint32_t peer)
	{
		m_movers.erase(std::remove(m_movers.begin(), m_movers.end(), peer), m_movers.end());
		m_entered.erase(std::remove(m_entered.begin(), m_entered.end(), peer), m_entered.end());
		m_barrier.drop(peer);
	}
	// The movers who leave their screen now (LoadWait Enter): ready, not entered yet, the barrier open. Marks them entered.
	std::vector<uint32_t> take_entering(uint64_t now)
	{
		std::vector<uint32_t> out;
		if (!m_active || !m_world_up || !m_barrier.enter(now))
			return out;
		for (uint32_t p : m_movers)
			if (m_barrier.is_ready(p) && !entered(p))
			{
				m_entered.push_back(p);
				out.push_back(p);
			}
		return out;
	}
	// Every mover entered (or was dropped).
	bool finished() const
	{
		if (!m_active || !m_world_up)
			return false;
		for (uint32_t p : m_movers)
			if (!entered(p))
				return false;
		return true;
	}
	// LoadWait rows: every mover, ready or not, in the order of start().
	std::vector<std::pair<uint32_t, bool>> rows() const
	{
		std::vector<std::pair<uint32_t, bool>> out;
		for (uint32_t p : m_movers)
			out.emplace_back(p, m_barrier.is_ready(p));
		return out;
	}
	void clear()
	{
		m_active = false;
		m_world_up = false;
		m_movers.clear();
		m_entered.clear();
		m_barrier.clear();
	}

	bool active() const { return m_active; }
	uint32_t epoch() const { return m_epoch; }
	bool world_is_up() const { return m_active && m_world_up; }
	bool is_mover(uint32_t peer) const
	{
		return m_active && std::find(m_movers.begin(), m_movers.end(), peer) != m_movers.end();
	}
	bool entered(uint32_t peer) const { return std::find(m_entered.begin(), m_entered.end(), peer) != m_entered.end(); }
	bool is_ready(uint32_t peer) const { return m_active && m_barrier.is_ready(peer); }
	const std::vector<uint32_t>& movers() const { return m_movers; }

private:
	bool m_active = false;
	bool m_world_up = false;
	uint32_t m_epoch = 0;
	std::vector<uint32_t> m_movers;
	std::vector<uint32_t> m_entered;
	levels::LoadBarrier m_barrier{0}; // the host player is peer 0 (kHostPeer)
};

// A player's loading screen of a simultaneous load (a remote client, or the host player in the host's process).
enum class Load : uint8_t
{
	None, // no simultaneous load
	Waiting, // a remote client: LoadWait Prepared came, its save is on the way (the old world is still on the screen)
	Loading, // the world of the transition loads under the screen (the host player: from his own process's reload on)
	Holding, // the world is up, LoadReady said: the screen stays until Enter
	Entering, // Enter came: leave the screen (a client goes live) as soon as the world is up
};

struct LoadScreen
{
	Load stage = Load::None;
	uint32_t epoch = 0;
	bool local = false; // the host player: his own world is the server's
	uint64_t since = 0; // LoadWait Prepared
	uint64_t progress_at = 0; // the last piece of the save (Waiting)
	uint64_t up_at = 0; // the world came up under the screen
	uint64_t said_at = 0; // the last LoadReady
	uint64_t host_up_at = 0; // the first word from the server after its world came up (LoadWait Waiting / Enter), 0 none
	std::vector<uint32_t> waiting_for; // who the server still waits for (its last LoadWait), the line on the screen
	uint32_t world = 0; // R1 4: this machine's world generation at Prepared (join::world_generation)

	// LoadWait Prepared {e}. `download`: a remote client, the save comes as a blob (Waiting); the host player's process loads
	// the world itself (Loading). False: refused (not newer than the transition that runs).
	bool prepared(uint32_t e, bool download, uint64_t now, uint32_t world_now)
	{
		if (stage != Load::None && e <= epoch)
			return false;
		*this = LoadScreen();
		stage = download ? Load::Waiting : Load::Loading;
		epoch = e;
		local = !download;
		since = progress_at = now;
		world = world_now;
		return true;
	}
	void progress(uint64_t now)
	{
		if (stage == Load::Waiting)
			progress_at = now;
	}
	// The save came and its load is issued (Waiting -> Loading).
	bool loading()
	{
		if (stage != Load::Waiting)
			return false;
		stage = Load::Loading;
		return true;
	}
	// The world is up under the screen: true once (Loading -> Holding) = say LoadReady.
	// R1 4: only a world created after the Prepared counts (`world_now` is this machine's world generation): the host
	// player's process runs the old level for a frame or two after the go.
	bool world_up(uint64_t now, uint32_t world_now)
	{
		if (stage != Load::Loading || world_now == world)
			return false;
		stage = Load::Holding;
		up_at = said_at = now;
		return true;
	}
	// Holding: true = say LoadReady again (every kLoadReadyRepeatMs).
	bool repeat(uint64_t now)
	{
		if (stage != Load::Holding || now < said_at + kLoadReadyRepeatMs)
			return false;
		said_at = now;
		return true;
	}
	// LoadWait Waiting {e}: the server's world is up; who is ready.
	void waiting(uint32_t e, const std::vector<std::pair<uint32_t, bool>>& rows, uint64_t now)
	{
		if (stage == Load::None || e != epoch)
			return;
		if (host_up_at == 0)
			host_up_at = now;
		waiting_for.clear();
		for (const auto& row : rows)
			if (!row.second)
				waiting_for.push_back(row.first);
	}
	// LoadWait Enter {e}: true if the screen goes now (or as soon as the world is up).
	bool enter(uint32_t e, uint64_t now)
	{
		if (e != epoch || (stage != Load::Loading && stage != Load::Holding))
			return false;
		if (host_up_at == 0)
			host_up_at = now;
		stage = Load::Entering;
		waiting_for.clear();
		return true;
	}
	// LoadWait Cancel {e}: the load is off, nothing is held.
	bool cancel(uint32_t e)
	{
		if (stage == Load::None || e != epoch)
			return false;
		reset();
		return true;
	}
	// Holding, and no Enter kLoadWaitMs + kLoadAloneGraceMs after the server's world was seen up: enter alone. Never before
	// the server's world is up (a client must not play in a world the host is not in yet); the host player's own world is it.
	bool alone(uint64_t now) const
	{
		if (stage != Load::Holding)
			return false;
		const uint64_t base = host_up_at != 0 ? host_up_at : (local ? up_at : 0);
		return base != 0 && now >= base + kLoadWaitMs + kLoadAloneGraceMs;
	}
	// Waiting, the server's world is up and the save has not moved for kLoadBlobStallMs: it is lost (ask for the world).
	bool stalled(uint64_t now) const
	{
		if (stage != Load::Waiting || host_up_at == 0)
			return false;
		return now >= std::max(progress_at, host_up_at) + kLoadBlobStallMs;
	}
	// The loading screen stays (CGamePersistent::game_loaded, coop::hold_loading_screen).
	bool holds() const { return stage == Load::Loading || stage == Load::Holding; }
	void reset() { *this = LoadScreen(); }
};
} // namespace transition
} // namespace coop
