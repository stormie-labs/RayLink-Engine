// Anomaly Coop — the host's own autosaves (F3, docs/CHECKPOINTS_DEATH.md §7): players have no save menu; the host
// writes coop_auto_N on a level change, after the shared sleep, every 10 minutes and at the end of the session, keeping
// the newest 10. Slot names, rotation and the timer: xrCoop/coop_autosave_rules.h.
//
// How a save is written (F3 Task 12): the world goes out as the same M_SAVE_GAME the join flow sends, to the temporary
// name coop_auto_tmp (.scop from the engine, .scoc from Anomaly's Lua), and only a finished pair is moved to
// coop_auto_N. The move is one change for the pair: the slot's old pair is put aside, the new one moved in, the old one
// deleted; a failure puts it all back, so a slot holds the old pair or the new one, never one file of each. A crash in
// the middle of a write leaves coop_auto_tmp.* behind, never a cut-off slot (the Restore list opens every slot, and
// CSavedGameWrapper is fatal on a cut-off file). A slot whose files cannot be replaced (read-only, held by another
// process) is skipped and the next one takes the save. The level change save is written by
// CALifeUpdateManager::change_level itself, to the same temporary name, and moved by level_change_commit().
// After a save the oldest slot files beyond coop_autosave_count are deleted (slots_to_prune).
//
// Console (host settings): coop_autosave_interval (minutes, 0 = off, default 10), coop_autosave_count (1..50, default
// 10), coop_autosave_now. Lua: coop.autosave.suspend(on) for the Restore window.
#pragma once

#include "xrCoop/coop_autosave_rules.h"

struct lua_State;

namespace coop
{
class Session;

namespace autosave
{
void install(Session& s); // sync::install
void reset(); // session started / ended (sync::on_session_started, sync::reset)
void frame(); // main thread, every frame (sync::frame)
void print_status(); // coop_status section (sync::print_status)
void register_console(); // from coop::register_console_commands
// Fills coop.autosave (suspend); called by coop_checkpoints_script_registrator once the `coop` table exists.
void register_lua(lua_State* L);

// Host in a session: write a save at the next safe frame (the host's character is up and awake, the level is not
// coming up). A save that waits for that longer than 2 minutes is written anyway; a request that finds no way to save
// for 5 minutes is dropped. Several requests before the save make one save. Anywhere else (a client, no session): nothing.
void request(Reason r);

// The Restore window is open (on = true, every frame while it is): no timer or requested save is written, no slot is
// overwritten, the end-of-session save and the level change save stay out of the ring. The pause is a lease of 3 seconds:
// a window that is closed in any way stops renewing it and it lapses by itself. suspend(false) lets it go on purpose, and
// so does the end of the session (reset). When a pause ends, either way, the interval starts afresh and the requests made
// meanwhile are dropped: the host may have loaded a slot, and the first frames after the load must not write over one.
// Host only, nothing on a client. Lua: coop.autosave.suspend(on) -> bool.
void suspend(bool on);

// The save of a level change (CALifeUpdateManager::change_level), three calls in order:
//   level_change_name()         "coop_auto_N" for the host in a session (the slot is taken now, coop_auto_tmp is
//                               cleaned), nullptr otherwise: then the stock name stays and the two calls below are not made.
//                               nullptr also while the Restore window is open, or when coop_auto_tmp cannot be cleaned.
//   level_change_temp_name()    the name to write the save to ("coop_auto_tmp").
//   level_change_commit()       the save is written: moves it to its slot. Returns the name the new level has to load from:
//                               "coop_auto_N" (the slot from level_change_name(), or the next one if that one could not be
//                               replaced), "coop_auto_tmp" when no slot took the save (the world is only there), nullptr
//                               when nothing was written (the command line stays as it is).
const char* level_change_name();
const char* level_change_temp_name();
const char* level_change_commit();

// Manager::stop on the host, before the session is shut down: the last save of the session, at once, whatever the
// host is doing (only a level that cannot be saved at all skips it, and a Restore window that is open). Nothing on a client.
void host_on_session_end();

// CLevel::net_Stop on the host, first thing, while the level is alive and ALife answers: the host leaves to the menu
// or closes the game with the session still running. The same last save as host_on_session_end. Not when a reconnect is
// queued (a level change, a load, a new game): the level is not closed for good then, and a restore must not rotate an
// autosave out of the way. Nothing on a client, nothing without a session.
// No second save for one exit: the buttons of the main menu end the session first (coop_menu.script stop_session_first
// -> Manager::stop), which leaves the host's phase, so this call then does nothing; the console `disconnect`, Alt+F4 and
// closing the window reach only this one. Once the level is gone host_on_session_end finds nothing to save.
void host_on_level_exit();
} // namespace autosave
} // namespace coop
