// Anomaly Coop — world objects owned by the host (WP7): physics objects and doors, destroyables and
// breakables, anomaly zones and campfires, artefact activation, grenades, bolts and explosions
// (SPEC R-10.3, R-5.1, R-2.1, P1, P5, P6).
//
// Authority
//   Physics objects (CPhysicObject doors/crates/barrels, loose items, thrown projectiles, corpse ragdolls,
//   breakable pieces, destroyable fragments, cars): the host simulates. A client makes every such body
//   kinematic (each active element Fixed: no gravity, practically infinite mass) and places it from host
//   samples; at rest it keeps the host's final pose. A client "use" of a CPhysicObject and a body push are
//   requests to the host. Animated physic objects (gates) follow the host's animation phase instead.
//   Destroyables / breakables: hits reach the host (WP2 ReqHit); the host destroys or breaks; clients
//   replay the destruction visual and get the host's fragments as entities (WP2 EntityLocal).
//   Zones: the host runs the state machine; a client applies host states with the elapsed phase and never
//   switches on its own. Zone damage: on a client only to the local actor; on the host never to a player
//   character (the owner computes its own anomaly damage).
//   Grenades / bolts: a client sends a throw intent; the host spawns and throws the projectile (ALife entity,
//   replicated by coop_entities), flies it, detonates it. Explosions: only the host detonates; clients play
//   the host's explosion (exact position) as a visual without damage, or a fallback effect if the object is
//   missing. Damage to players comes from the host (WP2 PlayerHit).
//
// Wire (ids in host numbering; every payload starts with u8 kind):
//   EntityState host -> clients
//     1 Samples   (unreliable) u32 host_ms, u8 count, entries:
//                 u16 id, u8 flags (1 moving, 2 pose only), u8 n,
//                 pose only: vec3 pos, quat16; else n x { vec3 pos, quat16, lin16, ang16 }
//     2 Rest      (reliable)   u32 host_ms, u8 count, entries: u16 id, u32 section_hash, u8 n, n x { vec3, quat16 }
//     3 Anim      (reliable)   u8 count, entries: u16 id, u8 playing, f32 speed, f32 time
//     4 Destroyed (reliable)   u16 id, u16 bone, u16 hit_type, vec3 dir, f32 impulse, vec3 p_in_bone
//     5 Broken    (reliable)   u16 id
//     6 Explode   (reliable)   u16 id, u16 initiator, vec3 pos, vec3 normal, str section
//   ZoneState host -> clients (reliable)
//     1 Zones     u16 count, entries: u16 id, u8 state, u32 elapsed_ms, u8 campfire (0 off, 1 on, 0xff n/a),
//                 f32 animator time (-1 if none)
//   EntityEvent client -> host (reliable)
//     1 Use       u16 object
//     2 Push      u16 object, u8 element, vec3 dir, f32 speed
//     3 Throw     u16 hand_item, vec3 pos, vec3 dir, vec3 velocity, u8 flags (1 consume, 2 can_take, 4 random spin)
//                 hand_item is in the character's inventory on the host; a grenade is always used up there, the
//                 consume flag only matters for a bolt (limited-bolts option of the thrower, R3 В7)
//     4 Activate  u16 artefact
//   quat16 = 4 x s16 (component * 32767); lin16/ang16 = 3 x s16 (value * 100).
//
// Threading: hooks may run in the game thread or the main thread; they only record facts under a lock.
// Sending and applying happen in frame() on the main thread.
//
// Included by many engine sources (hooks): keep it free of includes.
#pragma once

class CGameObject;
class CUsableScriptObject;
class CCustomZone;
class CMissile;
class CBlend;
class CSE_Abstract;
struct SHit;
struct SAnimParams;

