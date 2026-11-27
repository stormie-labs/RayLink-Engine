#include "stdafx.h"
#include "pch_script.h"
#include "coop_checkpoints.h"
#include "coop_checkpoint_sources.h"
#include "coop_lua.h" // lua_api: the registrar, the argument readers

// Lua side of the checkpoints: coop.checkpoint (registered by script::register_raw, with coop.rest and coop.autosave).
// Functions with arguments are plain C functions, as in coop_script.cpp: a wrong argument from an addon returns
// nil / false instead of a luabind "no matching overload" error, which is fatal in this engine.

namespace coop
{
namespace checkpoints
{
namespace
{
constexpr size_t kMaxReason = 128; // a story reason is a string id ("st_save_..."), only logged


// coop.checkpoint.get(local_id) -> nil | {kind = "campfire"|"rest"|"tent"|"arrival"|"story"|"mod", level, x, y, z,
// source = local id | nil}: this player's own checkpoint (0 = me) or another one the host shares.
int l_get(lua_State* L)
{
	u16 id = 0;
	checkpoint::Record r;
	if (!lua_api::arg_object_id(L, 1, id) || !get(id, r) || !r.valid())
	{
		lua_pushnil(L);
		return 1;
	}
	lua_createtable(L, 0, 6);
	lua_pushstring(L, checkpoint::kind_name(r.kind));
	lua_setfield(L, -2, "kind");
	lua_pushstring(L, r.level.c_str());
	lua_setfield(L, -2, "level");
	lua_pushnumber(L, lua_Number(r.pos[0]));
	lua_setfield(L, -2, "x");
	lua_pushnumber(L, lua_Number(r.pos[1]));
	lua_setfield(L, -2, "y");
	lua_pushnumber(L, lua_Number(r.pos[2]));
	lua_setfield(L, -2, "z");
	if (r.source != checkpoint::kNoSource)
	{
		lua_pushnumber(L, lua_Number(r.source));
		lua_setfield(L, -2, "source");
	}
	return 1;
}

// coop.checkpoint.story([reason]) -> bool: host, a story checkpoint for every present living player (false on a
// client, without a session, with a reason that is not a string of at most 128 bytes, or when nobody got one).
int l_story(lua_State* L)
{
	const char* reason = "";
	const int t = lua_type(L, 1);
	if (t == LUA_TSTRING)
	{
		size_t n = 0;
		reason = lua_tolstring(L, 1, &n);
		if (n > kMaxReason)
		{
			lua_pushboolean(L, 0);
			return 1;
		}
	}
	else if (t != LUA_TNONE && t != LUA_TNIL)
	{
		lua_pushboolean(L, 0);
		return 1;
	}
	lua_pushboolean(L, host_story(reason) ? 1 : 0);
	return 1;
}

// A finite number within +-checkpoint::kMaxCoord from argument `idx`.
bool arg_number(lua_State* L, int idx, float& out)
{
	double v = 0.0;
	if (!lua_api::arg_number_in(L, idx, -double(checkpoint::kMaxCoord), double(checkpoint::kMaxCoord), v))
		return false;
	out = float(v);
	return true;
}

// coop.checkpoint.register_source(id, radius) -> bool: a mod object counts as a camp within `radius` metres (the
// host, or the game before it hosts; false on a client, for no such object or a radius outside (0, 500]).
int l_register_source(lua_State* L)
{
	u16 id = 0;
	float radius = 0.0f;
	const bool ok = lua_api::arg_object_id(L, 1, id) && arg_number(L, 2, radius) && sources::host_register_source(id, radius);
	lua_pushboolean(L, ok ? 1 : 0);
	return 1;
}

// coop.checkpoint.unregister_source(id) -> bool: no longer a camp (false on a client or if it was none).
int l_unregister_source(lua_State* L)
{
	u16 id = 0;
	lua_pushboolean(L, lua_api::arg_object_id(L, 1, id) && sources::host_unregister_source(id) ? 1 : 0);
	return 1;
}

// The kind of a claim from argument `idx`: nothing or nil is a tent, else "tent" | "sleepbag" | "pack".
bool arg_claim_what(lua_State* L, int idx, checkpoint::ClaimWhat& out)
{
	const int t = lua_type(L, idx);
	if (t == LUA_TNONE || t == LUA_TNIL)
	{
		out = checkpoint::ClaimWhat::Tent;
		return true;
	}
	if (t != LUA_TSTRING)
		return false;
	const char* s = lua_tostring(L, idx);
	for (const checkpoint::ClaimWhat w :
	    {checkpoint::ClaimWhat::Tent, checkpoint::ClaimWhat::Sleepbag, checkpoint::ClaimWhat::Pack})
		if (s && !xr_strcmp(s, checkpoint::claim_what_name(w)))
		{
			out = w;
			return true;
		}
	return false;
}

// coop.checkpoint.claim_tent(x, y, z [, what]) -> bool: "I put up a tent (a sleeping bag) here" or "pack mine up at
// this spot" (what "tent" (default) | "sleepbag" | "pack"). Host: checked at once; client: sent to the host (at
// most one per 2 s). The answer is his Tent checkpoint (coop_checkpoint_changed), the item back for a pack-up, or
// coop_checkpoint_refused(reason, what); false outside a session, for a position or kind that is not one, or a claim
// sent too soon.
int l_claim_tent(lua_State* L)
{
	float p[3] = {0, 0, 0};
	checkpoint::ClaimWhat what = checkpoint::ClaimWhat::Tent;
	bool ok = arg_number(L, 1, p[0]) && arg_number(L, 2, p[1]) && arg_number(L, 3, p[2]) && arg_claim_what(L, 4, what);
	if (ok)
	{
		Fvector pos;
		pos.set(p[0], p[1], p[2]);
		ok = sources::claim_tent(what, pos);
	}
	lua_pushboolean(L, ok ? 1 : 0);
	return 1;
}

// coop.checkpoint.tent_owner(id) -> character id | nil: host, whose tent or sleeping bag `id` is (the host's Lua adds
// a client's tent to item_tent's list); nil on a client, for nobody's or an absent owner's.
int l_tent_owner(lua_State* L)
{
	u16 id = 0;
	const u16 owner = lua_api::arg_object_id(L, 1, id) ? sources::host_tent_owner(id) : u16(0xffff);
	if (owner == 0xffff)
		lua_pushnil(L);
	else
		lua_pushnumber(L, lua_Number(owner));
	return 1;
}
} // namespace

// Fills coop.checkpoint (made here if no one made it yet).
void register_lua(lua_State* L)
{
	static const luaL_Reg fns[] = {
		{"get", &l_get},
		{"story", &l_story},
		{"register_source", &l_register_source}, // a mod's API: mod_system_coop_checkpoints.ltx
		{"unregister_source", &l_unregister_source},
		{"claim_tent", &l_claim_tent},
		{"tent_owner", &l_tent_owner},
		{nullptr, nullptr},
	};
	lua_api::fill(L, "checkpoint", fns);
}
} // namespace checkpoints
} // namespace coop
