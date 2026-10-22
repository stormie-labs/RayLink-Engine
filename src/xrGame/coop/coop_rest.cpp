#include "stdafx.h"
#include "coop_rest.h"

// F3 Task 2 skeleton: RestRequest / RestState / RestSleep, the sleep decision and coop.rest come with F3 Task 11.

namespace coop
{
namespace rest
{
void install(Session&) {}

void reset() {}

void frame() {}

void print_status() {}

void register_console() {}

void register_lua(lua_State*) {}

State state_of(u16) { return State::Awake; }
} // namespace rest
} // namespace coop
