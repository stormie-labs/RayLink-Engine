// Anomaly Coop — the waiting point of a level change (owner 2026-10-23 20:08: "a place where you wait for your allies to
// go to the next level, with the name of the level, and nicer; when we get to split levels it goes away").
//
// Until the split levels of F3a every player is on the host's level and the whole group changes level together. The
// stock exit zone is where the group meets for that: a player who stands in the zone of an exit is READY for it (his
// "claim"); when every player who is in the world is ready for the same exit the group goes after kCountdownMs, by the
// path coop_accept_transition always took. The host may go at once (the hold of a key), a player may step back out of
// the zone and so withdraw. Any level change of the host's own (the stock zone, the "go?" dialog, jump_to_level of a guide)
// goes through the same board while somebody else is in the session (coop_join.cpp host_gate_level_change).
//
// The rule (docs in the report, tests/test_transition_wait.cpp):
//  - who must be ready: every player in the world and alive. A downed, dead or spectating player does not block (he cannot
//    walk to the exit; he goes with the group). A player who is not in the world yet (loading, choosing a character) holds
//    the group, but at most kLoadWaitMs, counted from the moment everybody else is ready (a clock that stops and starts
//    over whenever somebody is not ready), then the group goes without him (he joins the new world after its SessionReset
//    like everybody);
//  - the exit: players ready for different exits do not add up; the exit whose claim is the oldest leads, the others wait
//    "at another exit";
//  - the countdown: kCountdownMs from the moment nobody is missing; anybody stepping back, or a player who gets into the
//    world and is not ready, stops it ("cancelled");
//  - go now: the host holds the key at his exit (kHostGoHoldMs): the group goes at once if somebody (the host) stands at
//    that exit;
//  - a client's claim is kept alive by his refresh every kClaimRefreshMs and lapses kClaimLapseMs after the last one; the
//    host also drops it when the host's own view of the player is further than kLeaveM from the exit (he walked away and the
//    cancel got lost); an exit's claim needs him within kReachM when he makes it (coop_join.cpp checks that, it needs the
//    world);
//  - a guide trip (kGuideGoal: no exit zone, the host's script asked for the move) has the host as its first claim and
//    the others confirm it; it lapses after kGuideLifeMs without everybody's yes.
// Wire (protocol 10): TransitionVote = u8 op + u16 level changer (host numbering; kNoChanger = no exit, a "ready" without
// one confirms the guide trip); TransitionState (host -> all) = StateMsg below, at most every kStateEveryMs while the board
// is not idle, plus once when it goes idle.
// Pure C++17, header-only, no engine dependencies: used by the engine and by the unit-test exe.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "coop_protocol.h" // Writer, Reader, Msg, kMaxPlayers

