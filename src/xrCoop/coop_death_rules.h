// Anomaly Coop — death rules (F3, CHECKPOINTS_DEATH.md §3): the host's death settings and their wire form, which items
// a death takes from the character, the wipe test and the per-player tracker that makes sure one death is handled once
// (also across a leave/return and a world reload). The host decides everything here; the engine layer only feeds in
// what it sees and carries out the answer.
// Pure C++17, header-only, no engine dependencies: used by the engine and by the unit-test exe.
#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include "coop_wire.h"

namespace coop
{
namespace death
{
// checkpoint: a death costs items and money, the player comes back at their camp. hardcore: a death ends the run, the
// player watches the others.
enum class Mode : uint8_t
{
	Checkpoint = 0,
	Hardcore = 1
};

// What happens to the lost items. Drop: they fall where the character died. Corpse: they go into a stash container.
// Lose: they are gone.
enum class Drop : uint8_t
{
	Drop = 0,
	Corpse = 1,
	Lose = 2
};

namespace detail
{
inline uint32_t clamp_u32(uint32_t v, uint32_t lo, uint32_t hi)
{
	return v < lo ? lo : (v > hi ? hi : v);
}
} // namespace detail

// The host's death settings; one copy, sent to every client (Msg::DeathRules). The defaults are the plan's numbers
// (Ruling 22): T = 30 s, contusion 10 min, 10 % of the money, 10 % wear, no rank or reputation loss.
struct Rules
{
	Mode mode = Mode::Checkpoint;
	Drop drop = Drop::Drop;
	uint32_t bleedout_ms = 60000;  // how long a wounded player can be revived
	float revive_health = 0.3f;    // health a revived player gets
	uint32_t delay_ms = 30000;     // T: how long a dead player waits for the others before the respawn
	uint32_t contusion_s = 600;    // the debuff after a respawn
	uint8_t money_pct = 10, wear_pct = 10, rank_pct = 0;

	bool operator==(const Rules& o) const
	{
		return mode == o.mode && drop == o.drop && bleedout_ms == o.bleedout_ms && revive_health == o.revive_health &&
			delay_ms == o.delay_ms && contusion_s == o.contusion_s && money_pct == o.money_pct &&
			wear_pct == o.wear_pct && rank_pct == o.rank_pct;
	}

	// mode, drop (u8), bleedout_ms (u32), revive_health (f32), delay_ms (u32), contusion_s (u32), money, wear, rank (u8).
	void write(Writer& w) const
	{
		w.u8(static_cast<uint8_t>(mode));
		w.u8(static_cast<uint8_t>(drop));
		w.u32(bleedout_ms);
		w.f32(revive_health);
		w.u32(delay_ms);
		w.u32(contusion_s);
		w.u8(money_pct);
		w.u8(wear_pct);
		w.u8(rank_pct);
	}

