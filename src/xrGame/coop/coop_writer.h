// Anomaly Coop - R32 W1 (temp/r30/design/R32-final.md §1 W1, §8, §8a): who writes an object, as this process sees it. Core: no mode
// check here; the Core finds the object's root from ALife data (xrCoop/coop_writer_rules.h root_of) and asks the mode
// (Mode::writer_of) who writes it. The mode fills what only its own tables know (who leads a map, who holds a unit).
#pragma once

#include "xrCoop/coop_writer_rules.h"

struct lua_State;

namespace coop
{
namespace writing
{
// The root of `id` (this process's numbering) and what it is (writer::kind_of): a player's character (and the ghost, actor 0), a unit
// (a squad, or a living monster in no squad: what the transfers move), a resident of a map (a box, a ground item, a body, a physics
// object, a car, a crow, a phantom), or nothing of a map.
// The ALife server objects are read: on a player's machine (a mirror) the answer is the mode's own (Other: its host) and the
// facts are not needed. An id that is not there: kind None, root = id.
writer::RootFacts root_facts_of(u16 id);

// Is this process the writer of `id` (Mode::writer_of over its root)? Single player and the co-op host: always.
bool writes(u16 id);

// Lua: coop.writes(id) -> bool (the srv and hostplay layers answer coop_mode.writes(id) with it).
int lua_writes(lua_State* L);
} // namespace writing
} // namespace coop
