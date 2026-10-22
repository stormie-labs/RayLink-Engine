// Anomaly Coop — where a checkpoint may come from, on the host (F3, docs/CHECKPOINTS_DEATH.md §2.2): lit campfires
// (5 s near one, not in a fight), a player's own tent (CheckpointClaim), stock rest zones, objects of mods; and who is
// in a fight. Driven by coop_checkpoints: its reset and print_status call these, its frame calls host_frame on the
// host (wired by F3 Task 7).
//
// Skeleton of F3 Task 2; F3 Task 7 fills it (settings: configs\mod_system_coop_checkpoints.ltx).
#pragma once

namespace coop
{
namespace checkpoints
{
namespace sources
{
void host_frame(u64 now_ms); // host, main thread: checkpoints::frame
void reset(); // checkpoints::reset
void print_status(); // checkpoints::print_status
} // namespace sources
} // namespace checkpoints
} // namespace coop
