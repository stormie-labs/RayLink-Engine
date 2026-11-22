// Anomaly Coop - R32 W1: who writes an object, as this process sees it (coop_writer.h). Core.
#include "stdafx.h"
#include "pch_script.h"
#include "coop_writer.h"
#include "coop_manager.h"
#include "coop_levels.h"
#include "coop_world.h"
#include "xrCoop/coop_mode.h"

#include "ai_space.h"
#include "alife_simulator.h"
#include "alife_object_registry.h"
#include "xrServer_Objects_ALife_Monsters.h"

namespace coop
{
namespace writing
{
namespace
{
const CSE_ALifeDynamicObject* object_of(u16 id)
{
	if (id == writer::kNoId || !ai().get_alife())
		return nullptr;
	return ai().alife().objects().object(id, true);
}

u16 parent_of(u16 id)
{
	const CSE_ALifeDynamicObject* e = object_of(id);
	return e && e->ID_Parent != 0xffff ? e->ID_Parent : writer::kNoId;
}

// §8a A5: the squad of a creature is its m_group_id (levels unit_of resolves only squads that came over the wire).
u16 squad_of(u16 id)
{
	const CSE_ALifeMonsterAbstract* m = smart_cast<const CSE_ALifeMonsterAbstract*>(object_of(id));
	return m && m->m_group_id != 0xffff ? m->m_group_id : writer::kNoId;
}
} // namespace

writer::RootFacts root_facts_of(u16 id)
{
	writer::RootFacts f{writer::RootKind::None, id, writer::kNoLevel};
	const CSE_ALifeDynamicObject* e = object_of(id);
	if (!e)
		return f;
	f.root = writer::root_of(id, parent_of, squad_of);
	const CSE_ALifeDynamicObject* r = object_of(f.root);
	if (!r)
		return f;
	f.level = levels::level_of_object(f.root);
	if (f.root == writer::kGhostId || world::is_player_character(r))
		f.kind = writer::RootKind::Character;
	else if (smart_cast<const CSE_ALifeOnlineOfflineGroup*>(r))
		f.kind = writer::RootKind::Unit;
	else if (const CSE_ALifeCreatureAbstract* c = smart_cast<const CSE_ALifeCreatureAbstract*>(r); c && c->g_Alive())
		f.kind = writer::RootKind::Unit; // a living creature in no squad
	else if (f.level != writer::kNoLevel)
		f.kind = writer::RootKind::Resident;
	return f;
}

bool writes(u16 id)
{
	if (!mgr().active())
		return true; // single player: the stock game writes everything
	if (is_mirror())
		return false; // a player's machine: its objects are copies of its host's (Mode::writer_of: Other)
	return mode().writer_of(root_facts_of(id)).is == writer::WriterIs::This;
}

int lua_writes(lua_State* L)
{
	bool yes = false; // not an object id: nothing to write
	if (lua_isnumber(L, 1))
	{
		const lua_Number v = lua_tonumber(L, 1);
		if (v >= 0 && v < lua_Number(writer::kNoId) && v == lua_Number(u16(v)))
			yes = writes(u16(v));
	}
	lua_pushboolean(L, yes ? 1 : 0);
	return 1;
}
} // namespace writing
} // namespace coop
