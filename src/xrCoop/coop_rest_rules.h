// Anomaly Coop — shared sleep decision (F3, CHECKPOINTS_DEATH.md §4). Everybody present has to rest before the group
// sleeps; one player who is awake, in a fight or dead holds it, and so does a blocked world (an emission). The host
// asks `decide` whenever somebody's rest state or the world changes and acts on the answer.
// F3 Task 11: the messages RestRequest / RestState / RestSleep, the checks of a lie-down, the host's book of who rests
// (Group: lie down, get up, the 500 ms tick that ends rests and starts / ends the sleep) and what a machine tells its
// Lua (changes, waiting). The engine side is xrGame/coop/coop_rest.cpp.
// Pure C++17, header-only, no engine dependencies: used by the engine and by the unit-test exe.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iterator>
#include <map>
#include <vector>

#include "coop_protocol.h" // kMaxPlayers, Writer, Reader

namespace coop
{
namespace rest
{
enum class State : uint8_t
{
	Awake = 0,
	Resting = 1,  // sits at a rest spot and wants to sleep
	Sleeping = 2  // the shared sleep is running
};

inline const char* state_name(State s)
{
	switch (s)
	{
	case State::Resting: return "resting";
	case State::Sleeping: return "sleeping";
	case State::Awake:
	default: return "awake";
	}
}

// One player present in the session (not one who left).
struct Member
{
	uint16_t id;
	State state;
	uint8_t hours; // how long this player wants to sleep
	bool in_combat;
	bool alive;
};

// Why the sleep does not start. Dead beats Combat beats World beats NotAll.
enum class Block : uint8_t
{
	None,
	NotAll,
	Combat,
	World,
	Dead
};

struct Decision
{
	bool start = false;
	uint8_t hours = 0; // when start: the shortest wish of the group, 1..24
	Block block = Block::None;
};

// Sleep hours are 1..24.
inline uint8_t clamp_hours(int h)
{
	return h < 1 ? uint8_t(1) : (h > 24 ? uint8_t(24) : static_cast<uint8_t>(h));
}

// start: at least one player present, every one of them alive, Resting and out of a fight, and the world does not
// block (`world_blocked`: an emission or a story scene). hours = the minimum of the wishes, so nobody sleeps longer
// than they wanted. Nobody present is not a sleep: it blocks as NotAll (or World).
// `present` holds the players who take part in the sleep. A hardcore spectator (out of the run for good) is not one of
// them: passed in as a dead member he would block the sleep with Dead for ever, as a resting one he would be counted
// as asleep. The caller leaves him out.
inline Decision decide(const std::vector<Member>& present, bool world_blocked)
{
	Decision d;
	bool any_dead = false, any_combat = false, all_resting = !present.empty();
	int hours = 24;
	for (const Member& m : present)
	{
		if (!m.alive)
			any_dead = true;
		if (m.in_combat)
			any_combat = true;
		if (m.state != State::Resting)
			all_resting = false;
		if (int(m.hours) < hours)
			hours = int(m.hours);
	}

	if (any_dead)
		d.block = Block::Dead;
	else if (any_combat)
		d.block = Block::Combat;
	else if (world_blocked)
		d.block = Block::World;
	else if (!all_resting)
		d.block = Block::NotAll;
	else
	{
		d.start = true;
		d.hours = clamp_hours(hours);
	}
	return d;
}

// ---------------------------------------------------------------- F3 Task 11

// The shared sleep (its effects on every machine) lasts this long; then everybody asleep is awake again.
constexpr uint64_t kSleepMs = 12000;

// Why a lie-down was refused or a rest ended without the player's word (Lua coop_rest_refused(reason) on his machine).
// A refused lie-down also ends the rest he was waiting in (Group::on_refused). Values are part of the protocol
// (RestState); a new one is appended, never renumbered.
enum class Refusal : uint8_t
{
	None = 0,
	Place = 1, // not at a rest place (rest zone, own tent, lit campfire, mod camp); also at the start of the sleep
	Combat = 2, // in a fight
	Dead = 3, // not alive (wounded, dead, respawning)
	Busy = 4, // the group sleeps now, or the host cannot take it
	Camp = 5 // T11 review M2: the checkpoint could not be set where he lies (checkpoints::host_set refused it)
};
inline bool refusal_ok(uint8_t v) { return v <= uint8_t(Refusal::Camp); }

inline const char* refusal_name(Refusal r)
{
	switch (r)
	{
	case Refusal::Place: return "place";
	case Refusal::Combat: return "combat";
	case Refusal::Dead: return "dead";
	case Refusal::Busy: return "busy";
	case Refusal::Camp: return "camp";
	case Refusal::None:
	default: return "none";
	}
}

// "place" | "combat" | "dead" | "busy" | "camp"; anything else (nullptr, "none", unknown) is Busy: a rest ended for a
// reason the player's Lua does not know still tells him something.
inline Refusal refusal_from_name(const char* s)
{
	if (s)
		for (const Refusal r : {Refusal::Place, Refusal::Combat, Refusal::Dead, Refusal::Busy, Refusal::Camp})
		{
			const char* n = refusal_name(r);
			size_t i = 0;
			while (n[i] && s[i] == n[i])
				++i;
			if (!n[i] && !s[i])
				return r;
		}
	return Refusal::Busy;
}

// Block values are part of the protocol (RestState) too.
inline bool block_ok(uint8_t v) { return v <= uint8_t(Block::Dead); }

// "none" | "not_all" | "combat" | "world" | "dead" (Lua coop.rest.block()).
inline const char* block_name(Block b)
{
	switch (b)
	{
	case Block::NotAll: return "not_all";
	case Block::Combat: return "combat";
	case Block::World: return "world";
	case Block::Dead: return "dead";
	case Block::None:
	default: return "none";
	}
}

// The checks of a lie-down in this order: alive ("dead"), the group not asleep ("busy"), then, unless forced (passing
// out of tiredness), out of a fight ("combat") and at a rest place ("place").
inline Refusal check_lie_down(bool alive, bool group_sleeping, bool forced, bool in_combat, bool at_rest_place)
{
	if (!alive)
		return Refusal::Dead;
	if (group_sleeping)
		return Refusal::Busy;
	if (!forced && in_combat)
		return Refusal::Combat;
	if (!forced && !at_rest_place)
		return Refusal::Place;
	return Refusal::None;
}

// ---------------------------------------------------------------- messages

// RestRequest, client -> host: u8 op, u8 hours, u8 forced (0 / 1), nothing after. The host clamps the hours; a get-up
// carries 0, 0.
enum class Op : uint8_t
{
	LieDown = 1,
	GetUp = 2
};
struct Request
{
	Op op = Op::GetUp;
	uint8_t hours = 0;
	bool forced = false;
};

inline void write_request(Writer& w, const Request& r)
{
	w.u8(uint8_t(r.op));
	w.u8(r.hours);
	w.u8(r.forced ? 1 : 0);
}

inline bool read_request(Reader& rd, Request& out)
{
	const uint8_t op = rd.u8();
	const uint8_t hours = rd.u8();
	const uint8_t forced = rd.u8();
	if (!rd.ok() || rd.remaining() != 0 || (op != uint8_t(Op::LieDown) && op != uint8_t(Op::GetUp)) || forced > 1)
		return false;
	out.op = static_cast<Op>(op);
	out.hours = hours;
	out.forced = forced != 0;
	return true;
}

// One player of RestState (host numbering on the wire). An awake player has 0 hours, a resting or sleeping one 1..24.
struct Entry
{
	uint16_t character = 0xffff;
	State state = State::Awake;
	uint8_t hours = 0;
};
inline bool operator==(const Entry& a, const Entry& b)
{
	return a.character == b.character && a.state == b.state && a.hours == b.hours;
}
inline bool operator!=(const Entry& a, const Entry& b) { return !(a == b); }

// RestState, host -> clients: u8 block, u8 n (<= kMaxPlayers), n x {u16 character, u8 state, u8 hours} by character
// ascending, u8 refused, nothing after. `block` is what holds the sleep while somebody rests (None when nobody does).
// `refused` (a Refusal, 0..5; 5 Camp since T11 review M2) is not None only in the copy that goes to one player: his own
// lie-down was refused, or his rest ended without his word (a fight, death, his place at the start of the sleep,
// host_cancel). Every player present is listed, the awake ones too (waiting_for).
struct StateMsg
{
	Block block = Block::None;
	std::vector<Entry> entries;
	Refusal refused = Refusal::None;
};

inline void write_state(Writer& w, const StateMsg& m)
{
	const size_t n = m.entries.size() < size_t(kMaxPlayers) ? m.entries.size() : size_t(kMaxPlayers);
	w.u8(uint8_t(m.block));
	w.u8(uint8_t(n));
	for (size_t i = 0; i < n; ++i)
	{
		w.u16(m.entries[i].character);
		w.u8(uint8_t(m.entries[i].state));
		w.u8(m.entries[i].hours);
	}
	w.u8(uint8_t(m.refused));
}

inline bool read_state(Reader& rd, StateMsg& out)
{
	StateMsg m;
	const uint8_t block = rd.u8();
	const uint8_t n = rd.u8();
	if (!rd.ok() || !block_ok(block) || n > kMaxPlayers)
		return false;
	m.block = static_cast<Block>(block);
	m.entries.reserve(n);
	for (uint8_t i = 0; i < n; ++i)
	{
		Entry e;
		e.character = rd.u16();
		const uint8_t state = rd.u8();
		e.hours = rd.u8();
		if (!rd.ok() || e.character == 0xffff || state > uint8_t(State::Sleeping))
			return false;
		e.state = static_cast<State>(state);
		const bool hours_ok = e.state == State::Awake ? e.hours == 0 : e.hours >= 1 && e.hours <= 24;
		if (!hours_ok || (!m.entries.empty() && m.entries.back().character >= e.character))
			return false;
		m.entries.push_back(e);
	}
	const uint8_t refused = rd.u8();
	if (!rd.ok() || rd.remaining() != 0 || !refusal_ok(refused))
		return false;
	m.refused = static_cast<Refusal>(refused);
	out = m;
	return true;
}

// RestSleep, host -> every sleeper: u8 hours (1..24), nothing after.
inline void write_sleep(Writer& w, uint8_t hours) { w.u8(hours); }

inline bool read_sleep(Reader& rd, uint8_t& out)
{
	const uint8_t h = rd.u8();
	if (!rd.ok() || rd.remaining() != 0 || h < 1 || h > 24)
		return false;
	out = h;
	return true;
}

// ---------------------------------------------------------------- the host's book

// A player who takes part in the sleep at a tick (host numbering): in the world, not a hardcore spectator.
struct Seen
{
	uint16_t id = 0xffff;
	bool alive = true;
	bool in_combat = false;
};

// A rest that ended without the player's word. reason None: he left (nobody to tell).
struct Cancel
{
	uint16_t id = 0xffff;
	Refusal reason = Refusal::None;
};

struct Tick
{
	std::vector<Cancel> cancelled;
	bool started = false; // the shared sleep starts now: everybody present is Sleeping
	uint8_t hours = 0; // when started: the shortest wish
	bool woke = false; // the shared sleep is over: everybody asleep is awake
	Block block = Block::None; // what holds the sleep while somebody rests; None when nobody does
};

// Who rests, on the host (host numbering). A player not in the book is awake. The engine checks a lie-down
// (check_lie_down) before it calls lie_down, and calls tick every 500 ms with the players present.
class Group
{
public:
	// He rests with `hours` (clamped to 1..24); `forced` (passing out of tiredness) is a rest without a camp; `in_combat`:
	// he is in a fight now (only a forced rest starts in one). A fight that begins later ends the rest (tick). Lying
	// down again takes the new wish. False while the shared sleep runs, or when the book holds kMaxPlayers rests already
	// and he is not one of them.
	bool lie_down(uint16_t id, int hours, bool forced, bool in_combat)
	{
		if (m_sleep)
			return false;
		const auto it = m_rest.find(id);
		if (it == m_rest.end() && m_rest.size() >= size_t(kMaxPlayers))
			return false;
		Rest r;
		r.state = State::Resting;
		r.hours = clamp_hours(hours);
		r.forced = forced;
		r.fighting = in_combat;
		m_rest[id] = r;
		return true;
	}