	// A short packet, an unknown mode or drop, or a health that is not a number: false, the rules stay as they were.
	// Out-of-range values are cut to the range: bleedout 5000..600000 ms, revive 0.05..1, delay 0..300000 ms,
	// contusion 0..3600 s, every percentage 0..100.
	bool read(Reader& r)
	{
		const uint8_t m = r.u8();
		const uint8_t d = r.u8();
		Rules n;
		n.bleedout_ms = r.u32();
		n.revive_health = r.f32();
		n.delay_ms = r.u32();
		n.contusion_s = r.u32();
		n.money_pct = r.u8();
		n.wear_pct = r.u8();
		n.rank_pct = r.u8();
		if (!r.ok() || m > static_cast<uint8_t>(Mode::Hardcore) || d > static_cast<uint8_t>(Drop::Lose))
			return false;
		if (!std::isfinite(n.revive_health))
			return false;
		n.mode = static_cast<Mode>(m);
		n.drop = static_cast<Drop>(d);
		n.bleedout_ms = detail::clamp_u32(n.bleedout_ms, 5000, 600000);
		n.revive_health = std::fmin(std::fmax(n.revive_health, 0.05f), 1.0f);
		n.delay_ms = detail::clamp_u32(n.delay_ms, 0, 300000);
		n.contusion_s = detail::clamp_u32(n.contusion_s, 0, 3600);
		n.money_pct = static_cast<uint8_t>(detail::clamp_u32(n.money_pct, 0, 100));
		n.wear_pct = static_cast<uint8_t>(detail::clamp_u32(n.wear_pct, 0, 100));
		n.rank_pct = static_cast<uint8_t>(detail::clamp_u32(n.rank_pct, 0, 100));
		*this = n;
		return true;
	}
};

// Item categories of the loss shares (mod_system_coop_death.ltx, one share per category).
enum Category : uint8_t
{
	Materials,
	Ammo,
	Consumables,
	Gear,
	Artefacts,
	Tools,
	Other,
	kCategories
};

// "materials", "ammo", "consumables", "gear", "artefacts", "tools", "other" (lower case, as in the ltx). False for
// anything else; `out` is left as it was.
inline bool category_from_name(const char* name, uint8_t& out)
{
	static const char* const kNames[kCategories] = {
		"materials", "ammo", "consumables", "gear", "artefacts", "tools", "other"};
	if (!name)
		return false;
	for (uint8_t i = 0; i < kCategories; ++i)
		if (std::strcmp(name, kNames[i]) == 0)
		{
			out = i;
			return true;
		}
	return false;
}

struct Candidate
{
	uint16_t id;
	uint8_t category;
};
using Shares = std::array<float, kCategories>;

namespace detail
{
// xorshift32 (13, 17, 5): the same numbers on every platform, unlike std::uniform_*. The seed is scrambled first so
// that neighbouring seeds (1, 2, 3, ...) do not start with neighbouring numbers.
struct Rng
{
	uint32_t s;

	explicit Rng(uint32_t seed)
	{
		uint32_t h = seed + 0x9e3779b9u;
		h ^= h >> 16;
		h *= 0x85ebca6bu;
		h ^= h >> 13;
		h *= 0xc2b2ae35u;
		h ^= h >> 16;
		s = h ? h : 0x9e3779b9u;
		next();
		next();
	}

	uint32_t next()
	{
		s ^= s << 13;
		s ^= s >> 17;
		s ^= s << 5;
		return s;
	}

	// [0, 1)
	double unit() { return double(next() >> 8) * (1.0 / 16777216.0); }

