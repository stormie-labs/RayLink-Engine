// Anomaly Coop — the wire layouts of the F3a messages (split levels; docs/plans/2026-10-24-f3a-split-levels.md §2.1,
// protocol 11, Msg 0x01D0-0x01DF). One struct per message with write() and read(): read() takes nothing it cannot bound — a
// count over its limit, an id or an enum out of range, a float that is not finite or beyond any level, a string that is not a
// name, an id listed twice, a cut packet — and returns false with the message emptied; a message is read whole, and a packet
// with bytes left over is the caller's to refuse (read_all in the tests).
//
// A writer never turns a message that is over a limit into one that reads: the formats with a limit (LevelMap, LevelXferUp and
// Down, LevelBrain, Tunnel, LoadWait) have `bool fits() const` and `[[nodiscard]] bool write(Writer&) const`, which returns false
// and writes NOTHING (the Writer is as it was) for a message over a limit — a cargo of 32 KiB + 1, a ninth player, 256 squads,
// a packet of 65536 bytes. Cutting it down instead would deliver a partial Grant or a map without a player as a whole message.
// The caller sends such a message another way (a blob for a big cargo, two packets for the squads) or not at all. The fixed
// layouts have no limit to break and write() is void. A message within its limits is written and read back whole.
//
// Numbers of levels are game graph level ids (u16 on the wire, kNoLevel = none), players are session slots (PeerId < kMaxPlayers,
// the host is 0), objects are ids in the world server's numbering. The enums are numbered here, in the order of the plan's
// table where it lists names alone; the logic that sends and handles them keeps its own types (coop_xfer.h, coop_levels_rules.h,
// coop_load_barrier.h) and converts at the engine's edge.
//
// Three formats carry the layout of an older message after a short header: LevelSnapshot (EntitySnapshot), LevelEntity (Entity*
// or ItemState) and the packet of a Tunnel. Their *Head structs read the header off the front of the Reader and leave the body
// to its own reader.
//
// Pure C++17, header-only, no engine dependencies: used by the engine and by the unit-test exe.
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "coop_levels_rules.h" // Gate: the byte of a LevelMap gate row
#include "coop_protocol.h"
#include "coop_wire.h"

