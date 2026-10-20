// Anomaly Coop — remote-driven creatures ("puppets", WP1).
//
// A creature whose behaviour comes from the network (NPCs, monsters and traders on a client,
// player avatars everywhere) is an engine Remote object on this machine: it does not think,
// select animations, run its Lua binder update, use monster abilities or lose health locally.
// Every frame it is placed and animated from the snapshot buffers (entities::drive_mirror,
// players::drive_avatar); it dies only when the host says so (on_host_death).
//
// Threading: on_net_spawn and the shedule-side predicates may run on the game thread, update_cl /
// apply / on_host_death on the main thread; they never run concurrently with the network pump.
#pragma once

class CObject;
class CGameObject;
class CScriptBinder;

namespace coop
{
namespace remote
{
// Kinds of objects that can be puppets (and that the host streams snapshots for): stalkers,
// monsters (CBaseMonster) and traders (CAI_Trader).
bool is_puppet_type(CGameObject* obj);

// True for a puppet-type object that is remote-driven here (sync::is_remote_driven restricted to
// creatures: items, physics objects and the local actor never become puppets).
bool is_remote_creature(const CGameObject* obj);

// Level_network_spawn hook, right after a successful net_Spawn: turns a remote creature into an
// engine Remote object (no export to the server, no local kill), visible and unharmable.
void on_net_spawn(CObject* obj);

// UpdateCL hook (main thread; CCustomMonster and CAI_Trader). Keeps the engine Local flag in step
// with the predicate (session start/stop, proxies registered later) and drives a puppet from its
// snapshots. Returns true if the caller must skip its own placement and animation selection.
bool update_cl(CGameObject* obj);

// Drives one living puppet from its snapshot buffer (drive_avatar / drive_mirror), then syncs its
// body orientation, physics capsule, AI location, footstep/IK legs blend and, on the host, the
// player proxy's server entity. False if there is no snapshot yet or the creature is dead
// (corpses belong to the local ragdoll).
bool apply(CGameObject* obj);

// CScriptBinder::shedule_Update hook: true if the Lua binder update must not run (a puppet).
// reinit / net_spawn / save / load / net_destroy of the binder still run, so db.storage exists.
bool suspend_binder(CScriptBinder* binder);

// The host reported that `obj` died (killer in local numbering, 0xffff if unknown). Runs the
// normal death chain here: GE_DIE through the local server (CSE health/killer, ALife on_death),
// then Die(): ragdoll, corpse, death callbacks; the inventory stays. Repeats are ignored.
void on_host_death(CGameObject* obj, u16 killer_local_id);

// Health from a snapshot (0..1) for a living puppet. Never kills: values <= 0 keep it barely
// alive until on_host_death arrives.
void set_health(CGameObject* obj, float health);

void print_status();
} // namespace remote
} // namespace coop
