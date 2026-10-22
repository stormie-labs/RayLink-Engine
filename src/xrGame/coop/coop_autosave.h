// Anomaly Coop — the host's own autosaves (F3, docs/CHECKPOINTS_DEATH.md §7): players have no save menu; the host
// writes coop_auto_N on a level change, after the shared sleep, every 10 minutes and at the end of the session, keeping
// the newest 10. Slot names, rotation and the timer: xrCoop/coop_autosave_rules.h.
//
// Skeleton of F3 Task 2; F3 Task 12 fills it.
#pragma once

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

// The name of the save CALifeUpdateManager::change_level writes: "coop_auto_N" for the host in a session (the slot
// is taken now), nullptr otherwise (the stock name stays).
const char* level_change_name();
} // namespace autosave
} // namespace coop
