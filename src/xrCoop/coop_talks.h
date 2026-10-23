// Anomaly Coop - the host's memory of a player's recent talk locks (Ruling F2-34, F2 T12 round 2).
// coop_npc_companions.script takes a client's companion recruit only right after that player talked to the NPC (or to
// another member of its squad): the stock recruit conditions live in the dialog, which runs on the client. "Holds the
// lock now" is not enough: a client sends DialogUnlock as its talk window closes and the recruit event from its Lua
// outbox later in the frame; the host applies the unlock in the session update, before the event reaches Lua
// (deliver_inbox). So coop_dialog.cpp records, per player, the NPCs whose lock just ended, and coop.talk_recent asks
// talked_recently with a window (5 s for a recruit).
// Pure C++17, header-only, no engine dependencies: used by the engine and the unit-test exe.
#pragma once

#include <cstddef>
#include <cstdint>

namespace coop
{
namespace talks
{
constexpr uint16_t kNoNpc = 0xffff;

// The last kKept talk locks of one player that ended: the NPC and when. Bounded; the same NPC keeps one entry.
class Recent
{
public:
	static constexpr size_t kKept = 2;

	// The lock on `npc` ended at `now` (released, replaced by another talk, timed out, the NPC gone).
	void ended(uint16_t npc, uint64_t now)
	{
		if (npc == kNoNpc)
			return;
		for (size_t i = 0; i < m_count; ++i)
			if (m_talks[i].npc == npc)
			{
				m_talks[i].at = now;
				return;
			}
		if (m_count < kKept)
		{
			m_talks[m_count++] = {npc, now};
			return;
		}
		size_t oldest = 0;
		for (size_t i = 1; i < m_count; ++i)
			if (m_talks[i].at < m_talks[oldest].at)
				oldest = i;
		m_talks[oldest] = {npc, now};
	}

	// A lock on `npc` ended no more than `window_ms` before `now` (a clock read before the end counts as now).
	bool within(uint16_t npc, uint64_t now, uint64_t window_ms) const
	{
		if (npc == kNoNpc)
			return false;
		for (size_t i = 0; i < m_count; ++i)
			if (m_talks[i].npc == npc)
				return now <= m_talks[i].at || now - m_talks[i].at <= window_ms;
		return false;
	}

	size_t size() const { return m_count; }
	void clear() { m_count = 0; }

private:
	struct Talk
	{
		uint16_t npc = kNoNpc;
		uint64_t at = 0;
	};
	Talk m_talks[kKept];
	size_t m_count = 0;
};

// The player holds the lock of `npc` now (`holding` = the NPC whose lock he holds, kNoNpc if none), or his lock on it
// ended no more than `window_ms` ago.
inline bool talked_recently(uint16_t holding, const Recent& recent, uint16_t npc, uint64_t now, uint64_t window_ms)
{
	if (npc == kNoNpc)
		return false;
	return holding == npc || recent.within(npc, now, window_ms);
}
} // namespace talks
} // namespace coop