namespace coop
{
namespace levels
{
namespace wire
{
constexpr uint16_t kNoLevel = 0xFFFF; // no level: a player still loading, an empty field
constexpr uint16_t kNoObject = 0xFFFF; // no object id (the ids of the world server are u16)
constexpr uint32_t kNoNode = 0xFFFFFFFFu; // no level vertex

// A full state of a level goes as a blob of this kind (LevelSnapshot with round > 0). Kind 1 is the world save
// (coop_join.h BlobWorldSave); 2 is left for the prepared save of a simultaneous load (T8).
constexpr uint32_t kBlobLevelState = 3;

constexpr size_t kMaxMapPlayers = kMaxPlayers; // a row per player in the session
constexpr size_t kMaxMapServers = 8; // the host's process and at most 7 map servers (coop_max_map_servers is 0..7)
constexpr size_t kMaxMapGates = 128; // levels with a gate in one table
constexpr size_t kMaxGateReason = 64; // a string id of the placard
constexpr size_t kMaxXferCargo = 32 * 1024; // what goes in one packet; a bigger cargo goes as a blob
constexpr size_t kMaxScripted = 64; // the scheme or section name of a squad
constexpr size_t kHashBuckets = 32; // LevelHash buckets, LevelResync mask bits
constexpr size_t kMaxTunnelPacket = 0xFFFF; // a u16 length
constexpr size_t kMaxBrainRows = 255; // squads, and assignments, in one LevelBrain (a u8 count each)
constexpr float kMaxCoord = 1.0e6f; // meters: beyond any level, finite

// The last gate a LevelMap row may carry: T3's Gate (coop_levels_rules.h: Free 0, BetterTogether 1, Together 2) is the enum of
// that byte. A Gate added there is added here.
constexpr uint8_t kLastGate = uint8_t(levels::Gate::Together);

// State of a server, shared by LevelMap and MapServerState (order of the plan's table).
enum class ServerState : uint8_t
{
	Loading = 0,
	Ready = 1,
	Draining = 2,
	Stopped = 3,
};
constexpr uint8_t kLastServerState = uint8_t(ServerState::Stopped);

namespace detail
{
inline bool finite_coord(float v)
{
	return std::isfinite(v) && std::fabs(v) <= kMaxCoord;
}

// A name the placard or a scheme uses: printable ASCII.
inline bool printable(const std::string& s, size_t max)
{
	if (s.size() > max)
		return false;
	for (const char ch : s)
	{
		const unsigned char c = static_cast<unsigned char>(ch);
		if (c < 0x20 || c > 0x7E)
			return false;
	}
	return true;
}

inline bool slot_ok(uint32_t peer)
{
	return peer < kMaxPlayers;
}

// Reads the `n` bytes left in `r` into `out` (the free tail of a message), at most `max`.
inline bool read_tail(Reader& r, std::vector<uint8_t>& out, size_t max)
{
	out.clear();
	const size_t left = r.remaining();
	if (!r.ok() || left > max)
		return false;
	out.resize(left);
	return left == 0 || r.bytes(out.data(), left);
}
} // namespace detail

// ---------------------------------------------------------------- LevelMap (0x01D0, world server -> players and map servers)

// Wire: u8 flags, u8 n, n x {u32 peer, u16 level, u8 role}, u8 m, m x {u16 level, u8 server, u8 state},
//       u8 g, g x {u16 level, u8 gate, str reason}.
// A player row says on which level a player stands (kNoLevel while he loads); `role` is the Hello role of the node (0 a player,
// 1 a map server). A server row says who leads a level: the host's process (at most one) or a map server, and its state. A gate
// row is the world server's verdict on entering a level (the Gate of coop_levels_rules.h: 0 .. kLastGate) and the string id of
// the placard that says why; the game sends the levels whose gate is not open.
struct LevelMapMsg
{
	enum Flags : uint8_t
	{
		Split = 1, // split levels are on
		StoryTogether = 2, // the story levels are entered together
		Known = Split | StoryTogether,
	};
	enum ServerKind : uint8_t
	{
		HostProcess = 0,
		MapServerProcess = 1,
	};
	struct Player
	{
		uint32_t peer = 0;
		uint16_t level = kNoLevel;
		uint8_t role = kHelloRolePlayer;
	};
	struct Server
	{
		uint16_t level = kNoLevel;
		uint8_t server = HostProcess;
		uint8_t state = uint8_t(ServerState::Ready);
	};
	struct Gate
	{
		uint16_t level = kNoLevel;
		uint8_t gate = 0;
		std::string reason;
	};

	uint8_t flags = 0;
	std::vector<Player> players;
	std::vector<Server> servers;
	std::vector<Gate> gates;

	// The three lists are within their limits (a player a row at the most, 8 servers, 128 gates).
	bool fits() const
	{
		return players.size() <= kMaxMapPlayers && servers.size() <= kMaxMapServers && gates.size() <= kMaxMapGates;
	}

	// False, nothing written, for a map over a limit.
	[[nodiscard]] bool write(Writer& w) const
	{
		if (!fits())
			return false;
		w.u8(flags);
		w.u8(uint8_t(players.size()));
		for (const Player& p : players)
		{
			w.u32(p.peer);
			w.u16(p.level);
			w.u8(p.role);
		}
		w.u8(uint8_t(servers.size()));
		for (const Server& sv : servers)
		{
			w.u16(sv.level);
			w.u8(sv.server);
			w.u8(sv.state);
		}
		w.u8(uint8_t(gates.size()));
		for (const Gate& g : gates)
		{
			w.u16(g.level);
			w.u8(g.gate);
			w.str(g.reason);
		}
		return true;
	}

