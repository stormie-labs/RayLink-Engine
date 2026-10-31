// Anomaly Coop — death rules (F3, CHECKPOINTS_DEATH.md §3): the host's death settings and their wire form, which items
// a death takes from the character, the wipe test and the per-player tracker that makes sure one death is handled once
// (also across a leave/return and a world reload), and the decisions of the host's death flow around it (F3 Task 8: the
// respawn debt, the answer to an Alive report, the worn-items tail of a client's Dead report, the HUD format check) and
// hardcore's way back (F3 Task 10: "out" kept by the world, the request of a new character, the retirement of the old).
// The host decides everything here; the engine layer only feeds in what it sees and carries out the answer.
// Pure C++17, header-only, no engine dependencies: used by the engine and by the unit-test exe.
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "coop_checkpoint_rules.h" // checkpoint::ini_set / ini_erase: the custom data of a retired character
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

// What happens to the lost items. Drop: the share goes into a bag (a coop_death_stash) at the body, Ruling L-1; loose
// at the body only when no bag can be made. Corpse: the whole backpack goes into such a stash. Lose: the share is gone.
// Keep (build 10, the owner's request of 2026-10-23): nothing is taken, no bag, no PDA mark; the money, the wear and
// the rank go by their own rules. (death::plan_losses, coop_death_items.h)
enum class Drop : uint8_t
{
	Drop = 0,
	Corpse = 1,
	Lose = 2,
	Keep = 3
};

// The console token of a drop mode ("drop", "corpse", "lose", "keep"); "drop" for a byte that is none of them.
inline const char* drop_name(Drop d)
{
	switch (d)
	{
	case Drop::Corpse: return "corpse";
	case Drop::Lose: return "lose";
	case Drop::Keep: return "keep";
	default: return "drop";
	}
}

namespace detail
{
inline uint32_t clamp_u32(uint32_t v, uint32_t lo, uint32_t hi)
{
	return v < lo ? lo : (v > hi ? hi : v);
}
} // namespace detail

// Build 10, coop_death_share: the host's multiplier of the loss shares of drop and lose, in % (100 = the ltx shares as
// they are; scale_shares). Corpse and keep do not use it.
constexpr float kShareDefaultPct = 100.0f;
constexpr float kShareMaxPct = 200.0f;
constexpr uint32_t kShareMaxByte = 200; // Rules::share_pct on the wire

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
	uint8_t share_pct = 100; // build 10, coop_death_share: drop and lose take ltx share x share_pct / 100 (scale_shares)

	bool operator==(const Rules& o) const
	{
		return mode == o.mode && drop == o.drop && bleedout_ms == o.bleedout_ms && revive_health == o.revive_health &&
			delay_ms == o.delay_ms && contusion_s == o.contusion_s && money_pct == o.money_pct &&
			wear_pct == o.wear_pct && rank_pct == o.rank_pct && share_pct == o.share_pct;
	}

	// mode, drop (u8), bleedout_ms (u32), revive_health (f32), delay_ms (u32), contusion_s (u32), money, wear, rank,
	// share (u8). Build 10: drop 3 = keep, the share byte closes the packet.
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
		w.u8(share_pct);
	}

	// A short packet, an unknown mode or drop, or a health that is not a number: false, the rules stay as they were.
	// Out-of-range values are cut to the range: bleedout 5000..600000 ms, revive 0.05..1, delay 0..300000 ms,
	// contusion 0..3600 s, money, wear and rank 0..100 %, the share 0..200 %.
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
		n.share_pct = r.u8();
		if (!r.ok() || m > static_cast<uint8_t>(Mode::Hardcore) || d > static_cast<uint8_t>(Drop::Keep))
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
		n.share_pct = static_cast<uint8_t>(detail::clamp_u32(n.share_pct, 0, kShareMaxByte));
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

