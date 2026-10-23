// Anomaly Coop — client requests and host arbitration (WP2).
//
// Only the host creates and destroys entities and changes who holds an item (P1, P7, R-6.1).
// A client asks; the host validates, applies the first valid request through the normal engine
// paths and replies ReqResult. The world change itself reaches everybody through the usual
// relays (EntitySpawn, EntityDestroy, EntityParent, EntityLocal), so the requester applies the
// same result as everybody else, with the same ids.
//
// Wire layouts (client -> host, ids in host numbering, every request starts with u32 seq):
//   ReqSpawn      u32 seq, u8 0xC6, u16 parent, vec3 pos, str section, varu n, n bytes item state (coop_items blob)
//                 F2 T12b: u8 0xC7 adds u16 id, u16 source after parent - an item the client created already under an id
//                 the host leased it (IdLease / IdLeaseAck, xrCoop/coop_id_lease.h), source: the weapon a clone
//                 replaces (released by the host with the clone; the budget charges only what the clone costs more)
//                 legacy (no seq): str section, u16 parent, vec3 pos  (entities::intercept_local_spawn)
//                 The host builds the entity from its own section (it must have `class` and the lines its entity's
//                 constructor reads unchecked) and applies the item state with the ItemState checks; raw M_SPAWN
//                 bytes (old marker 0xC5) are refused (R3 В6). Parser and class lines: xrCoop/coop_spawn.h.
//   ReqDestroy    u32 seq, u16 item
//   ReqUse        u32 seq, u16 item                       (eatables; effect already applied)
//   ReqTake       u32 seq, u16 item, u16 from, u16 to, vec3 pos, u8 flags
//   ReqDrop       u32 seq, u16 item, u16 from, u16 to, vec3 pos, u8 flags
//   ReqTransfer   u32 seq, u16 item, u16 from, u16 to, vec3 pos, u8 flags
//                 flags: 1 = already applied locally (thrown missile, launched rocket), 2 = trade, 4 = exact drop
//                 position; the host checks the reach whatever the flags say (R3 В8)
//   ReqHit        u32 seq, u16 target, vec3 shooter_pos, hit   (power etc. after the client's before-hit Lua, W5)
//   ReqMoney      a change of the actor's money with a cause, not an amount: coop_money.h (F2 T1, R4 7)
//   ReqItemEvent  u32 seq, u16 item, u16 ge_type, u16 size, bytes after the GE header, u16 mechanic, u32 paid
//                 (F2 T5, ledger::UpgradePayment): only GE_INSTALL_UPGRADE, bought from `mechanic` (0xffff: a script's
//                 upgrade, refused unless scripted_upgrades) for `paid` RU the client's game charged. The host checks the
//                 mechanic (his talk lock, a live stalker with mechanic settings, within 6 m), the price
//                 (paid >= floor(cost x upgrade_min_percent)) and the balance, relays the upgrade, answers (always),
//                 then charges `paid` — the MoneyBalance follows the answer, also after a refusal of a paid one.
//                 Fix round 1: a sold upgrade goes onto the host's game object and the others' copies (the owner's
//                 ItemState never adds one); a client whose upgrade was refused builds the item again from the
//                 host's entity (local destroy, StateDumpRequest).
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
class CActor;
struct SHit;
struct lua_State;

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
void on_peer_left(PeerId id); // host: the player's spawn requests waiting for his budget go (F2-21)
void frame();
void print_status();
void register_console(); // coop_client_hit_perks (called from coop_presence::register_console, W5)

// --- client: queue requests (any thread; sent from frame() on the main thread) ---
// Spawn the client game asked its own server for. Forwards its section and item state when the parent
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
// GE_MONEY of our actor (router): passed to money::client_on_actor_money (F2 T1, the host keeps the balance).
void client_money(u32 money);
// Main thread: sends the queue now (as frame() does) and the queued money changes. Before a trade deal,
// so the host gets the item moves and money changes made before it first (W3b, F2 T1).
void client_flush_outbox();
void client_item_event(u16 local_item, u16 ge_type, const void* data, u32 size);
void client_player_died(u16 local_killer);
// GE_LAUNCH_ROCKET of our own launcher (router, any thread): reads the rocket's launch parameters and
// queues a ReqLaunch. False if the rocket has no game object here (nothing to send).
bool client_launch(u16 local_launcher, u16 local_rocket);