	bool read(Reader& r)
	{
		*this = LevelMapMsg();
		LevelMapMsg tmp;
		tmp.flags = r.u8();
		const uint8_t n = r.u8();
		if (!r.ok() || (tmp.flags & ~uint8_t(Known)) != 0 || n > kMaxMapPlayers)
			return false;
		for (uint8_t i = 0; i < n; ++i)
		{
			Player p;
			p.peer = r.u32();
			p.level = r.u16();
			p.role = r.u8();
			if (!r.ok() || !detail::slot_ok(p.peer) || p.role > kHelloRoleMapServer)
				return false;
			for (const Player& seen : tmp.players)
				if (seen.peer == p.peer)
					return false;
			tmp.players.push_back(p);
		}
		const uint8_t m = r.u8();
		if (!r.ok() || m > kMaxMapServers)
			return false;
		size_t host_processes = 0;
		for (uint8_t i = 0; i < m; ++i)
		{
			Server s;
			s.level = r.u16();
			s.server = r.u8();
			s.state = r.u8();
			if (!r.ok() || s.level == kNoLevel || s.server > MapServerProcess || s.state > kLastServerState)
				return false;
			for (const Server& seen : tmp.servers)
				if (seen.level == s.level)
					return false;
			host_processes += s.server == HostProcess ? 1 : 0;
			if (host_processes > 1)
				return false; // one process leads its level for the world server
			tmp.servers.push_back(s);
		}
		const uint8_t g = r.u8();
		if (!r.ok() || g > kMaxMapGates)
			return false;
		for (uint8_t i = 0; i < g; ++i)
		{
			Gate gate;
			gate.level = r.u16();
			gate.gate = r.u8();
			gate.reason = r.str();
			if (!r.ok() || gate.level == kNoLevel || gate.gate > kLastGate ||
			    !(gate.reason.empty() || char_name_valid(gate.reason, kMaxGateReason)))
				return false;
			for (const Gate& seen : tmp.gates)
				if (seen.level == gate.level)
					return false;
			tmp.gates.push_back(std::move(gate));
		}
		*this = std::move(tmp);
		return true;
	}
};

// ---------------------------------------------------------------- LevelMove (0x01D1, player -> world server)

// Wire: u32 seq, u8 kind, Exit: u16 changer; Trip: u16 graph, u32 node, vec3 pos, vec3 dir.
// A player leaves his level: through an exit (the level changer, its id in the world server's numbering) or on a trip (a
// guide's, a fast travel, a story ChangeLevel: the game vertex, the level vertex and the point of arrival). `seq` is answered
// by LevelMoveResult.
struct LevelMoveMsg
{
	enum Kind : uint8_t
	{
		Exit = 1,
		Trip = 2,
	};

	uint32_t seq = 0;
	uint8_t kind = Exit;
	uint16_t changer = kNoObject; // Exit
	uint16_t graph = kNoObject; // Trip: the game graph vertex
	uint32_t node = kNoNode; // Trip: the level vertex
	float pos[3] = {0.f, 0.f, 0.f}; // Trip
	float dir[3] = {0.f, 0.f, 0.f}; // Trip

	void write(Writer& w) const
	{
		w.u32(seq);
		w.u8(kind);
		if (kind == Exit)
			w.u16(changer);
		else
		{
			w.u16(graph);
			w.u32(node);
			w.vec3(pos);
			w.vec3(dir);
		}
	}

