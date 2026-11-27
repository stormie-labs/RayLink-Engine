// Anomaly Coop - a holder that hears "taken out" (GE_OWNERSHIP_REJECT / GE_TRADE_SELL, or GE_DESTROY_REJECT before a
// child's destroy) of an item it does not hold.
//
// In single player every child of an online holder is a game object, listed by its holder and attached to it, so the stock
// handlers never asked. A co-op client applies the decider's moves and destroys through its local server (coop_events.cpp
// step_parent, SLS_Clear at net_Stop), and there a child may be no game object (its spawn was dropped while the level was not
// ready, or its entity is gone), or a game object its holder never took (the take reached the holder before it existed).
// The stock handlers then broke:
//  - CInventoryBox::OnEvent erased end() of its id list: VERIFY is debug-only, a release build memmoves -2 bytes
//    (access violation, RL-QHQ3-2TUG, RL-NHT6-8SZ3, RL-29NA-4ZS6), and detached an item another holder had;
//  - CAI_Trader::OnEvent read a null item (RL-WKWE-CPCY, during net_Stop), and DropItem of an item of another inventory
//    wipes that inventory's pointer and detaches the item from its real holder;
//  - CLevel::cl_Process_Event told the holder of a GE_DESTROY_REJECT even when the child was not found;
//  - CInventoryItem::net_Destroy only VERIFYed (debug) that the item left its holder's inventory: a release build kept a
//    dangling entry, and scripts got a nil object from IterateRuck.
// Both modes run this code unchanged, and so does single player: a co-op client and a player's client of a map server apply
// the decider's moves the same way; the co-op host, the map server and the world server keep their children consistent,
// where every rule below gives the stock answer.
// Pure C++17, header-only, no engine dependencies: used by the engine and by the unit-test exe.
#pragma once

namespace coop
{
namespace container
{
// A holder with an inventory (a trader: CInventory::DropItem takes the item out of the lists and off the holder together)
// applies "taken out" only of an item that is a game object here and that its inventory lists.
inline bool reject_applies(bool child_found, bool listed)
{
	return child_found && listed;
}

// What CInventoryBox does with "taken out". A box keeps ids (m_items) and attaches game objects (H_Parent) separately.
struct BoxStep
{
	bool unlist; // erase the id from m_items (a stale id of a gone object goes too: the UI would get a null item)
	bool detach; // H_SetParent(NULL): only its own child, never another holder's; the take callback only then
	bool note;   // one plain log line: anything but the stock case
};

inline BoxStep box_reject(bool child_found, bool listed, bool held_by_box)
{
	BoxStep step;
	step.unlist = listed;
	step.detach = child_found && held_by_box;
	step.note = !(reject_applies(child_found, listed) && held_by_box);
	return step;
}

// What CInventoryItem::net_Destroy does with the inventory it still names. owner_alive: that inventory's owner is still a
// registered object of the level (an owner that went first is freed, its inventory is never read). listed: the inventory
// still has the item in its list, backpack, belt or a slot. A slot only the inventory's own code clears (CInventory keeps
// it protected, and DropItem from a destroy would run the drop callbacks of a level that may be unloading): the item leaves
// the lists, and keeps naming the inventory its slot belongs to.
struct Leftover
{
	bool unlist; // erase the item from m_all, m_ruck, m_belt; weight and state of the inventory counted again
	bool forget; // m_pInventory = NULL, as DropItem does
	bool note;   // one plain log line
};

inline Leftover leftover_on_destroy(bool owner_alive, bool listed, bool in_slot)
{
	Leftover left;
	left.unlist = owner_alive && listed;
	left.forget = left.unlist && !in_slot;
	left.note = left.unlist;
	return left;
}
} // namespace container
} // namespace coop
