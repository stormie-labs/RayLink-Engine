// Anomaly Coop — the wire layouts of the F3a messages (split levels; docs/plans/2026-10-24-f3a-split-levels.md §2.1,
// protocol 11, Msg 0x01D0-0x01DF) and of the journal of a map server (TZ R32, protocol 34, Msg 0x01E0-0x01E1: LevelJournal,
// LevelJournalAck, the bodies of namespace journal). One struct per message with write() and read(): read() takes nothing it cannot bound — a
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
// Numbers of levels are game graph level ids (u16 on the wire, kNoLevel = none), players are session slots (PeerId < kMaxPeerSlots,
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
#include <string_view>
#include <utility>
#include <vector>

#include "../coop_checkpoint_base_rules.h" // Build 57 (owner 2026-11-18): Footing, the tail of LevelBodyFacts
#include "../coop_levels_rules.h" // Gate: the byte of a LevelMap gate row
#include "../coop_protocol.h"
#include "../coop_wire.h"

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

constexpr size_t kMaxMapPlayers = kMaxPeerSlots; // a row per player slot of the session (seat 0 and clients 1 .. 16, F5-SEAT-1)
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
	return peer < kMaxPeerSlots;
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

// Wire: u8 flags, u8 n, n x {u32 peer, u16 level, u8 in_world, u16 proxy, u32 money}, u8 m, m x {u16 level, u8 server, u8 state},
//       u8 g, g x {u16 level, u8 gate, str reason}.
// A player row says on which level a player stands or is on his way to (kNoLevel: on none), whether he finished loading it and
// which character he plays and what his balance is (protocol 20: a map server takes its virtual peers from these rows, and judges
// a trade by that balance: the book is the world server's). A server row says that a map server leads a level, and its state. A
// gate row is the world server's verdict on entering a level (the Gate of coop_levels_rules.h: 0 .. kLastGate) and the string id
// of the placard that says why; the game sends the levels whose gate is not open.
// Reserved since build 57 (the hybrid is gone, owner 2026-11-18): flag bit 2 (the story levels entered together) and server kind 0
// (the host's own process). Nothing writes them; a map with either is refused.
struct LevelMapMsg
{
	enum Flags : uint8_t
	{
		Split = 1, // split levels are on
		Known = Split,
	};
	enum ServerKind : uint8_t
	{
		MapServerProcess = 1,
	};
	struct Player
	{
		uint32_t peer = 0;
		uint16_t level = kNoLevel;
		uint8_t in_world = 0;
		uint16_t proxy = kNoObject;
		uint32_t money = 0;
	};
	struct Server
	{
		uint16_t level = kNoLevel;
		uint8_t server = MapServerProcess;
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
			w.u8(p.in_world);
			w.u16(p.proxy);
			w.u32(p.money);
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
			p.in_world = r.u8();
			p.proxy = r.u16();
			p.money = r.u32();
			if (!r.ok() || !detail::slot_ok(p.peer) || p.in_world > 1)
				return false;
			for (const Player& seen : tmp.players)
				if (seen.peer == p.peer)
					return false;
			tmp.players.push_back(p);
		}
		const uint8_t m = r.u8();
		if (!r.ok() || m > kMaxMapServers)
			return false;
		for (uint8_t i = 0; i < m; ++i)
		{
			Server s;
			s.level = r.u16();
			s.server = r.u8();
			s.state = r.u8();
			if (!r.ok() || s.level == kNoLevel || s.server != MapServerProcess || s.state > kLastServerState)
				return false;
			for (const Server& seen : tmp.servers)
				if (seen.level == s.level)
					return false;
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
	uint32_t node = kNoNode; // Trip: the level vertex; kNoNode: the sender could not tell, a vertex of another level
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
			if (!r.ok() || tmp.graph == kNoObject)
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
// The ops are also those of a LevelJournal entry (protocol 34): Destroy .. Logic but Report, and the journal's own ops after Logic
// (Joined .. Flushed, section 5 of temp/r30/design/R32-final.md). A LevelEntity carries Destroy .. Logic alone: read() refuses the
// journal's ops (they need the batch's order and its answer).
struct LevelEntityHead
{
	enum Op : uint8_t
	{
		Destroy = 0,
		Parent = 1,
		Died = 2,
		State = 3,
		Item = 4, // the wear its level gave loot (ground, a body, a box): the layout of ItemState State after its op byte (u16 item, str
		          // section, u8 flags 0, varu n, n bytes blob); the world server takes the condition alone (coop_levels_xfer.cpp client_loot)
		Spawn = 5, // an object it made under its ids: the layout of EntitySpawn
		Report = 6, // what it writes in the buckets LevelResync asked for: the layout of LevelReportBody
		Money = 7, // a deal of a player of its level: u32 peer, u64 delta (two's complement), u8 1 for a trade
		Tent = 8, // a tent or sleeping bag it put up for a player of its level, after its Spawn: u16 tent, u16 character
		Stash = 9, // the stash of a retiring player of its level (OpStash) is over, after the Parent of his things: u16 character, u8 1: things stay on him
		Logic = 10, // what the binder of a static object of its level would save: the layout of LevelLogicBody
		// Protocol 34, LevelJournal only (namespace journal below has the bodies):
		Joined = 11, // a member joined a squad (register_member): journal::MemberBody
		Left = 12, // a member left a squad (unregister_member): journal::MemberBody
		Info = 13, // an info portion of a non-actor object given or taken by its writer: journal::InfoBody
		Ask = 14, // a release or an info the map server asks the world to execute on an object it does not write: journal::AskBody
		IdAsk = 15, // another block of ids, answered in the Ack: no body
		Carry = 16, // a part of the leaves of the modules' save_state the map server carries to the world save: journal::CarryBody
		Done = 17, // a numbered op ran here, with its answer: journal::DoneBody
		Flushed = 18, // the answer to LevelFlush: everything before it is in the journal: journal::FlushedBody
	};

	uint8_t op = Destroy;

	void write(Writer& w) const { w.u8(op); }

	bool read(Reader& r)
	{
		op = r.u8();
		if (!r.ok() || op > Logic)
		{
			*this = LevelEntityHead();
			return false;
		}
		return true;
	}
};

// The tail of LevelEntity State: what the authority of a level sees of a player's body and the world server does not, he is no object
// there. `place` is checkpoints::sources::Place, `source` the object of that place.
// Build 57 (owner 2026-11-18): `footing`, the base whose ground he stands on by the map server's geometry, with its point
// (checkpoint::base::Footing: u8 kind, u16 ref, 3 floats, u32 lv, u16 gv, always sent). Read as sent; whether it is sound
// (footing_ok) and names a base of the world is the world server's check. Protocol stays 32: LevelEntity goes only between the
// host's process and the map servers it starts from its own exe (coop_mapserver.cpp launch: GetModuleFileNameA(nullptr)).
// Build 57, protocol 33 (S3, owner's question: whom does he talk to there): `partner` right after `talking`, the NPC of his talk
// lock there (dialog::host_lock_of) in the world server's numbering, kNoObject none. The world server keeps it only while he talks
// (kept_partner) and only for an object it has; Lua coop.levels.talk_partner(peer). Wire: u8 fight, u8 talking, u16 partner,
// u8 place, u16 source, the footing (21 bytes): 28 bytes.
struct LevelBodyFacts
{
	uint8_t fight = 0;
	uint8_t talking = 0; // he holds a talk lock there
	uint16_t partner = kNoObject; // Build 57: the NPC of that lock
	uint8_t place = 0;
	uint16_t source = kNoObject;
	checkpoint::base::Footing footing; // Build 57

	void write(Writer& w) const
	{
		w.u8(fight);
		w.u8(talking);
		w.u16(partner);
		w.u8(place);
		w.u16(source);
		w.u8(footing.kind);
		w.u16(footing.ref);
		w.vec3(footing.pos);
		w.u32(footing.lv);
		w.u16(footing.gv);
	}

	bool read(Reader& r)
	{
		LevelBodyFacts tmp;
		tmp.fight = r.u8();
		tmp.talking = r.u8();
		tmp.partner = r.u16();
		tmp.place = r.u8();
		tmp.source = r.u16();
		tmp.footing.kind = r.u8();
		tmp.footing.ref = r.u16();
		r.vec3(tmp.footing.pos);
		tmp.footing.lv = r.u32();
		tmp.footing.gv = r.u16();
		if (!r.ok())
		{
			*this = LevelBodyFacts();
			return false;
		}
		*this = tmp;
		return true;
	}
};

// What the world server keeps of a report's partner: the NPC while he talks, else nobody (a partner without a talk lock says nothing).
inline uint16_t kept_partner(const LevelBodyFacts& f) { return f.talking != 0 ? f.partner : kNoObject; }

// What the world server does with a body report (LevelEntity State) of a map server. Take: it places his character and keeps the
// facts. Crossed: the report crossed his move. A level move puts his seat and his character on the new level at once
// (on_level_move: host_player_moves, then the teleport), while the map server he left hears it only from the next LevelMap;
// a report it sent in between (every second, unreliable) names a body that is on another level now, or of a player who is
// between two levels. Dropping it is right (placing it would pull his character back to the level he left) and honest: no
// fault, no "!" (stand b57g creator10_server, 2026-11-21: trip 3 of 10). Refused: a fault of its sender - it does not read,
// the world has no such object, the object is online here, it is no player character, its vertex is not on its sender's
// level, its position is not a number, or the sender has no level.
enum class BodyReport : uint8_t
{
	Take,
	Crossed,
	Refused,
};

inline BodyReport body_report_verdict(bool read, bool known, bool online_here, bool player_character, uint16_t body_level,
    uint16_t sender_level, uint16_t seat_level, bool position_valid)
{
	if (!read || !known || online_here || !player_character || sender_level == kNoLevel || body_level != sender_level ||
	    !position_valid)
		return BodyReport::Refused;
	return seat_level == sender_level ? BodyReport::Take : BodyReport::Crossed;
}

// How long a map server may go on reporting a player whose seat left its level before it is its fault, not a crossing: the
// LevelMap that tells it goes out at once and at least every 5 s on the reliable lane (kMapEveryMs), so three of them. A map
// server that never lets him go (a missed leave: his character would stay online on the level he left) is still refused with "!".
constexpr uint64_t kBodyCrossedGraceMs = 15000;

// One run of crossed reports of a character: the sender's level, the level of his seat, since when, the last one when.
struct BodyCrossing
{
	uint16_t sender = kNoLevel;
	uint16_t seat = kNoLevel;
	uint64_t since = 0;
	uint64_t last = 0;
};

// A crossed report joins the run of its character and says how long that run has lasted (0 for its first). Another sender or
// seat, or a gap longer than the grace, starts a new run.
inline uint64_t body_crossing_age(BodyCrossing& c, uint16_t sender_level, uint16_t seat_level, uint64_t now)
{
	if (c.sender != sender_level || c.seat != seat_level || now < c.last || now - c.last > kBodyCrossedGraceMs)
		c = BodyCrossing{sender_level, seat_level, now, now};
	c.last = now;
	return now - c.since;
}

// A crossing is honest while its run is within the grace; a longer one is the sender's fault.
inline BodyReport body_crossing_verdict(uint64_t age) { return age <= kBodyCrossedGraceMs ? BodyReport::Crossed : BodyReport::Refused; }

// LevelEntity Report wire: u32 mask, u16 n, n x {u16 id, u32 gen} [, u32 round]. The Server audit code handles the round
// after the rows. The auditor compares the objects in those buckets and their generations with the ledger (coop_xfer.h audit).
struct LevelReportBody
{
	static constexpr size_t kMaxRows = 4096;
	struct Row
	{
		uint16_t id = kNoObject;
		uint32_t gen = 0;
	};
	uint32_t buckets = 0;
	std::vector<Row> rows;

	[[nodiscard]] bool write(Writer& w) const
	{
		if (rows.size() > kMaxRows)
			return false;
		w.u32(buckets);
		w.u16(uint16_t(rows.size()));
		for (const Row& row : rows)
		{
			w.u16(row.id);
			w.u32(row.gen);
		}
		return true;
	}

	bool read(Reader& r)
	{
		*this = LevelReportBody();
		LevelReportBody tmp;
		tmp.buckets = r.u32();
		const uint16_t n = r.u16();
		if (!r.ok() || n > kMaxRows)
			return false;
		tmp.rows.resize(n);
		for (Row& row : tmp.rows)
		{
			row.id = r.u16();
			row.gen = r.u32();
		}
		if (!r.ok())
			return false;
		*this = std::move(tmp);
		return true;
	}
};

// The body of LevelEntity Logic (xrCoop/coop_levels_logic.h). Wire: u16 id, u16 n, n bytes: the client_data of its entity, as
// CGameObject::net_Save writes it. Never empty: that would clear what the world server has.
struct LevelLogicBody
{
	static constexpr size_t kMaxBytes = 8 * 1024; // the stock's max_objects_size_in_save: an object of a save has 16 KiB with its spawn data
	uint16_t id = kNoObject;
	std::vector<uint8_t> bytes;

	bool fits() const { return id != kNoObject && !bytes.empty() && bytes.size() <= kMaxBytes; }

	[[nodiscard]] bool write(Writer& w) const
	{
		if (!fits())
			return false;
		w.u16(id);
		w.u16(uint16_t(bytes.size()));
		w.bytes(bytes.data(), bytes.size());
		return true;
	}

	bool read(Reader& r)
	{
		*this = LevelLogicBody();
		LevelLogicBody tmp;
		tmp.id = r.u16();
		const uint16_t n = r.u16();
		if (!r.ok() || n > kMaxBytes || n > r.remaining())
			return false;
		tmp.bytes.resize(n);
		if (n && !r.bytes(tmp.bytes.data(), n))
			return false;
		if (!tmp.fits())
			return false;
		*this = std::move(tmp);
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

// LevelResync wire: u8 mask[4] [, u32 round]. Send the full state of the buckets whose bit is set. Bucket i is bit
// (i % 8) of byte i / 8, or bit i of a little-endian u32. The Server audit code handles the round after the mask.
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
// him (flag AllOnMap: to every player of the map, `peer` unused). `peer` is a session slot. Two flags say on which lane the host's
// process sends it on (protocol 20; neither: Reliable). A Tunnel carries a game message only: not a session message and not another
// Tunnel. The length is the rest of the packet.
struct TunnelMsg
{
	enum Flags : uint8_t
	{
		AllOnMap = 1,
		LaneUnreliable = 2,
		LaneBulk = 4,
		Known = AllOnMap | LaneUnreliable | LaneBulk,
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
		const bool one_lane = (flags & (LaneUnreliable | LaneBulk)) != (LaneUnreliable | LaneBulk);
		if (!r.ok() || (flags & ~uint8_t(Known)) != 0 || !one_lane || !peer_ok || len < 2 || r.remaining() != len)
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

// ---------------------------------------------------------------- the online byte of a spawn the trunk passes on (protocol 33)

// Build 57, step S3 (TZ 6.3: the psy phantoms of a bloodsucker were unseen in the server mode). EntitySpawn (write_spawn,
// coop_entities.cpp) is u16 id, u16 parent, u8 online, ...; the online byte says the entity is online where the sender has it. A
// player's machine brings a free entity that is in no level registry (a psy phantom: level.spawn_item with no game vertex) online
// at its spawn only when that flag speaks of the level he stands on (xrCoop/coop_mirror.h placement, flag_of_this_level): true of a
// co-op host, who stands on it. In the server mode every spawn comes from kHostPeer, the world server, which stands on no level of
// LevelMap; what the map server of his level sends him comes up the trunk in a Tunnel. The world server passes such an EntitySpawn
// on (coop_trunk.cpp trunk_host_on_tunnel, only to players of that level) with the byte kSpawnOnlineAtYourLevel: "online at the
// authority of the level you stand on". A reader takes any non-zero byte for online, as Reader::boolean always did; 2 also says
// whose flag it is. A co-op host writes 0 or 1, nothing else writes 2.
constexpr uint8_t kSpawnOnlineAtYourLevel = 2;
constexpr size_t kSpawnOnlineByte = 2 + 2 + 2; // in a carried packet (TunnelMsg::packet): its type, u16 id, u16 parent

// Marks a carried EntitySpawn that is online at its sender (the map server of the player's level). False, the packet as it was, for
// another message, an offline spawn, or a packet cut before its online byte.
inline bool mark_spawn_of_your_level(std::vector<uint8_t>& packet)
{
	if (packet.size() <= kSpawnOnlineByte || uint16_t(packet[0] | (packet[1] << 8)) != uint16_t(Msg::EntitySpawn) ||
	    packet[kSpawnOnlineByte] == 0)
		return false;
	packet[kSpawnOnlineByte] = kSpawnOnlineAtYourLevel;
	return true;
}

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

// LoadWait wire: u8 state, u32 epoch, u16 level, u8 reason, u8 n, n x {u32 peer, u8 ready}. The barrier of a simultaneous load, as the
// world server tells a player on his loading screen: Prepared (the world of this epoch is ready to be loaded: the save follows as a
// blob), Waiting (who is ready now), Enter (everybody is: leave the screen), Cancel (the load is off). `level` is the level
// of the epoch; `reason` says why a player loads: the group goes (Group). HostAlone and the states Away and Back were the hybrid's
// (the host went alone, the host's move that nobody notices): reserved since build 57, nobody sends them, a player takes them for
// nothing.
struct LoadWaitMsg
{
	enum State : uint8_t
	{
		Prepared = 0,
		Waiting = 1,
		Enter = 2,
		Cancel = 3,
		Away = 4,
		Back = 5,
	};
	enum Reason : uint8_t
	{
		Group = 0,
		HostAlone = 1,
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
	uint8_t reason = Group;

	// A row for each player of the session at the most.
	bool fits() const { return rows.size() <= kMaxPeerSlots; }

	// False, nothing written, for more rows: a player cut away would be taken for one nobody waits for.
	[[nodiscard]] bool write(Writer& w) const
	{
		if (!fits())
			return false;
		w.u8(state);
		w.u32(epoch);
		w.u16(level);
		w.u8(reason);
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
		tmp.reason = r.u8();
		const uint8_t n = r.u8();
		if (!r.ok() || tmp.state > Back || tmp.reason > HostAlone || n > kMaxPeerSlots)
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

namespace detail
{
// The bytes Writer::varu writes for `v`.
inline size_t varu_size(uint64_t v)
{
	size_t n = 1;
	while (v >= 0x80)
	{
		v >>= 7;
		++n;
	}
	return n;
}
} // namespace detail
} // namespace wire

// ---------------------------------------------------------------- the journal of a map server (protocol 34)

// TZ R32 W3 (temp/r30/design/R32-final.md sections 1 and 5): one journal up. Every registry fact of a map server's frame goes into
// one LevelJournal on the reliable lane (the lane of LevelXferUp: a frame's facts are applied before that frame's hand-back), and the
// world server answers each batch with a LevelJournalAck. A batch is read whole before any entry is applied: one entry that does
// not read refuses the batch, so a fault never applies half a frame.
//
// An entry is {u8 op, varu len, body[len]}; `op` is a LevelEntityHead::Op:
//   per-object ops (names_object: Destroy, Parent, Died, State, Item, Spawn, Tent, Stash, Logic): body = ObjectHead {u16 id, u16
//       section crc, u32 gen}, then today's LevelEntity layout of the op, whole. That layout starts with the same id (checked), so
//       today's readers (coop_levels_xfer.cpp host_on_level_entity) take layout_of(entry) unchanged. Fixed layouts are checked to
//       their size: Destroy {u16 id}, Parent {u16 item, u16 parent, vec3 pos}, Tent {u16 tent, u16 character}, Stash {u16
//       character, u8 things stay}; Logic is LevelLogicBody, Item is ItemFactsBody (the ItemState layout plus a source byte);
//       Died {u16 victim, u16 killer, state}, State {u16 character, ...} and Spawn (the EntitySpawn layout) end in a tail of their
//       own readers.
//   Money: today's layout {u32 peer, u64 delta, u8 trade}, no head (a player's book is no object).
//   Joined, Left: MemberBody. Info: InfoBody. Ask: AskBody. IdAsk: no body. Carry: CarryBody. Done: DoneBody. Flushed: FlushedBody.
//   Report (6) is no journal op: it stays a LevelEntity of its own (the answer to LevelResync).
// The head's `gen` is the keeper generation of the unit at the object's root (W1.2: the squad of a member, the holder chain of an
// item) as the sender holds it, 0 when that root is no unit it holds (W3: facts about a held unit carry its keeper generation and
// are refused for an older grant; same_object, coop_writer_rules.h, compares it with the world's). Section 5 of the design names
// the head `u16 id, u16 section crc` alone; the generation is the W3 / same_object half of an identity that it left out. The head's
// `crc` is section_crc of the object's section (for a Spawn the new object's).
namespace journal
{
constexpr size_t kMaxEntries = 16384; // entries of one batch (a surge frame: deaths, drops, item facts of some hundred NPCs)
constexpr size_t kMaxBody = 64 * 1024; // one entry (an EntitySpawn with its spawn data is some KiB; a carry part is at most 60 KiB)
constexpr size_t kMaxBatch = 8 * 1024 * 1024; // bytes of one batch (Session::send takes 16 MiB); a bigger frame goes as two batches
constexpr uint8_t kLastOp = wire::LevelEntityHead::Flushed;

// Why the world refused an entry (LevelJournalAck). The world's acceptance (coop_levels_accept.h) picks the reason.
enum Why : uint8_t
{
	NotWriter = 1, // the sender does not write the object (for a Spawn the new object's root, for a Parent the old or the new root)
	Identity = 2, // id, section crc or keeper generation differ from the world's object
	PastFence = 3, // the entry came after the sender's fence (its level move, a grant handed back)
	Unknown = 4, // the world has no such object
	NotOwnLevel = 5, // an ask about an object whose root does not stand on the sender's level
	IdRange = 6, // a spawn under an id that is not of the sender's slice, or that the world has under another object
	Ghost = 7, // the target is the ghost actor: nobody
	Malformed = 8, // the entry's layout does not read where the world reads it
	Refused = 9, // the world's own rule refused it (a deal, a logic, a carry conflict)
};
constexpr uint8_t kLastWhy = Refused;

// An op a journal carries: today's LevelEntity ops but Report, and the journal's own.
inline bool known_op(uint8_t op) { return op <= kLastOp && op != wire::LevelEntityHead::Report; }

// The ops whose body starts with an ObjectHead.
inline bool names_object(uint8_t op)
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
	case H::Logic: return true;
	default: break;
	}
	return false;
}

// For the logs; "?" for a value no op has.
inline const char* op_name(uint8_t op)
{
	using H = wire::LevelEntityHead;
	switch (op)
	{
	case H::Destroy: return "Destroy";
	case H::Parent: return "Parent";
	case H::Died: return "Died";
	case H::State: return "State";
	case H::Item: return "ItemFacts";
	case H::Spawn: return "Spawn";
	case H::Report: return "Report";
	case H::Money: return "Money";
	case H::Tent: return "Tent";
	case H::Stash: return "Stash";
	case H::Logic: return "Logic";
	case H::Joined: return "Joined";
	case H::Left: return "Left";
	case H::Info: return "Info";
	case H::Ask: return "Ask";
	case H::IdAsk: return "IdAsk";
	case H::Carry: return "Carry";
	case H::Done: return "Done";
	case H::Flushed: return "Flushed";
	default: break;
	}
	return "?";
}

inline const char* why_name(uint8_t why)
{
	switch (why)
	{
	case NotWriter: return "not the writer";
	case Identity: return "another object";
	case PastFence: return "past the fence";
	case Unknown: return "no such object";
	case NotOwnLevel: return "not on its level";
	case IdRange: return "id not of its slice";
	case Ghost: return "the ghost";
	case Malformed: return "malformed";
	case Refused: return "refused";
	default: break;
	}
	return "?";
}

// The u16 of an object's section in an ObjectHead and an AskBody: FNV-1a 32 of the name, its two halves xor-ed. Every process of a
// session is one exe, so one name is one value everywhere; two sections may share a value (1 in 65536): the id and the keeper
// generation are the rest of an identity.
inline uint16_t section_crc(std::string_view section)
{
	uint32_t h = 2166136261u;
	for (const char c : section)
		h = (h ^ uint8_t(c)) * 16777619u;
	return uint16_t((h >> 16) ^ (h & 0xFFFFu));
}

// The head of every per-object entry. Wire: u16 id, u16 crc, u32 gen.
struct ObjectHead
{
	static constexpr size_t kSize = 2 + 2 + 4;

	uint16_t id = wire::kNoObject;
	uint16_t crc = 0;
	uint32_t gen = 0;

	void write(Writer& w) const
	{
		w.u16(id);
		w.u16(crc);
		w.u32(gen);
	}

	bool read(Reader& r)
	{
		ObjectHead tmp;
		tmp.id = r.u16();
		tmp.crc = r.u16();
		tmp.gen = r.u32();
		if (!r.ok() || tmp.id == wire::kNoObject)
		{
			*this = ObjectHead();
			return false;
		}
		*this = tmp;
		return true;
	}
};

// One entry of a batch: its op and its body as sent (for a per-object op the head first). The frozen interface's journal::Entry
// (host_apply_fact, coop_levels_alife.h).
struct Entry
{
	uint8_t op = wire::LevelEntityHead::Destroy;
	std::vector<uint8_t> body;

	bool operator==(const Entry& o) const { return op == o.op && body == o.body; }
};

// Joined, Left: a squad and one member (stock CSE_ALifeOnlineOfflineGroup::register_member / unregister_member). Wire: u16 squad,
// u16 member.
struct MemberBody
{
	uint16_t squad = wire::kNoObject;
	uint16_t member = wire::kNoObject;

	bool fits() const { return squad != wire::kNoObject && member != wire::kNoObject && squad != member; }

	[[nodiscard]] bool write(Writer& w) const
	{
		if (!fits())
			return false;
		w.u16(squad);
		w.u16(member);
		return true;
	}

	bool read(Reader& r)
	{
		MemberBody tmp;
		tmp.squad = r.u16();
		tmp.member = r.u16();
		if (!r.ok() || !tmp.fits())
		{
			*this = MemberBody();
			return false;
		}
		*this = tmp;
		return true;
	}
};

// Info: an info portion of a non-actor object, given (on 1) or taken (on 0) by its writer. Wire: u16 id, u8 on, str info.
struct InfoBody
{
	static constexpr size_t kMaxInfo = 256;

	uint16_t id = wire::kNoObject;
	uint8_t on = 1;
	std::string info;

	bool fits() const { return id != wire::kNoObject && on <= 1 && !info.empty() && wire::detail::printable(info, kMaxInfo); }

	[[nodiscard]] bool write(Writer& w) const
	{
		if (!fits())
			return false;
		w.u16(id);
		w.u8(on);
		w.str(info);
		return true;
	}

	bool read(Reader& r)
	{
		InfoBody tmp;
		tmp.id = r.u16();
		tmp.on = r.u8();
		tmp.info = r.str();
		if (!r.ok() || !tmp.fits())
		{
			*this = InfoBody();
			return false;
		}
		*this = std::move(tmp);
		return true;
	}
};

// Ask: what a map server's script wants of an object of its level it does not write (W2a): the world executes it when it writes the
// object and its root stands on the sender's level (W2b), and the map server does not apply it first. Wire: u8 what, u16 id, u16 crc
// (section_crc of the object as the sender has it), then str info for GiveInfo and DisableInfo alone.
struct AskBody
{
	enum What : uint8_t
	{
		Release = 1,
		GiveInfo = 2,
		DisableInfo = 3,
	};

	uint8_t what = Release;
	uint16_t id = wire::kNoObject;
	uint16_t crc = 0;
	std::string info; // GiveInfo, DisableInfo: the info portion; empty for Release

	bool names_info() const { return what == GiveInfo || what == DisableInfo; }

	bool fits() const
	{
		if (what < Release || what > DisableInfo || id == wire::kNoObject)
			return false;
		return names_info() ? !info.empty() && wire::detail::printable(info, InfoBody::kMaxInfo) : info.empty();
	}

	[[nodiscard]] bool write(Writer& w) const
	{
		if (!fits())
			return false;
		w.u8(what);
		w.u16(id);
		w.u16(crc);
		if (names_info())
			w.str(info);
		return true;
	}

	bool read(Reader& r)
	{
		AskBody tmp;
		tmp.what = r.u8();
		tmp.id = r.u16();
		tmp.crc = r.u16();
		if (r.ok() && tmp.names_info())
			tmp.info = r.str();
		if (!r.ok() || !tmp.fits())
		{
			*this = AskBody();
			return false;
		}
		*this = std::move(tmp);
		return true;
	}
};

// Carry: part `part` of `total` of the leaves a map server carries from its modules' save_state to the world save (W6; the blob is
// coop_srv_carry.script's). Wire: u16 part, u16 total, then the blob: the rest of the body, 1 .. kMaxPart bytes.
struct CarryBody
{
	static constexpr size_t kMaxPart = 60 * 1024;

	uint16_t part = 0;
	uint16_t total = 1;
	std::vector<uint8_t> blob;

	bool fits() const { return total >= 1 && part < total && !blob.empty() && blob.size() <= kMaxPart; }

	[[nodiscard]] bool write(Writer& w) const
	{
		if (!fits())
			return false;
		w.u16(part);
		w.u16(total);
		w.bytes(blob.data(), blob.size());
		return true;
	}

	bool read(Reader& r)
	{
		*this = CarryBody();
		CarryBody tmp;
		tmp.part = r.u16();
		tmp.total = r.u16();
		if (!r.ok() || !wire::detail::read_tail(r, tmp.blob, kMaxPart) || !tmp.fits())
			return false;
		*this = std::move(tmp);
		return true;
	}
};

// Done: a numbered op that consumes or pays ran here (W7), with the answer the executor recorded; the world keeps it with the effect
// and hands it to the next map server. `origin` and `op` are the op's number {origin character, seq} as its Lua makes it. Wire:
// u32 origin, u32 op, str answer (0 .. kMaxAnswer bytes).
struct DoneBody
{
	static constexpr size_t kMaxAnswer = 8 * 1024;

	uint32_t origin = 0;
	uint32_t op = 0;
	std::string answer;

	bool fits() const { return answer.size() <= kMaxAnswer; }

	[[nodiscard]] bool write(Writer& w) const
	{
		if (!fits())
			return false;
		w.u32(origin);
		w.u32(op);
		w.str(answer);
		return true;
	}

	bool read(Reader& r)
	{
		DoneBody tmp;
		tmp.origin = r.u32();
		tmp.op = r.u32();
		tmp.answer = r.str();
		if (!r.ok() || !tmp.fits())
		{
			*this = DoneBody();
			return false;
		}
		*this = std::move(tmp);
		return true;
	}
};

// Flushed: the answer to LevelFlush{round} (W4.1): every fact before it is in the journal. Wire: u32 round (>= 1, as FlushRound's).
struct FlushedBody
{
	uint32_t round = 0;

	bool fits() const { return round != 0; }

	[[nodiscard]] bool write(Writer& w) const
	{
		if (!fits())
			return false;
		w.u32(round);
		return true;
	}

	bool read(Reader& r)
	{
		FlushedBody tmp;
		tmp.round = r.u32();
		if (!r.ok() || !tmp.fits())
		{
			*this = FlushedBody();
			return false;
		}
		*this = tmp;
		return true;
	}
};

// ItemFacts (op Item of a journal, W8.3): the condition, uses, upgrades or addon flags of an item the map server writes changed,
// players' items included. The layout of ItemState State after its op byte, plus the source of the change. Wire: u16 item, str
// section, u8 flags, varu n, n bytes of the ItemState blob (1 .. kMaxBlob), u8 source.
struct ItemFactsBody
{
	enum Source : uint8_t
	{
		Loot = 0, // the wear its level gave loot (the LevelEntity Item of protocol 33)
		Decider = 1, // the writer's deliberate write (a script's set_condition, set_remaining_uses, an upgrade it installed)
		Owner = 2, // the owner's report the writer took (his own use, on the latest version of the field: coop_item_tags.h fresh)
	};
	static constexpr size_t kMaxSection = 64;
	static constexpr size_t kMaxBlob = 4096; // kMaxItemState (coop_spawn.h): the ItemState's own bound
	static constexpr uint8_t kFlagAuthority = 1; // the one flag an ItemState has (coop_items.cpp kFlagAuthority)

	uint16_t item = wire::kNoObject;
	std::string section;
	uint8_t flags = 0;
	std::vector<uint8_t> blob;
	uint8_t source = Loot;

	bool fits() const
	{
		return item != wire::kNoObject && !section.empty() && wire::detail::printable(section, kMaxSection) &&
		    (flags & ~kFlagAuthority) == 0 && !blob.empty() && blob.size() <= kMaxBlob && source <= Owner;
	}

	[[nodiscard]] bool write(Writer& w) const
	{
		if (!fits())
			return false;
		w.u16(item);
		w.str(section);
		w.u8(flags);
		w.varu(blob.size());
		w.bytes(blob.data(), blob.size());
		w.u8(source);
		return true;
	}

	bool read(Reader& r)
	{
		*this = ItemFactsBody();
		ItemFactsBody tmp;
		tmp.item = r.u16();
		tmp.section = r.str();
		tmp.flags = r.u8();
		const uint64_t n = r.varu();
		if (!r.ok() || n == 0 || n > kMaxBlob || n > r.remaining())
			return false;
		tmp.blob.resize(size_t(n));
		if (!r.bytes(tmp.blob.data(), size_t(n)))
			return false;
		tmp.source = r.u8();
		if (!r.ok() || !tmp.fits())
			return false;
		*this = std::move(tmp);
		return true;
	}
};

// `r` reads as B with nothing left over.
template <class B>
bool reads_whole(Reader r, B& out)
{
	return out.read(r) && r.remaining() == 0;
}

// Does `body` read as the body of `op`? The batch's reader asks it of every entry before it keeps one, the writer and the builders
// before they write one.
inline bool body_ok(uint8_t op, const uint8_t* data, size_t size)
{
	using H = wire::LevelEntityHead;
	if (!known_op(op) || size > kMaxBody)
		return false;
	Reader r(data, size);
	if (names_object(op))
	{
		ObjectHead head;
		if (!head.read(r))
			return false;
		const Reader layout(r.cursor(), r.remaining());
		Reader peek = layout;
		if (peek.u16() != head.id || !peek.ok())
			return false; // today's layout starts with the object the head names
		const size_t n = layout.remaining();
		switch (op)
		{
		case H::Destroy: return n == 2;
		case H::Parent: return n == 2 + 2 + 3 * 4;
		case H::Tent: return n == 2 + 2;
		case H::Stash: return n == 2 + 1;
		case H::Died: return n >= 2 + 2;
		case H::Item:
		{
			ItemFactsBody b;
			return reads_whole(layout, b);
		}
		case H::Logic:
		{
			wire::LevelLogicBody b;
			return reads_whole(layout, b);
		}
		default: return true; // State, Spawn: their tails are their own readers'
		}
	}
	switch (op)
	{
	case H::Money:
	{
		const uint32_t peer = r.u32();
		r.u64();
		const uint8_t trade = r.u8();
		return r.ok() && r.remaining() == 0 && wire::detail::slot_ok(peer) && trade <= 1;
	}
	case H::Joined:
	case H::Left:
	{
		MemberBody b;
		return reads_whole(r, b);
	}
	case H::Info:
	{
		InfoBody b;
		return reads_whole(r, b);
	}
	case H::Ask:
	{
		AskBody b;
		return reads_whole(r, b);
	}
	case H::IdAsk: return size == 0;
	case H::Carry:
	{
		CarryBody b;
		return reads_whole(r, b);
	}
	case H::Done:
	{
		DoneBody b;
		return reads_whole(r, b);
	}
	case H::Flushed:
	{
		FlushedBody b;
		return reads_whole(r, b);
	}
	default: break;
	}
	return false;
}

inline bool body_ok(uint8_t op, const std::vector<uint8_t>& body) { return body_ok(op, body.data(), body.size()); }

// An entry of a per-object op: the head, then today's layout `layout` of the op (which starts with h.id). False, `out` empty, for
// another op or for what body_ok refuses.
inline bool object_entry(uint8_t op, const ObjectHead& h, const Writer& layout, Entry& out)
{
	out = Entry();
	if (!names_object(op))
		return false;
	Writer w(ObjectHead::kSize + layout.size());
	h.write(w);
	w.bytes(layout.ptr(), layout.size());
	if (!body_ok(op, w.ptr(), w.size()))
		return false;
	out.op = op;
	out.body = w.data();
	return true;
}

// An entry of an op without a head, from its body (MemberBody, InfoBody, AskBody, CarryBody, DoneBody, FlushedBody). False, `out`
// empty, for a per-object op or a body the op does not take.
template <class B>
bool body_entry(uint8_t op, const B& body, Entry& out)
{
	out = Entry();
	if (names_object(op))
		return false;
	Writer w;
	if (!body.write(w) || !body_ok(op, w.ptr(), w.size()))
		return false;
	out.op = op;
	out.body = w.data();
	return true;
}

// The IdAsk entry: no body.
inline Entry id_ask_entry()
{
	Entry e;
	e.op = wire::LevelEntityHead::IdAsk;
	return e;
}

// The head of a per-object entry. False (`h` cleared) for another op or a body without one.
inline bool object_of(const Entry& e, ObjectHead& h)
{
	h = ObjectHead();
	if (!names_object(e.op))
		return false;
	Reader r(e.body.data(), e.body.size());
	return h.read(r);
}

// Today's LevelEntity layout of a per-object entry, after its head; an empty Reader for another op.
inline Reader layout_of(const Entry& e)
{
	if (!names_object(e.op) || e.body.size() < ObjectHead::kSize)
		return Reader(nullptr, 0);
	return Reader(e.body.data() + ObjectHead::kSize, e.body.size() - ObjectHead::kSize);
}
} // namespace journal

namespace wire
{
// ---------------------------------------------------------------- LevelJournal (0x01E0), LevelJournalAck (0x01E1), protocol 34

// LevelJournal wire: u32 seq, u16 n, n x {u8 op, varu len, body[len]} (namespace journal above). `seq` counts the batches of one map
// server from 1; a batch has 1 .. kMaxEntries entries and at most kMaxBatch bytes. read() checks every entry (journal::body_ok)
// before it keeps any: a batch with one entry that does not read is refused whole (seq 0, no entries).
struct LevelJournalMsg
{
	uint32_t seq = 0;
	std::vector<journal::Entry> entries;

	size_t wire_size() const
	{
		size_t n = 4 + 2;
		for (const journal::Entry& e : entries)
			n += 1 + detail::varu_size(e.body.size()) + e.body.size();
		return n;
	}

	// What read() takes: a seq, 1 .. kMaxEntries entries that each read, kMaxBatch bytes at most.
	bool fits() const
	{
		if (seq == 0 || entries.empty() || entries.size() > journal::kMaxEntries || wire_size() > journal::kMaxBatch)
			return false;
		for (const journal::Entry& e : entries)
			if (!journal::body_ok(e.op, e.body))
				return false;
		return true;
	}

	// False, nothing written, for a batch read() would refuse: an entry that does not read would cost the world the whole frame.
	[[nodiscard]] bool write(Writer& w) const
	{
		if (!fits())
			return false;
		w.u32(seq);
		w.u16(uint16_t(entries.size()));
		for (const journal::Entry& e : entries)
		{
			w.u8(e.op);
			w.varu(e.body.size());
			w.bytes(e.body.data(), e.body.size());
		}
		return true;
	}

	bool read(Reader& r)
	{
		*this = LevelJournalMsg();
		const size_t start = r.remaining();
		LevelJournalMsg tmp;
		tmp.seq = r.u32();
		const uint16_t n = r.u16();
		// every entry is at least its op and a one-byte length: a count the packet cannot hold is refused before anything is kept
		if (!r.ok() || tmp.seq == 0 || n == 0 || n > journal::kMaxEntries || size_t(n) * 2 > r.remaining())
			return false;
		tmp.entries.reserve(n);
		for (uint16_t i = 0; i < n; ++i)
		{
			journal::Entry e;
			e.op = r.u8();
			const uint64_t len = r.varu();
			if (!r.ok() || len > journal::kMaxBody || len > r.remaining())
				return false;
			e.body.resize(size_t(len));
			if (len && !r.bytes(e.body.data(), size_t(len)))
				return false;
			if (!journal::body_ok(e.op, e.body))
				return false;
			tmp.entries.push_back(std::move(e));
		}
		if (start - r.remaining() > journal::kMaxBatch)
			return false;
		*this = std::move(tmp);
		return true;
	}
};

// LevelJournalAck wire: u32 upto, u8 n, n x {u32 seq, u8 op, u16 id, u8 why}, u8 has_block, [u16 first, u16 count]. Every batch of
// the bridge up to `upto` is applied but the entries the rows name: the world refused them (journal::Why; `id` kNoObject for an op
// that names no object), and the sender repairs toward the world (W2b: it releases what the world refused to create, keeping the
// id reserved; it fetches what the world refused to change or remove). The block answers an IdAsk: the ids first .. first + count
// - 1 are the sender's from now on (W9).
struct LevelJournalAckMsg
{
	static constexpr size_t kMaxRefusals = 255; // a u8 count; more go in another Ack with the same upto

	struct Refusal
	{
		uint32_t seq = 0;
		uint8_t op = 0;
		uint16_t id = kNoObject;
		uint8_t why = 0;
	};

	uint32_t upto = 0;
	std::vector<Refusal> refusals;
	bool has_block = false;
	uint16_t first = kNoObject;
	uint16_t count = 0;

	static bool refusal_ok(const Refusal& f, uint32_t upto)
	{
		return f.seq != 0 && f.seq <= upto && journal::known_op(f.op) && f.why >= 1 && f.why <= journal::kLastWhy;
	}
	// ids first .. first + count - 1, none of them kNoObject
	static bool block_ok(uint16_t first, uint16_t count) { return count != 0 && uint32_t(first) + count <= kNoObject; }

	// What read() takes: every Ack answers a batch (upto >= 1), its rows name batches it acknowledges, ops and reasons that exist.
	bool fits() const
	{
		if (upto == 0 || refusals.size() > kMaxRefusals || (has_block && !block_ok(first, count)))
			return false;
		for (const Refusal& f : refusals)
			if (!refusal_ok(f, upto))
				return false;
		return true;
	}

	// False, nothing written, for an Ack read() would refuse: a row cut away would be an entry the sender takes for applied.
	[[nodiscard]] bool write(Writer& w) const
	{
		if (!fits())
			return false;
		w.u32(upto);
		w.u8(uint8_t(refusals.size()));
		for (const Refusal& f : refusals)
		{
			w.u32(f.seq);
			w.u8(f.op);
			w.u16(f.id);
			w.u8(f.why);
		}
		w.u8(has_block ? 1 : 0);
		if (has_block)
		{
			w.u16(first);
			w.u16(count);
		}
		return true;
	}

	bool read(Reader& r)
	{
		*this = LevelJournalAckMsg();
		LevelJournalAckMsg tmp;
		tmp.upto = r.u32();
		const uint8_t n = r.u8();
		if (!r.ok() || tmp.upto == 0)
			return false;
		tmp.refusals.reserve(n);
		for (uint8_t i = 0; i < n; ++i)
		{
			Refusal f;
			f.seq = r.u32();
			f.op = r.u8();
			f.id = r.u16();
			f.why = r.u8();
			if (!r.ok() || !refusal_ok(f, tmp.upto))
				return false;
			tmp.refusals.push_back(f);
		}
		const uint8_t block = r.u8();
		if (!r.ok() || block > 1)
			return false;
		tmp.has_block = block != 0;
		if (tmp.has_block)
		{
			tmp.first = r.u16();
			tmp.count = r.u16();
			if (!r.ok() || !block_ok(tmp.first, tmp.count))
				return false;
		}
		*this = std::move(tmp);
		return true;
	}
};
} // namespace wire
} // namespace levels
} // namespace coop
