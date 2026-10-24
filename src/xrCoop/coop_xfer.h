// Anomaly Coop - F3a split levels: ownership transfer between the world server and the map servers (plan
// 2026-10-24-f3a §1.7, §4.1 requirements 1-6; tests/test_xfer.cpp, tests/test_xfer_fuzz.cpp).
// The world server (Ledger, in the host's process) owns every object of the world; a map server (Holder, a background
// process on the host's PC, Peer 1..n) owns the objects it runs online. Every change of owner goes through here - the
// one place under the fuzzer (plan §12 decision 6). The unit is one root id (an NPC or a container with its children, a
// squad with its members: the payload carries them); the key is (id, gen).
//
// Take (world -> map):     TakeReq ->  Grant {g, state} (the world freezes it: Granting, journal)
//                          TakeCommit {g} ->  Done {g} (owner = the map server; journal closed)
// Release (map -> world):  the map server freezes it (AI and physics stop, requests: Reason::Moving);
//                          ReleasePrepare {held g, state} ->  Ack {g'} (the state waits in the journal, not applied;
//                          the map server still owns it) -> it deletes the object: ReleaseCommit {g'} ->  Done {g'}
//                          (applied, owner = the world)
// Spawn (a leased id):     SpawnPrepare {0, state} ->  Ack {g} (registered, owner = that map server)
// Destroy:                 Destroy {held g, or 0 for a spawn not answered yet} ->  Done {g'}
// Abort (two-phase too):   Abort {g} (the world stays frozen) ->  AbortAck {g}; a map server that does not want a grant
//                          declines it the same way: AbortAck = "I do not hold (id, g) and never will"
// Stream within ownership: Snapshot {g, seq|state} ->  Snapshot {g, seq} (the ack; seq 0 from the world = "send me
//                          your state", a resync)
// Healing:                 Drop {g}: let go of every incarnation of the id up to g
// Both sides repeat until the answer (kRetryFirstMs doubling to kRetryMaxMs); every answer is idempotent. A grant
// nobody commits for kGrantTimeoutMs is aborted.
//
// Rulings (F3a T2; the reasons are in task-2-report.md):
//   R1 gens: one counter for all ids (the world epoch in the top bits, the next epoch after every load, below the
//      top bit). Every transfer takes a fresh gen and all its replies carry it; a release is keyed by the Ack's new
//      gen, a destroy answered by a new gen, every record - a remade one too - starts with a fresh gen; so a late
//      reply of an earlier transfer never passes for a later one's.
//   R2 a map server declines (AbortAck) only a grant it does not write; it never declines while it holds an older
//      incarnation (Held: the newer grant replaces it; Spawning/Releasing: no answer, the world's abort will come).
//      Every AbortAck - to an Abort in any state, or to a grant declined - is a promise kept by Holder::decline: that
//      gen is never made there later (review C1).
//   R3 SpawnPrepare registers at its arrival (atomic: the record itself goes into the save, "SpawnPrepare:
//      completed"); the journal keeps no spawn entry.
//   R4 a destroy tombstones the id for kTombstoneMs: a late SpawnPrepare or TakeReq gets nothing back; Destroy {0}
//      of a spawn the world has not seen tombstones it for that map server, anew even over an older tombstone that
//      ends sooner. A tombstone must outlive every packet in the network (60 s against an ordered loopback channel;
//      the fuzzer loses every packet older than the run's tombstone less a second).
//   R5 ReleasePrepare or Destroy with the gen of a grant still Granting to that map server is its TakeCommit too (it
//      can only have that gen from the Grant). While Aborting the abort wins over a release (the object stays the
//      world's at its last confirmed state) but a destroy wins over the abort: the map server's engine has destroyed
//      the object, and a destroy is never undone (review round 2, H1) - the world takes Destroy {g} while Aborting
//      {g}, the map server does not confirm the abort of the gen it is destroying.
//   R6 the journal (Granting, Aborting, Releasing) stays within kJournalMaxEntries and kJournalMaxBytes of text: a
//      transfer that does not fit is not started (no Grant, no Ack) - the map server repeats until it fits.
//   R7 a map server is known to the ledger by a peer register_server gives each connection: never reused (the next
//      one is kept in the journal across loads), so nothing of an earlier connection can pass for a later one's. The
//      ledger hears only registered peers that are not lost. server_lost means gone: its process is dead or its
//      connection closed for good and its Holder is never used again. Its objects go back to the world at once (a
//      prepared release completed, a grant or an abort to it resolved as if acknowledged); the Drops it returns are
//      best effort; its late messages are ignored.
//   R8 after a load (resolve_after_load) every map server is gone: Releasing - completed with the journal's state;
//      Granting/Aborting - rolled back; an object owned by a map server - back to the world at its last confirmed
//      state. A Releasing entry whose object the save lacks is recreated from its state; a Granting entry whose
//      object the save lacks is a violation (nothing to roll back to).
//   R9 audit() takes a report that is current: everything the map server sent before it was handled before it (one
//      ordered channel). Transfers in flight are never judged. A Lost heal takes the object back two-phase - Aborting
//      under a new gen, Abort repeated until the map server's AbortAck - so the world writes it again only once the map
//      server has let go: even a stale report cannot leave two writers. A Duplicate heal's Drop is one-shot; the next
//      audit repeats it while the duplicate is there.
//   R10 a late ReleaseCommit of a release the world finished before the object's current transfer gets Done again;
//      nothing is applied twice. Every record, a remade one too, starts with a fresh gen, so this always holds.
//   R11 a Destroy the world does not take (a late copy, an incarnation not given to that map server, an aborted
//      grant) gets Drop: the object is gone there already, the map server only stops asking.
// Pure C++17, header-only, no engine dependencies: used by the engine and the unit-test exe.
#pragma once