// An item the death may take. The caller decides what is a candidate: quest items are never lost (IsQuestItem(),
// i_quest, i_letter, the PDA and the like are filtered out before the call), and so are items that cannot leave the
// inventory. The candidates go in a stable order (by id): for one seed the answer follows the order of the input.
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
// Contract with the caller: `items` holds no quest items (see Candidate) and comes in a stable order, sorted by id;
// the same inputs and seed then give the same answer on every machine, which the tests pin with fixed vectors.
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

// The shares a drop or lose death picks with (build 10, coop_death_share): each ltx share x pct / 100, cut to 0..1.
// `pct` is cut to 0..kShareMaxPct first (NaN and a negative count as 0: nothing is lost); a share that is not a number,
// or negative, counts as 0.
inline Shares scale_shares(const Shares& base, float pct)
{
	double k = 0.0; // NaN and negatives: 0
	if (pct > 0.0f)
		k = pct < kShareMaxPct ? double(pct) : double(kShareMaxPct); // +inf: the top
	Shares out{};
	for (size_t i = 0; i < out.size(); ++i)
	{
		const double s = base[i] > 0.0f ? double(base[i]) * k / 100.0 : 0.0; // NaN: 0
		out[i] = s < 1.0 ? float(s) : 1.0f;
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

// One per player. Makes each death count once: `decided_seq` is the sequence number (a u16 counter of the player's
// current actor) of the last death the host acted on. It survives a leave/return and a reload of the world while the
// player is dead, so the items are never taken twice for one death; on_alive clears it. 0xffff is the guard "no death
// decided": the counter of the actor must never produce it.
struct Tracker
{
	uint16_t decided_seq = 0xffff;
	uint64_t respawn_at = 0;   // host clock of the respawn; 0 = not waiting
	bool respawn_due = false;  // the player left while dead: respawn when they are back
	bool hardcore_out = false; // out of the run until his player starts a new character (F3 Task 10)
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
	if (r.mode == Mode::Hardcore || t.hardcore_out) // F3 Task 10: out already (learn_out) costs nothing again, any mode
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

// The player stood up: nothing is waiting any more, and the next death is a new one. `decided_seq` goes back to the
// "none decided" guard because the client's death counter starts again from 0 with every new actor (a respawn, a
// world change, a rejoin): the next death of the new actor can carry the same number as the last one of the old actor
// and must not be taken for it. (A reload of the world while the player is dead is on_world_replaced, which keeps
// `decided_seq` until they stand up.) F3 review round 1, finding 1.
inline void on_alive(Tracker& t)
{
	t.respawn_at = 0;
	t.respawn_due = false;
	t.decided_seq = 0xffff;
}

// ---------------------------------------------------------------- the host's death flow (F3 Task 8, fix round 1 I-2)

// survival::Life numbers (coop_survival.h), as is_wipe takes them.
constexpr uint8_t kLifeAlive = 0, kLifeWounded = 1, kLifeDead = 2, kLifeSpectating = 3, kLifeRespawning = 4;

// Build 10 (the owner's live test of build 9): how a player's character lies by his life, the same for his avatar on the
// other machines and for his own actor (the overhead and spectator cameras show it). Standing: his own motions play.
enum class BodyPose : uint8_t
{
	Standing,
	Wounded, // a wounded pose (kWoundedPoses)
	Dead // a dead pose (kDeadPoses): dead, out of the run, being respawned
};

inline BodyPose body_pose(uint8_t life)
{
	if (life == kLifeWounded)
		return BodyPose::Wounded;
	if (life == kLifeDead || life == kLifeSpectating || life == kLifeRespawning)
		return BodyPose::Dead;
	return BodyPose::Standing; // alive, or a number no Life has
}

// Live 2026-10-25 (owner): "when somebody dies for good while lying there is no animation of his death". The dead pose is
// a looping wounded idle (stalkers have no death motions, they fall as ragdolls): a dead body breathed like a wounded one.
// It makes its last move and lies still kDeathStillMs after the death (a clock that went back: still at once).
constexpr uint64_t kDeathStillMs = 1000;
inline bool body_still(BodyPose pose, uint64_t died_ms, uint64_t now_ms)
{
	return pose == BodyPose::Dead && (now_ms < died_ms || now_ms - died_ms >= kDeathStillMs);
}

// The own camera does not turn with the mouse (and the body with it): dead, out of the run, being respawned. A wounded
// player looks around. Fire / Aim still switch the watched teammate (they are keys, not the mouse).
inline bool look_locked(uint8_t life)
{
	return life == kLifeDead || life == kLifeSpectating || life == kLifeRespawning;
}

// Live 2026-10-30 (owner): "monsters keep at you after you go down". Nobody hunts a player out of the run. A wounded one
// bleeds out on his own and a teammate may still get him up: a stalker may finish him, a monster leaves him and turns to
// whoever is standing.
inline bool out_of_fight(uint8_t life)
{
	return life == kLifeDead || life == kLifeSpectating || life == kLifeRespawning;
}

inline bool monster_leaves(uint8_t life)
{
	return life == kLifeWounded || out_of_fight(life);
}

// The respawn a character is owed in his world (fix round 1, M-1 and M-3). The engine keeps it with the character (his
// custom data), so a save written while he is dead or evacuated carries it to the world loaded from that save (a level
// change, an autosave restore), and a new game or another save does not have it. Set when the host decides a checkpoint
// death; paid when the owner reports Respawning: the order reached him and he applied it (the wear included), so a
// respawn that crossed a leave or a world change is not given twice. A hardcore death owes nothing. `decided` is what
// on_died answered to this report (None for every report that is not a death).
inline bool debt_after_report(bool debt, uint8_t life, Action decided)
{
	if (decided == Action::LossesAndEvacuate)
		return true;
	if (life == kLifeRespawning)
		return false;
	return debt;
}

// What the host does when the owner of a character reports him alive.
enum class AliveStep : uint8_t
{
	None,
	Respawn, // put him on his checkpoint: he came back (a leave, a world change) and a respawn is still owed
	Spectate // out of a hardcore run: watch again
};

// The owner reports `t`'s character alive. A respawn is owed when the tracker waited for his return (on_returned) or
// the world keeps a debt for him (`debt`, debt_after_report); `ordered`: a respawn order for this report's seq is out
// already (an Alive that crossed it on the wire must not get a second one). on_returned runs before on_alive, which
// forgets the death. Ruling F3-7: a character out of a hardcore run stays out whatever the mode is now and however often
// he rejoins: Spectate, never a respawn. F3 Task 10: his way back is a new character (on_new_character), and the world
// keeps him out across a level change or a load (learn_out).
inline AliveStep on_alive_report(Tracker& t, bool debt, bool ordered)
{
	const bool returned = on_returned(t);
	on_alive(t);
	if (t.hardcore_out)
		return AliveStep::Spectate;
	return (returned || debt) && !ordered ? AliveStep::Respawn : AliveStep::None;
}

// Ruling F3-4: the tail of a client's Dead report, {u8 n, n x u16 item}: the items in his slots and on his belt, host
// numbering. A proxy (an NPC: no outfit, no belt) cannot show them to the host.
constexpr uint8_t kMaxWornIds = 40; // 18 slots at most, a belt of a few artefacts: more is a modified client

// Writes the tail: the first kMaxWornIds of `ids` (the caller gives each id once).
inline void write_worn(Writer& w, const std::vector<uint16_t>& ids)
{
	const size_t n = ids.size() < kMaxWornIds ? ids.size() : size_t(kMaxWornIds);
	w.u8(static_cast<uint8_t>(n));
	for (size_t i = 0; i < n; ++i)
		w.u16(ids[i]);
}

enum class WornTail : uint8_t
{
	Absent, // no bytes left: the report has no list, the layout stays unknown
	Bad,    // n over kMaxWornIds or fewer bytes than n ids: the whole list is dropped
	Ok
};

// Reads the tail. Ok: `out` holds the ids sorted, each once (whether the proxy still carries them is the engine's
// check). Absent / Bad: `out` is empty. Bytes after the list are left unread.
inline WornTail read_worn(Reader& r, std::vector<uint16_t>& out)
{
	out.clear();
	if (r.remaining() == 0)
		return WornTail::Absent;
	const uint8_t n = r.u8();
	if (!r.ok() || n > kMaxWornIds || r.remaining() < size_t(n) * 2)
		return WornTail::Bad;
	out.reserve(n);
	for (uint8_t i = 0; i < n; ++i)
		out.push_back(r.u16());
	if (!r.ok())
	{
		out.clear();
		return WornTail::Bad;
	}
	std::sort(out.begin(), out.end());
	out.erase(std::unique(out.begin(), out.end()), out.end());
	return WornTail::Ok;
}

// ---------------------------------------------------------------- hardcore: out of the run, a new character (F3 Task 10)

// T8 review: "out of a hardcore run" is kept with the character in the world ([coop_death] out = 1 in his custom data),
// as the respawn debt is. The host's trackers start over with every world it serves (a level change, a load), so the
// tracker alone forgot it and an out player came back alive after the next level change. The character carries it into
// every world saved after his death; a new game or a save from before it has none. The engine reads it before each
// report of the owner is decided and tells the tracker here; what the world keeps after the report is
// `t.hardcore_out`. Only a new character (the retirement below) takes it away, never the world.
inline void learn_out(Tracker& t, bool out_in_world)
{
	if (out_in_world)
		t.hardcore_out = true;
}

// The owner of a character asks for a new one (Msg::NewCharacter; the host's own hold of USE or console the same).
enum class NewCharVerdict : uint8_t
{
	Start, // out of a hardcore run: all he has goes into a stash, the rest follows when it is done (retire_step)
	NotOut, // he still plays (or waits for a checkpoint respawn): refused, a modified client or a late repeat
	Busy // his retirement is under way already: the repeat changes nothing
};

inline NewCharVerdict on_new_character(const Tracker& t, bool retiring)
{
	if (retiring)
		return NewCharVerdict::Busy;
	return t.hardcore_out ? NewCharVerdict::Start : NewCharVerdict::NotOut;
}

// F3a T7 R1: the fall a new-life order names: none (the tracker's "no death decided", never an owner's counter), so a
// standing or dead owner never takes it (take_respawn) - a repeat that comes after he stood up moves nobody.
constexpr uint16_t kNewLifeSeq = 0xffff;

// F3a T7 R1: what an owner takes from a Respawn order (Msg::PlayerRevive OpRespawn, coop_survival apply_respawn).
enum class RespawnTake : uint8_t
{
	Stale, // nothing: an older fall, or a life no respawn is for
	Respawn, // back at his checkpoint (a Dead owner after the evacuation, an Alive one owed a respawn)
	NewLife // his new character after a hardcore run (the host player's; a client's comes by a new join)
};

// Dead or Alive: a respawn when the order names his fall (`my_seq`, never kNewLifeSeq). Spectating: the only Respawn a
// spectator is ever sent is his new life (Ruling F3-7: out of the run nobody is respawned), so the fall it names is not
// compared - the world's records start over with a world and may not know his. Wounded or Respawning: stale.
inline RespawnTake take_respawn(uint8_t life, uint16_t my_seq, uint16_t order_seq)
{
	if (life == kLifeSpectating)
		return RespawnTake::NewLife;
	if ((life == kLifeDead || life == kLifeAlive) && order_seq == my_seq && order_seq != kNewLifeSeq)
		return RespawnTake::Respawn;
	return RespawnTake::Stale;
}

// F3a T7 R1: the world's last order of the host player's new life (kept with his record of this world: a new world
// starts it over).
struct NewLife
{
	bool sent = false;
	uint64_t at = 0; // host clock of the last order
};

// The world gave the host player his new life (not out of the run, no retirement under way) but he reports `life`
// Spectating: the order did not reach him (refused, or dropped with a world). It goes again: at once when this record
// of him has none, else at most every `retry_ms` (a report that crossed the order on its way must not double it).
inline bool new_life_lost(uint8_t life, bool out, bool retiring, const NewLife& last, uint64_t now_ms, uint32_t retry_ms)
{
	if (life != kLifeSpectating || out || retiring)
		return false;
	return !last.sent || now_ms >= last.at + retry_ms;
}

// Review R2 of T9: a retiring character keeps his key and his player until his stash has his things. A pending stash
// knows its owner by the key when both sides have one, else by the player who plays him (same_owner,
// coop_death_items.h). Taking the key off, or sending his player away to make the new character (host_rejoin_peer: the
// old character then has no player, and create_proxy gives the key to the new one), while the stash still waits would
// leave it without an owner: it gives up, the things stay with the old character, and his release destroys them.
enum class RetireStep : uint8_t
{
	Wait, // his stash is pending: nothing of him changes yet
	Finish, // the stash is done (or none was needed): the key goes, his player makes a new character, he leaves the world
	Void, // the character is gone (another world, released already): nothing left to do
	Cancel // fix round 1, I1: his things are still on him and he would be released with them: not now, he stays out
};

// What became of the stash of a retiring character (death::host_stash_outcome).
enum class StashOutcome : uint8_t
{
	Pending, // it still waits for his things
	Done, // filled, dropped at the body instead, or none was needed (nothing to move)
	GaveUp // T9 StashStep::GiveUp: his player stayed away while it was online, or he is someone else now: his things
	       // are still on him
};

// `released`: the character leaves the world when he retires (a client's); the host's actor stays and keeps whatever
// the stash could not take. `items_left`: world items still on him (the server's word, after the stash). Fix round 1,
// I1: a client's character whose stash gave up, or who still carries world items, is not retired - the key, the player
// and the character stay, he is still out of the run and may ask again (better than his things going with him).
inline RetireStep retire_step(bool character_here, StashOutcome stash, bool items_left, bool released)
{
	if (!character_here)
		return RetireStep::Void;
	if (stash == StashOutcome::Pending)
		return RetireStep::Wait;
	if (released && (stash == StashOutcome::GaveUp || items_left))
		return RetireStep::Cancel;
	return RetireStep::Finish;
}

// Fix round 1, C1: the custom data of a character retired by retire_custom_data. His release is confirmed by his entity
// being gone; the id may meanwhile name another character (his player's new one), so only an entity that still says
// this is the old one.
inline bool is_retired(const std::string& text)
{
	std::string v;
	return checkpoint::ini_get(text, "coop", "retired", v) && v == "1";
}

// Ruling F3-10: a retirement is no farm. The new character of a retired key starts with no kit and no faction money
// unless the host turns coop_hardcore_new_kit on; every other new key gets the kit as before.
inline bool new_character_bare(bool retired_key, bool new_kit_option)
{
	return retired_key && !new_kit_option;
}

// The custom data of a retired character: every `key` line of [coop] goes, so find_key_value finds no player for him and
// his player's next join makes a new character (protocol 8), and `retired = 1` says why. The rest stays as it is.
inline void retire_custom_data(std::string& text)
{
	checkpoint::ini_erase(text, "coop", "key");
	checkpoint::ini_set(text, "coop", "retired", "1");
}

// Ruling F3-9: coop_max_new_characters (Ruling F2-10 b, limits::NewCharacters) stops new player keys, with which the
// joins could be spammed for kits. A key that had a character - in this session, or one that retired in this world -
// is the same player starting again: it neither checks nor spends the limit (else a hardcore group got stuck after a
// session's worth of deaths; starting again gains nothing over dying). `had_character`: such a key; `limit_admits`:
// limits::NewCharacters::admits for it now.
struct NewKeyCharge
{
	bool admitted = false;
	bool spends = false; // the host adds the key to limits::NewCharacters once his character is made
};

inline NewKeyCharge new_key_charge(bool had_character, bool limit_admits)
{
	NewKeyCharge c;
	c.admitted = had_character || limit_admits;
	c.spends = !had_character && limit_admits;
	return c;
}

// The retired keys of a world are kept in the host actor's custom data, [coop_retired] <digest> = 1 (the retired
// character is released, his key with him), so a player away when his character retired, or one coming in a later
// session, is still known. One line per player key; only a key digest (kKeyDigestDigits lowercase hex, as
// coop::kKeyDigestHexDigits, coop_session.h) is a key here. Fix round 1, I2: the actor's custom data goes into one
// 16 KB NET_Packet with every save (CSE_ALifeObject::STATE_Write), so at most kMaxRetiredKeys are kept, the oldest
// noted goes first (that key only spends a slot of coop_max_new_characters again, Ruling F3-9).
constexpr size_t kKeyDigestDigits = 64;
constexpr unsigned kMaxRetiredKeys = 64; // about 4.4 KB of custom data
constexpr const char* kRetiredSection = "coop_retired";

namespace detail
{
inline bool is_key_digest(const std::string& s)
{
	if (s.size() != kKeyDigestDigits)
		return false;
	for (const char c : s)
		if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
			return false;
	return true;
}
} // namespace detail

// The retired keys in the text, oldest first (the order of the lines), each once.
inline std::vector<std::string> retired_keys(const std::string& text)
{
	namespace ini = checkpoint::detail;
	std::vector<std::string> out;
	bool in = false;
	size_t pos = 0;
	ini::Line l;
	while (ini::next_line(text, pos, l))
	{
		const ini::Parsed p = ini::parse_line(text, l);
		if (p.kind == ini::LineKind::Section)
		{
			in = ini::ieq(text, p.name_b, p.name_e, kRetiredSection);
			continue;
		}
		if (!in || p.kind != ini::LineKind::Pair || text.compare(p.val_b, p.val_e - p.val_b, "1") != 0)
			continue;
		const std::string name = text.substr(p.name_b, p.name_e - p.name_b);
		if (detail::is_key_digest(name) && std::find(out.begin(), out.end(), name) == out.end())
			out.push_back(name);
	}
	return out;
}

// Notes a retired key as the newest; beyond kMaxRetiredKeys the oldest go.
inline void note_retired_key(std::string& text, const std::string& digest)
{
	if (!detail::is_key_digest(digest))
		return;
	checkpoint::ini_erase(text, kRetiredSection, digest.c_str()); // noted again: it becomes the newest
	checkpoint::ini_set(text, kRetiredSection, digest.c_str(), "1");
	const std::vector<std::string> keys = retired_keys(text);
	for (size_t i = 0; i + kMaxRetiredKeys < keys.size(); ++i)
		checkpoint::ini_erase(text, kRetiredSection, keys[i].c_str());
}

inline bool retired_key(const std::string& text, const std::string& digest)
{
	std::string v;
	return detail::is_key_digest(digest) && checkpoint::ini_get(text, kRetiredSection, digest.c_str(), v) && v == "1";
}

// HUD: a translated format string the engine fills with one value is used only if it has exactly one '%' and that one
// is followed by `conv` ('d', 's'); a broken translation must not reach sprintf with the wrong arguments.
inline bool one_conversion(const char* fmt, char conv)
{
	if (!fmt)
		return false;
	unsigned percents = 0;
	bool ok = false;
	for (const char* p = fmt; *p; ++p)
		if (*p == '%')
		{
			++percents;
			ok = p[1] == conv;
		}
	return percents == 1 && ok;
}
} // namespace death
} // namespace coop
