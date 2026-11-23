////////////////////////////////////////////////////////////////////////////
//	Module 		: alife_online_offline_group.cpp
//	Created 	: 25.10.2005
//  Modified 	: 25.10.2005
//	Author		: Dmitriy Iassenev
//	Description : ALife Online Offline Group class
////////////////////////////////////////////////////////////////////////////

#include "stdafx.h"
#include "coop/coop_alife.h"
#include "coop/coop_entities.h" // Anomaly Coop (R32 W3): client_applying, for the member hook
#include "xrCoop/coop_mode.h" // Anomaly Coop (R32 W3): the level side the member hook tells (docs/MODES.md section 4)
#include "xrServer_Objects_ALife_Monsters.h"
#include "ai_space.h"
#include "alife_simulator.h"
#include "alife_object_registry.h"
#include "alife_graph_registry.h"
#include "alife_schedule_registry.h"
#include "game_level_cross_table.h"
#include "alife_online_offline_group_brain.h"
#include "level_graph.h"
#include "alife_monster_movement_manager.h"
#include "alife_monster_detail_path_manager.h"

#pragma warning(push)
#pragma warning(disable:4995)
#include <malloc.h>
#pragma warning(pop)

extern void setup_location_types_line(GameGraph::TERRAIN_VECTOR& m_vertex_types, LPCSTR string);

// Anomaly Coop (R32 W3, world-view D3; temp/r30/design/R32-final.md §1 W3, §8): a member joined or left a squad. A process that leads
// the squad's level apart from its world has a level side (mode().level(): a map server, whose journal carries Joined / Left up); the
// others have none (the world server, the co-op host and client, a player of a server world, single player) and say nothing. Not a
// change this process makes, so quiet: a copy's apply of its world's word (entities::client_applying), the world's own apply of a
// journal entry (coop_join_member, coop_leave_member, coop_bind_members) and a load that binds the members of a save again
// (on_after_game_load), and a squad that is no object of this world any more (its release is under way: its Destroy says it).
// Per thread: the ALife's thread and the main thread do not quiet each other.
namespace
{
thread_local int g_coop_member_quiet = 0;

struct CoopMemberQuiet
{
	CoopMemberQuiet() { ++g_coop_member_quiet; }
	~CoopMemberQuiet() { --g_coop_member_quiet; }
	CoopMemberQuiet(const CoopMemberQuiet&) = delete;
	CoopMemberQuiet& operator=(const CoopMemberQuiet&) = delete;
};

void coop_member_up(ALife::_OBJECT_ID squad, ALife::_OBJECT_ID member, bool joined)
{
	if (g_coop_member_quiet)
		return;
	if (coop::entities::client_applying())
		return;
	coop::LevelSide* side = coop::mode().level();
	if (!side || !ai().get_alife() || !ai().alife().objects().object(squad, true))
		return;
	side->level_entity_up(joined ? coop::EntityUp::Joined : coop::EntityUp::Left, squad, member);
}
} // namespace

CSE_ALifeItemWeapon* CSE_ALifeOnlineOfflineGroup::tpfGetBestWeapon(ALife::EHitType& tHitType, float& fHitPower)
{
	return (0);
}

ALife::EMeetActionType CSE_ALifeOnlineOfflineGroup::tfGetActionType(CSE_ALifeSchedulable* tpALifeSchedulable,
                                                                    int iGroupIndex, bool bMutualDetection)
{
	return (ALife::eMeetActionTypeIgnore);
}

bool CSE_ALifeOnlineOfflineGroup::bfActive()
{
	return (!m_bOnline && !m_members.empty());
}

CSE_ALifeDynamicObject* CSE_ALifeOnlineOfflineGroup::tpfGetBestDetector()
{
	return (0);
}

bool CSE_ALifeOnlineOfflineGroup::need_update(CSE_ALifeDynamicObject* object)
{
	return true;
}

void CSE_ALifeOnlineOfflineGroup::update()
{
    try
    {
        m_members.begin(); // force actualize
        if (m_bOnline && !m_members.empty())
        {
            MEMBER* commander = (*m_members.begin()).second;
            if (commander)
            {
                o_Position = commander->o_Position;
                m_tNodeID = commander->m_tNodeID;
                m_tGraphID = commander->m_tGraphID;
            }
        }
        if (!bfActive())
            return;

        brain().update();

        MEMBERS::iterator I = m_members.begin();
        MEMBERS::iterator E = m_members.end();
        for (; I != E; ++I)
        {
            MEMBER* ptr = ((*I).second);
            if (ptr)
            {
                ptr->o_Position = o_Position;
                ptr->m_tNodeID = m_tNodeID;
                ptr->m_tGraphID = m_tGraphID;
                ptr->m_fDistance = m_fDistance;
            }

        }
    }
    catch (...)
    {

    }
}

