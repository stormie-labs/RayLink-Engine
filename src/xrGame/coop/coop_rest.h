// Anomaly Coop — shared sleep on the host (F3, docs/CHECKPOINTS_DEATH.md §4). Time in the world is shared: a player
// lies down at his rest spot (RestRequest) and dozes; the host skips the time for everybody (RestSleep) only when every
// present player rests, nobody is in a fight and no emission runs (rest::decide, xrCoop/coop_rest_rules.h), and
// tells everybody who rests (RestState).
//
// F3 Task 11. The host keeps the book (rest::Group) in host numbering; a client only asks and mirrors.
//   Lie down (RestRequest op 1, or the host's own Lua): checked in this order (rest::check_lie_down): the player is
//     Alive (survival::life_of; wounded, dead, respawning: "dead"), the group is not asleep ("busy"), and unless forced
//     (passing out of tiredness) he is not in a fight ("combat", checkpoint_sources::in_combat) and stands at a rest
//     place ("place", checkpoint_sources::rest_place). Then he is Resting with his hours (1..24); a rest that is not
//     forced always comes with that place as his checkpoint (checkpoints::host_set, kind Rest / Tent / Campfire / Mod):
//     a camp the store refuses refuses the lie-down ("camp", T11 review M2). A forced rest has no camp and no place or
//     fight check. A refused lie-down ends the rest he was waiting in (an older lie-down elsewhere: review M1); a
//     running sleep is kept.
//   Get up (op 2, or the own Lua): Awake.
//   Every 500 ms the host looks at the players present: the host and every player whose character is in the world
//     (join::host_player_in_world), not a hardcore spectator (Spectating, out of the run). A rest ends without the
//     player's word when he is not present any more (left, loading the world again: sync::on_peer_left does not reach
//     the F3 modules, this poll does), not alive ("dead") or a fight began for him ("combat"; the fight a forced rest
//     started in does not count). Then decide(present, world block). When it says start, every rest that is not forced
//     is looked at once more (rest_place, once per sleep, review M1): one who walked away from his camp loses his rest
//     ("place") and nobody sleeps on this tick. Else everybody present is Sleeping, the host
//     marks a TimeSync (worldstate::intercept_time_jump), skips the hours as stock change_game_time does (environment
//     and ALife clock), sends RestSleep to every sleeper (its own Lua: coop_rest_sleep), asks for an autosave
//     (autosave::request(Reason::Sleep): written once the host is awake again), and 12 s later (rest::kSleepMs)
//     everybody asleep is awake. The sleep also ends as soon as nobody sleeps any more.
//   A new world (a load or level change: join::host_world_epoch) or the session's start / end wakes everybody (Review
//     Focus 2).
//
// Messages (ids in host numbering; layouts in xrCoop/coop_rest_rules.h):
//   RestRequest  client -> host  {u8 op, u8 hours, u8 forced}                          reliable; the session takes 4,
//                then 1 per second (coop_limits.h); a client paces itself below that and sends only its newest word
//   RestState    host -> all     {u8 block, u8 n, n x {u16 character, u8 state, u8 hours}, u8 refused}   reliable
//                on change and every 5 s, every present player listed. `refused` is set only in the copy that goes to
//                one player: his lie-down was refused or his rest ended without his word.
//   RestSleep    host -> every sleeper {u8 hours}                                     reliable, after the RestState
//                that says Sleeping
//
// Lua coop.rest (any machine in a session unless said; ids are local, 0 = me):
//   lie_down(hours [, forced]) -> bool   asks to rest (false outside a session or for hours that are not a number)
//   get_up() -> bool
//   state_of(local_id) -> "awake" | "resting" | "sleeping"   ("awake" outside a session)
//   hours_of(local_id) -> hours he rests for, 0 when awake
//   waiting_for() -> { nick, ... }        the present players who are awake: the sleep waits for them
//   block() -> "none" | "not_all" | "combat" | "world" | "dead"   what holds the sleep while somebody rests
//   set_world_block(reason) -> bool       host (or no session): "" lets the sleep go, any other string holds it
//                                         (an emission, a story scene); it belongs to the world it was set in
// Callbacks (SendScriptCallback, through script::queue_callback: a shared bounded queue, a hint only — Lua re-reads
// coop.rest.state_of):
//   coop_rest_changed(local_id, state, hours)  every machine, for every change of a present player (a player gone or
//     a world forgotten: "awake", 0). After a new world they go out once this machine is back in it.
//   coop_rest_sleep(hours)                      every sleeper's machine: run the stock sleep effects (no time skip)
//   coop_rest_refused(reason)                   the player's own machine: "place" | "combat" | "dead" | "busy" |
//     "camp" (review M2: no checkpoint could be set where he lies). A new reason may be added later: an unknown one is
//     shown as a refusal all the same.
// One update gives one order on the host and on a client (rest::news, review M4): the coop_rest_changed first, then
// coop_rest_refused. The end of the session sends no callback (script::reset drops the queue): a doze window polls state_of(0) and
// closes on "awake", which is all a machine outside a session answers.
// Console: coop_rest_status.
#pragma once

#include "xrCoop/coop_rest_rules.h"

struct lua_State;

namespace coop
{
class Session;
class Endpoint;

namespace rest
{
void install_server(Session& s); // F3a T4: the world server's handlers (on the hosting session)
void install_client(Endpoint& e); // F3a T4: the client's (a remote client's session, the host player's LocalLink)
void reset(); // session started / ended (sync::on_session_started, sync::reset)
void frame(); // main thread, every frame (sync::frame)
void print_status(); // coop_status section (sync::print_status)
void register_console(); // from coop::register_console_commands
// Fills coop.rest; called by coop_checkpoints_script_registrator once the `coop` table exists.
void register_lua(lua_State* L);

// Rest state of the player whose character has `local_id` on this machine (0 = me). Awake if unknown.
State state_of(u16 local_id);
// The hours he rests for (1..24), 0 when awake or unknown.
u8 hours_of(u16 local_id);
// Host: the rest of `character` (host numbering) ends now; his machine gets coop_rest_refused(reason) ("place" |
// "combat" | "dead" | "busy" | "camp"; another reason is sent as "busy"). Nothing if he does not rest, or on a client.
void host_cancel(u16 character, const char* reason);
} // namespace rest
} // namespace coop
