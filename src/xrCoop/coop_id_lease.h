// Anomaly Coop — leased object ids (F2 T12b, Ruling F2-8; docs/research/f2-client-create-ids.md §5).
// A client that creates an item in its own inventory (alife():create, create(.., false) + register, clone_weapon) gets an
// object at once, under an id the host lent it beforehand; the host creates the item with that same id (ReqSpawn marker
// 0xC7, coop_spawn.h) or refuses, and the client then takes its object back.
//   Host: takes free ids out of its own generator (xrServer PerformIDgen(0xffff)) and leases kLeaseSize of them to each
//   player in the world (Msg::IdLease); a spent one is replaced 1:1. HostLeases knows who holds which id: only the
//   holder may create with it, once. The ids belong to the host world they came from: a new world (a load, a level
//   change) voids every lease without freeing anything (the new generator never had them).
//   Client: reserves each leased id in its own generator (a local object must not take it), gives back what it cannot
//   reserve or what a refused create leaves (Msg::IdLeaseAck); ClientPool hands them out, SpecBook follows each create
//   until the host answers: accepted - done; RateLimited - asked again; any other refusal - taken back.
// Pure C++17, header-only, no engine dependencies: used by the engine and by the unit-test exe.
#pragma once

#include "coop_protocol.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <initializer_list>
#include <map>
#include <set>
#include <string_view>
#include <utility>
#include <vector>

namespace coop
{
namespace lease
{
constexpr uint16_t kNoId = 0xffff;
constexpr size_t kLeaseSize = 48; // ids a client holds: 7 clients x 48 = 336 of 65534
constexpr size_t kMaxIdsPerMsg = 64; // IdLease / IdLeaseAck carry at most this many
constexpr uint32_t kMinFreeBlocks = 4; // the host leases nothing while its generator has fewer non-empty blocks
constexpr size_t kMaxGivenBack = 4 * kLeaseSize; // per player and world: past this he gets no new ids (churn)
constexpr uint32_t kRetryFirstMs = 100; // RateLimited: the same id asked again after this, ...
constexpr uint32_t kRetryStepMs = 50; // ... a little later each time, ...
constexpr uint32_t kRetryMaxMs = 250; // ... never later than this
constexpr uint32_t kMaxRetries = 40; // about 10 s of RateLimited answers: then the create is taken back
constexpr uint32_t kAckGapMs = 250; // a client sends at most one IdLeaseAck per this (the host's row: 8, 4 a second)
// Round 2 (review N1): a request the host neither answered nor queued in kAnswerTimeoutMs is asked again with the same id
// (coop_requests.cpp kResultTimeoutMs), kMaxResends times; then the create is taken back.
constexpr uint64_t kAnswerTimeoutMs = 10000;
constexpr uint32_t kMaxResends = 3;
// Round 2 (review N1): a client sends ReqSpawn no faster than the host takes it (coop_requests.cpp CatSpawn: 20 a second
// after 40 in a row; the session's row {ReqSpawn, 80, 25} drops what is over it without an answer): half the host's
// burst in a row, one per kSpawnPaceMs after (a little slower than its 50 ms: the network bunches messages up).
constexpr uint32_t kSpawnPaceBurst = 20;
constexpr uint32_t kSpawnPaceMs = 60;

// 0 is the actor of every machine, 0xffff no id: neither is ever leased.
inline bool leasable(uint16_t id) { return id != 0 && id != kNoId; }

// The wait before the next try of a create the host answered RateLimited `tries` times.
inline uint32_t retry_delay(uint32_t tries)
{
	const uint64_t d = uint64_t(kRetryFirstMs) + uint64_t(kRetryStepMs) * (tries ? tries - 1 : 0);
	return d < kRetryMaxMs ? uint32_t(d) : kRetryMaxMs;
}

// What the cost budget (coop_spawn.h) charges for a clone of a weapon: the host releases the source with the clone's
// creation, so only what the clone is worth more counts (a scope variant); a detached scope comes as an item of its own.
inline uint32_t clone_cost(uint32_t clone, uint32_t source) { return clone > source ? clone - source : 0; }

// Round 2 (review N3): a clone keeps to its weapon's variants - the same section, or one base (parent_section; a section
// without one is its own base), or one is the other's base: a scope or magnifier variant, a jam cleared. Another weapon
// would turn the source's rounds into its own ammo for the price difference.
inline bool clone_family(std::string_view source, std::string_view source_parent, std::string_view clone,
    std::string_view clone_parent)
{
	const std::string_view source_base = source_parent.empty() ? source : source_parent;
	const std::string_view clone_base = clone_parent.empty() ? clone : clone_parent;
	return source == clone || source_base == clone_base || source_base == clone || clone_base == source;
}

// Round 2 (review N1): the client's ReqSpawn pace (kSpawnPaceBurst in a row, then one per kSpawnPaceMs); a token bucket.
class SpawnPacer
{
public:
	bool take(uint64_t now)
	{
		if (!m_started)
		{
			m_started = true;
			m_tokens = double(kSpawnPaceBurst);
			m_at = now;
		}
		else if (now > m_at)
		{
			m_tokens += double(now - m_at) / double(kSpawnPaceMs);
			if (m_tokens > double(kSpawnPaceBurst))
				m_tokens = double(kSpawnPaceBurst);
			m_at = now;
		}
		if (m_tokens < 1.)
			return false;
		m_tokens -= 1.;
		return true;
	}
	void reset() { m_started = false; }

private:
	bool m_started = false;
	double m_tokens = 0.;
	uint64_t m_at = 0;
};

// Msg::IdLease (host -> client: ids added to his lease) and Msg::IdLeaseAck (client -> host: ids he gives back).
// Wire: u32 epoch (the host world, join::host_world_epoch), varu n (<= kMaxIdsPerMsg), n x u16 id.
struct IdsMsg
{
	uint32_t epoch = 0;
	std::vector<uint16_t> ids;