	// He is awake (his own word, or the host ends his rest). True if he was not.
	bool get_up(uint16_t id) { return m_rest.erase(id) != 0; }

	// lie_down would take him: no sleep runs, and he rests already or the book has room. T11 review M2: the engine asks
	// this before it sets the camp, so a camp never comes without the rest.
	bool may_lie_down(uint16_t id) const
	{
		return !m_sleep && (m_rest.count(id) != 0 || m_rest.size() < size_t(kMaxPlayers));
	}

	// T11 review M1: his lie-down was refused. His last word was to rest there and then, so a rest he was waiting in
	// (Resting, an older lie-down somewhere else) ends with it: no old consent stays in the book. A running sleep is
	// kept (the time is skipped already). True if a waiting rest ended.
	bool on_refused(uint16_t id)
	{
		const auto it = m_rest.find(id);
		if (it == m_rest.end() || it->second.state != State::Resting)
			return false;
		m_rest.erase(it);
		return true;
	}

	// T11 review M1: is `id` (not passed out) still at a rest place? Asked by tick once per sleep, when everybody rests.
	using PlaceCheck = std::function<bool(uint16_t)>;

	// Every 500 ms with the players taking part now (any order; an id twice counts once). In this order:
	//  1. a rest ends without the player's word: he is not present any more (left, loading the world again, a hardcore
	//     spectator; reason None), he is not alive (Dead), or a fight began for him since the last tick (Combat; the
	//     fight a forced rest started in does not count). This holds for the sleepers too: the time is skipped already.
	//  2. the shared sleep ends kSleepMs after it started, or as soon as nobody sleeps any more: the sleepers are awake.
	//  3. no sleep running: decide(present, world_blocked). On start (T11 review M1) every rest that is not a faint is
	//     looked at once more with `at_place` (none given: all stand); one who is not at a rest place any more loses his
	//     rest (Place) and nobody sleeps this tick. Else everybody present is Sleeping for kSleepMs.
	// Present players: an id given twice counts once (the first one given), at most kMaxPlayers (by id) are taken.
	Tick tick(std::vector<Seen> present, bool world_blocked, uint64_t now_ms, const PlaceCheck& at_place = PlaceCheck())
	{
		Tick t;
		std::stable_sort(present.begin(), present.end(), [](const Seen& a, const Seen& b) { return a.id < b.id; });
		present.erase(std::unique(present.begin(), present.end(), [](const Seen& a, const Seen& b) { return a.id == b.id; }),
		    present.end());
		if (present.size() > size_t(kMaxPlayers))
			present.resize(size_t(kMaxPlayers));
		m_present.clear();
		for (const Seen& s : present)
			m_present.push_back(s.id);

		for (auto it = m_rest.begin(); it != m_rest.end();)
		{
			const Seen* s = find(present, it->first);
			bool end = true;
			Refusal why = Refusal::None;
			if (!s)
				why = Refusal::None;
			else if (!s->alive)
				why = Refusal::Dead;
			else if (s->in_combat && !it->second.fighting)
				why = Refusal::Combat;
			else
				end = false;
			if (end)
			{
				Cancel c;
				c.id = it->first;
				c.reason = why;
				t.cancelled.push_back(c);
				it = m_rest.erase(it);
				continue;
			}
			it->second.fighting = s->in_combat;
			++it;
		}

		if (m_sleep)
		{
			bool anyone = false;
			for (const auto& r : m_rest)
				if (r.second.state == State::Sleeping)
					anyone = true;
			if (now_ms >= m_sleep_until || !anyone)
			{
				for (auto it = m_rest.begin(); it != m_rest.end();)
					it = it->second.state == State::Sleeping ? m_rest.erase(it) : std::next(it);
				m_sleep = false;
				m_sleep_until = 0;
				t.woke = true;
			}
		}

		m_block = Block::None;
		if (!m_sleep)
		{
			bool anyone_rests = false;
			const auto members_now = [&]() {
				std::vector<Member> members;
				members.reserve(present.size());
				anyone_rests = false;
				for (const Seen& s : present)
				{
					const auto it = m_rest.find(s.id);
					Member m;
					m.id = s.id;
					m.state = it == m_rest.end() ? State::Awake : it->second.state;
					m.hours = it == m_rest.end() ? uint8_t(0) : it->second.hours;
					m.in_combat = s.in_combat;
					m.alive = s.alive;
					members.push_back(m);
					if (m.state == State::Resting)
						anyone_rests = true;
				}
				return members;
			};
			Decision d = decide(members_now(), world_blocked);
			if (d.start && at_place)
			{
				bool walked_away = false;
				for (auto it = m_rest.begin(); it != m_rest.end();)
				{
					if (it->second.forced || at_place(it->first))
					{
						++it;
						continue;
					}
					Cancel c;
					c.id = it->first;
					c.reason = Refusal::Place;
					t.cancelled.push_back(c);
					it = m_rest.erase(it);
					walked_away = true;
				}
				if (walked_away)
				{
					d = decide(members_now(), world_blocked); // he is present and awake now: not all rest
					d.start = false;
				}
			}
			if (d.start)
			{
				for (auto& r : m_rest) // everybody present rests, and only they are in the book (step 1)
					r.second.state = State::Sleeping;
				m_sleep = true;
				m_sleep_until = now_ms + kSleepMs;
				t.started = true;
				t.hours = d.hours;
			}
			else if (anyone_rests)
				m_block = d.block;
		}
		t.block = m_block;
		return t;
	}