void CSE_ALifeOnlineOfflineGroup::on_location_change() const
{
	brain().on_location_change();
}


void CSE_ALifeOnlineOfflineGroup::register_member(ALife::_OBJECT_ID member_id)
{
	VERIFY(m_members.find(member_id) == m_members.end());
	CSE_ALifeDynamicObject* object = ai().alife().objects().object(member_id);
	CSE_ALifeMonsterAbstract* monster = smart_cast<CSE_ALifeMonsterAbstract*>(object);
	VERIFY(monster);
	VERIFY(monster->g_Alive());

	bool empty = m_members.empty();
	if (!object->m_bOnline)
	{
		if (m_bOnline)
		{
			object->switch_online();
			VERIFY(object->ID_Parent == 0xffff);
			alife().graph().level().remove(object);
		}
		else
		{
			alife().graph().remove(object, object->m_tGraphID);
			alife().scheduled().remove(object);
		}
	}
	else
	{
		if (!m_bOnline)
		{
			switch_online();
		}
		VERIFY(object->ID_Parent == 0xffff);
		alife().graph().level().remove(object);
	}
	VERIFY((monster->m_group_id == 0xffff) || (monster->m_group_id == ID));
	monster->m_group_id = ID;
	m_members.insert(std::make_pair(member_id, monster));
	coop_member_up(ID, member_id, true); // Anomaly Coop (R32 W3): Joined, to the world of a map server

	if (!empty)
		return;

	o_Position = monster->o_Position;
	m_tNodeID = monster->m_tNodeID;
	m_tGraphID = monster->m_tGraphID;
	m_fGoingSpeed = monster->m_fGoingSpeed;
	m_fCurrentLevelGoingSpeed = monster->m_fCurrentLevelGoingSpeed;
	m_flags.set(flUsedAI_Locations,TRUE);
	alife().graph().update(this);
}

void CSE_ALifeOnlineOfflineGroup::unregister_member(ALife::_OBJECT_ID member_id)
{
	// Anomaly Coop: an id read from a packet (STATE_Read) and never bound to its object (on_after_game_load did
	// not run: a squad mirrored on a coop client, a temporary entity) has no member state to restore, only the id
	// to forget. Without this the destructor writes through the null pointer (AV at unload / release).
	{
		MEMBERS::iterator U = m_members.find(member_id);
		if (U != m_members.end() && !(*U).second)
		{
			m_members.erase(U);
			if (m_members.empty())
				m_flags.set(flUsedAI_Locations,FALSE);
			coop_member_up(ID, member_id, false); // R32 W3: Left (a held squad's member whose packet had not come yet)
			return;
		}
	}

	CALifeGraphRegistry& graph = alife().graph();
	//	CALifeLevelRegistry			&level = graph.level();

	MEMBERS::iterator I = m_members.find(member_id);
	VERIFY(I != m_members.end());
	VERIFY((*I).second->m_group_id == ID);
	(*I).second->m_group_id = 0xffff;

	graph.update((*I).second);
	alife().scheduled().add((*I).second);
	m_members.erase(I);

	if (m_members.empty())
	{
		m_flags.set(flUsedAI_Locations,FALSE);
	}
	coop_member_up(ID, member_id, false); // Anomaly Coop (R32 W3): Left, to the world of a map server
}

CSE_ALifeOnlineOfflineGroup::MEMBER* CSE_ALifeOnlineOfflineGroup::member(ALife::_OBJECT_ID member_id, bool no_assert)
{
	MEMBERS::iterator I = m_members.find(member_id);
	if (I == m_members.end())
	{
		if (!no_assert)
			Msg("! There is no member with id %d in the OnlineOfflineGroup id %d", member_id, ID);
		VERIFY(no_assert);
		return (0);
	}
	return ((*I).second);
}

bool CSE_ALifeOnlineOfflineGroup::synchronize_location()
{
    m_members.begin(); // force actualize
	if (m_bOnline && !m_members.empty())
	{
		MEMBER* member = (*m_members.begin()).second;
        if (member)
        {
            o_Position = member->o_Position;
            m_tNodeID = member->m_tNodeID;
            m_tGraphID = member->m_tGraphID;
            m_fDistance = member->m_fDistance;
        }
	}

	return (true);
}

