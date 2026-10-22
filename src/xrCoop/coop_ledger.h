// Anomaly Coop — player money on the host (F2 T1, review R4 7; fix round 1: Ruling F2-1).
//
// The host owns every player's balance: the money of his character on the host (the proxy's m_dwMoney, kept in the
// host save). A client never sends an amount, only a change with a cause (Msg::ReqMoney = MoneyReq); the host checks
// it and answers every change with the balance it holds and the change it got to (Msg::MoneyBalance =
// MoneyBalanceMsg). Rules (apply_client_delta):
//   - a spend is accepted while the balance covers it;
//   - money a client gains by itself comes out of allowances, buckets of money kept per character for the whole
//     session (HostBook): a dialog payout while the player talks to an NPC (holds its talk lock) from the dialog
//     bucket (Policy::dialog_gain_cap in a row, dialog_gain_per_min per minute), anything else and the rest of a
//     payout from the script bucket (script_gain_burst in a row, script_gain_per_min per minute). Changing the NPC
//     or leaving the session refills nothing; a character made in this session (a new player key) starts with both
//     buckets empty (Ruling F2-10), and they refill from the moment it is live in the game (Ruling F2-20);
//   - no balance goes above Policy::max_money;
//   - a transfer to another player is a spend for the giver; the receiver is credited (check_transfer);
//   - a change of zero is malformed: a client never sends one.
// Trades, transfers and host grants change the balance on the host itself (coop_money host_adjust). The client shows
// the host's balance plus its own changes the host has not answered yet (client_view).
// Pure C++17, header-only, no engine dependencies: used by the engine and by the unit-test exe.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "coop_session.h" // find_key_value, migrate_key_value: the player key line of a character
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

// Host settings (configs\coop_money.ltx [coop_money], console coop_money_*; Ruling 21, Ruling F2-1).
struct Policy
{
	uint32_t script_gain_burst = 50000; // money a client may gain without the host in a row
	uint32_t script_gain_per_min = 10000; // and then per minute
	uint32_t dialog_gain_cap = 100000; // dialogs may pay this much in a row (one big quest reward)
	// and then this much per minute: several ordinary rewards (1 000 - 15 000 RU) a minute, a farm of talk locks
	// gains no more than 1.2 M RU an hour on top of the cap
	uint32_t dialog_gain_per_min = 20000;
	uint32_t max_money = 100000000; // no balance goes above this
};

// An allowance: a bucket of money, full at its first use, refilled per minute up to its size.
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
	NoAllowance, // a gain above what the allowances hold now
	OverMax, // the balance (the giver's or a transfer's receiver's) would go above max_money
	Malformed, // unknown cause, a change of zero, a transfer that gives nothing or to nobody
};

// Refills `a` (size `burst`, `per_min` a minute) up to `now_ms`; the first call fills it. A clock that goes back
// refills nothing.
inline void refill(Allowance& a, uint32_t burst, uint32_t per_min, uint64_t now_ms)
{
	if (!a.started)
	{
		a.started = true;
		a.tokens = double(burst);
		a.at = now_ms;
		return;
	}
	if (now_ms <= a.at)
		return;
	const double gained = double(now_ms - a.at) * double(per_min) / 60000.0;
	a.tokens = std::min(double(burst), a.tokens + gained);
	a.at = now_ms;
}

// The refill is fractional: whole RU must not miss by a rounding error.
constexpr double kSlack = 1e-6;