	void write(Writer& w) const
	{
		w.u32(epoch);
		w.varu(ids.size());
		for (uint16_t id : ids)
			w.u16(id);
	}
	// False (ids cleared) on overrun, more than kMaxIdsPerMsg ids, an id that cannot be leased or one given twice.
	bool read(Reader& r)
	{
		ids.clear();
		epoch = r.u32();
		const uint64_t n = r.varu();
		if (!r.ok() || n > kMaxIdsPerMsg)
			return false;
		std::set<uint16_t> seen;
		for (uint64_t i = 0; i < n; ++i)
		{
			const uint16_t id = r.u16();
			if (!r.ok() || !leasable(id) || !seen.insert(id).second)
			{
				ids.clear();
				return false;
			}
			ids.push_back(id);
		}
		return true;
	}
};

// ---------------------------------------------------------------- host

class HostLeases
{
public:
	struct Stats
	{
		uint64_t granted = 0, spent = 0, given_back = 0, dropped = 0, voided = 0;
	};

	// The host world the leases belong to. Another one: every lease is void, nothing is freed. True if it changed.
	bool set_epoch(uint32_t epoch)
	{
		if (epoch == m_epoch)
			return false;
		m_epoch = epoch;
		m_stats.voided += m_owner.size();
		m_peers.clear();
		m_owner.clear();
		return true;
	}
	uint32_t epoch() const { return m_epoch; }

	// How many ids `peer` may still get: up to a whole lease, none after kMaxGivenBack give-backs in this world.
	size_t want(PeerId peer) const
	{
		const auto it = m_peers.find(peer);
		if (it == m_peers.end())
			return kLeaseSize;
		if (it->second.given_back >= kMaxGivenBack)
			return 0;
		return it->second.ids.size() < kLeaseSize ? kLeaseSize - it->second.ids.size() : 0;
	}

	// Takes up to `n` ids (never more than want(peer)) from pick() (the host's generator; kNoId: none left, the grant
	// stops) into the peer's lease. A picked id that cannot be leased (0) or that a lease holds already goes to `bad`:
	// the caller frees it. Returns the ids granted, in order.
	template <class Pick>
	std::vector<uint16_t> grant(PeerId peer, size_t n, Pick&& pick, std::vector<uint16_t>& bad)
	{
		std::vector<uint16_t> out;
		n = std::min(n, want(peer));
		while (out.size() < n)
		{
			const uint16_t id = pick();
			if (id == kNoId)
				break;
			if (!leasable(id) || m_owner.count(id))
			{
				bad.push_back(id);
				continue;
			}
			m_owner[id] = peer;
			m_peers[peer].ids.insert(id);
			out.push_back(id);
		}
		m_stats.granted += out.size();
		return out;
	}

	bool owns(PeerId peer, uint16_t id) const
	{
		const auto it = m_owner.find(id);
		return it != m_owner.end() && it->second == peer;
	}
	// Some player holds it.
	bool leased(uint16_t id) const { return m_owner.count(id) != 0; }

	// The host created an entity with it: it is the world's now, not leased. False if `peer` does not hold it.
	bool spend(PeerId peer, uint16_t id)
	{
		if (!take(peer, id))
			return false;
		++m_stats.spent;
		return true;
	}

