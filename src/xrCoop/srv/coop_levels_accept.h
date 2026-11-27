// Anomaly Coop — the world's acceptance of what a map server reports.
//
// Checking a Spawn only for the slice and a free id, or Died and Parent by id alone, would keep a member a map server released,
// take a death of another object under a reused id, drop the item of a placement while keeping the placed object. One rule
// decides every entry of a map server's journal before the world applies it (srv/coop_levels_alife.cpp host_apply_fact):
//   a fact (Spawn, Destroy, Died, Parent, State, Item, Tent, Stash, Logic, Joined, Left, Info) is taken when the sender writes the
//     object (for a Spawn the new object's root, for a Parent and a Joined both the old and the new root), the object is the one
//     the world has under that id (section crc, the keeper generation of the unit at its root), and the entry is not past the
//     sender's fence;
//   an ask (release, info of an object the sender does not write) is taken when the world writes the object and its root stands
//     on the sender's map;
//   the journal's own ops (Money, Carry, Done, Flushed, IdAsk) are no object's: each has its own rule where it is applied.
// The departure flood: a Parent to the holder the object has here already changes nothing; it is taken as a repeat
// whoever sent it (unchanged_parent), after the checks of the fence, the object and its identity, before the writer's.
// A refusal names its reason (journal::Why): the journal's Ack carries it back and the sender repairs towards the world.
// A Spawn with no holder is the sender's only while it leads its map or stops (rootless_spawn_writer); a body report of a
// character its sender does not write is decided by his seat (body_report_by_seat), and a State as a whole by body_report (taken,
// crossed his move, refused); the world's release of what a map server released follows release_plan; a Left it accepted follows
// leave_plan.
// Pure C++17, header-only: the engine and the unit-test exe (tests/test_levels_accept.cpp).
#pragma once

#include <cstdint>

#include "../coop_writer_rules.h"
#include "coop_journal_wire.h"

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

// Is the object a report names the object the world has under that id? Its section crc and the keeper generation of its root (the
// sender's hold of that root whatever it is now, 0 when the sender does not hold it: levels::root_gen) match
// (writer::same_object). One allowance: a unit the sender made (its SpawnPrepare is registered here under `spawn_gen`, and the unit
// has not moved since: gen_here == spawn_gen) is named with gen 0 by a sender that has not heard the world's Ack yet: it is the same
// unit. A unit granted again since is another.
inline bool identity(uint16_t crc_here, uint16_t crc_report, uint32_t gen_here, uint32_t gen_report, uint32_t spawn_gen)
{
	if (writer::same_object(crc_here, crc_report, gen_here, gen_report))
		return true;
	return crc_here == crc_report && gen_report == 0 && spawn_gen != 0 && gen_here == spawn_gen;
}

// A new object with no parent stands where it is made: what a map server makes stands on its own map (a resident, or a unit born
// there under its slice); a unit that stands on no map yet (no game vertex) is born by it as well. Another map is not its to make:
// a map server never writes another map.
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
	uint32_t fence = writer::kNoFence; // the sender's fence for this object's root: kNoFence for none
	bool exists = false; // the world has an object under the entry's id (a Spawn: under the new object's id)
	bool ghost = false; // the root is the ghost (actor 0 where the actor plays nobody)
	bool same = true; // identity() of the object here and the entry's head (a Spawn: the object here, for a repeat)
	writer::Writer writer = writer::nobody(); // the world's writer of the root (a Spawn: of the new object's root; a Parent and a
	                                          // Joined: of the old root; a Left: of the squad)
	bool two_roots = false; // a Parent, a Joined: writer_new must be the sender too
	writer::Writer writer_new = writer::nobody(); // the writer of the new root (the new parent's, the squad joined)
	bool id_in_slice = false; // a Spawn: the id is the sender's to make (its slice, or a block this world gave it)
	uint16_t root_level = writer::kNoLevel; // an ask: the map the object's root stands on
	// A Spawn with no holder. Its writer is not `writer` above but rootless_spawn_writer(): the map it stands on
	// (`spawn_level`, kNoLevel for a unit born at no vertex yet) and whether the sender writes the residents of its own map now
	// (`sender_leads`: its server Leads or Stops, writer::writes; a Starting one does not). `writer` is then the world's
	// writer of the residents of the map the object stands on (of the sender's map for one at no map).
	bool rootless = false;
	uint16_t spawn_level = writer::kNoLevel;
	bool sender_leads = false;
	// A Parent: the object is in that holder here already (unchanged_parent).
	bool unchanged = false;
	// The root is a player's character (writer::RootKind::Character, the ghost aside): a State of it is his body report
	// (body_report_by_seat).
	bool character = false;
};