	bool sleeping() const { return m_sleep; } // the shared sleep runs
	Block block() const { return m_block; } // as of the last tick
	State state_of(uint16_t id) const
	{
		const auto it = m_rest.find(id);
		return it == m_rest.end() ? State::Awake : it->second.state;
	}
	uint8_t hours_of(uint16_t id) const
	{
		const auto it = m_rest.find(id);
		return it == m_rest.end() ? uint8_t(0) : it->second.hours;
	}
	bool forced(uint16_t id) const
	{
		const auto it = m_rest.find(id);
		return it != m_rest.end() && it->second.forced;
	}
	size_t resting() const { return m_rest.size(); } // resting or sleeping

	// Everybody present at the last tick and everybody resting (one who lay down since), by id; at most kMaxPlayers.
	std::vector<Entry> entries() const
	{
		std::vector<uint16_t> ids = m_present;
		for (const auto& r : m_rest)
			ids.push_back(r.first);
		std::sort(ids.begin(), ids.end());
		ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
		std::vector<Entry> out;
		out.reserve(ids.size());
		for (uint16_t id : ids)
		{
			if (out.size() >= size_t(kMaxPlayers))
				break;
			Entry e;
			e.character = id;
			e.state = state_of(id);
			e.hours = hours_of(id);
			out.push_back(e);
		}
		return out;
	}

