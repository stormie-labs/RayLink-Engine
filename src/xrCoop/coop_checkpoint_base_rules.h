// The checkpoint of a base (docs/CHECKPOINTS_DEATH.md §2.4). On a base the respawn point is its bed, not a campfire: a
// campfire on a base sets no checkpoint, and walking about the base does not repeat "checkpoint updated". A base without a
// sleep zone takes the place of the first spawn on it; on a hostile base the campfires work as usual. The same holds in the
// server mode.
//
// A base is either
//   - a row of [coop_bases] (configs\mod_system_coop_checkpoints.ltx): a stock sleep zone that stands in a real base, the
//     radius of its ground and the communities that own it (up to kMaxOwners). Its record is kind Rest with the zone as its
//     object. Key: the zone's object id (0..0xfffe);
//   - a row of [coop_start_bases]: a start location
//     of new_game_start_locations.ltx ("the place of the first spawn on that base") with the owners of its row, when no sleep
//     zone base or earlier start base already covers its ground. Its record is kind Rest WITHOUT an object, at the location's
//     point. Key: kStartKeyBase + its index in the list every process builds the same way (start_bases, merge_starts).
// Who decides: the process that leads the player's character (the host in co-op, the world server in the server mode); the
// map server of a split level only reports the base whose ground a player stands on (Footing, by geometry) and decides
// nothing. Single player and clients run none of it.
// Pure C++17, header-only, no engine dependencies: used by the engine (xrGame/coop/coop_checkpoint_sources.cpp,
// coop_checkpoints.cpp, coop_rest.cpp, coop_levels_xfer.cpp through coop_levels_wire.h) and by the unit-test exe
// (tests/test_checkpoint_base_rules.cpp, tests/test_protocol_f3a.cpp).
#pragma once

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "coop_checkpoint_rules.h"
#include "coop_ids.h"
#include "coop_text.h"
#include "coop_validate.h"

