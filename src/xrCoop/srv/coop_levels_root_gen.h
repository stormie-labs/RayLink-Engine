// Сервер: только сервер мира и серверы карт (docs/MODES.md)
// Anomaly Coop — the generation at the root of an object (TZ Р32 A6, A16; Р68-3, Р68-2). A per-object entry of a map server's
// journal names it in its head (journal::ObjectHead::gen), the world server checks it against its own (accept::identity). Both
// sides take it from their own record of the sender's hold of that root: the map server from its Holder (srv/coop_levels_xfer.cpp
// held_gen_of), the world from its Keeper for that sender (srv/coop_levels_alife.cpp same_as, Keeper::gen_held_by). Both find the
// root and its kind with one function, writing::root_facts_of (a member counts as its squad). One rule for both, so a test catches
// the sides drifting apart (tests/test_srv_levels_root_gen.cpp).
// Pure C++17, header-only: the engine and the unit-test exe.
#pragma once

#include <cstdint>

#include "../coop_writer_rules.h"

namespace coop
{
namespace levels
{
// The gen at the root of an object, as one side names it: `kind` is the root as that side sees it now (writing::root_facts_of:
// writer::kind_of of the root itself, the squad of a member), `held_gen` the gen its record says the sender holds that root under
// (0: it does not hold it).
//
// The hold decides, not the kind. A unit that dies on the map server that holds it stays held: no Joined, no release ends the hold
// (a body does not transfer), the map server's Holder keeps it and the world keeps its mark at that server. The map server names the
// body and its loot with the hold's gen, and so must the world: it gave 0 to every root that was no living unit and refused them all
// as "another object" (Р68-3, #1743-#1752). The two sides also see the kind at different moments: the map server writes a Died
// after the stock on_death (a resident to it), the world checks it while the creature still lives there (a unit), so a rule by kind
// would split them on the Died itself. Where the hold ends both sides end it together (Joined: Holder::forget, Keeper::drop_own; a
// release: Releasing names the old gen on both, A16), and a root nobody holds is 0 on both.
// A player's character is never held (the world never grants one: host_may_give; adopts_at_left takes none): 0, as before.
constexpr uint32_t root_gen(writer::RootKind kind, uint32_t held_gen)
{
	return kind == writer::RootKind::Character ? 0 : held_gen;
}
} // namespace levels
} // namespace coop
