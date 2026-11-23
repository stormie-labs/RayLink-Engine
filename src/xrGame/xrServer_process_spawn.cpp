#include "stdafx.h"
#include "xrServer.h"
#include "xrserver_objects.h"
#include "xrCoop/coop_calls.h" // Anomaly Coop (R32 W9): coop_writer_gate and the id steps

#include <atomic>

#ifdef DEBUG
#	include "xrserver_objects_alife_items.h"
#endif

// Anomaly Coop (R32 W9): the first lines of a kind, then every 100th (a pool that stays empty must not flood the log).
static bool coop_w9_line(std::atomic<u32>& count, u32& n)
{
	n = ++count;
	return n <= 10 || n % 100 == 0;
}

// Anomaly Coop (R32 W9): see xrServer.h. A peek at the pool (Mode::own_free_id hands out the next free id without taking it; an empty
// pool asks its world for a block at once): the stock code that follows takes its ids through PerformIDgen.
bool xrServer::CoopMayCreate(const char* what)
{
	if (!coop::mode().takes_own_ids() || coop::mode().own_free_id() != 0xffff)
		return true;
	const coop::calls::WriterGate setting = coop::calls::writer_gate_setting().load();
	const bool refused = coop::calls::no_id(setting, coop::calls::IdStep::Start) == coop::calls::NoId::Refuse;
	static std::atomic<u32> s_lines{0};
	u32 n = 0;
	if (coop_w9_line(s_lines, n))
		Msg("! [coop] W9: no id of this map server's pool is free for %s (#%u, coop_writer_gate %s): %s; a block of ids is asked for",
		    what ? what : "a create", n, coop::calls::writer_gate_name(setting),
		    refused ? "refused, nothing is created" : "it goes on with a stock id, which the world refuses (IdRange) and the repair releases");
	return !refused;
}

void xrServer::CoopTookStockId(u16 id)
{
	const coop::calls::WriterGate setting = coop::calls::writer_gate_setting().load();
	// under enforce every start that asked CoopMayCreate was refused there, so this is a create that did not ask: a stock chain that
	// already runs (IdStep::Chain: the stock id in every setting) or a create of coop's own C++ that has no CoopMayCreate yet (the
	// line cannot tell which); under log it may be any create
	const bool starts_refused = coop::calls::no_id(setting, coop::calls::IdStep::Start) == coop::calls::NoId::Refuse;
	static std::atomic<u32> s_lines{0};
	u32 n = 0;
	if (coop_w9_line(s_lines, n))
		Msg("! [coop] W9: no id of this map server's pool was free for %s: the stock generator gave %u, outside the pool (the world "
		    "refuses its Spawn, IdRange, and the repair releases it; #%u, coop_writer_gate %s)",
		    starts_refused ? "a create that did not ask CoopMayCreate (a stock chain that already runs, such as an object's supplies, or "
		                     "a create of coop's own C++)"
		                   : "a create",
		    u32(id), n, coop::calls::writer_gate_name(setting));
}

