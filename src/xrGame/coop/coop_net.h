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

#include "xrCoop/coop_session.h"
#include "coop_manager.h"

namespace coop
{
namespace net
{
// Host: to every client that is in the world (live), queued for clients that are loading.
// `except` skips one peer (the originator of a relayed message).
// F3a T4: to every player (Session::players()): the host player too, over the LocalLink, unless `except` is 0. The link
// passes only the messages moved onto it (link_enabled, coop_local_link.h); none yet.
void host_broadcast(Msg type, const Writer& payload, Lane lane = Lane::Reliable, PeerId except = kInvalidPeer);
// Host: to one client; queued while it loads (reliable lanes only, unreliable is dropped).
// F3a T4: `to` 0 is the host player, over the LocalLink (only the messages moved onto it; the rest is dropped as before).
void host_send(PeerId to, Msg type, const Writer& payload, Lane lane = Lane::Reliable);
// Client: to the host.
// F3a T4: in the host's process the host player is a client of his own world server: what he sends goes over the
// LocalLink to the server's handlers (from = 0). The link takes only the messages moved onto it (link_enabled): for the
// rest this stays the no-op it was on the host, which still takes its direct path.
void client_send(Msg type, const Writer& payload, Lane lane = Lane::Reliable);
// Host: flush the queue of a client that finished loading.
void host_flush(PeerId to);
// Host: drop the queue of a client that left.
void host_drop(PeerId to);
// Host: the queue of a loading client hit its cap (the client is stuck): it must be kicked.
bool host_queue_overflow(PeerId to);
void reset();

// True if this machine is in a live session (host, or client in the world).
inline bool live() { return mgr().is_host() || mgr().is_live_client(); }
inline bool host() { return mgr().is_host(); }
inline bool client() { return mgr().is_client(); }
} // namespace net
} // namespace coop
