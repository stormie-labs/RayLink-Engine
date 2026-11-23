// Ядро: без проверок режима (docs/MODES.md)
// Anomaly Coop — a player's own progress (rank, reputation, community) checked where his record is kept (F5-PRG-2, row 9 of F2,
// TZ 6.4; temp/b58/f5_plan.md section 6): the pure half. The engine half is xrGame/coop/coop_progress.cpp (coop_progress.h).
//
// A player's game tells his rank, reputation and community (Msg::Progress, coop_story.cpp progress_frame). The process that keeps
// his record (the co-op host; in the Server mode the world server) checks the report before it is stored, put on his character and
// sent on. The same rules in both modes; they do not know which one runs:
//   - the base: the last values stored for that player's character; at the first report after a join (no stored values yet), the
//     record of the character as the keeper has it (the save: the client's actor took exactly these values at the role swap,
//     coop_join.cpp), so the first report must equal it (± 0) or rise within the budget;
//   - the keeper's own changes of the character count for the player: the record's reputation above the stored one is the
//     reputation the keeper's fights added (coop_story.cpp host_flush_rep_deltas -> host_add_reported_reputation, sent to the
//     player as RelReputationDelta): it is part of the base, and it is kept for kCreditMs as the keeper's reputation in the budget
//     of his rises (Peer::keeper_rep, spent first), whatever order his reports and the put-back come in;
//   - the stored values belong to one world (Book::on_world): after a load or a level change they are forgotten;
//   - a rise of rank or reputation must fit the budget: the points the keeper saw the player earn since the last stored rise (the
//     ledger of coop_progress_check.rank_budget, F5-PRG-1/3/4), plus points pushed by coop.progress.budget, plus what was left of
//     the budget at the last stored rise (kCreditMs, so that a kill the keeper saw before the player's rank shows it is not lost);
//   - a loss always passes (nobody cheats himself down);
//   - a community other than the base and the record needs the keeper's allowance (coop.progress.community_ok: the coop_faction
//     record, a disguise); without anybody to ask it is not checked. actor_X and X are one faction (same_faction).
// What a failed check does is the setting of coop_trust (gamedata/configs/mod_system_coop_trust.ltx, [coop_trust], names
// progress_rank / progress_reputation / progress_community): "log" stores the values as told and writes the line, "refuse" stores
// the clamped ones (base + budget, the base's community), "off" does not check.
//
// Pure C++17, header-only, no engine dependencies: used by the engine and by the unit-test exe (tests/test_progress_rules.cpp).
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>

