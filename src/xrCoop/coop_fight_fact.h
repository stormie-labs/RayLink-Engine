// Anomaly Coop — a fight fact: the reputation a fight on a level gave a player, told by the process that leads that level to the keeper
// of his record. Pure C++17 (tests/test_fight_fact.cpp).
//
// Who does what: the engine's relation action on his character (coop_story.cpp RelationActionScope) counts the change where the fight
// is. The co-op host keeps the record itself: it applies the change to its book and tells his machine (RelReputationDelta). A map server
// keeps no record: its level side (LevelSide::level_fight_reputation, srv/coop_srv_mode.cpp) numbers the fact and sends it up as a
// Msg::Relation op kRelOp; the world server's script checks the transport's sender (a map server's bridge), his route and the order
// (coop_srv_progress.script, coop_trust.pass on every refusal), and applies it (coop.progress.apply_verified_fight) before his machine
// can report it in Progress. Nothing here asks a mode.
#pragma once

#include <cstdint>

#include "coop_ids.h"
#include "coop_wire.h"

namespace coop
{
namespace fight
{
// The Msg::Relation op of a fact (coop_story.cpp RelUpFightFact, static_assert there); a player's machine never sends it.
constexpr uint8_t kRelOp = 17;
// The largest change one fact carries, either way (the same bound as coop_srv_progress.script MAX_COUNT).
constexpr int32_t kMaxDelta = 10000;

// Wire after the op byte: u32 epoch (the world epoch it happened in), u32 fact (1.. in the order of its level's process), u16 character
// (the world's numbering), i32 delta. 14 bytes, nothing after them.
struct FactMsg
{
	uint32_t epoch = 0;
	uint32_t fact = 0;
	uint16_t character = kNoId;
	int32_t delta = 0;

	void write(Writer& w) const
	{
		w.u8(kRelOp);
		w.u32(epoch);
		w.u32(fact);
		w.u16(character);
		w.i32(delta);
	}

	// Reads the fields after the op byte; false: short, longer than a fact, or out of bounds (`*this` is then empty).
	bool read(Reader& r)
	{
		FactMsg tmp;
		tmp.epoch = r.u32();
		tmp.fact = r.u32();
		tmp.character = r.u16();
		tmp.delta = r.i32();
		if (!r.ok() || r.remaining() != 0 || !valid(tmp))
		{
			*this = FactMsg();
			return false;
		}
		*this = tmp;
		return true;
	}

	static bool valid(const FactMsg& m)
	{
		return m.fact != 0 && m.character != 0 && m.character != kNoId && m.delta != 0 && m.delta >= -kMaxDelta &&
		       m.delta <= kMaxDelta;
	}
};

// A change as one fact carries it.
inline int32_t bounded(int delta) { return delta > kMaxDelta ? kMaxDelta : (delta < -kMaxDelta ? -kMaxDelta : int32_t(delta)); }

// The numbering of a level process's facts: increasing from 1 (the keeper takes only a number above the last one of his route).
class Facts
{
public:
	uint32_t next()
	{
		if (m_last == UINT32_MAX)
			m_last = 0;
		return ++m_last;
	}

private:
	uint32_t m_last = 0;
};
} // namespace fight
} // namespace coop
