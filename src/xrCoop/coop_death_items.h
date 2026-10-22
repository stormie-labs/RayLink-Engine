// Anomaly Coop — which of a dead character's things a death may take, and where they land (F3 Task 9,
// CHECKPOINTS_DEATH.md §3.3). The engine layer (xrGame/coop/coop_death.cpp) reads the facts of every item and the
// tables of configs\mod_system_coop_death.ltx; this header turns them into the candidates of pick_losses (quest items
// out, a stable order), checks the drop point and carries Msg::DeathMarker.
// Pure C++17, header-only, no engine dependencies: used by the engine and by the unit-test exe.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "coop_death_rules.h"
#include "coop_wire.h"

namespace coop
{
namespace death
{
// The ltx name of a category (category_from_name the other way round); "?" for anything else.
inline const char* category_name(uint8_t category)
{
	static const char* const kNames[kCategories] = {
		"materials", "ammo", "consumables", "gear", "artefacts", "tools", "other"};
	return category < kCategories ? kNames[category] : "?";
}

// [coop_death_kinds]: an item's ltx `kind` -> its loss category.
class KindTable
{
public:
	static constexpr size_t kMaxKinds = 256; // a mod pack's ltx cannot grow it without end
	static constexpr size_t kMaxKindLength = 64;

	// False (the line is skipped) for an empty or overlong kind, an unknown category or a full table. A kind given
	// again takes the later category (a DLTX override comes after the base file).
	bool add(const char* kind, const char* category)
	{
		uint8_t c = 0;
		if (!kind || !kind[0] || std::strlen(kind) > kMaxKindLength || !category_from_name(category, c))
			return false;
		for (auto& row : m_rows)
			if (row.first == kind)
			{
				row.second = c;
				return true;
			}
		if (m_rows.size() >= kMaxKinds)
			return false;
		m_rows.emplace_back(kind, c);
		return true;
	}

	// The category of `kind` (matched exactly, as the ltx has it). False if it is not in the table; `out` is then
	// unchanged.
	bool find(const std::string& kind, uint8_t& out) const
	{
		for (const auto& row : m_rows)
			if (row.first == kind)
			{
				out = row.second;
				return true;
			}
		return false;
	}

