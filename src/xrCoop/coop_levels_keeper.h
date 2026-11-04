// Anomaly Coop — split levels (docs/SPLIT_LEVELS.md §7a): what a map server computes and what the world server keeps.
// The world server holds the world: offline, for every map. A map server computes the creatures near the players of its level. A
// creature (or a squad) the world server simulates offline moves between them only through the transfer core, xrCoop/coop_xfer.h,
// where every change of owner goes (plan §1.7): the map server asks (TakeReq), the world server freezes it and grants it with its
// fresh state, the map server gives it back with its state when it goes offline. An object the world server does not simulate (a
// restrictor, an item on the ground, a physics object, a smart terrain's online half) is not transferred: it goes online by the
// stock rules on the process that has that map loaded, and only what happens to it travels (LevelEntity).
//   lead_vote  the switch manager's question on a map server, for one object;
//   Keeper     the world server's side: the Ledger over the bridges of the map servers, and the one place that decides "the world
//              server does not simulate this object" (plan §5.3): every mark is set and cleared from what the ledger says after a
//              packet.
// The engine applies the changes (coop_levels_alife.cpp): a mark takes the object out of the host's offline simulation, without
// a game object on the host; the state of a change goes into its server entity.
// Pure C++17, header-only, no engine dependencies: used by the engine and by the unit-test exe.
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <utility>
#include <vector>

#include "coop_levels_wire.h"
#include "coop_xfer.h"

