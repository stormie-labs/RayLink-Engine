// Anomaly Coop — player money on the host (F2 T1, review R4 7).
//
// The host owns every player's balance: the money of his character on the host (the proxy's m_dwMoney, kept in the
// host save). A client never sends an amount, only a change with a cause (Msg::ReqMoney = MoneyReq); the host checks
// it and answers every change with the balance it holds and the change it got to (Msg::MoneyBalance =
// MoneyBalanceMsg). Rules (apply_client_delta):
//   - a spend is accepted while the balance covers it;
//   - money gained in a dialog while the player holds the NPC's talk lock: up to Policy::dialog_gain_cap per lock;
//   - any other gain (a mod's give_money, a service, the rest of a dialog payout) comes out of the player's allowance,
//     a bucket of money: Policy::script_gain_burst in a row, refilled with script_gain_per_min per minute;
//   - no balance goes above Policy::max_money;
//   - a transfer to another player is a spend for the giver; the caller credits the receiver.
// Trades, transfers and host grants change the balance on the host itself (coop_money host_adjust). The client shows
// the host's balance plus its own changes the host has not answered yet (client_view).
// Pure C++17, header-only, no engine dependencies: used by the engine and by the unit-test exe.
#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

#include "coop_wire.h"

namespace coop
{
namespace ledger
{
enum class Cause : uint8_t
{
	Script = 0, // the client's game changed the money by itself (Lua give_money, a service, a mod)
	Dialog = 1, // changed while the client held an NPC's talk lock (a dialog action paid or charged)
	Transfer = 2, // the client gives money to another player: MoneyReq::ref is the receiver, the delta is negative
};
constexpr uint8_t kCauseCount = 3;

// Host settings (configs\coop_money.ltx [coop_money], console coop_money_*; Ruling 21).
struct Policy
{
	uint32_t script_gain_burst = 50000; // money a client may gain without the host in a row
	uint32_t script_gain_per_min = 10000; // and then per minute
	uint32_t dialog_gain_cap = 100000; // gained in dialogs per talk lock
	uint32_t max_money = 100000000; // no balance goes above this
};

// The allowance of one player: a bucket of money, full at the first gain, refilled per minute up to the burst.
struct Allowance
{
	double tokens = 0;
	uint64_t at = 0; // ms of the last refill
	bool started = false;
};

enum class Verdict : uint8_t
{
	Applied,
	Insufficient, // a spend above the balance
	NoAllowance, // a gain above what the allowance (and the dialog cap) holds now
	OverMax, // the balance would go above max_money
	Malformed, // unknown cause, a transfer that gives nothing
};

// Refills `a` up to `now_ms` (the first call fills it). A clock that goes back refills nothing.
inline void refill(Allowance& a, const Policy& p, uint64_t now_ms)
{
	if (!a.started)
	{
		a.started = true;
		a.tokens = double(p.script_gain_burst);
		a.at = now_ms;
		return;
	}
	if (now_ms <= a.at)
		return;
	const double gained = double(now_ms - a.at) * double(p.script_gain_per_min) / 60000.0;
	a.tokens = std::min(double(p.script_gain_burst), a.tokens + gained);
	a.at = now_ms;
}

// Applies one change a client asked for to its balance on the host. Everything or nothing: a refused change leaves
// `balance`, `dialog_gained` and the allowance's tokens as they were. `dialog_lock`: the player holds an NPC's talk
// lock now; `dialog_gained`: what dialogs paid him under that lock (the caller zeroes it when the lock changes).
inline Verdict apply_client_delta(uint32_t& balance, int64_t delta, Cause cause, bool dialog_lock,
    uint32_t& dialog_gained, Allowance& script, const Policy& p, uint64_t now_ms)
{
	if (uint8_t(cause) >= kCauseCount)
		return Verdict::Malformed;
	if (cause == Cause::Transfer && delta >= 0)
		return Verdict::Malformed; // the giver's side of a transfer is a spend
	if (delta == 0)
		return Verdict::Applied;
	if (delta < 0)
	{
		const uint64_t spend = uint64_t(-(delta + 1)) + 1u; // no overflow at INT64_MIN
		if (spend > balance)
			return Verdict::Insufficient;
		balance = uint32_t(balance - spend);
		return Verdict::Applied;
	}
	const uint64_t gain = uint64_t(delta);
	if (uint64_t(balance) + gain > uint64_t(p.max_money))
		return Verdict::OverMax;
	uint64_t from_dialog = 0;
	if (cause == Cause::Dialog && dialog_lock && dialog_gained < p.dialog_gain_cap)
		from_dialog = std::min<uint64_t>(gain, uint64_t(p.dialog_gain_cap - dialog_gained));
	const uint64_t from_script = gain - from_dialog;
	if (from_script)
	{
		refill(script, p, now_ms);
		constexpr double kSlack = 1e-6; // the refill is fractional: whole RU must not miss by a rounding error
		if (double(from_script) > script.tokens + kSlack)
			return Verdict::NoAllowance;
		script.tokens = std::max(0.0, script.tokens - double(from_script));
	}
	dialog_gained += uint32_t(from_dialog);
	balance = uint32_t(uint64_t(balance) + gain);
	return Verdict::Applied;
}

// A change the client sent and the host has not answered yet.
struct Pending
{
	uint32_t seq;
	int32_t delta;
};

// Sequence numbers compare as serial numbers (as every coop seq): `a` is newer than `b`.
inline bool seq_newer(uint32_t a, uint32_t b) { return int32_t(a - b) > 0; }

// What the client shows: the host's balance plus its changes the host has not answered yet (seq newer than
// `acked_seq`), within [0, max(max_money, host_balance)] — the host's own figure is never cut.
inline uint32_t client_view(uint32_t host_balance, uint32_t acked_seq, const std::vector<Pending>& pending,
    uint32_t max_money = Policy().max_money)
{
	int64_t v = int64_t(host_balance);
	for (const Pending& p : pending)
		if (seq_newer(p.seq, acked_seq))
			v += p.delta;
	const int64_t hi = std::max<int64_t>(int64_t(max_money), int64_t(host_balance));
	return uint32_t(std::min<int64_t>(std::max<int64_t>(v, 0), hi));
}

// Drops the changes an answer with `acked_seq` covers (the host has applied or refused them).
inline void forget_acked(std::vector<Pending>& pending, uint32_t acked_seq)
{
	pending.erase(std::remove_if(pending.begin(), pending.end(),
	                  [acked_seq](const Pending& p) { return !seq_newer(p.seq, acked_seq); }),
	    pending.end());
}

// ---------------------------------------------------------------- wire

// Msg::ReqMoney, client -> host. Wire: u32 seq, i32 delta, u8 cause, u32 ref (13 bytes).
struct MoneyReq
{
	uint32_t seq = 0;
	int32_t delta = 0;
	Cause cause = Cause::Script;
	uint32_t ref = 0; // Transfer: the receiver's peer id; else 0 (ignored)
};
constexpr size_t kMoneyReqSize = 13;

// Msg::MoneyBalance, host -> the player it belongs to. Wire: u32 balance, u32 acked_seq (8 bytes).
// acked_seq: the newest ReqMoney of that player the host has applied or refused (0: none yet).
struct MoneyBalanceMsg
{
	uint32_t balance = 0;
	uint32_t acked_seq = 0;
};
constexpr size_t kMoneyBalanceSize = 8;

inline void write(Writer& w, const MoneyReq& m)
{
	w.u32(m.seq);
	w.i32(m.delta);
	w.u8(uint8_t(m.cause));
	w.u32(m.ref);
}

// False on overrun or an unknown cause.
inline bool read(Reader& r, MoneyReq& m)
{
	m.seq = r.u32();
	m.delta = r.i32();
	const uint8_t cause = r.u8();
	m.ref = r.u32();
	if (!r.ok() || cause >= kCauseCount)
		return false;
	m.cause = Cause(cause);
	return true;
}

inline void write(Writer& w, const MoneyBalanceMsg& m)
{
	w.u32(m.balance);
	w.u32(m.acked_seq);
}

// False on overrun.
inline bool read(Reader& r, MoneyBalanceMsg& m)
{
	m.balance = r.u32();
	m.acked_seq = r.u32();
	return r.ok();
}
} // namespace ledger
} // namespace coop
