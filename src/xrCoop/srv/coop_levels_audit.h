// Server ownership audit (A-S7N). Only the world server tracks rounds.
#pragma once

#include "../coop_ids.h"
#include "../coop_level_hash.h"
#include "../coop_hash.h"
#include "../coop_protocol.h"

#include <algorithm>
#include <iterator>
#include <set>
#include <vector>

namespace coop
{
namespace srv
{
// Manual audit extensions to the Server wire table (same message IDs, sender rules and world gate):
// Message                   Direction       Body                                      Gate / sender
// 0x01DA LevelResync        World -> bridge u32 mask [, u32 round]                     Drop / HostOnly
// 0x01D5 LevelEntity op=6   Bridge -> world u32 mask, u16 n, n x {u16 id, u32 gen},    Hold / TrunkOnly
//                                            [, u32 round] after the last row
// The bridge echoes the nonzero round. Legacy readers use the original prefix. Background and legacy packets have
// no trailer, can still heal the ledger, and cannot acknowledge a manual audit even with a full bucket mask.
inline void write_audit_round(Writer& w, uint32_t round)
{
	if (round)
		w.u32(round);
}

inline bool read_audit_round(Reader& r, uint32_t& round)
{
	round = 0;
	if (!r.ok())
		return false;
	if (r.remaining() == 0)
		return true;
	if (r.remaining() != sizeof(round))
		return false;
	round = r.u32();
	return round != 0;
}

// ---------------------------------------------------------------- the auditor compares content (membership)
// The background hash and the report carried who writes what under which gen (id, gen) alone: a squad whose member list differs
// between the world and the map server that holds it was never found. Now the hash of a held squad carries the digest of its
// members on both sides (Keeper::differs with a digest), and the map server's report carries the member list of each squad in the
// asked buckets; the world takes the writer's list (heal_members): a member the writer lists and the world's squad lacks joins it,
// a member the world's squad has and the writer does not list leaves it.
//
// Report wire (one exe for every role): LevelEntity op=6: u32 mask, u16 n, n x {u16 id, u32 gen} (LevelReportBody),
// then the members: u16 m, m x {u16 squad, u8 k, k x u16 member}, then [u32 round] (write_audit_round). A squad with no member is
// listed with k = 0.
struct MemberRow
{
	uint16_t squad = kNoId;
	std::vector<uint16_t> members;
};

constexpr size_t kMaxMemberRows = 8192; // squads in one report (every held object is at most one row)
constexpr size_t kMaxMembers = 255; // members of one squad (a u8 count; a stock squad has a dozen at most)

// Order-free digest of a member list; 0 for none (the hash's "not compared"), so an empty squad and a non-squad hash alike on both
// sides only when both are empty.
inline uint64_t members_digest(std::vector<uint16_t> ids)
{
	if (ids.empty())
		return 0;
	std::sort(ids.begin(), ids.end());
	ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
	uint64_t h = kFnv64Basis;
	for (const uint16_t id : ids)
		h = fnv1a64_step(fnv1a64_step(h, id & 0xffu), id >> 8);
	return h == 0 ? 1 : h;
}

struct MemberHeal
{
	std::vector<uint16_t> join; // the writer lists them, the world's squad lacks them
	std::vector<uint16_t> leave; // the world's squad has them, the writer does not list them
	bool empty() const { return join.empty() && leave.empty(); }
};

// The world's member list towards the writer's (the non-writer repairs towards the writer). Sorted, no repeats.
inline MemberHeal heal_members(std::vector<uint16_t> world, std::vector<uint16_t> writer)
{
	std::sort(world.begin(), world.end());
	world.erase(std::unique(world.begin(), world.end()), world.end());
	std::sort(writer.begin(), writer.end());
	writer.erase(std::unique(writer.begin(), writer.end()), writer.end());
	MemberHeal h;
	std::set_difference(writer.begin(), writer.end(), world.begin(), world.end(), std::back_inserter(h.join));
	std::set_difference(world.begin(), world.end(), writer.begin(), writer.end(), std::back_inserter(h.leave));
	return h;
}

// False, nothing written, for more rows or members than the reader takes.
[[nodiscard]] inline bool write_members(Writer& w, const std::vector<MemberRow>& rows)
{
	if (rows.size() > kMaxMemberRows)
		return false;
	for (const MemberRow& row : rows)
		if (row.squad == kNoId || row.members.size() > kMaxMembers)
			return false;
	w.u16(uint16_t(rows.size()));
	for (const MemberRow& row : rows)
	{
		w.u16(row.squad);
		w.u8(uint8_t(row.members.size()));
		for (const uint16_t id : row.members)
			w.u16(id);
	}
	return true;
}

// False, `rows` empty, for a cut list, a squad listed twice, kNoObject as a squad or a member, a count the packet cannot hold.
inline bool read_members(Reader& r, std::vector<MemberRow>& rows)
{
	rows.clear();
	const uint16_t n = r.u16();
	if (!r.ok() || n > kMaxMemberRows || size_t(n) * 3 > r.remaining())
		return false;
	std::vector<MemberRow> tmp;
	std::set<uint16_t> seen;
	tmp.reserve(n);
	for (uint16_t i = 0; i < n; ++i)
	{
		MemberRow row;
		row.squad = r.u16();
		const uint8_t k = r.u8();
		if (!r.ok() || row.squad == kNoId || !seen.insert(row.squad).second || size_t(k) * 2 > r.remaining())
			return false;
		row.members.reserve(k);
		for (uint8_t j = 0; j < k; ++j)
		{
			const uint16_t id = r.u16();
			if (!r.ok() || id == kNoId)
				return false;
			row.members.push_back(id);
		}
		tmp.push_back(std::move(row));
	}
	rows = std::move(tmp);
	return true;
}

// Peers are taken from Session's authenticated bridge transports. The bridge disconnect hook removes the slot
// before it can be reused. Starting a round replaces the previous pending set, including for the same connections.
class LevelsAudit
{
public:
	template <class Log>
	uint32_t begin(const std::vector<PeerId>& bridges, Log&& log)
	{
		if (++m_round == 0)
			++m_round; // zero means no round on the wire
		m_pending.clear();
		m_pending.insert(bridges.begin(), bridges.end());
		log("[coop] levels audit begin round=%u bridges=%u", m_round, static_cast<unsigned>(m_pending.size()));
		return m_round;
	}

	template <class Log>
	bool reply(uint32_t round, PeerId from, uint32_t mask, Log&& log)
	{
		if (round != m_round || mask != xfer::kAllBuckets || m_pending.erase(from) == 0)
		{
			log("[coop] levels audit refused round=%u peer=%u mask=%08x total=%u", round, from, mask, ++m_refused);
			return false;
		}
		log("[coop] levels audit ack round=%u peer=%u mask=%08x", round, from, mask);
		return true;
	}

	void disconnect(PeerId peer) { m_pending.erase(peer); }
	void clear() { m_pending.clear(); }

private:
	uint32_t m_round = 0;
	uint32_t m_refused = 0;
	std::set<PeerId> m_pending;
};
} // namespace srv
} // namespace coop