// The departure flood (Server). A map server switches a leaving player's character offline: the stock switch rejects every
// item with a hook and takes each back without one, and a map server whose snapshot did not list his character sent a Parent of each
// to the same character. The world writes the parked character by then, so it refused each as not the writer, and each refusal's
// repair (Fetch: StateDumpRequest Replace) destroyed and made the item again on that map server: 100-300 items per departure. A
// Parent into the holder the object has here already changes nothing whoever sends it: a repeat, never a refusal. A Parent to the
// ground is never one: it carries the place the object was put down at.
inline bool unchanged_parent(uint16_t parent_here, uint16_t to)
{
	return to != wire::kNoObject && parent_here == to;
}

// The sender writes the object's root, and for a Parent or a Joined the new root as well.
inline bool sender_writes(const Facts& f)
{
	const writer::Writer sender = writer::other(f.sender);
	return f.writer == sender && (!f.two_roots || f.writer_new == sender);
}

// Only a map server that leads its map, or stops, writes the map's residents; one that is still
// Starting (in session, loading its world before it says Ready) does not, and neither does one that leads another map. The writer
// of a new object with no holder:
//   on the sender's map, or at no map yet (a unit the sender bore at no vertex): the sender while it writes its map's residents,
//     else whoever the world says writes them (`f.writer`: the world while that map's server Starts or has none);
//   on another map: the writer of that map's residents (`f.writer`): never the sender (a map server never writes another map).
inline writer::Writer rootless_spawn_writer(const Facts& f)
{
	if (spawn_on_senders_map(f.spawn_level, f.sender_level) && f.sender_leads)
		return writer::other(f.sender);
	return f.writer;
}

// What the world does with an object a map server that writes it released (a Destroy, an Ask of a release). The facts
// of the object here:
struct ReleaseFacts
{
	bool player = false; // a player's character, or the ghost: never released by a map server's word
	bool online = false; // m_bOnline here: on the world server, simulated elsewhere (a held unit or a held squad's member)
	bool member = false; // a creature whose squad is here (m_group_id names an object of this world)
	bool own_vertex = false; // it stands at a valid game vertex
	bool squad_vertex = false; // its squad stands at a valid game vertex
};

// The plan: `release` it, after `take_squad_vertex` (a member that stands nowhere goes into the graph at its squad's vertex: the
// stock release puts it there on its way out, a vertex out of range would be read out of the registry) and `clear_online` (the
// stock release of an object marked online would take it out of the level registry, which the world server has not got); or not,
// and `why`:
//   a player's character, the ghost: never;
//   online elsewhere and no member: a held unit itself, its holder's Destroy on the transfer lane releases it (host_gone);
//   a member at no vertex whose squad stands at none either: no vertex to release it at.
struct ReleasePlan
{
	bool release = false;
	bool take_squad_vertex = false;
	bool clear_online = false;
	const char* why = nullptr;
};

inline ReleasePlan release_plan(const ReleaseFacts& f)
{
	ReleasePlan p;
	if (f.player)
	{
		p.why = "a player's character";
		return p;
	}
	if (f.online)
	{
		if (!f.member)
		{
			p.why = "online elsewhere, not a member";
			return p;
		}
		if (!f.own_vertex)
		{
			if (!f.squad_vertex)
			{
				p.why = "a member at no vertex";
				return p;
			}
			p.take_squad_vertex = true;
		}
		p.clear_online = true;
	}
	p.release = true;
	return p;
}

// what the world does with a Left it accepted (the sender writes the
// squad). The facts of the creature here:
struct LeaveFacts
{
	bool listed = false; // the squad lists it (m_members)
	bool bound = false; // ... bound to its object (register_member, coop_bind_members); not bound: an id waiting for its packet
	bool bound_elsewhere = false; // its m_group_id names another squad: a newer word bound it there before this Left (a squad the
	                              // sender made and moved it into in the frame of the Left: that squad's Spawn listed it)
	bool living = false; // a living creature on the ground (no holder)
	bool heal = false; // the audit's heal of a member list made this Left (levels::host_apply_heal), not the sender's journal: the
	                   // sender's Holder never adopted the creature at it
};

// The plan:
//   NotMember  it is not listed: a repeat, nothing changes;
//   Unlist     bound in another squad already: only this squad's list forgets it; its m_group_id, m_bOnline and registries are its
//              new squad's (adopting it, or taking it home, would break that binding and leave it nobody's);
//   Adopt      the sender's own Left of a living bound member: held by the sender on its own from now on (Keeper::adopt), online
//              elsewhere, in no registry here (coop_leave_member_held); Home when the keeper cannot give it to the sender;
//   Home       today's way (coop_leave_member): a creature of its own at home here, in the graph and the scheduler (a dead member, one
//              in a holder, the audit's heal), or the id of one never bound dropped.
enum class Leave : uint8_t
{
	NotMember,
	Unlist,
	Adopt,
	Home,
};

inline Leave leave_plan(const LeaveFacts& f)
{
	if (!f.listed)
		return Leave::NotMember;
	if (!f.bound)
		return Leave::Home;
	if (f.bound_elsewhere)
		return Leave::Unlist;
	return f.living && !f.heal ? Leave::Adopt : Leave::Home;
}

