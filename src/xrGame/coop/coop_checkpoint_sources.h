// Anomaly Coop — where a checkpoint may come from, on the host (F3 Task 7, docs/CHECKPOINTS_DEATH.md §2.2): lit
// campfires and objects mods declare as camps (stood next to one for campfire_dwell_ms, not in a fight), a player's own
// tent (CheckpointClaim), the stock rest zones and surge covers; and who is in a fight. Driven by coop_checkpoints: its
// install, reset and print_status call these, its frame calls host_frame on the host. Settings:
// configs\mod_system_coop_checkpoints.ltx ([coop_checkpoints], [coop_rest_zones]; pure rules and bounds:
// xrCoop/coop_checkpoint_source_rules.h).
//
// Campfires and mod objects: every 500 ms the host and every living player in the world (join::host_player_in_world)
// is checked against the nearest lit CZoneCampfire within campfire_radius, or a registered mod object within its own
// radius; checkpoint::dwell_update decides, the camp goes through checkpoints::host_set (kind Campfire / Mod, the
// object as its source, the player's own position).
// Tents: a claim "I put up a tent at P" (the host's own Lua: coop.checkpoint.claim_tent; a client: CheckpointClaim,
// his character from join::proxy_of_peer, never from the packet) is checked first: the player is alive ("dead"), P is
// within tent_claim_radius of him ("far"), he is not in a fight ("combat"); a client may own at most 8 tents ("limit",
// kMaxPlayerTents). Then
//   - the host's own claim waits tent_claim_ms for the nearest tent of tent_sections within tent_claim_radius of P that
//     nobody owns and that appeared at most tent_claim_ms before it (the stock item_tent of the host puts it up);
//   - a client's claim: the host puts the tent up itself (the first of tent_sections at P), because a client cannot
//     create a world object (coop_requests drops a client's alife_create without a parent until F2 Task 12b).
// The tent becomes the player's (its owner, kind Tent in his slot). Claims of players who left the world are dropped
// by a poll every second (sync::on_peer_left does not reach the F3 modules).
// Lost sources: every second each source of a record, each tent known and each mod object is looked up in ALife; one
// that is gone rolls its records back (checkpoints::host_source_lost).
//
// Callback on the host: coop_checkpoint_refused(reason, peer) — reason "dead" | "far" | "combat" | "limit", peer the
// claimant (0 = the host himself). The protocol has no message back to a client, so the host's Lua passes a client's
// refusal on (coop.send_event to `peer`). The queue is shared and bounded: a hint, not a guarantee.
// Lua: coop.checkpoint.register_source(id, radius) -> bool and unregister_source(id) -> bool (the host, or the game
//      before it hosts; false on a client). They are kept for the world they were made in, across the session's start
//      and end: every load or level change forgets them, and a mod registers again (actor_on_first_update).
//      coop.checkpoint.claim_tent(x, y, z) -> bool (host: the claim is checked now; client: CheckpointClaim sent, at
//      most one per 2 s; false outside a session).
#pragma once

class CGameObject;

namespace coop
{
class Session;

namespace checkpoints
{
namespace sources
{
// Where a player rests (F3 Task 11: lying down at the own checkpoint). Numbers are not stored or sent.
enum class Place : u8
{
	None,
	Rest, // inside a stock rest zone or surge cover ([coop_rest_zones], cover_list): source = the restrictor
	Tent, // within tent_rest_radius of a tent he owns: source = the tent
	Campfire, // within campfire_radius of a lit campfire: source = the campfire
	Mod // within the radius of a registered mod object: source = the object
};
const char* place_name(Place p);

void install(Session& s); // the CheckpointClaim handler (checkpoints::install)
void host_frame(u64 now_ms); // host, main thread: checkpoints::frame
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
// Host: `character` says he put up a tent at `pos` (see the top of the file).
void host_claim_tent(u16 character, const Fvector& pos);
// Any machine: coop.checkpoint.claim_tent. True if the claim was handed to the host.
bool claim_tent(const Fvector& pos);
} // namespace sources
} // namespace checkpoints
} // namespace coop