namespace coop
{
namespace xfer
{
using Bytes = std::vector<uint8_t>;
}

namespace levels
{
// ---------------------------------------------------------------- the wire of a transfer

// The core's ops and the wire's are numbered apart (coop_levels_wire.h levels_wire_xfer_ops_cover_the_core): these are the only
// places that turn one into the other. False for an op that does not go in that direction.
inline bool up_of(const xfer::Packet& p, wire::LevelXferUpMsg& m)
{
	using W = wire::LevelXferUpMsg;
	switch (p.op)
	{
	case xfer::Op::TakeReq: m.op = W::TakeReq; break;
	case xfer::Op::TakeCommit: m.op = W::TakeCommit; break;
	case xfer::Op::ReleasePrepare: m.op = W::ReleasePrepare; break;
	case xfer::Op::ReleaseCommit: m.op = W::ReleaseCommit; break;
	case xfer::Op::SpawnPrepare: m.op = W::SpawnPrepare; break;
	case xfer::Op::Destroy: m.op = W::Destroy; break;
	case xfer::Op::AbortAck: m.op = W::AbortAck; break;
	case xfer::Op::Snapshot: m.op = W::Snapshot; break;
	default: return false;
	}
	m.id = p.key.id;
	m.gen = p.key.gen;
	m.cargo = p.payload;
	return true;
}

inline xfer::Packet packet_of(const wire::LevelXferUpMsg& m)
{
	using W = wire::LevelXferUpMsg;
	xfer::Op op = xfer::Op::TakeReq;
	switch (m.op)
	{
	case W::TakeCommit: op = xfer::Op::TakeCommit; break;
	case W::ReleasePrepare: op = xfer::Op::ReleasePrepare; break;
	case W::ReleaseCommit: op = xfer::Op::ReleaseCommit; break;
	case W::SpawnPrepare: op = xfer::Op::SpawnPrepare; break;
	case W::Destroy: op = xfer::Op::Destroy; break;
	case W::AbortAck: op = xfer::Op::AbortAck; break;
	case W::Snapshot: op = xfer::Op::Snapshot; break;
	default: break;
	}
	return xfer::Packet{op, xfer::Key{m.id, m.gen}, 0, xfer::kWorld, m.cargo};
}

inline bool down_of(const xfer::Packet& p, wire::LevelXferDownMsg& m)
{
	using W = wire::LevelXferDownMsg;
	switch (p.op)
	{
	case xfer::Op::Grant: m.op = W::Grant; break;
	case xfer::Op::Ack: m.op = W::Ack; break;
	case xfer::Op::Done: m.op = W::Done; break;
	case xfer::Op::Abort: m.op = W::Abort; break;
	case xfer::Op::Drop: m.op = W::Drop; break;
	case xfer::Op::Snapshot: m.op = W::Snapshot; break;
	default: return false;
	}
	m.id = p.key.id;
	m.gen = p.key.gen;
	m.cargo = p.payload;
	return true;
}

inline xfer::Packet packet_of(const wire::LevelXferDownMsg& m, xfer::Peer self)
{
	using W = wire::LevelXferDownMsg;
	xfer::Op op = xfer::Op::Grant;
	switch (m.op)
	{
	case W::Ack: op = xfer::Op::Ack; break;
	case W::Done: op = xfer::Op::Done; break;
	case W::Abort: op = xfer::Op::Abort; break;
	case W::Drop: op = xfer::Op::Drop; break;
	case W::Snapshot: op = xfer::Op::Snapshot; break;
	default: break;
	}
	return xfer::Packet{op, xfer::Key{m.id, m.gen}, xfer::kWorld, self, m.cargo};
}

// ---------------------------------------------------------------- ids

// A map server creates objects at once (loot, a thrown grenade, ammo out of a magazine) and the game uses them at once, so a request
// to the world server will not do (recon §2.5). The world server keeps these ids out of its own generator in every world of a
// session with split levels, and each map server owns its slice of them: what it makes under its ids cannot meet what the world
// server or another map server makes. Nothing to lease, nothing to give back: the ids of a slice stay its own; an id it frees goes
// back to its own generator and comes round again.
// A slice feeds all a level makes and the id leases of its players: the four traders of k00_marsh alone are some 570 objects. The world
// stands below the slices: GAMMA's has 27296 objects, 0x6AA0, and a loaded world's generator gives its lowest ids first (CID_Generator).
constexpr uint16_t kLeaderIdFirst = 0x9000;
constexpr uint16_t kLeaderIdsEach = 4095; // 7 map servers: up to 0xFFF8
constexpr uint32_t kLeaderSlots = 7;

// The first id of the slice of map server k (1 .. kLeaderSlots, coop_mapserver_rules.h bridge_index); 0 for anybody else.
inline uint16_t leader_ids_of(uint32_t k)
{
	return k >= 1 && k <= kLeaderSlots ? uint16_t(kLeaderIdFirst + (k - 1) * kLeaderIdsEach) : uint16_t(0);
}

// How many slices the world server keeps from its own generator; what a map server cannot use stays the world's. `allowed` is
// coop_max_map_servers. `highest` is the highest number a map server has: it can stand over `allowed` (a lost server keeps its number
// while its players wait for the next start and another level takes the free place; the console can lower the setting), and a slice in
// use is never the world's.
inline uint32_t leader_slots_kept(uint32_t allowed, uint32_t highest)
{
	const uint32_t wanted = allowed > highest ? allowed : highest;
	return wanted < kLeaderSlots ? wanted : kLeaderSlots;
}

// ---------------------------------------------------------------- the vote

enum class LeadVote : uint8_t
{
	Stock, // the stock rules decide (distance, its own switch flags)
	Offline, // it stays offline here
};

struct LeadAsk
{
	LeadVote vote;
	bool ask; // near a player and not its own yet: ask the world server for it
};

// `transferred`: the host simulates this kind of object offline, so it has to be taken (a creature or a squad);
// `held`: this process writes it (Holder::authors); `frozen`: on its way back to the host (Holder::frozen);
// `in_reach`: within the switch distance of a player of the level.
// (Not `near`: the Windows headers define it to nothing. The parameter lost its name, `{Offline, near}` compiled as `{Offline, }`,
// and a map server never asked for anything; the tests, without those headers, saw nothing. Live 2026-11-01, test_source_names.)
inline LeadAsk lead_vote(bool transferred, bool held, bool frozen, bool in_reach)
{
	if (!transferred)
		return LeadAsk{LeadVote::Stock, false};
	if (held)
		return LeadAsk{frozen ? LeadVote::Offline : LeadVote::Stock, false};
	return LeadAsk{LeadVote::Offline, in_reach};
}

// ---------------------------------------------------------------- the world server's book

class Keeper
{
public:
	enum class Kind : uint8_t
	{
		Mark, // the host stops simulating the object: a client writes it (or is about to)
		Unmark, // it is the host's again: `state` is what it comes back with
		State, // a state from its client while it stays his
		Gone, // its client destroyed it
	};
	struct Change
	{
		Kind kind;
		uint16_t id;
		xfer::Bytes state;
	};

	const xfer::Ledger& ledger() const { return m_ledger; }