	bool read(Reader& r)
	{
		*this = LevelMoveMsg();
		LevelMoveMsg tmp;
		tmp.seq = r.u32();
		tmp.kind = r.u8();
		if (!r.ok() || (tmp.kind != Exit && tmp.kind != Trip))
			return false;
		if (tmp.kind == Exit)
		{
			tmp.changer = r.u16();
			if (!r.ok() || tmp.changer == kNoObject)
				return false;
		}
		else
		{
			tmp.graph = r.u16();
			tmp.node = r.u32();
			r.vec3(tmp.pos);
			r.vec3(tmp.dir);
			if (!r.ok() || tmp.graph == kNoObject || tmp.node == kNoNode)
				return false;
			for (int i = 0; i < 3; ++i)
				if (!detail::finite_coord(tmp.pos[i]) || !detail::finite_coord(tmp.dir[i]))
					return false;
		}
		*this = tmp;
		return true;
	}
};

// ---------------------------------------------------------------- LevelMoveResult (0x01D2, world server -> player)

// Wire: u32 seq, u8 verdict, u8 reason, u16 level.
// Go: the world server moves him now; Together: the level is entered together, he waits at its board; Refused: not now, and
// `reason` says why (Cap: the machine's limit of map servers, Gate: the level is closed to him, Invalid: not an exit he stands
// at). `level` is the level the answer is about. A verdict and its reason are one story: Go and Together have reason None,
// Refused always has one; read() refuses the other pairs.
struct LevelMoveResultMsg
{
	enum Verdict : uint8_t
	{
		Go = 0,
		Together = 1,
		Refused = 2,
	};
	enum Reason : uint8_t
	{
		None = 0,
		Cap = 1,
		Gate = 2,
		Invalid = 3,
	};

	uint32_t seq = 0;
	uint8_t verdict = Go;
	uint8_t reason = None;
	uint16_t level = kNoLevel;

	void write(Writer& w) const
	{
		w.u32(seq);
		w.u8(verdict);
		w.u8(reason);
		w.u16(level);
	}

	bool read(Reader& r)
	{
		seq = r.u32();
		verdict = r.u8();
		reason = r.u8();
		level = r.u16();
		if (!r.ok() || verdict > Refused || reason > Invalid || (verdict == Refused) != (reason != None))
		{
			*this = LevelMoveResultMsg();
			return false;
		}
		return true;
	}
};

// ---------------------------------------------------------------- LevelFlush (0x01D3, world server -> map server)

// Wire: u32 round, u8 why. Send the full state of your level (LevelSnapshot with this round), `why` is what it is for.
struct LevelFlushMsg
{
	enum Why : uint8_t
	{
		Save = 1,
		Move = 2,
		Stop = 3,
		HostMove = 4,
	};

	uint32_t round = 0;
	uint8_t why = Save;

	void write(Writer& w) const
	{
		w.u32(round);
		w.u8(why);
	}

	bool read(Reader& r)
	{
		round = r.u32();
		why = r.u8();
		if (!r.ok() || why < Save || why > HostMove)
		{
			*this = LevelFlushMsg();
			return false;
		}
		return true;
	}
};

// ---------------------------------------------------------------- LevelSnapshot (0x01D4), LevelEntity (0x01D5), map server -> world server

// LevelSnapshot wire: u16 level, u32 round, then the layout of EntitySnapshot. Round 0 is the stream; a round > 0 answers a
// LevelFlush and its full state goes as a blob of kind kBlobLevelState. Reads the header; the snapshot follows.
struct LevelSnapshotHead
{
	uint16_t level = kNoLevel;
	uint32_t round = 0;

	void write(Writer& w) const
	{
		w.u16(level);
		w.u32(round);
	}

	bool read(Reader& r)
	{
		level = r.u16();
		round = r.u32();
		if (!r.ok() || level == kNoLevel)
		{
			*this = LevelSnapshotHead();
			return false;
		}
		return true;
	}
};

// LevelEntity wire: u8 op, then the layout of the Entity* message or ItemState that the op names. Reads the op; the body follows.
struct LevelEntityHead
{
	enum Op : uint8_t
	{
		Destroy = 0,
		Parent = 1,
		Died = 2,
		State = 3,
		Item = 4,
	};

	uint8_t op = Destroy;

	void write(Writer& w) const { w.u8(op); }

