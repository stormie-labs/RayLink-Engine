// Server ownership audit (A-S7N). Only the world server tracks rounds.
#pragma once

#include "coop_level_hash.h"
#include "../coop_protocol.h"

#include <set>

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
