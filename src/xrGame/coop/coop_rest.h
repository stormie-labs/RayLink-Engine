// Anomaly Coop — shared sleep on the host (F3, docs/CHECKPOINTS_DEATH.md §4). Time in the world is shared: a player
// lies down at his rest spot (RestRequest) and dozes; the host skips the time for everybody (RestSleep) only when every
// present player rests, nobody is in a fight and no emission runs (rest::decide, xrCoop/coop_rest_rules.h), and
// tells everybody who rests (RestState).
//
// Skeleton of F3 Task 2; F3 Task 11 fills it.
#pragma once

#include "xrCoop/coop_rest_rules.h"

struct lua_State;

namespace coop
{
class Session;

namespace rest
{
void install(Session& s); // registers the message handlers (sync::install)
void reset(); // session started / ended (sync::on_session_started, sync::reset)
void frame(); // main thread, every frame (sync::frame)
void print_status(); // coop_status section (sync::print_status)
void register_console(); // from coop::register_console_commands
// Fills coop.rest; called by coop_checkpoints_script_registrator once the `coop` table exists.
void register_lua(lua_State* L);

// Rest state of the player whose character has `local_id` on this machine (0 = me). Awake if unknown.
State state_of(u16 local_id);
} // namespace rest
} // namespace coop
