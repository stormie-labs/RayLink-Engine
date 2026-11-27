// Anomaly Coop - remote trade through a concrete trading computer (a mod's computer that lists
// traders far away and opens the stock trade with one of them).
//
// Without a permit nothing changes: a client's deal needs the stock talk and his character by the trader on the decider
// (coop_trade.cpp host_apply: kTradeReach), and his window closes 3 m from the trader (CUIActorMenu::CheckDistance). Remote
// trade with any NPC stays impossible.
//
// The permit: the decider of the player's level (co-op: the host; server: the map server of his level) issues it after its Lua
// (coop_mods_trade_computer.script, request mods.tcomp_permit) checked a real trading computer, the player at it and the trader
// it lists. The engine takes that grant as the player's trade window with that trader opens on the decider (ReqContainer),
// keeps it per player (one each), and drops it when that window closes, the player leaves, the world changes or it expires.
// A deal with that trader then needs, instead of the talk and the reach to the trader: the same player, the same trader, the
// permit not expired and his character still at the computer (the reach of the permit, at most kMaxReach). A wounded trader,
// the items, the prices and the money are checked as for every deal.
//
// The player's own window: beyond the stock 3 m from the trader it stays open only for the granted trader, before the grant
// runs out, while his actor stands within the stock 3 m of the computer (window_keeps). The engine asks it from the hook of
// CUIActorMenu::CheckDistance (coop_trade.cpp keeps_trade_window).
//
// Every number out of Lua goes through object_of / ttl_of / reach_of: whole ids 1..0xfffe, finite positive times and reaches,
// clamped to kMaxTtlMs and kMaxReach.
// Pure C++17, header-only, no engine dependencies: used by the engine and the unit-test exe (tests/test_trade_permit.cpp).
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>

#include "coop_ids.h"

namespace coop
{
namespace trade_permit
{
constexpr uint16_t kNoObject = kNoId;
constexpr uint32_t kNoPlayer = 0xFFFFFFFFu;
constexpr uint64_t kMaxTtlMs = 600000; // 10 min: a trade window after a talk lives as long (talks::kTradeAfterTalkMs)
constexpr float kMaxReach = 6.f; // the player's character by the computer on the decider: the window's 3 m and the lag (kTradeReach)
constexpr float kWindowReach = 3.f; // the player's own window: the stock 3 m of CUIActorMenu::CheckDistance, from the computer
constexpr size_t kMaxPermits = 64; // one per player; more players than this never sit in one session

// An object id as Lua gives it (a number): a whole number 1..0xfffe, else kNoObject. 0 is the decider's own actor.
inline uint16_t object_of(double v)
{
	if (!(v >= 1.0 && v <= 65534.0) || std::floor(v) != v)
		return kNoObject;
	return uint16_t(v);
}

// A time in ms as Lua gives it: a whole number > 0, at most kMaxTtlMs; 0 = not valid.
inline uint64_t ttl_of(double v)
{
	if (!(v >= 1.0) || std::isinf(v) || std::floor(v) != v)
		return 0;
	return v >= double(kMaxTtlMs) ? kMaxTtlMs : uint64_t(v);
}

// A reach in metres as Lua gives it: finite and > 0, at most kMaxReach; 0 = not valid.
inline float reach_of(double v)
{
	if (!(v > 0.0) || std::isinf(v))
		return 0.f;
	return v >= double(kMaxReach) ? kMaxReach : float(v);
}

// `now` + `ttl` without wrapping around.
inline uint64_t expiry(uint64_t now, uint64_t ttl)
{
	return now > UINT64_MAX - ttl ? UINT64_MAX : now + ttl;
}

struct Permit
{
	uint32_t player = kNoPlayer;
	uint16_t computer = kNoObject;
	uint16_t trader = kNoObject;
	uint64_t expires = 0; // the decider's clock, ms
	float reach = 0.f; // the player's character to the computer, at most
};

// What the decider sees at a deal. to_computer: the player's character to the permit's computer in metres; negative or NaN:
// not measured (no computer here, no character).
struct Facts
{
	uint32_t player = kNoPlayer;
	uint16_t trader = kNoObject;
	uint64_t now = 0;
	float to_computer = -1.f;
};

enum class Verdict : uint8_t
{
	None = 0, // no permit: the stock rules
	Granted,
	OtherPlayer,
	OtherTrader, // a permit for another trader: this one keeps the stock rules
	Expired,
	NoComputer,
	FarFromComputer,
};

inline bool granted(Verdict v) { return v == Verdict::Granted; }

inline const char* verdict_name(Verdict v)
{
	switch (v)
	{
	case Verdict::None: return "none";
	case Verdict::Granted: return "granted";
	case Verdict::OtherPlayer: return "another player's";
	case Verdict::OtherTrader: return "another trader";
	case Verdict::Expired: return "expired";
	case Verdict::NoComputer: return "no computer";
	case Verdict::FarFromComputer: return "far from the computer";
	default: return "?";
	}
}

inline Verdict judge(const Permit* p, const Facts& f)
{
	if (!p)
		return Verdict::None;
	if (p->player != f.player)
		return Verdict::OtherPlayer;
	if (p->trader != f.trader)
		return Verdict::OtherTrader;
	if (f.now >= p->expires)
		return Verdict::Expired;
	if (!(f.to_computer >= 0.f)) // NaN too
		return Verdict::NoComputer;
	if (f.to_computer > p->reach)
		return Verdict::FarFromComputer;
	return Verdict::Granted;
}

// The decider's permits, one per player.
class Book
{
public:
	// A bad field (seat 0 or no player, an id outside 1..0xfffe, the computer = the trader, no time, no reach) keeps nothing.
	// A player's new permit replaces his old one. Expired permits make room; a book full of live ones refuses.
	bool issue(uint32_t player, uint16_t computer, uint16_t trader, uint64_t now, uint64_t ttl_ms, float reach)
	{
		const uint64_t ttl = ttl_ms > kMaxTtlMs ? kMaxTtlMs : ttl_ms;
		if (player == 0 || player == kNoPlayer || computer == 0 || computer == kNoObject || trader == 0 ||
		    trader == kNoObject || computer == trader || ttl == 0 || !(reach > 0.f) || std::isinf(reach))
			return false;
		Permit p;
		p.player = player;
		p.computer = computer;
		p.trader = trader;
		p.expires = expiry(now, ttl);
		p.reach = reach > kMaxReach ? kMaxReach : reach;
		for (size_t i = 0; i < m_count; ++i)
			if (m_permits[i].player == player)
			{
				m_permits[i] = p;
				return true;
			}
		prune(now);
		if (m_count >= kMaxPermits)
			return false;
		m_permits[m_count++] = p;
		return true;
	}

