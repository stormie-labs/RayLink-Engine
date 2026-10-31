// Anomaly Coop — which tags of an item state differ between the host copy and the owner's report (R3 В1).
// The host re-applies the owner's state to its copy of a player's item; a copy that still differs afterwards is
// "stuck" and is not forced any more. A tag the owner never carried must not make a copy stuck: the old symmetric
// comparison did exactly that for the scope tag of every weapon without scopes (pistols, knife).
// Pure C++17, header-only, no engine dependencies: used by the engine and by the unit-test exe.
#pragma once

#include <cstddef>
#include <cstdint>

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

// Parity audit F1: the digest of the state of an item nobody alive holds, as the host and a client each see it
// (coop_reconcile.h). Condition in steps of 1/1024: a step apart is a change, a float's last bits are not. The upgrades
// are the host's and a mechanic's business (they go out with ItemState when sold). `with_uses`: portions live in the
// game object only, so they count when the item is online at both ends.
// D is an item state with the fields has_X / X of ItemData (coop_items.cpp).
template <class D>
std::uint32_t state_digest(const D& d, bool with_uses)
{
	std::uint32_t h = 2166136261u;
	const auto mix = [&h](std::uint32_t v) { h = (h ^ v) * 16777619u; };
	mix(d.has_cond ? (std::uint32_t(d.cond) + 32u) >> 6 : 0xFFFFu);
	mix(d.has_box ? d.box : 0xFFFFu);
	mix(d.has_addons ? d.addons : 0xFFFFu);
	mix(d.has_main ? (std::uint32_t(d.main_count) << 8) | d.main_type : 0xFFFFFFFFu);
	if (with_uses)
		mix(d.has_uses ? d.uses : 0xFFFFu);
	return h | 1u; // never 0: a row without a state
}
} // namespace itemtags
} // namespace coop
