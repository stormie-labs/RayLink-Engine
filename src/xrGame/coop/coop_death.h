// Anomaly Coop — what a player's death costs, on the host (F3, docs/CHECKPOINTS_DEATH.md §3.3): a share of the
// backpack goes into a bag at the body (drop, Ruling L-1), the whole backpack into a stash (corpse) or a share is gone
// (lose) (death_drop, death::plan_losses), and every player sees where (DeathMarker).
// Rules, item categories and the death tracker: xrCoop/coop_death_rules.h; which items are candidates, the drop point
// and the marker's wire form: xrCoop/coop_death_items.h; the death flow itself: coop_survival.
//
// Settings: configs\mod_system_coop_death.ltx (merged into system.ltx), read when a session starts:
//   [coop_death_drop]  the share lost per category; [coop_death_kinds] item ltx `kind` -> category;
//   [coop_death_stash] the stash container (the stock backpack stash).
// Host: everything here moves items with the engine's own events, as a host deal over a proxy does (coop_trade): the
// server relays the parent changes (EntityParent) and the ownership mirror carries them to the player's machine.
//   Drop   - Ruling L-1 (2026-10-23, the owner's playtest of build 7): the share goes into a coop_death_stash at the body,
//            the Corpse way below with all its fallbacks; the DeathMarker names the stash and keeps the mode byte drop.
//            Loose only when no stash can be made or it does not come online in time: GE_OWNERSHIP_REJECT from the
//            character, item by item; a body that fell through the ground (or has no valid level vertex) has them put
//            0.3 m above its level vertex (GE_CHANGE_POS, exact drop position for clients);
//   Lose   - DestroyObject;
//   Corpse - a coop_death_stash is spawned at the body (ALife, relayed as every host spawn); once it is online
//            (within 5 s) the items go in by GE_TRADE_SELL from the character + GE_TRADE_BUY to the stash. A stash that
//            does not come online in time is released and the items drop at the body instead. If the player leaves
//            first, the move waits for him (the 5 s start again when he is back): as soon as the character and the stash
//            are both offline ALife moves the items (detach + attach), at any time, also after 10 min; only while the
//            host stays near the body (the stash online, the character offline) is it given up 10 min after the stash was
//            made, and the items stay with him. A stash still unfilled when the session ends or the world is replaced is
//            released if it is still an empty coop_death_stash (abandon_stashes).
//            The stash checks it is still for the same character (same_owner): by the key when both have one,
//            otherwise by the player who plays it. Task 10 may rewrite the key of a retiring character (no key left:
//            the player decides), a player who leaves takes his peer away (the key decides).
//            L-1 (carried from F3 T15): the stash names its owner in its custom data when it is spawned
//            ([coop_death_stash] nick = "<nick>", key = <digest>; stash_owner_to_ini), for the host's Lua marks after
//            a load; once a second the host releases every death stash on its level that is empty, not waiting for its
//            items and not open in anybody's window, the host's or a client's (release_empty_bag,
//            trade::host_container_open; L-1 round 1, I1): the stock frees only inv_backpack.
// Never taken: quest items (IsQuestItem, kind i_quest / i_letter), the PDA, helper items, host-only objects. A client's
// proxy does not show the host where its player keeps his things (an NPC cannot wear an outfit, has no belt and fills
// its slots in its own order). The owner's Dead report brings the ids of what he has on (Ruling F3-4, F3 T8b,
// survival::host_worn_items): then the proxy offers everything it carries except those ids, as the host's own actor
// offers its backpack. Without the list (an old client, a refused packet) nothing the player may have on (slot or belt
// items) is taken, as before.
// Msg::DeathMarker (host -> all, the frame after the move, so the EntityParent / EntitySpawn of the items is ahead of
// it): u16 character, u16 marker object, u8 drop mode. Every machine (the host too) fires the Lua callback
// coop_death_marker(player_local_id, marker_local_id, "drop" | "corpse"). Lose has no marker.
// Console (host): coop_death_preview [character id, default 0 = the host [death seq]] logs what a death would take now.
//
// Skeleton of F3 Task 2; filled by F3 Task 9. Hardcore's new character (F3 Task 10, coop_survival) retires the old one
// with host_move_to_stash(character, true), host_stash_pending and host_release_character.
#pragma once

