#include "stdafx.h"
#include "coop_entities.h"
#include "coop_entities_impl.h"
#include "coop_manager.h"
#include "xrCoop/coop_mode.h"
#include "coop_levels.h"
#include "coop_net.h"
#include "coop_join.h"
#include "coop_world.h"
#include "coop_requests.h"
#include "coop_script.h"
#include "coop_sync_api.h"
#include "xrCoop/coop_mirror.h"
#include "coop_items.h"
#include "coop_initial_state.h"
#include "coop_writer.h"

#include "Level.h"
#include "GameObject.h"
#include "coop_alife.h"
#include "alife_simulator.h"
#include "alife_object_registry.h"
#include "alife_graph_registry.h"
#include "alife_group_registry.h"
#include "xrServer.h"
#include "xrServer_Objects_ALife.h"
#include "xrServer_Objects_ALife_Monsters.h"
#include "../xrServerEntities/xrMessages.h"

namespace coop
{
namespace events
{
void client_forget(u16 local_id);
}

namespace entities
{
// F3a multilevel: online here, or "online at a client" (coop_levels_alife.cpp): a player of the host's level hears the first only.
bool online_here(const CSE_ALifeDynamicObject* e) { return e->m_bOnline && !mode().online_elsewhere(e->ID); }

// A game object of `id` here that is not on its way out (as coop_initial_state.cpp object_of sees an item's).
bool game_object_here(u16 id)
{
	if (!g_pGameLevel)
		return false;
	const CObject* o = Level().Objects.net_Find(id);
	return o && !o->getDestroy();
}

xr_set<u16> g_online; // local ids the host has online
bool log_allowed(u32& n) { return ++n <= 20 || (n % 100) == 0; }

u64 g_replaced = 0; // R32 W3 (npc-cleanup F5): spawns that found another object under their id here, which went
u64 g_squads_whole = 0; // R32 W3 (npc-alife F4): coop_squad_complete raised
u64 g_stale_spawns = 0; // R32 W3 (verify-D finding 7): spawns of another object under an id this process writes, which went nowhere

namespace
{
int g_applying = 0; // > 0 while we execute a host command on our ALife (ClientApply)

// ---------------------------------------------------------------- host side

enum class HostEventKind : u8
{
	Registered,
	Released,
	Switched,
};
struct HostEvent
{
	HostEventKind kind;
	u16 id;
	bool online;
	// Build 57 (S3): a spawn of what a map server reported goes out under its EchoOf (host_spawn_from_leader); none for the rest
	u16 echo = levels::wire::kNoLevel;
	// Registered while this process applied another's word (ClientApply): not its own create
	bool applied = false;
};
// Filled from ALife/server hooks (game thread or main thread, never concurrently with frame()).
xr_vector<HostEvent> g_host_events;
u64 g_host_spawns = 0, g_host_releases = 0, g_host_switches = 0;

void write_packet(Writer& w, const NET_Packet& p)
{
	w.u16(u16(p.B.count));
	w.bytes(p.B.data, p.B.count);
}

bool read_packet(Reader& r, NET_Packet& p)
{
	const u16 n = r.u16();
	if (!r.ok() || n > sizeof(p.B.data))
		return false;
	p.B.count = n;
	p.r_pos = 0;
	return r.bytes(p.B.data, n);
}

// Where the entity is. A psy phantom's server entity does not know: CPhantom::net_Export writes its update in a layout
// CSE_ALifeCreatureAbstract::UPDATE_Read does not read (stock: a float and two u32 where the timestamp, the flags and the
// position are read), so from its first export on its o_Position is zeros. Stand, build 52 (2026-11-17): a client's copy of
// a phantom the host spawned 12 m from him came online 277.3 m away, at the origin of the level, and flew from there. A
// phantom is placed by its game object here while it has one.
Fvector placement_position(const CSE_ALifeDynamicObject* e)
{
	if (g_pGameLevel && smart_cast<const CSE_ALifeCreaturePhantom*>(e))
	{
		const CObject* o = Level().Objects.net_Find(e->ID);
		if (o && !o->getDestroy())
			return o->Position();
	}
	return e->o_Position;
}

void write_placement(Writer& w, const CSE_ALifeDynamicObject* e)
{
	const Fvector at = placement_position(e);
	float pos[3] = {at.x, at.y, at.z};
	w.vec3(pos);
	float ang[3] = {e->o_Angle.x, e->o_Angle.y, e->o_Angle.z};
	w.vec3(ang);
	w.u32(e->m_tNodeID);
	w.u16(e->m_tGraphID);
}

bool write_spawn(Writer& w, CSE_ALifeDynamicObject* e, NET_Packet& spawn, NET_Packet& update)
{
	w.u16(e->ID);
	w.u16(e->ID_Parent);
	w.boolean(online_here(e));
	write_placement(w, e);
	write_packet(w, spawn);
	write_packet(w, update);
	// R32 W8.4 (§8a A12): after the clone's replacement state, the item's uses, condition, rounds and the decider's field versions
	return requests::write_clone_initial(w, e) && items::write_initial(w, e);
}

void host_send_spawn(CSE_ALifeDynamicObject* e)
{
	NET_Packet spawn, update;
	e->Spawn_Write(spawn, FALSE);
	update.w_begin(M_UPDATE);
	e->UPDATE_Write(update);

	Writer w(spawn.B.count + update.B.count + 32);
	if (!write_spawn(w, e, spawn, update))
		return;
	net::host_broadcast(Msg::EntitySpawn, w);
	++g_host_spawns;
}

} // namespace

void write_switch(Writer& w, CSE_ALifeDynamicObject* e, bool online)
{
	w.u16(e->ID);
	w.boolean(online);
	write_placement(w, e);
	NET_Packet update;
	update.w_begin(M_UPDATE);
	e->UPDATE_Write(update);
	write_packet(w, update);
}

namespace
{
void host_send_switch(CSE_ALifeDynamicObject* e, bool online)
{
	Writer w(256);
	write_switch(w, e, online);
	net::host_broadcast(Msg::EntitySwitch, w);
	++g_host_switches;
}

void host_process_events()
{
	PROF_EVENT("coop/coop_entities/host_process_events");
	if (g_host_events.empty())
		return;
	// Review R2 C2: KERNEL:disconnect/start run earlier in the same FrameMove (CApplication::OnFrame has a
	// higher priority than coop::on_frame): the simulator is gone, the events belong to the old world, and the
	// clients rejoin through SessionReset anyway.
	if (!ai().get_alife())
	{
		g_host_events.clear();
		return;
	}
	xr_vector<HostEvent> events;
	events.swap(g_host_events);
	for (const HostEvent& ev : events)
	{
		switch (ev.kind)
		{
		case HostEventKind::Registered:
			if (CSE_ALifeDynamicObject* e = ai().alife().objects().object(ev.id, true))
			{
				// under the echo of a map server's report: that level's players have it from their map server
				levels::with_echo(ev.echo, [&] { host_send_spawn(e); });
			}
			break;
		case HostEventKind::Released:
		{
			Writer w(4);
			w.u16(ev.id);
			net::host_broadcast(Msg::EntityDestroy, w);
			++g_host_releases;
			break;
		}
		case HostEventKind::Switched:
			if (CSE_ALifeDynamicObject* e = ai().alife().objects().object(ev.id, true))
				host_send_switch(e, ev.online);
			break;
		}
	}
}

// ---------------------------------------------------------------- client side

xr_set<u16> g_refetched; // entities asked again because their holder was missing (once each)
u64 g_client_spawns = 0, g_client_releases = 0, g_client_switches = 0, g_missing = 0;
u64 g_unbound_dropped = 0; // member ids forgotten by squads from the wire (released, dead, switched alone, disbanded)
// B2 (live 2026-10-23, FATAL "child registered but not found" at a client's exit): the mirror's holder/child books.
u64 g_mirror_with_holder = 0; // children the host had offline, brought online with their holder here (mirror::placement)
u64 g_mirror_broken_holder = 0; // children left offline: their holder is online here without a server entity
u64 g_mirror_stale = 0; // stale child ids of a gone entity taken out of a holder's list before an EntitySpawn
u64 g_mirror_online_now = 0; // free entities online on the host without a game vertex (psy phantoms), brought online here
u64 g_mirror_lost = 0; // child ids with no server entity met by a holder leaving the server (mirror_lost_child)
u64 g_mirror_listed_destroys = 0; // entities that left the server while their holder still listed them
// B2 review M2: two budgets. The routine "comes online in its holder" line fires for a whole inventory at a world's
// load and must not use up the lines of the repairs (the "!" lines a live run is read for).
u32 g_mirror_log = 0, g_mirror_log_info = 0;
bool mirror_log_allowed() { return log_allowed(g_mirror_log); }

void read_placement(Reader& r, Fvector& pos, Fvector& ang, u32& node, u16& graph)
{
	float p[3], a[3];
	r.vec3(p);
	r.vec3(a);
	pos.set(p[0], p[1], p[2]);
	ang.set(a[0], a[1], a[2]);
	node = r.u32();
	graph = r.u16();
}

void place(CSE_ALifeDynamicObject* e, const Fvector& pos, const Fvector& ang, u32 node, u16 graph_id)
{
	// Only objects that live in the graph registry move between its vertices (graph().change
	// requires used_ai_locations: a squad that is not bound to its members here has none and must
	// not be added to the level registry, see apply_group_switch).
	if (!e->m_bOnline && e->ID_Parent == kNoId && e->used_ai_locations() && e->m_tGraphID != graph_id &&
	    ai().game_graph().valid_vertex_id(graph_id) && ai().game_graph().valid_vertex_id(e->m_tGraphID))
		alife_rw().graph().change(e, e->m_tGraphID, graph_id);
	e->o_Position = pos;
	e->o_Angle = ang;
	e->m_tNodeID = node;
	e->m_tGraphID = graph_id;
}

void apply_update(CSE_ALifeDynamicObject* e, NET_Packet& update)
{
	update.r_pos = 0;
	u16 type;
	update.r_begin(type);
	if (type == M_UPDATE)
		e->UPDATE_Read(update);
}

} // namespace

// A squad lists its members by id. A squad from the save has them bound by the engine
// (on_after_game_load); a squad that came over the wire has null member pointers, which
// switch_online/offline and on_failed_switch_online dereference. Its members are ordinary
// objects here (m_group_id free), so the stock on_unregister never takes them out of it:
// unregister_member only forgets such an id (alife_online_offline_group.cpp).
bool group_bound(const CSE_ALifeOnlineOfflineGroup& g)
{
	for (const auto& m : g.squad_members())
		if (!m.second)
			return false;
	return true;
}

namespace
{
// Client: forget `id` in every squad that lists it unbound. On the host the creature has left its
// squad (release, death, a switch of its own); here nothing else would drop the id, and a stale id
// makes the squad switch whatever creature reuses it later.
void drop_unbound(u16 id)
{
	const CALifeGroupRegistry::OBJECTS& groups = ai().alife().groups().objects();
	for (const auto& it : groups)
	{
		CSE_ALifeOnlineOfflineGroup* g = it.second;
		const CSE_ALifeOnlineOfflineGroup::MEMBERS& members = g->squad_members();
		const auto found = members.find(id);
		if (found != members.end() && !found->second)
		{
			g->unregister_member(id);
			++g_unbound_dropped;
		}
	}
}

} // namespace

// The host switches squad members only together with their squad. A member that the host
// switches (or removes) on its own has left its squad there (death, script): do the same here,
// otherwise the squad keeps a raw pointer to it (bound) or a stale id (from the wire).
void leave_group(CSE_ALifeDynamicObject* e)
{
	CSE_ALifeMonsterAbstract* m = smart_cast<CSE_ALifeMonsterAbstract*>(e);
	if (!m)
		return;
	if (m->m_group_id == kNoId)
	{
		drop_unbound(m->ID);
		return;
	}
	const CALifeGroupRegistry::OBJECTS& groups = ai().alife().groups().objects();
	auto it = groups.find(m->m_group_id);
	if (it == groups.end() || !it->second->member(m->ID, true))
	{
		m->m_group_id = kNoId;
		return;
	}
	it->second->unregister_member(m->ID);
}

namespace
{
// Client: a squad is going away. As on the host (remove_squad) it is empty when released: bound
// members become ordinary objects, unbound ids are forgotten (a destructor never meets them).
void disband(CSE_ALifeOnlineOfflineGroup& g)
{
	xr_vector<u16> ids;
	for (const auto& m : g.squad_members())
	{
		ids.push_back(m.first);
		if (!m.second)
			++g_unbound_dropped;
	}
	for (u16 id : ids)
		g.unregister_member(id);
}

// Client, a squad from the wire: the host took its dead out of it at death (on_death ->
// notify_on_member_death), nothing tells the squad here. Forget them, or its next switch drags
// the bodies offline while the host keeps them where they fell.
void drop_dead_unbound(CSE_ALifeOnlineOfflineGroup& g)
{
	xr_vector<u16> dead;
	for (const auto& m : g.squad_members())
	{
		CSE_ALifeMonsterAbstract* c =
		    m.second ? nullptr : smart_cast<CSE_ALifeMonsterAbstract*>(ai().alife().objects().object(m.first, true));
		if (c && !c->g_Alive())
			dead.push_back(m.first);
	}
	for (u16 id : dead)
		g.unregister_member(id);
	g_unbound_dropped += dead.size();
}

void apply_switch(CSE_ALifeDynamicObject* e, bool online, const Fvector& pos, const Fvector& ang, u32 node,
                  u16 graph_id, NET_Packet* update)
{
	if (online)
	{
		if (e->m_bOnline)
			return;
		place(e, pos, ang, node, graph_id);
		if (update)
			apply_update(e, *update);
		alife_rw().switch_online(e);
		return;
	}
	if (e->m_bOnline)
		alife_rw().switch_offline(e);
	place(e, pos, ang, node, graph_id);
	if (update)
		apply_update(e, *update);
}

void apply_group_switch(CSE_ALifeOnlineOfflineGroup& g, bool online, const Fvector& pos, const Fvector& ang,
                        u32 node, u16 graph_id, NET_Packet& update)
{
	const bool bound = group_bound(g);
	if (!bound)
		drop_dead_unbound(g); // before the votes: a body's own vote is the host's (EntitySwitch of its own)
	xr_vector<u16> members;
	for (const auto& m : g.squad_members())
		members.push_back(m.first);
	// Members follow their squad: keep their votes in step for the local switch manager.
	for (u16 id : members)
	{
		if (online)
			g_online.insert(id);
		else
		{
			g_online.erase(id);
			g_tracks.erase(id);
		}
	}

	if (bound)
	{
		if (online && !g.m_bOnline)
		{
			place(&g, pos, ang, node, graph_id);
			apply_update(&g, update);
			// Offline members stand where their squad is (CSE_ALifeOnlineOfflineGroup::update on the host).
			for (const auto& m : g.squad_members())
			{
				if (m.second->m_bOnline)
					continue;
				m.second->o_Position = pos;
				m.second->m_tNodeID = node;
				m.second->m_tGraphID = graph_id;
			}
			alife_rw().switch_online(&g);
		}
		else if (!online)
		{
			if (g.m_bOnline)
				alife_rw().switch_offline(&g);
			place(&g, pos, ang, node, graph_id);
			apply_update(&g, update);
		}
		return;
	}

	// The squad came over the wire: its members are ordinary top-level objects here, switch them
	// one by one and leave the squad itself alone.
	place(&g, pos, ang, node, graph_id);
	apply_update(&g, update);
	for (u16 id : members)
	{
		CSE_ALifeMonsterAbstract* m = smart_cast<CSE_ALifeMonsterAbstract*>(ai().alife().objects().object(id, true));
		if (m && m->ID_Parent == kNoId && m->m_group_id == kNoId)
			apply_switch(m, online, pos, ang, node, graph_id, nullptr);
	}
}

// The holder is online here, so the item must be too: on the host it was created online (like
// alife():create with an online parent). Same steps as add_online_impl takes for each child.
void bring_child_online(CSE_ALifeDynamicObject* e)
{
	xrServer& server = alife_rw().server();
	CSE_ALifeDynamicObject* holder = ai().alife().objects().object(e->ID_Parent, true);
	if (!holder || !holder->m_bOnline || !server.ID_to_entity(e->ID_Parent))
		return; // stays offline in its holder, comes online with it
	ClientID client;
	client.set(server.GetServerClient() ? server.GetServerClient()->ID.value() : 0);
	NET_Packet packet;
	e->s_flags.or(M_SPAWN_UPDATE);
	CSE_Abstract* abstract = e;
	server.entity_Destroy(abstract); // frees the id; ALife keeps the entity (m_bALifeControl)
	e->o_Position = holder->o_Position;
	e->m_tNodeID = holder->m_tNodeID;
	server.Process_spawn(packet, client, FALSE, e);
	e->s_flags.and(u16(-1) ^ M_SPAWN_UPDATE);
	e->m_bOnline = true;
}

// A copy's object `id` (local numbering) goes: the writer released it (EntityDestroy), or another object has its id now (R32 W3:
// client_on_spawn's Replace), or a clone the writer refused.
void client_remove(u16 id)
{
	g_online.erase(id);
	g_tracks.erase(id);
	CSE_ALifeDynamicObject* e = ai().alife().objects().object(id, true);
	if (!e)
		return;
	ClientApply scope;
	leave_group(e); // on the host scripts unregister a member before releasing it
	if (CSE_ALifeOnlineOfflineGroup* g = smart_cast<CSE_ALifeOnlineOfflineGroup*>(e))
	{
		disband(*g);
		if (!g->m_bOnline && !ai().game_graph().valid_vertex_id(g->m_tGraphID))
			g->m_tGraphID = ai().alife().graph().actor()->m_tGraphID; // never placed: unregister reads its vertex
		alife_rw().release(e, true); // a squad has no game object, even when online
	}
	else if (e->m_bOnline)
	{
		// Online: the standard event chain destroys the game object and then the entity (E3).
		NET_Packet packet;
		packet.w_begin(M_EVENT);
		packet.w_u32(Level().timeServer());
		packet.w_u16(GE_DESTROY);
		packet.w_u16(id);
		Level().Send(packet, net_flags(TRUE, TRUE));
	}
	else
		alife_rw().release(e, true);
	++g_client_releases;
}

// R32 W3 receive (npc-alife F4; verify-D finding 4): putting a squad in its smart writes the smart. That is done where the smart is
// written: by the process that leads the squad's map apart from its world (Mode::level(): a map server), for a squad on that map
// (reconcile::squad_complete_here). The world server takes a map server's squads whole from its journal and runs no level logic; a map
// server hears of squads of every map, and the server of a squad's own map writes that smart; a mirror writes nothing.
bool squad_complete_here(const CSE_ALifeOnlineOfflineGroup& g)
{
	return reconcile::squad_complete_here(mode().level() != nullptr, levels::level_of_vertex(g.m_tGraphID), levels::my_level());
}

// R32 W3 receive (npc-alife F4, design R32-final.md §1 W3): a squad from the wire lists its members by id, and they come after it (a
// smart's respawn makes the squad first). The process that writes its smart (squad_complete_here) hears coop_squad_complete(squad)
// once, when the squad is whole here: at its own spawn when its members came first, else at its last missing member's
// (reconcile::squad_whole). Its srv Lua (coop_srv_levels_brain.script, when_srv) puts the squad in its smart with the stock
// assign_squad_to_smart. A squad from a save is bound, whole already.
void squads_made_whole(const CSE_ALifeDynamicObject* e)
{
	const auto here = [](u16 id) { return ai().alife().objects().object(id, true) != nullptr; };
	const auto members_of = [](const CSE_ALifeOnlineOfflineGroup& g) {
		std::vector<u16> ids;
		for (const auto& m : g.squad_members())
			ids.push_back(m.first);
		return ids;
	};
	const auto raise = [&](const CSE_ALifeOnlineOfflineGroup& g) {
		if (!squad_complete_here(g))
			return; // another map's squad: its own map server puts it in its smart
		if (!reconcile::squad_whole(members_of(g), here))
			return;
		script::queue_callback("coop_squad_complete", {script::cb_int(g.ID)});
		++g_squads_whole;
	};
	if (const CSE_ALifeOnlineOfflineGroup* g = smart_cast<const CSE_ALifeOnlineOfflineGroup*>(e))
	{
		raise(*g);
		return;
	}
	if (!smart_cast<const CSE_ALifeMonsterAbstract*>(e))
		return; // only a creature is a member
	for (const auto& it : ai().alife().groups().objects())
	{
		const CSE_ALifeOnlineOfflineGroup* g = it.second;
		const auto listed = g->squad_members().find(e->ID);
		if (listed != g->squad_members().end() && !listed->second)
			raise(*g);
	}
}

void client_on_spawn(PeerId, Reader& r)
{
	const u16 id_host = r.u16();
	const u16 id = join::to_local(id_host);
	const u16 parent_host = r.u16();
	const u16 parent = parent_host == kNoId ? kNoId : join::to_local(parent_host);
	// 0 offline, any other byte online at the sender (as Reader::boolean read it); build 57 (protocol 33): 2 = online at the
	// authority of the level I stand on, which the world server's trunk writes into a spawn of my level's map server
	// (xrCoop/coop_levels_wire.h mark_spawn_of_your_level). A co-op host writes 0 or 1.
	const u8 online_word = r.u8();
	const bool online = online_word != 0;
	Fvector pos, ang;
	u32 node;
	u16 graph_id;
	read_placement(r, pos, ang, node, graph_id);
	NET_Packet spawn, update;
	if (!read_packet(r, spawn) || !read_packet(r, update) || !ai().get_alife())
		return;
	std::vector<u8> initial;
	if (!requests::read_clone_initial(r, id, initial))
		return;
	// R32 W8.4 (§8a A12): the item's initial state rides after the clone's (write_spawn); read whole here, applied below
	const Reader initial_state(r.cursor(), r.remaining());
	if (!items::read_initial(r, nullptr))
		return;
	u16 type;
	spawn.r_begin(type);
	string64 section;
	spawn.r_stringZ(section);
	spawn.r_pos = 0;
	if (CSE_ALifeDynamicObject* known = ai().alife().objects().object(id, true))
	{
		// R32 W3 receive (npc-cleanup F5): the same kind is a repeat; another kind is another object under this id now (this copy
		// missed a release, the id was given again): ours goes, the writer's comes. A player's character is kind 0, never replaced.
		// Unless this process writes its object (verify-D finding 7): then the spawn is a stale word of its world (the echo of its own
		// spawn from before it released that object and made another under the id), and the writer's object stands.
		const u16 kind_here = id == 0 || world::is_player_character(known) ? u16(0) : reconcile::kind_of(known->s_name.c_str());
		switch (reconcile::spawn_of_known(kind_here, reconcile::kind_of(section), writing::writes(id)))
		{
		case reconcile::Known::Repeat:
		{
			if (!initial.empty() && !requests::stage_clone_initial(known, initial, true))
			{
				g_id_retry.add(id_host, mgr().now_ms());
				return;
			}
			// W8.4 (verify-D finding 6): the state and the decider's versions together, into an offline copy's entity only; the writer's
			// own object and a game object here keep their own
			Reader state = initial_state;
			const bool takes = reconcile::repeat_takes_state(writing::writes(id), game_object_here(id));
			items::read_initial(state, takes ? known : nullptr);
			g_id_retry.done(id_host);
			return; // already have it (P6: repeat is harmless)
		}
		case reconcile::Known::Keep:
			++g_stale_spawns;
			if (mirror_log_allowed())
				::Msg("[coop] id %u is [%s] here, written here, and [%s] in a word of the world: a stale spawn, ours stands (R32 W3)",
				    id_host, known->s_name.c_str(), section);
			g_id_retry.done(id_host);
			return;
		case reconcile::Known::Replace:
			break;
		}
		++g_replaced;
		if (mirror_log_allowed())
			::Msg("[coop] id %u is [%s] here and [%s] at its writer: replaced (R32 W3)", id_host, known->s_name.c_str(), section);
		client_remove(id);
		if (ai().alife().objects().object(id, true))
		{
			g_id_retry.add(id_host, mgr().now_ms()); // its game object goes through the event chain first: asked for again
			return;
		}
	}
	if (alife_rw().server().ID_to_entity(id))
	{
		// A local non-ALife object (EntityLocal, bolt...) holds the id: PerformIDgen would assert.
		++g_missing;
		g_id_retry.add(id_host, mgr().now_ms()); // D3: it goes in a moment, the entity is asked for again
		::Msg("! [coop] id %u is taken here by a local object, mirrored entity skipped", id_host);
		return;
	}
	if (!alife_rw().server().IsFreeID(id))
	{
		// F2 T12b round 1 (R2 minor "ID already used"): our generator has it taken with no entity holding it (a leased id
		// of ours, an entity between its steps): PerformIDgen below would assert
		++g_missing;
		g_id_retry.add(id_host, mgr().now_ms());
		::Msg("! [coop] id %u is taken in our id generator, mirrored entity skipped", id_host);
		return;
	}
	if (parent != kNoId && !ai().alife().objects().object(parent, true))
	{
		// Its holder is not here (P6): fetch the holder, then this entity again (once).
		++g_missing;
		if (g_refetched.insert(id).second)
		{
			Writer w(8);
			w.u16(parent_host);
			net::client_send(Msg::StateDumpRequest, w);
			Writer w2(4);
			w2.u16(id_host);
			net::client_send(Msg::StateDumpRequest, w2);
		}
		return;
	}

	CSE_Abstract* abstract = F_entity_Create(section);
	CSE_ALifeDynamicObject* e = smart_cast<CSE_ALifeDynamicObject*>(abstract);
	if (!e)
	{
		::Msg("! [coop] cannot create mirrored entity %u [%s]", id, section);
		F_entity_Destroy(abstract);
		return;
	}
	spawn.r_pos = 0;
	e->Spawn_Read(spawn);
	apply_update(e, update);
	// Script metadata must exist even for callbacks invoked by register_object/bring_child_online.
	e->ID = id;
	if (!requests::stage_clone_initial(e, initial))
	{
		F_entity_Destroy(abstract);
		return;
	}
	ClientApply scope;
	e->ID = alife_rw().server().PerformIDgen(id); // free: checked above
	e->ID_Parent = parent;
	// Not registered yet: set the placement only, register_object puts it into the registries.
	e->o_Position = pos;
	e->o_Angle = ang;
	e->m_tNodeID = node;
	e->m_tGraphID = graph_id;
	e->m_bOnline = false;
	{
		// R32 W8.4: the item's uses, condition, rounds and the decider's versions, into the entity before its object can come online
		// (net_Spawn reads the stock bytes); a non-item carries an empty state
		Reader state = initial_state;
		items::read_initial(state, e);
	}
	CSE_ALifeDynamicObject* holder = parent == kNoId ? nullptr : ai().alife().objects().object(parent, true);
	if (holder)
	{
		// B2: the id is neither in ALife nor on the server here, so a holder still listing it keeps a stale id of a gone
		// entity; register_object would list it twice and the holder's teardown would meet the stale one.
		xr_vector<u16>& list = holder->children;
		const auto stale = std::remove(list.begin(), list.end(), id);
		if (stale != list.end())
		{
			list.erase(stale, list.end());
			++g_mirror_stale;
			if (mirror_log_allowed())
				::Msg("! [coop] mirror: holder %u [%s] still listed %u, a gone entity, before its EntitySpawn: taken out",
				    parent_host, holder->name_replace(), id_host);
		}
	}
	alife_rw().register_object(e, true);
	g_id_retry.done(id_host);
	// B2: a child follows its holder HERE; the host's flag of a child is its holder's state there (xrCoop/coop_mirror.h).
	// F3a: on the world server a holder that a map server took is online by its flag alone (host_mark): that is no holder online here.
	// Build 51: without a valid game vertex register_object put it into no level registry (CALifeGraphRegistry::add), and
	// update_switch walks that registry only: a phantom of level.spawn_item never came online here. The host's flag speaks
	// of this level only on a mirror that stands on the level of player 0, the co-op host (not on the world server, which plays
	// nobody and is on no map of LevelMap, not on a map server), or when the byte says so.
	// Build 57, S3 (TZ 6.3, the psy phantoms of a bloodsucker unseen on a map server's level): a player on a level that a map
	// server leads got this message twice and could not tell the two apart: the map server's own (up the trunk in a Tunnel, its
	// flag about his level) and the world server's copy (its flag offline there), both from kHostPeer in no fixed order, so a
	// free entity without a game vertex stayed on the vote, unseen. Now the world server passes the map server's on marked
	// (online_word 2, coop_trunk.cpp) and sends that level's players no copy of its own (host_spawn_from_leader).
	const bool in_level_registry = ai().game_graph().valid_vertex_id(e->m_tGraphID);
	const bool flag_of_this_level = is_mirror() &&
	    (online_word == levels::wire::kSpawnOnlineAtYourLevel || mode().where(kHostPeer) == Where::Here);
	// Builds 52-54 (issue 1293): OnlineNow ran for squads too. A squad of the host has no game vertex here as a rule,
	// it comes online with its members (register_member, apply_group_switch), and a wire squad's members are ids
	// with no object yet: its own switch_online either met R_ASSERT(!m_bOnline) or walked a null member. Only an
	// entity that switches by itself and that register_object left offline comes online here (apply_switch's test).
	const bool switches_alone = !smart_cast<CSE_ALifeOnlineOfflineGroup*>(e) && !e->m_bOnline;
	switch (mirror::placement(parent != kNoId, online, holder && online_here(holder),
	    holder && alife_rw().server().ID_to_entity(parent), in_level_registry, flag_of_this_level, switches_alone))
	{
	case mirror::Placement::SwitchVote:
		g_online.insert(id);
		if (!in_level_registry && is_mirror() && !flag_of_this_level && log_allowed(g_mirror_log_info))
			::Msg("[coop] mirror: %u [%s] online on the host, no game vertex, this level is not the host process's: left "
			      "on the vote (no switch manager visits it; it may stay offline here)",
			    id_host, section);
		break;
	case mirror::Placement::OnlineNow:
		// as apply_switch does for an EntitySwitch online; the vote stays Online, and no switch manager visits it
		g_online.insert(id);
		++g_mirror_online_now;
		if (log_allowed(g_mirror_log_info))
			::Msg("[coop] mirror: %u [%s] comes online now: online on the host, no game vertex (no level registry here)",
			    id_host, section);
		alife_rw().switch_online(e);
		break;
	case mirror::Placement::WithHolder:
		if (!online)
		{
			++g_mirror_with_holder;
			if (log_allowed(g_mirror_log_info))
				::Msg("[coop] mirror: %u [%s] comes online in its holder %u [%s] here (offline on the host, where its "
				      "holder is offline)",
				    id_host, section, parent_host, holder->name_replace());
		}
		bring_child_online(e);
		break;
	case mirror::Placement::HolderBroken:
		++g_mirror_broken_holder;
		if (mirror_log_allowed())
			::Msg("! [coop] mirror: %u [%s] stays offline: its holder %u [%s] is online here without a server entity",
			    id_host, section, parent_host, holder->name_replace());
		break;
	case mirror::Placement::Offline:
		break;
	}
	if (!initial.empty())
	{
		const bool activated = requests::clone_activate(id);
		requests::clone_finish(id, activated);
		if (!activated)
		{
			client_remove(id);
			return;
		}
	}
	if (mode().level())
		squads_made_whole(e); // only a process that leads a map apart from its world writes a smart (squad_complete_here)
	++g_client_spawns;
}

void client_on_destroy(PeerId, Reader& r)
{
	const u16 id = join::to_local(r.u16());
	if (!r.ok() || id == 0)
		return;
	events::client_forget(id);
	if (ai().get_alife())
		client_remove(id);
}

void client_on_switch(PeerId, Reader& r)
{
	const u16 id = join::to_local(r.u16());
	const bool online = r.boolean();
	Fvector pos, ang;
	u32 node;
	u16 graph_id;
	read_placement(r, pos, ang, node, graph_id);
	NET_Packet update;
	if (!read_packet(r, update) || id == 0 || !ai().get_alife())
		return;
	CSE_ALifeDynamicObject* e = ai().alife().objects().object(id, true);
	if (!e)
	{
		++g_missing; // the host will not resend: ask for it (P3: find first, then create)
		Writer w(4);
		w.u16(join::to_host(id));
		net::client_send(Msg::StateDumpRequest, w);
		return;
	}
	ClientApply scope;
	if (online)
		g_online.insert(id);
	else
	{
		g_online.erase(id);
		g_tracks.erase(id);
	}
	++g_client_switches;
	if (e->ID_Parent != kNoId)
		return; // held by something here: it goes online/offline with its holder only
	leave_group(e);
	if (CSE_ALifeOnlineOfflineGroup* g = smart_cast<CSE_ALifeOnlineOfflineGroup*>(e))
		apply_group_switch(*g, online, pos, ang, node, graph_id, update);
	else
		apply_switch(e, online, pos, ang, node, graph_id, &update);
}

// The world's word on one entity to one copy (EntitySpawn: an answer, a repair). False: it could not be written.
bool host_send_spawn_to(PeerId to, CSE_ALifeDynamicObject* e)
{
	NET_Packet spawn, update;
	e->Spawn_Write(spawn, FALSE);
	update.w_begin(M_UPDATE);
	e->UPDATE_Write(update);
	Writer w(spawn.B.count + update.B.count + 32);
	if (!write_spawn(w, e, spawn, update))
		return false;
	net::host_send(to, Msg::EntitySpawn, w);
	return true;
}

// R32 W5 (design R32-final.md §1 W5.1): what is below an object goes with its spawn: a squad's members and a holder's children, each
// with what is below it, a holder before what it holds (client_on_spawn needs the holder first). A copy that misses a holder misses
// what it holds, and a squad's members came after it; what the copy has already is a repeat there (P6). As deep as root_of walks.
constexpr u32 kSubtreeDepth = 8;

void send_below(PeerId to, const CSE_ALifeDynamicObject* e, u32 depth)
{
	if (depth >= kSubtreeDepth)
		return;
	xr_vector<u16> below;
	below.assign(e->children.begin(), e->children.end());
	if (const CSE_ALifeOnlineOfflineGroup* g = smart_cast<const CSE_ALifeOnlineOfflineGroup*>(e))
		for (const auto& m : g->squad_members())
			below.push_back(m.first);
	for (const u16 id : below)
	{
		CSE_ALifeDynamicObject* c = id ? ai().alife().objects().object(id, true) : nullptr;
		if (!c || world::is_player_character(c) || !host_send_spawn_to(to, c))
			continue;
		send_below(to, c, depth + 1);
	}
}

} // namespace

// `root` and everything below it to one copy. False: the root's own spawn could not be written (nothing went).
bool send_subtree(PeerId to, CSE_ALifeDynamicObject* root)
{
	if (!root || !host_send_spawn_to(to, root))
		return false;
	send_below(to, root, 0);
	return true;
}

// ---------------------------------------------------------------- API

void install_server(Session& s)
{
	s.on(Msg::StateDumpRequest, [](PeerId from, Reader& r) {
		if (mode().decides_world() || mode().leads_level())
			host_on_entity_request(from, r);
	});
	s.on(Msg::EntityResync, &host_on_resync);
}

void install_client(Endpoint& e)
{
	// Live only: before that the client is still in its own world (or the menu). The host queues
	// world changes for a loading client and flushes them after WorldLoaded (P6); what arrives
	// earlier predates the snapshot and is already inside it.
	e.on(Msg::EntitySpawn, [](PeerId from, Reader& r) { if (mgr().is_live_client()) client_on_spawn(from, r); });
	e.on(Msg::EntityDestroy, [](PeerId from, Reader& r) { if (mgr().is_live_client()) client_on_destroy(from, r); });
	e.on(Msg::EntitySwitch, [](PeerId from, Reader& r) { if (mgr().is_live_client()) client_on_switch(from, r); });
	e.on(Msg::EntitySnapshot, [](PeerId from, Reader& r) { if (mgr().is_live_client()) client_on_snapshot(from, r); });
	e.on(Msg::EntityHash, [](PeerId from, Reader& r) { if (mgr().is_live_client()) client_on_hash(from, r); });
	e.on(Msg::EntityRoster, [](PeerId from, Reader& r) { if (mgr().is_live_client()) client_on_roster(from, r); });
	e.on(Msg::EntityTraits, [](PeerId from, Reader& r) { if (mgr().is_live_client()) client_on_traits(from, r); });
	e.on(Msg::EntitySquad, [](PeerId from, Reader& r) { if (mgr().is_live_client()) client_on_squad(from, r); });
}

void reset()
{
	g_host_events.clear();
	g_online.clear();
	g_refetched.clear();
	reset_snapshots();
	reset_roster();
	reset_pose_diag();
}

// F3a T8 (live 2026-10-24: "the client sees nobody, dogs killed him unseen"): a mover of a simultaneous load got the
// prepared save with the host's actor as its only online object (the host's new world was not loaded yet), and the
// objects the host's world brought online while it loaded never reached him as switches. Once he is in, he gets the
// host's whole online set, as a joiner gets it in his world save (client_set_initial_online). A repeat changes nothing.
void host_send_online_to(PeerId to)
{
	if (!ai().get_alife())
		return;
	u32 sent = 0;
	for (const auto& it : ai().alife().objects().objects())
	{
		CSE_ALifeDynamicObject* e = it.second;
		if (!e || !online_here(e) || e->ID_Parent != kNoId || e->ID == 0)
			continue; // the host's actor is the client's proxy of him: no switch ever (host_on_switch skips 0)
		Writer w(256);
		write_switch(w, e, true);
		net::host_send(to, Msg::EntitySwitch, w);
		++sent;
	}
	g_host_switches += sent;
	::Msg("[coop] online set of the new world -> #%u: %u object(s)", to, sent);
}

bool write_entity(Writer& w, CSE_ALifeDynamicObject* e)
{
	NET_Packet spawn, update;
	e->Spawn_Write(spawn, FALSE);
	update.w_begin(M_UPDATE);
	e->UPDATE_Write(update);
	// the host's numbering on the wire: the only ids that differ here are his actor and his proxy, and those hold what he makes
	w.u16(join::to_host(e->ID));
	w.u16(e->ID_Parent == kNoId ? kNoId : join::to_host(e->ID_Parent));
	w.boolean(false);
	write_placement(w, e);
	write_packet(w, spawn);
	write_packet(w, update);
	return requests::write_clone_initial(w, e) && items::write_initial(w, e); // R32 W8.4: the layout of write_spawn
}

void host_spawn_from_leader(Reader& r)
{
	Reader look(r.cursor(), r.remaining());
	const u16 id = join::to_local(look.u16());
	const size_t before = g_host_events.size();
	{
		ClientApply scope;
		client_on_spawn(kHostPeer, r);
	}
	// Build 57, S3 (TZ 6.3, the psy phantoms): the players of the level of that map server have its own spawn (up the trunk, marked
	// online at their level: coop_trunk.cpp). This world's copy goes out at the frame, after the EchoOf of the report is over: it
	// keeps it, so they do not get the copy too (it said offline here, and coming first it left a phantom offline for good).
	const u16 echo = levels::echo_now();
	for (size_t i = before; i < g_host_events.size(); ++i)
		if (g_host_events[i].kind == HostEventKind::Registered && g_host_events[i].id == id)
			g_host_events[i].echo = echo;
}

void host_send_place(PeerId to, CSE_ALifeDynamicObject* e)
{
	Writer w(256);
	write_switch(w, e, false);
	net::host_send(to, Msg::EntitySwitch, w);
	++g_host_switches;
}

void place_offline(CSE_ALifeDynamicObject* e, const Fvector& pos, const Fvector& ang, u32 node, u16 graph) { place(e, pos, ang, node, graph); }

void host_collect_online(xr_vector<u16>& out)
{
	out.clear();
	if (!ai().get_alife())
		return;
	// The host actor (0) is included: on the client it is our proxy, the host's avatar, and it
	// gets no EntitySwitch ever (host_on_switch skips 0), so without this it stays offline there.
	for (const auto& it : ai().alife().objects().objects())
	{
		const CSE_ALifeDynamicObject* e = it.second;
		if (e && online_here(e) && e->ID_Parent == kNoId)
			out.push_back(e->ID);
	}
}

bool host_records()
{
	return mgr().active() && (mode().decides_world() || (mode().leads_level() && !g_applying));
}

bool host_own_spawn_unsent(u16 id)
{
	for (const HostEvent& ev : g_host_events)
		if (ev.kind == HostEventKind::Registered && ev.id == id && !ev.applied)
			return true;
	return false;
}

void host_on_entity_registered(CSE_ALifeDynamicObject* e)
{
	if (host_records() && e)
	{
		HostEvent ev{HostEventKind::Registered, e->ID, false};
		ev.applied = g_applying > 0; // E-2: a map server's spawn the world takes (host_spawn_from_leader) is no create of its own
		g_host_events.push_back(ev);
	}
	if (e)
	{
		if (LevelSide* side = mode().level())
			side->level_entity_up(EntityUp::Spawned, e->ID, kNoId);
	}
}

void host_on_entity_released(u16 id)
{
	script::note_released(id); // F4 G3-1c: a weapon rider that came before this release never reaches a same-name replacement
	items::forget_item(id);
	syncapi::on_entity_destroyed(id); // F2 T14: its coop.sync scope goes (host, single player; any game thread)
	if (host_records())
		g_host_events.push_back({HostEventKind::Released, id, false});
	if (LevelSide* side = mode().level())
		side->level_entity_up(EntityUp::Released, id, kNoId);
}

void host_on_switch(CSE_ALifeDynamicObject* e, bool online)
{
	if (host_records() && e && e->ID != 0)
		g_host_events.push_back({HostEventKind::Switched, e->ID, online});
}

void client_set_initial_online(const xr_vector<u16>& host_ids)
{
	g_online.clear();
	for (u16 id : host_ids)
		g_online.insert(join::to_local(id));
	g_online.erase(0);
	::Msg("[coop] host has %u objects online", (u32)g_online.size());
}

bool client_wants_online(u16 id) { return g_online.count(id) != 0; }

void client_on_holder_changed(u16 id, u16 new_parent, bool host_online)
{
	if (id == 0 || id == kNoId)
		return;
	if (new_parent == kNoId && host_online)
		g_online.insert(id);
	else
	{
		g_online.erase(id); // in a holder it follows the holder; a free offline item stays offline
		g_tracks.erase(id);
	}
}

bool mirror_lost_child(CSE_Abstract* holder, u16 child)
{
	if (!holder)
		return false;
	CSE_ALifeDynamicObject* c = ai().get_alife() ? ai().alife().objects().object(child, true) : nullptr;
	CSE_ALifeDynamicObject* h = ai().get_alife() ? ai().alife().objects().object(holder->ID, true) : nullptr;
	CSE_ALifeInventoryItem* item = smart_cast<CSE_ALifeInventoryItem*>(c);
	const bool detachable =
	    item && h == holder && ai().get_game_graph() && ai().game_graph().valid_vertex_id(h->m_tGraphID);
	const mirror::LostChild what = mirror::lost_child(join::world_is_mirror(), c != nullptr, c && c->m_bOnline,
	    c && c->ID_Parent == holder->ID, detachable);
	if (what == mirror::LostChild::Fatal)
		return false; // single player, the host: the stock assert
	++g_mirror_lost;
	if (mirror_log_allowed())
	{
		string256 known;
		if (c)
			xr_sprintf(known, "[%s] in ALife, %s, held by %u", c->s_name.c_str(), c->m_bOnline ? "online" : "offline",
			    c->ID_Parent);
		else
			xr_strcpy(known, "neither in ALife");
		::Msg("! [coop] mirror: holder %u [%s][%s] lists child %u with no server entity here (%s): %s (local ids, 0 = "
		      "our actor)",
		    holder->ID, holder->name_replace(), holder->s_name.c_str(), child, known,
		    what == mirror::LostChild::Detach ? "taken out of it in ALife, it lies free and offline here"
		                                      : "dropped from its list");
	}
	if (what == mirror::LostChild::Detach)
		alife_rw().graph().detach(*h, item, h->m_tGraphID, true, true); // takes the id out of h->children
	else
	{
		xr_vector<u16>& list = holder->children;
		const auto it = std::find(list.rbegin(), list.rend(), child); // the last one: Perform_destroy reads back()
		if (it != list.rend())
			list.erase(std::next(it).base());
	}
	return true;
}

void mirror_on_destroy(xrServer& server, CSE_Abstract* entity)
{
	if (!entity || entity->ID_Parent == kNoId || !join::world_is_mirror() || server.ID_to_entity(entity->ID) != entity)
		return; // not on the server: add_online / bring_child_online free the id of an offline entity before its spawn
	CSE_Abstract* holder = ai().get_alife() ? ai().alife().objects().object(entity->ID_Parent, true) : nullptr;
	if (!holder)
		holder = server.ID_to_entity(entity->ID_Parent);
	if (!holder || std::find(holder->children.begin(), holder->children.end(), entity->ID) == holder->children.end())
		return;
	++g_mirror_listed_destroys;
	if (mirror_log_allowed())
		::Msg("! [coop] mirror: %u [%s] leaves the server while its holder %u [%s] still lists it (frame %u, local ids)",
		    entity->ID, entity->s_name.c_str(), entity->ID_Parent, holder->name_replace(), Device.dwFrame);
}

bool is_mirrored(u16 id) { return is_mirror() && id != 0; }

bool client_applying() { return g_applying > 0; }

ClientApply::ClientApply() { ++g_applying; }
ClientApply::~ClientApply() { --g_applying; }

void frame()
{
	PROF_EVENT("coop/coop_entities/frame");
	if (mode().decides_world() || mode().leads_level())
	{
		// Level-change review: what the engine recorded while the world was not up belongs to a world that is gone, or is
		// being built (every client rejoins from the new world's save): an id of the old one names another object in the
		// new one. R2 C2 did this when the simulator was null; a level in the making (g_pGameLevel set, simulator half
		// built, no actor) was not covered.
		if (!world_up())
		{
			g_host_events.clear();
			return;
		}
		host_process_events();
		host_send_snapshots();
		watch_decider(mgr().now_ms()); // P101/P103: a round over this decider's creatures every 10 s (bounded lines)
	}
}

void print_status()
{
	print_snapshot_status();
	if (mode().decides_world() || mode().leads_level())
	{
		::Msg("[coop]   world out: %llu spawns, %llu releases, %llu switches, %llu snapshot packets", g_host_spawns,
		    g_host_releases, g_host_switches, g_snapshots_sent);
		print_decider_poses();
		print_roster_host();
	}
	else
	{
		print_roster_client();
		::Msg("[coop]   world in: %u online, %u tracked, %llu spawns, %llu releases, %llu switches, %llu missing, "
		    "%llu spawns replaced another object under their id (R32 W3)",
		    (u32)g_online.size(), (u32)g_tracks.size(), g_client_spawns, g_client_releases, g_client_switches,
		    g_missing, g_replaced);
		u32 wire_squads = 0, wire_ids = 0;
		if (ai().get_alife())
			for (const auto& it : ai().alife().groups().objects())
			{
				u32 n = 0;
				for (const auto& m : it.second->squad_members())
					if (!m.second)
						++n;
				wire_squads += n ? 1 : 0;
				wire_ids += n;
			}
		::Msg("[coop]   squads from the wire: %u now (%u unbound member ids), %llu ids forgotten", wire_squads, wire_ids,
		    g_unbound_dropped);
		::Msg("[coop]   mirror holders (B2): %llu children online with their holder (offline on the host), %llu left "
		      "offline in a holder without a server entity, %llu stale ids taken out, %llu lost children met, %llu left "
		      "the server still listed",
		    g_mirror_with_holder, g_mirror_broken_holder, g_mirror_stale, g_mirror_lost, g_mirror_listed_destroys);
		::Msg("[coop]   mirror without a game vertex: %llu brought online at their EntitySpawn (psy phantoms)",
		    g_mirror_online_now);
		print_copy_poses();
	}
}
} // namespace entities
} // namespace coop
