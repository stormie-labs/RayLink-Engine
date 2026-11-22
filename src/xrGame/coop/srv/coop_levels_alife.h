// Сервер: только сервер мира и серверы карт (docs/MODES.md)
// Anomaly Coop — the world server applies what a map server reports (TZ Р32, temp/r30/design/R32-final.md §1 W2b, W3; §8 the
// frozen interface). The rest of coop_levels_alife.cpp is declared in coop_levels.h.
#pragma once

#include "xrCoop/coop_protocol.h"
#include "xrCoop/srv/coop_levels_wire.h"

#include <cstdint>

namespace coop
{
namespace levels
{
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

// The sender's fence for a root (W4: its level move, a grant handed back): its journal's seq up to which the root is its. kNoFence
// (coop_writer_rules.h) when no fence stands for it. Filled by the fences (WP9); until then no fence stands.
uint32_t host_fence_of(PeerId author, uint16_t root);

// coop_levels_status: what the acceptance took and refused, by reason.
void host_accept_print_status();
} // namespace levels
} // namespace coop