struct Verdict
{
	bool ok = false;
	uint8_t why = 0; // journal::Why when refused
	bool repeat = false; // taken, and it changes nothing: the world has it already (a Spawn repeated)
};

inline Verdict taken(bool repeat = false) { return Verdict{true, 0, repeat}; }
inline Verdict refused(journal::Why why) { return Verdict{false, uint8_t(why), false}; }

// The rule, first match wins.
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
			return refused(journal::Ghost); // nothing is made under the ghost
		if (!f.id_in_slice)
			return refused(journal::IdRange);
		if (f.exists)
			return f.same ? taken(true) : refused(journal::IdRange);
		if ((f.rootless ? rootless_spawn_writer(f) : f.writer) != sender)
			return refused(journal::NotWriter);
		return taken();
	}
	if (!f.exists)
		return refused(journal::Unknown);
	if (f.ghost)
		return refused(journal::Ghost);
	if (!f.same)
		return refused(journal::Identity);
	if (f.op == wire::LevelEntityHead::Parent && f.unchanged)
		return taken(true); // the departure flood: it changes nothing, so there is nothing to refuse and nothing to repair
	if (!sender_writes(f))
		return refused(journal::NotWriter);
	return taken();
}

// A body report (State) of a player's character that its sender does not write. The rule of the character's seat
// (writer::world_seat) makes the world the writer of a parked character and the new map's server the writer of a re-seated one, at
// once on his move, while the map server he left hears of it only from the next LevelMap; the report it journaled in between (one
// a second) reaches the world after that. The acceptance refuses such a report as not the writer before the verdict on the body
// (wire::body_report_verdict) that tells a crossing from a fault is reached, so that verdict decides it instead. Every other reason
// (no object, the ghost, another object, past the fence) stays a refusal.
inline bool body_report_by_seat(const Facts& f, const Verdict& v)
{
	return f.op == wire::LevelEntityHead::State && f.character && !v.ok && v.why == uint8_t(journal::NotWriter);
}

// What the world reads off a body report and off its own books for wire::body_report_verdict.
struct BodyFacts
{
	bool read = false; // the report read whole
	bool known = false; // the world has the object
	bool online_here = false;
	bool player_character = false;
	uint16_t body_level = wire::kNoLevel; // the level of the report's vertex
	uint16_t sender_level = wire::kNoLevel;
	uint16_t seat_level = wire::kNoLevel; // the level of his player's seat (kNoLevel: parked, between two levels)
	bool position_valid = false;
};

// What becomes of a body report at the world:
//   Taken    its sender writes him: the world places his character (`why` 0);
//   Crossed  it crossed his move within the grace (wire::kBodyCrossedGraceMs): a fact applied without effect. No refusal goes back
//            in the journal's Ack and neither side prints "!": the sender did nothing wrong, and a refusal would print one on the
//            map server for every departure whose report crossed it;
//   Refused  a fault of its sender: by the acceptance (`by_acceptance`, `why` its reason, its line is the acceptance's) or by the
//            verdict on the body (`why` journal::Refused, the world's line names the facts), a crossing past the grace among them.
enum class BodyOutcome : uint8_t
{
	Taken,
	Crossed,
	Refused,
};

struct BodyDecision
{
	BodyOutcome outcome = BodyOutcome::Refused;
	uint8_t why = 0; // journal::Why when refused
	bool by_acceptance = false;
	uint64_t crossed_ms = 0; // how long the run of crossed reports of this character has lasted (0: none or its first)
};

// The one decision on a State of a map server's journal (srv/coop_levels_xfer.cpp host_take_body): the acceptance, then the
// verdict on the body for a report the acceptance took or handed to his seat (body_report_by_seat), then the grace of a crossing
// (`run`: this character's run of crossed reports, `now` in ms).
inline BodyDecision body_report(const Facts& f, const BodyFacts& b, wire::BodyCrossing& run, uint64_t now)
{
	const Verdict v = accepts(f);
	if (!v.ok && !body_report_by_seat(f, v))
		return BodyDecision{BodyOutcome::Refused, v.why, true, 0};
	wire::BodyReport r = wire::body_report_verdict(b.read, b.known, b.online_here, b.player_character, b.body_level, b.sender_level,
	    b.seat_level, b.position_valid, v.ok);
	uint64_t crossed_ms = 0;
	if (r == wire::BodyReport::Crossed)
	{
		crossed_ms = wire::body_crossing_age(run, b.sender_level, b.seat_level, now);
		r = wire::body_crossing_verdict(crossed_ms); // a map server that never hears he left is at fault
	}
	switch (r)
	{
	case wire::BodyReport::Take: return BodyDecision{BodyOutcome::Taken, 0, false, 0};
	case wire::BodyReport::Crossed: return BodyDecision{BodyOutcome::Crossed, 0, false, crossed_ms};
	default: break;
	}
	return BodyDecision{BodyOutcome::Refused, uint8_t(journal::Refused), false, crossed_ms};
}
} // namespace accept
} // namespace levels
} // namespace coop
