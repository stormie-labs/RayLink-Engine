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
#include <map>
#include <set>
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
class SpecBook
{
public:
	struct Spec
	{
		uint16_t id = kNoId;
		uint16_t source = kNoId; // a clone: the weapon it replaces (the host releases it with the clone's creation)
		uint32_t seq = 0; // of the request in flight; 0 = not sent yet, or waiting for its retry
		uint32_t tries = 0; // RateLimited answers so far
		uint64_t retry_at = 0; // when due again (seq 0)
		bool waiting = false; // a retry is due at retry_at
	};

	// A create under leased `id` (source: the weapon a clone replaces, kNoId otherwise). Twice: one record.
	void add(uint16_t id, uint16_t source = kNoId)
	{
		Spec& s = m_specs[id];
		s.id = id;
		s.source = source;
	}
	// Its request went out with `seq` (the first one or a retry): the answer comes for that seq.
	void sent(uint16_t id, uint32_t seq)
	{
		const auto it = m_specs.find(id);
		if (it == m_specs.end())
			return;
		it->second.seq = seq;
		it->second.waiting = false;
	}

	// The host's answer to request `seq`. accepted: Confirmed (the record goes). rate_limited: Retry, due at now +
	// retry_delay(tries), or RolledBack after kMaxRetries. Any other refusal: RolledBack (the record goes). `id` gets the
	// create's id (unchanged for Unknown).
	Outcome answer(uint32_t seq, bool accepted, bool rate_limited, uint64_t now, uint16_t& id)
	{
		if (!seq)
			return Outcome::Unknown;
		for (auto it = m_specs.begin(); it != m_specs.end(); ++it)
		{
			Spec& s = it->second;
			if (s.seq != seq)
				continue;
			id = s.id;
			if (accepted)
			{
				m_specs.erase(it);
				return Outcome::Confirmed;
			}
			if (rate_limited && ++s.tries < kMaxRetries)
			{
				s.seq = 0;
				s.waiting = true;
				s.retry_at = now + retry_delay(s.tries);
				return Outcome::Retry;
			}
			m_specs.erase(it);
			return Outcome::RolledBack;
		}
		return Outcome::Unknown;
	}

	// The creates whose retry is due.
	std::vector<uint16_t> due(uint64_t now) const
	{
		std::vector<uint16_t> out;
		for (const auto& it : m_specs)
			if (it.second.waiting && it.second.retry_at <= now)
				out.push_back(it.first);
		return out;
	}

	bool pending(uint16_t id) const { return m_specs.count(id) != 0; }
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
	std::map<uint16_t, Spec> m_specs;
};
} // namespace lease
} // namespace coop
