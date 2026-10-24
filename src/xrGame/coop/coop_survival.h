// Anomaly Coop — player survival (WP8, F3 Task 8): down instead of dead, revive by a teammate, bleed-out, the death
// modes checkpoint / hardcore (SPEC R-10.4, DESIGN §6.12, docs/CHECKPOINTS_DEATH.md §3).
//
// Life of a player. The owner decides when it falls and when it bleeds out; the host decides what
// happens after death (it owns the rules, xrCoop/coop_death_rules.h) and checks every revive (the medkit
// is a world item).
//
//   Alive --health would reach 0--> Wounded --teammate holds USE with a medkit (host checks)--> Alive
//   Wounded --bleed-out timer ends / player holds USE to give up--> Dead
//   Dead --host, coop_death_mode hardcore--> Spectating (out of the run, camera on teammates)
//        --the owner holds USE 3 s / coop_new_character (F3 Task 10)--> a new character: all he had goes into a stash at
//        the body; a client then joins again and chooses one (protocol 8), the host player is ordered up at his
//        arrival point
//   Dead --host, checkpoint: losses (coop_death), money, Evacuate; after coop_respawn_delay--> Respawning
//        --> Alive at the player's own checkpoint (coop_checkpoints::host_respawn_point)
// While evacuated the dead player watches a living teammate (Fire / Aim switch), or the place of death from
// above when nobody is alive. On standing up the owner wears its slot items (coop_death_wear) and fires the
// Lua callback coop_respawned(kind, contusion_s, money_pct, rank_pct, seq): kind is the checkpoint kind name
// ("campfire", "arrival", ..., "none" for no camp); the money is already taken by the host (death ledger) and how much
// does not reach the owner, so Lua can only say "up to money_pct %"; seq is the number of the fall (g_me.seq) the respawn
// answers: the same seq again is the same fall (an order the host sent twice), another one is a new fall. A new actor
// (level change, load) counts from 0 again, so the numbers mean something inside one world only (F3 T15 round 1, F3-12).
// Host: coop_group_wipe() once when every present player is dead / spectating / respawning, again only
// after somebody stood up.
//
// The death is counted once (death::Tracker): a player who leaves while dead or evacuated comes back alive
// at his checkpoint; nothing is taken from him a second time. The respawn a checkpoint death owes is kept with
// the character in the world ([coop_death] respawn = 1 in his custom data, death::debt_after_report) until the
// owner reports Respawning, so a world saved while he owed it (a level change, an autosave) puts him on his
// checkpoint at his first Alive there, and a new game or another save does not. Ruling F3-7: a character out of a
// hardcore run stays a spectator at every Alive, whatever the mode is now; the world keeps that too
// ([coop_death] out = 1, death::learn_out, F3 Task 10), so a level change or a load does not bring him back.
// The host's actor (character 0) takes the same path. F3a T7: and the same messages - the host player is a client of
// the world server in his own process (peer 0 over the LocalLink): his reports, revive requests and NewCharacter go to
// it over the internal channel and its orders and rules come back the same way, at the next pump (character_of_peer /
// peer_of_character below). Nothing of his is applied directly any more.
//
// Hardcore's new character (F3 Task 10). Out of the run, the owner holds USE 3 s (or coop_new_character): every player
// sends Msg::NewCharacter (the host player over the internal channel). The host checks he is out
// (death::on_new_character) and moves all he has, worn and quest items included, into a stash at the body
// (death::host_move_to_stash, DeathMarker). Only when
// the stash is done (death::retire_step, review R2 of T9) does anything else of him change: a client's character loses
// his [coop] key (retired = 1), his player joins again (join::host_rejoin_peer) and chooses a new character
// (coop_menu_char, protocol 8), the old one is released. The host player's actor stays in the world: the world starts his
// character over (not out, nothing owed, 0 RU) and orders his owner with a Respawn at his arrival point (F3a T7, see
// Respawn below: he stands up fresh - health 1, no radiation or bleeding, fed, rested); the world's Lua
// coop_host_new_character() offers him the choice (menu.char_offer to peer 0, coop_menu_char).
//
// The engine actor never dies during a session, in either mode: while the player is down its health is
// pinned just above zero (CActorCondition hook) and every kill of it is intercepted (CEntity::KillEntity),
// so the single-player death flow (Lua actor_before_death, CActor::Die, game over with load_last_save)
// never runs and the save keeps a living character (P4). Player proxies stay unharmable on every machine.
//
// Messages (ids in host numbering):
//   PlayerWounded  owner -> host -> all  {u16 player, u8 life, u16 seq, u32 bleed_left_ms, str nick} reliable
//                  on change and every 5 s; the host also repeats every player's state every 5 s (P5, late
//                  joiners, the client roster of player names). Life = Dead is the owner's "I bled out":
//                  the host answers by its death mode. (Msg::PlayerDied stays WP2's engine-death relay.)
//                  A client's Dead adds {u8 n, n x u16 item}: what it has in its slots and on its belt
//                  (Ruling F3-4, at most 40 ids; the host keeps the ones its proxy really carries and does not
//                  relay them).
//   PlayerRevive   {u8 op, ...}                                                                   reliable
//     Request  reviver -> host       {u16 target, u16 seq, u16 item}
//     Revive   host -> target owner  {u16 target, u16 seq, u16 reviver, f32 health}
//     Respawn  host -> target owner  {u16 target, u16 seq, vec3 pos, u8 checkpoint kind}
//              (to a Dead owner after the evacuation; to an Alive one that came back to the world while dead; F3a T7:
//              to a Spectating owner, only as his new character after a hardcore run - nothing of the death is charged)
//     Spectate host -> target owner  {u16 target, u16 seq}
//     Refused  host -> reviver       {u16 target, u8 reason}
//     Evacuate host -> target owner  {u16 target, u16 seq, u32 delay_ms}
//     HUD v2 part 2, the hold made visible (layouts, rate and machines: xrCoop/coop_revive_hold.h):
//     Begin    reviver -> host       {u16 target, u16 seq, u16 item, u16 held_ms}  the hold went on 250 ms, one per 2 s
//     Cancel   reviver -> host       {u16 target, u16 seq}
//     Reviving host -> target owner  {u16 target, u16 seq, u16 reviver, u32 reviver_peer, u32 total_ms, u32 held_ms}
//     Stopped  host -> target owner  {u16 target, u16 seq, u16 reviver}
//     Done     host -> reviver       {u16 target, u16 seq}  the Request went through (Lua coop_revive_done)
//              Lua reads it all as coop.revive_progress() (revive_progress below).
//   PlayerRules    host -> all  death::Rules::write (mode, drop, bleed-out, revive health, delay, contusion, %, and
//                  since build 10 the share multiplier; drop 3 = keep). F3a T7: the host player takes them too; the
//                  world server decides by its own (rules() on the host)
//   NewCharacter   client -> host  {}: out of a hardcore run, a new character (F3 Task 10; 2 per 5 s)  reliable
// `seq` counts the falls of one player (never 0xffff, the tracker's "none"); every order names the fall it
// answers, so repeats are no-ops.
// Console (host rules, CCC_Float unless a token): coop_death_mode checkpoint | hardcore (respawn, respawn_keep =
// checkpoint), coop_death_drop drop | corpse | lose | keep (build 10: nothing is lost), coop_bleedout s,
// coop_revive_health, coop_respawn_delay 0..300 s (30), coop_contusion 0..60 min (10), coop_death_money /
// coop_death_wear / coop_death_rank 0..100 % (10 / 10 / 0), coop_death_share 0..200 % (100; build 10: the loss shares
// of drop and lose x this / 100); coop_new_character (out of a hardcore run: as the hold of USE); coop_wound_me (test).
//
// The body while down (build 10): a wounded player's character lies in a wounded pose, a dead, spectating or
// respawning one in a dead pose (death::body_pose), his avatar on the other machines and his own actor alike (the own
// one is seen by the overhead and spectator cameras). While dead, spectating or respawning the mouse turns nothing
// (death::look_locked). Standing up gives the actor its own motions back.
#pragma once