#include "coop_xfer_base.h"
#include "coop_xfer_holder.h"
#include "coop_xfer_journal.h"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace coop
{
namespace xfer
{
enum class Phase : uint8_t
{
	Stable,
	Granting, // the world's, frozen, granted to `peer`
	Aborting, // the world's, frozen, the grant to `peer` is being taken back
	Releasing, // `peer`'s, frozen there; the state waits in the journal
};

enum class Violation : uint8_t
{
	Duplicate, // a map server writes an object the ledger does not give it
	Lost, // the ledger gives a map server an object it does not have
	Drift // the owner's state differs from the world's copy
};

struct Finding
{
	Violation kind;
	uint16_t id;
	Peer server; // the map server audited
	uint32_t gen; // Duplicate, Drift: the gen it reported; Lost: the ledger's (or the higher of both)
	Peer owner; // the ledger's owner at the audit
};

class Ledger;
inline std::vector<Finding> audit(const Ledger& ledger, Peer server, const std::vector<Entry>& reported,
	uint32_t buckets = kAllBuckets);
inline std::vector<Packet> heal(Ledger& ledger, const std::vector<Finding>& findings);

// The world server's book: the owner of every id, the transfers in flight (the journal), the last confirmed state of
// each object. Every object of the host's own map is the world's.
class Ledger
{
public:
	struct Counters
	{
		// Journal entries now: transfers Granting, Aborting or Releasing, plus loaded entries not resolved yet.
		uint64_t in_flight;
		// The world's own repeats: Grant and Abort sent again by tick. A map server counts its own (Holder::retries).
		uint64_t retries;
		// Releases completed by a resolution with their state applied: a lost map server's prepared release, a
		// Releasing entry of a loaded journal.
		uint64_t completed;
		// Transfers undone by a resolution: a grant or an abort to a lost map server, a Granting or Aborting entry of a
		// loaded journal, an object a lost map server owned (back at its last confirmed state).
		uint64_t rolled_back;
		// Drifts healed by asking the owner for a snapshot.
		uint64_t resyncs;
		// Duplicates and losses healed, (id, map server) pairs refused a spawn under a taken id, Granting entries of a
		// loaded journal whose object the save lacks.
		uint64_t violations;
	};

	// The ledger's limits: the plan's values by default; smaller ones let the fuzzer reach them.
	struct Limits
	{
		size_t journal_entries = kJournalMaxEntries; // at most kJournalMaxEntries
		size_t journal_bytes = kJournalMaxBytes; // at most kJournalMaxBytes
		uint32_t tombstone_ms = kTombstoneMs;
		uint32_t grant_timeout_ms = kGrantTimeoutMs;
	};

	Ledger() = default;
	explicit Ledger(const Limits& limits) : m_limits(limits)
	{
		m_limits.journal_entries = std::min(m_limits.journal_entries, kJournalMaxEntries);
		m_limits.journal_bytes = std::min(m_limits.journal_bytes, kJournalMaxBytes);
	}

	// A map server connected: the peer the ledger knows it by from now on (R7). Never reused - the engine maps the
	// session slot of the connection to it, and a new connection in the same slot gets a new peer.
	Peer register_server()
	{
		assert(m_next_peer != 0 && "xfer: the peer counter is exhausted");
		const Peer p = m_next_peer++;
		m_live.insert(p);
		return p;
	}

	// An object of the world (start, load, the world's own spawn). Creates it owned by the world; an existing one keeps
	// its owner and takes the state only when the world writes it. Clears a tombstone of the id.
	void add_world_object(uint16_t id, std::vector<uint8_t> state)
	{
		m_dead.erase(id);
		auto it = m_recs.find(id);
		if (it == m_recs.end())
		{
			make_world_record(id, std::move(state)); // R10: a fresh gen, above every gen of the id's earlier life
			return;
		}
		if (it->second.owner == kWorld && it->second.phase == Phase::Stable)
			it->second.state = std::move(state);
	}

	// The world's own simulation changed an object it writes (offline, or on the host's map). False when it does not
	// write it (at a map server, or frozen for a transfer).
	bool world_update(uint16_t id, std::vector<uint8_t> state)
	{
		Rec* r = find(id);
		if (!r || r->owner != kWorld || r->phase != Phase::Stable)
			return false;
		r->state = std::move(state);
		return true;
	}

	// The world destroys an object it writes.
	bool world_destroy(uint16_t id, uint64_t now)
	{
		Rec* r = find(id);
		if (!r || r->owner != kWorld || r->phase != Phase::Stable)
			return false;
		bury(id, kWorld, r->gen, now);
		return true;
	}

	std::vector<Packet> on_packet(const Packet& p, uint64_t now)
	{
		std::vector<Packet> out;
		if (p.from == kWorld || p.to != kWorld || !m_live.count(p.from))
			return out; // R7: only registered map servers that are not lost are heard
		switch (p.op)
		{
		case Op::TakeReq: on_take_req(p, now, out); break;
		case Op::TakeCommit: on_take_commit(p, out); break;
		case Op::AbortAck: on_abort_ack(p); break;
		case Op::ReleasePrepare: on_release_prepare(p, out); break;
		case Op::ReleaseCommit: on_release_commit(p, out); break;
		case Op::SpawnPrepare: on_spawn_prepare(p, out); break;
		case Op::Destroy: on_destroy(p, now, out); break;
		case Op::Snapshot: on_snapshot(p, out); break;
		default: break; // the world's own messages are not taken from a map server
		}
		return out;
	}

	// Repeats grants and aborts, aborts grants nobody commits, forgets old tombstones.
	std::vector<Packet> tick(uint64_t now)
	{
		std::vector<Packet> out;
		std::vector<uint16_t> gone;
		for (uint16_t id : m_busy)
		{
			Rec& r = m_recs.at(id);
			if (!m_live.count(r.peer))
			{
				gone.push_back(id); // cannot happen through server_lost (it resolves them); kept as a guard
				continue;
			}
			if (r.next_retry == 0) // a heal's abort: its first repeat counts from the first tick
			{
				r.next_retry = now + detail::backoff(0);
				if (r.phase == Phase::Aborting)
					continue;
			}
			if (r.phase == Phase::Granting && now >= r.since + m_limits.grant_timeout_ms)
			{
				r.phase = Phase::Aborting;
				arm(r, now);
				out.push_back(detail::packet(Op::Abort, id, r.gen, kWorld, r.peer));
			}
			else if ((r.phase == Phase::Granting || r.phase == Phase::Aborting) && now >= r.next_retry)
			{
				++r.tries;
				r.next_retry = now + detail::backoff(r.tries);
				++m_counters.retries;
				out.push_back(r.phase == Phase::Granting ? grant(id, r) :
				                                           detail::packet(Op::Abort, id, r.gen, kWorld, r.peer));
			}
			// Releasing: the map server drives it
		}
		for (uint16_t id : gone)
			resolve_lost(id, m_recs.at(id), m_recs.at(id).peer);
		if (now >= m_next_sweep)
		{
			m_next_sweep = now + 1000;
			for (auto it = m_dead.begin(); it != m_dead.end();)
			{
				if (now >= it->second.at + m_limits.tombstone_ms)
					it = m_dead.erase(it);
				else
					++it;
			}
		}
		return out;
	}

	// The world takes back a grant not committed yet (two-phase: frozen until AbortAck).
	std::vector<Packet> abort(uint16_t id, uint64_t now)
	{
		Rec* r = find(id);
		if (!r || r->phase != Phase::Granting)
			return {};
		r->phase = Phase::Aborting;
		arm(*r, now);
		return {detail::packet(Op::Abort, id, r->gen, kWorld, r->peer)};
	}

	// A map server is gone (R7): a release it prepared is completed, a grant to it rolled back, its objects go back to
	// the world at their last confirmed state, under a new gen. Returns Drops for it (best effort).
	std::vector<Packet> server_lost(Peer server, uint64_t now)
	{
		(void)now;
		std::vector<Packet> out;
		if (server == kWorld)
			return out;
		m_live.erase(server);
		std::vector<uint16_t> ids;
		for (const auto& kv : m_recs)
			if (kv.second.owner == server || (kv.second.phase != Phase::Stable && kv.second.peer == server))
				ids.push_back(kv.first);
		for (uint16_t id : ids)
		{
			Rec& r = m_recs.at(id);
			const uint32_t held = r.gen;
			resolve_lost(id, r, server);
			out.push_back(detail::packet(Op::Drop, id, held, kWorld, server));
		}
		return out;
	}

	// The [coop_xfer] section of the host actor's custom data: every transfer in flight (R6 limits).
	//   [coop_xfer]
	//   v = 1
	//   gen = <the gen counter>
	//   n = <entries>
	//   x<i> = <G|A|R>,<id>,<map server>,<gen>,<gen it held before the release>,<state in base64, R only>
	std::string save_journal() const
	{
		std::vector<JournalEntry> list;
		for (uint16_t id : m_busy)
		{
			const Rec& r = m_recs.at(id);
			list.push_back(JournalEntry{kind_of(r.phase), id, r.peer, r.gen, r.from_gen, r.pending});
		}
		for (const JournalEntry& j : m_loaded)
			list.push_back(j);
		return write_journal(m_gen, m_next_peer, std::move(list));
	}

	// Reads a journal written by save_journal (other sections may surround [coop_xfer]); the entries wait for
	// resolve_after_load. False, and nothing taken, for anything malformed.
	bool load_journal(const std::string& text)
	{
		uint32_t gen = 0;
		Peer next_peer = 0;
		std::vector<JournalEntry> loaded;
		if (!read_journal(text, gen, next_peer, loaded))
			return false;
		m_loaded = std::move(loaded);
		m_gen = std::max(m_gen, gen);
		m_next_peer = std::max(m_next_peer, next_peer);
		return true;
	}

	// After a load, before the first tick (R8): every map server is gone; the journal is resolved and emptied; a new
	// epoch of gens begins.
	void resolve_after_load()
	{
		// Every map server is gone: whatever this ledger still had with them goes back to the world first ...
		std::set<Peer> peers;
		for (const auto& kv : m_recs)
		{
			if (kv.second.owner != kWorld)
				peers.insert(kv.second.owner);
			if (kv.second.phase != Phase::Stable)
				peers.insert(kv.second.peer);
		}
		for (Peer p : peers)
			server_lost(p, 0);
		// ... so every record is the world's and settled, and each loaded entry applies.
		for (JournalEntry& j : m_loaded)
		{
			Rec* r = find(j.id);
			if (j.kind == 'R')
			{
				if (r)
					r->state = std::move(j.payload);
				else
					make_world_record(j.id, std::move(j.payload));
				++m_counters.completed;
			}
			else
			{
				if (!r)
					++m_counters.violations; // a grant of an object the save does not have: nothing to roll back to
				++m_counters.rolled_back;
			}
		}
		m_loaded.clear();
		m_dead.clear();
		m_live.clear(); // a load starts a new world: every map server registers anew (with a new peer)
		// A new epoch below the top bit: each session starts at most at 127 << kEpochShift and has more than 2^31 gens
		// ahead (one per transfer - centuries at any real rate), so next_gen never wraps.
		const uint64_t epoch = (uint64_t(m_gen) >> kEpochShift) + 1;
		m_gen = uint32_t((epoch < 128 ? epoch : 1) << kEpochShift);
	}

	// How many of the ledger's owner sets hold `id`: exactly 1 for a registered id, 0 otherwise.
	uint16_t owner_count(uint16_t id) const
	{
		uint16_t n = 0;
		for (const auto& kv : m_owned)
			n = uint16_t(n + kv.second.count(id));
		return n;
	}
	// kWorld for an id the ledger does not have too: see registered().
	Peer owner(uint16_t id) const
	{
		const Rec* r = find(id);
		return r ? r->owner : kWorld;
	}
	uint32_t gen(uint16_t id) const
	{
		const Rec* r = find(id);
		return r ? r->gen : 0;
	}
	const std::vector<uint8_t>* last_confirmed(uint16_t id) const
	{
		const Rec* r = find(id);
		return r ? &r->state : nullptr;
	}
	bool registered(uint16_t id) const { return find(id) != nullptr; }
	Phase phase(uint16_t id) const
	{
		const Rec* r = find(id);
		return r ? r->phase : Phase::Stable;
	}
	// The map server of a transfer in flight (kWorld when none).
	Peer peer(uint16_t id) const
	{
		const Rec* r = find(id);
		return r && r->phase != Phase::Stable ? r->peer : kWorld;
	}
	// In a transfer: the world's simulation and other requests leave it alone.
	bool frozen(uint16_t id) const { return phase(id) != Phase::Stable; }
	// The world writes it: its own and not frozen.
	bool world_authors(uint16_t id) const
	{
		const Rec* r = find(id);
		return r && r->owner == kWorld && r->phase == Phase::Stable;
	}
	std::vector<uint16_t> ids() const
	{
		std::vector<uint16_t> v;
		v.reserve(m_recs.size());
		for (const auto& kv : m_recs)
			v.push_back(kv.first);
		return v;
	}
	// What `server` should report (Holder::report): its objects with the gen it holds and the digest of its state. The
	// engine overwrites parent and alive from its copies (the core fills kNoParent and true on both sides).
	std::vector<Entry> owned_by(Peer server) const
	{
		std::vector<Entry> v;
		auto it = m_owned.find(server);
		if (server == kWorld || it == m_owned.end())
			return v;
		for (uint16_t id : it->second)
		{
			const Rec& r = m_recs.at(id);
			const bool releasing = r.phase == Phase::Releasing;
			const uint64_t d = digest(releasing ? r.pending : r.state);
			v.push_back(Entry{id, releasing ? r.from_gen : r.gen, kNoParent, true, d});
		}
		return v;
	}
	size_t journal_size() const { return m_busy.size() + m_loaded.size(); }
	Counters counters() const
	{
		Counters c = m_counters;
		c.in_flight = journal_size();
		return c;
	}

private:
	friend std::vector<Finding> audit(const Ledger&, Peer, const std::vector<Entry>&, uint32_t);
	friend std::vector<Packet> heal(Ledger&, const std::vector<Finding>&);

	struct Rec
	{
		Peer owner = kWorld;
		uint32_t gen = 0; // the gen of the last transfer (0: never moved)
		Phase phase = Phase::Stable;
		Peer peer = kWorld; // Granting/Aborting: the grantee; Releasing: the releasing map server
		uint32_t from_gen = 0; // Releasing: the gen the map server held (its ReleasePrepare's key)
		std::vector<uint8_t> state; // the last confirmed state
		std::vector<uint8_t> pending; // Releasing: the state kept in the journal
		uint32_t snap_seq = 0; // the newest Snapshot taken in this gen
		uint32_t spawn_gen = 0; // registered by a SpawnPrepare under this gen (0: not a spawn)
		Peer spawned_by = kWorld;
		Peer refused = kWorld; // the last map server refused a spawn under this id
		uint64_t since = 0, next_retry = 0;
		uint32_t tries = 0;
		size_t bytes = 0; // journal text reserved
	};
	struct Tomb
	{
		uint32_t gen; // the destroy's gen (its Done)
		Peer by;
		uint32_t from_gen; // the gen the Destroy named
		uint64_t at;
	};

	std::map<uint16_t, Rec> m_recs;
	std::map<Peer, std::set<uint16_t>> m_owned; // owner -> ids
	std::set<uint16_t> m_busy; // ids in a transfer (the journal)
	std::set<Peer> m_live; // registered map servers not lost (R7): the only ones heard
	Peer m_next_peer = 1; // never reused (kWorld = 0 is the world)
	std::map<uint16_t, Tomb> m_dead;
	std::vector<JournalEntry> m_loaded; // read by load_journal, waiting for resolve_after_load
	uint32_t m_gen = 0;
	size_t m_journal_bytes = detail::kHeaderBytes;
	uint64_t m_next_sweep = 0;
	Counters m_counters{};
	Limits m_limits;

	static char kind_of(Phase p) { return p == Phase::Releasing ? 'R' : p == Phase::Aborting ? 'A' : 'G'; }

	Rec* find(uint16_t id)
	{
		auto it = m_recs.find(id);
		return it == m_recs.end() ? nullptr : &it->second;
	}
	const Rec* find(uint16_t id) const
	{
		auto it = m_recs.find(id);
		return it == m_recs.end() ? nullptr : &it->second;
	}

	// A fresh gen. Never wraps in practice: a session starts below 2^31 (resolve_after_load), and 2^31 transfers do not
	// happen; the assert says so in debug builds.
	uint32_t next_gen()
	{
		assert(m_gen != 0xffffffffu && "xfer: the gen counter is exhausted");
		++m_gen;
		if (!m_gen)
			m_gen = uint32_t(1) << kEpochShift;
		return m_gen;
	}

	void make_world_record(uint16_t id, std::vector<uint8_t> state)
	{
		Rec r;
		r.gen = next_gen();
		r.state = std::move(state);
		m_recs.emplace(id, std::move(r));
		m_owned[kWorld].insert(id);
	}

	// `r` was with map server `server`, which is gone (R7): a prepared release is completed with its state, a grant or
	// an abort is resolved as if acknowledged, an object it owned goes back to the world at its last confirmed state.
	void resolve_lost(uint16_t id, Rec& r, Peer server)
	{
		if (r.phase == Phase::Releasing && r.peer == server)
		{
			r.state = std::move(r.pending);
			settle(id, r);
			set_owner(id, r, kWorld);
			r.gen = next_gen();
			++m_counters.completed;
		}
		else if ((r.phase == Phase::Granting || r.phase == Phase::Aborting) && r.peer == server)
		{
			settle(id, r);
			++m_counters.rolled_back;
		}
		else if (r.phase == Phase::Stable && r.owner == server)
		{
			set_owner(id, r, kWorld);
			r.gen = next_gen();
			++m_counters.rolled_back;
		}
		r.snap_seq = 0;
	}

	static void arm(Rec& r, uint64_t now)
	{
		r.tries = 0;
		r.next_retry = now + detail::backoff(0);
	}

	// The journal takes `entries` more and `bytes` more text (R6).
	bool fits(size_t entries, size_t bytes) const
	{
		return m_busy.size() + m_loaded.size() + entries <= m_limits.journal_entries &&
		       m_journal_bytes + bytes <= m_limits.journal_bytes;
	}

	void open(uint16_t id, Rec& r, size_t bytes)
	{
		m_journal_bytes += bytes;
		r.bytes += bytes;
		m_busy.insert(id);
	}

	// The transfer of `r` is over: out of the journal, not frozen.
	void settle(uint16_t id, Rec& r)
	{
		m_journal_bytes -= r.bytes;
		r.bytes = 0;
		r.phase = Phase::Stable;
		r.peer = kWorld;
		r.from_gen = 0;
		r.pending.clear();
		m_busy.erase(id);
	}

	void set_owner(uint16_t id, Rec& r, Peer owner)
	{
		auto it = m_owned.find(r.owner);
		if (it != m_owned.end())
		{
			it->second.erase(id);
			if (it->second.empty())
				m_owned.erase(it);
		}
		r.owner = owner;
		m_owned[owner].insert(id);
	}

	// Removes `id` and leaves a tombstone; returns the destroy's gen.
	uint32_t bury(uint16_t id, Peer by, uint32_t from_gen, uint64_t now)
	{
		Rec& r = m_recs.at(id);
		if (r.phase != Phase::Stable)
			settle(id, r);
		auto it = m_owned.find(r.owner);
		if (it != m_owned.end())
		{
			it->second.erase(id);
			if (it->second.empty())
				m_owned.erase(it);
		}
		m_recs.erase(id);
		const uint32_t g = next_gen();
		m_dead[id] = Tomb{g, by, from_gen, now};
		return g;
	}

	Packet grant(uint16_t id, const Rec& r) const
	{
		return detail::packet(Op::Grant, id, r.gen, kWorld, r.peer, r.state);
	}

	void on_take_req(const Packet& p, uint64_t now, std::vector<Packet>& out)
	{
		const uint16_t id = p.key.id;
		Rec* r = find(id);
		if (!r)
		{
			// No such object (destroyed, or never made): refused with gen 0, so the map server stops asking. Gen 0
			// says "no incarnation of yours": a refusal that arrives late, when that map server spawns the id
			// meanwhile, must not read as taking the spawn back (an Abort with a gen does - round 2, item 3).
			out.push_back(detail::packet(Op::Abort, id, 0, kWorld, p.from));
			return;
		}
		if (r->phase == Phase::Granting && r->peer == p.from)
		{
			out.push_back(grant(id, *r)); // the request repeated: the same grant
			return;
		}
		if (r->phase != Phase::Stable || r->owner != kWorld)
			return; // busy (in a transfer, or at a map server): the map server repeats
		const size_t bytes = detail::kEntryBytes;
		if (!fits(1, bytes))
			return; // R6: the journal is full, the map server repeats
		r->gen = next_gen();
		r->phase = Phase::Granting;
		r->peer = p.from;
		r->since = now;
		arm(*r, now);
		open(id, *r, bytes);
		out.push_back(grant(id, *r));
	}

	void on_take_commit(const Packet& p, std::vector<Packet>& out)
	{
		const uint16_t id = p.key.id;
		Rec* r = find(id);
		if (!r || r->gen != p.key.gen)
			return;
		if (r->phase == Phase::Granting && r->peer == p.from)
		{
			settle(id, *r);
			set_owner(id, *r, p.from);
			r->snap_seq = 0;
			out.push_back(detail::packet(Op::Done, id, r->gen, kWorld, p.from));
		}
		else if (r->phase == Phase::Stable && r->owner == p.from)
			out.push_back(detail::packet(Op::Done, id, r->gen, kWorld, p.from)); // repeated
		// Aborting: the abort wins, the map server will get Abort (R5)
	}

	void on_abort_ack(const Packet& p)
	{
		const uint16_t id = p.key.id;
		Rec* r = find(id);
		const bool in_grant = r && (r->phase == Phase::Granting || r->phase == Phase::Aborting);
		if (in_grant && r->peer == p.from && r->gen == p.key.gen)
			settle(id, *r); // the world's again, at its last confirmed state
	}

	void on_release_prepare(const Packet& p, std::vector<Packet>& out)
	{
		const uint16_t id = p.key.id;
		Rec* r = find(id);
		if (!r)
			return;
		if (r->phase == Phase::Releasing && r->peer == p.from && r->from_gen == p.key.gen)
		{
			out.push_back(detail::packet(Op::Ack, id, r->gen, kWorld, p.from)); // repeated
			return;
		}
		const bool owns = r->phase == Phase::Stable && r->owner == p.from && r->gen == p.key.gen;
		const bool granted = r->phase == Phase::Granting && r->peer == p.from && r->gen == p.key.gen; // R5
		if (!owns && !granted)
			return;
		const size_t bytes = journal_entry_bytes(p.payload);
		if (!fits(granted ? 0 : 1, bytes - (granted ? r->bytes : 0)))
			return; // R6: the map server repeats
		if (granted)
		{
			settle(id, *r);
			set_owner(id, *r, p.from);
		}
		r->from_gen = r->gen;
		r->gen = next_gen();
		r->phase = Phase::Releasing;
		r->peer = p.from;
		r->pending = p.payload;
		open(id, *r, bytes);
		out.push_back(detail::packet(Op::Ack, id, r->gen, kWorld, p.from));
	}

	void on_release_commit(const Packet& p, std::vector<Packet>& out)
	{
		const uint16_t id = p.key.id;
		Rec* r = find(id);
		if (r && r->phase == Phase::Releasing && r->peer == p.from && r->gen == p.key.gen)
		{
			r->state = std::move(r->pending);
			settle(id, *r);
			set_owner(id, *r, kWorld);
			r->snap_seq = 0;
			out.push_back(detail::packet(Op::Done, id, p.key.gen, kWorld, p.from));
			return;
		}
		// Not the release in flight: one the world finished before the object's current transfer (applied, then maybe
		// moved on or destroyed). Answered again so the map server stops asking; nothing is applied twice. Gens of the
		// current transfer, and gens the world never gave, are not answered.
		const uint32_t x = p.key.gen;
		bool past = x != 0 && x <= m_gen;
		if (past && r)
			past = r->phase == Phase::Stable ? x <= r->gen : x < (r->phase == Phase::Releasing ? r->from_gen : r->gen);
		if (past)
			out.push_back(detail::packet(Op::Done, id, x, kWorld, p.from));
	}

	void on_spawn_prepare(const Packet& p, std::vector<Packet>& out)
	{
		const uint16_t id = p.key.id;
		Rec* r = find(id);
		if (r)
		{
			if (r->spawned_by == p.from && r->spawn_gen != 0)
			{
				// Its own spawn: answered again while nothing moved it; a late copy after that is ignored.
				if (r->phase == Phase::Stable && r->owner == p.from && r->gen == r->spawn_gen)
					out.push_back(detail::packet(Op::Ack, id, r->gen, kWorld, p.from));
				return;
			}
			// The id is someone else's (a lease fault): refused, the map server lets go of its object. One violation
			// per map server and id, however often it repeats.
			if (r->refused != p.from)
			{
				r->refused = p.from;
				++m_counters.violations;
			}
			out.push_back(detail::packet(Op::Drop, id, 0, kWorld, p.from));
			return;
		}
		if (m_dead.count(id))
			return; // R4: destroyed lately - a late copy of a spawn; a real reuse waits for the tombstone to go
		Rec n;
		n.owner = p.from;
		n.gen = next_gen();
		n.spawn_gen = n.gen;
		n.spawned_by = p.from;
		n.state = p.payload;
		const uint32_t g = n.gen;
		m_recs.emplace(id, std::move(n));
		m_owned[p.from].insert(id);
		out.push_back(detail::packet(Op::Ack, id, g, kWorld, p.from));
	}

	void on_destroy(const Packet& p, uint64_t now, std::vector<Packet>& out)
	{
		const uint16_t id = p.key.id;
		Rec* r = find(id);
		if (r)
		{
			const bool own_spawn =
				p.key.gen == 0 && r->spawned_by == p.from && r->spawn_gen != 0 && r->gen == r->spawn_gen;
			const bool owns = r->phase == Phase::Stable && r->owner == p.from && (p.key.gen == r->gen || own_spawn);
			const bool granted = (r->phase == Phase::Granting || r->phase == Phase::Aborting) && r->peer == p.from &&
			                     p.key.gen != 0 && r->gen == p.key.gen;
			if (owns || granted) // R5: a destroy of the incarnation granted is its commit too, and wins over an abort
			{
				const uint32_t g = bury(id, p.from, p.key.gen, now);
				out.push_back(detail::packet(Op::Done, id, g, kWorld, p.from));
				return;
			}
		}
		else
		{
			auto d = m_dead.find(id);
			if (d != m_dead.end() && d->second.by == p.from && d->second.from_gen == p.key.gen)
			{
				out.push_back(detail::packet(Op::Done, id, d->second.gen, kWorld, p.from)); // repeated
				return;
			}
			if (p.key.gen == 0)
			{
				// R4: destroyed before the world saw the spawn - tombstoned for this spawn (anew, even over an older
				// tombstone of the id that ends sooner), so a late SpawnPrepare copy registers nothing.
				const uint32_t g = next_gen();
				m_dead[id] = Tomb{g, p.from, 0, now};
				out.push_back(detail::packet(Op::Done, id, g, kWorld, p.from));
				return;
			}
		}
		// Not a destroy the world can take (a late copy, an incarnation it does not give this map server, an aborted
		// grant): the object is gone there already, so the map server only has to stop asking. Nothing changes here.
		out.push_back(detail::packet(Op::Drop, id, p.key.gen, kWorld, p.from));
	}

	void on_snapshot(const Packet& p, std::vector<Packet>& out)
	{
		const uint16_t id = p.key.id;
		Rec* r = find(id);
		if (!r || r->phase != Phase::Stable || r->owner != p.from || r->gen != p.key.gen)
			return;
		uint32_t seq = 0;
		std::vector<uint8_t> state;
		if (!read_snapshot(p.payload, seq, state) || seq == 0)
			return;
		if (seq > r->snap_seq)
		{
			r->state = std::move(state);
			r->snap_seq = seq;
		}
		out.push_back(detail::packet(Op::Snapshot, id, r->gen, kWorld, p.from, snapshot_payload(r->snap_seq, {})));
	}
};

// The auditor (R9): compares what `server` reports (its objects in `buckets`) with the ledger.
//   Duplicate - it writes an object the ledger gives to someone else, or one the ledger does not have;
//   Lost      - the ledger gives it an object (settled, in `buckets`) it does not report, or under another gen;
//   Drift     - right owner and gen, but the state's digest differs from the world's copy.
inline std::vector<Finding> audit(const Ledger& ledger, Peer server, const std::vector<Entry>& reported,
	uint32_t buckets)
{
	std::vector<Finding> f;
	if (!ledger.m_live.count(server))
		return f; // R7: not registered or gone, nothing of it counts
	std::set<uint16_t> seen;
	for (const Entry& e : reported)
	{
		if (!(buckets & (1u << bucket_of(e.id))))
			continue;
		seen.insert(e.id);
		const Ledger::Rec* r = ledger.find(e.id);
		if (!r)
		{
			if (e.gen != 0) // gen 0: a spawn the world has not seen yet
				f.push_back(Finding{Violation::Duplicate, e.id, server, e.gen, kWorld});
			continue;
		}
		if (r->owner == server)
		{
			if (r->phase == Phase::Releasing)
				continue; // frozen there until the Ack: in flight
			if (e.gen == 0 && r->spawned_by == server && r->gen == r->spawn_gen)
				continue; // its spawn, the Ack on its way
			if (e.gen != r->gen)
				f.push_back(Finding{Violation::Lost, e.id, server, std::max(e.gen, r->gen), server});
			else if (e.digest != 0 && e.digest != digest(r->state))
				f.push_back(Finding{Violation::Drift, e.id, server, e.gen, server});
			continue;
		}
		if ((r->phase == Phase::Granting || r->phase == Phase::Aborting) && r->peer == server && e.gen == r->gen)
			continue; // made from the grant, the commit or the abort on its way
		f.push_back(Finding{Violation::Duplicate, e.id, server, e.gen, r->owner});
	}
	auto owned = ledger.m_owned.find(server);
	if (server != kWorld && owned != ledger.m_owned.end())
		for (uint16_t id : owned->second)
		{
			if (!(buckets & (1u << bucket_of(id))) || seen.count(id))
				continue;
			const Ledger::Rec& r = ledger.m_recs.at(id);
			if (r.phase == Phase::Stable)
				f.push_back(Finding{Violation::Lost, id, server, r.gen, server});
		}
	return f;
}

// Self-healing of audit findings (each is checked again against the ledger first).
//   Duplicate - Drop at the illegal writer; the legal owner, a map server, is asked for a snapshot (violation).
//   Lost      - back to the world from its last confirmed state under a new gen, Drop at the map server (violation).
//   Drift     - the owner is asked for a snapshot (resync).
inline std::vector<Packet> heal(Ledger& ledger, const std::vector<Finding>& findings)
{
	std::vector<Packet> out;
	for (const Finding& f : findings)
	{
		Ledger::Rec* r = ledger.find(f.id);
		switch (f.kind)
		{
		case Violation::Duplicate:
			if (!ledger.m_live.count(f.server))
				break; // gone (R7)
			if (r && r->owner == f.server && !(r->phase == Phase::Stable && r->gen != f.gen))
				break; // it is the owner now
			++ledger.m_counters.violations;
			out.push_back(detail::packet(Op::Drop, f.id, f.gen, kWorld, f.server));
			if (r && r->owner != kWorld && r->owner != f.server && r->phase == Phase::Stable)
			{
				r->snap_seq = 0;
				out.push_back(detail::packet(Op::Snapshot, f.id, r->gen, kWorld, r->owner, snapshot_payload(0, {})));
			}
			break;
		case Violation::Lost:
			if (!r || r->owner != f.server || r->phase != Phase::Stable || !ledger.m_live.count(f.server))
				break; // moved since, or the map server is gone (server_lost has done it)
			if (!ledger.fits(1, detail::kEntryBytes))
				break; // R6: the journal is full - the next audit finds it again
			// R9: taken back two-phase - Aborting under a new gen (above any gen the map server can hold), Abort
			// repeated by tick until AbortAck; the world writes it again only then.
			++ledger.m_counters.violations;
			ledger.set_owner(f.id, *r, kWorld);
			r->gen = ledger.next_gen();
			r->phase = Phase::Aborting;
			r->peer = f.server;
			r->since = 0;
			r->tries = 0;
			r->next_retry = 0; // armed by the next tick
			r->snap_seq = 0;
			ledger.open(f.id, *r, detail::kEntryBytes);
			out.push_back(detail::packet(Op::Abort, f.id, r->gen, kWorld, f.server));
			break;
		case Violation::Drift:
			if (!r || r->owner != f.server || r->phase != Phase::Stable || r->gen != f.gen ||
			    !ledger.m_live.count(f.server))
				break;
			++ledger.m_counters.resyncs;
			r->snap_seq = 0;
			out.push_back(detail::packet(Op::Snapshot, f.id, r->gen, kWorld, f.server, snapshot_payload(0, {})));
			break;
		}
	}
	return out;
}

// The log line of a finding: "!! [coop] ownership violation: ..." for a duplicate or a loss.
inline std::string describe(const Finding& f)
{
	char buf[192];
	switch (f.kind)
	{
	case Violation::Duplicate:
		std::snprintf(buf, sizeof(buf),
			"!! [coop] ownership violation: duplicate - map server %u writes id %u (gen 0x%08x), the ledger's "
			"owner is %u",
			unsigned(f.server), unsigned(f.id), unsigned(f.gen), unsigned(f.owner));
		break;
	case Violation::Lost:
		std::snprintf(buf, sizeof(buf),
			"!! [coop] ownership violation: lost - map server %u lacks id %u (gen 0x%08x), back to the world from "
			"its last confirmed state",
			unsigned(f.server), unsigned(f.id), unsigned(f.gen));
		break;
	case Violation::Drift:
		std::snprintf(buf, sizeof(buf),
			"[coop] levels: drift - id %u at map server %u differs from the world's copy, resync", unsigned(f.id),
			unsigned(f.server));
		break;
	}
	return buf;
}
} // namespace xfer
} // namespace coop
