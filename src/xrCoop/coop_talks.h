// Anomaly Coop - the host's memory of a player's recent talk locks (Ruling F2-34, F2 T12 round 2).
// coop_npc_companions.script takes a client's companion recruit only right after that player talked to the NPC (or to
// another member of its squad): the stock recruit conditions live in the dialog, which runs on the client. "Holds the
// lock now" is not enough: a client sends DialogUnlock as its talk window closes and the recruit event from its Lua
// outbox later in the frame; the host applies the unlock in the session update, before the event reaches Lua
// (deliver_inbox). So coop_dialog.cpp records, per player, the NPCs whose lock just ended, and coop.talk_recent asks
// talked_recently with a window (5 s for a recruit).
//
// B8 (build 10): the host also judges whether an NPC talks to a client's character at all (the answer to a lock
// request), and says why not; and the client names the cause when a talk that the host allowed does not start. The
// pure parts of both are here: the DialogLockResult layout, the refusal reasons, the start-blocker classification.
// Pure C++17, header-only, no engine dependencies: used by the engine and the unit-test exe.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "coop_wire.h"

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

// R4 2 on Anomaly 1.5.3: the counterparty of a client's deal is one he trades with now. The stock trade flag says so for
// an engine trade window; Anomaly opens trade from the dialog, and its meet logic turns that flag off on every update
// (xr_meet.script), so there the player's talk with the NPC says so: held now, or ended no more than kTradeAfterTalkMs
// ago (the dialog hands over to the trade window, which may stay open a while). A wounded NPC never trades.
constexpr uint64_t kTradeAfterTalkMs = 600000;
inline bool may_trade_with(bool trade_flag, bool talked_recently, bool wounded)
{
	return !wounded && (trade_flag || talked_recently);
}

// ---------------------------------------------------------------------------------------------------------------------
// B8: DialogLockResult - the host's answer to a client's request for the talk lock (ids in host numbering).
//   u16 npc, u8 result, and for NotWilling one more byte: the Reason. Results 0..4 are unchanged since protocol 8; an
//   older client reads three bytes and ignores the rest, so it only prints "talk refused by the host: ?".
// ---------------------------------------------------------------------------------------------------------------------
namespace lock
{
enum LockResult : uint8_t
{
	Granted = 0,
	Busy = 1, // another player talks to this NPC
	Gone = 2, // no such NPC on the host or it is dead
	Refused = 3, // not a talk partner (player avatar)
	TooFar = 4, // the host sees the player's character far from the NPC
	NotWilling = 5, // B8: the NPC would not talk to this player's character (see Reason)
};

inline const char* name(uint8_t r)
{
	switch (r)
	{
	case Granted: return "granted";
	case Busy: return "busy";
	case Gone: return "gone";
	case Refused: return "refused";
	case TooFar: return "too far";
	case NotWilling: return "not willing";
	default: return "?";
	}
}
} // namespace lock

// Why the NPC does not talk to the player's character. None = it does.
enum class Reason : uint8_t
{
	None = 0,
	Unwilling = 1, // the meet rules of the running game say no (or no better word)
	Hostile = 2, // an enemy of the player's group and not willing to talk
	Combat = 3, // fighting an enemy of the player: no time for talking
	Wounded = 4, // wounded and unable to talk
};

// The name the host's Lua (coop_npc_meet.talk_verdict) returns. A refusal never reads as "allowed": no name, an empty
// or an unknown one is Unwilling.
inline Reason reason_from_name(const char* name)
{
	if (name)
	{
		if (!std::strcmp(name, "hostile"))
			return Reason::Hostile;
		if (!std::strcmp(name, "combat"))
			return Reason::Combat;
		if (!std::strcmp(name, "wounded"))
			return Reason::Wounded;
	}
	return Reason::Unwilling;
}

inline const char* reason_name(Reason r)
{
	switch (r)
	{
	case Reason::Hostile: return "hostile";
	case Reason::Combat: return "combat";
	case Reason::Wounded: return "wounded";
	case Reason::Unwilling: return "unwilling";
	default: return "none";
	}
}