namespace coop
{
namespace progress
{
// coop_trust names (keys of [coop_trust]; the default "log" covers them). Permanent: config keys and log text.
constexpr const char* kTrustSection = "coop_trust";
constexpr const char* kTrustDefault = "default";
constexpr const char* kTrustRank = "progress_rank";
constexpr const char* kTrustReputation = "progress_reputation";
constexpr const char* kTrustCommunity = "progress_community";

enum class Trust : uint8_t
{
	Log,
	Refuse,
	Off,
};

namespace detail
{
// no strcmp here: the engine marks it deprecated (C4995, xrCore)
inline bool same_text(const char* a, const char* b)
{
	for (;; ++a, ++b)
	{
		if (*a != *b)
			return false;
		if (!*a)
			return true;
	}
}
inline const char* after_prefix(const char* s, const char* prefix)
{
	const char* p = s;
	for (; *prefix; ++p, ++prefix)
		if (*p != *prefix)
			return s;
	return p;
}
} // namespace detail

// coop_trust.script MODES: exactly "log", "refuse" or "off". Anything else: false, `out` untouched.
inline bool trust_of(const char* text, Trust& out)
{
	if (!text)
		return false;
	if (detail::same_text(text, "log"))
		out = Trust::Log;
	else if (detail::same_text(text, "refuse"))
		out = Trust::Refuse;
	else if (detail::same_text(text, "off"))
		out = Trust::Off;
	else
		return false;
	return true;
}

inline const char* trust_word(Trust t)
{
	switch (t)
	{
	case Trust::Log: return "log";
	case Trust::Refuse: return "refuse";
	case Trust::Off: return "off";
	}
	return "?";
}

struct Modes
{
	Trust rank = Trust::Log;
	Trust reputation = Trust::Log;
	Trust community = Trust::Log;
};

constexpr uint16_t kNoCharacter = 0xffff;
// coop_progress_check.constants.max_number: no budget is larger.
constexpr int64_t kMaxPoints = 1000000000;
// What is carried from one stored rise to the next (left budget, coop.progress.budget pushes): an honest burst (achievements,
// a squad killed) fits, hours of idle allowance do not pile up (the ledger itself caps its idle allowance at 30 minutes).
constexpr int64_t kMaxCreditRank = 5000;
constexpr int64_t kMaxCreditReputation = 1000;
constexpr uint64_t kCreditMs = 60000;
// Players are at most 16 (F5-SEAT-1); the rest of the room is for pushes for peers that never report.
constexpr size_t kMaxPeers = 64;
// coop_trust.script LOG_FIRST: the first 8 failures of a name are written, then every 64th.
constexpr uint64_t kLogFirst = 8;
constexpr uint64_t kLogEvery = 64;

inline bool log_due(uint64_t failed) { return failed <= kLogFirst || failed % kLogEvery == 0; }

// A number from Lua: finite and >= 0, cut to whole points, at most kMaxPoints; anything else is 0.
inline int64_t points_of(double v)
{
	if (!(v == v) || v < 0.0 || v > 1e300)
		return 0;
	if (v >= double(kMaxPoints))
		return kMaxPoints;
	return int64_t(std::floor(v));
}

// "actor_dolg" and "dolg" are one faction: a player's character uses actor_X where the NPC record says X (coop_join.cpp role
// swap). Empty names never match.
inline bool same_faction(const char* a, const char* b)
{
	if (!a || !b)
		return false;
	a = detail::after_prefix(a, "actor_");
	b = detail::after_prefix(b, "actor_");
	return *a && detail::same_text(a, b);
}

struct Values
{
	int32_t rank = 0;
	int32_t reputation = 0;
	int32_t community = -1;
};

inline bool operator==(const Values& a, const Values& b)
{
	return a.rank == b.rank && a.reputation == b.reputation && a.community == b.community;
}
inline bool operator!=(const Values& a, const Values& b) { return !(a == b); }

// Points the keeper lets the player rise by.
struct Budget
{
	int64_t rank = 0;
	int64_t reputation = 0;
};

// The keeper's answer about a community the player told that is neither his base nor his record.
enum class Faction : uint8_t
{
	Same, // nothing changed (or the same faction)
	Allowed, // the coop_faction record / the disguise allows it
	NotAllowed,
	Unknown, // nobody to ask (no coop.progress.community_ok) or no answer: not checked
};

// Bits of the checks (Verdict, Outcome).
enum : uint8_t
{
	kRank = 1,
	kReputation = 2,
	kCommunity = 4,
};

struct Verdict
{
	Values clamped; // what "refuse" stores: the report with each failed field cut back
	uint8_t checked = 0; // checks that ran
	uint8_t failed = 0;
};

namespace detail
{
inline int32_t to_i32(int64_t v)
{
	return v > INT32_MAX ? INT32_MAX : (v < INT32_MIN ? INT32_MIN : int32_t(v));
}
inline int64_t add_capped(int64_t a, int64_t b, int64_t cap)
{
	const int64_t s = a + b; // both are within [0, kMaxPoints]: no overflow
	return s > cap ? cap : s;
}
} // namespace detail

// The decision on one report. `previous`: the base (see the header). `budget_known` false: there is no budget source (the ledger
// is absent or failed), rank and reputation are not checked. `community`: the keeper's answer when the community differs.
inline Verdict decide(const Values& previous, const Values& reported, const Budget& budget, bool budget_known, Faction community)
{
	Verdict v;
	v.clamped = reported;
	if (budget_known)
	{
		v.checked |= kRank | kReputation;
		if (int64_t(reported.rank) - int64_t(previous.rank) > budget.rank)
		{
			v.failed |= kRank;
			v.clamped.rank = detail::to_i32(int64_t(previous.rank) + budget.rank);
		}
		if (int64_t(reported.reputation) - int64_t(previous.reputation) > budget.reputation)
		{
			v.failed |= kReputation;
			v.clamped.reputation = detail::to_i32(int64_t(previous.reputation) + budget.reputation);
		}
	}
	if (community == Faction::Allowed || community == Faction::NotAllowed)
	{
		v.checked |= kCommunity;
		if (community == Faction::NotAllowed)
		{
			v.failed |= kCommunity;
			v.clamped.community = previous.community;
		}
	}
	return v;
}

// What the engine gathered for one report.
struct Report
{
	Values told; // as the player (or the put-back of the stored record) told them
	bool told_community_valid = true; // false: an index outside the community table (nobody applies it)
	bool record_known = false;
	Values record; // the character as the keeper has him now (Level object, else the alife object)
};

struct Outcome
{
	Values out; // what to store, put on the character and send on
	Values base;
	Budget budget; // what the rises could use (only when budget_known)
	uint8_t checked = 0;
	uint8_t failed = 0;
	uint8_t refused = 0; // failed checks whose "refuse" cut the value back
	bool repeat = false; // the stored values again (the 30 s refresh, the put-back): nothing checked
	bool baseline = false; // the first report of this character: measured from its record
	bool no_record = false; // neither stored values nor a record: taken as told
	bool budget_known = false;
	bool commit = false; // a rise was stored: the caller closes the ledger's window (coop_progress_check.rank_budget(c, true))
};

struct Peer
{
	uint16_t character = kNoCharacter; // kNoCharacter: only pushed points so far
	bool have = false; // `accepted` holds the last stored values
	Values accepted;
	Budget credit;
	uint64_t credit_at = 0; // ms of the last change of `credit`
	// Reputation the keeper added to his character (the record above the stored value: host_add_reported_reputation), whatever
	// order his reports and the put-back come in: his rises spend it first; kept kCreditMs from its last change (keeper_at).
	int64_t keeper_rep = 0;
	uint64_t keeper_at = 0;
};

// The keeper's book of the players' last stored values, per peer (the sender as the transport names him).
class Book
{
public:
	// coop.progress.budget: points the keeper saw `peer` earn that the ledger does not hold. Negative points: false.
	bool add_credit(uint32_t peer, int64_t rank, int64_t reputation, uint64_t now)
	{
		if (rank < 0 || reputation < 0)
			return false;
		auto it = m_peers.find(peer);
		if (it == m_peers.end())
		{
			if (m_peers.size() >= kMaxPeers)
				return false;
			it = m_peers.emplace(peer, Peer()).first;
		}
		Peer& p = it->second;
		const Budget c = credit_now(p, now);
		p.credit.rank = detail::add_capped(c.rank, rank > kMaxPoints ? kMaxPoints : rank, kMaxCreditRank);
		p.credit.reputation =
		    detail::add_capped(c.reputation, reputation > kMaxPoints ? kMaxPoints : reputation, kMaxCreditReputation);
		p.credit_at = now;
		return true;
	}

