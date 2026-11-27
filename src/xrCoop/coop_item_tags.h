// Anomaly Coop — which tags of an item state differ between the host copy and the owner's report.
// The host re-applies the owner's state to its copy of a player's item; a copy that still differs afterwards is
// "stuck" and is not forced any more. A tag the owner never carried must not make a copy stuck: the old symmetric
// comparison did exactly that for the scope tag of every weapon without scopes (pistols, knife).
// Pure C++17, header-only, no engine dependencies: used by the engine and by the unit-test exe.
#pragma once

#include <cstddef>
#include <cstdint>

#include "coop_hash.h"
#include "coop_wire.h"

namespace coop
{
namespace itemtags
{
// One bit per tag of the item blob (coop_items.cpp).
enum Bit : std::uint32_t
{
	Cond = 1u << 0,
	Upgrades = 1u << 1,
	Box = 1u << 2,
	Addons = 1u << 3,
	Scope = 1u << 4,
	Main = 1u << 5,
	Launcher = 1u << 6,
	Uses = 1u << 7,
};
constexpr std::uint32_t kAll = Cond | Upgrades | Box | Addons | Scope | Main | Launcher | Uses;

inline const char* name(Bit b)
{
	switch (b)
	{
	case Cond: return "condition";
	case Upgrades: return "upgrades";
	case Box: return "box count";
	case Addons: return "addons";
	case Scope: return "scope";
	case Main: return "magazine";
	case Launcher: return "launcher";
	case Uses: return "portions";
	default: return "?";
	}
}

// The scope tag exists only for a weapon that has scopes to choose from (CWeapon::m_scopes is not empty).
// A weapon without them (pistols, knife) carries no scope tag at all: neither the owner nor the host copy reports one.
inline bool scope_tag_applies(std::size_t scopes)
{
	return scopes != 0;
}

// A scope index from a peer is usable only below the number of scopes the weapon has.
inline bool scope_index_ok(std::uint32_t scope, std::size_t scopes)
{
	return scope < scopes;
}

// Tags of `cur` (the host copy as it is now) that differ from `owner` (the state the owner reported). Only tags the
// owner carries are compared: a state without a tag says nothing about it, so there is nothing to enforce. A tag the
// owner carries and the copy lacks counts as different.
// D is an item state with the fields has_X / X of ItemData (coop_items.cpp); `same_upgrades(a.upgrades, b.upgrades)`
// compares the upgrade lists (the engine strings and the order rules stay with the caller).
template <class D, class SameUpgrades>
std::uint32_t diverged(const D& cur, const D& owner, SameUpgrades&& same_upgrades)
{
	std::uint32_t m = 0;
	if (owner.has_cond && (!cur.has_cond || cur.cond != owner.cond))
		m |= Cond;
	if (owner.has_upgrades && (!cur.has_upgrades || !same_upgrades(cur.upgrades, owner.upgrades)))
		m |= Upgrades;
	if (owner.has_box && (!cur.has_box || cur.box != owner.box))
		m |= Box;
	if (owner.has_addons && (!cur.has_addons || cur.addons != owner.addons))
		m |= Addons;
	if (owner.has_scope && (!cur.has_scope || cur.scope != owner.scope))
		m |= Scope;
	if (owner.has_main && (!cur.has_main || cur.main_count != owner.main_count || cur.main_type != owner.main_type))
		m |= Main;
	if (owner.has_launcher && (!cur.has_launcher || cur.gl_count != owner.gl_count || cur.gl_type != owner.gl_type))
		m |= Launcher;
	if (owner.has_uses && (!cur.has_uses || cur.uses != owner.uses))
		m |= Uses;
	return m;
}

// The host moves an item into a player's
// character and then pushes its own state of it (coop_items.cpp host_push, authority flag): the player's copy may be
// stale (an NPC's magazine, a corpse's condition). By then the owner's own take callbacks have run on it (a mod may pin a
// taken firearm's condition in actor_on_item_take). What the owner changed itself since the take is the owner's and must survive the host's push.

// Tags whose value differs between `now` (the owner's copy) and `base` (the same copy right before the take), whichever
// side carries them.
template <class D, class SameUpgrades>
std::uint32_t changed_since(const D& now, const D& base, SameUpgrades&& same_upgrades)
{
	return diverged(now, base, same_upgrades) | diverged(base, now, same_upgrades);
}

// The tags an owner keeps from its own copy over the host's push. The upgrade list is the host's (a mechanic's sale,
// coop_ledger.h), never the owner's.
constexpr std::uint32_t kOwnerTakeTags = kAll & ~std::uint32_t(Upgrades);

// An item the owner made
// itself under a leased id (a weapon's clone for a scope, a kit, a detach) is the owner's whole. The host builds its copy
// from its own view of the source and pushes it into the owner's hands; that view may be older than the owner's live
// weapon. The tags the owner keeps over the host's push: all of them (but the upgrade list) for an item it made itself,
// the ones its take callbacks changed (`changed_since_take`) for an item the host's take gave it.
inline std::uint32_t push_keeps(bool made_here, std::uint32_t changed_since_take)
{
	return (made_here ? kOwnerTakeTags : changed_since_take) & kOwnerTakeTags;
}

// What the owner's copy becomes when the host's push `host` arrives: the host's tags, except those in `mine` (changed by
// the owner since the take, see changed_since), which stay as `now` has them.
template <class D>
D take_push(D host, const D& now, std::uint32_t mine)
{
	mine &= kOwnerTakeTags;
	if (mine & Cond)
	{
		host.has_cond = now.has_cond;
		host.cond = now.cond;
	}
	if (mine & Box)
	{
		host.has_box = now.has_box;
		host.box = now.box;
	}
	if (mine & Addons)
	{
		host.has_addons = now.has_addons;
		host.addons = now.addons;
	}
	if (mine & Scope)
	{
		host.has_scope = now.has_scope;
		host.scope = now.scope;
	}
	if (mine & Main)
	{
		host.has_main = now.has_main;
		host.main_count = now.main_count;
		host.main_type = now.main_type;
	}
	if (mine & Launcher)
	{
		host.has_launcher = now.has_launcher;
		host.gl_count = now.gl_count;
		host.gl_type = now.gl_type;
	}
	if (mine & Uses)
	{
		host.has_uses = now.has_uses;
		host.uses = now.uses;
	}
	return host;
}

// Only the tags in `mask` of `d`: a partial state. A receiver applies and enforces only the tags a state carries.
template <class D>
D only_tags(D d, std::uint32_t mask)
{
	d.has_cond = d.has_cond && (mask & Cond);
	d.has_upgrades = d.has_upgrades && (mask & Upgrades);
	d.has_box = d.has_box && (mask & Box);
	d.has_addons = d.has_addons && (mask & Addons);
	d.has_scope = d.has_scope && (mask & Scope);
	d.has_main = d.has_main && (mask & Main);
	d.has_launcher = d.has_launcher && (mask & Launcher);
	d.has_uses = d.has_uses && (mask & Uses);
	return d;
}

// The digest of the state of an item nobody alive holds, as the host and a client each see it
// (coop_reconcile.h). Condition in steps of 1/1024: a step apart is a change, a float's last bits are not. The upgrades
// are the host's and a mechanic's business (they go out with ItemState when sold). `with_uses`: portions live in the
// game object only, so they count when the item is online at both ends.
// D is an item state with the fields has_X / X of ItemData (coop_items.cpp).
template <class D>
std::uint32_t state_digest(const D& d, bool with_uses)
{
	std::uint32_t h = kFnv32Basis;
	const auto mix = [&h](std::uint32_t v) { h = fnv1a32_step(h, v); };
	mix(d.has_cond ? (std::uint32_t(d.cond) + 32u) >> 6 : 0xFFFFu);
	mix(d.has_box ? d.box : 0xFFFFu);
	mix(d.has_addons ? d.addons : 0xFFFFu);
	mix(d.has_main ? (std::uint32_t(d.main_count) << 8) | d.main_type : 0xFFFFFFFFu);
	if (with_uses)
		mix(d.has_uses ? d.uses : 0xFFFFu);
	return h | 1u; // never 0: a row without a state
}

// ---------------------------------------------------------------- a version per tagged field

// The decider of an item (its writer, coop_writer_rules.h) changes a field on purpose: a script's set_condition or
// set_remaining_uses, an upgrade it installs. The owner (the player whose character holds the item) reports the state his game has.
// The newer report alone cannot decide: a report the owner made before he had applied the decider's debit would restore it, and
// a push of the decider could undo the owner's own eating. So every tagged field has a version: the decider moves it
// on at each deliberate write (bump) and pushes it with the value; the owner applies the push and keeps its version (take), and
// reports the versions with the fields. The decider takes a field of a report only when the report was made on the latest version
// of that field (fresh): the owner's own change on top of the latest write counts, a stale report never undoes a write.
// Version 0: no deliberate write yet (an item as it was made).
constexpr std::size_t kTagCount = 8; // Cond .. Uses: one version each
static_assert(kAll == 0xFFu, "one version per Bit: the u8 mask of write_versions names every tag");

// The index of the version of `bit` (a single Bit); kTagCount for 0, for several bits, for a bit that is no tag.
inline std::size_t tag_index(std::uint32_t bit)
{
	for (std::size_t i = 0; i < kTagCount; ++i)
		if (bit == (1u << i))
			return i;
	return kTagCount;
}

struct FieldVersions
{
	std::uint16_t of[kTagCount] = {};

