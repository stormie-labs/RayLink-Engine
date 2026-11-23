// Сервер: только сервер мира и серверы карт (docs/MODES.md)
// Anomaly Coop — the seats of the writer answers of the server's processes (TZ Р32, temp/r30/design/R32-final.md §1 W1.2, §8a A3-A5;
// review R32 W1 B1-B4 and its fix round, finding 5).
//
// The world server and a map server ask the Core's rule writer::writer_of (xrCoop/coop_writer_rules.h) with a Seat they fill from
// their own tables (§8a A3: the mode fills Seat). How they fill it is the server's topology (bridges, holders, seats), needed by no
// co-op process, so it lives here, in the server's layer: WorldServerMode and MapServerMode (srv/coop_srv_mode.cpp) answer from
// world_answer / map_answer below, the tests from rows (tests/test_writer_answers.cpp).
// Pure C++17, header-only, no engine.
#pragma once

#include <cstdint>

#include "../coop_writer_rules.h"

namespace coop
{
namespace writer
{
// ------------------------------------------------------------------------------------------------- the server's seats (W1.2, §8a A3)

// What writer_of is asked with: the root (a character's map as the asking process's tables name it) and the seat.
struct Asked
{
	RootFacts root;
	Seat seat;
};

// The world server's tables, as world_character_seat and world_seat ask them (WorldServerMode answers from its own books, the tests
// from rows):
//   PeerId player_of(uint16_t character)    the connected player who plays it (join::peer_of_proxy), kInvalidPeer: nobody
//   uint16_t seat_of(PeerId player)         the map of his seat (levels::level_of_player): the map he stands on or loads into,
//                                           kNoLevel: not seated
//   uint16_t own_map()                      the map of its own seat, the ghost's (levels::my_level)
//   MapLead lead_of_map(uint16_t level)     the life of that map's server and its bridge (lead_of(host_life_of), bridge_of)
//   PeerId holder_of(uint16_t root)         the bridge of the map server that holds the unit (levels::holder_of), kInvalidPeer: home
//   uint16_t map_of_bridge(PeerId bridge)   the map that bridge's server leads (mapserver::level_of_bridge)

// A player's character as the world server's tables place it: who plays it, and the map whose server decides for him.
//   player  kInvalidPeer: nobody plays it (the ghost; parked: its player left, join::on_peer_left -> host_park_proxy; retired: its
//           player joins again with a new character; a level move's rejoin under way, host_rejoin_peer)
//   map     his player's seat, kNoLevel when nobody plays it, he is not seated (he joins), or he is seated on the world server's
//           own map (or its world is being replaced): the world keeps him
// WorldServerMode::decider_of answers from it (Nobody without a player, Other for a map, else This) and world_seat names the
// writer from it, so the two never part (review R32 B2): a map server writes a character only while a player plays him there.
struct CharacterSeat
{
	PeerId player;
	uint16_t map;
};

template <class Tables>
CharacterSeat world_character_seat(uint16_t character, const Tables& t)
{
	const PeerId player = character == kGhostId ? kInvalidPeer : t.player_of(character);
	const uint16_t seat = player == kInvalidPeer ? kNoLevel : t.seat_of(player);
	const uint16_t own = t.own_map();
	return CharacterSeat{player, seat == kNoLevel || own == kNoLevel || seat == own ? kNoLevel : seat};
}

// What the world server asks writer_of with (WorldServerMode::writer_of):
//   a character   his map is his seat's (world_character_seat), not where the world's copy of his object stood last; a character
//                 nobody plays is the world's, wherever his body stands (review R32 B2: the world named the map server of his body
//                 while that map server, which has no player of him, named the world: nobody wrote him or his inventory);
//   a unit        held: the bridge that holds it and that bridge's map; at home: the world (a unit a map server made reaches the
//                 world's keeper with its SpawnPrepare, after its Spawn: W3);
//   a resident    the server of its map.
template <class Tables>
Asked world_seat(const RootFacts& in, const Tables& t)
{
	Asked a{in, Seat{}};
	switch (in.kind)
	{
	case RootKind::Character:
		a.root.level = world_character_seat(in.root, t).map;
		if (a.root.level != kNoLevel)
			a.seat.map = t.lead_of_map(a.root.level);
		break;
	case RootKind::Resident:
		if (in.level != kNoLevel)
			a.seat.map = t.lead_of_map(in.level);
		break;
	case RootKind::Unit:
		{
			const PeerId bridge = t.holder_of(in.root);
			if (bridge != kInvalidPeer)
			{
				a.seat.hold = Hold::Held;
				a.seat.holder = bridge;
				a.seat.holder_level = t.map_of_bridge(bridge);
			}
			break;
		}
	case RootKind::None: break;
	}
	return a;
}

// A map server's tables, as map_seat asks them (MapServerMode answers from its own books, the tests from rows):
//   bool plays_here(uint16_t character)   a player of its session plays it (join::peer_of_proxy, its session's peer): its decider_of
//                                         says This for exactly these
//   bool authors(uint16_t root)           its holder authors the unit: granted to it, or born and flushed (levels::xfer_authors)
//   bool born(uint16_t root)              it made the unit under its own id slice and its holder has not seen it yet (the next
//                                         flush: levels::xfer_born)
// `own_map`: the map it leads (levels::my_level); `own_lead`: Leads once it told its world its map is up (mapserver::server_leads)
// until its process ends, so a stopping server still writes until its Stopped (§8a A4); Starting before.
// What it asks writer_of with: a character a player plays here stands on its map; a unit its holder authors is Held, one it made
// and has not flushed yet is Born (review R32 B3: held from its birth, W1.2b; before, it asked its world for its own squad until
// the next flush); a resident of its map is its own; anything else is its world's.
// born() is asked before authors(), each once: they are two separately locked reads (levels::xfer_born, xfer_authors: "any
// thread"), and nothing here may rely on the threads' schedule to keep the flush (client_flush_up: the Spawn leaves the queue and
// the holder authors the unit, one step) from landing between them. The hold only moves from born to authored, so in this order a
// flush between them still finds one of the two; "authored?" first could find neither and ask its world for its own squad (review
// R32 B fix round, finding 3).
template <class Tables>
Asked map_seat(const RootFacts& in, uint16_t own_map, Lead own_lead, const Tables& t)
{
	Asked a{in, Seat{}};
	switch (in.kind)
	{
	case RootKind::Character:
		if (t.plays_here(in.root))
		{
			a.root.level = own_map;
			a.seat.map = MapLead{own_lead, kInvalidPeer};
		}
		break;
	case RootKind::Unit:
		{
			const bool born = t.born(in.root);
			const bool held = t.authors(in.root); // the holder's word wins over a mark the flush has not cleared
			if (held || born)
			{
				a.seat.hold = held ? Hold::Held : Hold::Born;
				a.seat.holder_level = own_map;
			}
			break;
		}
	case RootKind::Resident:
		if (in.level == own_map)
			a.seat.map = MapLead{own_lead, kInvalidPeer};
		break;
	case RootKind::None: break;
	}
	return a;
}

// The writer of a root as the world server and as a map server answer it (Mode::writer_of of WorldServerMode, MapServerMode).
template <class Tables>
Writer world_answer(const RootFacts& r, const Tables& t)
{
	const Asked a = world_seat(r, t);
	return writer_of(a.root, Process{ProcessKind::World, kNoLevel, kHostPeer}, a.seat);
}

template <class Tables>
Writer map_answer(const RootFacts& r, uint16_t own_map, Lead own_lead, const Tables& t)
{
	const Asked a = map_seat(r, own_map, own_lead, t);
	return writer_of(a.root, Process{ProcessKind::Level, own_map, kHostPeer}, a.seat);
}
} // namespace writer
} // namespace coop
