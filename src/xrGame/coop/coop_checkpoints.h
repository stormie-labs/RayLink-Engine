// Anomaly Coop — per-player checkpoints on the host (F3, docs/CHECKPOINTS_DEATH.md §2). Every player has his own
// checkpoint (campfire, own tent, rest place, level arrival, story event); death sends him back to it and never rolls
// the world back. The host keeps them by character and decides every change; a client only reports (CheckpointClaim)
// and mirrors what the host sends (CheckpointState). Record format and rules: xrCoop/coop_checkpoint_rules.h.
//
// Skeleton of F3 Task 2: the lifecycle slots called from coop_sync / coop_console and the `coop.checkpoint` Lua table
// hook. The store, the messages and the Lua API come with F3 Task 6, the sources (coop_checkpoint_sources) with Task 7.
#pragma once

#include "script_export_space.h"

struct lua_State;

namespace coop
{
class Session;

namespace checkpoints
{
void install(Session& s); // registers the message handlers (sync::install)
void reset(); // session started / ended (sync::on_session_started, sync::reset)
void frame(); // main thread, every frame (sync::frame)
void print_status(); // coop_status section (sync::print_status)
void register_console(); // from coop::register_console_commands
// Fills coop.checkpoint; called by coop_checkpoints_script_registrator once the `coop` table exists.
void register_lua(lua_State* L);
} // namespace checkpoints
} // namespace coop

// Script export entry (export_classes, right after coop_script_registrator made the `coop` table): coop.checkpoint
// and coop.rest (coop_checkpoints_lua.cpp).
struct coop_checkpoints_script_registrator
{
	DECLARE_SCRIPT_REGISTER_FUNCTION
};
