// Anomaly Coop — per-player checkpoints on the host (F3, docs/CHECKPOINTS_DEATH.md §2). Every player has his own
// checkpoint (campfire, own tent, rest place, level arrival, story event); death sends him back to it and never rolls
// the world back. The host keeps them by character and decides every change; a client only reports (CheckpointClaim)
// and mirrors what the host sends (CheckpointState). Record format and rules: xrCoop/coop_checkpoint_rules.h.
//
// Store (F3 Task 6): one slot (current and previous camp) per character, in host numbering (0 = the host's actor, else
// a player's character). Until the player scope of F2 has a C++ API the slot lives in the character's custom data
// ([coop_checkpoint] cur / prev, the host's actor also `arrival`; xrCoop/coop_checkpoint_store.h), so it goes into the
// host's save with the character and comes back with it; the store reads it again whenever the served world changes
// (join::host_world_epoch) and on every access, so a reused entity id never hands one character's slot to another.
//
// Messages (ids in host numbering):
//   CheckpointState  host -> owner, and -> all while coop_checkpoints_group = 1   reliable
//                    {u16 character, u8 flags (1 rollback, 2 cleared), record unless cleared}; a player who comes into
//                    the world gets the whole list he may see (his own, everyone's with the group view on) and his
//                    own id as cleared when he has none. A slot that changed under the store (the character is gone,
//                    or its id belongs to another character now) is announced like any change (R1 I-1).
// Callback (owner, and everyone with the group view on; the host too):
//   coop_checkpoint_changed(local_id, kind, source_local_id or -1, rollback, level or nil). Kind "none" (source -1,
//   level nil) means the player has no checkpoint any more for this machine: rolled back to nothing, his character
//   gone, or no longer shared (the group view turned off). The callback queue is shared and bounded, a dropped
//   callback is not sent again: Lua re-reads coop.checkpoint.get.
// Lua: coop.checkpoint.get(local_id) -> nil | {kind, level, x, y, z, source = local id | nil};
//      coop.checkpoint.story(reason) -> bool (host);
//      register_source, unregister_source, claim_tent and the callback coop_checkpoint_refused:
//      coop_checkpoint_sources.h (F3 Task 7).
// Console: coop_checkpoints_group 0/1 (host setting), coop_checkpoint_status, coop_checkpoint_here (host, debug).
#pragma once

#include "script_export_space.h"
#include "xrCoop/coop_checkpoint_rules.h"

struct lua_State;
class CGameObject;
class CSE_ALifeCreatureActor;

namespace coop
{
class Session;

namespace checkpoints
{
void install(Session& s); // registers the message handlers (sync::install)
void reset(); // session started / ended (sync::on_session_started, sync::reset)
void frame(); // main thread, every frame (sync::frame)
void print_status(); // coop_status section (sync::print_status)
void register_console(); // from coop::register_console_commands
// Fills coop.checkpoint; called by coop_checkpoints_script_registrator once the `coop` table exists.
void register_lua(lua_State* L);

// --- host (in a session; false / nothing otherwise). Messages and callbacks go out from frame(), so these may run
// from Lua or from the level change.

// A new camp for a character: he is a player's (or the host), alive, the record is valid, on this level and on a
// vertex of its AI map. Written through to the character's custom data.
bool host_set(u16 character, const checkpoint::Record& r);
// The object `source` is gone: every slot with a record from it rolls back (owner / group told, rollback = true).
void host_source_lost(u16 source);
// The arrival point of a level change (kind Arrival, no source; no level check: it is on the next level). The host's
// own is also kept as his `arrival` (the fallback of every respawn on that level).
void host_set_arrival(u16 character, const checkpoint::Record& r);
// CALifeUpdateManager::change_level, after the actor's new place was read and before the save: the host arrives where
// the level changer puts him, every player in the world 0.8 m * n further along x (a spot off the AI map moves to the
// host's own when the new level is loaded).
void host_on_level_change(const CSE_ALifeCreatureActor& actor);
// A story checkpoint (stock xr_effects.scenario_autosave): kind Story where the host and every present living player
// stands. True if at least one was set.
bool host_story(const char* reason);
// Where a character comes back to life on this level (checkpoint::resolve): his camp, the previous one, `anchor`, the
// host's arrival point, else `body`. `kind` is the kind of the record used (None for the anchor and the body).
// False (out = body) when there is no host world.
bool host_respawn_point(u16 character, const Fvector* anchor, const Fvector& body, Fvector& out, checkpoint::Kind& kind);
// A record of where `character` stands now (an object on this level); not valid if there is no object.
checkpoint::Record record_here(const CGameObject* character, checkpoint::Kind kind, u16 source);
// The slot of a character, nullptr if he has no checkpoint. Valid until the next change of the store.
const checkpoint::Slot* host_slot(u16 character);
// F3 Task 7 (coop_checkpoint_sources): every record of the host's cache that came from an object (current and previous
// of each slot): whose it is, the object and the kind. For the sweep of lost sources and the owners of tents; empty
// without a host world.
struct SourceRef
{
	u16 character = 0xffff;
	u16 source = checkpoint::kNoSource;
	checkpoint::Kind kind = checkpoint::Kind::None;
};
void host_record_sources(xr_vector<SourceRef>& out);

// --- any machine
// The current checkpoint of the player whose character has `local_id` here (0 = me), source in local numbering: the
// own one always, the others while the host shares them (coop_checkpoints_group).
bool get(u16 local_id, checkpoint::Record& out);
} // namespace checkpoints
} // namespace coop

// Script export entry (export_classes, right after coop_script_registrator made the `coop` table): coop.checkpoint
// and coop.rest (coop_checkpoints_lua.cpp).
struct coop_checkpoints_script_registrator
{
	DECLARE_SCRIPT_REGISTER_FUNCTION
};
