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
class IKinematicsAnimated;
class xrServer;

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
// The host changed the holder of `id` (local numbering, EntityParent). The online set holds only free
// objects: a dropped item the host keeps online joins it (no EntitySwitch comes, it never switched there),
// otherwise the local switch manager takes it offline at once; an item put into an inventory leaves it.
void client_on_holder_changed(u16 id, u16 new_parent, bool host_online);
void on_client_live();
// B2 (xrCoop/coop_mirror.h), engine hooks of xrServer::Perform_destroy and Process_event_destroy: `holder` leaves the
// server and lists `child`, which has no server entity here. False outside a mirror (single player, the host: the
// stock assert). On a mirror (join::world_is_mirror, also after the session ended) it logs the child as ALife knows it
// and takes the id out of holder->children (an offline item of that holder the ALife way, graph().detach), no FATAL.
bool mirror_lost_child(CSE_Abstract* holder, u16 child);
// B2 diagnostics, engine hook of xrServer::entity_Destroy: on a mirror, logs an entity that leaves the server while
// its holder still lists it (the holder's next teardown would meet a lost child).
void mirror_on_destroy(xrServer& server, CSE_Abstract* entity);
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
// True if a MotionID value from another machine exists in this visual's motion set. Same visual
// means same ids; a different one may have fewer motions, and playing a missing one reads past
// the motion table.
bool motion_valid(IKinematicsAnimated* ka, u32 motion);
} // namespace entities
} // namespace coop
