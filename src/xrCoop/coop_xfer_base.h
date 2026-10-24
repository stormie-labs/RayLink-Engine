// Anomaly Coop - F3a split levels: ownership transfer, the shared part - keys, ops, packets, timings, digests (plan
// 2026-10-24-f3a §1.7). The protocol and its rulings are described in coop_xfer.h; the map server's side is
// coop_xfer_holder.h, the journal's text coop_xfer_journal.h.
// Pure C++17, header-only, no engine dependencies: used by the engine and the unit-test exe.
#pragma once

#include "coop_level_hash.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace coop
{
namespace xfer
{
using Peer = uint32_t;
constexpr Peer kWorld = 0;

struct Key
{
	uint16_t id;
	uint32_t gen;
};

enum class Op : uint8_t
{
	TakeReq, // map -> world
	Grant, // world -> map: payload = state
	TakeCommit, // map -> world
	Done, // world -> map: TakeCommit, ReleaseCommit, Destroy answered
	ReleasePrepare, // map -> world: key.gen = the gen held, payload = state
	Ack, // world -> map: ReleasePrepare (key.gen = the release's gen), SpawnPrepare (key.gen = the spawn's gen)
	ReleaseCommit, // map -> world
	SpawnPrepare, // map -> world: key.gen = 0, payload = state
	Destroy, // map -> world: key.gen = the gen held, 0 for a spawn not answered yet
	Abort, // world -> map
	AbortAck, // map -> world
	Drop, // world -> map
	Snapshot // map -> world: snapshot_payload(seq, state); world -> map: the ack, snapshot_payload(seq, {})
};

struct Packet
{
	Op op;
	Key key;
	Peer from, to;
	std::vector<uint8_t> payload;
};

constexpr uint32_t kRetryFirstMs = 250;
constexpr uint32_t kRetryMaxMs = 2000;
constexpr uint32_t kGrantTimeoutMs = 10000;
constexpr uint32_t kTombstoneMs = 60000;
constexpr size_t kJournalMaxEntries = 64;
constexpr size_t kJournalMaxBytes = 256 * 1024;
constexpr uint32_t kEpochShift = 24;

inline const char* op_name(Op op)
{
	switch (op)
	{
	case Op::TakeReq: return "TakeReq";
	case Op::Grant: return "Grant";
	case Op::TakeCommit: return "TakeCommit";
	case Op::Done: return "Done";
	case Op::ReleasePrepare: return "ReleasePrepare";
	case Op::Ack: return "Ack";
	case Op::ReleaseCommit: return "ReleaseCommit";
	case Op::SpawnPrepare: return "SpawnPrepare";
	case Op::Destroy: return "Destroy";
	case Op::Abort: return "Abort";
	case Op::AbortAck: return "AbortAck";
	case Op::Drop: return "Drop";
	case Op::Snapshot: return "Snapshot";
	}
	return "?";
}

// FNV-1a 64 of a state; never 0 (0 means "not compared" in an Entry).
inline uint64_t digest(const std::vector<uint8_t>& state)
{
	uint64_t h = 0xCBF29CE484222325ull;
	for (uint8_t b : state)
	{
		h ^= b;
		h *= 0x100000001B3ull;
	}
	return h ? h : 1;
}

// Snapshot payload: u32 seq (little-endian), then the state (empty in the world's ack).
inline std::vector<uint8_t> snapshot_payload(uint32_t seq, const std::vector<uint8_t>& state)
{
	std::vector<uint8_t> p(4 + state.size());
	for (int i = 0; i < 4; ++i)
		p[size_t(i)] = uint8_t(seq >> (8 * i));
	std::copy(state.begin(), state.end(), p.begin() + 4);
	return p;
}

inline bool read_snapshot(const std::vector<uint8_t>& payload, uint32_t& seq, std::vector<uint8_t>& state)
{
	if (payload.size() < 4)
		return false;
	seq = 0;
	for (int i = 0; i < 4; ++i)
		seq |= uint32_t(payload[size_t(i)]) << (8 * i);
	state.assign(payload.begin() + 4, payload.end());
	return true;
}

namespace detail
{
inline Packet packet(Op op, uint16_t id, uint32_t gen, Peer from, Peer to, std::vector<uint8_t> payload = {})
{
	return Packet{op, Key{id, gen}, from, to, std::move(payload)};
}

// The wait before repeat number `tries` + 1: kRetryFirstMs, doubling, at most kRetryMaxMs.
inline uint64_t backoff(uint32_t tries)
{
	const uint64_t d = uint64_t(kRetryFirstMs) << std::min<uint32_t>(tries, 8);
	return d < kRetryMaxMs ? d : kRetryMaxMs;
}
} // namespace detail
} // namespace xfer
} // namespace coop
