// Anomaly Coop - requests: personal map zones (xrCoop/coop_local_zones.h). A map mod may make a script_zone under every
// pin and every right click on the PDA map (alife_create with no parent); its map spot is the player's own. The host's
// request path knows items of our inventory only, so the create would answer nil and no pin could be set. The zone is
// made here and nowhere else: an object of our ALife copy under a leased id, offline, not saved, no request to the host.
// alife():object(id) finds it, the spot finds its object (CMapLocation::Update), and alife():release takes it out again
// and gives its id back to our pool.
#include "stdafx.h"
#include "coop_requests_impl.h"
#include "coop_entities.h"
#include "coop_alife.h"

#include "Level.h"
#include "xrServer.h"
#include "alife_simulator.h"
#include "alife_object_registry.h"
#include "xrServer_Objects_ALife.h"
#include "ai_space.h"

namespace coop
{
namespace requests
{
// The zones of this client, under ids of the same lease. Under g_out_lock, cleared with the pool.
zones::Book g_zones;

// nullptr: not made - the caller goes on as before (the request path refuses a world spawn).
CSE_Abstract* local_zone_create(const char* section, const Fvector& pos, u32 level_vertex, u16 game_vertex)
{
	// the game vertex is what the ALife registries file the object by: a number outside the graph would index past them
	if (!lease_world_ready() || !ai().get_alife() || !ai().get_game_graph() ||
	    !ai().game_graph().valid_vertex_id(game_vertex))
		return nullptr;
	CSE_Abstract* e = make_temp(section, pos, level_vertex, game_vertex, kNoId);
	CSE_ALifeDynamicObject* d = e ? smart_cast<CSE_ALifeDynamicObject*>(e) : nullptr;
	if (!d)
	{
		if (e)
			F_entity_Destroy(e);
		return nullptr;
	}
	u16 id = kNoId;
	size_t ready = 0;
	{
		xrCriticalSectionGuard guard(g_out_lock);
		lease_sync_world_locked();
		id = g_zones.issue(g_pool);
		ready = g_pool.size();
	}
	if (id == kNoId)
	{
		F_entity_Destroy(e);
		if (g_log.allow())
			::Msg("! [coop] a personal map zone [%s] was not made: %u leased ids ready, %u are kept for items", section,
			    (u32)ready, (u32)zones::kKeepForItems);
		return nullptr;
	}
	xrServer* server = Level().Server;
	// the id is ours: reserved in our generator (never free) and nothing here holds it, as create_leased asks
	if (!server || server->IsFreeID(id) || server->ID_to_entity(id) || ai().alife().objects().object(id, true))
	{
		{
			xrCriticalSectionGuard guard(g_out_lock);
			g_zones.cancel(id);
		}
		return_unused_lease(id);
		F_entity_Destroy(e);
		++g_lc.not_created;
		if (g_log.allow())
			::Msg("! [coop] a personal map zone [%s] could not take its leased id %u here", section, id);
		return nullptr;
	}
	{
		entities::ClientApply apply; // our own world takes it as the host's would: nothing of it is a request
		e->ID = id;
		name_with_id(e);
		e->ID_Phantom = kNoId;
		d->m_bOnline = false;
		d->m_flags.set(CSE_ALifeObject::flCanSave, FALSE); // in no roster of the host's digest, in no save
		alife_rw().register_object(d, true);
	}
	return e;
}

// alife():release of a zone made here: out of our ALife copy, its entity kept a while (a script may still hold the se_obj),
// its id back in the pool. False: not a zone of ours - or one that is online, which our frozen ALife never does: the
// stock code takes it.
bool local_zone_release(CSE_Abstract* object)
{
	const u16 id = object->ID;
	{
		xrCriticalSectionGuard guard(g_out_lock);
		lease_sync_world_locked();
		if (!g_zones.has(id))
			return false;
	}
	if (!lease_world_ready())
	{
		// the world is going - we leave it (a disconnect, a rejoin, a load) and the mod lets go of its
		// zones. Their objects go with this world's ALife and their ids with its pool (lease_sync_world_locked):
		// out of the book, handled here. The stock release would be a client's world call with no request path.
		xrCriticalSectionGuard guard(g_out_lock);
		if (g_zones.cancel(id))
			++g_lc.zones_left;
		return true;
	}
	CSE_ALifeDynamicObject* e = ai().alife().objects().object(id, true);
	if (!e || e != object || e->m_bOnline)
	{
		if (g_log.allow())
			::Msg("! [coop] a personal map zone %u is not an offline object of ours here: left to the stock release", id);
		return false;
	}
	{
		xrCriticalSectionGuard guard(g_quarantine_lock);
		g_quarantine_marks.insert(e);
	}
	{
		entities::ClientApply apply;
		alife_rw().release(e, true);
	}
	xrServer& server = *Level().Server;
	xrCriticalSectionGuard guard(g_out_lock);
	// the release freed the id in the generator: reserved again, it is the pool's (and the host's lease of us is as it was)
	const zones::Fate fate = g_zones.retire(g_pool, id,
	    [&server](u16 i)
	    {
		    if (!server.IsFreeID(i))
			    return false;
		    server.PerformIDgen(i);
		    return true;
	    });
	if (fate == zones::Fate::Lost && g_log.allow())
		::Msg("! [coop] the id %u of a released personal map zone is held here by something else: not lent out again", id);
	return true;
}

bool local_zone(const char* section)
{
	return section && zones::wants(section, kNoId) && lease_world_ready() && spawnable_section(section);
}
} // namespace requests
} // namespace coop