	// One report of `peer` for his `character`. window(Budget&) -> bool: the ledger's points since its last commit (asked only
	// when something rose; false: no budget source). faction(base, told) -> Faction: asked only when the told community is
	// neither the base's nor the record's.
	template <class WindowFn, class FactionFn>
	Outcome check(uint32_t peer, uint16_t character, const Report& r, const Modes& m, uint64_t now, WindowFn&& window,
	    FactionFn&& faction)
	{
		Outcome o;
		o.out = r.told;
		Peer& p = entry(peer);
		if (p.character != character)
		{
			if (p.character != kNoCharacter)
				p = Peer(); // another character on this slot: a join of its own
			p.character = character;
		}
		if (!r.told_community_valid)
			o.out.community = p.have ? p.accepted.community : (r.record_known ? r.record.community : r.told.community);
		// the keeper's reputation is noted before the repeat test: a refresh he sent before the keeper's change reached him repeats
		// the stored values, and coop_story then stores and applies them (the record loses the change) - rv_F5-PRG-2 A1
		if (now > p.keeper_at + kCreditMs)
			p.keeper_rep = 0;
		if (p.have && r.record_known && r.record.reputation > p.accepted.reputation)
		{
			p.keeper_rep =
			    detail::add_capped(p.keeper_rep, int64_t(r.record.reputation) - p.accepted.reputation, kMaxCreditReputation);
			p.keeper_at = now;
		}
		if (p.have && o.out == p.accepted)
		{
			o.repeat = true;
			return o;
		}
		Budget credit = credit_now(p, now);
		if (p.have)
		{
			o.base = p.accepted;
			if (r.record_known)
			{
				if (r.record.rank > o.base.rank)
					o.base.rank = r.record.rank; // the keeper's own change: never against the player
				// reputation the keeper added (a fight on its level): part of the base; for his reports that do not show it yet (or
				// come after a put-back or a stale report that dropped it from the record) it is p.keeper_rep in the budget
				if (r.record.reputation > o.base.reputation)
					o.base.reputation = r.record.reputation;
			}
		}
		else if (r.record_known)
		{
			o.base = r.record;
			o.baseline = true;
		}
		else
		{
			o.no_record = true; // nothing to measure from
			p.have = true;
			p.accepted = o.out;
			return o;
		}

		const bool rank_on = m.rank != Trust::Off, rep_on = m.reputation != Trust::Off;
		const bool rises = (rank_on && o.out.rank > o.base.rank) || (rep_on && o.out.reputation > o.base.reputation);
		Budget win;
		o.budget_known = rises ? bool(window(win)) : true;
		if (o.budget_known)
		{
			o.budget.rank = detail::add_capped(rises ? clamp_points(win.rank) : 0, credit.rank, kMaxPoints);
			o.budget.reputation = detail::add_capped(rises ? clamp_points(win.reputation) : 0, credit.reputation, kMaxPoints);
			o.budget.reputation = detail::add_capped(o.budget.reputation, p.keeper_rep, kMaxPoints);
		}

		Faction f = Faction::Same;
		if (m.community != Trust::Off && o.out.community != o.base.community &&
		    !(r.record_known && o.out.community == r.record.community))
			f = faction(o.base.community, o.out.community);
		const Verdict v = decide(o.base, o.out, o.budget, o.budget_known, f);

		apply(v, kRank, m.rank, o, o.out.rank, v.clamped.rank);
		apply(v, kReputation, m.reputation, o, o.out.reputation, v.clamped.reputation);
		apply(v, kCommunity, m.community, o, o.out.community, v.clamped.community);

		const int64_t rank_used = (o.checked & kRank) && o.out.rank > o.base.rank ? int64_t(o.out.rank) - o.base.rank : 0;
		const int64_t rep_used =
		    (o.checked & kReputation) && o.out.reputation > o.base.reputation ? int64_t(o.out.reputation) - o.base.reputation : 0;
		if (o.budget_known && rises && (rank_used > 0 || rep_used > 0))
		{
			o.commit = true;
			// what the rise did not use is carried (a log-mode failure used all of it and more)
			if (o.checked & kRank)
				p.credit.rank = carry(o.budget.rank - rank_used, kMaxCreditRank);
			if (o.checked & kReputation)
			{
				// the keeper's reputation is spent first; what the rest of the budget did not use is carried (not the keeper's
				// remainder: it stays in keeper_rep and is not counted twice)
				const int64_t keeper_used = rep_used < p.keeper_rep ? rep_used : p.keeper_rep;
				p.keeper_rep -= keeper_used;
				p.credit.reputation = carry(o.budget.reputation - rep_used - p.keeper_rep, kMaxCreditReputation);
			}
			p.credit_at = now;
		}
		p.have = true;
		p.accepted = o.out;
		return o;
	}

