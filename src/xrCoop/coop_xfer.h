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
//   R1 gens: one counter for all ids (the world epoch in the top 8 bits, the next epoch after every load). Every
//      transfer takes a fresh gen and all its replies carry it; a release is keyed by the Ack's new gen, a destroy
//      answered by a new gen - so a late reply of an earlier transfer never passes for a later one's.
//   R2 a map server declines (AbortAck) only a grant it does not write; it never declines while it holds an older
//      incarnation (Held: the newer grant replaces it; Spawning/Releasing: no answer, the world's abort will come).
//   R3 SpawnPrepare registers at its arrival (atomic: the record itself goes into the save, "SpawnPrepare:
//      completed"); the journal keeps no spawn entry.
//   R4 a destroy tombstones the id for kTombstoneMs: a late SpawnPrepare or TakeReq gets nothing back; Destroy {0}
//      of a spawn the world has not seen tombstones it as well.
//   R5 ReleasePrepare or Destroy with the gen of a grant still Granting to that map server is its TakeCommit too (it
//      can only have that gen from the Grant). While Aborting the abort wins: the object stays the world's at its
//      last confirmed state.
//   R6 the journal (Granting, Aborting, Releasing) stays within kJournalMaxEntries and kJournalMaxBytes of text: a
//      transfer that does not fit is not started (no Grant, no Ack) - the map server repeats until it fits.
//   R7 server_lost means gone: its process is dead or its connection closed for good and its Holder is never used
//      again. Its objects go back to the world at once; the Drops it returns are best effort.
//   R8 after a load (resolve_after_load) every map server is gone: Releasing - completed with the journal's state;
//      Granting/Aborting - rolled back; an object owned by a map server - back to the world at its last confirmed
//      state. A Releasing entry whose object the save lacks is recreated from its state; a Granting entry whose
//      object the save lacks is a violation (nothing to roll back to).
//   R9 audit() takes a report that is current: everything the map server sent before it was handled before it (one
//      ordered channel). Transfers in flight are never judged. A Lost heal bumps the gen and sends Drop too, so even
//      a stale report cannot leave two writers.
// Pure C++17, header-only, no engine dependencies: used by the engine and the unit-test exe.
#pragma once

#include "coop_level_hash.h"

#include <algorithm>
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

constexpr char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

inline size_t base64_size(size_t n) { return (n + 2) / 3 * 4; }

inline std::string base64(const std::vector<uint8_t>& in)
{
	std::string out;
	out.reserve(base64_size(in.size()));
	size_t i = 0;
	for (; i + 2 < in.size(); i += 3)
	{
		const uint32_t v = (uint32_t(in[i]) << 16) | (uint32_t(in[i + 1]) << 8) | in[i + 2];
		out += kB64[(v >> 18) & 63];
		out += kB64[(v >> 12) & 63];
		out += kB64[(v >> 6) & 63];
		out += kB64[v & 63];
	}
	if (i + 1 == in.size())
	{
		const uint32_t v = uint32_t(in[i]) << 16;
		out += kB64[(v >> 18) & 63];
		out += kB64[(v >> 12) & 63];
		out += "==";
	}
	else if (i + 2 == in.size())
	{
		const uint32_t v = (uint32_t(in[i]) << 16) | (uint32_t(in[i + 1]) << 8);
		out += kB64[(v >> 18) & 63];
		out += kB64[(v >> 12) & 63];
		out += kB64[(v >> 6) & 63];
		out += '=';
	}
	return out;
}

inline int base64_value(char c)
{
	if (c >= 'A' && c <= 'Z')
		return c - 'A';
	if (c >= 'a' && c <= 'z')
		return c - 'a' + 26;
	if (c >= '0' && c <= '9')
		return c - '0' + 52;
	if (c == '+')
		return 62;
	if (c == '/')
		return 63;
	return -1;
}

