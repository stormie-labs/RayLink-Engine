// Anomaly Coop: when a player's machine may start a throw, and what a save does
// with a thrown hand item the authority has not yet removed. Pure rules, no engine types (tests/test_throw_rules.cpp).
//
// A throw of an item whose create is still pending (its leased id not confirmed by the authority) cannot be made:
// the authority does not have the item yet and refuses the throw, and the hand item's own destroy, held by the
// lease rule (coop_requests.cpp) until the create is confirmed, would then use it up with no throw. So the throw
// does not start while the lease waits; the item stays in hand and the player can throw it a moment later. Nothing is
// dropped: no request is sent, nothing is destroyed.
//
// A thrown hand item is removed by its owner; the authority keeps a 3 s record and removes it itself if the owner did
// not (coop_objects.cpp g_consume). A save made inside that window must not keep the item: the throw already happened
// and the thrown copy is never saved. So a save first applies every pending removal (flush), and a world load that
// clears the record cannot bring the item back.
#pragma once

namespace coop
{
namespace throws
{
// May this machine start a throw of this item now?
//  mirror        - this process is a player's machine (Coop client, Server-mode player)
//  lease_waiting - the item's create is still pending on this machine (requests::client_lease_waiting)
inline bool may_start(bool mirror, bool lease_waiting)
{
	return !(mirror && lease_waiting);
}

// What a pending removal record does when the authority writes a save (or when its time is up).
enum class ConsumeAt
{
	Keep, // not due yet and no save: wait for the owner's own destroy
	Remove // remove the thrown hand item now
};

//  saving      - the authority is about to write a save
//  due         - the record's 3 s are over
//  same_entity - the entity under the id is still the thrown one (same pointer and section: an id can be reused)
inline ConsumeAt consume_at(bool saving, bool due, bool same_entity)
{
	if (!same_entity)
		return ConsumeAt::Keep; // nothing of the throw is left under this id: the record just goes
	return (saving || due) ? ConsumeAt::Remove : ConsumeAt::Keep;
}
} // namespace throws
} // namespace coop