	size_t size() const { return m_rows.size(); }
	void clear() { m_rows.clear(); }

private:
	std::vector<std::pair<std::string, uint8_t>> m_rows;
};

// What the engine knows of one item the dead character carries.
struct ItemFacts
{
	uint16_t id = 0xffff;
	std::string kind; // the item's ltx `kind`, empty if none
	bool ammo = false; // CWeaponAmmo
	bool outfit = false; // CCustomOutfit or CHelmet
	bool quest = false; // IsQuestItem()
	bool pda = false; // CPda
	bool helper = false; // is_helper_item(): the engine's own, never a world object
	bool world = true; // an ALife entity: in the save and on every machine (false: host only, e.g. an NPC's supply bolt)
	bool wearable = false; // its owner can have it in a slot or on the belt (a base slot, or the `belt` flag)
	bool worn = false; // F3 T8b: the owner reported it in a slot or on the belt (mark_worn): never lost
};

// At most this many items are offered (the lowest ids): the whole set goes to pick_losses in one vector.
constexpr size_t kMaxCandidates = 1024;

// kind i_quest / i_letter: story items, never lost.
inline bool kept_kind(const std::string& kind)
{
	return kind == "i_quest" || kind == "i_letter";
}

// Never taken by a death: quest items (IsQuestItem, kind i_quest / i_letter), the PDA, helper items and items that are
// not world objects.
inline bool never_lost(const ItemFacts& f)
{
	return f.id == 0xffff || f.quest || f.pda || f.helper || !f.world || kept_kind(f.kind);
}

// The loss category (plan T9 step 2): ammo -> Ammo; else the kind in `kinds`; else an outfit or a helmet -> Gear; else
// Other.
inline uint8_t category_of(const ItemFacts& f, const KindTable& kinds)
{
	if (f.ammo)
		return Ammo;
	uint8_t c = Other;
	if (!f.kind.empty() && kinds.find(f.kind, c))
		return c;
	return f.outfit ? uint8_t(Gear) : uint8_t(Other);
}

namespace detail
{
// Sorts by id, drops repeated ids, keeps the lowest kMaxCandidates.
template <class T, class Id>
void sort_unique_bounded(std::vector<T>& v, Id id_of)
{
	std::sort(v.begin(), v.end(), [&](const T& a, const T& b) { return id_of(a) < id_of(b); });
	v.erase(std::unique(v.begin(), v.end(), [&](const T& a, const T& b) { return id_of(a) == id_of(b); }), v.end());
	if (v.size() > kMaxCandidates)
		v.resize(kMaxCandidates);
}
} // namespace detail

// F3 T8b (Ruling F3-4): applies the list of what the owner of a client's proxy reported on him (his slots and belt, host
// numbering, as survival::host_worn_items gives it) to the proxy's items: `worn` is set on the items named in it and
// cleared on all the others, so a second list replaces the first. The order of the list, repeated ids and ids the proxy
// does not carry do not matter; 0xffff is no id; an item that cannot be worn is never marked. Returns how many items
// are marked.
inline size_t mark_worn(std::vector<ItemFacts>& items, const std::vector<uint16_t>& worn)
{
	std::vector<uint16_t> ids(worn);
	std::sort(ids.begin(), ids.end());
	size_t marked = 0;
	for (ItemFacts& f : items)
	{
		// T8b review N1: only a wearable item can be worn - a client naming his parts and ammo keeps nothing by it
		f.worn = f.wearable && f.id != 0xffff && std::binary_search(ids.begin(), ids.end(), f.id);
		if (f.worn)
			++marked;
	}
	return marked;
}

// The candidates of pick_losses (its contract: no quest items, a stable order). The caller passes the backpack; the
// items never lost are left out, the rest go by id ascending, each id once, at most kMaxCandidates.
// `layout_known` false: the character is a client's proxy and the host does not know where its player keeps his things
// — an NPC cannot wear an outfit, has no belt and fills its slots in the order it took the items — so whatever the
// player may have on (wearable) stays with him.
// `layout_known` true: the host's own actor (`items` is its exact backpack), or a client's proxy whose owner reported
// what he has on (F3 T8b: `items` is everything the proxy carries, mark_worn has set `worn`); the items marked `worn`
// stay with him, the rest — wearable or not — can be taken.
inline std::vector<Candidate> loss_candidates(const std::vector<ItemFacts>& items, const KindTable& kinds,
	bool layout_known)
{
	std::vector<Candidate> out;
	out.reserve(items.size() < kMaxCandidates ? items.size() : kMaxCandidates);
	for (const ItemFacts& f : items)
	{
		if (never_lost(f) || f.worn || (!layout_known && f.wearable))
			continue;
		out.push_back(Candidate{f.id, category_of(f, kinds)});
	}
	detail::sort_unique_bounded(out, [](const Candidate& c) { return c.id; });
	return out;
}

// What goes into a death stash, by id ascending, each id once, at most kMaxCandidates. `everything` (hardcore, the
// character retires): all he has, worn and quest items included — only helper items and non-world items stay out.
// Otherwise (corpse): the backpack, as loss_candidates.
inline std::vector<uint16_t> stash_ids(const std::vector<ItemFacts>& items, const KindTable& kinds, bool everything,
	bool layout_known)
{
	std::vector<uint16_t> out;
	if (!everything)
	{
		for (const Candidate& c : loss_candidates(items, kinds, layout_known))
			out.push_back(c.id);
		return out;
	}
	for (const ItemFacts& f : items)
		if (f.id != 0xffff && !f.helper && f.world)
			out.push_back(f.id);
	detail::sort_unique_bounded(out, [](uint16_t id) { return id; });
	return out;
}

// The seed of one death's pick (plan T9 step 2): character ^ (seq << 16) ^ the low 32 bits of the game time.
inline uint32_t loss_seed(uint16_t character, uint16_t seq, uint64_t game_time)
{
	return uint32_t(character) ^ (uint32_t(seq) << 16) ^ uint32_t(game_time & 0xffffffffull);
}

struct Point
{
	float x = 0.0f, y = 0.0f, z = 0.0f;
};

struct DropPlace
{
	Point at;
	bool moved = false; // the items are put at `at` (GE_CHANGE_POS); false: they fall from the body
};

constexpr float kBelowVertexM = 1.0f; // a body this far below its level vertex fell through the ground
constexpr float kAboveVertexM = 0.3f; // moved items appear this far above the vertex

// Review Focus 4: dropped items must stay where the player can reach them. They fall from the body, unless its own
// level vertex is not valid, the body is more than kBelowVertexM below that vertex (fell through the ground) or its
// position is not a number; then they go kAboveVertexM above `vertex` — the body's own vertex, or the nearest one when
// the own is not valid.
// `vertex_found` false: no level graph here, nothing better than the body.
inline DropPlace drop_place(const Point& body, bool own_vertex_valid, bool vertex_found, const Point& vertex)
{
	DropPlace d;
	d.at = body;
	if (!vertex_found)
		return d;
	const bool body_ok = std::isfinite(body.x) && std::isfinite(body.y) && std::isfinite(body.z);
	if (own_vertex_valid && body_ok && body.y >= vertex.y - kBelowVertexM)
		return d;
	d.at = vertex;
	d.at.y += kAboveVertexM;
	d.moved = true;
	return d;
}

constexpr float kSpreadMaxM = 0.6f;

// The offset of the i-th moved item around the drop point, so that items put at one point do not start inside each
// other (their physics would throw them apart). 0 for the first, never more than kSpreadMaxM.
inline Point spread(size_t i)
{
	if (i == 0)
		return {};
	// A sunflower: the golden angle between neighbours, the radius growing with sqrt(i) up to kSpreadMaxM, then
	// round again on the outer ring.
	const size_t k = (i - 1) % 24 + 1;
	const float angle = 2.39996323f * float(i);
	const float r = 0.12f * std::sqrt(float(k));
	const float rr = r < kSpreadMaxM ? r : kSpreadMaxM;
	Point p;
	p.x = rr * std::cos(angle);
	p.z = rr * std::sin(angle);
	return p;
}

// Msg::DeathMarker (host -> all): u16 character, u16 marker object, u8 drop mode; host numbering. Only Drop (the
// marker is the first dropped item) and Corpse (the stash) have a marker; Lose leaves nothing.
struct Marker
{
	uint16_t character = 0xffff;
	uint16_t object = 0xffff;
	Drop drop = Drop::Drop;
};

constexpr size_t kMarkerSize = 5;

inline void write(Writer& w, const Marker& m)
{
	w.u16(m.character);
	w.u16(m.object);
	w.u8(static_cast<uint8_t>(m.drop));
}

// False on a short packet, a missing id (0xffff) or a mode without a marker (Lose, unknown); `m` is then unchanged.
inline bool read(Reader& r, Marker& m)
{
	Marker n;
	n.character = r.u16();
	n.object = r.u16();
	const uint8_t d = r.u8();
	if (!r.ok() || n.character == 0xffff || n.object == 0xffff)
		return false;
	if (d != static_cast<uint8_t>(Drop::Drop) && d != static_cast<uint8_t>(Drop::Corpse))
		return false;
	n.drop = static_cast<Drop>(d);
	m = n;
	return true;
}

// The `drop` argument of the Lua callback coop_death_marker: "drop" or "corpse".
inline const char* marker_kind(Drop d)
{
	return d == Drop::Corpse ? "corpse" : "drop";
}

// ---------------------------------------------------------------- a stash waiting for its items (T9 fix round 1)

// Who a death stash is for: taken when it is spawned and checked again before the items move, because the character id
// alone can be given to another character once this one is released.
struct StashOwner
{
	uint32_t peer = 0xffffffffu; // the player who plays the character (a session PeerId, 0 = the host); none: 0xffffffff
	std::string key; // the player key digest on the character's custom data ([coop] key), empty if none
};

// Still the same character. Both sides have a key: the same key (R1). Otherwise the same player still plays it. So a
// hardcore retirement (Task 10) can rewrite the key while its player is still there (review I1: the key is gone, the
// peer stays), and a player who leaves keeps the key on his parked character but not his peer (review minor 1: the key
// stays, the peer is gone).
inline bool same_owner(const StashOwner& then, const StashOwner& now)
{
	// T8b, review R1: two keys decide alone. A different key is another character even when the peer number matches
	// (the id was released and given to a new proxy of a player who took the same number).
	if (!then.key.empty() && !now.key.empty())
		return then.key == now.key;
	// A side without a key (Task 10 took it away, or it never had one): the peer is all there is.
	return then.peer != 0xffffffffu && then.peer == now.peer;
}

// Where an object is on the host. Gone: no entity. Online: its game object is in the level (the stash: as an
// inventory box). Offline: an ALife entity switched off. Between: online in ALife without its game object yet.
enum class Presence : uint8_t
{
	Gone,
	Online,
	Offline,
	Between
};

enum class StashStep : uint8_t
{
	Wait,
	Fill, // both online: GE_TRADE_SELL from the character + GE_TRADE_BUY to the stash
	MoveOffline, // both offline (the player left and nobody is near): ALife detach from the character, attach to the stash
	DropAtBody, // the character is here, the stash did not come online in time or is gone: the items drop at the body
	GiveUp // another character now, the character is gone, or his player stayed away: the items stay, the stash goes
};

constexpr uint64_t kStashWaitMs = 5000; // plan T9 step 3: while the character is here, the stash has this long
constexpr uint64_t kStashAwayWaitMs = 600000; // a player who left: his parked character waits this long for the move

// What a pending stash does this frame. `deadline`: the 5 s of the stash (see stash_presence); `give_up_at`: its
// creation + kStashAwayWaitMs.
inline StashStep stash_step(bool owner_same, Presence character, Presence stash, uint64_t now, uint64_t deadline,
	uint64_t give_up_at)
{
	if (!owner_same || character == Presence::Gone)
		return StashStep::GiveUp;
	if (character == Presence::Online)
	{
		if (stash == Presence::Online)
			return StashStep::Fill;
		return stash == Presence::Gone || now >= deadline ? StashStep::DropAtBody : StashStep::Wait;
	}
	if (character == Presence::Offline && stash == Presence::Offline)
		return StashStep::MoveOffline;
	return stash == Presence::Gone || now >= give_up_at ? StashStep::GiveUp : StashStep::Wait;
}

constexpr float kDropReachM = 3.0f; // Review Focus 4: what fell lies within this distance of the death spot

// Where the items of a stash that did not come online drop (StashStep::DropAtBody). They belong where he died: `died`
// is the place the stash was made for, `now` the drop place of the body as it is, `body` its position. A body within
// kDropReachM of `died` lets them fall as usual (`now`); one farther away (the player left and came back, or was
// already respawned) has them put at `died` — kAboveVertexM up when that was the body's own spot.
inline DropPlace fallback_place(const DropPlace& died, const DropPlace& now, const Point& body)
{
	const float dx = body.x - died.at.x, dy = body.y - died.at.y, dz = body.z - died.at.z;
	const float d2 = dx * dx + dy * dy + dz * dz; // NaN: not within reach
	if (d2 <= kDropReachM * kDropReachM)
		return now;
	DropPlace r = died;
	if (!r.moved)
	{
		r.at.y += kAboveVertexM;
		r.moved = true;
	}
	return r;
}

// The 5 s count while the character is here: when he is back after an absence (he left and returned, or came online
// again) they start again from `now`. `away` is the pending stash's own flag, false at its creation.
inline void stash_presence(uint64_t& deadline, bool& away, Presence character, uint64_t now)
{
	if (character != Presence::Online)
	{
		away = true;
		return;
	}
	if (away)
	{
		away = false;
		deadline = now + kStashWaitMs;
	}
}
} // namespace death
} // namespace coop