// Applies one change a client asked for to its balance on the host. Everything or nothing: a refused change leaves
// `balance` and the tokens of both allowances as they were. `dialog_lock`: the player talks to an NPC now (holds its
// talk lock); only then does a Dialog gain draw on `dialog` first. `from_dialog` (optional): the part of an applied
// gain the dialog allowance paid.
inline Verdict apply_client_delta(uint32_t& balance, int64_t delta, Cause cause, bool dialog_lock, Allowance& dialog,
    Allowance& script, const Policy& p, uint64_t now_ms, uint32_t* from_dialog_out = nullptr)
{
	if (from_dialog_out)
		*from_dialog_out = 0;
	if (uint8_t(cause) >= kCauseCount || delta == 0)
		return Verdict::Malformed;
	if (cause == Cause::Transfer && delta > 0)
		return Verdict::Malformed; // the giver's side of a transfer is a spend
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
	if (cause == Cause::Dialog && dialog_lock)
	{
		refill(dialog, p.dialog_gain_cap, p.dialog_gain_per_min, now_ms);
		from_dialog = std::min<uint64_t>(gain, uint64_t(std::max(0.0, dialog.tokens + kSlack)));
	}
	const uint64_t from_script = gain - from_dialog;
	if (from_script)
	{
		refill(script, p.script_gain_burst, p.script_gain_per_min, now_ms);
		if (double(from_script) > script.tokens + kSlack)
			return Verdict::NoAllowance;
		script.tokens = std::max(0.0, script.tokens - double(from_script));
	}
	if (from_dialog)
		dialog.tokens = std::max(0.0, dialog.tokens - double(from_dialog));
	balance = uint32_t(uint64_t(balance) + gain);
	if (from_dialog_out)
		*from_dialog_out = uint32_t(from_dialog);
	return Verdict::Applied;
}

// The receiver of a transfer as the host sees it.
struct Receiver
{
	uint32_t peer = 0xFFFFFFFFu;
	bool present = false; // a player of the session (joined, or the host) who has a character
	uint32_t balance = 0;
};