// --- Lua wrappers on a client (alife():create/create_ammo/clone_weapon/register/release) ---
// The request paths of the call gate (coop_callgate.h, F2 T12): alife_simulator_script.cpp asks the gate first and
// calls these on Request.
bool lua_client(); // true on a coop client in the host's world: the hooks below apply
// Returns nullptr after forwarding/refusing; for register=false returns a temporary object
// without an id that a later lua_register_spawn() forwards.
CSE_Abstract* lua_create(const char* section, const Fvector& pos, u32 level_vertex, u16 game_vertex, u16 parent,
    bool register_now, int ammo = -1);
CSE_Abstract* lua_clone(CSE_Abstract* source, const char* section, const Fvector& pos, u32 level_vertex,
    u16 game_vertex, u16 parent, bool register_now);
CSE_Abstract* lua_register_spawn(CSE_Abstract* object);
// alife():release on a client, the gate included: true = handled here (an own item released through the host, a
// refusal, a temporary object that was never created); false = the gate runs the stock code (an ltx override).
bool lua_release(CSE_Abstract* object);
// A refusal of the request path after the gate let the call through: counted, logged by the gate (name = a kCalls
// name such as "alife:register"). Returns nullptr.
CSE_Abstract* lua_refuse(const char* name, const char* why);

// --- F2 T12b: leased ids (xrCoop/coop_id_lease.h) ---
// On a live client with a leased id, lua_create into our actor, lua_clone of our weapon and lua_register_spawn of a
// temporary object that holds an id create our item at once (registered, online in our inventory) and answer it; the
// host creates it with the same id (ReqSpawn 0xC7) or refuses, and the item is taken back here. A clone's source is
// released by the host with the clone: lua_release of it while the clone waits sends nothing.
// xrServer::entity_Destroy hook: true = `entity` is an item of ours the host refused, released just now; it stays
// allocated kQuarantineMs (a script may still hold its se_obj) and is freed later by this module. Any thread.
bool keep_released(CSE_Abstract* entity);
// Lua: coop.lease_ids() -> the leased ids this client can create its own items with now (0 elsewhere). A raw C function
// for coop::script::register_raw (coop_script.cpp); coop_mods_guard.script lets a weapon's scope swap through with one.
int lua_lease_ids(lua_State* L);
// Lua calls that would change the client's frozen ALife copy (teleport_object, kill_entity,
// set_switch_online/offline; W3a; F2 T12 round 1: set_interactive, the restrictions). A wrapper over the call gate
// (F2 T12): name is the kCalls name ("alife:kill_entity"), target the object id (0xffff = none; else it must exist in
// this world). runs_while_applying: a host command being applied runs it (the round-1 gates); false keeps the refusal
// then (the gates before round 1). true on a coop client = refuse (the gate counted and logged it).
bool lua_client_refuse(const char* name, u16 target = u16(-1), bool runs_while_applying = false);

// --- damage (R-5.1) ---
// CAI_Stalker::Hit hook. true = do not apply here. Host: a player character never takes damage
// itself; hits of creatures and every explosion (R3 В2, whoever set it off) go to its player as
// PlayerHit (rules: xrCoop/coop_hits.h). Client: host-driven stalkers and avatars never take local damage.
bool stalker_hit(CAI_Stalker* npc, SHit* hit);
// Host side of the CActor::Hit hook (events::client_block_actor_hit). true = the host's player does not take this
// hit: another player's damage with friendly fire off. Its shots and blows are refused as ReqHit; its grenade, rocket
// or the barrel it shot explode here and hit the host's actor directly (R3 minor).
bool host_actor_hit_dropped(const CActor* actor, const SHit* hit);
// Host: detonates the free explosive `explosive` (a planted charge) as set off by `initiator`, a player's character or
// the host's actor: its blast and fragments are that player's, so friendly fire, kills and NPC blame count for them
// (R3 В2 review; stock CScriptGameObject::explode names the charge itself). False (nothing done) for a client, an
// explosive with a parent, or an initiator that is not an online player's character.
bool host_explode_as(u16 explosive, u16 initiator);
// Lua: coop.explode_as(explosive_id, initiator_id) -> bool, host numbering. A raw C function for
// coop::script::register_raw (coop_script.cpp); coop_npc_world.script calls it when present.
int lua_explode_as(lua_State* L);
} // namespace requests
} // namespace coop
