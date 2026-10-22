#include "stdafx.h"
#include "pch_script.h"
#include "coop_checkpoints.h"
#include "coop_rest.h"

// Lua side of the F3 modules: coop.checkpoint (here, F3 Tasks 6 and 7) and coop.rest (coop_rest.cpp, F3 Task 11).
// Functions with arguments are plain C functions, as in coop_script.cpp: a wrong argument from an addon returns
// nil / false instead of a luabind "no matching overload" error, which is fatal in this engine (R-11.4).

namespace coop
{
namespace checkpoints
{
void register_lua(lua_State*) {} // F3 Task 6: coop.checkpoint.get / story; Task 7: register_source, claim_tent
} // namespace checkpoints
} // namespace coop

#pragma optimize("s", on)
void coop_checkpoints_script_registrator::script_register(lua_State* L)
{
	// export_classes runs this right after coop_script_registrator, which made the `coop` table.
	lua_pushstring(L, "coop");
	lua_rawget(L, LUA_GLOBALSINDEX);
	const bool have_coop = lua_istable(L, -1);
	lua_pop(L, 1);
	if (!have_coop)
	{
		::Msg("! [coop] script namespace 'coop' was not created, coop.checkpoint and coop.rest unavailable");
		return;
	}
	coop::checkpoints::register_lua(L);
	coop::rest::register_lua(L);
}