	// The client cannot use it (taken there by an object of its own, or its create was refused): out of the lease, the
	// caller frees it in its generator. False if `peer` does not hold it (a spent id is never freed this way).
	bool give_back(PeerId peer, uint16_t id)
	{
		if (!take(peer, id))
			return false;
		++m_peers[peer].given_back;
		++m_stats.given_back;
		return true;
	}

	// The player left (or goes through the join again): what he holds, for the caller to free.
	std::vector<uint16_t> drop(PeerId peer)
	{
		std::vector<uint16_t> out;
		const auto it = m_peers.find(peer);
		if (it == m_peers.end())
			return out;
		out.assign(it->second.ids.begin(), it->second.ids.end());
		for (uint16_t id : out)
			m_owner.erase(id);
		m_stats.dropped += out.size();
		m_peers.erase(it);
		return out;
	}

	// The session ends in this world: every lease, for the caller to free.
	std::vector<uint16_t> drop_all()
	{
		std::vector<uint16_t> out;
		for (const auto& it : m_owner)
			out.push_back(it.first);
		m_stats.dropped += out.size();
		m_peers.clear();
		m_owner.clear();
		return out;
	}

	size_t count(PeerId peer) const
	{
		const auto it = m_peers.find(peer);
		return it == m_peers.end() ? 0 : it->second.ids.size();
	}
	size_t total() const { return m_owner.size(); }
	std::vector<PeerId> holders() const
	{
		std::vector<PeerId> out;
		for (const auto& it : m_peers)
			if (!it.second.ids.empty())
				out.push_back(it.first);
		return out;
	}
	const Stats& stats() const { return m_stats; }
	// A session starts or ends: the world goes with it, nothing is freed.
	void clear()
	{
		m_peers.clear();
		m_owner.clear();
	}

private:
	bool take(PeerId peer, uint16_t id)
	{
		if (!owns(peer, id))
			return false;
		m_owner.erase(id);
		m_peers[peer].ids.erase(id);
		return true;
	}

	struct Peer
	{
		std::set<uint16_t> ids;
		size_t given_back = 0;
	};
	std::map<PeerId, Peer> m_peers;
	std::map<uint16_t, PeerId> m_owner; // an id is in one lease at most
	uint32_t m_epoch = 0;
	Stats m_stats;
};

// ---------------------------------------------------------------- client

class ClientPool
{
public:
	struct Stats
	{
		uint64_t received = 0, rejected = 0, taken = 0, put_back = 0;
	};

	// The ids of an IdLease of host world `epoch`. Another world than the pool's: the pool starts empty (the old ids went
	// with the old generator). Each id: one the pool has is skipped (neither reserved twice nor given back); else
	// take_here(id) reserves it on this machine (true) or it goes to `rejected` (an object here holds it: back to the
	// host). Returns the number added.
	template <class TakeHere>
	size_t add(uint32_t epoch, const std::vector<uint16_t>& ids, TakeHere&& take_here, std::vector<uint16_t>& rejected)
	{
		if (epoch != m_epoch)
		{
			m_ids.clear();
			m_epoch = epoch;
		}
		size_t added = 0;
		for (uint16_t id : ids)
		{
			if (!leasable(id) || has(id))
				continue;
			++m_stats.received;
			if (!take_here(id))
			{
				rejected.push_back(id);
				++m_stats.rejected;
				continue;
			}
			m_ids.push_back(id);
			++added;
		}
		return added;
	}

