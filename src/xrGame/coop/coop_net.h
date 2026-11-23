// Anomaly Coop — message routing helpers shared by all coop subsystems.
//
// Topology is a star: clients talk only to the host, the host relays.
// Reliable world changes for a client that is still loading the world are queued by the
// host and flushed after it reports WorldLoaded (P6: deferred, never lost, never early).
//
// Ids inside payloads are always in HOST numbering. Use join::to_host() before writing a
// local id on a client and join::to_local() after reading on a client. On the host both
// numberings are the same.
#pragma once

#include "../../xrCoop/coop_mode.h"
#include "../../xrCoop/coop_routes.h"
#include "../../xrCoop/coop_session.h"

namespace coop
{
namespace net
{
// S4 (docs/MODES.md section 4, P-B): host_broadcast, host_stream and client_send forward to the session's mode
// (xrCoop/coop_mode.h mode().deliver / stream / up), which knows who sees what; outside a session, and on a process that
// decides nothing, nothing is sent.
// A world change to every player who sees it: in the world (live), queued for one that is loading. `except` skips one peer
// (the originator of a relayed message). The co-op host: every player, the host player too over the LocalLink unless
// `except` is 0 (the link passes only the messages moved onto it, link_enabled). The world server: the players a
// broadcast of this type reaches (levels::passes_to) and the bridges of the map servers. A map server: the players of its map.
void host_broadcast(Msg type, const Writer& payload, Lane lane = Lane::Reliable, PeerId except = kInvalidPeer);
// A state stream (unreliable, nothing queued) to the players of this process's map: the same audience rule.
void host_stream(Msg type, const Writer& payload, PeerId except = kInvalidPeer);
// The world's or a map's decider: to one peer; queued while it loads (reliable lanes only, unreliable is dropped).
// F3a T4: `to` 0 is the host player, over the LocalLink (only the messages moved onto it; the rest is dropped as before).
void host_send(PeerId to, Msg type, const Writer& payload, Lane lane = Lane::Reliable);
// host_send without the question who sends: what the modes' delivery uses (a bridge is a peer like a player here).
void to_peer(PeerId to, Msg type, const Writer& payload, Lane lane);
// To whoever decides for this process: a client's host; the co-op host player's own process over the LocalLink (from = 0;
// only the messages moved onto it pass); a map server's world server (what a ghost may say, mapserver::map_server_sends).
void client_send(Msg type, const Writer& payload, Lane lane = Lane::Reliable);
// Host: flush the queue of a client that finished loading.
void host_flush(PeerId to);
// Host: drop the queue of a client that left.
void host_drop(PeerId to);
// Host: the queue of a loading client hit its cap (the client is stuck): it must be kicked.
bool host_queue_overflow(PeerId to);
void reset();
void print_status(); // D16: the join queues

// True if this machine is in a live session (host, or client in the world).
bool live();
bool host();
// A client of the world includes a process that leads one of its levels.
bool client();
inline bool client(bool active, const Mode& mode) { return active && !mode.decides_world(); }

// Outside a session the router leaves all callbacks to the stock game.
inline bool route_runs_here(routes::Domain domain, bool active, const Mode& mode)
{
	return !active || routes::runs_here(domain, mode.decides_world(), mode.leads_level());
}
} // namespace net
} // namespace coop
