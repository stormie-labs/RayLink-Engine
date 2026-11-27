// Anomaly Coop - the initial state of an item rides its create. Core: no mode check here; whether this process is the item's
// decider or its owner is asked of the item engine (coop_items.h item_versions).
// The wire layout and the stock client_data are pure rules (xrCoop/coop_initial_rules.h).
//
// write_initial: the uses, the condition, the ammo count and the decider's field versions of item `e` as this process has them (the
// game object here, else the entity and its client_data). A non-item writes an empty state (one byte).
// read_initial: the same into the entity `e` of a create that has no game object yet: the condition and the ammo count into the
// entity, the uses into its client_data (the object that comes online reads them as a stock save), or, for a scripted section with no
// client_data yet (its binder's bytes cannot be made here), staged for its next game object here (coop_items.h stage_uses), the
// versions into the item engine (they travel with the item). Uses only for an eatable section (is_eatable_section), never
// read from or written into another item's client_data. False: a malformed tail (nothing applied).
// Wired: EntitySpawn's tail carries it after the clone's replacement state. coop_entities.cpp write_spawn (the world's
// spawns and repairs) and write_entity (a map server's journal Spawn) write it; client_on_spawn reads it whole, applies it to a new
// entity before register_object, and to a known one only where this process is not its writer and has no game object for it
// (reconcile::repeat_takes_state: a repeat leaves the writer's own state alone, and a game object's state stands, so its versions are
// not taken without it). tests/test_initial_rules.cpp initial_state_rides_the_entity_spawn_tail.
#pragma once

#include "xrCoop/coop_initial_rules.h"
#include "xrCoop/coop_wire.h"

class CSE_Abstract;
struct lua_State;

namespace coop
{
namespace items
{
bool write_initial(Writer& w, const CSE_Abstract* e);
bool read_initial(Reader& r, CSE_Abstract* e);

// The item's initial state, set server side by its writer: with a game object here, through it (a deliberate write: coop_items.h
// decider_wrote); with none, into the entity. §8a A21 E-2: also by the creator of this frame's create in a process that does not write
// the item (no game object here, its spawn not sent yet: entities::host_own_spawn_unsent), into the entity, versions not taken; the
// EntitySpawn tail carries it to the writer. False: no such item, not this process's to write ("not written here"), or a value out
// of range (`why`).
bool set_initial(u16 id, const initial::State& s, const char*& why);

// coop.items.set_initial(id, t) (Core; t = { uses = n, condition = 0..1, ammo = n }) -> true, or false and why. Registered by
// coop_script.cpp.
void register_lua(lua_State* L);
} // namespace items
} // namespace coop