	// Drops the peers whose slot no longer names their character (left, reused) and pushes nobody reported for in time.
	// proxy_of(peer) -> uint16_t: the character of that peer now (kNoCharacter: none). Returns how many were dropped.
	template <class ProxyOf>
	size_t sweep(ProxyOf&& proxy_of, uint64_t now)
	{
		size_t n = 0;
		for (auto it = m_peers.begin(); it != m_peers.end();)
		{
			Peer& p = it->second;
			const bool expired = now > p.credit_at + kCreditMs;
			if (expired)
				p.credit = Budget();
			if (now > p.keeper_at + kCreditMs)
				p.keeper_rep = 0;
			const bool gone = p.character == kNoCharacter ? expired : uint16_t(proxy_of(it->first)) != p.character;
			if (gone)
			{
				it = m_peers.erase(it);
				++n;
			}
			else
				++it;
		}
		return n;
	}

	// F5-REV-1 S3: reputation the keeper gave `peer`'s `character` that its record does not show - a map server's verified fight on
	// the world server (coop.progress.apply_verified_fight: host_apply_fight puts it into his stored report, the alife record never
	// sees it). Kept as the keeper's reputation of the budget (keeper_rep, kCreditMs), as the record's rise is for the co-op host's
	// own fights. Only for the character his entry names, a rise only; nothing for a peer the book does not know.
	void credit_keeper(uint32_t peer, uint16_t character, int64_t delta, uint64_t now)
	{
		auto it = m_peers.find(peer);
		if (delta <= 0 || it == m_peers.end() || it->second.character != character)
			return;
		Peer& p = it->second;
		if (now > p.keeper_at + kCreditMs)
			p.keeper_rep = 0;
		p.keeper_rep = detail::add_capped(p.keeper_rep, delta > kMaxPoints ? kMaxPoints : delta, kMaxCreditReputation);
		p.keeper_at = now;
	}

