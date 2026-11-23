// Anomaly Coop - F3a split levels: the level hash of a map server (LevelHash 0x01D9) and its comparison (plan
// 2026-10-24-f3a §1.7 "auditor", §4.1 requirement 4).
// Every 10 s (and on coop_levels_audit) a map server sends the hash of what it owns: the number of ids, a hash of the
// set of ids and 32 buckets over (id, gen, parent, alive, digest). The world server builds the same from its ledger
// (xfer::Ledger::owned_by plus its own copies) and asks for a full resync of each bucket that differs (LevelResync,
// u8 mask[4]), after the StoryHash / StoryResync model (coop_story.cpp). The full states of those buckets then go
// through xfer::audit (coop_xfer.h). Order-independent: each bucket is a sum of mixed entries, so neither side sorts.
// Wire: u32 n, u64 set, 32 x u32.
// Pure C++17, header-only, no engine dependencies: used by the engine and the unit-test exe.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace coop
{
namespace xfer
{
constexpr uint32_t kBuckets = 32;
constexpr uint32_t kAllBuckets = 0xffffffffu;
constexpr uint16_t kNoParent = 0xffff;

// One object as its owner reports it (and as the world server expects it).
struct Entry
{
	uint16_t id = 0;
	uint32_t gen = 0; // the ledger's gen of the transfer that gave it to its owner (0: a spawn not answered yet)
	uint16_t parent = kNoParent;
	bool alive = true;
	uint64_t digest = 0; // xfer::digest of the object's state; 0 on both sides = state not compared
};

struct LevelHash
{
	uint32_t n = 0; // ids
	uint64_t set = 0; // the set of ids
	uint32_t bucket[kBuckets] = {}; // the entries of each bucket
};

// The bucket of an id (LevelResync mask bit). Sequential ids spread evenly.
inline uint32_t bucket_of(uint16_t id) { return uint32_t(id) % kBuckets; }

// splitmix64's finalizer: every input bit moves about half of the output bits.
inline uint64_t mix64(uint64_t x)
{
	x ^= x >> 30;
	x *= 0xBF58476D1CE4E5B9ull;
	x ^= x >> 27;
	x *= 0x94D049BB133111EBull;
	return x ^ (x >> 31);
}

inline uint64_t entry_hash(const Entry& e)
{
	const uint64_t a = uint64_t(e.id) | (uint64_t(e.gen) << 16) | (uint64_t(e.parent) << 48);
	const uint64_t b = mix64(a ^ (e.alive ? 0x5A5A5A5A00000000ull : 0x00000000A5A5A5A5ull));
	return mix64(b + e.digest * 0x9E3779B97F4A7C15ull + 0x632BE59BD9B4E019ull);
}

// The hash of what one side owns (any order; ids are unique on each side).
inline LevelHash hash_of(const std::vector<Entry>& owned)
{
	LevelHash h;
	uint64_t sums[kBuckets] = {};
	for (const Entry& e : owned)
	{
		++h.n;
		h.set += mix64(uint64_t(e.id) + 0x2545F4914F6CDD1Dull);
		sums[bucket_of(e.id)] += entry_hash(e);
	}
	for (uint32_t i = 0; i < kBuckets; ++i)
		h.bucket[i] = uint32_t(sums[i] ^ (sums[i] >> 32));
	return h;
}

// The buckets that differ (bit i = bucket i). When the counts or the id sets differ but no bucket shows it (a collision
// of 32-bit sums), every bucket is asked for: a difference is never reported as none.
inline uint32_t mismatched(const LevelHash& a, const LevelHash& b)
{
	uint32_t mask = 0;
	for (uint32_t i = 0; i < kBuckets; ++i)
		if (a.bucket[i] != b.bucket[i])
			mask |= 1u << i;
	if (!mask && (a.n != b.n || a.set != b.set))
		mask = kAllBuckets;
	return mask;
}
} // namespace xfer
} // namespace coop