#include "xrCoop/coop_death_rules.h"

namespace coop
{
class Session;

namespace death
{
constexpr u16 kNoSeq = 0xffff; // no death seq (the owner's seq counter skips it)

void install(Session& s); // registers the message handlers (sync::install)
void reset(); // session started / ended (sync::on_session_started, sync::reset)
void frame(); // main thread, every frame (sync::frame)
void print_status(); // coop_status section (sync::print_status)
void register_console(); // from coop::register_console_commands

// --- host, main thread. `character`: 0 = the host's actor, else a player's proxy (host numbering) ---

// What `character` loses for his death `seq` (death::Tracker decides it once per death) by `mode` (plan_losses): Drop
// and Lose take floor(n x share) (+1 by chance) of each category of his backpack, Drop into a stash at the body (Ruling
// L-1), Lose destroys them; Corpse puts the whole backpack into a stash. Returns the number of items destroyed or on
// their way to the stash (or dropped at the body when no stash could be made). DeathMarker (the stash, mode drop or
// corpse; the first dropped item when they fell loose) goes to everybody once they are in. 0 for anything that is not
// a player character online.
// `seq` is the death of the client's Dead report: it picks the list of worn items (see above).
u32 host_apply_losses(u16 character, u16 seq, Drop mode);
// Spawns a coop_death_stash at the body of `character` and moves into it his backpack (`everything` false: Corpse) or
// all he has, worn and quest items included (`everything` true: a hardcore character retires, Task 10). Returns the
// stash id, 0xffff if none was made (nothing to move, or the items dropped at the body instead). The items go in when
// the stash is online (up to 5 s): see host_stash_pending. Sends DeathMarker (corpse) when they are in.
// `seq` (kNoSeq: none) is the death whose list of worn items a client's proxy is read with when `everything` is false;
// without a list (no seq, no report) nothing the player may have on is taken.
u16 host_move_to_stash(u16 character, bool everything, u16 seq = kNoSeq);
// True while a stash of `character` waits to be filled: his items are still on him. Task 10 changes nothing of a
// retiring character (his key, his player, his release) before this is false (review R2 of T9, death::retire_step).
bool host_stash_pending(u16 character);
// Fix round 1, I1: how the stash of `character` ended since his last host_move_to_stash: Pending while it waits, GaveUp if
// T9 gave it up (his things stayed on him), else Done (filled, dropped at the body, or none was needed).
StashOutcome host_stash_outcome(u16 character);
// The world items `character` carries in the server's state right now (ownership events change it at once).
u32 host_items_on(u16 character);
// F3 Task 10: a retired hardcore character (a client's, never the host's actor; his custom data says retired = 1,
// death::is_retired) leaves the world: offline through ALife at once, online by the server's GE_DESTROY (fix round 1,
// C1: his proxy is a remote puppet, DestroyObject sends nothing), done on a later frame. The caller confirms he is gone
// and asks again if not. False if no retired player character is under the id.
bool host_release_character(u16 character);
// Ruling F3-10: the new character of a retired key gets the faction's kit and money: console coop_hardcore_new_kit 1 or
// [coop_hardcore] new_kit = 1 in mod_system_coop_death.ltx (either turns it on). Off by default: he starts with nothing.
bool host_new_kit();
// The money part of a death (F2 T1: the host's ledger holds every balance): takes `pct` % (0..100, rounded down) of the
// balance of `character`'s player through money::host_adjust, which sends him the new balance. Returns the amount.
u32 host_take_money(u16 character, u8 pct);
// Console coop_death_preview: logs what a death of `character` would take now in each mode; moves nothing. `seq`
// (kNoSeq: none): for a client's proxy, the death whose reported list of worn items is applied.
void host_preview(u16 character, u16 seq = kNoSeq);
} // namespace death
} // namespace coop