	bool read(Reader& r)
	{
		op = r.u8();
		if (!r.ok() || op > Item)
		{
			*this = LevelEntityHead();
			return false;
		}
		return true;
	}
};

// ---------------------------------------------------------------- LevelXferUp (0x01D6), LevelXferDown (0x01D7)

// Wire: u8 op, u16 id, u32 gen, then the cargo: the rest of the packet, at most kMaxXferCargo (a bigger one goes as a blob). The
// ops are the two halves of the two-phase transfer of coop_xfer.h (xfer::Op, one wire value for each op the core sends in that
// direction: levels_wire_xfer_ops_cover_the_core); which op carries a cargo is the logic's rule, not the wire's.
//   LevelXferUp   (the Holder, map server -> world server)  TakeReq, TakeCommit, ReleasePrepare, ReleaseCommit, SpawnPrepare,
//                                                            Destroy, AbortAck, Snapshot (the stream: cargo = seq + state)
//   LevelXferDown (the Ledger, world server -> map server)  Grant, Ack, Done, Abort, Drop, Snapshot (the ack of the stream and
//                                                            the auditor's resync request: cargo = seq)
// `id` is the root of the transferred unit in the world server's numbering, `gen` the generation of the transfer.
struct LevelXferUpMsg
{
	enum Op : uint8_t
	{
		TakeReq = 0,
		TakeCommit = 1,
		ReleasePrepare = 2,
		ReleaseCommit = 3,
		SpawnPrepare = 4,
		Destroy = 5,
		AbortAck = 6,
		Snapshot = 7,
	};

	uint8_t op = TakeReq;
	uint16_t id = kNoObject;
	uint32_t gen = 0;
	std::vector<uint8_t> cargo;

	// The cargo fits a packet (kMaxXferCargo); a bigger one goes as a blob.
	bool fits() const { return cargo.size() <= kMaxXferCargo; }

	// False, nothing written, for a cargo over the limit: a cargo cut to the limit would be applied as the whole object.
	[[nodiscard]] bool write(Writer& w) const
	{
		if (!fits())
			return false;
		w.u8(op);
		w.u16(id);
		w.u32(gen);
		if (!cargo.empty())
			w.bytes(cargo.data(), cargo.size());
		return true;
	}

	bool read(Reader& r)
	{
		op = r.u8();
		id = r.u16();
		gen = r.u32();
		if (!r.ok() || op > Snapshot || id == kNoObject || !detail::read_tail(r, cargo, kMaxXferCargo))
		{
			*this = LevelXferUpMsg();
			return false;
		}
		return true;
	}
};

struct LevelXferDownMsg
{
	enum Op : uint8_t
	{
		Grant = 0,
		Ack = 1,
		Done = 2,
		Abort = 3,
		Drop = 4,
		Snapshot = 5, // the ack of a snapshot stream (cargo: its sequence number) and the auditor's resync request
	};

	uint8_t op = Grant;
	uint16_t id = kNoObject;
	uint32_t gen = 0;
	std::vector<uint8_t> cargo; // Grant

	// The cargo fits a packet (kMaxXferCargo); a bigger one goes as a blob.
	bool fits() const { return cargo.size() <= kMaxXferCargo; }

	// False, nothing written, for a cargo over the limit: a cargo cut to the limit would be applied as the whole object.
	[[nodiscard]] bool write(Writer& w) const
	{
		if (!fits())
			return false;
		w.u8(op);
		w.u16(id);
		w.u32(gen);
		if (!cargo.empty())
			w.bytes(cargo.data(), cargo.size());
		return true;
	}