namespace coop
{
namespace checkpoint
{
namespace base
{
constexpr float kDefaultRadius = 50.0f; // a row without a radius (YACS saves on a base within 50 m too)
constexpr float kMaxRadius = 500.0f; // as a source radius (coop_checkpoint_source_rules.h kMaxSourceRadius)
constexpr float kLeaveMargin = 10.0f; // once on a base he stays on it until radius + this (no flicker at the edge)
constexpr float kHeight = 10.0f; // a base is within this height of its point (floors of the scientists' bunker, Skadovsk)
constexpr float kHeightLeaveMargin = 5.0f; // ... and he leaves it beyond kHeight + this
constexpr uint32_t kDefaultDwellMs = 3000; // on a base this long, not in a fight, and its record is his checkpoint
constexpr size_t kMaxBases = 64; // rows of [coop_bases] a process keeps (stock: 15)
constexpr size_t kMaxStarts = 64; // start bases a process keeps (rows of [coop_start_bases]: 20)
constexpr size_t kMaxOwners = 6; // owners of one row ([coop_bases], [coop_start_bases])
constexpr size_t kMaxStartOwners = 16; // owners of one start base after merge_starts gave it those of the merged ones
constexpr float kDefaultStartRadius = 50.0f; // base_start_radius
constexpr float kMaxStartCoord = 100000.0f; // a start location's x and z (and y) are within this
constexpr int kEnemies = -1000; // stock game_relations.ENEMIES: a sum at or below it is an enemy
using coop::kNoLevel; // a start location whose game vertex names no level

// ---------------------------------------------------------------- the key of a base

using Key = uint32_t;
constexpr Key kNoBase = 0xffffffffu;
constexpr Key kStartKeyBase = 0x10000u; // a start location's key: this + its index

inline Key zone_key(uint16_t zone) { return zone == kNoSource ? kNoBase : Key(zone); }
inline Key start_key(size_t index) { return index < kMaxStarts ? kStartKeyBase + Key(index) : kNoBase; }
inline bool is_zone_key(Key k) { return k < Key(kNoSource); }
inline bool is_start_key(Key k) { return k >= kStartKeyBase && k < kStartKeyBase + Key(kMaxStarts); }
inline bool is_base_key(Key k) { return is_zone_key(k) || is_start_key(k); }
// The sleep zone of a zone key; kNoSource for any other key.
inline uint16_t zone_of(Key k) { return is_zone_key(k) ? uint16_t(k) : kNoSource; }
// The index of a start key; kMaxStarts for any other key.
inline size_t start_of(Key k) { return is_start_key(k) ? size_t(k - kStartKeyBase) : kMaxStarts; }

// ---------------------------------------------------------------- [coop_bases]

// One row: <sleep zone name> = <radius in m>, <owner community>[, <owner>...] or none. Up
// to kMaxOwners owners, as the game's simulation data names several factions for one camp. owners empty = none (never
// hostile). Owners keep the row's order, and the first one is the holder (base_hostile).
struct Row
{
	std::string zone;
	float radius = kDefaultRadius;
	std::vector<std::string> owners;
};

namespace detail
{
// The comma-separated fields of a value, each trimmed (at least one, maybe empty). At most `max` + 1 are cut: a longer
// list gives max + 1 fields and is refused by the caller.
inline std::vector<std::string> fields_of(const std::string& value, size_t max)
{
	std::vector<std::string> fields;
	size_t pos = 0;
	for (;;)
	{
		const size_t comma = value.find(',', pos);
		const std::string_view field = std::string_view(value).substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
		fields.emplace_back(ascii::trim(field, ascii::space_or_tab));
		if (comma == std::string::npos || fields.size() > max)
			break;
		pos = comma + 1;
	}
	return fields;
}

// The owners of fields[from..]: nothing, one empty field or "none" alone is none; else 1..kMaxOwners community names
// ([A-Za-z0-9_], 1..64), none of them empty or "none", each kept once. False: not an owner list.
inline bool owners_of(const std::vector<std::string>& fields, size_t from, std::vector<std::string>& out)
{
	out.clear();
	if (from >= fields.size())
		return true;
	if (fields.size() - from == 1 && (fields[from].empty() || fields[from] == "none"))
		return true;
	if (fields.size() - from > kMaxOwners)
		return false;
	for (size_t i = from; i < fields.size(); ++i)
	{
		if (fields[i] == "none" || !level_ok(fields[i]))
			return false;
		bool known = false;
		for (const std::string& o : out)
			known = known || o == fields[i];
		if (!known)
			out.push_back(fields[i]);
	}
	return true;
}
} // namespace detail

// The value may leave out the radius (the default) and the owners (none): "50, dolg", "50, stalker, csky", "50", ", killer",
// "". A zone or owner name is 1..64 characters of [A-Za-z0-9_]; the radius is a plain number in (0, kMaxRadius]; then 0..6
// owners or "none" alone. On false `out` is left as it was.
inline bool parse_row(const std::string& zone, const std::string& value, Row& out)
{
	if (!level_ok(zone))
		return false;
	const std::vector<std::string> fields = detail::fields_of(value, 1 + kMaxOwners);
	Row r;
	r.zone = zone;
	if (!fields[0].empty())
	{
		float v = 0;
		if (!checkpoint::detail::parse_float(fields[0], v) || !(v > 0.0f) || v > kMaxRadius)
			return false;
		r.radius = v;
	}
	if (!detail::owners_of(fields, 1, r.owners))
		return false;
	out = r;
	return true;
}

// ---------------------------------------------------------------- who is on a base

// A base as the deciding process sees it for one player now: its key, the object of its record (the sleep zone; kNoSource for
// a start base), the level and the point of its record, the radius of its ground, and whether it is hostile to this player
// (hostile bases are no bases for him).
struct Spot
{
	Key id = kNoBase;
	uint16_t bed = kNoSource;
	std::string level;
	float pos[3] = {0, 0, 0};
	float radius = kDefaultRadius;
	bool hostile = false;
};

// `at` on the ground of `s`: the same level, within the radius horizontally and kHeight in height; `entered` (he is on it
// already) widens both by the leave margins. A position that is not a number is on no base, nor is a spot of no base.
inline bool inside(const Spot& s, const std::string& level, const float at[3], bool entered, float leave_margin = kLeaveMargin)
{
	if (!is_base_key(s.id) || s.level != level || !validate::finite3(at) || !validate::finite3(s.pos))
		return false;
	const double r = double(s.radius) + (entered ? double(leave_margin) : 0.0);
	const double h = double(kHeight) + (entered ? double(kHeightLeaveMargin) : 0.0);
	const double dx = double(at[0]) - double(s.pos[0]);
	const double dz = double(at[2]) - double(s.pos[2]);
	const double dy = double(at[1]) - double(s.pos[1]);
	return dx * dx + dz * dz <= r * r && std::fabs(dy) <= h;
}

// A tent of his stands on this base (the strict ground, no leave margin).
inline bool stands_on(const Spot& s, const std::string& level, const float pos[3]) { return inside(s, level, pos, false); }

// The friendly base `at` is on: the one he is on already (`current`) while he is within its leave range, else the nearest
// friendly one whose ground he stands on. kNoBase: none.
inline Key base_at(const std::vector<Spot>& spots, const std::string& level, const float at[3], Key current,
	float leave_margin = kLeaveMargin)
{
	if (is_base_key(current))
		for (const Spot& s : spots)
			if (s.id == current && !s.hostile && inside(s, level, at, true, leave_margin))
				return current;
	Key best = kNoBase;
	double best_d2 = 0;
	for (const Spot& s : spots)
	{
		if (s.hostile || !inside(s, level, at, false, leave_margin))
			continue;
		const double dx = double(at[0]) - double(s.pos[0]);
		const double dz = double(at[2]) - double(s.pos[2]);
		const double d2 = dx * dx + dz * dz;
		if (best == kNoBase || d2 < best_d2)
		{
			best = s.id;
			best_d2 = d2;
		}
	}
	return best;
}

// ---------------------------------------------------------------- the record of a base

// A record is the record of base `s`: kind Rest; a sleep zone base's has the zone as its object; a start base's has no object
// and lies on its level within kSamePlaceM (3D) of its point (checkpoint::same_place: the same place).
inline bool is_base_record(const Record& r, const Spot& s)
{
	if (!r.valid() || r.kind != Kind::Rest)
		return false;
	if (is_zone_key(s.id))
		return s.bed != kNoSource && r.source == s.bed;
	if (!is_start_key(s.id) || r.source != kNoSource || r.level != s.level || !validate::finite3(s.pos))
		return false;
	double d2 = 0;
	for (int i = 0; i < 3; ++i)
	{
		const double d = double(r.pos[i]) - double(s.pos[i]);
		d2 += d * d;
	}
	return d2 <= double(kSamePlaceM) * double(kSamePlaceM);
}

// What a record is for the text its player reads. The engine of the machine
// that tells its Lua (coop_checkpoint_changed) says it, so a client needs no ALife: Bed, the record of a sleep zone base ("a
// sleeping place"); Base, the record of a start base ("a base"); None, neither ("a resting place" for any other rest record);
// Unknown, this machine cannot tell (the script falls back to its own lookup). `zone_base` (a record with an object): 1 its
// object is a sleep zone of [coop_bases], 0 it is not, -1 not known here; `start_point` (a record without one): it lies
// within kSamePlaceM of a start base's point.
enum class Mark : uint8_t
{
	Unknown,
	None,
	Bed,
	Base
};

inline Mark mark_of(const Record& r, int zone_base, bool start_point)
{
	if (!r.valid() || r.kind != Kind::Rest)
		return Mark::None;
	if (r.source != kNoSource)
		return zone_base > 0 ? Mark::Bed : zone_base == 0 ? Mark::None : Mark::Unknown;
	return start_point ? Mark::Base : Mark::None;
}

// The value the callback carries: "bed" | "base" | "none"; nullptr for Unknown (the callback passes nil).
inline const char* mark_name(Mark m)
{
	switch (m)
	{
	case Mark::Bed: return "bed";
	case Mark::Base: return "base";
	case Mark::None: return "none";
	case Mark::Unknown:
	default: return nullptr;
	}
}

// checkpoint::set for the record of base `ground` (its key, object, level, point
// and radius). An arrival onto a base (a level change, the first sight of him on a level) wrote an Arrival record that pushed
// the record before it down; 3 s later the base's record pushed the arrival down again: prev held a worthless arrival at the
// same base and the older record (his tent, the base he came from) was lost, on every arrival there. Now: when `r` is the
// base's record and the current record is an Arrival on the base's level within its ground (inside(), the strict ground), the
// base's record takes the arrival's place and prev is kept (Moved: written and announced once, nothing pushed down). Any other
// case is set().
inline SetOutcome set_on_base(Slot& s, const Record& r, const Spot& ground)
{
	if (is_base_record(r, ground) && s.cur.valid() && s.cur.kind == Kind::Arrival && s.cur.level == r.level &&
		inside(ground, s.cur.level, s.cur.pos, false))
	{
		s.cur = r;
		return SetOutcome::Moved;
	}
	return set(s, r);
}

// What the deciding process does for one player in one tick.
enum class Step : uint8_t
{
	Outside, // not on a friendly base: campfires and mod objects as before
	OnBase, // on a base: campfires and mod objects are no sources, nothing to write now
	SetBed // write the base's record now
};

// The visit of one player to a base (one per player, beside the campfire dwell).
struct Visit
{
	Key base = kNoBase;
	uint64_t since = 0;
	bool fired = false;
};

// Feed it every tick with the friendly base he is on (base_at; kNoBase: none), whether his checkpoint is settled here (it is
// this base's record already, or his own tent standing on this base: he chose it), whether he is in a fight and the clock.
// SetBed once when he has been on the base `dwell_ms` with an unsettled checkpoint and no fight; a settled checkpoint holds
// the count (so a folded tent, or a story checkpoint set on the base, gives way to the base `dwell_ms` later); a fight
// restarts it; leaving or another base starts a new visit.
inline Step step(Visit& v, Key base, bool settled, bool in_combat, uint64_t now_ms, uint32_t dwell_ms)
{
	if (!is_base_key(base))
	{
		v = Visit();
		return Step::Outside;
	}
	stay_moves(v, &Visit::base, base, now_ms);
	if (settled)
	{
		v.since = now_ms;
		v.fired = false;
		return Step::OnBase;
	}
	return stay_fires(v, in_combat, now_ms, dwell_ms) ? Step::SetBed : Step::OnBase;
}

// ---------------------------------------------------------------- lying down on a base

// The record a lie-down writes (who may lie down where is not this rule's: rest_place answers that, unchanged). On a
// friendly base (`bed` a usable record of it: kind Rest, with or without an object) it is the base's record, not the spot he
// lies at - unless he lies at his own tent (his choice). Off a base: where he lies, as before.
inline Record lie_down_record(const Record& where, bool at_own_tent, const Record* bed)
{
	if (at_own_tent || !bed || !bed->valid() || bed->kind != Kind::Rest)
		return where;
	return *bed;
}

// ---------------------------------------------------------------- bases of one level

// Two bases share ground: the same level, closer horizontally than both radii and both leave margins, and within twice the
// height a base reaches (with its leave margin). A player could walk from one to the other without leaving a base.
inline bool overlaps(const Spot& a, const Spot& b, float margin)
{
	if (a.level.empty() || a.level != b.level || !validate::finite3(a.pos) || !validate::finite3(b.pos))
		return false;
	const double dx = double(a.pos[0]) - double(b.pos[0]);
	const double dz = double(a.pos[2]) - double(b.pos[2]);
	const double dy = double(a.pos[1]) - double(b.pos[1]);
	const double r = double(a.radius) + double(b.radius) + 2.0 * double(margin);
	return dx * dx + dz * dz < r * r && std::fabs(dy) <= 2.0 * (double(kHeight) + double(kHeightLeaveMargin));
}

constexpr size_t kAdded = size_t(-1); // accept(): the candidate is a base of this level now
constexpr size_t kNotAdded = size_t(-2); // accept(): it names no base, its point is not a number, or the list is full

// The list of one level, built in order (the [coop_bases] rows first, then the start locations): a candidate that overlaps an
// accepted base makes no base of its own (the index of that base is returned: two sleep zones of one camp, a start location
// in a camp that has a bed), so two zones of one camp do not repeat "checkpoint updated" when he walks across it.
inline size_t accept(std::vector<Spot>& accepted, const Spot& candidate, float margin)
{
	if (!is_base_key(candidate.id) || !validate::finite3(candidate.pos) || candidate.level.empty())
		return kNotAdded;
	for (size_t i = 0; i < accepted.size(); ++i)
		if (accepted[i].id == candidate.id || overlaps(accepted[i], candidate, margin))
			return i;
	if (accepted.size() >= kMaxBases + kMaxStarts)
		return kNotAdded;
	accepted.push_back(candidate);
	return kAdded;
}

// ---------------------------------------------------------------- hostility

// The NPC community an actor community stands for: "actor_<faction>" -> <faction>; anything else as it is. Only
// the fallback of judged_community, when the relation table has no row for his own community.
inline std::string true_faction(const std::string& community)
{
	static const char kPrefix[] = "actor_";
	const size_t n = sizeof(kPrefix) - 1;
	if (community.size() > n && community.compare(0, n, kPrefix) == 0)
		return community.substr(n);
	return community;
}

// One owner community is hostile to this player: the game's own sum - the owner community's relation to his community plus
// the owner community's goodwill to him (relation_registry.community_relation + community_goodwill, the sum
// coop_task_audience.script hostile() judges a task giver by) - is at or below kEnemies. No owner: never.
inline bool hostile(bool has_owner, int relation, int goodwill)
{
	return has_owner && (long long)relation + (long long)goodwill <= (long long)kEnemies;
}

// Every owner the game knows (`valid_owners`, at least one) is his enemy (`hostile_owners`, counted with hostile()).
inline bool all_hostile(size_t valid_owners, size_t hostile_owners)
{
	return valid_owners > 0 && hostile_owners == valid_owners;
}

// A row's owners are every faction its smart terrain admits (the game's
// simulation data), not only the guards, so "hostile only when ALL owners are enemies" made Rookie Village (stalker, csky,
// ecolog) a friendly base for a mercenary or a soldier whom its stalker guards shoot. The FIRST owner of a row is its holder:
// the faction whose guards hold the place. A base is hostile to him when its holder is his enemy (`holder_hostile`; false
// when the row's first owner is no community the game knows: then there is no holder), or when every owner the game knows is
// (all_hostile). A base of nobody never is.
inline bool base_hostile(bool holder_hostile, size_t valid_owners, size_t hostile_owners)
{
	return valid_owners > 0 && (holder_hostile || all_hostile(valid_owners, hostile_owners));
}

// The community a base's guards judge a player by is his own as the game keeps
// it: an actor community "actor_<f>" is its own column of [communities_relations] (dynamic relations may change that column
// alone). `own`: his server entity's community index; `npc`: the index of the NPC community <f> it stands for (true_faction;
// < 0 when there is none); `rows`: the rows of the relation table. His own index while the table has a row for it, else the NPC
// community's, else none (-1).
inline int judged_community(int own, int npc, int rows)
{
	if (own >= 0 && own < rows)
		return own;
	if (npc >= 0 && npc < rows)
		return npc;
	return -1;
}

// ---------------------------------------------------------------- start locations (new_game_start_locations.ltx)

// A start location: its id (the section of its numbers), the level of its game vertex (a number the engine fills from the
// game graph; kNoLevel: none), its point and vertices, its owners (the row of [coop_start_bases], start_bases), and whether it
// was merged into an earlier start base of its level (`into`: that one's index).
struct Start
{
	std::string id;
	uint16_t level = kNoLevel;
	float pos[3] = {0, 0, 0};
	uint32_t lvid = 0xffffffffu;
	uint16_t gvid = 0xffff;
	std::vector<std::string> owners;
	bool merged = false;
	size_t into = kMaxStarts;
};

// The numbers of a start location's section: lvid and gvid plain decimals (not the "no vertex" values), x, y, z plain decimal
// numbers within +-kMaxStartCoord. On false `out` is left as it was; on true its owners, level and merge are reset.
inline bool parse_start(const std::string& id, const std::string& lvid, const std::string& gvid, const std::string& x,
	const std::string& y, const std::string& z, Start& out)
{
	if (!level_ok(id))
		return false;
	uint64_t lv = 0, gv = 0;
	if (!checkpoint::detail::parse_uint(std::string(ascii::trim(lvid, ascii::space_or_tab)), 0xfffffffeull, lv) ||
		!checkpoint::detail::parse_uint(std::string(ascii::trim(gvid, ascii::space_or_tab)), 0xfffeull, gv))
		return false;
	Start s;
	s.id = id;
	const std::string* v[3] = {&x, &y, &z};
	for (int i = 0; i < 3; ++i)
	{
		float f = 0;
		if (!checkpoint::detail::parse_float(std::string(ascii::trim(*v[i], ascii::space_or_tab)), f) || !(std::fabs(f) < kMaxStartCoord))
			return false;
		s.pos[i] = f;
	}
	s.lvid = uint32_t(lv);
	s.gvid = uint16_t(gv);
	out = s;
	return true;
}

// The index of start location `id` in `list`; list.size() when it is not there.
inline size_t find_start(const std::vector<Start>& list, const std::string& id)
{
	for (size_t i = 0; i < list.size(); ++i)
		if (list[i].id == id)
			return i;
	return list.size();
}

// A start location is a base only by a row of [coop_start_bases]
// (configs\mod_system_coop_checkpoints.ltx): <start location id> = <owner community>[, <owner>...] or none. Only real bases
// without a sleep zone are listed (the lead curated them); the factions whose new-game lists name a place are not its owners.
// The first owner is the holder, as in [coop_bases]; merge_starts appends the merged ones' owners after it.
struct StartRow
{
	std::string id;
	std::vector<std::string> owners;
};

// One row of [coop_start_bases]: the id a name ([A-Za-z0-9_], 1..64), the value 1..kMaxOwners community names or "none" (or
// nothing). On false `out` is left as it was.
inline bool parse_start_row(const std::string& id, const std::string& value, StartRow& out)
{
	if (!level_ok(id))
		return false;
	StartRow r;
	r.id = id;
	if (!detail::owners_of(detail::fields_of(value, kMaxOwners), 0, r.owners))
		return false;
	out = r;
	return true;
}

// The start bases: every row in its order, at most kMaxStarts, an id once (its first row), with the place `located` holds for
// that id (parse_start of its section in the start locations file) and the row's owners. A row whose id `located` does not
// hold makes no base and is named in `missing`; a place without a row makes none either.
inline std::vector<Start> start_bases(
	const std::vector<StartRow>& rows, const std::vector<Start>& located, std::vector<std::string>& missing)
{
	std::vector<Start> out;
	for (const StartRow& row : rows)
	{
		if (out.size() >= kMaxStarts)
			break;
		if (find_start(out, row.id) != out.size())
			continue;
		const size_t i = find_start(located, row.id);
		if (i == located.size())
		{
			missing.push_back(row.id);
			continue;
		}
		Start s = located[i];
		s.owners = row.owners;
		s.merged = false;
		s.into = kMaxStarts;
		out.push_back(s);
	}
	return out;
}

// Start locations of one level whose ground overlaps (overlaps(), `radius` each, the ltx positions) are one base: in list
// order a later one is merged into the earliest base it overlaps (`merged`, `into`) and gives it its owners. Only bases that
// were not merged take others. A location of no level merges with nothing. Uses nothing but the ltx and the level of the game
// vertex, so every process (host, world server, map server) gets the same list and the same keys.
inline void merge_starts(std::vector<Start>& list, float radius, float margin)
{
	const auto spot_of = [radius](const Start& s) {
		Spot p;
		p.id = kStartKeyBase;
		p.level = std::to_string(unsigned(s.level));
		p.pos[0] = s.pos[0];
		p.pos[1] = s.pos[1];
		p.pos[2] = s.pos[2];
		p.radius = radius;
		return p;
	};
	for (size_t i = 0; i < list.size(); ++i)
	{
		list[i].merged = false;
		list[i].into = kMaxStarts;
		if (list[i].level == kNoLevel)
			continue;
		const Spot me = spot_of(list[i]);
		for (size_t j = 0; j < i; ++j)
		{
			if (list[j].merged || list[j].level != list[i].level || !overlaps(spot_of(list[j]), me, margin))
				continue;
			list[i].merged = true;
			list[i].into = j;
			for (const std::string& o : list[i].owners)
			{
				bool known = false;
				for (const std::string& had : list[j].owners)
					known = known || had == o;
				if (!known && list[j].owners.size() < kMaxStartOwners)
					list[j].owners.push_back(o);
			}
			break;
		}
	}
}

// ---------------------------------------------------------------- the footing a map server reports

// "He stands on the ground of a base": the map server of his level finds it by geometry (base_at over its own list of the
// level, none hostile: hostility is the world server's) and sends it in LevelBodyFacts; the world server checks it against its
// own tables (coop_checkpoint_sources.cpp host_footing) before it uses it. Wire: u8 kind, u16 ref, 3 floats, u32 lv, u16 gv.
constexpr uint8_t kFootNone = 0;
constexpr uint8_t kFootZone = 1; // ref: the sleep zone's object id
constexpr uint8_t kFootStart = 2; // ref: the start location's index
struct Footing
{
	uint8_t kind = kFootNone;
	uint16_t ref = kNoId;
	float pos[3] = {0, 0, 0}; // the base's point (where its record lies)
	uint32_t lv = 0xffffffffu; // its level vertex
	uint16_t gv = 0xffff; // its game vertex
};

// The form is sound: a known kind; "none" names nothing (ref kNoId), a zone names an object id, a start an index below
// kMaxStarts; the point is a number a record may hold. Whether it names a base of the world is the world server's check.
inline bool footing_ok(const Footing& f)
{
	if (f.kind > kFootStart)
		return false;
	if (f.kind == kFootNone && f.ref != kNoId)
		return false;
	if (f.kind == kFootZone && f.ref == kNoSource)
		return false;
	if (f.kind == kFootStart && size_t(f.ref) >= kMaxStarts)
		return false;
	for (int i = 0; i < 3; ++i)
		if (!std::isfinite(f.pos[i]) || std::fabs(f.pos[i]) > kMaxCoord)
			return false;
	return true;
}

// The key a footing names; kNoBase for "none" and for a footing that is not sound.
inline Key key_of(const Footing& f)
{
	if (!footing_ok(f))
		return kNoBase;
	if (f.kind == kFootZone)
		return zone_key(f.ref);
	if (f.kind == kFootStart)
		return start_key(f.ref);
	return kNoBase;
}

// The kind and ref of key `k` into `out` (its point is the caller's); kNoBase is "none". False (out unchanged): `k` names
// nothing.
inline bool footing_of(Key k, Footing& out)
{
	if (k == kNoBase)
	{
		out.kind = kFootNone;
		out.ref = kNoId;
		return true;
	}
	if (is_zone_key(k))
	{
		out.kind = kFootZone;
		out.ref = zone_of(k);
		return true;
	}
	if (is_start_key(k))
	{
		out.kind = kFootStart;
		out.ref = uint16_t(start_of(k));
		return true;
	}
	return false;
}

// The world server's check of a footing a map server reported. `sender_level`:
// the level of the map server it came from; `gv_level`: the level of the footing's game vertex in this world's game graph
// (kNoLevel: not a game vertex); `base`: what this process's own tables say of the base the footing names (`known` false: no
// [coop_bases] zone of this world, or no start base that is not merged); its level, authored point and radius. A start
// base's reported point must be near its ltx point. For a sleep zone, only the map server has the level graph to place the
// bed. The restrictor's o_Position may be far from its node or from the centre of its shapes.
struct FootBase
{
	bool known = false;
	uint16_t level = kNoLevel;
	float pos[3] = {0, 0, 0};
	float radius = kDefaultRadius;
};

enum class FootCheck : uint8_t
{
	Ok, // keep it
	None, // a sound "none": he stands on no base (nothing refused)
	NotSound, // kind, ref or point not what a footing may hold
	VertexOff, // its game vertex is not on the sender's level
	NoBase, // it names no base of this world
	OtherLevel, // the base is on another level than the sender's
	OffGround, // its point is not on the base's ground
	BodyOff // the body position of the same report is not on the base's ground
};

inline const char* foot_check_name(FootCheck c)
{
	switch (c)
	{
	case FootCheck::Ok: return "ok";
	case FootCheck::None: return "none";
	case FootCheck::NotSound: return "not a footing (kind, ref or point)";
	case FootCheck::VertexOff: return "its game vertex is not on the level of that map server";
	case FootCheck::NoBase: return "no base of this world (an unknown zone, an unknown or merged start base)";
	case FootCheck::OtherLevel: return "the base is on another level";
	case FootCheck::OffGround: return "the point is off the ground of the base";
	case FootCheck::BodyOff: return "the body of the same report is off the ground of the base";
	}
	return "?";
}

// Cross-check the same report's body (LevelEntity State) against the reported bed for a sleep zone, or the configured
// point for a start base. Allow the radius + 2 x `leave_margin` horizontally and 2 x kHeight vertically. The map server may
// have used the live object's position when finding its footing, while the report carries the server entity's position.
inline FootCheck footing_check(const Footing& f, uint16_t sender_level, uint16_t gv_level, const FootBase& base,
	float leave_margin, const float body[3])
{
	if (f.kind == kFootNone && footing_ok(f))
		return FootCheck::None;
	if (!is_base_key(key_of(f)))
		return FootCheck::NotSound;
	if (sender_level == kNoLevel || gv_level != sender_level)
		return FootCheck::VertexOff;
	if (!base.known)
		return FootCheck::NoBase;
	if (base.level != sender_level)
		return FootCheck::OtherLevel;
	const bool zone = f.kind == kFootZone;
	if ((!zone && !validate::finite3(base.pos)) || !std::isfinite(base.radius) || !std::isfinite(leave_margin))
		return FootCheck::OffGround;
	const double h = 2.0 * double(kHeight); // no fabs: the engine marks it deprecated (C4995)
	if (!zone)
	{
		const double dx = double(f.pos[0]) - double(base.pos[0]);
		const double dz = double(f.pos[2]) - double(base.pos[2]);
		const double dy = double(f.pos[1]) - double(base.pos[1]);
		const double r = double(base.radius) + double(leave_margin);
		if (!(dx * dx + dz * dz <= r * r) || !(dy <= h && dy >= -h))
			return FootCheck::OffGround;
	}
	if (!body || !validate::finite3(body))
		return FootCheck::BodyOff;
	const float* ground = zone ? f.pos : base.pos;
	const double bx = double(body[0]) - double(ground[0]);
	const double bz = double(body[2]) - double(ground[2]);
	const double by = double(body[1]) - double(ground[1]);
	const double br = double(base.radius) + 2.0 * double(leave_margin);
	if (!(bx * bx + bz * bz <= br * br) || !(by <= h && by >= -h))
		return FootCheck::BodyOff;
	return FootCheck::Ok;
}

// What the world server keeps of a map server's body report (the footing with
// the rest) is usable only while it is fresh (`fresh_ms` since `at_ms`) and came from `facts_level`, the level he is on now
// (`level_now`): a report of the level he has just left says nothing of where he is.
inline bool facts_current(uint16_t facts_level, uint16_t level_now, uint64_t at_ms, uint64_t now_ms, uint32_t fresh_ms)
{
	return facts_level != kNoLevel && facts_level == level_now && now_ms >= at_ms && now_ms - at_ms <= fresh_ms;
}

// ... and the base a stored footing names is his only when its game vertex is on the level of his entity now.
inline bool footing_on_level(uint16_t footing_level, uint16_t entity_level)
{
	return footing_level != kNoLevel && footing_level == entity_level;
}

// The far path's decision (the world server, a player whose character is an object of a map server): the base he is on is
// the one his footing names when that footing is usable (fresh, of his level, on the level of his entity, a base of this
// world: `usable`) and the base is not hostile to him; then the same step() as on the host's level.
inline Key far_key(const Footing& f, bool usable, bool hostile) { return usable && !hostile ? key_of(f) : kNoBase; }

inline Step far_step(Visit& v, const Footing& f, bool usable, bool hostile, bool settled, bool in_combat, uint64_t now_ms,
	uint32_t dwell_ms)
{
	const Key k = far_key(f, usable, hostile);
	const bool on = is_base_key(k);
	return step(v, k, on && settled, on && !settled && in_combat, now_ms, dwell_ms);
}

// The record of the base could not be written (no record, the store refused):
// the visit counts again from now, so the base is tried again after another dwell, not only after he leaves and comes back.
inline void retry_visit(Visit& v, uint64_t now_ms)
{
	v.fired = false;
	v.since = now_ms;
}

// ---------------------------------------------------------------- respawn

// What a respawn does with a current record that is the
// record of a base hostile to him now. Keep: it is not such a record, or it is not on `level` (no place here anyway). UsePrev:
// the previous record is on `level`. DropCur: there is none on `level`: the hostile record is not used, he comes back where a
// player without a checkpoint does (resolve: the anchor, the arrival point, the body). The slot in the store stays as it is.
// The previous record is used only when it is not itself the record of a base hostile to
// him (`prev_hostile_bed`): a Freedom player with the Bar's bed and the Duty post's bed before it comes back at neither. The
// same when the current record is on another level and resolve would take the previous one of `level`: DropPrev.
enum class BedChoice : uint8_t
{
	Keep,
	UsePrev,
	DropCur,
	DropPrev // the current record is not on `level`, the previous one is, and it is a hostile base's
};

inline BedChoice hostile_bed_choice(const Slot& s, const std::string& level, bool cur_hostile_bed, bool prev_hostile_bed)
{
	const bool cur_here = s.cur.valid() && s.cur.level == level;
	const bool prev_here = s.prev.valid() && s.prev.level == level;
	if (cur_here && cur_hostile_bed)
		return prev_here && !prev_hostile_bed ? BedChoice::UsePrev : BedChoice::DropCur;
	if (!cur_here && prev_here && prev_hostile_bed)
		return BedChoice::DropPrev;
	return BedChoice::Keep;
}

// Applies hostile_bed_choice to `s` (the respawn's copy of the slot) and returns it. DropCur also drops a previous record on
// `level`; one on another level stays (resolve skips it).
inline BedChoice skip_hostile_bed(Slot& s, const std::string& level, bool cur_hostile_bed, bool prev_hostile_bed)
{
	const BedChoice c = hostile_bed_choice(s, level, cur_hostile_bed, prev_hostile_bed);
	if (c == BedChoice::UsePrev)
	{
		s.cur = s.prev;
		s.prev = Record();
	}
	else if (c == BedChoice::DropCur)
	{
		s.cur = Record();
		if (s.prev.valid() && s.prev.level == level)
			s.prev = Record();
	}
	else if (c == BedChoice::DropPrev)
		s.prev = Record();
	return c;
}

// After a hostile base's record was dropped, a fallback point of the respawn (his anchor,
// the host's arrival, his body) is used only when it is not on that base's ground (with the leave margin: he was on it).
// `dropped`: the ground of the dropped record, nullptr when nothing was dropped. A missing point is never usable.
inline bool fallback_ok(const Spot* dropped, const std::string& level, const float* pos)
{
	if (!pos)
		return false;
	return !dropped || !inside(*dropped, level, pos, true);
}
} // namespace base
} // namespace checkpoint
} // namespace coop