	// [0, n), n > 0
	uint32_t below(uint32_t n) { return uint32_t((uint64_t(next()) * n) >> 32); }
};
} // namespace detail

// Which items a death takes. Per category with n items and share s: floor(n*s) items and one more with the
// probability frac(n*s); which ones is a seeded shuffle (xorshift32), so the same seed gives the same answer. A share
// is cut to 0..1 (NaN counts as 0). Items of an unknown category are not touched; an id that is offered twice is
// counted once. The result holds ids from `items` only, none twice.
inline std::vector<uint16_t> pick_losses(const std::vector<Candidate>& items, const Shares& shares, uint32_t seed)
{
	std::vector<uint16_t> bucket[kCategories];
	std::vector<bool> seen(65536, false);
	for (const Candidate& c : items)
	{
		if (c.category >= kCategories || seen[c.id])
			continue;
		seen[c.id] = true;
		bucket[c.category].push_back(c.id);
	}

	detail::Rng rng(seed);
	std::vector<uint16_t> out;
	for (uint8_t cat = 0; cat < kCategories; ++cat)
	{
		std::vector<uint16_t>& b = bucket[cat];
		if (b.empty())
			continue;
		float share = shares[cat];
		if (!(share > 0.0f))
			share = 0.0f;
		else if (share > 1.0f)
			share = 1.0f;

		// n * s in double; a product a hair off a whole number (10 * 0.1f) is that whole number.
		double x = double(b.size()) * double(share);
		const double whole = std::round(x);
		if (std::fabs(x - whole) < 1e-4)
			x = whole;
		size_t take = size_t(std::floor(x));
		const double frac = x - std::floor(x);
		if (take < b.size() && frac > 0.0 && rng.unit() < frac)
			++take;
		if (take > b.size())
			take = b.size();

		// A partial Fisher-Yates shuffle: the first `take` places are a uniform pick.
		for (size_t i = 0; i < take; ++i)
		{
			const size_t j = i + rng.below(uint32_t(b.size() - i));
			const uint16_t tmp = b[i];
			b[i] = b[j];
			b[j] = tmp;
			out.push_back(b[i]);
		}
	}
	return out;
}

// Lives as survival::Life numbers (coop_survival.h): 0 Alive, 1 Wounded, 2 Dead, 3 Spectating, 4 Respawning.
// A wipe is when there is somebody present and every one of them is dead, spectating or being respawned.
inline bool is_wipe(const std::vector<uint8_t>& lives_of_present_players)
{
	if (lives_of_present_players.empty())
		return false;
	for (const uint8_t life : lives_of_present_players)
		if (life != 2 && life != 3 && life != 4)
			return false;
	return true;
}

// One per player. Makes each death count once: `decided_seq` is the sequence number of the last death the host acted
// on. It survives a leave/return and a reload of the world, so the items are never taken twice for one death.
struct Tracker
{
	uint16_t decided_seq = 0xffff;
	uint64_t respawn_at = 0;   // host clock of the respawn; 0 = not waiting
	bool respawn_due = false;  // the player left while dead: respawn when they are back
	bool hardcore_out = false; // out of the run for good
};

enum class Action : uint8_t
{
	None,
	LossesAndEvacuate, // take the items, start the T countdown
	Spectate           // hardcore: the player is out
};

// A player's death (sequence number `seq`) was seen. Only the first report of each death does anything.
inline Action on_died(Tracker& t, uint16_t seq, uint64_t now_ms, const Rules& r)
{
	if (seq == t.decided_seq)
		return Action::None;
	t.decided_seq = seq;
	t.respawn_due = false;
	if (r.mode == Mode::Hardcore)
	{
		t.hardcore_out = true;
		t.respawn_at = 0;
		return Action::Spectate;
	}
	t.respawn_at = now_ms + r.delay_ms;
	if (t.respawn_at == 0) // 0 means "not waiting"
		t.respawn_at = 1;
	return Action::LossesAndEvacuate;
}

// The evacuation time is over: true once.
inline bool due(Tracker& t, uint64_t now_ms)
{
	if (t.respawn_at == 0 || now_ms < t.respawn_at)
		return false;
	t.respawn_at = 0;
	return true;
}

// The player left. If they were dead or being evacuated, the respawn waits for their return (the countdown stops).
// A living player is not touched.
inline void on_left(Tracker& t)
{
	if (t.hardcore_out)
		return;
	if (t.respawn_at == 0 && !t.respawn_due)
		return;
	t.respawn_due = true;
	t.respawn_at = 0;
}

// The player is back. True once if a respawn is waiting for them.
inline bool on_returned(Tracker& t)
{
	if (t.hardcore_out || !t.respawn_due)
		return false;
	t.respawn_due = false;
	return true;
}

// The host's world was replaced (another map, a loaded save). True if the player was dead or being evacuated: the
// caller puts them alive on their camp. The wait is cleared; `decided_seq` stays, so the same death is not charged
// again.
inline bool on_world_replaced(Tracker& t)
{
	if (t.hardcore_out)
		return false;
	const bool pending = t.respawn_at != 0 || t.respawn_due;
	t.respawn_at = 0;
	t.respawn_due = false;
	return pending;
}

// The player stood up: nothing is waiting any more. `decided_seq` stays.
inline void on_alive(Tracker& t)
{
	t.respawn_at = 0;
	t.respawn_due = false;
}
} // namespace death
} // namespace coop