// Strict: whole quads, the alphabet only, '=' only as the padding of the last quad.
inline bool unbase64(const std::string& in, std::vector<uint8_t>& out)
{
	out.clear();
	if (in.size() % 4)
		return false;
	out.reserve(in.size() / 4 * 3);
	for (size_t i = 0; i < in.size(); i += 4)
	{
		const bool last = i + 4 == in.size();
		int v[4];
		int pad = 0;
		for (int k = 0; k < 4; ++k)
		{
			const char c = in[i + size_t(k)];
			if (c == '=' && last && k >= 2)
			{
				v[k] = 0;
				++pad;
				continue;
			}
			if (pad)
				return false; // data after a '='
			v[k] = base64_value(c);
			if (v[k] < 0)
				return false;
		}
		const uint32_t q = (uint32_t(v[0]) << 18) | (uint32_t(v[1]) << 12) | (uint32_t(v[2]) << 6) | uint32_t(v[3]);
		out.push_back(uint8_t(q >> 16));
		if (pad < 2)
			out.push_back(uint8_t(q >> 8));
		if (pad < 1)
			out.push_back(uint8_t(q));
	}
	return true;
}

// Strict unsigned decimal within [0, max].
inline bool parse_u32(const std::string& s, uint32_t max, uint32_t& out)
{
	if (s.empty() || s.size() > 10)
		return false;
	uint64_t v = 0;
	for (char c : s)
	{
		if (c < '0' || c > '9')
			return false;
		v = v * 10 + uint64_t(c - '0');
	}
	if (v > max)
		return false;
	out = uint32_t(v);
	return true;
}

inline std::string trim(const std::string& s)
{
	size_t b = 0, e = s.size();
	while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r'))
		++b;
	while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r'))
		--e;
	return s.substr(b, e - b);
}