	// A world change or a session reset: everybody awake, no sleep, nobody present until the next tick.
	void clear() { *this = Group(); }

private:
	struct Rest
	{
		State state = State::Resting;
		uint8_t hours = 1;
		bool forced = false;
		bool fighting = false; // in a fight at the last tick (or when he lay down)
	};

	static const Seen* find(const std::vector<Seen>& sorted, uint16_t id)
	{
		const auto it = std::lower_bound(sorted.begin(), sorted.end(), id, [](const Seen& s, uint16_t v) { return s.id < v; });
		return it != sorted.end() && it->id == id ? &*it : nullptr;
	}

	std::map<uint16_t, Rest> m_rest; // resting or sleeping (<= kMaxPlayers)
	std::vector<uint16_t> m_present; // as of the last tick, ascending (<= kMaxPlayers)
	bool m_sleep = false;
	uint64_t m_sleep_until = 0;
	Block m_block = Block::None;
};

// ---------------------------------------------------------------- what a machine tells its Lua

// A coop_rest_changed(id, state, hours).
struct Change
{
	uint16_t id = 0xffff;
	State state = State::Awake;
	uint8_t hours = 0;
};

// What changed for this machine's Lua between two lists (each by id ascending): an id whose state or hours differ;
// one missing from a list counts as awake with 0 hours, so a player gone or a forgotten world (Review Focus 2) is
// reported awake and a newcomer who is awake is no change.
inline std::vector<Change> changes(const std::vector<Entry>& before, const std::vector<Entry>& after)
{
	std::vector<Change> out;
	size_t i = 0, j = 0;
	while (i < before.size() || j < after.size())
	{
		Entry b, a;
		if (j >= after.size() || (i < before.size() && before[i].character < after[j].character))
		{
			b = before[i++];
			a.character = b.character; // gone: awake, 0 hours
		}
		else if (i >= before.size() || after[j].character < before[i].character)
		{
			a = after[j++];
			b.character = a.character; // new: was awake, 0 hours
		}
		else
		{
			b = before[i++];
			a = after[j++];
		}
		if (a.state != b.state || a.hours != b.hours)
		{
			Change c;
			c.id = a.character;
			c.state = a.state;
			c.hours = a.hours;
			out.push_back(c);
		}
	}
	return out;
}

// One callback for this machine's Lua: coop_rest_changed(change) or, with `refusal`, coop_rest_refused(refused).
struct Note
{
	bool refusal = false;
	Change change;
	Refusal refused = Refusal::None;
};

// T11 review M4: what one update tells this machine's Lua, in one order on the host and on a client: every change
// (changes(before, after), by id), then why this machine's player was refused or his rest ended (when `refused` is not
// None). The state is there before the reason is told.
inline std::vector<Note> news(const std::vector<Entry>& before, const std::vector<Entry>& after, Refusal refused)
{
	std::vector<Note> out;
	for (const Change& c : changes(before, after))
	{
		Note n;
		n.change = c;
		out.push_back(n);
	}
	if (refused != Refusal::None)
	{
		Note n;
		n.refusal = true;
		n.refused = refused;
		out.push_back(n);
	}
	return out;
}

// The players the sleep waits for: present and awake (Lua coop.rest.waiting_for gives their nicks).
inline std::vector<uint16_t> waiting(const std::vector<Entry>& entries)
{
	std::vector<uint16_t> out;
	for (const Entry& e : entries)
		if (e.state == State::Awake)
			out.push_back(e.character);
	return out;
}
} // namespace rest
} // namespace coop