void CSE_ALifeOnlineOfflineGroup::try_switch_online()
{
	if (m_members.empty())
		return;

	// Anomaly Coop: on a client the host decides what is online
	const coop::SwitchVote vote = coop::client_switch_vote(ID);
	if (vote != coop::SwitchVote::Default)
	{
		if (vote == coop::SwitchVote::Online)
			alife().switch_online(this);
		else
			on_failed_switch_online();
		return;
	}

	if (!can_switch_online())
		return;

	if (!can_switch_offline())
	{
		inherited1::try_switch_online();
		return;
	}
	MEMBERS::iterator I = m_members.begin();
	MEMBERS::iterator E = m_members.end();
	for (; I != E; ++I)
	{
		if (!(*I).second)
			return; // Anomaly Coop: coop_bind_members, its packet is on the way
		VERIFY3((*I).second->g_Alive(), "Incorrect situation : some of the OnlineOffline group members is dead",
		        (*I).second->name_replace());
		VERIFY3((*I).second->can_switch_online(),
		        "Incorrect situation : some of the OnlineOffline group members cannot be switched online due to their personal properties",
		        (*I).second->name_replace());
		VERIFY3((*I).second->can_switch_offline(),
		        "Incorrect situation : some of the OnlineOffline group members cannot be switched online due to their personal properties",
		        (*I).second->name_replace());
		if (coop::observer_distance((*I).second->o_Position) > alife().online_distance()) // Anomaly Coop
		{
			continue;
		}
		inherited1::try_switch_online();
		return;
	}
	on_failed_switch_online();
}

void CSE_ALifeOnlineOfflineGroup::try_switch_offline()
{
	if (m_members.empty())
		return;

	// Anomaly Coop: on a client the host decides what is online
	const coop::SwitchVote vote = coop::client_switch_vote(ID);
	if (vote != coop::SwitchVote::Default)
	{
		if (vote == coop::SwitchVote::Offline)
			alife().switch_offline(this);
		return;
	}

	if (!can_switch_offline())
		return;

	if (!can_switch_online())
	{
		alife().switch_offline(this);
		return;
	}

	MEMBERS::iterator I = m_members.begin();
	MEMBERS::iterator E = m_members.end();
	for (; I != E; ++I)
	{
		if (!(*I).second)
			return; // Anomaly Coop: coop_bind_members, its packet is on the way
		VERIFY3((*I).second->g_Alive(), "Incorrect situation : some of the OnlineOffline group members is dead",
		        (*I).second->name_replace());
		VERIFY3((*I).second->can_switch_offline(),
		        "Incorrect situation : some of the OnlineOffline group members cannot be switched online due to their personal properties",
		        (*I).second->name_replace());
		VERIFY3((*I).second->can_switch_online(),
		        "Incorrect situation : some of the OnlineOffline group members cannot be switched online due to their personal properties",
		        (*I).second->name_replace());

		if (coop::observer_distance((*I).second->o_Position) <= alife().offline_distance()) // Anomaly Coop
			return;
	}

	alife().switch_offline(this);
}

void CSE_ALifeOnlineOfflineGroup::switch_online()
{
	R_ASSERT(!m_bOnline);
	m_bOnline = true;

	MEMBERS::iterator I = m_members.begin();
	MEMBERS::iterator E = m_members.end();
	for (; I != E; ++I)
	{
		if ((*I).second->m_bOnline == false)
			alife().add_online((*I).second, false);
	}

	alife().scheduled().remove(this);
	alife().graph().remove(this, m_tGraphID, false);
}

void CSE_ALifeOnlineOfflineGroup::switch_offline()
{
	R_ASSERT(m_bOnline);
	m_bOnline = false;

	if (!m_members.empty())
	{
		MEMBER* member = (*m_members.begin()).second;

		member->synchronize_location();

		o_Position = member->o_Position;
		m_tNodeID = member->m_tNodeID;
		m_tGraphID = member->m_tGraphID;
		m_fDistance = member->m_fDistance;
	}

	MEMBERS::iterator I = m_members.begin();
	MEMBERS::iterator E = m_members.end();
	for (; I != E; ++I)
	{
		if ((*I).second->m_bOnline == true)
		{
			(*I).second->clear_client_data();
			alife().remove_online((*I).second, false);
		}
	}

	alife().scheduled().add(this);
	alife().graph().add(this, m_tGraphID, false);
}

bool CSE_ALifeOnlineOfflineGroup::redundant() const
{
	return (m_members.empty());
}

void CSE_ALifeOnlineOfflineGroup::notify_on_member_death(MEMBER* member)
{
	unregister_member(member->ID);
}

void CSE_ALifeOnlineOfflineGroup::on_before_register()
{
	m_tGraphID = GameGraph::_GRAPH_ID(-1);
	m_flags.set(flUsedAI_Locations,FALSE);
}

