// Anomaly Coop - the initial state of an item rides its create (coop_initial_state.h).
#include "stdafx.h"
#include "pch_script.h"
#include "coop_initial_state.h"
#include "coop_items.h"
#include "coop_lua.h" // lua_api::fill
#include "coop_writer.h" // writing::writes (this process writes the item)
#include "coop_entities.h" // entities::host_own_spawn_unsent (this process's own create, its spawn not sent yet)

#include "ai_space.h"
#include "Level.h"
#include "inventory_item.h"
#include "eatable_item.h"
#include "WeaponAmmo.h"
#include "alife_simulator.h"
#include "alife_object_registry.h"
#include "xrServer_Objects_ALife_Items.h"
#include "../xrServerEntities/xrServer_Objects.h"

namespace coop
{
namespace items
{
namespace
{
CInventoryItem* object_of(u16 id)
{
	if (!g_pGameLevel)
		return nullptr;
	CObject* o = Level().Objects.net_Find(id);
	if (!o || o->getDestroy())
		return nullptr;
	return smart_cast<CInventoryItem*>(o);
}

// The state of `id` as this process has it.
initial::State state_of(const CSE_Abstract* e)
{
	initial::State s;
	const CSE_ALifeInventoryItem* ii = smart_cast<const CSE_ALifeInventoryItem*>(e);
	if (!ii)
		return s;
	CInventoryItem* item = object_of(e->ID);
	s.has_cond = true;
	s.cond = initial::quant(item ? item->GetCondition() : ii->m_fCondition);
	if (const CSE_ALifeItemAmmo* a = smart_cast<const CSE_ALifeItemAmmo*>(e))
	{
		s.has_ammo = true;
		const CWeaponAmmo* box = item ? smart_cast<const CWeaponAmmo*>(item) : nullptr;
		s.ammo = box ? box->m_boxCurr : a->a_elapsed;
	}
	if (CEatableItem* eat = item ? item->cast_eatable_item() : nullptr)
	{
		s.has_uses = true;
		s.uses = eat->GetRemainingUses();
	}
	else if (!item && is_eatable_section(e->s_name))
	{
		// Only an eatable has uses (WP6 fix round, finding 5): a held weapon's client_data has the same first bytes and its rounds
		// where an eatable has its uses byte. Uses staged for the next object (a scripted section) are newer than its client_data.
		const std::vector<u8> cd(e->client_data.begin(), e->client_data.end());
		s.has_uses = staged_uses(e->ID, s.uses) || initial::read_uses(cd, s.uses);
	}
	s.versions = item_versions(e->ID);
	s.versions_mask = owner::versions_mask(s.versions);
	return s;
}

// Into an entity with no game object here (a create that is not online yet, an offline item).
bool apply_to_entity(CSE_Abstract* e, const initial::State& s, const char*& why)
{
	CSE_ALifeInventoryItem* ii = smart_cast<CSE_ALifeInventoryItem*>(e);
	if (!ii)
	{
		why = "not an inventory item";
		return false;
	}
	if (s.has_ammo)
	{
		CSE_ALifeItemAmmo* a = smart_cast<CSE_ALifeItemAmmo*>(e);
		if (!a)
		{
			why = "ammo count for a non-ammo item";
			return false;
		}
		if (s.ammo > a->m_boxSize)
		{
			why = "more rounds than the box holds";
			return false;
		}
	}
	if (s.has_uses && !is_eatable_section(e->s_name))
	{
		why = "uses for a non-eatable"; // as the object path says (set_initial), whatever the client_data looks like
		return false;
	}
	if (s.has_uses && !initial::uses_ok(s.uses, section_max_uses(e->s_name)))
	{
		why = "more uses than the section has";
		return false;
	}
	if (s.has_cond)
		ii->m_fCondition = initial::dequant(s.cond);
	if (s.has_ammo)
		smart_cast<CSE_ALifeItemAmmo*>(e)->a_elapsed = s.ammo;
	// The uses into the entity's client_data; a scripted section with none yet (its binder's bytes cannot be made here) keeps them
	// staged for its next game object here, which sets them after its net_Spawn read the stock bytes (WP6 fix round, finding 1).
	if (s.has_uses && !entity_write_uses(e, s.uses))
		stage_uses(e->ID, s.uses);
	if (s.versions_mask)
		item_versions_arrive(e->ID, s.versions, s.versions_mask);
	return true;
}

// coop.items.set_initial(id, t)
int l_set_initial(lua_State* L)
{
	const char* why = nullptr;
	bool ok = false;
	if (lua_type(L, 1) == LUA_TNUMBER && lua_istable(L, 2))
	{
		const lua_Number idn = lua_tonumber(L, 1);
		initial::State s;
		bool args = idn >= 0 && idn < lua_Number(0xffff);
		lua_getfield(L, 2, "uses");
		if (lua_type(L, -1) == LUA_TNUMBER)
		{
			const lua_Number v = lua_tonumber(L, -1);
			args = args && v >= 0 && v <= 255;
			s.has_uses = true;
			s.uses = u8(args ? v : 0);
		}
		lua_pop(L, 1);
		lua_getfield(L, 2, "condition");
		if (lua_type(L, -1) == LUA_TNUMBER)
		{
			const lua_Number v = lua_tonumber(L, -1);
			args = args && v >= 0 && v <= 1;
			s.has_cond = true;
			s.cond = initial::quant(float(args ? v : 0));
		}
		lua_pop(L, 1);
		lua_getfield(L, 2, "ammo");
		if (lua_type(L, -1) == LUA_TNUMBER)
		{
			const lua_Number v = lua_tonumber(L, -1);
			args = args && v >= 0 && v <= 0xffff;
			s.has_ammo = true;
			s.ammo = u16(args ? v : 0);
		}
		lua_pop(L, 1);
		if (!args)
			why = "bad arguments";
		else
			ok = set_initial(u16(idn), s, why);
	}
	else
		why = "bad arguments";
	lua_pushboolean(L, ok ? 1 : 0);
	if (ok)
		return 1;
	lua_pushstring(L, why ? why : "refused");
	return 2;
}
} // namespace

bool write_initial(Writer& w, const CSE_Abstract* e)
{
	initial::write(w, state_of(e));
	return true;
}

bool read_initial(Reader& r, CSE_Abstract* e)
{
	initial::State s;
	if (!initial::read(r, s))
		return false;
	if (s.empty() || !e)
		return true;
	const char* why = nullptr;
	if (object_of(e->ID))
	{
		// An object exists already (a repeated spawn): its own state stands, only the versions arrive with it.
		if (s.versions_mask)
			item_versions_arrive(e->ID, s.versions, s.versions_mask);
		return true;
	}
	if (!apply_to_entity(e, s, why))
		::Msg("! [coop] initial state of %u [%s] not applied: %s", e->ID, e->s_name.c_str(), why ? why : "?");
	return true;
}

bool set_initial(u16 id, const initial::State& s, const char*& why)
{
	why = nullptr;
	CSE_Abstract* e = ai().get_alife() ? ai().alife().objects().object(id, true) : nullptr;
	if (!e || !smart_cast<CSE_ALifeInventoryItem*>(e))
	{
		why = "no such item";
		return false;
	}
	CInventoryItem* const item = object_of(id);
	// The writer sets it; so does the creator of this frame's create in a process that does not write the item, while its spawn
	// has not left (§8a A21 E-2: the world server's reward copy for a player on a map a map server leads): into the entity, and the
	// EntitySpawn tail (write_initial) carries it to the writer, whose read_initial applies it (coop_initial_rules.h set_path).
	const initial::SetPath path = initial::set_path(writing::writes(id), item != nullptr, entities::host_own_spawn_unsent(id));
	if (path == initial::SetPath::NotWrittenHere)
	{
		why = "not written here"; // a mirror asks
		return false;
	}
	if (path == initial::SetPath::Object)
	{
		// The game object is the item: the setter's path, a deliberate write of the decider.
		if (s.has_uses)
		{
			CEatableItem* eat = item->cast_eatable_item();
			if (!eat)
			{
				why = "uses for a non-eatable";
				return false;
			}
			if (!initial::uses_ok(s.uses, section_max_uses(e->s_name)))
			{
				why = "more uses than the section has";
				return false;
			}
		}
		CWeaponAmmo* box = s.has_ammo ? smart_cast<CWeaponAmmo*>(item) : nullptr;
		if (s.has_ammo && (!box || s.ammo > box->m_boxSize))
		{
			why = box ? "more rounds than the box holds" : "ammo count for a non-ammo item";
			return false;
		}
		if (s.has_cond)
		{
			item->SetCondition(initial::dequant(s.cond));
			decider_wrote(item, owner::Setter::Condition);
		}
		if (s.has_uses)
		{
			discard_staged_uses(id); // the object has the newest value: an older staged one must not follow it
			item->cast_eatable_item()->SetRemainingUses(s.uses);
			decider_wrote(item, owner::Setter::Uses);
		}
		if (box)
		{
			box->m_boxCurr = s.ammo;
			if (box->m_pInventory)
				box->m_pInventory->InvalidateState();
		}
		return true;
	}
	initial::State entity_state = s;
	entity_state.versions_mask = 0; // a script sets values; the versions are the item engine's
	return apply_to_entity(e, entity_state, why);
}

void register_lua(lua_State* L)
{
	static const luaL_Reg fns[] = {
		{"set_initial", &l_set_initial},
		{nullptr, nullptr},
	};
	lua_api::fill(L, "items", fns); // coop_script logs a missing coop
}
} // namespace items
} // namespace coop
