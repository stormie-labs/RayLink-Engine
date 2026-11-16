#include "stdafx.h"
#include "game_sv_single.h"
#include "alife_simulator.h"
#include "xrServer_Objects.h"
#include "xrServer.h"
#include "xrmessages.h"
#include "ai_space.h"
#include "coop/coop_entities.h" // Anomaly Coop
#include "coop/coop_levels.h" // Anomaly Coop: the split levels' word on a lost child (Perform_destroy)
#include "alife_object_registry.h" // Anomaly Coop: what ALife knows of a lost child

void xrServer::Perform_destroy(CSE_Abstract* object, u32 mode)
{
	R_ASSERT(object);
	if (object->ID_Parent != 0xffff)
	{
		// LPCSTR name = object->name();
		// if (name)
		// 	Msg("Attempt to destroy object %s with parent %d", name, object->ID_Parent);
		CSE_Abstract* parent = game->get_entity_from_eid(object->ID_Parent);
		if (parent)
			Perform_reject(object, parent, 2 * NET_Latency);
	}
	R_ASSERT(object->ID_Parent == 0xffff);

#ifdef DEBUG
#	ifdef SLOW_VERIFY_ENTITIES
		verify_entities			();
#	endif
#endif

	while (!object->children.empty())
	{
		CSE_Abstract* child = game->get_entity_from_eid(object->children.back());
		if (!child && coop::entities::mirror_lost_child(object, object->children.back()))
			continue; // Anomaly Coop (B2): a co-op client's mirror logs it and takes it off the list, no FATAL
		if (!child)
		{
			// Anomaly Coop (crash triage RL-DNED-2WKU, build 50, a host with split levels): the FATAL below named only the
			// id; the holder and what ALife and the split levels know of the child go first, so the next report names it
			const u16 lost = object->children.back();
			CSE_ALifeDynamicObject* known = ai().get_alife() ? ai().alife().objects().object(lost, true) : nullptr;
			string256 alife_says;
			if (known)
				xr_sprintf(alife_says, "ALife: [%s], parent %u, %s, game vertex %u", known->s_name.c_str(),
				           u32(known->ID_Parent), known->m_bOnline ? "online" : "offline", u32(known->m_tGraphID));
			else
				xr_strcpy(alife_says, "ALife: no such object");
			Msg("! child registered but not found: holder %u [%s][%s] lists child %u; %s; split levels %s, online at a map "
			    "server %s",
			    u32(object->ID), object->name_replace(), object->s_name.c_str(), u32(lost), alife_says,
			    coop::levels::split() ? "on" : "off", coop::levels::online_at_server(lost) ? "yes" : "no");
		}
		R_ASSERT2(child, make_string("child registered but not found [%d]",object->children.back()));
		//		Msg					("SLS-CLEAR : REJECT  [%s][%s] FROM [%s][%s]",child->name(),child->name_replace(),object->name(),object->name_replace());
		Perform_reject(child, object, 2 * NET_Latency);
#ifdef DEBUG
#	ifdef SLOW_VERIFY_ENTITIES
		verify_entities			();
#	endif
#endif
		Perform_destroy(child, mode);
	}

	//	Msg						("SLS-CLEAR : DESTROY [%s][%s]",object->name(),object->name_replace());
	u16 object_id = object->ID;
	entity_Destroy(object);

#ifdef DEBUG
#	ifdef SLOW_VERIFY_ENTITIES
		verify_entities		();
#	endif
#endif

	NET_Packet P;
	P.w_begin(M_EVENT);
	P.w_u32(Device.dwTimeGlobal - 2 * NET_Latency);
	P.w_u16(GE_DESTROY);
	P.w_u16(object_id);
	SendBroadcast(BroadcastCID, P, mode);
}

void xrServer::SLS_Clear()
{
#if 0
	Msg									("SLS-CLEAR : %d objects");
	xrS_entities::const_iterator		I = entities.begin();
	xrS_entities::const_iterator		E = entities.end();
	for ( ; I != E; ++I)
		Msg								("entity to destroy : [%d][%s][%s]",(*I).second->ID,(*I).second->name(),(*I).second->name_replace());
#endif

	u32 mode = net_flags(TRUE,TRUE);
	while (!entities.empty())
	{
		bool found = false;
		xrS_entities::const_iterator I = entities.begin();
		xrS_entities::const_iterator E = entities.end();
		for (; I != E; ++I)
		{
			if ((*I).second->ID_Parent != 0xffff)
				continue;
			found = true;
			Perform_destroy((*I).second, mode);
			break;
		}
		if (!found) //R_ASSERT(found);
		{
			I = entities.begin();
			E = entities.end();
			for (; I != E; ++I)
			{
				if (I->second)
					Msg("! ERROR: can't destroy object [%d][%s] with parent [%d]",
					    I->second->ID, I->second->s_name.size() ? I->second->s_name.c_str() : "unknown",
					    I->second->ID_Parent
					);
				else
					Msg("! ERROR: can't destroy entity [%d][?] with parent[?]", I->first);
			}
			Msg("! ERROR: FATAL: can't delete all entities !");
			entities.clear();
		}
	}
}
