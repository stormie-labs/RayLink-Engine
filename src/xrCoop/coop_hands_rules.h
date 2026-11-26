// Anomaly Coop (TZ R71): what a puppet's hands show on a player's machine. Pure rules, no engine types, no mode checks
// (tests/test_hands_rules.cpp); coop_entities.cpp apply_creature_flags feeds them from a snapshot and the puppet here.
//
// Live: an NPC at a campfire plays the guitar on the host, and a co-op player sees a pistol in its hand. Every guitar
// state asks the weapon "strapped" (state_lib): a strappable best weapon goes on the back, a pistol (no strap_bone0) is
// hidden. The host's snapshot says Holds + Strapped with the held item's id, or no Holds. A puppet runs no AI and gets
// no torso animation callbacks (it plays the host's motions), so its object handler's planner cannot end what waits
// for one; the copy sets the planner's world itself:
//  - eWorldPropertyStrapped is ONE property for every weapon of the stalker (object_handler_planner_weapon.cpp
//    CObjectPropertyEvaluatorMember on m_storage). Forced true while another item is still active (the pistol of a
//    moment ago), every weapon counts as strapped: Hide needs it false, and the one operator left, Unstrapping, waits for
//    an animation callback forever (A). So it is true only when the item the host straps is the active one here, shown
//    and strappable; until then the planner swaps the items with it false (hide the pistol, show the rifle).
//  - No Holds: the planner hides the active item itself (Activate(NO_ACTIVE_SLOT) and the hide motion's timer). If an
//    item is still seen in the hands a grace later, it is put away directly (B). The same net holds for A: an item the
//    host does not hold, still seen a grace later, is put away; the planner then shows the held one.
#pragma once

#include <cstdint>

namespace coop
{
namespace hands
{
constexpr uint16_t kNoId = 0xffff;

// How long an active item the host does not hold may stay visible before the copy puts it away itself. The host clears
// Holds only after its own hide ended (creature_flags reads ActiveItem, cleared once the item is hidden), and the
// puppet's body already played that hide motion, so the copy's own hide timer is only lag; 0.6 s lets the planner's
// hide start and the swap of a held item begin, and still shows the right hands soon (review of R71, 2026-11-24).
constexpr uint32_t kHideGraceMs = 600;

// The host's word (the newest snapshot) and the puppet's active item here.
struct Seen
{
	bool want_holds = false; // CreatureHolds: the host has an active item
	bool want_strapped = false; // CreatureStrapped: that item is a weapon on the back
	uint16_t held_id = kNoId; // the held item's local id (kNoId without Holds)
	uint16_t active_id = kNoId; // the puppet's active item here (kNoId: none)
	bool active_hidden = false; // its HUD state is hidden (between its hide and the slot change, or before its show)
	bool active_can_strap = false; // a weapon with strap bones (CWeapon::can_be_strapped)
	bool held_can_strap = false; // the held item has strap bones
	bool held_planned = true; // the object planner can put the held item in hands (a weapon or a missile)
	uint32_t ms_wrong_item_visible = 0; // the caller's clock: an active item visible that is not the one the host holds
};

struct Decision
{
	bool strap = false; // the planner's eWorldPropertyStrapped (Strapped2Idle stays false)
	bool put_away = false; // hide the active item now, not waiting for the planner's hide
	bool done = false; // the hands show what the host's do
};

// Is the puppet's active item one the host does not hold (any shown item with no Holds, another item with Holds)? The
// caller runs its clock (Seen::ms_wrong_item_visible) on this.
inline bool wrong_item_shown(const Seen& s)
{
	const bool shown = s.active_id != kNoId && !s.active_hidden;
	const bool holds_it = s.want_holds && s.held_id != kNoId && s.active_id == s.held_id;
	return shown && !holds_it;
}

inline Decision decide(const Seen& s)
{
	Decision d;
	const bool shown = s.active_id != kNoId && !s.active_hidden;
	const bool holds_it = s.want_holds && s.held_id != kNoId && s.active_id == s.held_id;
	const bool wrong = wrong_item_shown(s);
	// Strapped without Holds (malformed) straps nothing; a non-strappable held item stays in the hands. With nothing
	// active a strappable held item is strapped at once: it then shows straight on the back (no other weapon is active
	// to get stuck as "strapped").
	const bool held_on_back = s.want_holds && s.want_strapped && s.held_id != kNoId;
	d.strap = held_on_back && ((holds_it && shown && s.active_can_strap) || (s.active_id == kNoId && s.held_can_strap));
	// A wrong item still seen a grace later is put away, with Holds too (A: the pistol while the host straps a rifle);
	// it never fights the host, who holds another item or none.
	d.put_away = wrong && s.ms_wrong_item_visible >= kHideGraceMs;
	// A held item the object planner cannot put in hands (only weapons and missiles have its operators) never becomes
	// the active one: the hands are right once nothing wrong is seen.
	if (!s.want_holds)
		d.done = !shown;
	else if (s.held_id == kNoId)
		d.done = false;
	else
		d.done = s.held_planned ? (holds_it && shown) : !wrong;
	return d;
}
} // namespace hands
} // namespace coop