namespace coop
{
namespace transition
{
constexpr uint16_t kNoChanger = 0xffff;
constexpr uint32_t kGuideGoal = 0x10000; // one guide trip at a time (a goal is a level changer id, or this)
constexpr uint32_t kCountdownMs = 5000; // nobody missing -> this long -> the group goes
constexpr uint32_t kLoadWaitMs = 60000; // a player who is still loading holds the group this long at most
constexpr uint32_t kHostGoHoldMs = 1500; // the host holds the key this long to go at once (like the revive hold)
constexpr uint32_t kGuideLifeMs = 60000; // a guide trip lives this long without everybody's yes
constexpr uint32_t kClaimLapseMs = 20000; // a client's claim lapses this long after his last refresh
constexpr uint32_t kClaimRefreshMs = 5000; // a client says "still ready" this often
constexpr float kReachM = 5.f; // to claim an exit: within this of its nearest shape (the host's view of the player)
constexpr float kLeaveM = 9.f; // the host drops a claim of a player further than this (he left, the cancel got lost)
constexpr uint32_t kIntentSettleMs = 1200; // a client waits this long for the host's board to show what it just sent
constexpr uint32_t kStateEveryMs = 500; // the host's TransitionState while the board is not idle
constexpr uint16_t kUnknownDist = 0xffff;
constexpr size_t kMaxLevelName = 64; // a stock level name is "l01_escape"

inline uint32_t goal_of_exit(uint16_t changer) { return changer; }
inline bool is_guide(uint32_t goal) { return goal == kGuideGoal; }

enum class State : uint8_t
{
	Idle = 0, // nobody waits
	Waiting = 1, // somebody is ready, somebody is missing
	Counting = 2, // nobody is missing: the countdown runs
};

enum class Mark : uint8_t
{
	Ready = 0, // at the exit that leads
	Away = 1, // in the world, not ready
	Loading = 2, // not in the world yet (loading, choosing a character)
	Down = 3, // wounded / dead / spectating: does not block
	Elsewhere = 4, // ready for another exit
};

// A player as the host sees him this tick.
struct Member
{
	uint32_t id = 0; // peer id, the host is 0
	bool in_world = true; // connected and has finished loading (join::host_player_in_world)
	bool alive = true; // survival::Life::Alive
	bool remote = true; // a client: his claim lapses without a refresh (the host's own never does)
};

struct Row
{
	uint32_t id = 0;
	Mark mark = Mark::Away;
	uint16_t dist_m = kUnknownDist; // metres to the leading exit, as the host sees him
};

struct Status
{
	State state = State::Idle;
	uint32_t goal = 0; // the leading exit (a level changer id or kGuideGoal)
	uint32_t ready = 0, total = 0; // total: who counts (everybody but the downed, and the loaders after their minute)
	uint32_t countdown_ms = 0; // left, Counting only
	uint32_t load_ms = 0; // left of the loaders' minute, Waiting for loaders only
	bool go = false; // the group goes now (the claims are gone with it)
	bool cancelled = false; // a countdown that ran was stopped by this tick
	bool expired = false; // a guide trip timed out by this tick
	std::vector<Row> rows; // by player id; empty when Idle
};

// Metres from player `id` to the exit `goal`, or < 0 if unknown (the host's view, coop_join.cpp).
using DistFn = std::function<float(uint32_t id, uint32_t goal)>;

class Board
{
public:
	// `id` is ready for `goal`. The same claim again is a refresh (it keeps its age); another goal replaces the old one.
	void claim(uint32_t id, uint32_t goal, uint64_t now)
	{
		auto it = m_claims.find(id);
		if (it != m_claims.end() && it->second.goal == goal)
		{
			it->second.seen = now;
			return;
		}
		m_claims[id] = Claim{goal, now, now};
	}
	// His claim ends. False if he had none.
	bool release(uint32_t id) { return m_claims.erase(id) != 0; }
	bool claimed(uint32_t id, uint32_t* goal = nullptr) const
	{
		const auto it = m_claims.find(id);
		if (it == m_claims.end())
			return false;
		if (goal)
			*goal = it->second.goal;
		return true;
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
	// The host holds the key at exit `goal`: the next tick lets the group go at once if somebody claimed that exit.
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
		m_counting = false;
		m_loading = false;
	}
	bool empty() const { return m_claims.empty(); }

	Status tick(const std::vector<Member>& members, const DistFn& dist, uint64_t now)
	{
		Status st;
		const bool was_counting = m_counting;
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
			bool drop = !m || !m->in_world || !m->alive;
			if (!drop && m->remote && now > it->second.seen && now - it->second.seen > kClaimLapseMs)
				drop = true;
			if (!drop && !is_guide(it->second.goal) && dist)
			{
				const float d = dist(it->first, it->second.goal);
				if (d >= 0.f && d > kLeaveM)
					drop = true;
			}
			it = drop ? m_claims.erase(it) : std::next(it);
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
			m_counting = false;
			m_loading = false;
			st.cancelled = was_counting;
			return st; // Idle
		}
		st.goal = lead;
		// 4. the rows
		std::vector<Member> sorted(members);
		std::sort(sorted.begin(), sorted.end(), [](const Member& a, const Member& b) { return a.id < b.id; });
		uint32_t blockers = 0, loaders = 0;
		for (const Member& m : sorted)
		{
			Row r;
			r.id = m.id;
			const auto c = m_claims.find(m.id);
			if (c != m_claims.end())
			{
				r.mark = c->second.goal == lead ? Mark::Ready : Mark::Elsewhere;
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
			if (m.in_world && dist)
			{
				const float d = dist(m.id, lead);
				if (d >= 0.f)
					r.dist_m = uint16_t(std::min(65534.f, std::floor(d + 0.5f)));
			}
			st.rows.push_back(r);
		}
		st.total = st.ready + blockers + loaders;
		// 5. the go-now key of the host
		if (forced && std::any_of(m_claims.begin(), m_claims.end(),
		                         [forced_goal](const auto& c) { return c.second.goal == forced_goal; }))
		{
			st.go = true;
			st.goal = forced_goal;
			st.state = State::Counting;
			m_claims.clear();
			m_guide = false;
			m_counting = false;
			m_loading = false;
			return st;
		}
		// 6. who is missing
		if (blockers > 0)
		{
			m_counting = false;
			m_loading = false;
			st.state = State::Waiting;
			st.cancelled = was_counting;
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
				m_counting = false;
				st.state = State::Waiting;
				st.load_ms = uint32_t(kLoadWaitMs - waited);
				st.cancelled = was_counting;
				return st;
			}
			st.total -= loaders; // the minute is over: the group does not wait for them any more
		}
		else
			m_loading = false;
		// 7. nobody is missing: the countdown
		if (!m_counting)
		{
			m_counting = true;
			m_count_since = now;
		}
		const uint64_t elapsed = now > m_count_since ? now - m_count_since : 0;
		st.state = State::Counting;
		if (elapsed >= kCountdownMs)
		{
			st.go = true;
			st.countdown_ms = 0;
			m_claims.clear();
			m_guide = false;
			m_counting = false;
			m_loading = false;
		}
		else
			st.countdown_ms = uint32_t(kCountdownMs - elapsed);
		return st;
	}

private:
	struct Claim
	{
		uint32_t goal = 0;
		uint64_t since = 0, seen = 0;
	};
	std::map<uint32_t, Claim> m_claims;
	bool m_guide = false;
	uint64_t m_guide_since = 0;
	bool m_forced = false;
	uint32_t m_forced_goal = 0;
	bool m_counting = false;
	uint64_t m_count_since = 0;
	bool m_loading = false;
	uint64_t m_load_since = 0;
};

// ---------------------------------------------------------------- messages

// Client -> host (Msg::TransitionVote, protocol 10): u8 op, u16 level changer in HOST numbering. Ready at an exit (the host
// checks the exit, that the player is in the world and within kReachM of it), Ready without an exit = "yes" to the host's
// guide trip, Cancel = "not any more" (the changer is not read).
enum VoteOp : uint8_t
{
	VoteReady = 0,
	VoteCancel = 1,
};

struct VoteMsg
{
	uint8_t op = VoteReady;
	uint16_t changer = kNoChanger;

