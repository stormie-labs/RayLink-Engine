#include "stdafx.h"
#include "coop_checkpoints.h"
#include "coop_checkpoint_sources.h"

// F3 Task 2 skeleton: the host store, CheckpointState and the arrival / story checkpoints come with F3 Task 6.

namespace coop
{
namespace checkpoints
{
void install(Session&) {}

void reset() { sources::reset(); }

void frame() {}

void print_status() { sources::print_status(); }

void register_console() {}
} // namespace checkpoints
} // namespace coop
