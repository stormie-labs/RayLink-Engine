// Anomaly Coop - the host's rules for the two sides of a client's item move (Rulings F2-32, F2-32b; F2 T12 round 1).
// coop_requests.cpp host_move checks the request first (the item exists, it is where the client thinks, the requester
// owns one side, never another player's pockets, the destination is a container, reach) and then these rules on the
// two sides it fills from the game:
//   - a live NPC's inventory, out of it or into it (Ruling F2-32b), only while the requesting player holds the talk
//     lock of that NPC (a mechanic returning a repaired item, a dialog handing an item over) or when the NPC is that
//     player's own companion (its squad is led by his character: the stock companion inventory opens without a
//     dialog, axr_companions). With Ruling F2-30 a client may move an item out of another's inventory to its own
//     actor; without this rule it could take anything from a live trader in reach. Another player's companion and a
//     stranger without a talk are refused;
//   - box and corpse locks as the stock game keeps them in its UI: a closed box (inv_box_closed) or corpse
//     (deadbody_closed) neither gives nor takes anything (ActorInput.cpp does not open it); can_take = false
//     (inv_box_can_take, deadbody_can_take) refuses items put into it, taking out stays allowed
//     (UIActorMenuDeadBodySearch.cpp ToDeadBodyBag). Stock scripts can bypass the UI; the host cannot tell a script
//     from the UI, so a script move against a lock is refused too (visible in the host's log, allowed one by one).
// Our own inventory, the ground and other containers add nothing here.
// Pure C++17, header-only, no engine dependencies: used by the engine and the unit-test exe.
#pragma once

#include <cstdint>

namespace coop
{
namespace moves
{
// One side of a move: the inventory the item leaves (source) or goes to (destination), by its top owner.
struct Side
{
	enum Kind : uint8_t
	{
		World,   // the ground (no parent)
		Mine,    // the requesting player's character or something under it
		Player,  // another player's character
		LiveNpc, // a living NPC (stalker, trader, mechanic, companion)
		Corpse,  // a dead creature with an inventory
		Box,     // an inventory box (stash, container)
		Other    // anything else
	};
	Kind kind = World;
	bool closed = false;   // Box: inv_box_closed; Corpse: deadbody_closed
	bool can_take = true;  // Box: inv_box_can_take; Corpse: deadbody_can_take (may items be put into it)
	bool talking = false;   // LiveNpc: the requesting player holds the talk lock of this NPC now
	bool companion = false; // LiveNpc: the NPC is the requesting player's companion (its squad is led by his character)
};

enum class Verdict : uint8_t
{
	Ok,
	NotOwner, // another player's pockets, or a live NPC's without a talk (and not one's own companion)
	Locked    // a closed box or corpse, or one that takes no items
};

inline const char* verdict_text(Verdict v)
{
	switch (v)
	{
	case Verdict::Ok: return "ok";
	case Verdict::NotOwner: return "another player's pockets, or a live NPC's without a talk (not one's companion)";
	case Verdict::Locked: return "a box or corpse lock (closed, or it takes no items)";
	}
	return "ok";
}

inline bool locked_kind(Side::Kind k) { return k == Side::Box || k == Side::Corpse; }

// A live NPC's inventory is open to the requesting player only in a talk or as his own companion.
inline bool closed_npc(const Side& s) { return s.kind == Side::LiveNpc && !s.talking && !s.companion; }

// The verdict for a move from `src` to `dst`.
inline Verdict check(const Side& src, const Side& dst)
{
	if (src.kind == Side::Player || closed_npc(src) || closed_npc(dst))
		return Verdict::NotOwner;
	if (locked_kind(src.kind) && src.closed)
		return Verdict::Locked;
	if (locked_kind(dst.kind) && (dst.closed || !dst.can_take))
		return Verdict::Locked;
	return Verdict::Ok;
}
} // namespace moves
} // namespace coop