	// A new world: nothing of the old one counts, and no peer of the old ledger can pass for a new connection's (R7).
	void restart()
	{
		xfer::Ledger fresh;
		fresh.set_peer_floor(m_ledger.next_peer());
		m_ledger = std::move(fresh);
		m_peers.clear();
		m_marked.clear();
		m_changes.clear();
	}

	// Requirement 2: what is in flight at a save, the text of a chunk of its own (a custom data goes through a 16 KB packet, the
	// journal may need 256 KB).
	std::string journal() const { return m_ledger.save_journal(); }

	// That save was loaded (R8): every map server is gone and nobody's half counts. A release the world had taken comes home with its
	// state, a grant rolls back to what the save holds; the gens go on in a new epoch and no peer is reused. False, and nothing
	// changed, for a malformed text.
	bool resume(const std::string& text)
	{
		uint32_t gen = 0;
		xfer::Peer next = 0;
		std::vector<xfer::JournalEntry> list;
		if (!xfer::read_journal(text, gen, next, list))
			return false;
		xfer::Ledger fresh;
		fresh.set_peer_floor(m_ledger.next_peer());
		if (!fresh.load_journal(text))
			return false;
		fresh.resolve_after_load();
		m_ledger = std::move(fresh);
		m_peers.clear();
		m_marked.clear();
		m_changes.clear();
		for (xfer::JournalEntry& j : list)
			if (j.kind == 'R')
				m_changes.push_back(Change{Kind::State, j.id, std::move(j.payload)});
		return true;
	}

	// A map server leads its level from now on. A connection that was open before is closed first: nothing of it can pass for this
	// one's (R7: the ledger never reuses a peer).
	void connect(uint32_t session_peer)
	{
		disconnect(session_peer, 0);
		m_peers[session_peer] = m_ledger.register_server();
	}
	bool connected(uint32_t session_peer) const { return m_peers.count(session_peer) != 0; }

	// It stopped, or crashed, or its world is gone: everything of its comes home at its last state.
	void disconnect(uint32_t session_peer, uint64_t now)
	{
		const auto it = m_peers.find(session_peer);
		if (it == m_peers.end())
			return;
		const xfer::Peer peer = it->second;
		m_peers.erase(it);
		std::vector<uint16_t> ids;
		for (const auto& kv : m_marked)
			if (kv.second == peer)
				ids.push_back(kv.first);
		m_ledger.server_lost(peer, now);
		for (uint16_t id : ids)
			settle(id, peer);
	}

	// A packet of the client's half (LevelXferUp). `fresh`: for a TakeReq, the object's state now, null when the host does not
	// have the object or may not give it (it stands on another level, it is online here): he is refused, gen 0, and stops
	// asking, unless the object is his already (his request repeated). Returns what goes down to him; take_changes() has what
	// the engine must apply.
	std::vector<xfer::Packet> on_up(uint32_t session_peer, xfer::Packet p, const xfer::Bytes* fresh, uint64_t now)
	{
		const auto it = m_peers.find(session_peer);
		if (it == m_peers.end())
			return {};
		const xfer::Peer peer = it->second;
		p.from = peer;
		p.to = xfer::kWorld;
		if (p.op == xfer::Op::TakeReq)
		{
			const bool his = m_ledger.peer(p.key.id) == peer || m_ledger.owner(p.key.id) == peer;
			if (fresh)
				m_ledger.add_world_object(p.key.id, *fresh); // only what a client asked for ever has a record
			else if (!his)
				return {xfer::detail::packet(xfer::Op::Abort, p.key.id, 0, xfer::kWorld, peer)};
		}
		std::vector<xfer::Packet> down = m_ledger.on_packet(p, now);
		settle(p.key.id, peer);
		if (p.op == xfer::Op::Snapshot && !m_ledger.world_authors(p.key.id))
			if (const xfer::Bytes* st = m_ledger.last_confirmed(p.key.id))
				m_changes.push_back(Change{Kind::State, p.key.id, *st});
		return down;
	}