	static bool is_client_op(uint8_t o) { return o == VoteReady || o == VoteCancel; }
	void write(Writer& w) const
	{
		w.u8(op);
		w.u16(changer);
	}
	bool read(Reader& r)
	{
		op = r.u8();
		changer = r.u16();
		return r.ok() && is_client_op(op);
	}
};

// Host -> all (Msg::TransitionState): the board as every machine's placard shows it. `level` is the stock name of the level
// the leading exit leads to ("l01_escape"; the Lua side translates it), empty when the exit has none (read: unknown).
//   u8 state, u8 kind (0 exit, 1 guide), u16 changer, str level, u16 ready, u16 total, u32 countdown_ms, u32 load_ms,
//   u8 n, n x { u32 player, u8 mark, u16 dist_m }
struct StateMsg
{
	State state = State::Idle;
	uint8_t kind = 0;
	uint16_t changer = kNoChanger;
	std::string level;
	uint16_t ready = 0, total = 0;
	uint32_t countdown_ms = 0, load_ms = 0;
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
			w.u8(uint8_t(rows[i].mark));
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
		if (!r.ok() || s > uint8_t(State::Counting) || kind > 1 || level.size() > kMaxLevelName || n > kMaxPlayers)
			return false;
		state = State(s);
		rows.clear();
		for (uint8_t i = 0; i < n; ++i)
		{
			Row row;
			row.id = r.u32();
			const uint8_t m = r.u8();
			row.dist_m = r.u16();
			if (!r.ok() || m > uint8_t(Mark::Elsewhere))
				return false;
			row.mark = Mark(m);
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
	m.countdown_ms = s.countdown_ms;
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
// row or lost): `want` / `goal` is what the player does (stands in an exit's zone, or confirmed the guide trip), the view is
// what the host's last TransitionState says about this player. The caller paces what it sends (limits::Pacer) and calls
// sent() only when something went out.
struct Intent
{
	uint64_t last = 0; // 0: nothing sent since the last reset

	Send wants(bool want, uint32_t goal, bool view_ready, uint32_t view_goal, uint64_t now) const
	{
		if (last != 0 && now >= last && now - last < kIntentSettleMs)
			return Send::Nothing; // what went out a moment ago is still on its way to the board
		if (!want)
			return view_ready ? Send::Cancel : Send::Nothing;
		if (!view_ready || view_goal != goal)
			return Send::Ready;
		if (last != 0 && now >= last && now - last >= kClaimRefreshMs)
			return Send::Ready; // still here: the host's claim of a client lapses without this
		return Send::Nothing;
	}
	void sent(uint64_t now) { last = now; }
	void reset() { last = 0; }
};
} // namespace transition
} // namespace coop