void CSE_ALifeOnlineOfflineGroup::on_after_game_load()
{
	if (m_members.empty())
		return;

	ALife::_OBJECT_ID* temp = (ALife::_OBJECT_ID*)_alloca(m_members.size() * sizeof(ALife::_OBJECT_ID));
	ALife::_OBJECT_ID *i = temp, *e = temp + m_members.size();

	{
		MEMBERS::const_iterator I = m_members.begin();
		MEMBERS::const_iterator E = m_members.end();
		for (; I != E; ++I, ++i)
		{
			VERIFY(!(*I).second);
			*i = (*I).first;
		}
	}

	m_members.clear();

	const CoopMemberQuiet quiet; // Anomaly Coop (R32 W3): a load binds the members of the save again, nobody joined
	for (i = temp; i != e; ++i)
		register_member(*i);
}

// Anomaly Coop: a squad a map server made came here as a packet: its members are ids (STATE_Read). Only a load binds them, and a squad that
// comes home in the running world has none: the first update walked a null. What register_member does, without its word on where a member
// stands: it may be an object of its own by now. `forget`: an id with no object goes, else it waits for its packet. True: all are bound.
bool CSE_ALifeOnlineOfflineGroup::coop_bind_members(bool forget)
{
	const CoopMemberQuiet quiet; // R32 W3: it binds what its world said, it changes no membership of its own
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
			// R32 §8a A17 (package A, fix round 2, finding 2): a map server made this squad and moved the member in from another in one
			// frame, so this Spawn comes before the old squad's Left. The member leaves its old squad's list first, as coop_join_member
			// does: never listed in two squads, also when that Left is refused (the Left then finds no member: nothing to change).
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

// Anomaly Coop (R32 W3, world-view D3): a member joined this squad on the map server that writes it. A creature of another squad
// leaves that one first, as the stock re-registration does; then it is bound as coop_bind_members binds a member, its m_bOnline that
// of the squad: a squad held by a map server is online there, never here (register_member would switch it online in this world).
bool CSE_ALifeOnlineOfflineGroup::coop_join_member(ALife::_OBJECT_ID member_id)
{
	const CoopMemberQuiet quiet; // the world applies a journal entry: it raises no Joined / Left of its own
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

// Anomaly Coop (R32 W3): a member left this squad on the map server that writes it, alive. It is a creature of its own at home in this
// world (unregister_member puts it in the graph and the scheduler), not online anywhere here; a map server that keeps it near a
// player asks the world for it as for any creature. False: it was no member.
bool CSE_ALifeOnlineOfflineGroup::coop_leave_member(ALife::_OBJECT_ID member_id)
{
	const CoopMemberQuiet quiet; // the world applies a journal entry: it raises no Left of its own
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

ALife::_OBJECT_ID CSE_ALifeOnlineOfflineGroup::commander_id()
{
	if (!m_members.empty())
		return (*m_members.begin()).first;
	return 0xffff;
}

CSE_ALifeOnlineOfflineGroup::MEMBERS const& CSE_ALifeOnlineOfflineGroup::squad_members() const
{
	return m_members;
}

u32 CSE_ALifeOnlineOfflineGroup::npc_count() const
{
	return m_members.size();
}

void CSE_ALifeOnlineOfflineGroup::clear_location_types()
{
	m_tpaTerrain.clear();
	MEMBERS::iterator I = m_members.begin();
	MEMBERS::iterator E = m_members.end();
	for (; I != E; ++I)
	{
		if ((*I).second) // Anomaly Coop: coop_bind_members, its packet is on the way
			(*I).second->m_tpaTerrain.clear();
	}
}


void CSE_ALifeOnlineOfflineGroup::add_location_type(LPCSTR mask)
{
	setup_location_types_line(m_tpaTerrain, mask);
	MEMBERS::iterator I = m_members.begin();
	MEMBERS::iterator E = m_members.end();
	for (; I != E; ++I)
	{
		setup_location_types_line((*I).second->m_tpaTerrain, mask);
	}
}

void CSE_ALifeOnlineOfflineGroup::force_change_position(Fvector position)
{
	u32 new_level_vertex = ai().level_graph().vertex_id(position);
	GameGraph::_GRAPH_ID new_graph_vertex = ai().cross_table().vertex(new_level_vertex).game_vertex_id();
	o_Position = position;
	m_tNodeID = new_level_vertex;
	if (m_tGraphID != new_graph_vertex)
	{
		alife().graph().change(this, m_tGraphID, new_graph_vertex);
	}

	//m_tGraphID				= new_graph_vertex;
}

void CSE_ALifeOnlineOfflineGroup::on_failed_switch_online()
{
	MEMBERS::const_iterator I = m_members.begin();
	MEMBERS::const_iterator E = m_members.end();
	for (; I != E; ++I)
	{
		(*I).second->clear_client_data();
	}
}