	bool read(Reader& r)
	{
		op = r.u8();
		id = r.u16();
		gen = r.u32();
		if (!r.ok() || op > Snapshot || id == kNoObject || !detail::read_tail(r, cargo, kMaxXferCargo))
		{
			*this = LevelXferDownMsg();
			return false;
		}
		return true;
	}
};

// ---------------------------------------------------------------- LevelBrain (0x01D8, world server -> map server)

// Wire: u8 s, s x {u16 squad, u16 assigned, u16 current, u8 action, u32 stay_ms, str scripted}, u8 a, a x {u16 squad, u16 smart,
// u16 old_smart}. The world's brain tells the squads that are online at the map server where to go (assigned smart, the smart
// they stand in, the action and how long to stay, the scripted scheme if any) and moves squads between smarts (old -> new).
struct LevelBrainMsg
{
	struct Squad
	{
		uint16_t squad = 0;
		uint16_t assigned = 0xFFFF;
		uint16_t current = 0xFFFF;
		uint8_t action = 0;
		uint32_t stay_ms = 0;
		std::string scripted;
	};
	struct Assign
	{
		uint16_t squad = 0;
		uint16_t smart = 0xFFFF;
		uint16_t old_smart = 0xFFFF;
	};

	std::vector<Squad> squads; // at most kMaxBrainRows
	std::vector<Assign> assigns; // at most kMaxBrainRows

	// At most kMaxBrainRows squads and as many assignments; the sender splits more over several packets.
	bool fits() const { return squads.size() <= kMaxBrainRows && assigns.size() <= kMaxBrainRows; }

	// False, nothing written, for more rows than that: the 256th squad would be one that is never told where to go.
	[[nodiscard]] bool write(Writer& w) const
	{
		if (!fits())
			return false;
		w.u8(uint8_t(squads.size()));
		for (const Squad& q : squads)
		{
			w.u16(q.squad);
			w.u16(q.assigned);
			w.u16(q.current);
			w.u8(q.action);
			w.u32(q.stay_ms);
			w.str(q.scripted);
		}
		w.u8(uint8_t(assigns.size()));
		for (const Assign& a : assigns)
		{
			w.u16(a.squad);
			w.u16(a.smart);
			w.u16(a.old_smart);
		}
		return true;
	}

	bool read(Reader& r)
	{
		*this = LevelBrainMsg();
		LevelBrainMsg tmp;
		const uint8_t s = r.u8();
		if (!r.ok())
			return false;
		for (uint8_t i = 0; i < s; ++i)
		{
			Squad q;
			q.squad = r.u16();
			q.assigned = r.u16();
			q.current = r.u16();
			q.action = r.u8();
			q.stay_ms = r.u32();
			q.scripted = r.str();
			if (!r.ok() || !detail::printable(q.scripted, kMaxScripted))
				return false;
			tmp.squads.push_back(std::move(q));
		}
		const uint8_t a = r.u8();
		if (!r.ok())
			return false;
		for (uint8_t i = 0; i < a; ++i)
		{
			Assign as;
			as.squad = r.u16();
			as.smart = r.u16();
			as.old_smart = r.u16();
			if (!r.ok())
				return false;
			tmp.assigns.push_back(as);
		}
		*this = std::move(tmp);
		return true;
	}
};

// ---------------------------------------------------------------- LevelHash (0x01D9), LevelResync (0x01DA)

// LevelHash wire: u16 level, u32 n, u64 set, 32 x u32. The auditor's view of a map server's objects: how many (n), a hash of the
// set of their ids, and a hash for each of 32 buckets of (id, gen, parent, alive) (coop_level_hash.h).
struct LevelHashMsg
{
	uint16_t level = kNoLevel;
	uint32_t n = 0;
	uint64_t set = 0;
	uint32_t bucket[kHashBuckets] = {};

	void write(Writer& w) const
	{
		w.u16(level);
		w.u32(n);
		w.u64(set);
		for (size_t i = 0; i < kHashBuckets; ++i)
			w.u32(bucket[i]);
	}

	bool read(Reader& r)
	{
		level = r.u16();
		n = r.u32();
		set = r.u64();
		for (size_t i = 0; i < kHashBuckets; ++i)
			bucket[i] = r.u32();
		if (!r.ok() || level == kNoLevel)
		{
			*this = LevelHashMsg();
			return false;
		}
		return true;
	}
};

// LevelResync wire: u8 mask[4]. Send the full state of the buckets whose bit is set; bucket i is bit (i % 8) of byte i / 8,
// which is bit i of a little-endian u32.
struct LevelResyncMsg
{
	uint32_t mask = 0;

