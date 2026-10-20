// Anomaly Coop — mirroring the host world on clients.
//
// Host: streams the lifecycle of server entities (spawn, release, online/offline, parent
// changes) reliably, and snapshots of online creatures (NPCs, monsters) unreliably.
// Client: its ALife is frozen; it applies exactly what the host says, with the same ids.
#pragma once

#include "xrCoop/coop_session.h"

class CGameObject;
class CSE_Abstract;
class CSE_ALifeDynamicObject;
class NET_Packet;

namespace coop
{
namespace entities
{
void install(Session& s);
void reset();
void frame();
void print_status();
void on_peer_left(PeerId id);

// --- host ---
void host_begin_join(PeerId id); // start queueing reliable world changes for a loading peer
void host_end_join(PeerId id); // flush the queue and start streaming to it
// Engine hooks on the host (all ids in host numbering).
void host_on_entity_registered(CSE_ALifeDynamicObject* e); // new entity in ALife after the join snapshot
void host_on_entity_released(u16 id);
void host_on_switch(CSE_ALifeDynamicObject* e, bool online);
void host_on_event(NET_Packet& event); // GE_* event processed by the host server

// Host: ids of all top-level ALife objects that are online right now (sent with the world).
void host_collect_online(xr_vector<u16>& out);

// --- client ---
// Online set of the host at the moment of the world snapshot (host numbering).
void client_set_initial_online(const xr_vector<u16>& host_ids);
// ALife switching vote on a client (local numbering): true = keep/bring online.
bool client_wants_online(u16 id);
void on_client_live();
// True if `id` is an object whose behaviour comes from the host (any creature on a client).
bool is_mirrored(u16 id);
// Local spawn attempt on a client (see sync::intercept_local_spawn).
bool intercept_local_spawn(CSE_Abstract* entity);
// True while the client applies a host command itself (spawn/release/switch must go through).
bool client_applying();
// RAII scope for code that applies host commands on this machine (local events must pass).
struct ClientApply
{
	ClientApply();
	~ClientApply();
	ClientApply(const ClientApply&) = delete;
	ClientApply& operator=(const ClientApply&) = delete;
};
// Client: play the host snapshot on a mirrored creature (called by coop_remote).
bool drive_mirror(CGameObject* obj);
} // namespace entities
} // namespace coop
