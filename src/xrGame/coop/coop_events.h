// Anomaly Coop — game events between the local game and the host (WP2).
//
// Client router: every gameplay message the local game sends to its own server passes
// CLevel::Send. On a coop client client_filter() decides per message: allow (purely local),
// forward to the host as a request and swallow (world changes), or swallow (the host's job).
// Code that applies host commands opens entities::ClientApply; its events pass unchanged.
//
// Host relay: the host server reports ownership changes, deaths and destroyed non-ALife
// children and new creature models; frame() broadcasts them (EntityParent, EntityDied, PlayerDied, EntityLocal,
// EntityVisual).
// Clients apply them with the same ids through the normal engine event chain.
//
// Wire layouts (ids in host numbering):
//   EntityParent  u16 item, u16 old_parent, u16 new_parent, u8 online, vec3 position, u8 flags
//                 (flags 1: an exact drop position, e.g. drop_item_and_teleport; else the holder's)
//   EntityDied    u16 victim, u16 killer
//   PlayerDied    u16 character (host actor 0 or a proxy), u16 killer
//   EntityVisual  EntityVisualMsg: u16 creature, str visual (protocol 17)
//   EntityLocal   u8 op (0 spawn, 1 destroy, 2 launch), u16 id; spawn: u16 parent, u16 size, M_SPAWN bytes;
//                 launch (W3a, id = rocket): u16 launcher, vec3 pos, vec3 dir, vec3 vel, vec3 angular_vel
//                 (a rocket the host launched for a player: the other clients fly their copy the same way)
#pragma once

#include "xrCoop/coop_session.h"

class NET_Packet;
class CSE_Abstract;
class CActor;
class CGameObject;
class CObject;
struct SHit;

namespace coop
{
namespace events
{
void install_server(Session& s); // F3a T4: the world server's handlers (on the hosting session)
void install_client(Endpoint& e); // F3a T4: the client's (a remote client's session, the host player's LocalLink)
void reset();
void frame(); // call after entities::frame() so a spawn goes out before its parent change
void print_status();

// --- client ---
// CLevel::Send hook. true = the message is handled by coop and must not reach the local server.
bool client_filter(NET_Packet& P);
// CActor::Hit hook. true = drop this hit: it comes from a host-driven object, the host sends the
// authoritative damage as PlayerHit. On the host it applies friendly fire to the host's player instead
// (requests::host_actor_hit_dropped, R3 minor).
bool client_block_actor_hit(const CActor* actor, const SHit* hit);
// Lua attach_vehicle hook (CScriptGameObject::AttachVehicle). true = refuse: on a coop client cars are
// kinematic copies the host drives; mounted guns and other holders stay local (same rule as the router).
bool client_refuse_vehicle(CGameObject* holder);

// --- host (called from xrServer event processing, any game thread) ---
void host_on_parent_changed(u16 item, u16 old_parent, u16 new_parent);
void host_on_death(u16 victim, u16 killer);
void host_on_destroy(const CSE_Abstract* entity);
// The roster's repairs (xrCoop/coop_reconcile.h): the host's word on an entity, to one client. EntityDied only if it is dead.
void host_send_parent(PeerId to, CSE_Abstract* e);
void host_send_death(PeerId to, CSE_Abstract* e);
// Client: the host's holder of `item` (local numbering) is being applied here.
bool client_parent_pending(u16 item);
// GE_CHANGE_VISUAL reached the server entity `id`: a creature's new model goes to the clients (EntityVisual).
void host_on_visual(u16 id);
// A ReqSpawn produced an entity ALife does not register (child of an item: fake missile,
// rocket). Clients get it through EntityLocal with the same id.
void host_relay_local_spawn(CSE_Abstract* entity);
// A requested drop goes to an exact position (sent with the EntityParent of this frame).
void host_set_drop_position(u16 item, const Fvector& pos);
// The host launched `rocket` from `launcher` for player `requester` (ReqLaunch): the other clients launch
// their copy with the same parameters (EntityLocal op 2). Main thread.
void host_relay_launch(u16 rocket, u16 launcher, const Fvector& pos, const Fvector& dir, const Fvector& vel,
    const Fvector& angular_vel, PeerId requester);
// Last player character that held a now free item (0 = host actor, proxy id, 0xffff = none).
u16 host_last_holder(u16 item);

// --- one id, one object on a mirror (xrCoop/coop_object_identity.h, live RL-UGYE-3WGE 2026-11-09). The game thread
// (CLevel::net_Update). Single player and the host: the stock behaviour (true / nothing). ---
// CLevel::cl_Process_Spawn: false = this M_SPAWN makes no game object (its entity is gone or another one has the id
// now, or a live object of its section holds the id). Logged once per id.
bool client_spawn_allowed(const CSE_Abstract* spawn);
// CLevel::ClientSend: false = this object writes no update (another object is the level's one for its id, or the
// entity under its id is of another section). Logged once per id.
bool client_export_allowed(CObject* obj);
// xrServer::Process_update: `e` read `read` bytes of a chunk of `size`. true = skip the chunk (a mirror: logged once per
// id, the host asked for the object at the next frame); false = the stock FATAL.
bool client_update_mismatch(CSE_Abstract* e, u32 read, u32 size);

// --- both ---
// Server entity by id (ALife registry first, then the server's online entities).
CSE_Abstract* find_entity(u16 id);
// Top-level owner of an entity (itself if it has no parent), 0xffff if unknown.
u16 root_of(u16 id);
} // namespace events
} // namespace coop