	// The next id, first in first out; kNoId when empty. A taken id is the caller's (a temporary object, a create).
	uint16_t take()
	{
		if (m_ids.empty())
			return kNoId;
		const uint16_t id = m_ids.front();
		m_ids.pop_front();
		++m_stats.taken;
		return id;
	}
	// A taken id the caller did not use (a temporary object released before its register): back into the pool.
	void put_back(uint16_t id)
	{
		if (!leasable(id) || has(id))
			return;
		m_ids.push_back(id);
		++m_stats.put_back;
	}
	bool has(uint16_t id) const { return std::find(m_ids.begin(), m_ids.end(), id) != m_ids.end(); }
	size_t size() const { return m_ids.size(); }
	uint32_t epoch() const { return m_epoch; }
	const Stats& stats() const { return m_stats; }
	void clear() { m_ids.clear(); }

private:
	std::deque<uint16_t> m_ids;
	uint32_t m_epoch = 0;
	Stats m_stats;
};

// What became of a create of ours after the host's answer to its request.
enum class Outcome : uint8_t
{
	Unknown, // no create of ours was asked with that seq
	Confirmed, // the host created it with our id
	Retry, // RateLimited: the same id is asked again (SpecBook::due)
	RolledBack, // refused: our object goes, the id goes back to the host
};

// The creates of this client the host has not answered yet, by id.
// Round 2 (review N1): every request sent for a create is remembered (a resend gets a new seq): the Ok of any of them
// confirms it - the host made it under one, and refuses the others as not his any more; a refusal decides only for the
// latest one. A request the host put into its budget queue (ReqResult Queued) waits for its answer, however long: a
// second request with its id would queue twice. Any other request without an answer for kAnswerTimeoutMs is asked again
// (the session's rate row drops what is over it without an answer), kMaxResends times, then the create is taken back:
// nothing waits for ever.
class SpecBook
{
public:
	struct Spec
	{
		uint16_t id = kNoId;
		uint16_t source = kNoId; // a clone: the weapon it replaces (the host releases it with the clone's creation)
		uint32_t seq = 0; // of the latest request in flight; 0 = in our own outbox, or waiting for its retry
		std::vector<uint32_t> seqs; // every request sent for it (round 2)
		uint32_t tries = 0; // RateLimited answers so far
		uint32_t resends = 0; // requests asked again for want of an answer (round 2)
		uint64_t retry_at = 0; // when due again (waiting)
		uint64_t sent_at = 0; // when the latest request went out (round 2)
		bool waiting = false; // a retry is due at retry_at
		bool queued = false; // the host keeps it in its budget queue (round 2)
	};

	// What due() found: requests to send again (the caller queues them and calls sent(id, 0, now)), and creates to take
	// back (no answer after kMaxResends: the records are gone).
	struct Due
	{
		std::vector<uint16_t> resend;
		std::vector<uint16_t> rollback;
	};

	// A create under leased `id` (source: the weapon a clone replaces, kNoId otherwise). Twice: one record.
	void add(uint16_t id, uint16_t source = kNoId)
	{
		Spec& s = m_specs[id];
		s.id = id;
		s.source = source;
	}
	// Its request went out with `seq` at `now` (the first one or another); seq 0: it waits in our own outbox (paced).
	void sent(uint16_t id, uint32_t seq, uint64_t now)
	{
		const auto it = m_specs.find(id);
		if (it == m_specs.end())
			return;
		Spec& s = it->second;
		s.seq = seq;
		s.waiting = false;
		s.sent_at = now;
		if (seq)
			s.seqs.push_back(seq);
	}

	// Round 2: the host put request `seq` into its budget queue (not an answer yet). False: no request of ours.
	bool queued(uint32_t seq)
	{
		Spec* s = by_seq(seq);
		if (!s)
			return false;
		s->queued = true;
		return true;
	}

	// The host's answer to request `seq`. accepted (any request of the create): Confirmed, the record goes. A refusal of
	// an older request than the latest: Unknown (stale; the latest decides). rate_limited: Retry, due at now +
	// retry_delay(tries), or RolledBack after kMaxRetries. Any other refusal: RolledBack (the record goes). `id` gets the
	// create's id (also for a stale one).
	Outcome answer(uint32_t seq, bool accepted, bool rate_limited, uint64_t now, uint16_t& id)
	{
		Spec* s = by_seq(seq);
		if (!s)
			return Outcome::Unknown;
		id = s->id;
		if (accepted)
		{
			m_specs.erase(s->id);
			return Outcome::Confirmed;
		}
		if (seq != s->seq)
			return Outcome::Unknown;
		if (rate_limited && ++s->tries < kMaxRetries)
		{
			s->seq = 0;
			s->waiting = true;
			s->queued = false;
			s->retry_at = now + retry_delay(s->tries);
			return Outcome::Retry;
		}
		m_specs.erase(s->id);
		return Outcome::RolledBack;
	}

	// The retries whose time has come, and (round 2) the requests without an answer for kAnswerTimeoutMs that the host
	// did not queue: asked again, or after kMaxResends taken back (their records go).
	Due due(uint64_t now)
	{
		Due d;
		for (auto it = m_specs.begin(); it != m_specs.end();)
		{
			Spec& s = it->second;
			if (s.waiting)
			{
				if (s.retry_at <= now)
					d.resend.push_back(s.id);
			}
			else if (s.seq && !s.queued && now >= s.sent_at && now - s.sent_at >= kAnswerTimeoutMs)
			{
				if (s.resends >= kMaxResends)
				{
					d.rollback.push_back(s.id);
					it = m_specs.erase(it);
					continue;
				}
				++s.resends;
				s.seq = 0; // until the caller sends it again
				s.sent_at = now;
				d.resend.push_back(s.id);
			}
			++it;
		}
		return d;
	}