namespace coop
{
class Session;
class Endpoint;
using PeerId = uint32_t; // same alias as xrCoop/coop_protocol.h

namespace objects
{
void install_server(Session& s); // F3a T4: the world server's handlers (on the hosting session)
void install_client(Endpoint& e); // F3a T4: the client's (a remote client's session, the host player's LocalLink)
void frame();
void reset();
void print_status();

// Host: a client finished loading (JoinComplete). Pushes zone states and phases, the rest poses of
// physics objects that moved since the level loaded, and the phases of animated physic objects (P5).
void host_on_peer_ready(PeerId id);

// Client: true if `obj` is a world physics object whose motion comes from the host here.
bool is_host_driven(const CGameObject* obj);

// ---------------------------------------------------------------- engine hooks
// Physics objects
void physic_anim(CGameObject* obj, CBlend* blend); // CPhysicObject::UpdateCL: animated door/gate phase
// CUsableScriptObject::use (hook requested from the lead): true = a client use of a physic object was sent
// to the host; the caller returns true without running the use callback.
bool client_intercept_use(CGameObject* obj, CGameObject* who);
// Client: the "Sleep" tip on a sleeping bag (its ph_idle logic runs on the host only); called by CActor's pick when the
// usable object it looks at has no tip.
void client_usable_tip(CUsableScriptObject* usable, CGameObject* obj);
// CHelicopter::UpdateCL (hook requested from the lead): true = placed from host poses, skip own movement.
bool client_drive_pose(CGameObject* obj);

// Destroyables and breakables
bool world_hit_blocked(CGameObject* obj); // CDestroyablePhysicsObject::Hit: true = drop (client)
// CPHDestroyable::Destroy: true = do not spawn fragments here (client; the host's fragments arrive).
bool ph_destroy(CGameObject* obj, const SHit& fatal_hit);
// CPHDestroyable::NotificateDestroy: true = client, `part` is a fragment the host spawned.
bool fragment_from_host(CGameObject* source, CGameObject* part);
void on_break(CGameObject* obj); // CBreakableObject::Break
// CBreakableObject::SendDestroy, CExplosive::OnAfterExplosion: true = the host removes this object.
bool host_destroys(CGameObject* obj);

// Anomaly zones
// SwitchZoneState, GoEnabledState, GoDisabledState: true = a client must not switch on its own.
bool zone_switch_blocked(CCustomZone* zone);
// CCustomZone::OnStateSwitch: returns the time already spent in the new state (ms) to start from.
int on_zone_state(CCustomZone* zone, u8 new_state);
// CCustomZone::CreateHit: true = drop (client: anything but the local actor; host: player characters).
bool zone_hit_blocked(CCustomZone* zone, u16 target_id);
// CZoneCampfire::turn_on_script / turn_off_script: true = drop (client: the host decides).
bool campfire_turn_blocked(CGameObject* campfire, bool on);
// CTorridZone::UpdateWorkload (hook requested from the lead): step for the zone's path animator that keeps
// the client on the host's phase. `dt` in seconds.
float zone_anim_dt(CCustomZone* zone, const SAnimParams& anim, float dt);

// Artefacts
bool client_forward_artefact_activation(CGameObject* artefact); // CArtefact::ActivateArtefact

// Grenades, bolts, explosions
bool skip_fake_missile(CGameObject* missile); // CMissile::spawn_fake_missile: true on a client
void host_mark_projectile(CSE_Abstract* projectile); // spawn_fake_missile: host lets ALife take the thrown copy
void on_projectile_free(CGameObject* missile); // CMissile::OnH_B_Independent
bool client_throw(CMissile* missile); // CGrenade::Throw, CBolt::Throw: true = sent to the host
bool explode_blocked(CGameObject* explosive); // CExplosive::GenExplodeEvent: true on a client
void on_explode(CGameObject* explosive, u16 initiator, const Fvector& pos, const Fvector& normal); // CExplosive::OnEvent
bool explosion_visual_only(CGameObject* explosive); // CExplosive::Explode: no hits / blast here
} // namespace objects
} // namespace coop
