// Anomaly Coop — mirroring the host world on clients.
//
// Host: streams the lifecycle of server entities (spawn, release, online/offline, parent
// changes) reliably, and snapshots of online creatures (NPCs, monsters) unreliably.
// Client: its ALife is frozen; it applies exactly what the host says, with the same ids.
//
// Files: coop_entities.cpp (the lifecycle, both ends), coop_snapshots.cpp (creature snapshots and the copies they drive),
// coop_roster.cpp (digests, rosters and repairs, xrCoop/coop_reconcile.h), coop_pose_diag.cpp (pose lines in the log);
// what they share is in coop_entities_impl.h.
#pragma once

#include "xrCoop/coop_session.h"

class CGameObject;
class CSE_Abstract;
class CSE_ALifeDynamicObject;
class IKinematicsAnimated;
class xrServer;

namespace coop
{
namespace mirror_pose
{
struct Book; // xrCoop/coop_mirror_pose_rules.h
}
namespace entities
{
void install_server(Session& s); // the world's handlers (on the hosting session)
void install_client(Endpoint& e); // the client's (a remote client's session, the host player's LocalLink)
void reset();
void frame();
void print_status();
void on_peer_left(PeerId id);
// The roster (xrCoop/coop_reconcile.h): after events::frame, so that a digest counts what this frame's flush told the
// clients. Host: a bucket's digest, the rosters asked for; client: the repairs that wait for their rate.
void reconcile_frame();
// Console coop_reconcile: 0 stops digests and repairs.
void register_console();

// --- host ---
// Engine hooks on the host (all ids in host numbering).
void host_on_entity_registered(CSE_ALifeDynamicObject* e); // new entity in ALife after the join snapshot
void host_on_entity_released(u16 id);
void host_on_switch(CSE_ALifeDynamicObject* e, bool online);

// Host: ids of all top-level ALife objects that are online right now (sent with the world).
void host_collect_online(xr_vector<u16>& out);
void host_send_online_to(PeerId to); // the host's online set to a mover of a simultaneous load, once he is in
// World server: where an offline object stands, to the map server of the level (EntitySwitch offline, to it alone).
void host_send_place(PeerId to, CSE_ALifeDynamicObject* e);
// A map server: what it made under its own ids, in EntitySpawn's layout; world server: it, into the world there.
bool write_entity(Writer& w, CSE_ALifeDynamicObject* e);
void host_spawn_from_leader(Reader& r);
// An offline object goes to this place in the registries of this machine.
void place_offline(CSE_ALifeDynamicObject* e, const Fvector& pos, const Fvector& ang, u32 node, u16 graph);

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
// xrCoop/coop_mirror.h, engine hooks of xrServer::Perform_destroy and Process_event_destroy: `holder` leaves the
// server and lists `child`, which has no server entity here. False outside a mirror (single player, the host: the
// stock assert). On a mirror (join::world_is_mirror, also after the session ended) it logs the child as ALife knows it
// and takes the id out of holder->children (an offline item of that holder the ALife way, graph().detach), no FATAL.
bool mirror_lost_child(CSE_Abstract* holder, u16 child);
// Diagnostics, engine hook of xrServer::entity_Destroy: on a mirror, logs an entity that leaves the server while
// its holder still lists it (the holder's next teardown would meet a lost child).
void mirror_on_destroy(xrServer& server, CSE_Abstract* entity);
// True if `id` is an object whose behaviour comes from the host (any creature on a client).
bool is_mirrored(u16 id);
// The host half of a level hears of a change here. Not on a map server while it applies what the world server told it: the players
// have that from the world server themselves.
bool host_records();
// Is `id` this process's own create whose EntitySpawn has not left it yet? A Registered event of it is still in the
// queue that the next frame() sends (host_process_events), recorded while this process applied nobody's word (not a map server's spawn
// the world takes into its world, host_spawn_from_leader). items::set_initial asks it for an item this process does not write. The
// same threads as the hooks that fill the queue, never concurrently with frame().
bool host_own_spawn_unsent(u16 id);
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
// Client: play the host snapshot on a mirrored creature (called by coop_remote). `book` is the copy's pose book for its
// skeleton (coop_remote keeps it per copy): a partition no usable host motion reaches gets the copy's own idle once the
// copy settled, and a copy that may stand in its spawn pose is said once per episode. False when no snapshot of
// the id is here (the copy is neither placed nor driven by the host then).
bool drive_mirror(CGameObject* obj, mirror_pose::Book& book);
// Client, one bounded line: what the copy `id` (local numbering) has from the decider and what it plays now; `why` is
// the reason it is asked (a talk the decider answered "gone" for an NPC that is here).
void describe_copy(u16 id, const char* why);
// True if a MotionID value from another machine exists in this visual's motion set. Same visual
// means same ids; a different one may have fewer motions, and playing a missing one reads past
// the motion table.
bool motion_valid(IKinematicsAnimated* ka, u32 motion);
} // namespace entities
} // namespace coop
