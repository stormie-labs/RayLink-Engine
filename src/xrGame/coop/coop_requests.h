// Anomaly Coop — client requests and host arbitration (WP2).
//
// Only the host creates and destroys entities and changes who holds an item (P1, P7, R-6.1).
// A client asks; the host validates, applies the first valid request through the normal engine
// paths and replies ReqResult. The world change itself reaches everybody through the usual
// relays (EntitySpawn, EntityDestroy, EntityParent, EntityLocal), so the requester applies the
// same result as everybody else, with the same ids.
//
// Wire layouts (client -> host, ids in host numbering, every request starts with u32 seq):
//   ReqSpawn      u32 seq, u8 0xC5, u16 parent, vec3 pos, u16 size, M_SPAWN bytes (full state)
//                 legacy (no seq): str section, u16 parent, vec3 pos  (entities::intercept_local_spawn)
//   ReqDestroy    u32 seq, u16 item
//   ReqUse        u32 seq, u16 item                       (eatables; effect already applied)
//   ReqTake       u32 seq, u16 item, u16 from, u16 to, vec3 pos, u8 flags
//   ReqDrop       u32 seq, u16 item, u16 from, u16 to, vec3 pos, u8 flags
//   ReqTransfer   u32 seq, u16 item, u16 from, u16 to, vec3 pos, u8 flags
//                 flags: 1 = already applied locally (thrown missile, launched rocket), 2 = trade
//   ReqHit        u32 seq, u16 target, vec3 shooter_pos, hit   (power etc. after the client's before-hit Lua, W5)
//   ReqMoney      u32 seq, u32 money, u32 deals           (the client owns its money, R-6.7; deals = newest
//                 trade answer already in `money`, the host adds the accepted trades after it: coop_trade.h)
//   ReqItemEvent  u32 seq, u16 item, u16 ge_type, u16 size, bytes after the GE header
//   ReqLaunch     u32 seq, u16 launcher, u16 rocket, vec3 pos, vec3 dir, vec3 vel, vec3 angular_vel (W3a)
//                 the client already launched its copy (a visual that never detonates there); the host
//                 checks it, launches its own copy (it flies, detonates, is removed for everybody) and
//                 relays the launch to the other clients (EntityLocal op 2). Refused: the host removes the rocket.
// Host -> client:
//   ReqResult     u32 seq, u8 accepted, u8 reason, u16 subject
//   PlayerHit     u16 character, hit                     (damage for the receiver's own actor)
// hit = u16 who, u16 weapon, vec3 dir, f32 power, u16 bone, vec3 p_in_bone, f32 impulse,
//       u8 aim_bullet, u16 hit_type, f32 armor_piercing, u32 bullet_id
#pragma once

#include "xrCoop/coop_session.h"

class CSE_Abstract;
class CAI_Stalker;
struct SHit;

// Server option: players can hurt each other (default on, R-5.3). Console var for the lead.
extern int g_coop_friendly_fire;
// Client option (W5 C2.1, console coop_client_hit_perks, default on): before a hit of ours on a host creature goes
// out as ReqHit, the Lua before-hit chain runs here on the puppet (npc_on_before_hit / monster_on_before_hit with
// this player's belt and skills: PBA, Haru's skills, silent kills...), and the resulting power, impulse, direction
// and type are sent; a handler that returns false drops the hit. The host runs its own chain as before (its Lua
// skips the host player's perks for a hit whose draftsman is another player's character) and still checks bounds.
extern int g_coop_client_hit_perks;

namespace coop
{
namespace requests
{
enum class Reason : u8
{
	Ok = 0,
	Malformed,
	NoProxy, // the peer has no character in this world
	Missing, // the object does not exist (any more)
	Moved, // the object is not where the client thinks: somebody else was first
	NotOwner, // not the requester's object / container of another player
	NotAllowed, // class or section a client may not create / touch
	BadSection,
	RateLimited,
	TooFar,
	Offline, // object or parent is offline on the host
	FriendlyFire,
	Engine, // the engine refused the change
	Count
};
const char* reason_name(Reason r);

void install(Session& s);
void reset();
void frame();
void print_status();
void register_console(); // coop_client_hit_perks (called from coop_presence::register_console, W5)

// --- client: queue requests (any thread; sent from frame() on the main thread) ---
// Spawn the client game asked its own server for. Forwards it with full state when the parent
// chain ends at our actor; otherwise it is the host's business and is dropped.
// Returns true if forwarded. `entity` stays owned by the caller.
bool client_forward_spawn(CSE_Abstract* entity);
void client_destroy(u16 local_item, bool use);
// Ownership change: from/to are local ids (0xffff = the world). Back-to-back reject + take of
// one item (box -> actor) merge into one transfer.
void client_move(u16 local_item, u16 from, u16 to, const Fvector* pos, u8 flags);
// Updates the drop position of a queued move (GE_CHANGE_POS right after a drop). False if none.
bool client_move_position(u16 local_item, const Fvector& pos);
void client_hit(u16 local_target, const SHit& hit);
void client_money(u32 money);
// Main thread: sends the queue now (as frame() does). Before a trade deal, so the host gets the
// money snapshots and item moves made before it first (W3b).
void client_flush_outbox();
void client_item_event(u16 local_item, u16 ge_type, const void* data, u32 size);
void client_player_died(u16 local_killer);
// GE_LAUNCH_ROCKET of our own launcher (router, any thread): reads the rocket's launch parameters and
// queues a ReqLaunch. False if the rocket has no game object here (nothing to send).
bool client_launch(u16 local_launcher, u16 local_rocket);

// --- Lua wrappers on a client (alife():create/create_ammo/clone_weapon/register/release) ---
bool lua_client(); // true on a coop client in the host's world: the hooks below apply
// Returns nullptr after forwarding/refusing; for register=false returns a temporary object
// without an id that a later lua_register_spawn() forwards.
CSE_Abstract* lua_create(const char* section, const Fvector& pos, u32 level_vertex, u16 game_vertex, u16 parent,
    bool register_now, int ammo = -1);
CSE_Abstract* lua_clone(CSE_Abstract* source, const char* section, const Fvector& pos, u32 level_vertex,
    u16 game_vertex, u16 parent, bool register_now);
CSE_Abstract* lua_register_spawn(CSE_Abstract* object);
void lua_release(CSE_Abstract* object);
CSE_Abstract* lua_refuse(const char* what);
// Lua calls that would change the client's frozen ALife copy (teleport_object, kill_entity,
// set_switch_online/offline; W3a). true on a coop client = refuse; logged once per `what` (a literal).
bool lua_client_refuse(const char* what);

// --- damage (R-5.1) ---
// CAI_Stalker::Hit hook. true = do not apply here. Host: a player character never takes damage
// itself; creature hits go to its player as PlayerHit. Client: host-driven stalkers and avatars
// never take local damage.
bool stalker_hit(CAI_Stalker* npc, SHit* hit);
} // namespace requests
} // namespace coop
