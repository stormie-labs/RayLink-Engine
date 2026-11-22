// Сервер: только сервер мира и серверы карт (docs/MODES.md)
// Anomaly Coop — the world's acceptance of what a map server reports (TZ Р32, temp/r30/design/R32-final.md §1 W2b, W3; §7 WP3;
// §8a A5, A6; R32-addendum-gaps2.md P1).
//
// Before: the world server checked a Spawn only for the slice and a free id, dropped a Destroy of an object marked online elsewhere
// or of a root on another map, and applied Died and Parent by id alone. So the world kept a member a map server had released
// (D1, G3), took a death of another object under a reused id (cleanup M3), dropped the item of a placement while it kept the
// placed object (P1). Now one rule decides every entry of a map server's journal before the world applies it
// (srv/coop_levels_alife.cpp host_apply_fact):
//   a fact (Spawn, Destroy, Died, Parent, State, Item, Tent, Stash, Logic, Joined, Left, Info) is taken when the sender writes the
//     object (for a Spawn the new object's root, for a Parent and a Joined both the old and the new root), the object is the one
//     the world has under that id (section crc, the keeper generation of the unit at its root: A6), and the entry is not past the
//     sender's fence (W4);
//   an ask (release, info of an object the sender does not write) is taken when the world writes the object and its root stands
//     on the sender's map;
//   the journal's own ops (Money, Carry, Done, Flushed, IdAsk) are no object's: each has its own rule where it is applied.
// A refusal names its reason (journal::Why): the journal's Ack carries it back and the sender repairs towards the world (W2b).
// Pure C++17, header-only: the engine and the unit-test exe (tests/test_levels_accept.cpp).
#pragma once

#include <cstdint>

#include "../coop_writer_rules.h"
#include "coop_levels_wire.h"

namespace coop
{
namespace levels
{
namespace accept
{
// What an entry is to the acceptance.
enum class Kind : uint8_t
{
	Fact, // the sender's word about an object it writes
	Ask, // the sender's request about an object of its map it does not write
	Own, // a journal op that names no object: its own rule decides it
};

inline Kind kind_of(uint8_t op)
{
	using H = wire::LevelEntityHead;
	switch (op)
	{
	case H::Destroy:
	case H::Parent:
	case H::Died:
	case H::State:
	case H::Item:
	case H::Spawn:
	case H::Tent:
	case H::Stash:
	case H::Logic:
	case H::Joined:
	case H::Left:
	case H::Info: return Kind::Fact;
	case H::Ask: return Kind::Ask;
	default: break;
	}
	return Kind::Own;
}

// Is the object a report names the object the world has under that id? Its section crc and the keeper generation of the unit at
// its root (A6; 0 for a root that is no unit the sender holds) match (writer::same_object). One allowance: a unit the sender made
// (its SpawnPrepare is registered here under `spawn_gen`, and the unit has not moved since: gen_here == spawn_gen) is named with
// gen 0 by a sender that has not heard the world's Ack yet: it is the same unit. A unit granted again since is another.
inline bool identity(uint16_t crc_here, uint16_t crc_report, uint32_t gen_here, uint32_t gen_report, uint32_t spawn_gen)
{
	if (writer::same_object(crc_here, crc_report, gen_here, gen_report))
		return true;
	return crc_here == crc_report && gen_report == 0 && spawn_gen != 0 && gen_here == spawn_gen;
}

// A new object with no parent stands where it is made: what a map server makes stands on its own map (a resident, or a unit born
// there under its slice); a unit that stands on no map yet (no game vertex) is born by it as well. Another map is not its to make:
// a map server never writes another map (W2a).
inline bool spawn_on_senders_map(uint16_t spawn_level, uint16_t sender_level)
{
	return sender_level != writer::kNoLevel && (spawn_level == sender_level || spawn_level == writer::kNoLevel);
}

// What the world knows of an entry, filled by the world server from its ALife and its tables (host_apply_fact).
struct Facts
{
	uint8_t op = wire::LevelEntityHead::Destroy;
	PeerId sender = kInvalidPeer; // the bridge of the map server that sent it
	uint16_t sender_level = writer::kNoLevel; // the map it leads
	uint32_t epoch = 0; // the seq of the batch the entry came in
	uint32_t fence = writer::kNoFence; // the sender's fence for this object's root (W4): kNoFence for none
	bool exists = false; // the world has an object under the entry's id (a Spawn: under the new object's id)
	bool ghost = false; // the root is the ghost (actor 0 where the actor plays nobody)
	bool same = true; // identity() of the object here and the entry's head (a Spawn: the object here, for a repeat)
	writer::Writer writer = writer::nobody(); // the world's writer of the root (a Spawn: of the new object's root; a Parent and a
	                                          // Joined: of the old root; a Left: of the squad)
	bool two_roots = false; // a Parent, a Joined: writer_new must be the sender too
	writer::Writer writer_new = writer::nobody(); // the writer of the new root (the new parent's, the squad joined)
	bool id_in_slice = false; // a Spawn: the id is of the sender's slice of ids
	uint16_t root_level = writer::kNoLevel; // an ask: the map the object's root stands on
};

struct Verdict
{
	bool ok = false;
	uint8_t why = 0; // journal::Why when refused
	bool repeat = false; // taken, and it changes nothing: the world has it already (a Spawn repeated)
};

inline Verdict taken(bool repeat = false) { return Verdict{true, 0, repeat}; }
inline Verdict refused(journal::Why why) { return Verdict{false, uint8_t(why), false}; }

// The rule (W2b), first match wins.
inline Verdict accepts(const Facts& f)
{
	const Kind kind = kind_of(f.op);
	if (kind == Kind::Own)
		return taken();
	if (!writer::fence_admits(f.epoch, f.fence))
		return refused(journal::PastFence);
	const writer::Writer sender = writer::other(f.sender);
	if (kind == Kind::Ask)
	{
		if (!f.exists)
			return refused(journal::Unknown);
		if (f.ghost)
			return refused(journal::Ghost);
		if (!f.same)
			return refused(journal::Identity);
		if (f.writer.is != writer::WriterIs::This)
			return refused(journal::NotWriter);
		if (f.sender_level == writer::kNoLevel || f.root_level != f.sender_level)
			return refused(journal::NotOwnLevel);
		return taken();
	}
	if (f.op == wire::LevelEntityHead::Spawn)
	{
		if (f.ghost)
			return refused(journal::Ghost); // nothing is made under the ghost (W1.5)
		if (!f.id_in_slice)
			return refused(journal::IdRange);
		if (f.exists)
			return f.same ? taken(true) : refused(journal::IdRange);
		if (f.writer != sender)
			return refused(journal::NotWriter);
		return taken();
	}
	if (!f.exists)
		return refused(journal::Unknown);
	if (f.ghost)
		return refused(journal::Ghost);
	if (!f.same)
		return refused(journal::Identity);
	if (f.writer != sender || (f.two_roots && f.writer_new != sender))
		return refused(journal::NotWriter);
	return taken();
}
} // namespace accept
} // namespace levels
} // namespace coop
