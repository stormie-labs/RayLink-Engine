#include "stdafx.h"
#include "coop_autosave.h"

// F3 Task 2 skeleton: slots, the timer, the sleep / level change / session end saves come with F3 Task 12.

namespace coop
{
namespace autosave
{
void install(Session&) {}

void reset() {}

void frame() {}

void print_status() {}

void register_console() {}

const char* level_change_name() { return nullptr; }
} // namespace autosave
} // namespace coop