	// The auditor (plan §1.7, requirements 3 and 4): the hash of what he says he writes against what the ledger gives him. States
	// are not compared: the stream is seconds behind by design, only who writes what under which gen is the auditor's question.
	static xfer::Entry bare(xfer::Entry e)
	{
		e.parent = xfer::kNoParent;
		e.alive = true;
		e.digest = 0;
		return e;
	}
	static std::vector<xfer::Entry> bare(std::vector<xfer::Entry> all)
	{
		for (xfer::Entry& e : all)
			e = bare(e);
		return all;
	}
	// The buckets to ask him for (LevelResync), 0 when his hash is what the ledger expects.
	uint32_t differs(uint32_t session_peer, const xfer::LevelHash& his) const
	{
		const auto it = m_peers.find(session_peer);
		return it == m_peers.end() ? 0u : xfer::mismatched(his, xfer::hash_of(bare(m_ledger.owned_by(it->second))));
	}
	// His report of those buckets: duplicates and losses are healed by the core (coop_xfer.h heal). `found` gets the findings for
	// the loud log line. Returns what goes down to him.
	std::vector<xfer::Packet> audit(uint32_t session_peer, const std::vector<xfer::Entry>& reported, uint32_t buckets,
	    std::vector<xfer::Finding>& found)
	{
		const auto it = m_peers.find(session_peer);
		if (it == m_peers.end())
			return {};
		const xfer::Peer peer = it->second;
		found = xfer::audit(m_ledger, peer, reported, buckets);
		std::vector<xfer::Packet> down = xfer::heal(m_ledger, found);
		for (const xfer::Finding& f : found)
			settle(f.id, peer);
		return down;
	}

	// Repeats and timeouts of the host's side; session_of says where each packet goes.
	std::vector<xfer::Packet> tick(uint64_t now)
	{
		std::vector<xfer::Packet> down = m_ledger.tick(now);
		for (auto it = m_marked.begin(); it != m_marked.end();)
		{
			const uint16_t id = it->first;
			const xfer::Peer peer = it->second;
			++it; // settle may erase this one
			settle(id, peer);
		}
		return down;
	}

	// The session peer of a ledger peer, kNoSession when he is gone.
	static constexpr uint32_t kNoSession = 0xffffffffu;
	uint32_t session_of(xfer::Peer peer) const
	{
		for (const auto& kv : m_peers)
			if (kv.second == peer)
				return kv.first;
		return kNoSession;
	}

	// The host does not simulate this object now.
	bool away(uint16_t id) const { return m_marked.count(id) != 0; }
	// R13, a new simulator that keeps this book: everything it has to be told is away.
	std::vector<uint16_t> marks() const
	{
		std::vector<uint16_t> out;
		out.reserve(m_marked.size());
		for (const auto& kv : m_marked)
			out.push_back(kv.first);
		return out;
	}
	// The session peer that holds it, kNoSession if nobody does.
	uint32_t holder_of(uint16_t id) const
	{
		const auto it = m_marked.find(id);
		return it == m_marked.end() ? kNoSession : session_of(it->second);
	}
	size_t held_by(uint32_t session_peer) const
	{
		const auto it = m_peers.find(session_peer);
		if (it == m_peers.end())
			return 0;
		size_t n = 0;
		for (const auto& kv : m_marked)
			n += kv.second == it->second ? 1u : 0u;
		return n;
	}
	size_t marked() const { return m_marked.size(); }

	std::vector<Change> take_changes()
	{
		std::vector<Change> c;
		c.swap(m_changes);
		return c;
	}

private:
	// The one place of a mark. The ledger is the truth: away = it has the record and the world does not write it.
	void settle(uint16_t id, xfer::Peer peer)
	{
		const bool away = m_ledger.registered(id) && !m_ledger.world_authors(id);
		const auto it = m_marked.find(id);
		if (away && it == m_marked.end())
		{
			m_marked[id] = peer;
			m_changes.push_back(Change{Kind::Mark, id, {}});
		}
		else if (!away && it != m_marked.end())
		{
			m_marked.erase(it);
			if (const xfer::Bytes* st = m_ledger.last_confirmed(id))
				m_changes.push_back(Change{Kind::Unmark, id, *st});
			else
				m_changes.push_back(Change{Kind::Gone, id, {}});
		}
	}

	xfer::Ledger m_ledger;
	std::map<uint32_t, xfer::Peer> m_peers; // session slot -> the ledger's peer of this connection
	std::map<uint16_t, xfer::Peer> m_marked;
	std::vector<Change> m_changes;
};
} // namespace levels
} // namespace coop