// Upper bounds of the journal text: the header and one entry line besides its base64.
constexpr size_t kHeaderBytes = 64;
constexpr size_t kEntryBytes = 64;
} // namespace detail

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
		uint64_t in_flight; // journal entries now
		uint64_t retries; // grants and aborts repeated
		uint64_t completed; // journal entries completed by a resolution (a release applied after a loss or a load)
		uint64_t rolled_back; // grants undone, objects of a gone map server back at their last confirmed state
		uint64_t resyncs; // drifts healed by a snapshot of the owner
		uint64_t violations; // duplicates and losses healed, spawns refused under a taken id, broken journal entries
	};

	// An object of the world (start, load, the world's own spawn). Creates it owned by the world; an existing one keeps
	// its owner and takes the state only when the world writes it. Clears a tombstone of the id.
	void add_world_object(uint16_t id, std::vector<uint8_t> state)
	{
		m_dead.erase(id);
		auto it = m_recs.find(id);
		if (it == m_recs.end())
		{
			Rec r;
			r.state = std::move(state);
			m_recs.emplace(id, std::move(r));
			m_owned[kWorld].insert(id);
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
		if (p.from == kWorld || p.to != kWorld)
			return out;
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
		for (uint16_t id : m_busy)
		{
			Rec& r = m_recs.at(id);
			if (r.phase == Phase::Granting && now >= r.since + kGrantTimeoutMs)
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
		if (now >= m_next_sweep)
		{
			m_next_sweep = now + 1000;
			for (auto it = m_dead.begin(); it != m_dead.end();)
			{
				if (now >= it->second.at + kTombstoneMs)
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
		std::vector<uint16_t> ids;
		for (const auto& kv : m_recs)
			if (kv.second.owner == server || (kv.second.phase != Phase::Stable && kv.second.peer == server))
				ids.push_back(kv.first);
		for (uint16_t id : ids)
		{
			Rec& r = m_recs.at(id);
			const uint32_t held = r.gen;
			if (r.phase == Phase::Releasing)
			{
				r.state = std::move(r.pending);
				settle(id, r);
				set_owner(id, r, kWorld);
				r.gen = next_gen();
				++m_counters.completed;
			}
			else if (r.phase == Phase::Granting || r.phase == Phase::Aborting)
			{
				settle(id, r);
				++m_counters.rolled_back;
			}
			else
			{
				set_owner(id, r, kWorld);
				r.gen = next_gen();
				++m_counters.rolled_back;
			}
			r.snap_seq = 0;
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
		std::vector<Job> list;
		for (uint16_t id : m_busy)
		{
			const Rec& r = m_recs.at(id);
			list.push_back(Job{kind_of(r.phase), id, r.peer, r.gen, r.from_gen, r.pending});
		}
		for (const Job& j : m_loaded)
			list.push_back(j);
		std::sort(list.begin(), list.end(), [](const Job& a, const Job& b) { return a.id < b.id; });
		std::string s = "[coop_xfer]\nv = 1\ngen = " + std::to_string(m_gen) + "\nn = " + std::to_string(list.size());
		s += "\n";
		for (size_t i = 0; i < list.size(); ++i)
		{
			const Job& j = list[i];
			s += "x" + std::to_string(i) + " = " + j.kind + "," + std::to_string(j.id) + "," + std::to_string(j.peer);
			s += "," + std::to_string(j.gen) + "," + std::to_string(j.from_gen) + ",";
			s += detail::base64(j.payload) + "\n";
		}
		return s;
	}

	// Reads a journal written by save_journal (other sections may surround [coop_xfer]); the entries wait for
	// resolve_after_load. False, and nothing taken, for anything malformed.
	bool load_journal(const std::string& text)
	{
		if (text.size() > kJournalMaxBytes)
			return false;
		std::vector<std::string> lines;
		bool in = false, found = false;
		size_t pos = 0;
		while (pos < text.size())
		{
			size_t nl = text.find('\n', pos);
			if (nl == std::string::npos)
				nl = text.size();
			const std::string line = detail::trim(text.substr(pos, nl - pos));
			pos = nl + 1;
			if (line.empty())
				continue;
			if (line[0] == '[')
			{
				if (found)
					break; // the next section
				in = found = line == "[coop_xfer]";
				continue;
			}
			if (in)
				lines.push_back(line);
		}
		if (!found)
			return false;
		std::map<std::string, std::string> kv;
		for (const std::string& line : lines)
		{
			const size_t eq = line.find('=');
			if (eq == std::string::npos)
				return false;
			const std::string key = detail::trim(line.substr(0, eq));
			if (key.empty() || !kv.emplace(key, detail::trim(line.substr(eq + 1))).second)
				return false;
		}
		uint32_t version = 0, gen = 0, n = 0;
		if (!kv.count("v") || !detail::parse_u32(kv["v"], 1, version) || version != 1)
			return false;
		if (!kv.count("gen") || !detail::parse_u32(kv["gen"], 0xffffffffu, gen))
			return false;
		if (!kv.count("n") || !detail::parse_u32(kv["n"], uint32_t(kJournalMaxEntries), n))
			return false;
		if (kv.size() != 3 + size_t(n))
			return false;
		std::vector<Job> loaded;
		std::set<uint16_t> seen;
		for (uint32_t i = 0; i < n; ++i)
		{
			auto it = kv.find("x" + std::to_string(i));
			if (it == kv.end())
				return false;
			std::vector<std::string> f;
			for (size_t b = 0;;)
			{
				const size_t c = it->second.find(',', b);
				f.push_back(it->second.substr(b, c == std::string::npos ? std::string::npos : c - b));
				if (c == std::string::npos)
					break;
				b = c + 1;
			}
			if (f.size() != 6 || f[0].size() != 1 || (f[0][0] != 'G' && f[0][0] != 'A' && f[0][0] != 'R'))
				return false;
			Job j;
			j.kind = f[0][0];
			uint32_t id = 0;
			if (!detail::parse_u32(f[1], 0xffff, id) || !detail::parse_u32(f[2], 0xffffffffu, j.peer) ||
			    j.peer == kWorld || !detail::parse_u32(f[3], 0xffffffffu, j.gen) || j.gen == 0 || j.gen > gen ||
			    !detail::parse_u32(f[4], 0xffffffffu, j.from_gen) || !detail::unbase64(f[5], j.payload))
				return false;
			if (j.kind != 'R' && (!j.payload.empty() || j.from_gen != 0))
				return false;
			j.id = uint16_t(id);
			if (!seen.insert(j.id).second)
				return false;
			loaded.push_back(std::move(j));
		}
		m_loaded = std::move(loaded);
		m_gen = std::max(m_gen, gen);
		return true;
	}

	// After a load, before the first tick (R8): every map server is gone; the journal is resolved and emptied; a new
	// epoch of gens begins.
	void resolve_after_load()
	{
		for (Job& j : m_loaded)
		{
			Rec* r = find(j.id);
			if (j.kind == 'R')
			{
				if (!r)
				{
					Rec n;
					n.state = std::move(j.payload);
					m_recs.emplace(j.id, std::move(n));
					m_owned[kWorld].insert(j.id);
				}
				else if (r->owner == kWorld && r->phase == Phase::Stable)
					r->state = std::move(j.payload);
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
		m_dead.clear();
		const uint64_t epoch = (uint64_t(m_gen) >> kEpochShift) + 1;
		m_gen = epoch < 256 ? uint32_t(epoch << kEpochShift) : uint32_t(1) << kEpochShift;
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
	struct Job // a journal entry
	{
		char kind = 'G'; // G Granting, A Aborting, R Releasing
		uint16_t id = 0;
		Peer peer = kWorld;
		uint32_t gen = 0;
		uint32_t from_gen = 0;
		std::vector<uint8_t> payload;
	};

	std::map<uint16_t, Rec> m_recs;
	std::map<Peer, std::set<uint16_t>> m_owned; // owner -> ids
	std::set<uint16_t> m_busy; // ids in a transfer (the journal)
	std::map<uint16_t, Tomb> m_dead;
	std::vector<Job> m_loaded; // read by load_journal, waiting for resolve_after_load
	uint32_t m_gen = 0;
	size_t m_journal_bytes = detail::kHeaderBytes;
	uint64_t m_next_sweep = 0;
	Counters m_counters{};

	static char kind_of(Phase p) { return p == Phase::Releasing ? 'R' : p == Phase::Aborting ? 'A' : 'G'; }
	static size_t cost_of(const std::vector<uint8_t>& payload)
	{
		return detail::kEntryBytes + detail::base64_size(payload.size());
	}

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

	uint32_t next_gen()
	{
		++m_gen;
		if (!m_gen)
			m_gen = uint32_t(1) << kEpochShift;
		return m_gen;
	}

	static void arm(Rec& r, uint64_t now)
	{
		r.tries = 0;
		r.next_retry = now + detail::backoff(0);
	}

	// The journal takes `entries` more and `bytes` more text (R6).
	bool fits(size_t entries, size_t bytes) const
	{
		return m_busy.size() + m_loaded.size() + entries <= kJournalMaxEntries &&
		       m_journal_bytes + bytes <= kJournalMaxBytes;
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
			// No such object (destroyed, or never made): refused, so the map server stops asking.
			auto d = m_dead.find(id);
			out.push_back(detail::packet(Op::Abort, id, d != m_dead.end() ? d->second.gen : 0, kWorld, p.from));
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
		const size_t bytes = cost_of(p.payload);
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
			// The id is someone else's (a lease fault): refused, the map server lets go of its object.
			++m_counters.violations;
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
			const bool granted =
				r->phase == Phase::Granting && r->peer == p.from && p.key.gen != 0 && r->gen == p.key.gen;
			if (owns || granted) // R5: a destroy of the incarnation granted is its commit too
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
			if (d == m_dead.end() && p.key.gen == 0)
			{
				// R4: destroyed before the world saw the spawn - tombstoned, a late SpawnPrepare registers nothing.
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

// A map server's side: what it owns, asks for, gives back; it repeats until answered. The engine creates the object on
// a Create event, deletes it on Remove, freezes it while frozen(); Refused ends a request (no such object, or aborted).
class Holder
{
public:
	struct Event
	{
		enum class Kind : uint8_t
		{
			Create, // a grant accepted: make the object from `state`
			Remove, // delete the object (released, aborted, dropped)
			Refused // a request ended without an object
		};
		Kind kind;
		uint16_t id;
		std::vector<uint8_t> state;
	};

	explicit Holder(Peer self) : m_self(self) {}

	// A player came near: the object should be online here.
	std::vector<Packet> want_online(uint16_t id, uint64_t now)
	{
		auto it = m_objs.find(id);
		if (it != m_objs.end())
		{
			if (it->second.st == St::Releasing || it->second.st == St::Committing)
				it->second.again = true; // taken again once the release is through
			return {};
		}
		Obj o;
		o.st = St::Requesting;
		arm(o, now);
		m_objs.emplace(id, std::move(o));
		return {out(Op::TakeReq, id, 0)};
	}

	// The object should go back to the world with `state` (frozen from now on). For a request not answered yet: the
	// request is taken back (a grant that still comes is declined); for a spawn not answered yet: released right after.
	std::vector<Packet> want_offline(uint16_t id, std::vector<uint8_t> state, uint64_t now)
	{
		auto it = m_objs.find(id);
		if (it == m_objs.end())
			return {};
		Obj& o = it->second;
		switch (o.st)
		{
		case St::Requesting:
			m_objs.erase(it);
			return {};
		case St::Spawning:
			if (!o.release_after) // frozen from the first call on: its state stays
			{
				o.release_after = true;
				o.state = std::move(state);
			}
			return {};
		case St::Held:
			o.st = St::Releasing;
			o.state = std::move(state);
			arm(o, now);
			return {out(Op::ReleasePrepare, id, o.gen, o.state)};
		case St::Releasing:
		case St::Committing:
			o.again = false;
			return {};
		case St::Destroying:
			return {};
		}
		return {};
	}

	// The map server made an object under a leased id.
	std::vector<Packet> spawned(uint16_t id, std::vector<uint8_t> state, uint64_t now)
	{
		if (m_objs.count(id))
			return {};
		Obj o;
		o.st = St::Spawning;
		o.state = std::move(state);
		arm(o, now);
		auto& ref = m_objs.emplace(id, std::move(o)).first->second;
		return {out(Op::SpawnPrepare, id, 0, ref.state)};
	}

	// The engine destroyed the object (it is gone here already).
	std::vector<Packet> destroyed(uint16_t id, uint64_t now)
	{
		auto it = m_objs.find(id);
		if (it == m_objs.end())
			return {};
		Obj& o = it->second;
		if (o.st != St::Held && !(o.st == St::Spawning && !o.release_after))
			return {}; // frozen for a release, deleted already, or only asked for
		o.st = St::Destroying; // a spawn not answered yet: gen 0 (R4)
		o.state.clear();
		arm(o, now);
		return {out(Op::Destroy, id, o.gen)};
	}

	// The state changed within ownership: streamed to the world (repeated until acknowledged).
	std::vector<Packet> changed(uint16_t id, std::vector<uint8_t> state, uint64_t now)
	{
		auto it = m_objs.find(id);
		if (it == m_objs.end())
			return {};
		Obj& o = it->second;
		if (o.st != St::Held && !(o.st == St::Spawning && !o.release_after))
			return {}; // frozen or not here
		o.state = std::move(state);
		++o.seq;
		if (o.st != St::Held || !o.committed)
			return {}; // sent once the world has answered
		arm(o, now);
		return {out(Op::Snapshot, id, o.gen, snapshot_payload(o.seq, o.state))};
	}

	std::vector<Packet> on_packet(const Packet& p, uint64_t now)
	{
		if (p.to != m_self || p.from != kWorld)
			return {};
		switch (p.op)
		{
		case Op::Grant: return on_grant(p, now);
		case Op::Done: return on_done(p, now);
		case Op::Ack: return on_ack(p, now);
		case Op::Abort: return on_abort(p);
		case Op::Drop: on_drop(p); return {};
		case Op::Snapshot: return on_snapshot_ack(p, now);
		default: return {};
		}
	}

	std::vector<Packet> tick(uint64_t now)
	{
		std::vector<Packet> v;
		for (auto& kv : m_objs)
		{
			Obj& o = kv.second;
			const uint16_t id = kv.first;
			if (now < o.next_retry)
				continue;
			switch (o.st)
			{
			case St::Requesting: v.push_back(out(Op::TakeReq, id, 0)); break;
			case St::Spawning: v.push_back(out(Op::SpawnPrepare, id, 0, o.state)); break;
			case St::Held:
				if (!o.committed)
					v.push_back(out(Op::TakeCommit, id, o.gen));
				else if (o.acked < o.seq)
					v.push_back(out(Op::Snapshot, id, o.gen, snapshot_payload(o.seq, o.state)));
				else
				{
					o.next_retry = kNever;
					continue;
				}
				break;
			case St::Releasing: v.push_back(out(Op::ReleasePrepare, id, o.gen, o.state)); break;
			case St::Committing: v.push_back(out(Op::ReleaseCommit, id, o.gen)); break;
			case St::Destroying: v.push_back(out(Op::Destroy, id, o.gen)); break;
			}
			++o.tries;
			o.next_retry = now + detail::backoff(o.tries);
		}
		return v;
	}

	// Writes the object: it exists here and nobody else may run it (frozen ones included, until the world's Ack).
	bool authors(uint16_t id) const
	{
		const Obj* o = find(id);
		return o && (o->st == St::Held || o->st == St::Releasing || o->st == St::Spawning);
	}
	// Frozen for its release: AI and physics stop, requests on it are refused (Reason::Moving).
	bool frozen(uint16_t id) const
	{
		const Obj* o = find(id);
		return o && (o->st == St::Releasing || (o->st == St::Spawning && o->release_after));
	}
	// Something is still going on for the id (a request, a transfer, a destroy, an unacknowledged change).
	bool busy(uint16_t id) const
	{
		const Obj* o = find(id);
		return o && !settled(*o);
	}
	bool idle() const
	{
		for (const auto& kv : m_objs)
			if (!settled(kv.second))
				return false;
		return true;
	}
	bool requesting(uint16_t id) const
	{
		const Obj* o = find(id);
		return o && o->st == St::Requesting;
	}
	std::vector<uint16_t> ids() const
	{
		std::vector<uint16_t> v;
		v.reserve(m_objs.size());
		for (const auto& kv : m_objs)
			v.push_back(kv.first);
		return v;
	}
	const std::vector<uint8_t>* state(uint16_t id) const
	{
		const Obj* o = find(id);
		return o && authors(id) ? &o->state : nullptr;
	}
	// The gen it holds the object under (0: not held, or a spawn not answered yet).
	uint32_t gen(uint16_t id) const
	{
		const Obj* o = find(id);
		return o && authors(id) ? o->gen : 0;
	}
	// Everything it writes, for the auditor and the level hash (parent and alive are the engine's to fill).
	std::vector<Entry> report() const
	{
		std::vector<Entry> v;
		for (const auto& kv : m_objs)
			if (authors(kv.first))
				v.push_back(Entry{kv.first, kv.second.gen, kNoParent, true, digest(kv.second.state)});
		return v;
	}
	std::vector<Event> take_events()
	{
		std::vector<Event> e;
		e.swap(m_events);
		return e;
	}
	Peer self() const { return m_self; }

private:
	enum class St : uint8_t
	{
		Requesting, // TakeReq until Grant
		Spawning, // SpawnPrepare until Ack (writes it)
		Held, // writes it; TakeCommit until Done, then the snapshot stream
		Releasing, // frozen, ReleasePrepare until Ack (still writes it)
		Committing, // deleted, ReleaseCommit until Done
		Destroying // deleted, Destroy until Done
	};
	struct Obj
	{
		St st = St::Requesting;
		uint32_t gen = 0; // Held/Releasing/Destroying: the gen held; Committing: the release's gen; Spawning: 0
		bool committed = false; // Held: the world answered the take (Done) or the spawn (Ack)
		bool release_after = false; // Spawning: give it back as soon as the spawn is answered
		bool again = false; // Releasing/Committing: ask for it again once the release is through
		std::vector<uint8_t> state;
		uint32_t seq = 0, acked = 0; // the snapshot stream
		uint64_t next_retry = 0;
		uint32_t tries = 0;
	};
	static constexpr uint64_t kNever = ~uint64_t(0);

	Peer m_self;
	std::map<uint16_t, Obj> m_objs;
	std::map<uint16_t, uint32_t> m_tomb; // per id: the newest gen finished here, never made again
	std::vector<Event> m_events;

	const Obj* find(uint16_t id) const
	{
		auto it = m_objs.find(id);
		return it == m_objs.end() ? nullptr : &it->second;
	}
	static bool settled(const Obj& o) { return o.st == St::Held && o.committed && o.acked >= o.seq; }
	uint32_t tomb(uint16_t id) const
	{
		auto it = m_tomb.find(id);
		return it == m_tomb.end() ? 0 : it->second;
	}
	void bury(uint16_t id, uint32_t gen)
	{
		uint32_t& t = m_tomb[id];
		t = std::max(t, gen);
	}
	static void arm(Obj& o, uint64_t now)
	{
		o.tries = 0;
		o.next_retry = now + detail::backoff(0);
	}
	Packet out(Op op, uint16_t id, uint32_t gen, std::vector<uint8_t> payload = {}) const
	{
		return detail::packet(op, id, gen, m_self, kWorld, std::move(payload));
	}

	std::vector<Packet> on_grant(const Packet& p, uint64_t now)
	{
		const uint16_t id = p.key.id;
		const uint32_t x = p.key.gen;
		if (x == 0)
			return {};
		auto it = m_objs.find(id);
		if (it == m_objs.end())
		{
			if (x < tomb(id))
				return {}; // a stale copy
			bury(id, x); // not wanted (any more): declined, never made here (R2)
			return {out(Op::AbortAck, id, x)};
		}
		Obj& o = it->second;
		switch (o.st)
		{
		case St::Requesting:
			if (x <= tomb(id))
				return x == tomb(id) ? std::vector<Packet>{out(Op::AbortAck, id, x)} : std::vector<Packet>{};
			o.st = St::Held;
			o.gen = x;
			o.committed = false;
			o.state = p.payload;
			o.seq = o.acked = 0;
			arm(o, now);
			m_events.push_back(Event{Event::Kind::Create, id, o.state});
			return {out(Op::TakeCommit, id, x)};
		case St::Held:
			if (x == o.gen)
				return {out(Op::TakeCommit, id, x)}; // the grant repeated: the commit repeated
			if (x > o.gen)
			{
				// The world grants a newer incarnation: it holds that this one is not here. Follow the world (R2).
				bury(id, o.gen);
				m_events.push_back(Event{Event::Kind::Remove, id, {}});
				o.gen = x;
				o.committed = false;
				o.state = p.payload;
				o.seq = o.acked = 0;
				arm(o, now);
				m_events.push_back(Event{Event::Kind::Create, id, o.state});
				return {out(Op::TakeCommit, id, x)};
			}
			return {};
		case St::Spawning:
		case St::Releasing:
			return {}; // writes an incarnation of it: never declined (R2); the world's abort will come
		case St::Committing:
		case St::Destroying:
			if (x < tomb(id) || x <= o.gen)
				return {};
			bury(id, x); // deleted here: declined
			return {out(Op::AbortAck, id, x)};
		}
		return {};
	}

	std::vector<Packet> on_done(const Packet& p, uint64_t now)
	{
		const uint16_t id = p.key.id;
		const uint32_t x = p.key.gen;
		auto it = m_objs.find(id);
		if (it == m_objs.end())
			return {};
		Obj& o = it->second;
		switch (o.st)
		{
		case St::Held:
			if (x == o.gen && !o.committed)
			{
				o.committed = true;
				if (o.acked < o.seq)
				{
					arm(o, now);
					return {out(Op::Snapshot, id, o.gen, snapshot_payload(o.seq, o.state))};
				}
			}
			return {};
		case St::Committing:
			if (x == o.gen)
			{
				const bool again = o.again;
				bury(id, x);
				m_objs.erase(it);
				if (again)
					return want_online(id, now);
			}
			return {};
		case St::Destroying:
			if (x > std::max(o.gen, tomb(id)))
			{
				bury(id, x);
				m_objs.erase(it);
			}
			return {};
		default:
			return {};
		}
	}

	std::vector<Packet> on_ack(const Packet& p, uint64_t now)
	{
		const uint16_t id = p.key.id;
		const uint32_t x = p.key.gen;
		auto it = m_objs.find(id);
		if (it == m_objs.end())
			return {};
		Obj& o = it->second;
		switch (o.st)
		{
		case St::Spawning:
			if (x <= tomb(id))
				return {};
			o.st = St::Held;
			o.gen = x;
			o.committed = true;
			o.acked = 0;
			if (o.release_after)
			{
				o.st = St::Releasing;
				arm(o, now);
				return {out(Op::ReleasePrepare, id, x, o.state)};
			}
			if (o.seq)
			{
				arm(o, now);
				return {out(Op::Snapshot, id, x, snapshot_payload(o.seq, o.state))};
			}
			return {};
		case St::Releasing:
			if (x <= o.gen)
				return {};
			bury(id, o.gen);
			m_events.push_back(Event{Event::Kind::Remove, id, {}});
			o.st = St::Committing;
			o.gen = x;
			o.state.clear();
			arm(o, now);
			return {out(Op::ReleaseCommit, id, x)};
		case St::Committing:
			if (x == o.gen)
				return {out(Op::ReleaseCommit, id, x)}; // the Ack repeated
			return {};
		default:
			return {};
		}
	}

	std::vector<Packet> on_abort(const Packet& p)
	{
		const uint16_t id = p.key.id;
		const uint32_t x = p.key.gen;
		auto it = m_objs.find(id);
		if (it == m_objs.end())
		{
			bury(id, x);
			return {out(Op::AbortAck, id, x)}; // nothing of it here
		}
		Obj& o = it->second;
		switch (o.st)
		{
		case St::Requesting:
			m_events.push_back(Event{Event::Kind::Refused, id, {}});
			bury(id, x);
			m_objs.erase(it);
			return {out(Op::AbortAck, id, x)};
		case St::Held:
		case St::Releasing:
		case St::Destroying:
			if (o.gen != 0 && x >= o.gen)
			{
				// The world takes back the grant this incarnation came from (never committed).
				if (o.st != St::Destroying)
					m_events.push_back(Event{Event::Kind::Remove, id, {}});
				bury(id, x);
				m_objs.erase(it);
			}
			return {out(Op::AbortAck, id, x)};
		default:
			return {out(Op::AbortAck, id, x)}; // it holds no incarnation up to x
		}
	}

	void on_drop(const Packet& p)
	{
		const uint16_t id = p.key.id;
		const uint32_t x = p.key.gen;
		auto it = m_objs.find(id);
		if (it == m_objs.end())
			return;
		Obj& o = it->second;
		const bool writes = o.st == St::Held || o.st == St::Releasing || o.st == St::Spawning;
		if ((writes || o.st == St::Destroying) && o.gen <= x)
		{
			if (writes)
				m_events.push_back(Event{Event::Kind::Remove, id, {}});
			bury(id, o.gen);
			m_objs.erase(it);
		}
	}

	std::vector<Packet> on_snapshot_ack(const Packet& p, uint64_t now)
	{
		const uint16_t id = p.key.id;
		auto it = m_objs.find(id);
		if (it == m_objs.end() || it->second.st != St::Held || it->second.gen != p.key.gen)
			return {};
		Obj& o = it->second;
		uint32_t seq = 0;
		std::vector<uint8_t> unused;
		if (!read_snapshot(p.payload, seq, unused))
			return {};
		o.committed = true; // the world takes snapshots from the owner only
		if (seq == 0)
		{
			// The world asks for the state (a resync).
			if (!o.seq)
				o.seq = 1;
			o.acked = 0;
			arm(o, now);
			return {out(Op::Snapshot, id, o.gen, snapshot_payload(o.seq, o.state))};
		}
		o.acked = std::max(o.acked, std::min(seq, o.seq));
		return {};
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
			if (!r || r->owner != f.server || r->phase != Phase::Stable)
				break; // moved since
			++ledger.m_counters.violations;
			ledger.set_owner(f.id, *r, kWorld);
			out.push_back(detail::packet(Op::Drop, f.id, std::max(f.gen, r->gen), kWorld, f.server));
			r->gen = ledger.next_gen();
			r->snap_seq = 0;
			break;
		case Violation::Drift:
			if (!r || r->owner != f.server || r->phase != Phase::Stable || r->gen != f.gen)
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