	std::uint16_t get(Bit b) const
	{
		const std::size_t i = tag_index(b);
		return i < kTagCount ? of[i] : std::uint16_t(0);
	}
	void set(Bit b, std::uint16_t v)
	{
		const std::size_t i = tag_index(b);
		if (i < kTagCount)
			of[i] = v;
	}
	bool operator==(const FieldVersions& o) const
	{
		for (std::size_t i = 0; i < kTagCount; ++i)
			if (of[i] != o.of[i])
				return false;
		return true;
	}
	bool operator!=(const FieldVersions& o) const { return !(*this == o); }
};

// The version after `v`. 0 stays "never written": after 65535 comes 1.
inline std::uint16_t next_version(std::uint16_t v) { return v == 0xFFFFu ? std::uint16_t(1) : std::uint16_t(v + 1); }

// The decider wrote the tags in `mask` on purpose: their versions move on.
inline void bump(FieldVersions& v, std::uint32_t mask)
{
	for (std::size_t i = 0; i < kTagCount; ++i)
		if (mask & (1u << i))
			v.of[i] = next_version(v.of[i]);
}

// The owner applied a push of the tags in `mask`: his versions of them are the push's.
inline void take(FieldVersions& applied, const FieldVersions& push, std::uint32_t mask)
{
	for (std::size_t i = 0; i < kTagCount; ++i)
		if (mask & (1u << i))
			applied.of[i] = push.of[i];
}

// The tags of `mask` (those an owner's report carries) made on the decider's latest version: the ones the decider applies. A
// version is compared, not ordered (it wraps).
inline std::uint32_t fresh(const FieldVersions& latest, const FieldVersions& report, std::uint32_t mask)
{
	std::uint32_t out = 0;
	for (std::size_t i = 0; i < kTagCount; ++i)
		if ((mask & (1u << i)) && latest.of[i] == report.of[i])
			out |= 1u << i;
	return out;
}

// On the wire: a tag of the ItemState blob (coop_items.cpp Tag; 1 .. 11 are taken), u8 tag, varu len, payload, so a reader
// skips a tag it does not know by its length (read_data's default). Payload: u8 mask (the tags whose version follows), then a u16 per bit of
// the mask, lowest bit first.
constexpr std::uint8_t kVersionsTag = 12;
// Two more tags:
// 13 the decider's own change of the portions in the push of its latest version of them: i16 (coop_owner_rules.h counters);
// 14 the stock client_data of a held eatable as the writer's game object saved it (the CGameObject::net_Save body, at most
//    initial::kMaxClientData bytes, coop_initial_rules.h): a map server's ItemFacts, so that a world with no object stores the
//    binder's bytes too.
constexpr std::uint8_t kUsesDeltaTag = 13;
constexpr std::uint8_t kClientDataTag = 14;

inline void write_versions(Writer& w, const FieldVersions& v, std::uint32_t mask)
{
	mask &= kAll;
	w.u8(std::uint8_t(mask));
	for (std::size_t i = 0; i < kTagCount; ++i)
		if (mask & (1u << i))
			w.u16(v.of[i]);
}

// False (`v` and `mask` cleared) for a cut payload. What follows the versions of the mask is the caller's to refuse, as for every
// reader of the wire; a tag that is not in the mask reads as version 0.
inline bool read_versions(Reader& r, FieldVersions& v, std::uint32_t& mask)
{
	v = FieldVersions();
	mask = 0;
	FieldVersions tmp;
	const std::uint32_t m = r.u8();
	for (std::size_t i = 0; i < kTagCount && r.ok(); ++i)
		if (m & (1u << i))
			tmp.of[i] = r.u16();
	if (!r.ok())
		return false;
	v = tmp;
	mask = m;
	return true;
}
} // namespace itemtags
} // namespace coop
