// Anomaly Coop — one writer per object.
//
// The owner's rule as a contract: every object has exactly one writer. Only the writer's word changes it (creates under it,
// releases it, moves it, kills it, changes its state, its squad, its info portions, its Lua rows). Everybody else keeps a copy
// and asks. The writer is read from the object's root (root_of) and changes hands only at a fence (fence_admits).
//
// The pure part, header-only, no engine: the Core names a root's kind (kind_of) and asks Mode::writer_of(RootFacts)
// (xrCoop/coop_mode.h), each mode answers it with writer_of() below from the facts of its own tables (Seat: who leads a map, who
// holds a unit, whose move fence is open; the server's processes build it with world_seat / map_seat of
// xrCoop/srv/coop_writer_seats.h from their tables' answers); the Core's script gate asks gate(); the world's acceptance asks
// fence_admits() and same_object(); the module carry of a map server follows the carry rule. Pure C++17: used by the engine and
// by the unit-test exe (tests/test_writer_rules.cpp).
//
// Everything is in coop::writer: coop::Writer is the wire writer (coop_wire.h), so the writer's Writer lives here as
// coop::writer::Writer.
#pragma once

#include <cstdint>
#include <string>

#include "coop_ids.h"
#include "coop_protocol.h"

namespace coop
{
namespace writer
{
using coop::kNoLevel; // no map
using coop::kNoId; // no object: the top of a parent chain, no squad
constexpr uint16_t kGhostId = 0; // actor 0: the ghost where this process's actor plays nobody
constexpr int kRootHops = 8; // the parent chain is walked at most this far (a deeper chain is a broken registry)
constexpr uint32_t kNoFence = 0xffffffffu; // no fence: every entry of the sender is admitted

// ------------------------------------------------------------------------------------------------- the root

// What the root of an object is. The Core finds the root (root_of) and its kind (kind_of) from its ALife data:
//   Character  a player's character, and the ghost (actor 0 where the actor plays nobody);
//   Unit       a squad, or a living monster in no squad: what the world grants to a map server and takes back (xfer);
//   Resident   anything else that stands on a map: a box, a ground item, a body, a physics object, a zone, a car, a crow, a phantom;
//   None       an object of no map: money, story, tasks, records.
enum class RootKind : uint8_t
{
	Character,
	Unit,
	Resident,
	None,
};

// The root as the Core found it: its kind, its id (this process's numbering), the map it stands on (a character: the map he
// stands on or loads into); level: kNoLevel for none.
struct RootFacts
{
	RootKind kind;
	uint16_t root;
	uint16_t level;
};

// The root of `id`: ID_Parent walked to the top (at most kRootHops hops); a top that is a creature in a squad gives its
// squad (unit_of). parent_of(id) -> uint16_t: the parent, kNoId at the top or for an id that is not there; squad_of(id) ->
// uint16_t: the squad of a creature, kNoId for none.
template <class ParentOf, class SquadOf>
uint16_t root_of(uint16_t id, ParentOf parent_of, SquadOf squad_of)
{
	uint16_t top = id;
	for (int hop = 0; hop < kRootHops; ++hop)
	{
		const uint16_t up = parent_of(top);
		if (up == kNoId)
			break;
		top = up;
	}
	const uint16_t squad = squad_of(top);
	return squad == kNoId ? top : squad;
}

// ------------------------------------------------------------------------------------------------- the writer

// Who writes the object: this process, another process (peer: how this process names it), nobody (the ghost).
enum class WriterIs : uint8_t
{
	This,
	Other,
	Nobody,
};

// peer is the writer's peer as this process names it, for Other only (This, Nobody: kInvalidPeer):
//   the world server: the bridge of the map server that writes it;
//   a map server: its world (the host of its bridge), whoever writes it: it asks its world for everything it does not write;
//   a player's machine: the host of its session.
struct Writer
{
	WriterIs is;
	PeerId peer;
};

constexpr Writer this_process() { return {WriterIs::This, kInvalidPeer}; }
constexpr Writer other(PeerId peer) { return {WriterIs::Other, peer}; }
constexpr Writer nobody() { return {WriterIs::Nobody, kInvalidPeer}; }
constexpr bool operator==(const Writer& a, const Writer& b) { return a.is == b.is && a.peer == b.peer; }
constexpr bool operator!=(const Writer& a, const Writer& b) { return !(a == b); }

// This process as the writer rule and the gate see it, from the Mode's answers (process_kind):
//   Whole   writes its whole world: the co-op host (decides the world, leads its map), and no session (single player);
//   Mirror  a player's machine, both modes: its objects are copies of their writer's;
//   World   decides the world, leads no map: the world server;
//   Level   leads one map, decides no world: a map server.
enum class ProcessKind : uint8_t
{
	Whole,
	Mirror,
	World,
	Level,
};

// The kind from what this process answers: is_mirror(), Mode::decides_world(), Mode::leads_level().
constexpr ProcessKind process_kind(bool mirror, bool decides_world, bool leads_level)
{
	return mirror                          ? ProcessKind::Mirror
	    : decides_world && !leads_level    ? ProcessKind::World
	    : leads_level && !decides_world    ? ProcessKind::Level
	                                       : ProcessKind::Whole;
}

// This process: its kind, the map it leads (Level; kNoLevel for the others), the peer it names its world by (Mirror: the host
// of its session; Level: the host of its bridge to the world).
struct Process
{
	ProcessKind kind = ProcessKind::Whole;
	uint16_t level = kNoLevel;
	PeerId world = kHostPeer;
};

// The server of a map as this process knows it (the world server: its table of map servers; a map server: its own state for
// its own map, its world's LevelMap for the others).
//   None      no process (Off, Lost): the world writes the map's residents;
//   Starting  launched, loading its world (a save plus the tail): the world still writes them;
//   Leads     up: Ready, Serving, Draining (mapserver::host_ready): it writes them;
//   Stopping  its Stop order is out: it still writes them until its Stopped, after its journal is acknowledged.
enum class Lead : uint8_t
{
	None,
	Starting,
	Leads,
	Stopping,
};

// Does the server of a map in that state write the map's residents and its players' characters?
constexpr bool writes(Lead l) { return l == Lead::Leads || l == Lead::Stopping; }

// The Lead of a map server's life (levels::ServerState of coop_levels_rules.h: Off, Starting, Ready, Serving, Draining, Stopping,
// Lost), for a mode that keeps that state machine. A template: the rules here do not include the levels' header.
template <class State>
constexpr Lead lead_of(State s)
{
	return s == State::Starting                                                    ? Lead::Starting
	    : s == State::Ready || s == State::Serving || s == State::Draining          ? Lead::Leads
	    : s == State::Stopping                                                     ? Lead::Stopping
	                                                                               : Lead::None;
}

// A map's server and the peer that names it here (the world server: its bridge; elsewhere kInvalidPeer: not named).
struct MapLead
{
	Lead lead = Lead::None;
	PeerId peer = kInvalidPeer;
};

// How a unit is held:
//   Home  at home in the world: the world simulates it, also on a led map until it is granted;
//   Held  granted to a map server (the keeper has it away, the holder authors it);
//   Born  made by a map server under its own id slice: held by it from its birth (Holder::spawned -> SpawnPrepare), also
//         before the world answered.
enum class Hold : uint8_t
{
	Home,
	Held,
	Born,
};

// What this process's tables say of a root besides RootFacts: the mode fills it, the Core never does.
struct Seat
{
	MapLead map; // Character, Resident: the server of RootFacts::level
	uint16_t from = kNoLevel; // Character: the map he left while his move fence is not through; kNoLevel: no move
	MapLead from_map; // its server
	Hold hold = Hold::Home; // Unit
	uint16_t holder_level = kNoLevel; // Unit held or born: the map of the server that holds it
	PeerId holder = kInvalidPeer; // that server as this process names it (the world server: its bridge)
};

// The writer of a root, first match wins:
//   the ghost (a Character with id kGhostId where the actor plays nobody: World, Level)             nobody;
//   Whole                                                                                           this process;
//   Mirror                                                                                          its host;
//   a. a character: the server of the map he left while his move fence is open and it still writes, else the server of the map
//      he stands on or loads into while it writes, else the world;
//   b. a unit held or born: its holder, wherever it stands; at home: the world;
//   c. a resident: the server of its map while it writes, else the world;
//   d. anything else: the world.
// On the world server "the world" is this process and a map server is Other(its bridge); on a map server the server of its own
// map is this process and every other writer is Other(its world).
inline Writer writer_of(const RootFacts& root, const Process& self, const Seat& seat)
{
	switch (self.kind)
	{
	case ProcessKind::Whole: return this_process();
	case ProcessKind::Mirror: return other(self.world);
	case ProcessKind::World:
	case ProcessKind::Level: break;
	}
	if (root.kind == RootKind::Character && root.root == kGhostId)
		return nobody();

	// a map server writes the root (false: the world), the server of which map, and its peer here
	bool by_map = false;
	uint16_t level = kNoLevel;
	PeerId peer = kInvalidPeer;
	const auto map_writes = [&](uint16_t map, PeerId its_peer) {
		by_map = true;
		level = map;
		peer = its_peer;
	};
	switch (root.kind)
	{
	case RootKind::Character:
		if (seat.from != kNoLevel && writes(seat.from_map.lead))
			map_writes(seat.from, seat.from_map.peer);
		else if (root.level != kNoLevel && writes(seat.map.lead))
			map_writes(root.level, seat.map.peer);
		break;
	case RootKind::Unit:
		if (seat.hold != Hold::Home)
			map_writes(seat.holder_level, seat.holder);
		break;
	case RootKind::Resident:
		if (root.level != kNoLevel && writes(seat.map.lead))
			map_writes(root.level, seat.map.peer);
		break;
	case RootKind::None: break;
	}

	if (self.kind == ProcessKind::World)
		return by_map ? other(peer) : this_process();
	return by_map && level == self.level ? this_process() : other(self.world);
}

// ------------------------------------------------------------------------------------------------- the kind of a root

// What the Core reads of a root to name its kind (writing::root_facts_of from the ALife objects; the tests from rows).
struct KindFacts
{
	bool ghost; // actor 0 (kGhostId)
	bool player_character; // a player's character (section coop_player_avatar)
	bool group; // a squad (CSE_ALifeOnlineOfflineGroup)
	bool monster; // a CSE_ALifeMonsterAbstract: a stalker, a mutant (a crow, a phantom are creatures, no monsters)
	bool alive; // a creature that lives
	bool has_level; // it stands on a map
};

// A unit is what the world grants to a map server and takes back: a squad or a living monster, the predicate of the transfers
// (srv/coop_levels_alife.cpp levels::transfers). A crow or a phantom is a creature but no monster: the transfers never move it, only
// its map's server has it online, so it is that map's resident like a box (the world must not name itself its writer and refuse
// the map server's Died and Destroy of a crow).
constexpr bool is_unit(bool group, bool monster, bool alive)
{
	return group || (monster && alive);
}

// The kind of a root, first match wins: the ghost or a player's character (alive or dead); a unit (is_unit, wherever it stands);
// anything else on a map (a box, a ground item, a body, a car, a crow, a phantom); nothing of a map (money, story, records).
constexpr RootKind kind_of(const KindFacts& k)
{
	return k.ghost || k.player_character         ? RootKind::Character
	    : is_unit(k.group, k.monster, k.alive)   ? RootKind::Unit
	    : k.has_level                            ? RootKind::Resident
	                                             : RootKind::None;
}

// ------------------------------------------------------------------------------------------------- the gate

// What a script path does to its target: alife create / spawn_item / spawn_item2 / create_ammo / register / clone_weapon /
// level.spawn_item (Create), release (Release), a change of its state (State), give_info / disable_info (Info).
enum class WriteOp : uint8_t
{
	Create,
	Release,
	State,
	Info,
};

// The code that asks, as the router keeps it (coop_route.script guard, the timers it wraps; the engine next to runs_now):
//   LevelCode  the `level`, `owner` or `view` domain: it runs on the map server of the map as well;
//   Decision   the `host` domain, a request handler, the far view's scope, an engine apply;
//   Unknown    not routed: counts as a decision on the world server (smart respawns, death consequences, C++ to Lua paths).
enum class CodeScope : uint8_t
{
	LevelCode,
	Decision,
	Unknown,
};

// What the gate does with one script write:
//   Run     the stock code;
//   Mirror  a player's machine: today's verdict, unchanged (calls::decide);
//   Refuse  nothing: the stock "nothing" (nil, false), counted, one log line per script;
//   Ask     a map server: a journal entry to its world, which executes it if it writes the object; not applied here first;
//   Order   the world server: the numbered far order to the map server that writes it, held until answered (that server gone:
//           the world applies it itself, as far orders do).
enum class GateVerdict : uint8_t
{
	Run,
	Mirror,
	Refuse,
	Ask,
	Order,
};

// The gate table. `w`: the writer of the target's root (Mode::writer_of); `root_on_own_level`: that root stands on the map this
// process leads; `ghost_target`: the root is the ghost.
//   WS or Mk  | the ghost             | create, info                       Refuse, whatever `w` says
//   any       | This                  | -                                  Run
//   Whole     | not This              | -                                  Run (it writes everything: nobody to defer to)
//   Mirror    | not This              | -                                  Mirror
//   WS        | nobody (the ghost)    | release, state                     Run (the world keeps its ghost in its save)
//   WS        | a Mk                  | level code                         Refuse: that Mk runs its own copy of this code
//   WS        | a Mk                  | decision, unknown: create          Run (the spawn goes down)
//   WS        | a Mk                  | decision, unknown: release, state, info    Order
//   Mk        | not This, root on its own map   | release, info            Ask
//   Mk        | not This              | create, state, or any op on another map    Refuse
inline GateVerdict gate(ProcessKind p, Writer w, WriteOp op, CodeScope s, bool root_on_own_level, bool ghost_target)
{
	const bool create_or_give = op == WriteOp::Create || op == WriteOp::Info;
	if ((p == ProcessKind::World || p == ProcessKind::Level) && ghost_target && create_or_give)
		return GateVerdict::Refuse;
	if (w.is == WriterIs::This)
		return GateVerdict::Run;
	switch (p)
	{
	case ProcessKind::Whole: return GateVerdict::Run;
	case ProcessKind::Mirror: return GateVerdict::Mirror;
	case ProcessKind::World:
		if (w.is == WriterIs::Nobody)
			return create_or_give ? GateVerdict::Refuse : GateVerdict::Run;
		if (s == CodeScope::LevelCode)
			return GateVerdict::Refuse;
		return op == WriteOp::Create ? GateVerdict::Run : GateVerdict::Order;
	case ProcessKind::Level:
		return root_on_own_level && (op == WriteOp::Release || op == WriteOp::Info) ? GateVerdict::Ask : GateVerdict::Refuse;
	}
	return GateVerdict::Refuse;
}

// ------------------------------------------------------------------------------------------------- fences and identity

// Is an entry of the sender at `entry_epoch` (its journal's seq) before or at the sender's fence `fence_epoch` (the seq of its
// Flushed for that fence; kNoFence: none)? The old writer's facts after its fence are refused. Epochs count from 1 in each
// process's life and do not wrap within it (u32, one batch per frame).
inline bool fence_admits(uint32_t entry_epoch, uint32_t fence_epoch)
{
	return entry_epoch <= fence_epoch;
}

// Is the object a report names the object this process has under that id? Its section's crc and its keeper generation (0 for
// an object that is no unit) both match. A reused id (another section) or a unit granted again since (another generation) is
// another object.
inline bool same_object(uint16_t section_crc_here, uint16_t section_crc_report, uint32_t gen_here, uint32_t gen_report)
{
	return section_crc_here == section_crc_report && gen_here == gen_report;
}

// ------------------------------------------------------------------------------------------------- names (log, coop_status)

inline const char* writer_name(WriterIs w)
{
	switch (w)
	{
	case WriterIs::This: return "this";
	case WriterIs::Other: return "other";
	case WriterIs::Nobody: return "nobody";
	}
	return "nobody";
}

inline const char* verdict_name(GateVerdict v)
{
	switch (v)
	{
	case GateVerdict::Run: return "run";
	case GateVerdict::Mirror: return "mirror";
	case GateVerdict::Refuse: return "refuse";
	case GateVerdict::Ask: return "ask";
	case GateVerdict::Order: return "order";
	}
	return "refuse";
}

inline const char* op_name(WriteOp op)
{
	switch (op)
	{
	case WriteOp::Create: return "create";
	case WriteOp::Release: return "release";
	case WriteOp::State: return "state";
	case WriteOp::Info: return "info";
	}
	return "state";
}

inline const char* kind_name(ProcessKind k)
{
	switch (k)
	{
	case ProcessKind::Whole: return "whole";
	case ProcessKind::Mirror: return "mirror";
	case ProcessKind::World: return "world";
	case ProcessKind::Level: return "level";
	}
	return "whole";
}
} // namespace writer
} // namespace coop
