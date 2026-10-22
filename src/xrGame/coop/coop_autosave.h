// Anomaly Coop — the host's own autosaves (F3, docs/CHECKPOINTS_DEATH.md §7): players have no save menu; the host
// writes coop_auto_N on a level change, after the shared sleep, every 10 minutes and at the end of the session, keeping
// the newest 10. Slot names, rotation and the timer: xrCoop/coop_autosave_rules.h.
//
// How a save is written (F3 Task 12): the world goes out as the same M_SAVE_GAME the join flow sends, to the temporary
// name coop_auto_tmp (.scop from the engine, .scoc from Anomaly's Lua), and only a finished pair is moved over
// coop_auto_N in one step each. A crash in the middle of a write leaves coop_auto_tmp.* behind, never a cut-off slot
// (the Restore list opens every slot, and CSavedGameWrapper is fatal on a cut-off file). The level change save is written
// by CALifeUpdateManager::change_level itself, to the same temporary name, and moved by level_change_commit().
// After a save the oldest slot files beyond coop_autosave_count are deleted (slots_to_prune).
//
// Console (host settings): coop_autosave_interval (minutes, 0 = off, default 10), coop_autosave_count (1..50, default
// 10), coop_autosave_now.
#pragma once

#include "xrCoop/coop_autosave_rules.h"

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

// Host in a session: write a save at the next safe frame (the host's character is up and awake, the level is not
// coming up). A save that waits for that longer than 2 minutes is written anyway; a request that finds no way to save
// for 5 minutes is dropped. Several requests before the save make one save. Anywhere else (a client, no session): nothing.
void request(Reason r);

// The save of a level change (CALifeUpdateManager::change_level), three calls in order:
//   level_change_name()         "coop_auto_N" for the host in a session (the slot is taken now, coop_auto_tmp is
//                               cleaned), nullptr otherwise: then the stock name stays and the two calls below are not made.
//   level_change_temp_name()    the name to write the save to ("coop_auto_tmp"); the new level loads from "coop_auto_N".
//   level_change_commit()       the save is written: moves it over the slot. false = it could not be moved, the world is
//                               only in level_change_temp_name(): the new level has to load from there.
const char* level_change_name();
const char* level_change_temp_name();
bool level_change_commit();

// Manager::stop on the host, before the session is shut down: the last save of the session, at once, whatever the
// host is doing (only a level that cannot be saved at all skips it). Nothing on a client.
void host_on_session_end();

// CLevel::net_Stop on the host, first thing, while the level is alive and ALife answers: the host leaves to the menu
// or closes the game with the session still running (Manager::stop is not called on these ways). The same last save as
// host_on_session_end. Not when a reconnect is queued (a level change, a load, a new game): the level is not closed for
// good then, and a restore must not rotate an autosave out of the way. Nothing on a client, nothing without a session.
// No second save for one exit: Manager::stop leaves the host's phase (this call then does nothing), and once the level
// is gone host_on_session_end finds nothing to save.
void host_on_level_exit();
} // namespace autosave
} // namespace coop
