// Anomaly Coop — the seats of the writer answers of the server's processes.
//
// The world server and a map server ask the Core's rule writer::writer_of (xrCoop/coop_writer_rules.h) with a Seat they fill from
// their own tables (the mode fills Seat). How they fill it is the server's topology (bridges, holders, seats), needed by no
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
// ------------------------------------------------------------------------------------------------- the server's seats

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
//   PeerId maker_of(uint16_t root)          the bridge whose ids `root` has (its slice or a block this world gave it:
//                                           levels::host_id_is_senders), kInvalidPeer: none

// A player's character as the world server's tables place it: who plays it, and the map whose server decides for him.
//   player  kInvalidPeer: nobody plays it (the ghost; parked: its player left, join::on_peer_left -> host_park_proxy; retired: its
//           player joins again with a new character; a level move's rejoin under way, host_rejoin_peer)
//   map     his player's seat, kNoLevel when nobody plays it, he is not seated (he joins), or he is seated on the world server's
//           own map (or its world is being replaced): the world keeps him
// WorldServerMode::decider_of answers from it (Nobody without a player, Other for a map, else This) and world_seat names the
// writer from it, so the two never part: a map server writes a character only while a player plays him there.
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
//                 nobody plays is the world's, wherever his body stands (else the world would name the map server of his body
//                 while that map server, which has no player of him, names the world: nobody would write him);
//   a unit        held: the bridge that holds it and that bridge's map; at home: the world (a unit a map server made reaches the
//                 world's keeper with its SpawnPrepare, after its Spawn);
//   a resident    the server of its map;
//   of no map     (RootKind::None) one a map server made under its ids is a resident of that server's map (made_at_no_map), else
//                 the world's.
//
// What a map server makes at no game vertex (a psy phantom: level.spawn_item with no vertex, the stock phantom_manager) stands on no
// map in any process's graph, and it never gets a vertex (CPhantom does not export one). The world takes its Spawn as the maker's
// (accept::rootless_spawn_writer: born at no vertex by the sender), so its later facts are the maker's as well: only that map server
// has it online. Read as "of no map" here, the world named itself the writer and refused the maker's Died of every phantom.
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
	case RootKind::None:
		{
			const PeerId maker = t.maker_of(in.root);
			if (maker != kInvalidPeer)
			{
				a.root.kind = RootKind::Resident;
				a.root.level = t.map_of_bridge(maker);
				if (a.root.level != kNoLevel)
					a.seat.map = t.lead_of_map(a.root.level);
			}
			break;
		}
	}
	return a;
}

// A map server's tables, as map_seat asks them (MapServerMode answers from its own books, the tests from rows):
//   bool plays_here(uint16_t character)   a player of its session plays it (join::peer_of_proxy, its session's peer): its decider_of
//                                         says This for exactly these
//   bool authors(uint16_t root)           its holder authors the unit: granted to it, or born and flushed (levels::xfer_authors)
//   bool born(uint16_t root)              it made the unit under its own id slice and its holder has not seen it yet (the next
//                                         flush: levels::xfer_born)
//   bool made_here(uint16_t root)         `root` has an id of its own pool (its slice or a block of its world: levels::xfer_made_here)
// `own_map`: the map it leads (levels::my_level); `own_lead`: Leads once it told its world its map is up (mapserver::server_leads)
// until its process ends, so a stopping server still writes until its Stopped; Starting before.
// What it asks writer_of with: a character a player plays here stands on its map; a unit its holder authors is Held, one it made
// and has not flushed yet is Born (held from its birth; else it would ask its world for its own squad until
// the next flush); a resident of its map is its own; what it makes or made at no map is its map's resident (a new root at no vertex,
// an object of no map under an id of its pool: world_seat names the same writer); anything else is its world's.
// born() is asked before authors(), each once: they are two separately locked reads (levels::xfer_born, xfer_authors: "any
// thread"), and nothing here may rely on the threads' schedule to keep the flush (client_flush_up: the Spawn leaves the queue and
// the holder authors the unit, one step) from landing between them. The hold only moves from born to authored, so in this order a
// flush between them still finds one of the two; "authored?" first could find neither and ask its world for its own squad.
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
		if (in.level == own_map || (in.level == kNoLevel && in.root == kNoId)) // a new root it makes at no vertex stands on its map
		{
			a.root.level = own_map;
			a.seat.map = MapLead{own_lead, kInvalidPeer};
		}
		break;
	case RootKind::None:
		if (t.made_here(in.root))
		{
			a.root.kind = RootKind::Resident;
			a.root.level = own_map;
			a.seat.map = MapLead{own_lead, kInvalidPeer};
		}
		break;
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
