// Anomaly Coop - requests: the ALife calls of a client's Lua (alife():create / create_ammo / clone_weapon / register /
// release) on the request paths of the call gate (coop_callgate.h), and the calls the gate refuses there.
#include "stdafx.h"
#include "pch_script.h"
#include "coop_requests_impl.h"
#include "coop_manager.h"
#include "xrCoop/coop_mode.h"
#include "coop_levels.h"
#include "coop_join.h"
#include "coop_callgate.h"

#include <algorithm>

#include "xrServer_Objects_ALife.h"
#include "xrServer_Objects_ALife_Items.h"

namespace coop
{
namespace requests
{
namespace
{
// The temporary objects of an old world were made by its Lua VM, which the new world closed: freeing
// one now runs a luabind destructor in a closed lua_State. They are forgotten (a small leak), not freed.
void temps_sync_world()
{
	const u32 world = join::world_generation();
	if (world == g_temps_world)
		return;
	g_temps_world = world;
	g_lc.temps_left += g_lua_temps.size();
	g_lua_temps.clear();
	g_lua_forwarded.clear();
	xrCriticalSectionGuard guard(g_out_lock);
	g_temp_leases.clear(); // keys are those objects; their ids went with the old pool
}

CSE_Abstract* lua_forward(CSE_Abstract* e, const char* name)
{
	if (client_forward_spawn(e))
		++g_cl.lua_forwarded;
	else
	{
		// the call gate already refused a parent that is not ours; this is the backstop, counted with it
		string256 why;
		xr_sprintf(why, "[%s] is a world spawn, the host's job", e->s_name.c_str());
		lua_refuse(name, why);
	}
	return nullptr;
}
} // namespace

xr_vector<CSE_Abstract*> g_lua_temps; // alife():create(..., false) results waiting for register()
xr_set<CSE_Abstract*> g_lua_forwarded;

// The world (join::world_generation) the temporary objects of g_lua_temps were made in
u32 g_temps_world = 0;

bool lua_client()
{
	const Phase p = mgr().phase();
	return !mode().decides_world() && (p == Phase::LoadingWorld || p == Phase::Live);
}

bool lua_mirror() { return lua_client() && is_mirror(); }

CSE_Abstract* lua_create(const char* section, const Fvector& pos, u32 level_vertex, u16 game_vertex, u16 parent,
    bool register_now, int ammo)
{
	temps_sync_world();
	// A script_zone without a parent (a map mod's zone under a pin): ours alone, made here
	if (register_now && local_zone(section) && zones::wants(section, parent))
		if (CSE_Abstract* zone = local_zone_create(section, pos, level_vertex, game_vertex))
			return zone;
	CSE_Abstract* e = make_temp(section, pos, level_vertex, game_vertex, parent);
	if (!e)
	{
		::Msg("! [coop] alife():create: unknown section [%s]", section ? section : "");
		return nullptr;
	}
	if (ammo >= 0)
		if (CSE_ALifeItemAmmo* box = smart_cast<CSE_ALifeItemAmmo*>(e))
			box->a_elapsed = u16(_min(ammo, int(box->m_boxSize)));
	// an item into our actor takes a leased id and is ours at once (nil only without one)
	const u16 id = take_lease(section, parent);
	if (!register_now)
	{
		g_lua_temps.push_back(e); // the script edits it, then calls alife():register(se)
		if (id != kNoId)
		{
			e->ID = id; // a script keys its data by it before the register (m_data, item parts)
			name_with_id(e); // se_obj:name() as in single player
			xrCriticalSectionGuard guard(g_out_lock);
			g_temp_leases[e] = LeasedTemp{id, kNoId};
			++g_lc.temps;
		}
		return e;
	}
	if (id != kNoId)
	{
		if (create_leased(e, id, kNoId))
			return e;
		return_unused_lease(id);
	}
	lua_forward(e, "alife:create");
	F_entity_Destroy(e);
	return nullptr;
}

CSE_Abstract* lua_clone(CSE_Abstract* source, const char* section, const Fvector& pos, u32 level_vertex,
    u16 game_vertex, u16 parent, bool register_now)
{
	temps_sync_world();
	CSE_ALifeItemWeaponMagazined* from = smart_cast<CSE_ALifeItemWeaponMagazined*>(source);
	if (!from)
		return nullptr; // as the engine: only magazined weapons are cloned
	// a weapon the host has not made yet, or one a clone of ours replaces already, is not
	// cloned (nil; coop_mods_guard tells the player before the mod's own creates)
	if (client_lease_waiting(from->ID))
	{
		++g_lc.clones_refused;
		return lua_refuse("alife:clone_weapon", "its weapon waits for the host (a create, clone or removal in flight)");
	}
	// a variant of the same weapon only (the host refuses another: its source's rounds would become
	// another ammo); a plain create would leave the source beside the clone
	if (!section || !clone_variant(from->s_name.c_str(), section))
	{
		++g_lc.not_variant;
		return lua_refuse("alife:clone_weapon", "not a variant of its weapon (scope, magnifier, upgrade kit)");
	}
	const u16 id = callgate::is_own(from->ID) ? take_lease(section, parent) : kNoId;
	if (id == kNoId)
		return lua_refuse("alife:clone_weapon", "no leased id for a complete replacement");
	CSE_Abstract* e = make_temp(section, pos, level_vertex, game_vertex, parent);
	CSE_ALifeItemWeaponMagazined* clone = smart_cast<CSE_ALifeItemWeaponMagazined*>(e);
	if (!clone)
	{
		if (e)
			F_entity_Destroy(e);
		return_unused_lease(id);
		return nullptr;
	}
	clone->wpn_flags = from->wpn_flags;
	clone->m_addon_flags = from->m_addon_flags;
	clone->m_fCondition = from->m_fCondition;
	clone->ammo_type = from->ammo_type;
	clone->m_upgrades = from->m_upgrades;
	clone->set_ammo_elapsed(from->get_ammo_elapsed());
	e->ID = id;
	name_with_id(e);
	if (!prepare_local_clone(from, e))
	{
		e->ID = kNoId;
		F_entity_Destroy(e);
		return_unused_lease(id);
		return lua_refuse("alife:clone_weapon", "incomplete, oversized or incompatible replacement snapshot");
	}
	// a clone of our own weapon into our actor takes a leased id; the host releases the source with it
	if (!register_now)
	{
		g_lua_temps.push_back(e);
		if (id != kNoId)
		{
			e->ID = id; // _g.alife_clone_weapon keys m_data and the parts by it before the register
			name_with_id(e);
			xrCriticalSectionGuard guard(g_out_lock);
			g_temp_leases[e] = LeasedTemp{id, from->ID};
			++g_lc.temps;
		}
		return e;
	}
	if (id != kNoId)
	{
		if (create_leased(e, id, from->ID))
			return e;
		return_unused_lease(id);
	}
	after_clone_register(id, false);
	F_entity_Destroy(e);
	return nullptr;
}

CSE_Abstract* lua_register_spawn(CSE_Abstract* object)
{
	temps_sync_world();
	const auto temp = std::find(g_lua_temps.begin(), g_lua_temps.end(), object);
	if (temp == g_lua_temps.end())
		return lua_refuse("alife:register", "an object the host did not create");
	if (g_lua_forwarded.count(object))
		return lua_refuse("alife:register", "a second register of one object"); // would be a duplicate
	// a temporary object with a leased id is ours at once: register answers it, as in single player
	LeasedTemp leased;
	if (take_temp_lease(object, leased))
	{
		if (create_leased(object, leased.id, leased.source))
		{
			g_lua_temps.erase(temp); // our ALife's entity now, not a temporary object to free at the session's end
			return object;
		}
		object->ID = kNoId;
		after_clone_register(leased.id, false);
		return_unused_lease(leased.id);
		if (leased.source != kNoId)
		{
			// its source's release may have gone with it already (stock _g.alife_clone_weapon releases
			// before the register): a clone by the request path now would leave both
			g_lua_forwarded.insert(object);
			return lua_refuse("alife:register", "a clone whose leased id could not be used here");
		}
	}
	g_lua_forwarded.insert(object);
	return lua_forward(object, "alife:register");
}

bool lua_release(CSE_Abstract* object)
{
	if (!object)
		return true;
	temps_sync_world();
	if (local_zone_release(object)) // a personal map zone of ours
		return true;
	if (std::find(g_lua_temps.begin(), g_lua_temps.end(), object) != g_lua_temps.end())
	{
		// never created: nothing to release; its leased id serves the next create
		LeasedTemp leased;
		if (take_temp_lease(object, leased))
		{
			after_clone_register(leased.id, false);
			object->ID = kNoId;
			xrCriticalSectionGuard guard(g_out_lock);
			g_pool.put_back(leased.id);
		}
		return true;
	}
	const u16 id = object->ID;
	// the weapon a clone of ours replaces: the host releases it with the clone (and keeps it if it refuses the
	// clone), so this release is no request of its own (_g.alife_clone_weapon releases it before or after the register)
	if (clone_source(id))
	{
		++g_lc.released_with_clone;
		return true;
	}
	// the call gate decides; our own items (not our actor) have the client_destroy path. It never runs the stock
	// release while a host command is applied (the client's ALife copy is the host's).
	callgate::Args args;
	args.runs_while_applying = false;
	switch (callgate::client_gate_own("alife:release", calls::own_release(id == kActor, callgate::is_own(id)), args))
	{
	case calls::Gate::Run: return false; // an ltx override runs it here
	case calls::Gate::Refuse: ++g_cl.lua_refused; return true; // the gate logged it
	case calls::Gate::Request: break;
	}
	client_destroy(id, false); // ours: the host releases it and tells everybody
	return true;
}

CSE_Abstract* lua_refuse(const char* name, const char* why)
{
	++g_cl.lua_refused;
	callgate::note_refused(name, why); // counted and logged with the gate's refusals
	return nullptr;
}

int lua_lease_waiting(lua_State* L)
{
	bool waiting = false;
	if (lua_isnumber(L, 1) && lease_world_ready())
	{
		const lua_Number id = lua_tonumber(L, 1);
		if (id >= 0 && id < lua_Number(kNoId))
			waiting = client_lease_waiting(u16(id));
	}
	lua_pushboolean(L, waiting ? 1 : 0);
	return 1;
}

int lua_lease_ids(lua_State* L)
{
	size_t n = 0;
	if (lease_world_ready())
	{
		xrCriticalSectionGuard guard(g_out_lock);
		lease_sync_world_locked();
		n = g_pool.size();
	}
	lua_pushnumber(L, lua_Number(n));
	return 1;
}

bool held_by_a_client(const char* name, u16 id)
{
	if (!levels::online_at_server(id))
		return false;
	++g_cl.lua_refused;
	if (g_log.allow())
		::Msg("[coop] levels: %s on object %u refused: a client computes it (it comes home when he leaves it)", name, id);
	return true;
}

bool lua_client_refuse(const char* name, u16 target, bool runs_while_applying)
{
	if (held_by_a_client(name, target))
		return true;
	if (!lua_mirror())
		return false;
	// the call gate decides. These calls have no request path, so a client refuses them unless an ltx override runs
	// them here (or a host command is being applied and the caller allows it); the gate counts and logs. The target
	// must exist in this world, a missing id would crash the ALife call.
	callgate::Args args;
	args.id = target;
	args.runs_while_applying = runs_while_applying;
	if (callgate::client_gate_id(name, target, args) == calls::Gate::Run)
		return false;
	++g_cl.lua_refused;
	return true;
}
} // namespace requests
} // namespace coop
