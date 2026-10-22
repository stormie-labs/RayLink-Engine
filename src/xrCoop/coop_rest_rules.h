// Anomaly Coop — shared sleep decision (F3, CHECKPOINTS_DEATH.md §4). Everybody present has to rest before the group
// sleeps; one player who is awake, in a fight or dead holds it, and so does a blocked world (an emission). The host
// asks `decide` whenever somebody's rest state or the world changes and acts on the answer.
// Pure C++17, header-only, no engine dependencies: used by the engine and by the unit-test exe.
#pragma once

#include <cstdint>
#include <vector>

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
} // namespace rest
} // namespace coop