CSE_Abstract* xrServer::Process_spawn(NET_Packet& P, ClientID sender, BOOL bSpawnWithClientsMainEntityAsParent,
                                      CSE_Abstract* tpExistedEntity)
{
	// create server entity
	xrClientData* CL = ID_to_client(sender);
	CSE_Abstract* E = tpExistedEntity;
	if (!E)
	{
		// read spawn information
		string64 s_name;
		P.r_stringZ(s_name);
		// create entity
		E = entity_Create(s_name);
		R_ASSERT3(E, "Can't create entity.", s_name);
		E->Spawn_Read(P);
		if (
				//.				!( (game->Type()==E->s_gameid) || (GAME_ANY==E->s_gameid) ) ||

			!E->m_gameType.MatchType((u16)game->Type()) ||
			!E->match_configuration() ||
			!game->OnPreCreate(E)
		)
		{
#ifndef MASTER_GOLD
			Msg			("- SERVER: Entity [%s] incompatible with current game type.",*E->s_name);
#endif // #ifndef MASTER_GOLD
			F_entity_Destroy(E);
			return NULL;
		}

		//		E->m_bALifeControl = false;
	}
	else
	{
		VERIFY(E->m_bALifeControl);
		//		E->owner			= CL;
		//		if (CL != NULL)
		//		{
		//			int x=0;
		//			x=x;
		//		};
		//		E->m_bALifeControl = true;
	}

	CSE_Abstract* e_parent = 0;
	if (E->ID_Parent != 0xffff)
	{
		e_parent = ID_to_entity(E->ID_Parent);
		if (!e_parent)
		{
			R_ASSERT(!tpExistedEntity);
			//			VERIFY3			(smart_cast<CSE_ALifeItemBolt*>(E) || smart_cast<CSE_ALifeItemGrenade*>(E),*E->s_name,E->name_replace());
			F_entity_Destroy(E);
			return NULL;
		}
	}

	// Anomaly Coop (R32 W9): a new entity takes its ids below (its own when it has none, a phantom's): a map server with no id left in its
	// pool refuses it here under coop_writer_gate = enforce, before any id is taken, with the stock "not created" (as for a missing
	// parent above); an existing entity (tpExistedEntity: a switch online) keeps the id it has
	const bool coop_takes_ids = E->ID == 0xffff || (E->RespawnTime && 0xffff == E->ID_Phantom) || E->s_flags.is(M_SPAWN_OBJECT_PHANTOM);
	if (!tpExistedEntity && coop_takes_ids && !CoopMayCreate("a spawn packet"))
	{
		F_entity_Destroy(E);
		return NULL;
	}

	// check if we can assign entity to some client
	if (0 == CL)
	{
		CL = SelectBestClientToMigrateTo(E);
	}

	// check for respawn-capability and create phantom as needed
	if (E->RespawnTime && (0xffff == E->ID_Phantom))
	{
		// Create phantom
		CSE_Abstract* Phantom = entity_Create(*E->s_name);
		R_ASSERT(Phantom);
		Phantom->Spawn_Read(P);
		Phantom->ID = PerformIDgen(0xffff);
		Phantom->ID_Phantom = Phantom->ID; // Self-linked to avoid phantom-breeding
		Phantom->owner = NULL;
		entities.insert(mk_pair(Phantom->ID, Phantom));

		Phantom->s_flags.set(M_SPAWN_OBJECT_PHANTOM,TRUE);

		// Spawn entity
		E->ID = PerformIDgen(E->ID);
		E->ID_Phantom = Phantom->ID;
		E->owner = CL;
		entities.insert(mk_pair(E->ID, E));
	}
	else
	{
		if (E->s_flags.is(M_SPAWN_OBJECT_PHANTOM))
		{
			// Clone from Phantom
			E->ID = PerformIDgen(0xffff);
			E->owner = CL; //		= SelectBestClientToMigrateTo	(E);
			E->s_flags.set(M_SPAWN_OBJECT_PHANTOM,FALSE);
			entities.insert({ E->ID, E });
		}
		else
		{
			// Simple spawn
			if (bSpawnWithClientsMainEntityAsParent)
			{
				R_ASSERT(CL);
				CSE_Abstract* P = CL->owner;
				R_ASSERT(P);
				E->ID_Parent = P->ID;
			}
			E->ID = PerformIDgen(E->ID);
			E->owner = CL;
			entities.insert({ E->ID, E });
		}
	}

	// PROCESS NAME; Name this entity
	if (CL && (E->s_flags.is(M_SPAWN_OBJECT_ASPLAYER)))
	{
		CL->owner = E;
		//		E->set_name_replace	(CL->Name);
	}

	// PROCESS RP;	 3D position/orientation
	PerformRP(E);
	E->s_RP = 0xFE; // Use supplied

	// Parent-Connect
	if (!tpExistedEntity)
	{
		game->OnCreate(E->ID);

		if (0xffff != E->ID_Parent)
		{
			R_ASSERT(e_parent);

			game->OnTouch(E->ID_Parent, E->ID);

			e_parent->children.push_back(E->ID);
		}
	}

	// create packet and broadcast packet to everybody
	NET_Packet Packet;
	if (CL)
	{
		// For local ONLY
		E->Spawn_Write(Packet,TRUE);
		if (E->s_flags.is(M_SPAWN_UPDATE))
			E->UPDATE_Write(Packet);
		SendTo(CL->ID, Packet, net_flags(TRUE,TRUE));

		// For everybody, except client, which contains authorative copy
		E->Spawn_Write(Packet,FALSE);
		if (E->s_flags.is(M_SPAWN_UPDATE))
			E->UPDATE_Write(Packet);
		SendBroadcast(CL->ID, Packet, net_flags(TRUE,TRUE));
	}
	else
	{
		E->Spawn_Write(Packet,FALSE);
		if (E->s_flags.is(M_SPAWN_UPDATE))
			E->UPDATE_Write(Packet);
		ClientID clientID;
		clientID.set(0);
		SendBroadcast(clientID, Packet, net_flags(TRUE,TRUE));
	}
	if (!tpExistedEntity)
	{
		game->OnPostCreate(E->ID);
	};

	// log
	//Msg		("- SERVER: Spawning '%s'(%d,%d,%d) as #%d, on '%s'", E->s_name_replace, E->g_team(), E->g_squad(), E->g_group(), E->ID, CL?CL->Name:"*SERVER*");
	return E;
}

/*
void spawn_WithPhantom
void spawn_FromPhantom
void spawn_Simple
*/