#include "xrCoop/coop_session.h"

class CActor;
class CActorCondition;
class CEntity;
class CAI_Stalker;
class CInventoryOwner;

namespace coop
{
namespace death
{
struct Rules;
}

namespace survival
{
enum class Life : u8
{
	Alive = 0,
	Wounded = 1, // down, bleeding out, can be revived
	Dead = 2, // bled out: waiting for the host's decision, then evacuated (checkpoint mode)
	Spectating = 3, // hardcore: out of the run, watching teammates
	Respawning = 4, // respawn ordered, teleport pending
};
const char* life_name(Life l);
inline bool is_down(Life l) { return l != Life::Alive; }
// The death modes and the item loss modes are death::Mode / death::Drop (xrCoop/coop_death_rules.h).

// A player known on this machine (self included on request).
struct PlayerRef
{
	PeerId peer = kInvalidPeer;
	u16 host_id = 0xffff; // character id in host numbering (host actor = 0, client = its proxy)
	u16 local_id = 0xffff; // object id on this machine (0 = my own actor)
	xr_string nick;
	bool self = false;
};
void collect_players(xr_vector<PlayerRef>& out, bool include_self);
// Nick of the player whose character has `host_id`, "?" if unknown.
xr_string nick_of(u16 host_id);

// F3a T7, the world server (the host's process; elsewhere 0xffff / kInvalidPeer / false). Its players are the host player,
// peer 0 over the internal channel (LocalLink), who plays the host's actor (character 0), and the remote clients, who play
// their proxies. A handler of a player's message finds his character by the peer it came from, never by the packet; an
// answer to a character goes to his peer (net::host_send: peer 0 over the link).
u16 character_of_peer(PeerId peer); // 0xffff: no character of his in this world
PeerId peer_of_character(u16 character); // kInvalidPeer: nobody plays it now
// The player of `character` is in the served world: the host player while the host's level and actor are up and the
// world is served (R1: not in the frame a world was created in, as join::host_player_in_world keeps it for a client), a
// client once he has loaded it (join::host_player_in_world).
bool player_in_world(u16 character);

void install_server(Session& s); // F3a T4: the world server's handlers (on the hosting session)
void install_client(Endpoint& e); // F3a T4: the client's (a remote client's session, the host player's LocalLink)
void register_console(); // from coop::register_console_commands (console vars are host rules)
void frame(); // main thread, every frame
void reset(); // session started / ended
void print_status();
void render_hud(); // HUDManager::RenderUI (main thread, render phase): reads data prepared by frame()

// Life of the player whose character has `local_id` on this machine (0 = me). Alive if unknown.
Life life_of(u16 local_id);
// The death rules in force here: the host's console on the host, the host's last PlayerRules on a client (the client's
// own console until they come). Main thread.
const death::Rules& rules();
// Wounded players: milliseconds of bleed-out left (as last reported), 0 otherwise.
u32 bleed_left_ms(u16 local_id);

// HUD v2 part 2 (docs/HUD_V2.md "Поднятие союзника"): the holds of USE as this machine's Lua HUD draws them,
// coop.revive_progress(). Calling it tells the engine a Lua HUD is there, so its own down / revive text steps aside.
struct ReviveProgress
{
	// I hold USE on a wounded teammate (revive::kHoldMs, 3 s)
	bool reviving = false;
	bool reviving_sent = false; // the hold is over and the Request is out: the ring is full, waiting for the host's Done
	u16 target_local = 0xffff; // object id of the wounded on this machine
	PeerId target_peer = kInvalidPeer;
	xr_string target_nick;
	float reviving_fraction = 0.f; // 0..1
	u32 reviving_left_ms = 0;
	const char* item_kind = ""; // "medkit" | "stimpack" (prefix of the section I hold)
	xr_string item_section; // "medkit_army"
	// I am wounded
	bool wounded = false;
	u32 bleed_left_ms = 0, bleed_total_ms = 0;
	// ... and a teammate holds USE on me (the host's Reviving, animated here)
	bool revived_by = false;
	PeerId by_peer = kInvalidPeer;
	u16 by_local = 0xffff; // object id of the reviver's character on this machine
	xr_string by_nick;
	float by_fraction = 0.f;
	u32 by_left_ms = 0;
	// ... and I hold USE to give up
	bool giving_up = false;
	float giveup_fraction = 0.f;
	u32 giveup_left_ms = 0;
};
// False: nothing to show (no session, or neither reviving nor wounded).
bool revive_progress(ReviveProgress& out);
// Host (Ruling F3-4): what the owner of `character` had in his slots and on his belt when he reported his death `seq`,
// host numbering, sorted, each id once and carried by the character when the report came (at most 40). False when
// no report of that death brought the list (the host's own actor: its slots and belt are seen directly).
bool host_worn_items(u16 character, u16 seq, xr_vector<u16>& out);

// --- engine hooks ---

// CActorCondition::UpdateCondition, first line and right after inherited::UpdateCondition().
// In a live session a lethal health value becomes the Wounded state; while down health stays pinned.
// May run on the GameThread worker: it only touches the condition and atomics.
void actor_condition_hook(CActorCondition& c);
// CActorCondition::UpdateCondition: true in a live session (the single-player death effector would end
// in health -1 and the stock game-over flow).
bool suppress_death_effector();
// CEntity::KillEntity (lead hook): a scripted or engine kill of the local actor during a session.
// True = handled (the actor falls wounded instead), the caller must return.
bool intercept_actor_kill(CEntity* victim, u16 killer_id);
// CActor::ActorUse (lead hook) before talking to the person we look at. True = consumed (revive hold
// started on a wounded player, or refused with a hint).
bool on_use_person(CInventoryOwner* person);
// players::drive_avatar (lead hook): a wounded / dead player's avatar plays a lying pose instead of the
// owner's motions. True = pose handled, skip the snapshot motions.
bool avatar_pose_override(CAI_Stalker* avatar);
// Build 10 (the owner's live test of build 9: "I do not lie, and the mouse turns me"). My own actor lies as my avatar
// does on the other machines (death::body_pose; frame() plays the pose). CActor::g_SetAnimation (lead hook, first
// line): true while that pose holds on `actor` - its own motions must not play over it (it plays a new cycle whenever
// its MotionID changes: the weapon put away at the fall, a turn, a landing). May run on the GameThread worker (the
// scheduler): it reads one atomic.
bool own_pose_held(const CActor* actor);
// CActor::IR_OnMouseMove (lead hook, before the camera moves): true while my player is dead, out of the run or being
// respawned (death::look_locked) - the mouse turns neither the camera nor the body. A wounded player looks around.
bool look_locked();
} // namespace survival
} // namespace coop
