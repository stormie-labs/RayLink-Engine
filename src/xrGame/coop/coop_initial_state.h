// Anomaly Coop - TZ R32 W8.4 (temp/r30/design/R32-final.md §1 W8.4, §8): the initial state of an item rides its create. Core: no mode
// check here; whether this process is the item's decider or its owner is asked of the item engine (coop_items.h item_versions).
// The wire layout and the stock client_data are pure rules (xrCoop/coop_initial_rules.h).
//
// write_initial: the uses, the condition, the ammo count and the decider's field versions of item `e` as this process has them (the
// game object here, else the entity and its client_data). A non-item writes an empty state (one byte).
// read_initial: the same into the entity `e` of a create that has no game object yet: the condition and the ammo count into the
// entity, the uses into its client_data (the object that comes online reads them as a stock save), or, for a scripted section with no
// client_data yet (its binder's bytes cannot be made here), staged for its next game object here (coop_items.h stage_uses), the
// versions into the item engine (§8a A12: they travel with the item). Uses only for an eatable section (is_eatable_section), never
// read from or written into another item's client_data. False: a malformed tail (nothing applied).
// NOT WIRED: no caller yet. EntitySpawn's tail (coop_entities.cpp write_spawn / its reader) is to carry it after the clone's
// replacement state (the lead wires it; tests/test_initial_rules.cpp initial_state_spawn_tail_is_not_wired_yet trips then).
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
// decider_wrote); with none, into the entity. False: no such item, not this process's to write, or a value out of range (`why`).
bool set_initial(u16 id, const initial::State& s, const char*& why);

// coop.items.set_initial(id, t) (Core; t = { uses = n, condition = 0..1, ammo = n }) -> true, or false and why. Registered by
// coop_script.cpp.
void register_lua(lua_State* L);
} // namespace items
} // namespace coop