	bool wants(size_t bucket) const { return bucket < kHashBuckets && ((mask >> bucket) & 1u) != 0; }

	void write(Writer& w) const
	{
		for (int i = 0; i < 4; ++i)
			w.u8(uint8_t(mask >> (8 * i)));
	}

	bool read(Reader& r)
	{
		mask = 0;
		for (int i = 0; i < 4; ++i)
			mask |= uint32_t(r.u8()) << (8 * i);
		if (!r.ok())
		{
			mask = 0;
			return false;
		}
		return true;
	}
};

// ---------------------------------------------------------------- Tunnel (0x01DB, host's process <-> map server)

// Wire: u32 peer, u8 flags, u16 len, packet[len]. The packet is a whole game message (its 2-byte type first) of a player of the
// map server's level: toward the map server it is what the player sent, toward the host's process what the map server sends to
// him (flag AllOnMap: to every player of the map, `peer` unused). `peer` is a session slot. A Tunnel carries a game message
// only: not a session message and not another Tunnel. The length is the rest of the packet.
struct TunnelMsg
{
	enum Flags : uint8_t
	{
		AllOnMap = 1,
		Known = AllOnMap,
	};

	uint32_t peer = 0;
	uint8_t flags = 0;
	std::vector<uint8_t> packet;

	// The type of the carried message (only after a read, or for a packet of at least two bytes).
	Msg type() const
	{
		return packet.size() >= 2 ? static_cast<Msg>(uint16_t(packet[0] | (packet[1] << 8))) : static_cast<Msg>(0);
	}

	// The packet is what a u16 length can name.
	bool fits() const { return packet.size() <= kMaxTunnelPacket; }

	// False, nothing written, for a longer packet: a packet cut to 65535 bytes would reach its handler as a whole message.
	[[nodiscard]] bool write(Writer& w) const
	{
		if (!fits())
			return false;
		w.u32(peer);
		w.u8(flags);
		w.u16(uint16_t(packet.size()));
		if (!packet.empty())
			w.bytes(packet.data(), packet.size());
		return true;
	}

	bool read(Reader& r)
	{
		peer = r.u32();
		flags = r.u8();
		const uint16_t len = r.u16();
		packet.clear();
		// the peer is a session slot, unless the packet is for everybody on the map (then it is unused)
		const bool peer_ok = (flags & AllOnMap) != 0 || detail::slot_ok(peer);
		if (!r.ok() || (flags & ~uint8_t(Known)) != 0 || !peer_ok || len < 2 || r.remaining() != len)
		{
			*this = TunnelMsg();
			return false;
		}
		packet.resize(len);
		if (!r.bytes(packet.data(), len))
		{
			*this = TunnelMsg();
			return false;
		}
		const uint16_t inner = uint16_t(type());
		if (inner < uint16_t(Msg::GameBase) || inner == uint16_t(Msg::Tunnel))
		{
			*this = TunnelMsg();
			return false;
		}
		return true;
	}
};

// ---------------------------------------------------------------- MapServerState (0x01DC), MapServerOrder (0x01DD)

// MapServerState wire: u8 state, u16 level, u32 frame_avg_ms, u32 frame_max_ms, u32 online, u32 mem_mb. What a map server tells
// the world server about itself: where it is in its life, the frame times of the last period, the objects it holds online, its
// private memory.
struct MapServerStateMsg
{
	uint8_t state = uint8_t(ServerState::Loading);
	uint16_t level = kNoLevel;
	uint32_t frame_avg_ms = 0;
	uint32_t frame_max_ms = 0;
	uint32_t online = 0;
	uint32_t mem_mb = 0;

