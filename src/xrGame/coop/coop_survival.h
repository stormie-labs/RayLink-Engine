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
//   Dead --host, checkpoint: losses (coop_death), money, Evacuate; after coop_respawn_delay--> Respawning
//        --> Alive at the player's own checkpoint (coop_checkpoints::host_respawn_point)
// While evacuated the dead player watches a living teammate (Fire / Aim switch), or the place of death from
// above when nobody is alive. On standing up the owner wears its slot items (coop_death_wear) and fires the
// Lua callback coop_respawned(kind, contusion_s, money_pct, rank_pct): kind is the checkpoint kind name
// ("campfire", "arrival", ..., "none" for no camp); the money is already taken by the host (death ledger).
// Host: coop_group_wipe() once when every present player is dead / spectating / respawning, again only
// after somebody stood up.
//
// The death is counted once (death::Tracker): a player who leaves while dead or evacuated comes back alive
// at his checkpoint; nothing is taken from him a second time. The respawn a checkpoint death owes is kept with
// the character in the world ([coop_death] respawn = 1 in his custom data, death::debt_after_report) until the
// owner reports Respawning, so a world saved while he owed it (a level change, an autosave) puts him on his
// checkpoint at his first Alive there, and a new game or another save does not. Ruling F3-7: until F3 Task 10
// a character out of a hardcore run stays a spectator at every Alive, whatever the mode is now.
// The host's actor (character 0) takes the same path.
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
//              (to a Dead owner after the evacuation; to an Alive one that came back to the world while dead)
//     Spectate host -> target owner  {u16 target, u16 seq}
//     Refused  host -> reviver       {u16 target, u8 reason}
//     Evacuate host -> target owner  {u16 target, u16 seq, u32 delay_ms}
//   PlayerRules    host -> all  death::Rules::write (mode, drop, bleed-out, revive health, delay, contusion, %)
// `seq` counts the falls of one player (never 0xffff, the tracker's "none"); every order names the fall it
// answers, so repeats are no-ops.
// Console (host rules, CCC_Float unless a token): coop_death_mode checkpoint | hardcore (respawn, respawn_keep =
// checkpoint), coop_death_drop drop | corpse | lose, coop_bleedout s, coop_revive_health, coop_respawn_delay 0..300 s
// (30), coop_contusion 0..60 min (10), coop_death_money / coop_death_wear / coop_death_rank 0..100 % (10 / 10 / 0);
// coop_wound_me (test).
#pragma once

#include "xrCoop/coop_session.h"

class CActorCondition;
class CEntity;
class CAI_Stalker;
class CInventoryOwner;

namespace coop
{
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

void install(Session& s);
void register_console(); // from coop::register_console_commands (console vars are host rules)
void frame(); // main thread, every frame
void reset(); // session started / ended
void print_status();
void render_hud(); // HUDManager::RenderUI (main thread, render phase): reads data prepared by frame()

// Life of the player whose character has `local_id` on this machine (0 = me). Alive if unknown.
Life life_of(u16 local_id);
// Wounded players: milliseconds of bleed-out left (as last reported), 0 otherwise.
u32 bleed_left_ms(u16 local_id);
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
} // namespace survival
} // namespace coop