	const Peer* find(uint32_t peer) const
	{
		const auto it = m_peers.find(peer);
		return it == m_peers.end() ? nullptr : &it->second;
	}
	size_t size() const { return m_peers.size(); }
	bool empty() const { return m_peers.empty(); }
	void clear() { m_peers.clear(); }

	// The world the stored values belong to (the engine passes join::host_world_epoch(): a load or a level change replaces the
	// world). Another world forgets every peer: what was stored in the old one is no base in the new one, where his first report
	// is measured from the new record (rv_F5-PRG-2 B). True when it forgot.
	bool on_world(uint32_t world)
	{
		const bool other = m_world_known && world != m_world;
		if (other)
			m_peers.clear();
		m_world = world;
		m_world_known = true;
		return other;
	}

private:
	static int64_t clamp_points(int64_t v) { return v < 0 ? 0 : (v > kMaxPoints ? kMaxPoints : v); }
	static int64_t carry(int64_t left, int64_t cap) { return left <= 0 ? 0 : (left > cap ? cap : left); }

	static Budget credit_now(Peer& p, uint64_t now)
	{
		if (now > p.credit_at + kCreditMs)
			p.credit = Budget();
		return p.credit;
	}

	static void apply(const Verdict& v, uint8_t bit, Trust mode, Outcome& o, int32_t& out, int32_t clamped)
	{
		if (mode == Trust::Off || !(v.checked & bit))
			return;
		o.checked |= bit;
		if (!(v.failed & bit))
			return;
		o.failed |= bit;
		if (mode == Trust::Refuse)
		{
			o.refused |= bit;
			out = clamped;
		}
	}

	// A report always finds room: an entry that holds only pushed points gives way (the oldest first).
	Peer& entry(uint32_t peer)
	{
		auto it = m_peers.find(peer);
		if (it != m_peers.end())
			return it->second;
		if (m_peers.size() >= kMaxPeers)
		{
			auto victim = m_peers.end();
			for (auto i = m_peers.begin(); i != m_peers.end(); ++i)
				if (i->second.character == kNoCharacter && (victim == m_peers.end() || i->second.credit_at < victim->second.credit_at))
					victim = i;
			if (victim == m_peers.end())
				victim = m_peers.begin(); // every entry is a reporting player: cannot happen below kMaxPeers players
			m_peers.erase(victim);
		}
		return m_peers.emplace(peer, Peer()).first->second;
	}

	std::map<uint32_t, Peer> m_peers;
	uint32_t m_world = 0;
	bool m_world_known = false;
};
} // namespace progress
} // namespace coop
