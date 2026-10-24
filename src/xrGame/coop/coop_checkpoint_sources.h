// Anomaly Coop — where a checkpoint may come from, on the host (F3 Task 7, docs/CHECKPOINTS_DEATH.md §2.2): lit
// campfires and objects mods declare as camps (stood next to one for campfire_dwell_ms, not in a fight), a player's own
// tent or sleeping bag (CheckpointClaim), the stock rest zones and surge covers; and who is in a fight. Driven by
// coop_checkpoints: its install_server / install_client, reset and print_status call these, its frame calls host_frame
// on the host. Settings: configs\mod_system_coop_checkpoints.ltx ([coop_checkpoints], [coop_rest_zones]; pure rules and bounds:
// xrCoop/coop_checkpoint_source_rules.h; the claim and its answer: xrCoop/coop_checkpoint_store.h).
//
// Campfires and mod objects: every 500 ms the host and every living player in the world (join::host_player_in_world)
// is checked against the nearest lit CZoneCampfire within campfire_radius, or a registered mod object within its own
// radius; checkpoint::dwell_update decides, the camp goes through checkpoints::host_set (kind Campfire / Mod, the
// object as its source, the player's own position).
//
// Tents and sleeping bags (a "tent" below is either: an object of tent_sections). A claim — CheckpointClaim of a player,
// his character from his peer (survival::character_of_peer), never from the packet; F3a T7: the host player's too, from
// peer 0 over the internal channel (coop.checkpoint.claim_tent in his Lua) — says what (ClaimWhat: tent 1, sleeping bag
// 2, pack up 3) and where. It is checked in this order: the player is alive ("dead"), the spot is within
// tent_claim_radius of him ("far"), then
//   - tent / sleeping bag: he is not in a fight ("combat"); then (Ruling F3-6, R1 I-1): at most 8 tents of his in the
//     world ("limit"), an item of the stock TentBagSections / SleepBagSections in his inventory ("no_item"); the host
//     puts up tent_section / sleepbag_section at the spot, makes it his camp, and only then takes the item: everything
//     or nothing ("failed"). A player cannot create a world object himself until F2 Task 12b. F3a T7: the host player
//     too - the stock item_tent no longer puts his tent up (coop_tent_guard.script), the wait for a stock tent is gone;
//   - pack up (Ruling F3-8): his own tent within tent_claim_radius of the spot ("far"); the host gives him back the item
//     it took for it (a tent of the host player's that the stock put up before F3a T7: the stock item of its kind),
//     releases the tent, and the camp on it rolls back as with stock ("failed" if the item cannot be given).
// A tent's owner (his player key, ledger::character_key) and the item it was paid with are on the tent's custom data
// ([coop_tent] owner / item), so they outlast loads and level changes (R1 I-2); the limit counts his tents in the
// world.
// Every claim that reaches host_claim_tent gets one answer: a tent or sleeping bag put up is a CheckpointState with his
// Tent record; a pack-up is the item back in his inventory (and a rollback if the tent was his camp); anything else is
// a refusal. Claims of players who left the world are dropped by a poll every second (sync::on_peer_left does not reach
// the F3 modules): nobody is there to answer.
// Lost sources: every second each source of a record, each tent known and each mod object is looked up in ALife; one
// that is gone, or whose id names an object of another section now (R1 M-4), rolls its records back
// (checkpoints::host_source_lost).
//
// Callbacks:
//   coop_checkpoint_refused(reason, what) — the claimant's machine: reason "dead" | "far" | "combat" | "limit" |
//     "no_item" | "failed", what "tent" | "sleepbag" | "pack". A player's engine calls it from CheckpointState flag 4
//     (Ruling F3-5; F3a T7: the host player's from his copy over the internal channel). The callback queue is shared and
//     bounded: a hint.
//   coop_tent_placed(tent, character) — the host: the host put up a tent or sleeping bag for a player (its world Lua adds
//     it to item_tent.spawned_tents, CHECKPOINTS_DEATH §10.2).
// Lua: coop.checkpoint.register_source(id, radius) -> bool and unregister_source(id) -> bool (the host, or the game
//      before it hosts; false on a client). They are kept for the world they were made in, across the session's start
//      and end: every load or level change forgets them, and a mod registers again (actor_on_first_update).
//      coop.checkpoint.claim_tent(x, y, z [, "tent" | "sleepbag" | "pack"]) -> bool (a player: CheckpointClaim sent, at
//      most one per 2 s - F3a T7: the host player's over the internal channel at the next frame; false outside a
//      session).
//      coop.checkpoint.tent_owner(id) -> character id | nil (host: whose tent or sleeping bag it is).
#pragma once

#include "xrCoop/coop_checkpoint_store.h" // checkpoint::ClaimWhat

class CGameObject;

namespace coop
{
class Session;
class Endpoint;

namespace checkpoints
{
namespace sources
{
// Where a player rests (F3 Task 11: lying down at the own checkpoint). Numbers are not stored or sent.
enum class Place : u8
{
	None,
	Rest, // inside a stock rest zone or surge cover ([coop_rest_zones], cover_list): source = the restrictor
	Tent, // within tent_rest_radius of a tent or sleeping bag he owns: source = the tent
	Campfire, // within campfire_radius of a lit campfire: source = the campfire
	Mod // within the radius of a registered mod object: source = the object
};
const char* place_name(Place p);

void install_server(Session& s); // F3a T4: the world server's handlers (on the hosting session)
void install_client(Endpoint& e); // F3a T4: the client's (a remote client's session, the host player's LocalLink)
void host_frame(u64 now_ms); // host, main thread: checkpoints::frame
void player_frame(); // F3a T7, a player, main thread (checkpoints::frame): the host player's claim goes out
void reset(); // checkpoints::reset
void print_status(); // checkpoints::print_status

// Host: an online CAI_Stalker or CBaseMonster within combat_radius whose selected enemy is `character` (players'
// characters never count).
bool in_combat(const CGameObject* character);
// Host: where `character` (host numbering) may rest now, in this order: rest zone, own tent, lit campfire, mod object.
// `source` is the object (0xffff for None).
Place rest_place(u16 character, u16& source);
// Host (or no session): a mod object (an existing ALife object, not a player's character) counts as a camp within
// `radius` (0 < radius <= checkpoint::kMaxSourceRadius); a known id takes the new radius.
bool host_register_source(u16 id, float radius);
// Host (or no session): no longer a camp. Records it made stay while the object exists.
bool host_unregister_source(u16 id);
// Host: a claim of `character` (see the top of the file).
void host_claim_tent(u16 character, checkpoint::ClaimWhat what, const Fvector& pos);
// Host: the character who owns tent `id` (by the key on it), 0xffff if nobody's or he is not in this world.
u16 host_tent_owner(u16 tent);
// Any machine: coop.checkpoint.claim_tent. True if the claim was handed to the host.
bool claim_tent(checkpoint::ClaimWhat what, const Fvector& pos);
} // namespace sources
} // namespace checkpoints
} // namespace coop
