#include "stdafx.h"
#include "pch_script.h"
#include "coop_join_impl.h"
#include "coop_manager.h"
#include "coop_world.h"
#include "coop_players.h"
#include "xrCoop/coop_mode.h"

#include "ai_space.h"
#include "level_graph.h"
#include "Level.h"
#include "Actor.h"
#include "ActorCondition.h"
#include "alife_simulator.h"
#include "alife_object_registry.h"
#include "alife_graph_registry.h"
#include "xrServer_Objects_ALife_Monsters.h"
#include "xrServer_Objects_ALife_Items.h"
#include "character_community.h"

#include <algorithm>

namespace coop
{
namespace join
{
// The journal of this player's last transition (xrCoop/coop_arrival.h); outlives the session's state and the level.
arrival::Journal g_arrival;

namespace
{
u32 g_arrival_from = 0; // g_world_generation when it began: only the new world's actor is sampled
u32 g_arrival_lines = 0; // the lines of this process (kArrivalLines at most)
constexpr u32 kArrivalLines = 60;
constexpr u64 kArrivalGiveUpMs = 300000; // a world that never came: the line says so after this long

// ---------------------------------------------------------------- the arrival on a new level
// xrCoop/coop_arrival.h: what happened to a player's character through a level change with the group. One line
// per transition on the player's machine ("[coop] arrival: ..."): the vitals before leaving and kWindowMs after arriving,
// where the health went (hits, the condition's own update, co-op applies), and where his character was put. The host logs
// each mover's last report at the go and his first report kWindowMs after he entered ("[coop] arrival (host): ...").
arrival::Vitals4 own_vitals()
{
	arrival::Vitals4 v;
	CActor* a = smart_cast<CActor*>(g_actor);
	if (!a || !g_pGameLevel)
		return v;
	CActorCondition& c = a->conditions();
	v.health = c.GetHealth();
	v.bleeding = c.BleedingSpeed();
	v.radiation = c.GetRadiation();
	v.satiety = c.GetSatiety();
	return v;
}
} // namespace

void arrival_begin(const char* kind, u16 level)
{
	if (!mgr().active() || !mode().actor_is_player())
		return; // a ghost (a map server's, the world server's) arrives nowhere
	g_arrival.begin(kind, level_name_by_id(level).c_str(), own_vitals(), mgr().now_ms());
	g_arrival_from = g_world_generation;
}

void arrival_step(u64 now)
{
	if (!g_arrival.active())
		return;
	const bool new_world = g_world_generation != g_arrival_from && world_up() && g_actor && !Device.dwPrecacheFrame;
	if (new_world)
	{
		g_arrival.frame(own_vitals().health, now);
		const bool in = mgr().active() && !mode().decides_world() ? mgr().is_live_client() : g_load.screen.stage == transition::Load::None;
		if (in)
			g_arrival.arrived(now);
	}
	const bool stale = g_arrival.stale(now, kArrivalGiveUpMs);
	if (!g_arrival.due(now) && !stale)
		return;
	const std::string line = g_arrival.finish(new_world ? own_vitals() : arrival::Vitals4());
	if (++g_arrival_lines <= kArrivalLines)
		::Msg("[coop] arrival: %s%s", line.c_str(), stale ? " (the new world never came)" : "");
}

void arrival_apply(const char* what, float from, float to) { g_arrival.apply(what, from, to); }

void arrival_hit(const CActor* actor, const SHit* hit, bool blocked)
{
	if (!g_arrival.active() || !hit)
		return;
	const CObject* who = hit->who;
	string64 name;
	if (!who)
		xr_strcpy(name, "nobody");
	else if (who == actor)
		xr_strcpy(name, "self");
	else
		xr_sprintf(name, "'%s' %u", who->cName().c_str(), who->ID());
	g_arrival.hit(ALife::g_cafHitType2String(hit->hit_type), hit->damage(), name, int(hit->boneID), blocked);
}

// Client, after ALife registered the loaded objects (alife_storage_manager.cpp): the new level's graph is loaded (the
// actor's registration set the level up), the actor has not spawned. A character the host placed blind goes on the ground.
void on_alife_registered()
{
	if (!g_join.arrival_check)
		return;
	g_join.arrival_check = false;
	const CLevelGraph* lg = ai().get_level_graph();
	CSE_ALifeCreatureActor* actor = ai().get_alife() ? ai().alife().graph().actor() : nullptr;
	if (!lg || !actor)
	{
		::Msg("! [coop] arrival: no level graph (%p) or actor (%p) to put my character on the ground", lg, actor);
		return;
	}
	const arrival::Ground ground = [lg](float x, float probe_y, float z, float& ground_y, u32& vertex)
	{
		const Fvector probe = {x, probe_y, z};
		if (!lg->valid_vertex_position(probe))
			return false;
		const u32 id = lg->vertex_id(probe);
		if (!lg->valid_vertex_id(id))
			return false;
		ground_y = lg->vertex_plane_y(id, x, z);
		vertex = id;
		return true;
	};
	const Fvector was = actor->o_Position;
	arrival::Point own{was.x, was.y, was.z};
	arrival::Point anchor = own;
	if (lg->valid_vertex_id(actor->m_tNodeID))
	{
		const Fvector v = lg->vertex_position(actor->m_tNodeID); // the vertex the host gave: the level changer's point
		anchor = arrival::Point{v.x, v.y, v.z};
	}
	const arrival::Placed p = arrival::place(own, anchor, ground);
	if (p.how != arrival::How::NoGround)
	{
		actor->o_Position.set(p.at.x, p.at.y, p.at.z);
		if (lg->valid_vertex_id(p.vertex))
			actor->m_tNodeID = p.vertex;
	}
	string256 text;
	xr_sprintf(text, "%s: [%.2f %.2f %.2f] -> [%.2f %.2f %.2f], ground %s%.2f (his point %+.2f over it), exit vertex %u",
	    arrival::how_name(p.how), was.x, was.y, was.z, p.at.x, p.at.y, p.at.z, p.had_ground ? "" : "none ",
	    p.had_ground ? p.at.y - arrival::kLift : 0.f, p.drop, actor->m_tNodeID);
	g_arrival.place(text);
	::Msg("[coop] arrival: the host put my character blind, %s", text);
}

void on_alife_objects_loaded(CALifeObjectRegistry& objects)
{
	// The world being loaded is the host's; on_world_created, at the end of this simulator's constructor, gives it
	// the next generation. It stays a mirror after the session ends, until another world replaces it.
	if (client_loading_world())
		g_mirror_generation = g_world_generation + 1;
	// Layer::place_ghost: the actor of the save is this process's ghost, not a player: the server's
	// layer puts it on its level (a map server's at its key's vertex, the world server's on its service level). Nobody swaps: the
	// numbering is the world server's. A map server whose world has no place for it stopped its session (no longer loading then).
	if (coop::place_ghost(objects))
	{
		if (client_loading_world())
			g_join.swap_done = true; // its world goes live as a client's does (frame, client_go_live)
		return;
	}
	if (!client_loading_world() || g_join.my_proxy == kNoId)
		return;
	g_join.arrival_check = false;
	if (!g_arrival.active())
	{
		const CSE_ALifeDynamicObject* pe = objects.object(g_join.my_proxy, true);
		arrival_begin("join", pe ? level_id_of(pe->m_tGraphID) : levels::wire::kNoLevel); // a world without a transition's go
	}

	CSE_ALifeCreatureActor* actor = smart_cast<CSE_ALifeCreatureActor*>(objects.object(0, true));
	CSE_ALifeHumanStalker* proxy = smart_cast<CSE_ALifeHumanStalker*>(objects.object(g_join.my_proxy, true));
	const bool diag_fail = g_coop_diag_fail_swap != 0;
	g_coop_diag_fail_swap = 0; // one load: a console integer goes into user.ltx
	if (!actor || !proxy || diag_fail)
	{
		::Msg("! [coop] role swap impossible: actor %p, proxy %u %p", actor, g_join.my_proxy, proxy);
		mgr().stop("the world from the host has no character for us");
		return;
	}

	// Where we stand and what we look like. CActor::net_Spawn takes its view (r_model_yaw, camera)
	// from o_torso, not from o_Angle.
	std::swap(actor->o_Position, proxy->o_Position);
	std::swap(actor->o_Angle, proxy->o_Angle);
	std::swap(actor->o_model, proxy->o_model);
	std::swap(actor->o_torso, proxy->o_torso);
	std::swap(actor->m_tNodeID, proxy->m_tNodeID);
	std::swap(actor->m_tGraphID, proxy->m_tGraphID);
	{
		// The actor spawns with the player's own health from his last report, not the proxy's (an unharmable stalker, 1.0):
		// a health set down after going live is one frame's drop that a body-health mod books as a hit on a random body part.
		// A new character has no report: the proxy's.
		const float h = actor->get_health();
		float mine = arrival::spawn_health(proxy->m_ini_string.c_str(), proxy->get_health());
		// In a load that continues his world he stands up with the health he left with (held by this process the
		// moment the load was announced), not with his last report to the host (up to kVitalsMs older)
		float held = mine;
		if (players::condition_carry_health(mgr().session().world_epoch(), held))
			mine = clampr(held, arrival::kMinSpawnHealth, 1.f);
		g_arrival.apply("swap", proxy->get_health(), mine);
		actor->set_health(mine);
		proxy->set_health(h);
		// The host put the character here blind (mark_arrival): the ground comes once the new level's graph is loaded.
		g_join.arrival_check = proxy->m_ini_string.size() && arrival::placed_by_host(proxy->m_ini_string.c_str());
	}
	// The host may sit in a vehicle; our character does not (net_Spawn would seat us in it).
	actor->m_holderID = u16(-1);

	// Who we are: money, community, rank, reputation, name.
	std::swap(actor->m_dwMoney, proxy->m_dwMoney);
	std::swap(actor->m_community_index, proxy->m_community_index);
	std::swap(actor->CSE_ALifeTraderAbstract::m_rank, proxy->CSE_ALifeTraderAbstract::m_rank);
	std::swap(actor->m_reputation, proxy->m_reputation);
	std::swap(actor->m_character_name, proxy->m_character_name);
	std::swap(actor->m_character_name_str, proxy->m_character_name_str); // what the spawns rebuild the name above from
	// The proxy is an NPC of community X; a player character uses actor_X (game_relations.ltx has
	// both, relations and scripts tell them apart).
	if (actor->m_community_index != NO_COMMUNITY_INDEX)
	{
		const shared_str npc = CHARACTER_COMMUNITY::IndexToId(actor->m_community_index, shared_str(), true);
		if (npc.size() && strncmp(npc.c_str(), "actor", 5))
		{
			string128 player;
			xr_sprintf(player, "actor_%s", npc.c_str());
			const CHARACTER_COMMUNITY_INDEX index =
			    CHARACTER_COMMUNITY::IdToIndex(shared_str(player), NO_COMMUNITY_INDEX, true);
			if (index != NO_COMMUNITY_INDEX)
				actor->m_community_index = index;
		}
	}

	// What we carry: items keep their ids, only parents swap. Children lists are rebuilt from
	// ID_Parent when ALife registers the loaded objects right after this hook.
	u32 to_proxy = 0, to_actor = 0;
	for (auto& it : objects.objects())
	{
		CSE_ALifeDynamicObject* e = it.second;
		if (!e)
			continue;
		if (e->ID_Parent == 0)
		{
			e->ID_Parent = g_join.my_proxy;
			++to_proxy;
		}
		else if (e->ID_Parent == g_join.my_proxy)
		{
			e->ID_Parent = 0;
			++to_actor;
		}
	}
	actor->children.clear();
	proxy->children.clear();

	// Two things of the player that live in the save's entities, not in his actor's save.
	// The stance: CActor::net_Spawn takes crouch / slow walk from the actor entity's mstate, which is the host's actor's
	// (the host crouched at the exit: every client came in crouched). The head torch: the host keeps the torches of the
	// players' characters dark (a lit one shone from the wrong place), so its save says off for the player's own.
	// In a load that continues his world both are what he left with; else he stands and the light is off.
	{
		u16 stance = 0, lit_torch = kNoId;
		const bool held = players::condition_carry_extras(mgr().session().world_epoch(), stance, lit_torch);
		actor->mstate = u16((actor->mstate & ~u16(mcCrouch | mcAccel)) | (held ? stance : 0));
		if (held && lit_torch != kNoId)
		{
			CSE_ALifeItemTorch* torch = smart_cast<CSE_ALifeItemTorch*>(objects.object(lit_torch, true));
			if (torch && torch->ID_Parent == 0) // his own, after the parents' swap above
				torch->m_active = true;
		}
	}

	g_join.swap_done = true;
	::Msg("[coop] role swap done: actor <-> proxy %u (%u items to me, %u to the host avatar)", g_join.my_proxy, to_actor,
	    to_proxy);
	world::client_mark_host_avatar(g_join.my_proxy);
}
} // namespace join
} // namespace coop
