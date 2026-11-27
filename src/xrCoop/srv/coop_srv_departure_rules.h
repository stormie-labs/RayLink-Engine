// Anomaly Coop — why a player left the level of a map server (docs/VEHICLES.md section 9): he
// quit the world, or he went to another map of it.
//
// Who decides: the world server holds the session of the world and its table of seats; whenever that table changes it sends it to every
// player and map server (LevelMap, xrCoop/coop_levels_wire.h). A map server keeps a virtual peer for each player of its level
// (VirtualPeers, xrCoop/coop_mapserver_rules.h) and drops it when the table no longer puts him there (coop_trunk.cpp server_on_map), for
// a quit and for a move alike. What the map server has: that table, when it drops him. It already tells the cases apart: the world
// server removes the seat of a peer who left its session (coop_levels.cpp on_peer_left), a move only changes the level of his seat. So
// this needs no message, no protocol and no trust in a client's word.
// The fact reaches the Core through the mode (Mode::departure, xrCoop/coop_mode.h; MapServerMode in srv/coop_srv_mode.cpp): the seat
// the player held in a car of this level is released as Disconnect or LeftLevel (xrCoop/coop_vehicle_drive_rules.h release_reason).
// Co-op: none. Whoever leaves the co-op host's session quit (a co-op process never runs the server's layer).
//
// Pure C++17, no engine dependencies: tests/test_vehicle_departure.cpp.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "coop_levels_wire.h"
#include "../coop_death_rules.h"
#include "../coop_mode.h"

namespace coop
{
namespace srv
{
namespace departure
{
// What the world's table says of player `peer`, whose virtual peer leaves level `level` of this map server now: no row of his, he left
// the session of the world; a row on another level (or on none), he went there; a row on this level, another character took the place
// of the one he had here (or none yet: the world sends him the world again).
inline Departure classify(uint16_t level, PeerId peer, const std::vector<levels::wire::LevelMapMsg::Player>& rows)
{
	for (const levels::wire::LevelMapMsg::Player& r : rows)
		if (r.peer == peer)
			return r.level == level ? Departure::Replaced : Departure::LeftLevel;
	return Departure::Disconnected;
}

// For the log of the level's session (the reason of its virtual_leave).
inline const char* text(Departure d)
{
	switch (d)
	{
	case Departure::Disconnected: return "he left the world";
	case Departure::LeftLevel: return "he went to another level";
	case Departure::Replaced: return "another character of his took his place here";
	}
	return "?";
}

// A map server's facts of how the players of its level left it: one per player, for the character he had here. Written when his
// virtual peer leaves; dropped when he comes back to the level (his next departure writes its own). The Core asks in its next frame,
// and a level has at most kMaxMapPlayers players: past kKept facts the oldest goes.
class Book
{
public:
	static constexpr size_t kKept = 2 * levels::wire::kMaxMapPlayers;

	void left(PeerId peer, uint16_t character, Departure why)
	{
		came(peer);
		if (m_facts.size() >= kKept)
			m_facts.erase(m_facts.begin());
		m_facts.push_back({peer, character, why});
	}

	// He is a player of this level again: what his last departure said is over.
	void came(PeerId peer)
	{
		for (auto it = m_facts.begin(); it != m_facts.end();)
			it = it->peer == peer ? m_facts.erase(it) : it + 1;
	}

	// How `peer` left with `character`; Disconnected when nothing was said of that pair (the answer of a peer who left the session).
	Departure of(PeerId peer, uint16_t character) const
	{
		for (const Fact& f : m_facts)
			if (f.peer == peer && f.character == character)
				return f.why;
		return Departure::Disconnected;
	}

	size_t size() const { return m_facts.size(); }

private:
	struct Fact
	{
		PeerId peer;
		uint16_t character;
		Departure why;
	};
	std::vector<Fact> m_facts;
};

// A stock ChangeLevel fades for 3 s before its message goes. A player who goes down and dies in that time must not be
// moved all the same and arrive downed again (health 0.01, a new bleed-out), out of the evacuation and
// respawn of his death. Who decides: the world server, by the life his game reported (survival::Life, numbers of
// xrCoop/coop_death_rules.h); his own game asks only standing (coop_join.cpp client_level_trip), the world does not take its word.
// Only a standing player leaves his level, through an exit or on a trip. Co-op: none (its group transition counts the standing).
inline bool may_leave(uint8_t life)
{
	return life == ::coop::death::kLifeAlive;
}
} // namespace departure
} // namespace srv
} // namespace coop
