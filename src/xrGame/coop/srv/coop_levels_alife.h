// Сервер: только сервер мира и серверы карт (docs/MODES.md)
// Anomaly Coop — the world server applies what a map server reports (TZ Р32, temp/r30/design/R32-final.md §1 W2b, W3; §8 the
// frozen interface). The rest of coop_levels_alife.cpp is declared in coop_levels.h.
#pragma once

#include "xrCoop/coop_protocol.h"
#include "xrCoop/srv/coop_levels_wire.h"

#include <cstdint>
#include <string>

namespace coop
{
namespace levels
{
// R32 W7.2 (§8a A21 E-1; srv/coop_levels_xfer.cpp): a map server's numbered op ran here with `answer` (coop_main.encode of the executor's
// answer, scripts\coop_ops_done.script): a Done entry {origin, seq, answer} queued after the facts of the op's effect its hooks queued
// so far, so the frame's journal batch carries both (Lua srv.journal_done). False, nothing queued, on any other process, for a number or
// an answer journal::done::valid refuses, before this map server's holder of its world is made and once its hand-over is over.
bool journal_done(uint32_t origin, uint32_t seq, const std::string& answer);

// What became of one entry of a map server's journal:
//   Applied  the world took it (or had it already: a repeat changes nothing);
//   Ordered  reserved: the world passed it on as a far order to the map server that writes the object (the W2a gate, WP7);
//   Refused  the acceptance (xrCoop/srv/coop_levels_accept.h) or the world's own rule refused it: `why` (journal::Why) goes back in
//            the journal's Ack, and the sender repairs towards the world.
enum class ApplyVerdict : uint8_t
{
	Applied,
	Ordered,
	Refused,
};

// The world server: one entry from the map server behind the bridge `author`, in the batch `epoch` (its seq; 0: no batch, every
// entry is before any fence). Every object fact goes through here: the acceptance first, then the one apply, whatever m_bOnline
// says (W3); the broadcasts it makes skip that map's players (EchoOf). The journal's own ops that name no object (Carry, Done,
// Flushed, IdAsk) are the journal receiver's: Refused here. Money is applied here as before (a deal of a player of its map).
// Main thread, in a world server's session; Refused (why Refused) anywhere else.
ApplyVerdict host_apply_fact(PeerId author, const journal::Entry& e, uint8_t& why, uint32_t epoch = 0);

// The world server, R32 W5.2: an entry the audit's member heal makes in the name of the map server behind `author` (a Joined or a Left
// its report of a squad it holds implies; srv/coop_levels_xfer.cpp host_heal_member). The acceptance and the apply of host_apply_fact,
// but that map server never raised it, so its Holder adopted nobody at it: a Left takes the member home here (accept::leave_plan:
// heal; §8a A17 is for the map server's own Left, fix round 2 of package A, verifier finding 4).
ApplyVerdict host_apply_heal(PeerId author, const journal::Entry& e, uint8_t& why, uint32_t epoch);

// The sender's fence for a root (W4: its level move, a grant handed back): its journal's seq up to which the root is its. kNoFence
// (coop_writer_rules.h) when no fence stands for it. Filled by the fences (WP9); until then no fence stands.
uint32_t host_fence_of(PeerId author, uint16_t root);

// coop_levels_status: what the acceptance took and refused, by reason.
void host_accept_print_status();
} // namespace levels
} // namespace coop
