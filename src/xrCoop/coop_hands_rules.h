// Anomaly Coop: what a puppet's hands show on a player's machine. Pure rules, no engine types, no mode checks
// (tests/test_hands_rules.cpp); coop_entities.cpp apply_creature_flags feeds them from a snapshot and the puppet here.
//
// The case: an NPC at a campfire plays the guitar on the host, and a co-op player sees a pistol in its hand. Every guitar
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
// hide start and the swap of a held item begin, and still shows the right hands soon.
constexpr uint32_t kHideGraceMs = 600;

// A wait of the copy's hands, for the "[coop-hands]" lines and the counts of coop_status (coop_entities.cpp
// apply_creature_flags). A check is one look at the copy's hands against the host's word; a wait not done asks for its
// next check a retry later (100 ms; 1 s while a torch is not here either), and that check comes with the copy's next
// client update (UpdateCL). The engine gives a copy that update
//  - every frame while it is a "crow": within 30 m of the camera, or rendered within 60 m (CObject::UpdateCL,
//    CROW_RADIUS and CROW_RADIUS2);
//  - else once per scheduler step of the copy (CObject::shedule_Update makes it a crow for one update): 100 ms at the
//    camera, 178 ms at 30 m, 493 ms at 150 m, 625 ms from 200 m on (CSheduler::ProcessStep with a creature's t_min 100
//    and t_max 250: 100 + 525 * distance / 200, at most (1000 + 250) / 2). The distance is the camera's: a process that
//    only mirrors the world keeps no viewers list (xrCoop/coop_viewers.h);
//  - later than that when the scheduler runs short: each frame it serves the objects that are due, oldest due first, up
//    to its time budget (psShedulerCurrent, 3-66 ms), so while frames are long or the due objects are costly (a level
//    full of NPCs loading around a client that has just arrived) a copy that is not a crow waits several frames for
//    its turn. Without its update the copy is neither placed nor animated, and its hands stay as they were.
// The copy's inventory moves in that same scheduler step (CInventoryOwner::UpdateInventoryOwner), so a show or a swap
// takes a number of the copy's own steps, not of frames: the planner asks for the slot in one update
// (CObjectActionShow, CObjectActionHide), the inventory changes it in the next step, the item's new state follows. A swap
// through the put-away is four steps after the check that began the wait (the put-away, the slot cleared, the held
// item's slot, its state): 2.5 s for a copy 200 m away.
//
// So a wait is counted in the copy's own time: the gaps between its checks, each as no more than kWaitStepMs, the
// slowest step a copy has while its scheduler keeps up. A longer gap is one step whatever its length: a frame of
// seconds, or the copy's turn in the scheduler coming late. A wait is stuck at kStuckWaitSteps such steps, which means
// the same for a near and a far copy: 5 s of running frames, in which the farthest copy has had 8 steps of its own,
// twice what a swap costs it, and a nearer one more. A copy that never gets there (a hide waiting for an animation
// callback a copy never gets) passes the mark at any distance; one checked once a second (a torch not here) in 8 s. The
// put-away comes first (kHideGraceMs), so the remedy has had its chance.
//
// A check later than one slowest step after it was due has one of two causes, told apart by the frames of its gap:
//  - a frame stall: the frames of the gap took a slowest step or longer each on average (a frame of seconds), so no
//    update of the copy could have come sooner;
//  - starvation: shorter frames ran and the scheduler served the copy none of them (it ran short, as above).
// Neither is a fault of the hands code: until that check the copy stood as it was.
constexpr uint32_t kWaitStepMs = 625;
constexpr uint32_t kStuckWaitSteps = 8;
constexpr uint32_t kStuckWaitMs = kStuckWaitSteps * kWaitStepMs;

// One wait, as its checks count it.
struct Wait
{
	uint32_t checks = 0; // checks of this wait: the one that began it and the one that ended it among them
	uint32_t own_ms = 0; // the gaps between them, each counted as at most kWaitStepMs
	uint32_t late_ms = 0; // the latest a check came after it was due (its gap less the retry asked for)
	uint32_t late_frames = 0; // frames that ran in that gap: one is a long frame, many is the copy waiting for its turn
	uint32_t stall_ms = 0; // the latest a check came more than a step late across a frame stall
	uint32_t starved_ms = 0; // the latest a check came more than a step late while frames ran without the copy's update
	uint32_t wrong_ms = 0; // the longest an item the host does not hold was seen in the hands (0: empty or hidden)
};

// One check of a wait. gap_ms and frames: since the check before it; due_ms: the retry asked for after that check;
// ms_wrong_item_visible: Seen's. The check that begins a wait has no gap: nothing of it is counted.
inline void wait_check(Wait& w, uint32_t gap_ms, uint32_t due_ms, uint32_t frames, uint32_t ms_wrong_item_visible)
{
	if (w.checks++)
	{
		w.own_ms += gap_ms < kWaitStepMs ? gap_ms : kWaitStepMs;
		const uint32_t late = gap_ms > due_ms ? gap_ms - due_ms : 0u;
		if (late >= w.late_ms)
		{
			w.late_ms = late;
			w.late_frames = frames;
		}
		if (late > kWaitStepMs)
		{
			const bool stall = uint64_t(frames) * kWaitStepMs <= gap_ms;
			uint32_t& kind = stall ? w.stall_ms : w.starved_ms;
			if (late > kind)
				kind = late;
		}
	}
	if (ms_wrong_item_visible > w.wrong_ms)
		w.wrong_ms = ms_wrong_item_visible;
}

// Slow: longer in the wall clock than the put-away grace. Its end is a plain line and a count, whatever made it long.
inline bool wait_is_slow(uint32_t ms_waiting) { return ms_waiting >= kHideGraceMs; }

// Stuck: the error line. The copy had kStuckWaitSteps steps of its own and its hands are still not the host's.
inline bool wait_is_stuck(const Wait& w) { return w.own_ms >= kStuckWaitMs; }

// A check of this wait came more than a step late across a frame stall.
inline bool wait_had_stall(const Wait& w) { return w.stall_ms != 0; }

// A check of this wait came more than a step late while frames ran: the copy was left without its own updates.
inline bool wait_was_starved(const Wait& w) { return w.starved_ms != 0; }

// How long a copy showed its hands as its spawn or its last track left them before its first snapshot here: from the
// first time it was driven without a track (`driven_trackless_at`; 0: it never was, a snapshot always came first) to
// that snapshot. Its appearance here (`appeared_at`) bounds it, so a time left from an earlier object of the same id
// counts nothing of it. A track that goes (an offline switch, a reparent, the client going live) and comes again starts
// a new wait here, not one from the copy's first appearance.
inline uint32_t first_snapshot_wait_ms(uint64_t appeared_at, uint64_t driven_trackless_at, uint64_t first_sample_at)
{
	if (!driven_trackless_at)
		return 0u;
	const uint64_t from = driven_trackless_at > appeared_at ? driven_trackless_at : appeared_at;
	if (first_sample_at <= from)
		return 0u;
	const uint64_t ms = first_sample_at - from;
	return ms < 0xffffffffull ? uint32_t(ms) : 0xffffffffu;
}

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
