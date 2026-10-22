// Anomaly Coop — the host's own autosaves (F3, docs/CHECKPOINTS_DEATH.md §7): players have no save menu; the host
// writes coop_auto_N on a level change, after the shared sleep, every 10 minutes and at the end of the session, keeping
// the newest 10. Slot names, rotation and the timer: xrCoop/coop_autosave_rules.h.
//
// How a save is written (F3 Task 12): the world goes out as the same M_SAVE_GAME the join flow sends, to the temporary
// name coop_auto_tmp (.scop from the engine, .scoc from Anomaly's Lua), and only a finished pair is moved over
// coop_auto_N in one step each. A crash in the middle of a write leaves coop_auto_tmp.* behind, never a cut-off slot
// (the Restore list opens every slot, and CSavedGameWrapper is fatal on a cut-off file). The one save that is written
// straight to its slot is the level change one: CALifeUpdateManager::change_level writes it itself.
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

// The name of the save CALifeUpdateManager::change_level writes: "coop_auto_N" for the host in a session (the slot
// is taken now), nullptr otherwise (the stock name stays).
const char* level_change_name();

// Manager::stop on the host, before the session is shut down: the last save of the session, at once, whatever the
// host is doing (only a level that cannot be saved at all skips it). Nothing on a client.
void host_on_session_end();
} // namespace autosave
} // namespace coop