// The giver's side of a transfer of -delta from player `from` (balance `balance`) to `to`. Applied: `balance` is the
// giver's new balance and `credit` what the receiver gets; else both stay. Malformed: no receiver (nobody, himself) or
// nothing given.
inline Verdict check_transfer(uint32_t from, const Receiver& to, int64_t delta, uint32_t& balance, uint32_t& credit,
    const Policy& p)
{
	credit = 0;
	if (!to.present || to.peer == from)
		return Verdict::Malformed;
	if (delta >= 0)
		return Verdict::Malformed;
	const uint64_t give = uint64_t(-(delta + 1)) + 1u;
	if (give > balance)
		return Verdict::Insufficient;
	if (uint64_t(to.balance) + give > uint64_t(p.max_money))
		return Verdict::OverMax;
	balance = uint32_t(balance - give);
	credit = uint32_t(give);
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
// `acked_seq`), less `held` (F2 T5: upgrades it paid whose requests the host has not answered, UpgradeHolds), within
// [0, max(max_money, host_balance)] — the host's own figure is never cut.
inline uint32_t client_view(uint32_t host_balance, uint32_t acked_seq, const std::vector<Pending>& pending,
    uint32_t max_money = Policy().max_money, uint64_t held = 0)
{
	int64_t v = int64_t(host_balance);
	for (const Pending& p : pending)
		if (seq_newer(p.seq, acked_seq))
			v += p.delta;
	v -= int64_t(std::min<uint64_t>(held, uint64_t(INT64_MAX) / 2)); // at most 64 x u32 in practice
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
// ---------------------------------------------------------------- host book

// The key the host knows a character by: the digest on the "key" line of its custom data ([coop] key = <sha256 hex>),
// the line by which coop_join binds the character to its player (find_proxy_by_key). A save from before R1 I1 still
// holds the key itself: its digest is returned, as coop_join migrates it (here on a copy). Empty if there is none.
// Not the entity id: an id freed and given to another character later says nothing about who plays it (F2-13).
inline std::string character_key(std::string custom_data)
{
	migrate_key_value(custom_data);
	size_t at = 0, len = 0;
	return find_key_value(custom_data, at, len) ? custom_data.substr(at, len) : std::string();
}

// The allowances of one character (by its key, character_key). Kept for the whole session: a player who leaves and
// joins again (a new connection, maybe a new peer id) finds them as he left them. A character that was in the save
// when the session started gets full allowances at its first change; one made in this session (a new player key)
// starts empty and only gets the refills (Ruling F2-10). They refill only while the character is in the game: its
// player leaving freezes them (Ruling F2-13), and the clock runs from the moment he is live again — his join is complete,
// peer_online — not from his first gain: the time in the game before it is credited, the time away is not (Ruling F2-20).
struct CharacterAllowances
{
	Allowance script;
	Allowance dialog;
	bool away = false; // its player left: no refill until he is back
};

// What the host does with one ReqMoney.
struct Decision
{
	Verdict verdict = Verdict::Malformed;
	bool stale = false; // not newer than the newest answered change of this connection: nothing done
	uint32_t balance = 0; // the sender's balance after it (to write, and to answer with)
	uint32_t credit = 0; // a transfer applied: what the receiver gets
	uint32_t acked = 0; // MoneyBalance::acked_seq of the answer
	uint32_t gain_script = 0, gain_dialog = 0; // an applied gain, by the allowance that paid it
};

// The host's state of the players' money changes: per connection (peer id) the newest answered change, per character
// the allowances. A request is answered whatever the verdict (MoneyBalance{balance, acked}).
class HostBook
{
public:
	// A new session: nothing is kept. `from_save`: the characters in the world when it started (a player key the host
	// has not seen makes a new one later, which then starts with empty allowances).
	void start_session(const std::vector<std::string>& from_save)
	{
		m_acked.clear();
		m_character_of.clear();
		m_characters.clear();
		m_from_save.clear();
		for (const std::string& key : from_save)
			if (!key.empty())
				m_from_save.insert(key);
	}
	bool from_save(const std::string& character) const { return m_from_save.count(character) != 0; }

	// ReqMoney `q` of connection `peer` whose character is `character` and has `balance`. `dialog_lock`: he holds an
	// NPC's talk lock now; `to`: the receiver of a transfer (looked up from q.ref; ignored for other causes).
	Decision on_request(uint32_t peer, const std::string& character, const MoneyReq& q, uint32_t balance,
	    bool dialog_lock, const Receiver& to, const Policy& p, uint64_t now_ms)
	{
		Decision d;
		d.balance = balance;
		m_character_of[peer] = character;
		uint32_t& acked = m_acked[peer];
		if (!seq_newer(q.seq, acked))
		{
			d.stale = true; // a repeat or an old change: the answer only says where we are
			d.acked = acked;
			return d;
		}
		acked = q.seq;
		d.acked = q.seq;
		if (q.cause == Cause::Transfer)
		{
			d.verdict = check_transfer(peer, to, q.delta, d.balance, d.credit, p);
			return d;
		}
		// Ruling F2-13: the form first. A request refused by it, and a spend, touch no allowance and start no clock.
		if (uint8_t(q.cause) >= kCauseCount || q.delta <= 0)
		{
			Allowance untouched_dialog, untouched_script; // a spend never draws on them
			d.verdict = apply_client_delta(d.balance, q.delta, q.cause, false, untouched_dialog, untouched_script, p,
			    now_ms);
			return d;
		}
		CharacterAllowances& a = allowances_of(character, now_ms);
		uint32_t from_dialog = 0;
		d.verdict = apply_client_delta(d.balance, q.delta, q.cause, dialog_lock, a.dialog, a.script, p, now_ms,
		    &from_dialog);
		if (d.verdict == Verdict::Applied && q.delta > 0)
		{
			d.gain_dialog = from_dialog;
			d.gain_script = uint32_t(q.delta) - from_dialog;
		}
		return d;
	}

	// Connection `peer` is live in the world with `character` (its join is complete; Ruling F2-20): the refill clock of
	// the character runs from now, not from its first income.
	//   - a character made in this session: both buckets exist now, empty, clock at `now_ms`;
	//   - a character from the save that has not been here yet this session: full at its first income (a full bucket
	//     does not refill, so its clock cannot matter until then);
	//   - a character that is back: its tokens as it left them, clock at `now_ms` (nothing for the time away);
	//   - a character already in the game: nothing changes.
	// The connection is bound to the character from here, so that peer_left freezes it even if he never asked for money.
	// A connection that is never marked (no join hook ran) still works: its buckets are made at its first income.
	void peer_online(uint32_t peer, const std::string& character, uint64_t now_ms)
	{
		m_character_of[peer] = character;
		allowances_of(character, now_ms);
	}

	// MoneyBalance::acked_seq for connection `peer` (0: nothing answered yet).
	uint32_t acked(uint32_t peer) const
	{
		const auto it = m_acked.find(peer);
		return it == m_acked.end() ? 0u : it->second;
	}
	// A connection ended: its numbering goes (a new one starts at 1); the character's allowances stay, frozen until
	// he is back (Ruling F2-13).
	void peer_left(uint32_t peer)
	{
		m_acked.erase(peer);
		const auto who = m_character_of.find(peer);
		if (who == m_character_of.end())
			return;
		const auto it = m_characters.find(who->second);
		if (it != m_characters.end())
			it->second.away = true;
		m_character_of.erase(who);
	}
	// Nothing is kept, no character counts as one from the save.
	void reset() { start_session({}); }
	const CharacterAllowances* allowances(const std::string& character) const
	{
		const auto it = m_characters.find(character);
		return it == m_characters.end() ? nullptr : &it->second;
	}
	const std::map<uint32_t, uint32_t>& connections() const { return m_acked; }

private:
	// The allowances of `character`, made if there are none yet. Called when the character goes live (peer_online, the
	// normal way: Ruling F2-20) and by a request (the fallback for a connection that was never marked online). The
	// clock of a character that is back starts now, so what was spent away stays spent and the time away pays nothing.
	CharacterAllowances& allowances_of(const std::string& character, uint64_t now_ms)
	{
		const auto found = m_characters.find(character);
		if (found != m_characters.end())
		{
			CharacterAllowances& a = found->second;
			if (a.away)
			{
				// Ruling F2-13: back in the game. The clock goes on from now; the time away pays nothing.
				a.away = false;
				for (Allowance* x : {&a.script, &a.dialog})
					if (x->started && now_ms > x->at)
						x->at = now_ms;
			}
			return a;
		}
		CharacterAllowances& a = m_characters[character];
		a.script.at = a.dialog.at = now_ms;
		if (!from_save(character))
		{
			// Ruling F2-10: made in this session. Started and empty: refill() gives no burst, only the refills from now.
			a.script.started = a.dialog.started = true;
		}
		// else: from the save, not started: the first refill() makes it full (Ruling F2-10), whatever `at` is.
		return a;
	}

	std::map<uint32_t, uint32_t> m_acked; // peer -> newest answered seq
	std::map<uint32_t, std::string> m_character_of; // peer -> the key of his character (its last request)
	std::map<std::string, CharacterAllowances> m_characters; // character key -> allowances
	std::set<std::string> m_from_save; // keys of the characters in the world when the session started
};

// Settles an applied transfer (Decision::balance, Decision::credit) in `b`, which has set(peer, RU) and add(peer, RU):
// the giver's new balance first, then the receiver gets the credit on top of what he holds at that moment. So two
// connections on one character (a fair game never has them) keep exactly the money they had: crediting first and then
// writing the giver's figure, taken before, would lose the credit. Nothing ever prints money (review round 2).
template <class Balances>
void settle_transfer(Balances& b, uint32_t giver, uint32_t giver_balance, uint32_t receiver, uint32_t credit)
{
	b.set(giver, giver_balance);
	b.add(receiver, credit);
}

// ---------------------------------------------------------------- upgrades at a mechanic (F2 T5)
// A client installs an upgrade on its machine; the stock Lua effect pays for it there
// (inventory_upgrades.effect_functor_a: give_money(-price), price = floor(cost x mech_discount x eco `upgrade`)). That
// spend is not sent as a ReqMoney: the upgrade's request (Msg::ReqItemEvent GE_INSTALL_UPGRADE) carries it with the
// mechanic (UpgradePayment), and the host checks both (judge_upgrade) and charges the player's balance itself. The
// client counts the payment until the host answers that request (UpgradeHolds, client_view `held`); the host answers
// first and sends the balance after the answer.

// The least a mechanic takes: paid >= floor(cost x min_percent). min_percent is a host setting with a few decimals
// (configs\coop_money.ltx upgrade_min_percent); it counts to 6 decimals in whole numbers, so a float's error (2.21f is
// 2.2100000381) never asks one RU more than the stock price of a large cost. A broken setting (NaN, infinite, below 0)
// refuses.
inline bool upgrade_price_ok(uint32_t paid, uint32_t cost, float min_percent)
{
	if (!(min_percent >= 0.f) || !std::isfinite(min_percent))
		return false;
	const double micro = std::min(std::round(double(min_percent) * 1e6), 4e9); // u32 cost x 4e9 fits in u64
	const uint64_t need = uint64_t(cost) * uint64_t(micro) / 1000000u;
	return uint64_t(paid) >= need;
}

// Why the host refuses a client's upgrade.
enum class UpgradeVerdict : uint8_t
{
	Ok,
	Scripted, // no mechanic named (a script installed it) and the host does not take those (scripted_upgrades)
	NoMechanic, // not a live stalker whose talk lock the player holds, or not a mechanic (stalkers_upgrade_info)
	TooFar, // the player's character on the host stands farther than the reach from the mechanic
	Underpaid, // paid less than floor(cost x upgrade_min_percent)
	Insufficient, // the player's balance on the host does not cover what he paid
};
constexpr uint8_t kUpgradeVerdictCount = uint8_t(UpgradeVerdict::Insufficient) + 1;

// What the host knows about one upgrade request.
struct UpgradeFacts
{
	bool mechanic_named = false; // the request names a mechanic (not 0xffff)
	bool scripted_allowed = false; // host setting scripted_upgrades
	bool mechanic_ok = false; // see UpgradeVerdict::NoMechanic
	float distance = 0.f; // player character <-> mechanic on the host
	float reach = 6.f;
	uint32_t paid = 0; // what the client's game charged for it
	uint32_t cost = 0; // `cost` of the upgrade's section
	float min_percent = 0.f;
	uint32_t balance = 0; // the player's balance on the host
};

// The checks in order: a mechanic, within reach, paid enough, has the money. An upgrade without a mechanic (a script's)
// has no price: if the host takes those, it only needs the money it paid.
inline UpgradeVerdict judge_upgrade(const UpgradeFacts& f)
{
	if (!f.mechanic_named)
	{
		if (!f.scripted_allowed)
			return UpgradeVerdict::Scripted;
		return f.paid > f.balance ? UpgradeVerdict::Insufficient : UpgradeVerdict::Ok;
	}
	if (!f.mechanic_ok)
		return UpgradeVerdict::NoMechanic;
	if (!(f.distance <= f.reach)) // NaN is never near
		return UpgradeVerdict::TooFar;
	if (!upgrade_price_ok(f.paid, f.cost, f.min_percent))
		return UpgradeVerdict::Underpaid;
	if (f.paid > f.balance)
		return UpgradeVerdict::Insufficient;
	return UpgradeVerdict::Ok;
}

// Msg::ReqItemEvent ends with this (F2 T5), after the event bytes. Wire: u16 mechanic, u32 paid (6 bytes).
struct UpgradePayment
{
	uint16_t mechanic = 0xFFFF; // host numbering; 0xffff: none (an upgrade a script installed)
	uint32_t paid = 0; // RU the client's game charged for it
};
constexpr size_t kUpgradePaymentSize = 6;

inline void write(Writer& w, const UpgradePayment& m)
{
	w.u16(m.mechanic);
	w.u32(m.paid);
}

// False on overrun.
inline bool read(Reader& r, UpgradePayment& m)
{
	m.mechanic = r.u16();
	m.paid = r.u32();
	return r.ok();
}

// Client: upgrades paid on this machine whose requests the host has not answered yet, by a key of the client's (one per
// request, never 0). Their sum is held out of the view (client_view `held`), so a balance the host sent before it
// charged the upgrade does not show the money from before the payment. The answer releases it (accepted or refused:
// the host sends the balance right after its answer). At most kMax: the oldest goes first (its request timed out).
class UpgradeHolds
{
public:
	static constexpr size_t kMax = 64;

	void hold(uint32_t key, uint32_t amount)
	{
		if (!key || !amount)
			return;
		if (m_items.size() >= kMax)
			m_items.erase(m_items.begin());
		m_items.push_back({key, amount});
	}
	// False if `key` holds nothing (answered already, dropped, never held).
	bool release(uint32_t key)
	{
		for (auto it = m_items.begin(); it != m_items.end(); ++it)
			if (it->first == key)
			{
				m_items.erase(it);
				return true;
			}
		return false;
	}
	uint64_t sum() const
	{
		uint64_t s = 0;
		for (const auto& it : m_items)
			s += it.second;
		return s;
	}
	size_t size() const { return m_items.size(); }
	void clear() { m_items.clear(); }

private:
	std::vector<std::pair<uint32_t, uint32_t>> m_items; // key, RU; oldest first
};
} // namespace ledger
} // namespace coop