	void write(Writer& w) const
	{
		w.u8(state);
		w.u16(level);
		w.u32(frame_avg_ms);
		w.u32(frame_max_ms);
		w.u32(online);
		w.u32(mem_mb);
	}

	bool read(Reader& r)
	{
		state = r.u8();
		level = r.u16();
		frame_avg_ms = r.u32();
		frame_max_ms = r.u32();
		online = r.u32();
		mem_mb = r.u32();
		if (!r.ok() || state > kLastServerState || level == kNoLevel)
		{
			*this = MapServerStateMsg();
			return false;
		}
		return true;
	}
};

// MapServerOrder wire: u8 order, u16 level. Serve the level; drain it (hand everything back, take no player); stop.
struct MapServerOrderMsg
{
	enum Order : uint8_t
	{
		Serve = 0,
		Drain = 1,
		Stop = 2,
	};

	uint8_t order = Serve;
	uint16_t level = kNoLevel;

	void write(Writer& w) const
	{
		w.u8(order);
		w.u16(level);
	}

	bool read(Reader& r)
	{
		order = r.u8();
		level = r.u16();
		if (!r.ok() || order > Stop || level == kNoLevel)
		{
			*this = MapServerOrderMsg();
			return false;
		}
		return true;
	}
};

// ---------------------------------------------------------------- LoadWait (0x01DE), LoadReady (0x01DF)

// LoadWait wire: u8 state, u32 epoch, u16 level, u8 n, n x {u32 peer, u8 ready}. The barrier of a simultaneous load, as the world
// server tells a player on his loading screen: Prepared (the world of this epoch is ready to be loaded: the save follows as a
// blob), Waiting (who is ready now), Enter (everybody is: leave the screen), Cancel (the load is off). `level` is the level
// of the epoch.
struct LoadWaitMsg
{
	enum State : uint8_t
	{
		Prepared = 0,
		Waiting = 1,
		Enter = 2,
		Cancel = 3,
	};
	struct Row
	{
		uint32_t peer = 0;
		uint8_t ready = 0;
	};

	uint8_t state = Prepared;
	uint32_t epoch = 0;
	uint16_t level = kNoLevel;
	std::vector<Row> rows;

	// A row for each player of the session at the most.
	bool fits() const { return rows.size() <= kMaxPlayers; }

	// False, nothing written, for more rows: a player cut away would be taken for one nobody waits for.
	[[nodiscard]] bool write(Writer& w) const
	{
		if (!fits())
			return false;
		w.u8(state);
		w.u32(epoch);
		w.u16(level);
		w.u8(uint8_t(rows.size()));
		for (const Row& row : rows)
		{
			w.u32(row.peer);
			w.u8(row.ready);
		}
		return true;
	}

	bool read(Reader& r)
	{
		*this = LoadWaitMsg();
		LoadWaitMsg tmp;
		tmp.state = r.u8();
		tmp.epoch = r.u32();
		tmp.level = r.u16();
		const uint8_t n = r.u8();
		if (!r.ok() || tmp.state > Cancel || n > kMaxPlayers)
			return false;
		for (uint8_t i = 0; i < n; ++i)
		{
			Row row;
			row.peer = r.u32();
			row.ready = r.u8();
			if (!r.ok() || !detail::slot_ok(row.peer) || row.ready > 1)
				return false;
			for (const Row& seen : tmp.rows)
				if (seen.peer == row.peer)
					return false;
			tmp.rows.push_back(row);
		}
		*this = std::move(tmp);
		return true;
	}
};

// LoadReady wire: u32 epoch. The player's loading screen of this epoch is up (and the world loaded under it).
struct LoadReadyMsg
{
	uint32_t epoch = 0;

	void write(Writer& w) const { w.u32(epoch); }

	bool read(Reader& r)
	{
		epoch = r.u32();
		if (!r.ok())
		{
			epoch = 0;
			return false;
		}
		return true;
	}
};
} // namespace wire
} // namespace levels
} // namespace coop
