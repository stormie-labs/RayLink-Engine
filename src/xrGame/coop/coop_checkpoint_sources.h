// Anomaly Coop — where a checkpoint may come from, on the host (F3 Task 7, docs/CHECKPOINTS_DEATH.md §2.2): lit
// campfires and objects mods declare as camps (stood next to one for campfire_dwell_ms, not in a fight), a player's own
// tent or sleeping bag (CheckpointClaim), the stock rest zones and surge covers; and who is in a fight. Driven by
// coop_checkpoints: its install_server / install_client, reset and print_status call these, its frame calls host_frame
// on the host. Settings: configs\mod_system_coop_checkpoints.ltx ([coop_checkpoints], [coop_rest_zones], [coop_bases]; pure rules and bounds:
// xrCoop/coop_checkpoint_source_rules.h; the claim and its answer: xrCoop/coop_checkpoint_store.h).
//
// Campfires and mod objects: every 500 ms the host and every living player in the world (join::host_player_in_world)
// is checked against the nearest lit CZoneCampfire within campfire_radius, or a registered mod object within its own
// radius; checkpoint::dwell_update decides, the camp goes through checkpoints::host_set (kind Campfire / Mod, the
// object as its source, the player's own position).
//
// Bases (Build 57, owner 2026-11-18: "on any base the respawn must be at the bed, not at the campfire, and no 'checkpoint
// updated' every N-th time while we are on a base"; pure rules xrCoop/coop_checkpoint_base_rules.h): [coop_bases] names
// the stock sleep zones that stand in a real base, the radius of its ground and its owner communities (round 2: up to 6;
// round 3, review B1: the first owner is the holder, the faction whose guards hold the place; the base is hostile when the
// holder is his enemy, or every known owner is; his community is his own actor_<f>, review S4). The zones are looked up in
// ALife when the world is served (the log
// names every row not found). In the same 500 ms tick, before the
// campfires: a player on the ground of a base not hostile to him (the same level, within the radius and 10 m in
// height; he leaves it beyond radius + base_leave_margin) has its bed as his checkpoint after base_dwell_ms there out of a
// fight (kind Rest, the sleep zone as its object), once; nothing when it is his checkpoint already or his own tent stands
// on that base. While he is on it, its campfires and mod objects are no sources. A lie-down on it writes the bed
// (base_bed_record; who may lie down is rest_place's, unchanged). A respawn whose checkpoint is the bed of a base hostile
// to him now takes the previous record on his level unless that one is a hostile base's too, without one he comes back where
// a player without a checkpoint does (round 2/3; bed_hostile, checkpoint::base::skip_hostile_bed,
// checkpoints::host_respawn_point). A story checkpoint of a living player on a friendly base writes the base's record (round 2;
// checkpoints::host_story). A base's record written over an Arrival record on that base's ground takes its place and keeps
// the record before it (round 3, review S3; base_ground, checkpoint::base::set_on_base in checkpoints' apply_set).
// Build 57 (owner 2026-11-18: "for the bases that are not covered, just put the point at the place of the first spawn";
// "in split maps in the console form this must be done too"): a start location named in [coop_start_bases] (round 2: opt-in,
// only real bases without a sleep zone; owners from its row) whose ground no sleep zone base or earlier start base covers is
// a base too (record kind Rest without an object at its point; the position from base_start_locations, GAMMA
// plugins\new_game_start_locations.ltx). The list of a level is built with checkpoint::base::accept (overlapping rows are
// dropped). Who decides, per mode (CLAUDE.md: two modes):
//   Co-op (the host plays, everybody on its level): the host, base_tick, by his position on this level's list;
//   Server (a world server and map servers): the world server, far_base_tick, by the footing the map server of his level
//   reports in LevelBodyFacts (map_server_footing: geometry only), checked against the world server's own tables
//   (host_footing: checkpoint::base::footing_check) and usable only while it is fresh and of his level now
//   (levels::body_facts, checkpoint::base::footing_on_level) - the same pure rules, the hostility and the records are the
//   world server's.
// A map server decides nothing and writes no record; single player and clients run none of it.
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
#include "xrCoop/coop_checkpoint_base_rules.h" // Build 57 (owner 2026-11-18): checkpoint::base::Footing

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
void map_server_scan(u64 now_ms); // a map server: its level's campfires and tents, for rest_place
void player_frame(); // F3a T7, a player, main thread (checkpoints::frame): the host player's claim goes out
void reset(); // checkpoints::reset
void print_status(); // checkpoints::print_status
void print_level_status(); // the local scan summary, after the common tent-claim summary

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
// World server: a map server put up tent `tent` for `character` on its level: its owner into the book, his camp on it.
void host_tent_up(u16 tent, u16 character);
// Host: the character who owns tent `id` (by the key on it), 0xffff if nobody's or he is not in this world.
u16 host_tent_owner(u16 tent);
// Any machine: coop.checkpoint.claim_tent. True if the claim was handed to the host.
bool claim_tent(checkpoint::ClaimWhat what, const Fvector& pos);
// Any machine: an object of this section is a sleeping bag a player sleeps on (sleepbag_section; live 2026-10-25).
bool is_sleeping_bag(const shared_str& section);
// Any machine: an object of this section is a tent players put up (tent_section).
bool is_tent(const shared_str& section);
// Build 57 (owner 2026-11-18): host (the world server): `character` stands on a friendly base: `out` is the record of that
// base (what a lie-down there writes instead of the spot he lies at). Online here (co-op, the host's level): by his position
// and this level's list of bases; on a map server's level (the Server mode): by the footing that
// map server reported (host_footing). False: on none (or not the host, or a map server).
bool base_bed_record(u16 character, checkpoint::Record& out);
// Build 57: host: `r` is the record of a base (a sleep zone's, or a start base's: kind Rest without an object within
// kSamePlaceM of its point) hostile to `character` now (round 3, review B1: its holder, the first owner of its row, is his
// enemy, or every owner is; the respawn does not use it: skip_hostile_bed, for the current and the previous record). Needs no
// level geometry: the owners come from this process's tables, in every mode.
bool bed_hostile(u16 character, const checkpoint::Record& r);
// Build 57 round 3 (review S3): host (the world server): `r` is the record of a base of this world (a [coop_bases] zone found
// in it, or a start base that is not merged): `out` is that base's ground for checkpoint::base::set_on_base (its key, object,
// level, its point - the record's own point for a zone, the start location's for a start base - and radius). False for any
// other record, on a map server and without a world. Needs no level geometry: both modes.
bool base_ground(const checkpoint::Record& r, checkpoint::base::Spot& out);
// Build 57 round 3 (review S6): any machine that tells its Lua of a record (coop_checkpoint_changed): what the record is for
// the text, checkpoint::base::mark_of -> "bed" (a sleep zone base's), "base" (a start base's), "none" (neither), nullptr (this
// machine cannot tell). The host (world server) by its own tables; a client without ALife: a record without an object near
// the ltx point of a [coop_start_bases] row is a base, a record with an object by that object on this machine
// (`source_local`, its name a row of [coop_bases]); an object this machine does not hold gives nullptr.
const char* record_base(const checkpoint::Record& r, u16 source_local);
// Build 57: a map server (client_body): the base whose ground the body of `character` at `pos` stands on, by geometry only
// (this level's list of bases, none hostile: hostility is the world server's), with the base's point; kind none otherwise.
// Decides and writes nothing.
void map_server_footing(u16 character, const Fvector& pos, checkpoint::base::Footing& out);
// Build 57: the world server, a body report of the map server of `level` (coop_levels_xfer.cpp): `f` names a base of this
// world on that level (a [coop_bases] zone of its ALife, or a start location that is not merged), its game vertex is on that
// level and its point is on the base's ground. Round 3 (review S5): `body`, the body position of the same report, is on that
// ground too (checkpoint::base::footing_check). False: nothing to keep (kind none), or refused (counted, the first few logged).
bool host_footing(const checkpoint::base::Footing& f, u16 level, const float body[3]);
} // namespace sources
} // namespace checkpoints
} // namespace coop