	const Permit* find(uint32_t player) const
	{
		for (size_t i = 0; i < m_count; ++i)
			if (m_permits[i].player == player)
				return &m_permits[i];
		return nullptr;
	}

	Verdict judge(const Facts& f) const { return trade_permit::judge(find(f.player), f); }

	// The player holds a live permit for `trader` (his window may show that trader's goods before any deal).
	bool covers(uint32_t player, uint16_t trader, uint64_t now) const
	{
		const Permit* p = find(player);
		return p && p->trader == trader && now < p->expires;
	}

	bool revoke(uint32_t player)
	{
		for (size_t i = 0; i < m_count; ++i)
			if (m_permits[i].player == player)
			{
				m_permits[i] = m_permits[--m_count];
				return true;
			}
		return false;
	}

	void clear() { m_count = 0; }
	size_t size() const { return m_count; }

private:
	void prune(uint64_t now)
	{
		for (size_t i = 0; i < m_count;)
		{
			if (now >= m_permits[i].expires)
				m_permits[i] = m_permits[--m_count];
			else
				++i;
		}
	}

	Permit m_permits[kMaxPermits];
	size_t m_count = 0;
};

// The player's machine: what its Lua was granted for one trade window (its own clock and numbering).
struct Window
{
	uint16_t computer = kNoObject;
	uint16_t trader = kNoObject;
	uint64_t until = 0;
};

// The grant as Lua gives it (coop_mods_trade_computer.window_grant): the computer's id and the ms left. A bad value: no grant.
inline Window window_of(uint16_t trader, double computer, double ms_left, uint64_t now)
{
	Window w;
	const uint16_t c = object_of(computer);
	const uint64_t ttl = ttl_of(ms_left);
	if (c == kNoObject || trader == kNoObject || trader == 0 || c == trader || !ttl)
		return w;
	w.computer = c;
	w.trader = trader;
	w.until = expiry(now, ttl);
	return w;
}

// The stock window closes 3 m from the trader. With the grant for this trader, before it runs out, it stays open while the
// player's actor stands within the stock 3 m of the computer instead. to_computer negative or NaN: the computer is gone.
inline bool window_keeps(const Window& w, uint16_t partner, uint64_t now, float to_computer)
{
	return w.computer != kNoObject && w.trader != kNoObject && w.trader == partner && now < w.until && to_computer >= 0.f &&
	    to_computer <= kWindowReach;
}
} // namespace trade_permit
} // namespace coop