	bool pending(uint16_t id) const { return m_specs.count(id) != 0; }
	// Round 1 (review C1): the first id of `ids` whose create the host has not answered (an action about it waits for
	// the answer: Held), kNoId if none.
	uint16_t first_pending(std::initializer_list<uint16_t> ids) const
	{
		for (uint16_t id : ids)
			if (id != kNoId && pending(id))
				return id;
		return kNoId;
	}
	// Round 1 (review C1): a clone of `id` now would be refused or take a source another clone took: the host has not
	// created it yet, or a pending clone replaces it.
	bool waiting(uint16_t id) const { return pending(id) || is_source(id); }
	// A pending clone replaces item `id`: a release of it is the host's part of the clone.
	bool is_source(uint16_t id) const
	{
		if (id == kNoId)
			return false;
		for (const auto& it : m_specs)
			if (it.second.source == id)
				return true;
		return false;
	}
	const Spec* find(uint16_t id) const
	{
		const auto it = m_specs.find(id);
		return it == m_specs.end() ? nullptr : &it->second;
	}
	bool forget(uint16_t id) { return m_specs.erase(id) != 0; }
	size_t size() const { return m_specs.size(); }
	void clear() { m_specs.clear(); }

private:
	Spec* by_seq(uint32_t seq)
	{
		if (!seq)
			return nullptr;
		for (auto& it : m_specs)
			if (std::find(it.second.seqs.begin(), it.second.seqs.end(), seq) != it.second.seqs.end())
				return &it.second;
		return nullptr;
	}

	std::map<uint16_t, Spec> m_specs;
};

// Round 1 (review C1): the requests about an item of ours the host has not created yet (its create waits in the host's
// budget queue, or for a retry after RateLimited). Sent before the create, the host refuses them (Missing) and creates
// the item later: a duplicate. They wait here by the id they wait for, in order: released at the host's Ok, dropped at
// a rollback (the item goes itself).
template <class T>
class Held
{
public:
	void hold(uint16_t id, T item) { m_held[id].push_back(std::move(item)); }
	bool holds(uint16_t id) const { return m_held.count(id) != 0; }
	// The ones waiting for `id`, in the order they were made; forgotten here.
	std::vector<T> release(uint16_t id)
	{
		std::vector<T> out;
		const auto it = m_held.find(id);
		if (it == m_held.end())
			return out;
		out = std::move(it->second);
		m_held.erase(it);
		return out;
	}
	// The create of `id` was taken back: drop(item) for each one waiting for it. Returns how many.
	template <class Drop>
	size_t drop(uint16_t id, Drop&& drop)
	{
		std::vector<T> gone = release(id);
		for (T& item : gone)
			drop(item);
		return gone.size();
	}
	template <class Drop>
	void clear(Drop&& drop)
	{
		for (auto& it : m_held)
			for (T& item : it.second)
				drop(item);
		m_held.clear();
	}
	size_t size() const
	{
		size_t n = 0;
		for (const auto& it : m_held)
			n += it.second.size();
		return n;
	}

private:
	std::map<uint16_t, std::vector<T>> m_held;
};

// Round 1 (review I2): rolled-back entities kept a while after their release (a script may still hold one) and freed
// later - but only in the world they were released in: a new world closed the Lua state their script objects live in,
// and freeing one then runs a destructor in it (an access violation). Those are forgotten instead (a small leak).
template <class T>
class Quarantine
{
public:
	void keep(T item, uint64_t now, uint32_t world) { m_items.push_back(Entry{std::move(item), now, world}); }
	// The ones kept `ms` ago or more (every one with `all`): free(item) when kept in `world`, else leak(item). Returns
	// how many left the quarantine.
	template <class Free, class Leak>
	size_t expire(uint64_t now, uint64_t ms, uint32_t world, bool all, Free&& free, Leak&& leak)
	{
		size_t n = 0;
		for (auto it = m_items.begin(); it != m_items.end();)
		{
			if (!all && (now < it->at || now - it->at < ms)) // a clock behind the entry: not due
			{
				++it;
				continue;
			}
			if (it->world == world)
				free(it->item);
			else
				leak(it->item);
			it = m_items.erase(it);
			++n;
		}
		return n;
	}
	size_t size() const { return m_items.size(); }

private:
	struct Entry
	{
		T item;
		uint64_t at;
		uint32_t world;
	};
	std::vector<Entry> m_items;
};
} // namespace lease
} // namespace coop
