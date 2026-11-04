#include "stdafx.h"
#include "coop_alife.h"
#include "coop_manager.h"
#include "coop_world.h"
#include "coop_entities.h"
#include "coop_join.h"
#include "coop_levels.h"

#include "alife_simulator.h"
#include "alife_object_registry.h"
#include "alife_graph_registry.h"
#include "xrServer_Objects_ALife_Monsters.h"

namespace coop
{
CALifeSimulator& alife_rw() { return *const_cast<CALifeSimulator*>(ai().get_alife()); }

float observer_distance(const Fvector& pos)
{
	const CSE_ALifeCreatureActor* actor = ai().alife().graph().actor();
	// a map server's actor is a ghost: it watches nothing (plan §1.6), the players of its level do
	float best = (actor && !ghost_actor()) ? actor->o_Position.distance_to(pos) : flt_max;
	if (!is_map_authority())
		return best;

	for (u32 peer = 1; peer < kMaxPlayers; ++peer)
	{
		const u16 proxy = join::proxy_of_peer(peer);
		if (proxy == 0xffff || !levels::on_host_level(peer))
			continue; // F3a multilevel: a player on another level watches nothing here, whatever his coordinates say
		const CSE_ALifeDynamicObject* e = ai().alife().objects().object(proxy, true);
		if (!e)
			continue;
		const float d = e->o_Position.distance_to(pos);
		if (d < best)
			best = d;
	}
	return best;
}

SwitchVote client_switch_vote(u16 id)
{
	if (!mgr().is_client())
		return SwitchVote::Default;
	if (id == 0)
		return SwitchVote::Online; // our actor
	if (levels::leads_my_level())
		return levels::xfer_stock_rules(id) ? SwitchVote::Default : SwitchVote::Offline; // F3a: a map server's level is its own
	return entities::client_wants_online(id) ? SwitchVote::Online : SwitchVote::Offline;
}

SwitchVote switch_vote(const CSE_ALifeDynamicObject* e)
{
	if (mgr().is_client())
		return client_switch_vote(e->ID);
	if (world::is_player_character(e) && !(is_map_authority() && join::host_player_in_world(e->ID)))
		return SwitchVote::Offline; // W9: his player is not in the session (or still loading)
	return SwitchVote::Default;
}
} // namespace coop
