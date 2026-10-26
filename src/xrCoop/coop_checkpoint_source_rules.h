// Anomaly Coop — the pure part of the checkpoint sources on the host (F3 Task 7, CHECKPOINTS_DEATH.md §2.2): the
// settings of mod_system_coop_checkpoints.ltx and their bounds, the CheckpointClaim message, the nearest source a
// player stands at, the objects mods declare as camps, the mark a tent carries in its custom data (whose it is and
// the item it was made from), the item a tent is paid with, the sections behind the ids of sources and the book of
// tents (who owns which one, who may pack which one up). The engine side is
// xrGame/coop/coop_checkpoint_sources.cpp.
// Pure C++17, header-only, no engine dependencies: used by the engine and by the unit-test exe.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "coop_checkpoint_rules.h"
#include "coop_checkpoint_store.h"
#include "coop_wire.h"

namespace coop
{
namespace checkpoint
{
// ---------------------------------------------------------------- settings ([coop_checkpoints])

// A radius of a source, of the fight check or of a tent: a number in (0, kMaxSourceRadius] metres.
constexpr float kMaxSourceRadius = 500.0f;
// A period of a source: 0..kMaxSourcePeriodMs (10 minutes).
constexpr uint32_t kMaxSourcePeriodMs = 600000;
// R1 M-11: a claim waits at least this long — the host's scan runs once a second and the stock tent animation takes
// 4 s, so a shorter wait never finds the tent. F3a T7 R1: LEGACY with tent_claim_ms (no claim waits any more).
constexpr uint32_t kMinClaimMs = 5000;
// tent_sections keeps at most this many names; a name is 1..64 characters of [A-Za-z0-9_].
constexpr size_t kMaxTentSections = 16;

struct SourceSettings
{
	float campfire_radius = 3.0f; // a lit campfire this close counts
	uint32_t campfire_dwell_ms = 5000; // stood by it this long, not in a fight
	float combat_radius = 50.0f; // an enemy that picked the player this close is a fight
	std::vector<std::string> tent_sections{"ph_tent", "ph_sleepbag"}; // the objects that are tents
	// R1 (Ruling F3-6): what the host puts up for a client's claim, one of tent_sections each.
	std::string tent_section = "ph_tent";
	std::string sleepbag_section = "ph_sleepbag";
	float tent_rest_radius = 2.0f; // the own tent this close is a rest place
	float tent_claim_radius = 4.0f; // a claimed spot: this close to the player, the tent this close to the spot
	// LEGACY (F3a T7 R1): the host's own claim waited this long for the tent the stock put up. Every player's tent is put
	// up by the host at once now (coop_checkpoint_sources place_for): neither read from the ltx nor used.
	uint32_t tent_claim_ms = 15000;
	std::string cover_list = "misc\\surge_manager.ltx"; // its [list]: the stock surge covers (rest places too)
	// R1 I-1: its [tent_i_bag_sections] and [sleeping_bag_sections] (the stock TentBagSections / SleepBagSections):
	// the items a client pays for a tent / a sleeping bag with.
	std::string item_lists = "items\\settings\\itms_manager.ltx";
};

inline bool radius_ok(float r) { return std::isfinite(r) && r > 0.0f && r <= kMaxSourceRadius; }
inline bool period_ok(double ms) { return std::isfinite(ms) && ms >= 0.0 && ms <= double(kMaxSourcePeriodMs); }
// tent_claim_ms (LEGACY, see there): a whole number of ms in kMinClaimMs..kMaxSourcePeriodMs.
inline bool claim_period_ok(double ms)
{
	return period_ok(ms) && ms >= double(kMinClaimMs) && ms == std::floor(ms);
}

// "ph_tent, ph_sleepbag" -> {"ph_tent", "ph_sleepbag"}: split at commas, blanks trimmed; an empty, overlong or
// malformed name and a repeated one are skipped; at most `max_names` are kept, in order.
std::vector<std::string> split_names(const std::string& list, size_t max_names = kMaxTentSections);

inline bool name_listed(const std::string& name, const std::vector<std::string>& list)
{
	return std::find(list.begin(), list.end(), name) != list.end();
}

// ---------------------------------------------------------------- Msg::CheckpointClaim

// u8 what (ClaimWhat), vec3 pos.
void write_claim(Writer& w, ClaimWhat what, const float pos[3]);
// Strict: what is one of ClaimWhat, a position within +-kMaxCoord (NaN and infinity fail), nothing after it. On false
// the outputs are left as they were.
bool read_claim(Reader& rd, ClaimWhat& what, float out[3]);

// ---------------------------------------------------------------- the source a player stands at

struct SourceSpot
{
	uint16_t id = kNoSource;
	Kind kind = Kind::None; // Campfire or Mod
	float pos[3] = {0, 0, 0};
	float radius = 0.0f; // its own reach
};

// The spot nearest to `at` among those whose own radius reaches it (a tie goes to the lower id); nullptr if none.
const SourceSpot* nearest_spot(const std::vector<SourceSpot>& spots, const float at[3]);

// ---------------------------------------------------------------- objects mods declare as camps

constexpr size_t kMaxModSources = 256;

struct ModSource
{
	float radius = 0.0f;
	std::string section; // of the object when it was registered (R1 M-4: an id given to another object is not it)
};

// coop.checkpoint.register_source(id, radius): an object a mod declares as a camp ("as a campfire": dwell, no fight).
class ModSources
{
public:
	// False for "no object" (0xffff), a radius that is not radius_ok, or a new id over kMaxModSources; a known id takes
	// the new radius and section.
	bool add(uint16_t id, float radius, const std::string& section);
	bool remove(uint16_t id);
	bool has(uint16_t id) const;
	const std::map<uint16_t, ModSource>& all() const { return m_src; }
	void clear() { m_src.clear(); }

private:
	std::map<uint16_t, ModSource> m_src;
};

// ---------------------------------------------------------------- the sections behind ids (R1 M-4)

constexpr size_t kMaxRememberedSources = 2048;

// ALife hands a freed id to the next object it makes. A source is the object it was only while its id still names an
// object of the same section: the section is remembered when the id is first met.
class SourceSections
{
public:
	// True if `id` names an object of the section remembered for it (first met: remembered now; over the bound: not
	// remembered and taken as the same). False when another section is under the id: the old object is gone.
	bool same(uint16_t id, const std::string& section);
	// Keeps only these ids.
	void retain(const std::set<uint16_t>& ids);
	void forget(uint16_t id) { m_sect.erase(id); }
	void clear() { m_sect.clear(); }
	size_t size() const { return m_sect.size(); }

private:
	std::map<uint16_t, std::string> m_sect;
};

// ---------------------------------------------------------------- the mark of a tent (its custom data)

// R1 I-2: whose a tent is and what it was made from lives on the tent itself, so it outlasts a load and a level change:
// section [coop_tent] of its custom data, `owner` = the player key digest of the owner (ledger::character_key) or
// kHostOwner, `item` = the item section the host took for it (given back when it is packed up; none on the host's own
// tents the stock item_tent put up before F3a T7 - they give back the stock item of their kind).
constexpr const char* kTentIniSection = "coop_tent";
constexpr const char* kTentIniOwner = "owner";
constexpr const char* kTentIniItem = "item";
constexpr const char* kHostOwner = "host"; // the host's actor has no player key
constexpr size_t kMaxOwnerKey = 80;

struct TentMark
{
	std::string owner;
	std::string item; // empty: nothing to give back
};

// An owner key: 1..kMaxOwnerKey characters of [A-Za-z0-9_#] (a 64-digit hex digest, kHostOwner, "#<id>").
bool owner_key_ok(const std::string& key);
// Writes the mark (an owner that is not owner_key_ok writes nothing; an item that is not a section name is left out).
// Other sections, keys and line breaks stay as they are.
void tent_mark_to_ini(std::string& text, const TentMark& m);
// The mark of a tent; false if it has no owner (or a malformed one). A malformed item reads as none.
bool tent_mark_from_ini(const std::string& text, TentMark& out);

// F3a T7 R1: may the player whose owner key is `key` pack up a tent whose owner (the book's, from its mark) is `owner`
// (empty: nobody's)? His own tent, always. A tent nobody owns only the host player (kHostOwner): the stock put every
// unmarked tent up for the host (in single player, or before a claim named it) - a client's tent always carries his mark
// (Ruling F3-6). Another player's tent, never.
inline bool may_pack(const std::string& key, const std::string& owner)
{
	return owner_key_ok(key) && (owner == key || (owner.empty() && key == kHostOwner));
}

// ---------------------------------------------------------------- the item a tent is paid with (R1 I-1)

struct ItemSeen
{
	uint16_t id = kNoSource;
	std::string section;
};

// The item to take: the lowest id whose section is one of `sections`; kNoSource if none.
uint16_t pick_item(const std::vector<ItemSeen>& items, const std::vector<std::string>& sections);

// ---------------------------------------------------------------- tents

constexpr size_t kMaxTents = 1024; // tents (and their owners) the host keeps track of; more are not kept

struct TentSeen
{
	uint16_t id = kNoSource;
	float pos[3] = {0, 0, 0};
	uint8_t what = 0; // ClaimWhat of its section (tent_section / sleepbag_section); 0: another tent section, either
};

// What the host knows of the tents of a world: when each one appeared on the level and whose it is (by owner key: the
// engine fills it from the tents' marks). F3a T7 R1: the claims that waited for a tent the stock put up (the host's own,
// before F3a T7) are gone - the host puts every player's tent up at once.
class TentBook
{
public:
	// A newly served world: tents and owners are forgotten, and every tent the next observe() sees is old.
	void new_world();
	// The tents of tent_sections now on the level. A tent not known yet appeared at `now_ms` (old on the first call
	// after new_world()). False if some were not kept (kMaxTents).
	bool observe(const std::vector<TentSeen>& tents, uint64_t now_ms);
	// The tent is gone from the world: forgotten with its owner.
	void forget(uint16_t tent);
	// False for an owner key that is not owner_key_ok, or a new tent over kMaxTents owners.
	bool set_owner(uint16_t tent, const std::string& key);
	const std::string& owner_of(uint16_t tent) const; // empty if nobody's
	size_t owned_by(const std::string& key) const;
	// R1 I-2: one more tent for this key keeps it within `max` (tents and sleeping bags together).
	bool may_place(const std::string& key, size_t max) const;
	// Every tent known (for the check that it still exists).
	std::vector<uint16_t> known() const;
	size_t tents() const { return m_tents.size(); }
	size_t owners() const { return m_owner.size(); }

private:
	struct Tent
	{
		uint64_t first_seen = 0;
		bool old = false; // stood when the world was served
	};
	std::map<uint16_t, Tent> m_tents;
	std::map<uint16_t, std::string> m_owner; // tent -> owner key
	bool m_first = true; // the next observe() is the first of this world
};

// ---------------------------------------------------------------- implementation

namespace detail
{
inline bool blank(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

// Squared distance in double: no overflow for coordinates within kMaxCoord; NaN stays NaN (every comparison fails).
inline double dist2(const float a[3], const float b[3])
{
	const double dx = double(a[0]) - double(b[0]);
	const double dy = double(a[1]) - double(b[1]);
	const double dz = double(a[2]) - double(b[2]);
	return dx * dx + dy * dy + dz * dz;
}

inline bool pos_ok(const float p[3])
{
	for (int i = 0; i < 3; ++i)
		if (!std::isfinite(p[i]) || std::fabs(p[i]) > kMaxCoord)
			return false;
	return true;
}

inline const std::string& empty_string()
{
	static const std::string s;
	return s;
}
} // namespace detail

inline std::vector<std::string> split_names(const std::string& list, size_t max_names)
{
	std::vector<std::string> out;
	size_t pos = 0;
	while (out.size() < max_names)
	{
		const size_t comma = list.find(',', pos);
		size_t b = pos;
		size_t e = comma == std::string::npos ? list.size() : comma;
		while (b < e && detail::blank(list[b]))
			++b;
		while (e > b && detail::blank(list[e - 1]))
			--e;
		const std::string name = list.substr(b, e - b);
		// a section name has the alphabet of a level name: 1..64 characters of [A-Za-z0-9_]
		if (level_ok(name) && std::find(out.begin(), out.end(), name) == out.end())
			out.push_back(name);
		if (comma == std::string::npos)
			break;
		pos = comma + 1;
	}
	return out;
}

inline void write_claim(Writer& w, ClaimWhat what, const float pos[3])
{
	w.u8(static_cast<uint8_t>(what));
	w.vec3(pos);
}

inline bool read_claim(Reader& rd, ClaimWhat& what, float out[3])
{
	const uint8_t w = rd.u8();
	float p[3] = {0, 0, 0};
	rd.vec3(p);
	if (!rd.ok() || rd.remaining() != 0 || !claim_what_ok(w) || !detail::pos_ok(p))
		return false;
	what = static_cast<ClaimWhat>(w);
	for (int i = 0; i < 3; ++i)
		out[i] = p[i];
	return true;
}

inline const SourceSpot* nearest_spot(const std::vector<SourceSpot>& spots, const float at[3])
{
	const SourceSpot* best = nullptr;
	double best_d2 = 0.0;
	for (const SourceSpot& s : spots)
	{
		if (s.id == kNoSource || !radius_ok(s.radius))
			continue;
		const double d2 = detail::dist2(s.pos, at);
		if (!(d2 <= double(s.radius) * double(s.radius))) // NaN fails too
			continue;
		if (!best || d2 < best_d2 || (d2 == best_d2 && s.id < best->id))
		{
			best = &s;
			best_d2 = d2;
		}
	}
	return best;
}

inline bool ModSources::add(uint16_t id, float radius, const std::string& section)
{
	if (id == kNoSource || !radius_ok(radius))
		return false;
	const auto it = m_src.find(id);
	if (it != m_src.end())
	{
		it->second.radius = radius;
		it->second.section = section;
		return true;
	}
	if (m_src.size() >= kMaxModSources)
		return false;
	ModSource s;
	s.radius = radius;
	s.section = section;
	m_src.emplace(id, s);
	return true;
}

inline bool ModSources::remove(uint16_t id) { return m_src.erase(id) != 0; }

inline bool ModSources::has(uint16_t id) const { return m_src.count(id) != 0; }

inline bool SourceSections::same(uint16_t id, const std::string& section)
{
	const auto it = m_sect.find(id);
	if (it != m_sect.end())
		return it->second == section;
	if (m_sect.size() < kMaxRememberedSources)
		m_sect.emplace(id, section);
	return true;
}

inline void SourceSections::retain(const std::set<uint16_t>& ids)
{
	for (auto it = m_sect.begin(); it != m_sect.end();)
		if (ids.count(it->first))
			++it;
		else
			it = m_sect.erase(it);
}

inline bool owner_key_ok(const std::string& key)
{
	if (key.empty() || key.size() > kMaxOwnerKey)
		return false;
	for (const char c : key)
	{
		const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
			c == '#';
		if (!ok)
			return false;
	}
	return true;
}

inline void tent_mark_to_ini(std::string& text, const TentMark& m)
{
	if (!owner_key_ok(m.owner))
		return;
	ini_set(text, kTentIniSection, kTentIniOwner, m.owner);
	if (level_ok(m.item)) // a section name: the alphabet of a level name
		ini_set(text, kTentIniSection, kTentIniItem, m.item);
	else
		ini_erase(text, kTentIniSection, kTentIniItem);
}

inline bool tent_mark_from_ini(const std::string& text, TentMark& out)
{
	TentMark m;
	if (!ini_get(text, kTentIniSection, kTentIniOwner, m.owner) || !owner_key_ok(m.owner))
		return false;
	if (!ini_get(text, kTentIniSection, kTentIniItem, m.item) || !level_ok(m.item))
		m.item.clear();
	out = m;
	return true;
}

inline uint16_t pick_item(const std::vector<ItemSeen>& items, const std::vector<std::string>& sections)
{
	uint16_t best = kNoSource;
	for (const ItemSeen& it : items)
		if (it.id != kNoSource && it.id < best && name_listed(it.section, sections))
			best = it.id;
	return best;
}

inline void TentBook::new_world()
{
	m_tents.clear();
	m_owner.clear();
	m_first = true;
}

inline bool TentBook::observe(const std::vector<TentSeen>& tents, uint64_t now_ms)
{
	bool all = true;
	for (const TentSeen& t : tents)
	{
		if (t.id == kNoSource || m_tents.count(t.id))
			continue;
		if (m_tents.size() >= kMaxTents)
		{
			all = false;
			continue;
		}
		Tent n;
		n.first_seen = now_ms;
		n.old = m_first;
		m_tents.emplace(t.id, n);
	}
	m_first = false;
	return all;
}

inline void TentBook::forget(uint16_t tent)
{
	m_tents.erase(tent);
	m_owner.erase(tent);
}

inline bool TentBook::set_owner(uint16_t tent, const std::string& key)
{
	if (tent == kNoSource || !owner_key_ok(key))
		return false;
	const auto it = m_owner.find(tent);
	if (it != m_owner.end())
	{
		it->second = key;
		return true;
	}
	if (m_owner.size() >= kMaxTents)
		return false;
	m_owner.emplace(tent, key);
	return true;
}

inline const std::string& TentBook::owner_of(uint16_t tent) const
{
	const auto it = m_owner.find(tent);
	return it == m_owner.end() ? detail::empty_string() : it->second;
}

inline size_t TentBook::owned_by(const std::string& key) const
{
	size_t n = 0;
	for (const auto& it : m_owner)
		if (it.second == key)
			++n;
	return n;
}

inline bool TentBook::may_place(const std::string& key, size_t max) const
{
	return owner_key_ok(key) && owned_by(key) < max;
}

inline std::vector<uint16_t> TentBook::known() const
{
	std::set<uint16_t> ids;
	for (const auto& it : m_tents)
		ids.insert(it.first);
	for (const auto& it : m_owner)
		ids.insert(it.first);
	return std::vector<uint16_t>(ids.begin(), ids.end());
}
} // namespace checkpoint
} // namespace coop
