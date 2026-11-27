// Anomaly Coop: the world server's half of the members of a squad a map server writes (declared in xrServer_Objects_ALife_Monsters.h).
// A squad that came as a packet gets its members bound to their objects (coop_bind_members), and a map server's journal entry Joined or
// Left is applied as register_member and unregister_member would, without switching anything online in this world (coop_join_member,
// coop_leave_member). None of them raises a Joined / Left of its own (squad::MemberQuiet). Called from coop_levels_alife.cpp.
#include "stdafx.h"
#include "../coop_squad_members.h"
#include "ai_space.h"
#include "game_graph.h"
#include "alife_simulator.h"
#include "alife_object_registry.h"
#include "alife_graph_registry.h"
#include "alife_schedule_registry.h"
#include "xrServer_Objects_ALife_Monsters.h"

// A squad a map server made came here as a packet: its members are ids (STATE_Read). Only a load binds them, and a squad that
// comes home in the running world has none: the first update walked a null. What register_member does, without its word on where a member
// stands: it may be an object of its own by now. `forget`: an id with no object goes, else it waits for its packet. True: all are bound.
bool CSE_ALifeOnlineOfflineGroup::coop_bind_members(bool forget)
{
	const coop::squad::MemberQuiet quiet; // it binds what its world said, it changes no membership of its own
	xr_vector<ALife::_OBJECT_ID> gone;
	bool placed = false;
	CALifeGraphRegistry& graph = alife().graph();
	MEMBERS::iterator I = m_members.begin();
	MEMBERS::iterator E = m_members.end();
	for (; I != E; ++I)
	{
		if ((*I).second)
			continue;
		CSE_ALifeMonsterAbstract* monster = smart_cast<CSE_ALifeMonsterAbstract*>(ai().alife().objects().object((*I).first, true));
		if (!monster)
		{
			gone.push_back((*I).first);
			continue;
		}
		if (monster->m_group_id != 0xffff && monster->m_group_id != ID)
		{
			// A map server made this squad and moved the member in from another in one frame, so this Spawn comes before the old
			// squad's Left. The member leaves its old squad's list first, as coop_join_member does: never
			// listed in two squads, also when that Left is refused (the Left then finds no member:
			// nothing to change).
			CSE_ALifeOnlineOfflineGroup* old =
			    smart_cast<CSE_ALifeOnlineOfflineGroup*>(ai().alife().objects().object(monster->m_group_id, true));
			if (old)
				old->coop_leave_member(monster->ID);
			else
				monster->m_group_id = 0xffff;
		}
		monster->m_bOnline = m_bOnline;
		alife().scheduled().remove(monster, true);
		const GameGraph::_GRAPH_ID vertex = monster->m_tGraphID;
		if (ai().game_graph().valid_vertex_id(vertex))
		{
			const auto& here = graph.objects()[vertex].objects().objects();
			if (here.find(monster->ID) != here.end())
				graph.remove(monster, vertex, false);
		}
		graph.level().remove(monster, true);
		monster->m_group_id = ID;
		(*I).second = monster;
		if (!placed && !ai().game_graph().valid_vertex_id(m_tGraphID) && ai().game_graph().valid_vertex_id(vertex))
		{
			// as the first member of register_member: a squad with no place stands where it does
			placed = true;
			o_Position = monster->o_Position;
			m_tNodeID = monster->m_tNodeID;
			m_tGraphID = vertex;
			m_fGoingSpeed = monster->m_fGoingSpeed;
			m_fCurrentLevelGoingSpeed = monster->m_fCurrentLevelGoingSpeed;
		}
	}
	if (forget)
		for (const ALife::_OBJECT_ID id : gone)
			unregister_member(id);
	if (!m_members.empty() && ai().game_graph().valid_vertex_id(m_tGraphID))
		m_flags.set(flUsedAI_Locations, TRUE);
	if (placed)
		graph.add(this, m_tGraphID, false); // it stood in no graph point with no place
	return forget || gone.empty();
}

// A member joined this squad on the map server that writes it. A creature of another squad
// leaves that one first, as the stock re-registration does; then it is bound as coop_bind_members binds a member, its m_bOnline that
// of the squad: a squad held by a map server is online there, never here (register_member would switch it online in this world).
bool CSE_ALifeOnlineOfflineGroup::coop_join_member(ALife::_OBJECT_ID member_id)
{
	const coop::squad::MemberQuiet quiet; // the world applies a journal entry: it raises no Joined / Left of its own
	const MEMBERS::iterator known = m_members.find(member_id);
	if (known != m_members.end())
		return (*known).second != nullptr; // a repeat
	CSE_ALifeMonsterAbstract* monster = smart_cast<CSE_ALifeMonsterAbstract*>(ai().alife().objects().object(member_id, true));
	if (!monster || !monster->g_Alive() || monster->ID_Parent != 0xffff || monster->ID == ID)
		return false;
	if (monster->m_group_id != 0xffff && monster->m_group_id != ID)
	{
		CSE_ALifeOnlineOfflineGroup* old = smart_cast<CSE_ALifeOnlineOfflineGroup*>(ai().alife().objects().object(monster->m_group_id, true));
		if (old)
			old->coop_leave_member(member_id);
		else
			monster->m_group_id = 0xffff;
	}
	m_members.insert(std::make_pair(member_id, static_cast<MEMBER*>(nullptr)));
	coop_bind_members(false);
	const MEMBERS::iterator bound = m_members.find(member_id);
	return bound != m_members.end() && (*bound).second != nullptr;
}

// A member left this squad on the map server that writes it, alive. It is a creature of its own at home in this
// world (unregister_member puts it in the graph and the scheduler), not online anywhere here; a map server that keeps it near a
// player asks the world for it as for any creature. False: it was no member.
bool CSE_ALifeOnlineOfflineGroup::coop_leave_member(ALife::_OBJECT_ID member_id)
{
	const coop::squad::MemberQuiet quiet; // the world applies a journal entry: it raises no Left of its own
	const MEMBERS::iterator I = m_members.find(member_id);
	if (I == m_members.end())
		return false;
	if (MEMBER* m = (*I).second)
	{
		m->m_bOnline = false;
		if (!ai().game_graph().valid_vertex_id(m->m_tGraphID) && ai().game_graph().valid_vertex_id(m_tGraphID))
			m->m_tGraphID = m_tGraphID; // the graph registry takes it at a vertex: its squad's
		if (!ai().game_graph().valid_vertex_id(m->m_tGraphID))
		{
			// no vertex to put it at (a squad that came as a packet and never stood anywhere): it leaves the squad and nothing else
			m->m_group_id = 0xffff;
			m_members.erase(I);
			if (m_members.empty())
				m_flags.set(flUsedAI_Locations, FALSE);
			return true;
		}
	}
	unregister_member(member_id);
	return true;
}