// The byte on the wire. 0 (a refusal without a reason) and a value this build does not know (a newer host) read as
// Unwilling: the client still shows a line.
inline Reason reason_from_wire(uint8_t b)
{
	switch (static_cast<Reason>(b))
	{
	case Reason::Hostile:
	case Reason::Combat:
	case Reason::Wounded:
	case Reason::Unwilling: return static_cast<Reason>(b);
	default: return Reason::Unwilling;
	}
}

// The HUD line: the string-table id (configs/text/*/st_coop_core.xml) and the English text shown when the table lacks
// it.
inline const char* reason_string_id(Reason r)
{
	switch (r)
	{
	case Reason::Hostile: return "st_coop_talk_hostile";
	case Reason::Combat: return "st_coop_talk_combat";
	case Reason::Wounded: return "st_coop_talk_wounded";
	default: return "st_coop_talk_unwilling";
	}
}

inline const char* reason_fallback(Reason r)
{
	switch (r)
	{
	case Reason::Hostile: return "He is hostile to you and will not talk";
	case Reason::Combat: return "No time for talking: he is in a fight";
	case Reason::Wounded: return "He is wounded and cannot talk";
	default: return "He does not want to talk to you";
	}
}

struct LockResultMsg
{
	uint16_t npc = kNoNpc;
	uint8_t result = lock::Granted;
	Reason reason = Reason::None; // only NotWilling carries one
};

inline void write_lock_result(Writer& w, const LockResultMsg& m)
{
	w.u16(m.npc);
	w.u8(m.result);
	if (m.result == lock::NotWilling)
		w.u8(static_cast<uint8_t>(m.reason == Reason::None ? Reason::Unwilling : m.reason));
}

// False on a cut message. NotWilling without the reason byte (an older host) reads as Unwilling.
inline bool read_lock_result(Reader& r, LockResultMsg& m)
{
	m.npc = r.u16();
	m.result = r.u8();
	m.reason = Reason::None;
	if (!r.ok())
		return false;
	if (m.result == lock::NotWilling)
		m.reason = r.remaining() >= 1 ? reason_from_wire(r.u8()) : Reason::Unwilling;
	return r.ok();
}

// ---------------------------------------------------------------------------------------------------------------------
// B8: a talk the host allowed did not start on the client. The state the engine reads right after the attempt; the
// first cause in the order below is reported in the log (one line), then the lock is given back.
// ---------------------------------------------------------------------------------------------------------------------
struct StartState
{
	bool npc_found = true; // the NPC's object exists here
	bool npc_alive = true;
	bool actor_alive = true; // the player's own character
	bool paused = false; // the game is paused (the menu is open)
	bool actor_talking = false; // the character is already in a talk
	bool window_shown = false; // a talk window (or another one) is open
	bool partner_talking = false; // the NPC is already in a talk
	bool talk_enabled = true; // CInventoryOwner::IsTalkEnabled of the NPC
};

enum class StartBlock : uint8_t
{
	None = 0, // nothing in the state explains it: the engine refused for its own reason
	NpcGone,
	NpcDead,
	ActorDead,
	Paused,
	ActorBusy,
	PartnerBusy,
	TalkOff,
};

inline StartBlock start_block(const StartState& s)
{
	if (!s.npc_found)
		return StartBlock::NpcGone;
	if (!s.npc_alive)
		return StartBlock::NpcDead;
	if (!s.actor_alive)
		return StartBlock::ActorDead;
	if (s.paused)
		return StartBlock::Paused;
	if (s.actor_talking || s.window_shown)
		return StartBlock::ActorBusy;
	if (s.partner_talking)
		return StartBlock::PartnerBusy;
	if (!s.talk_enabled)
		return StartBlock::TalkOff;
	return StartBlock::None;
}

inline const char* start_block_name(StartBlock b)
{
	switch (b)
	{
	case StartBlock::NpcGone: return "the NPC is gone";
	case StartBlock::NpcDead: return "the NPC is dead";
	case StartBlock::ActorDead: return "your character is dead";
	case StartBlock::Paused: return "busy: the game is paused";
	case StartBlock::ActorBusy: return "busy: you are in a talk or a window is open";
	case StartBlock::PartnerBusy: return "busy: the NPC is in a talk";
	case StartBlock::TalkOff: return "talk is switched off on the NPC";
	default: return "refused by the engine (no known cause)";
	}
}
} // namespace talks
} // namespace coop
