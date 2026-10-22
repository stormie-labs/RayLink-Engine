// Anomaly Coop — what a player's death costs, on the host (F3, docs/CHECKPOINTS_DEATH.md §3.3): a share of the
// backpack drops at the body, stays in a stash or is gone (death_drop), and every player sees where (DeathMarker).
// Rules, item categories and the death tracker: xrCoop/coop_death_rules.h; the death flow itself: coop_survival.
//
// Skeleton of F3 Task 2; F3 Task 9 fills it (settings: configs\mod_system_coop_death.ltx), Task 10 adds hardcore's
// NewCharacter.
#pragma once

namespace coop
{
class Session;

namespace death
{
void install(Session& s); // registers the message handlers (sync::install)
void reset(); // session started / ended (sync::on_session_started, sync::reset)
void frame(); // main thread, every frame (sync::frame)
void print_status(); // coop_status section (sync::print_status)
void register_console(); // from coop::register_console_commands
} // namespace death
} // namespace coop
