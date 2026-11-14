#include "stdafx.h"
#include "pch_script.h"
// F3a split levels: "online at a map server" on the host's ALife, and the state a transfer carries (docs/SPLIT_LEVELS.md §3).
//
// Host. A creature or a squad a map server took is marked online without a game object (recon §2.1, variant A): m_bOnline, out of the
// offline scheduler and out of the graph registry, the same as the registry half of switch_online. Everything in the stock that
// understands "online" leaves it alone: the offline scheduler, the offline combat of sim_offline_combat, .online in Lua.
// Only coop_levels_xfer.cpp calls host_mark and host_unmark, from what the ledger says (xrCoop/coop_levels_keeper.h).
//
// The state of an object: its placement, its money and its M_UPDATE, then the same of a squad's members. Spawn data does not travel: every
// machine has the object from the world's save or its EntitySpawn. What Lua keeps of it goes with it when it changes hands
// (state_with_logic, coop_levels_brain.script): the logic of an NPC, a squad's smart and target. A smart's own Lua state stays on each
// side: only the map server runs its jobs, only the world server its respawn.
#include "coop_levels.h"
#include "coop_entities.h"
#include "coop_join.h"
#include "coop_manager.h"
#include "coop_mapserver.h"
#include "coop_alife.h"
#include "coop_world.h"
#include "xrCoop/coop_levels_keeper.h"

#include "ai_space.h"
#include "game_graph.h"
#include "alife_simulator.h"
#include "alife_object_registry.h"
#include "alife_graph_registry.h"
#include "alife_schedule_registry.h"
#include "alife_group_registry.h"
#include "alife_monster_brain.h"
#include "alife_monster_detail_path_manager.h"
#include "alife_monster_movement_manager.h"
#include "alife_online_offline_group_brain.h"
#include "xrServer.h"
#include "xrServer_Objects_ALife_Monsters.h"
#include "xrServer_Objects_ALife_Items.h"
#include "xrMessages.h"
#include "script_engine.h"

namespace coop
{
namespace levels
{
namespace
{
constexpr size_t kMaxLogicBytes = 8 * 1024; // of one object; more would not fit a cargo with its squad, and it stays with the old side
std::string g_logic; // what Lua hands the engine, and the other way (coop_levels_brain.script)

void read_logic(Reader& r);

struct Placed
{
	Fvector pos;
	Fvector ang;
	u32 node = 0;
	u16 graph = 0;
	u32 money = 0;
};

// A trader's money is in no M_UPDATE (CSE_ALifeTraderAbstract::UPDATE_Write is empty), the stock keeps it for the save.
u32 money_of(CSE_ALifeDynamicObject* e)
{
	const CSE_ALifeTraderAbstract* t = smart_cast<CSE_ALifeTraderAbstract*>(e);
	return t ? t->m_dwMoney : 0;
}

// A player's money is the world server's book (coop_money.cpp): no state of his body sets it.
void set_money(CSE_ALifeDynamicObject* e, u32 money)
{
	CSE_ALifeTraderAbstract* t = smart_cast<CSE_ALifeTraderAbstract*>(e);
	if (t && !world::is_player_character(e))
		t->m_dwMoney = money;
}

void write_one(Writer& w, CSE_ALifeDynamicObject* e)
{
	float pos[3] = {e->o_Position.x, e->o_Position.y, e->o_Position.z};
	float ang[3] = {e->o_Angle.x, e->o_Angle.y, e->o_Angle.z};
	w.vec3(pos);
	w.vec3(ang);
	w.u32(e->m_tNodeID);
	w.u16(e->m_tGraphID);
	w.u32(money_of(e));
	NET_Packet update;
	update.w_begin(M_UPDATE);
	e->UPDATE_Write(update);
	w.u16(u16(update.B.count));
	w.bytes(update.B.data, update.B.count);
}

bool read_one(Reader& r, Placed& out, NET_Packet& update)
{
	float pos[3], ang[3];
	r.vec3(pos);
	r.vec3(ang);
	out.pos.set(pos[0], pos[1], pos[2]);
	out.ang.set(ang[0], ang[1], ang[2]);
	out.node = r.u32();
	out.graph = r.u16();
	out.money = r.u32();
	const u16 n = r.u16();
	if (!r.ok() || n > sizeof(update.B.data))
		return false;
	update.B.count = n;
	update.r_pos = 0;
	return r.bytes(update.B.data, n);
}

void read_update(CSE_ALifeDynamicObject* e, NET_Packet& update)
{
	update.r_pos = 0;
	u16 type;
	update.r_begin(type);
	if (type == M_UPDATE)
		e->UPDATE_Read(update);
}

// The time it spent on the other side is no time it walked: the next offline update of this side would take it all in one step.
void clock_here(CSE_ALifeDynamicObject* e)
{
	if (CSE_ALifeOnlineOfflineGroup* g = smart_cast<CSE_ALifeOnlineOfflineGroup*>(e))
		g->brain().movement().detail().skip_time();
	else if (CSE_ALifeMonsterAbstract* m = smart_cast<CSE_ALifeMonsterAbstract*>(e))
		m->brain().movement().detail().skip_time();
}

// An object out of every registry (marked, or a squad's member): its fields only.
void set_fields(CSE_ALifeDynamicObject* e, const Placed& at, NET_Packet& update)
{
	e->o_Position = at.pos;
	e->o_Angle = at.ang;
	e->m_tNodeID = at.node;
	e->m_tGraphID = at.graph;
	set_money(e, at.money);
	read_update(e, update);
}

CSE_ALifeDynamicObject* object_of(u16 id)
{
	return (world_up() && ai().get_alife()) ? ai().alife().objects().object(id, true) : nullptr;
}

// A death on a map server's level, for the tasks of the world: the world server's numbering, killer 0xffff when there is none. The script
// may not be there, and functor() alone would complain of the file: a game without it hears nothing of this.
void tell_tasks_of_death(u16 victim, u16 killer)
{
	static const char* const script = "coop_quests_far";
	CScriptEngine& lua = ai().script_engine();
	lua.process_file_if_exists(script, false);
	luabind::functor<void> tell;
	if (lua.namespace_loaded(script) && lua.functor("coop_quests_far.on_death", tell))
		tell(victim, killer);
}

// The members a state lists, in the order write_one put them: `fn(member, placed, update)` for those this machine has.
template <typename Fn>
bool for_members(Reader& r, Fn fn)
{
	const u32 n = r.u8();
	for (u32 i = 0; i < n; ++i)
	{
		const u16 id = join::to_local(r.u16());
		Placed at;
		NET_Packet update;
		if (!read_one(r, at, update))
			return false;
		if (CSE_ALifeDynamicObject* m = object_of(id))
			fn(m, at, update);
	}
	return r.ok();
}

// A squad from the wire knows its members by id alone (entities: group_bound). The stock binding of a loaded squad does it, once
// every member is here as a free offline object.
bool bind_squad(CSE_ALifeOnlineOfflineGroup& g)
{
	bool unbound = false;
	for (const auto& m : g.squad_members())
	{
		if (m.second)
			continue;
		unbound = true;
		const CSE_ALifeDynamicObject* o = ai().alife().objects().object(m.first, true);
		if (!o || o->m_bOnline || o->ID_Parent != 0xffff || !smart_cast<const CSE_ALifeMonsterAbstract*>(o))
			return false;
	}
	if (unbound)
		g.on_after_game_load();
	return true;
}
} // namespace

u16 unit_of(const CSE_ALifeDynamicObject* e)
{
	const CSE_ALifeMonsterAbstract* m = smart_cast<const CSE_ALifeMonsterAbstract*>(e);
	if (!m || m->m_group_id != 0xffff)
		return e->ID;
	for (const auto& it : ai().alife().groups().objects())
	{
		const auto found = it.second->squad_members().find(e->ID);
		if (found != it.second->squad_members().end() && !found->second)
			return it.second->ID;
	}
	return e->ID;
}

bool transfers(const CSE_ALifeDynamicObject* e)
{
	if (smart_cast<const CSE_ALifeOnlineOfflineGroup*>(e))
		return true;
	// a corpse, a car, a crow: nothing of the host's offline simulation runs on them
	const CSE_ALifeMonsterAbstract* m = smart_cast<const CSE_ALifeMonsterAbstract*>(e);
	return m && const_cast<CSE_ALifeMonsterAbstract*>(m)->g_Alive();
}

std::vector<u8> state_of(CSE_ALifeDynamicObject* e)
{
	Writer w(256);
	write_one(w, e);
	u8 n = 0;
	CSE_ALifeOnlineOfflineGroup* g = smart_cast<CSE_ALifeOnlineOfflineGroup*>(e);
	if (g)
		for (const auto& m : g->squad_members())
			n = m.second && n < 0xff ? u8(n + 1) : n;
	w.u8(n);
	if (g && n)
	{
		u32 left = n;
		for (const auto& m : g->squad_members())
		{
			if (!m.second || !left)
				continue;
			--left;
			w.u16(join::to_host(m.first));
			write_one(w, m.second);
		}
	}
	return std::vector<u8>(w.data().begin(), w.data().end());
}

bool host_may_give(const CSE_ALifeDynamicObject* e, LevelId level)
{
	if (!e || e->ID_Parent != 0xffff || e->m_bOnline || level == wire::kNoLevel || world::is_player_character(e))
		return false;
	const CSE_ALifeMonsterAbstract* m = smart_cast<const CSE_ALifeMonsterAbstract*>(e);
	if (m && m->m_group_id != 0xffff)
		return false; // a member goes with its squad
	return level_of_vertex(e->m_tGraphID) == level;
}

namespace
{
void mark_object(CSE_ALifeDynamicObject* e)
{
	if (!e || e->m_bOnline)
		return;
	const u16 id = e->ID;
	CALifeSimulator& a = alife_rw();
	if (CSE_ALifeOnlineOfflineGroup* g = smart_cast<CSE_ALifeOnlineOfflineGroup*>(e))
		for (const auto& m : g->squad_members())
			if (m.second)
				m.second->m_bOnline = true; // members have no registries of their own, as in the stock switch_online
	e->m_bOnline = true;
	if (a.scheduled().object(id, true))
		a.scheduled().remove(e);
	// a squad that came as a packet may stand in no graph point
	const GameGraph::_GRAPH_ID vertex = e->m_tGraphID;
	if (!ai().game_graph().valid_vertex_id(vertex))
		return;
	const auto& here = a.graph().objects()[vertex].objects().objects();
	if (here.find(id) != here.end())
		a.graph().remove(e, vertex, false);
}
} // namespace

void host_mark(u16 id) { mark_object(object_of(id)); }

// A squad of a map server comes before its members, one packet each, and the world server's simulation runs it from the first frame: it is
// bound as they come, whether it ever comes home or not (a squad with no game vertex never does).
void host_bind_squads(u16 id)
{
	static xr_vector<u16> waiting;
	if (smart_cast<CSE_ALifeOnlineOfflineGroup*>(object_of(id)))
		waiting.push_back(id);
	for (size_t i = 0; i < waiting.size();)
	{
		CSE_ALifeOnlineOfflineGroup* g = smart_cast<CSE_ALifeOnlineOfflineGroup*>(object_of(waiting[i]));
		if (!g || g->coop_bind_members(false))
			waiting.erase(waiting.begin() + i);
		else
			++i;
	}
}

void host_mark_in_new_world(u16 id) { mark_object(ai().alife().objects().object(id, true)); }

void host_unmark(u16 id, const std::vector<u8>& state)
{
	CSE_ALifeDynamicObject* e = object_of(id);
	if (!e || !e->m_bOnline)
		return;
	CALifeSimulator& a = alife_rw();
	CSE_ALifeOnlineOfflineGroup* g = smart_cast<CSE_ALifeOnlineOfflineGroup*>(e);
	if (g)
	{
		g->coop_bind_members(true);
		for (const auto& m : g->squad_members())
			if (m.second)
				m.second->m_bOnline = false;
	}
	e->m_bOnline = false;
	if (!state.empty())
		host_apply_state(id, state);
	clock_here(e);
	if (!a.scheduled().object(id, true))
		a.scheduled().add(e);
	if (ai().game_graph().valid_vertex_id(e->m_tGraphID))
		a.graph().add(e, e->m_tGraphID, false);
}

void host_apply_state(u16 id, const std::vector<u8>& state)
{
	CSE_ALifeDynamicObject* e = object_of(id);
	if (!e)
		return;
	Reader r(state.data(), state.size());
	Placed at;
	NET_Packet update;
	if (!read_one(r, at, update) || !ai().game_graph().valid_vertex_id(at.graph))
		return;
	set_fields(e, at, update);
	// A squad's last word from a map server that fell may be older than a member's death here: it would give the body its life back.
	const auto alive_only = [](CSE_ALifeDynamicObject* m, const Placed& p, NET_Packet& u) {
		const CSE_ALifeCreatureAbstract* c = smart_cast<const CSE_ALifeCreatureAbstract*>(m);
		if (!c || c->g_Alive())
			set_fields(m, p, u);
	};
	if (for_members(r, alive_only))
		read_logic(r);
}

void host_gone(u16 id)
{
	CSE_ALifeDynamicObject* e = object_of(id);
	if (!e)
		return;
	if (e->m_bOnline)
		host_unmark(id, std::vector<u8>());
	alife_rw().release(e, true); // the usual way: every player's copy goes with EntityDestroy
}

void host_apply_parent(u16 item, u16 parent, const Fvector& pos)
{
	CSE_ALifeDynamicObject* d = object_of(item);
	CSE_ALifeInventoryItem* ii = smart_cast<CSE_ALifeInventoryItem*>(d);
	if (!d || !ii || d->ID_Parent == parent)
		return;
	CALifeGraphRegistry& graph = alife_rw().graph();
	if (d->ID_Parent != 0xffff)
	{
		CSE_ALifeDynamicObject* holder = object_of(d->ID_Parent);
		if (!holder || std::find(holder->children.begin(), holder->children.end(), item) == holder->children.end())
			return;
		graph.detach(*holder, ii, holder->m_tGraphID, true, true);
	}
	if (parent == 0xffff)
	{
		d->o_Position = pos;
		return;
	}
	if (CSE_ALifeDynamicObject* holder = object_of(parent))
		graph.attach(*holder, ii, d->m_tGraphID, true, true);
}

static u16 g_body_stays = 0xffff;

bool host_body_stays(u16 id) { return id != 0xffff && id == g_body_stays; }

void host_apply_death(u16 victim, u16 killer, const std::vector<u8>& state)
{
	CSE_ALifeCreatureAbstract* c = smart_cast<CSE_ALifeCreatureAbstract*>(object_of(victim));
	if (!c || !c->g_Alive())
		return;
	// Online on its map server it leaves a body there. The stock remove_npc releases a member it sees offline, as it must for one
	// that dies in the offline simulation: only the host's own look at it is offline below.
	const bool body_there = c->m_bOnline;
	Reader r(state.data(), state.size());
	Placed at;
	NET_Packet update;
	if (read_one(r, at, update) && ai().game_graph().valid_vertex_id(at.graph))
		set_fields(c, at, update);
	if (c->get_health() > 0.f)
		c->set_health(0.f);
	const CSE_ALifeMonsterAbstract* m = smart_cast<const CSE_ALifeMonsterAbstract*>(c);
	if (m && m->m_group_id != 0xffff)
		c->m_bOnline = false; // it leaves its squad in on_death, into the host's registries: the body is nobody's to simulate
	g_body_stays = body_there ? victim : 0xffff;
	alife_rw().on_death(c, object_of(killer));
	g_body_stays = 0xffff;
	tell_tasks_of_death(victim, killer);
}

void host_apply_release(u16 id, LevelId level)
{
	CSE_ALifeDynamicObject* e = object_of(id);
	if (!e || e->m_bOnline || id == 0 || world::is_player_character(e))
		return;
	// A carried item keeps the vertex it was taken on, whatever level its holder stands on: the root says where it is.
	const CSE_ALifeDynamicObject* root = e;
	for (int depth = 0; depth < 8 && root && root->ID_Parent != 0xffff; ++depth)
		root = object_of(root->ID_Parent);
	if (root && level_of_vertex(root->m_tGraphID) == level)
		alife_rw().release(e, true);
}

void host_reserve_leader_ids(bool say_so)
{
	settle_machine_settings(); // the ceiling counted below may still be an earlier build's default
	xrServer& server = alife_rw().server();
	const u32 slots = leader_slots_kept(u32(_max(0, g_coop_max_map_servers)), mapserver::host_highest_number());
	u32 reserved = 0;
	for (u32 id = kLeaderIdFirst; id < u32(kLeaderIdFirst) + slots * kLeaderIdsEach; ++id)
		if (server.IsFreeID(u16(id)))
		{
			server.PerformIDgen(u16(id));
			++reserved;
		}
	if (say_so)
		::Msg("[coop] levels: %u ids kept for what the map servers make on their levels", reserved);
}

void logic_put(const char* bytes, size_t size) { g_logic.assign(bytes, size); }

const std::string& logic_bytes() { return g_logic; }

// A map server's or the world server's Lua, one object: u16 id (host numbering), u16 size, the bytes.
void write_logic(Writer& w, u16 id)
{
	g_logic.clear();
	luabind::functor<void> ask;
	if (ai().script_engine().functor("coop_levels_brain.logic_of", ask))
		ask(id);
	const size_t size = g_logic.size() <= kMaxLogicBytes ? g_logic.size() : 0;
	w.u16(join::to_host(id));
	w.u16(u16(size));
	if (size)
		w.bytes(reinterpret_cast<const u8*>(g_logic.data()), size);
}

namespace
{
// What write_logic wrote, read after the members of a state (a state of the stream has none).
void read_logic(Reader& r)
{
	const u32 n = r.remaining() ? u32(r.u8()) : 0;
	luabind::functor<void> take;
	const bool lua = n && ai().script_engine().functor("coop_levels_brain.logic_in", take);
	for (u32 i = 0; i < n; ++i)
	{
		const u16 id = join::to_local(r.u16());
		const u16 size = r.u16();
		if (!r.ok() || size > r.remaining())
			return;
		g_logic.resize(size);
		if (size && !r.bytes(reinterpret_cast<u8*>(&g_logic[0]), size))
			return;
		if (size && lua)
			take(id);
	}
}
} // namespace

std::vector<u8> state_with_logic(CSE_ALifeDynamicObject* e)
{
	std::vector<u8> state = state_of(e);
	std::vector<u16> ids(1, e->ID);
	if (CSE_ALifeOnlineOfflineGroup* g = smart_cast<CSE_ALifeOnlineOfflineGroup*>(e))
		for (const auto& m : g->squad_members())
			if (ids.size() < 0xff)
				ids.push_back(m.first);
	Writer w(1024);
	w.u8(u8(ids.size()));
	for (const u16 id : ids)
		write_logic(w, id);
	state.insert(state.end(), w.data().begin(), w.data().end());
	return state;
}

bool client_apply_state(u16 local_id, const std::vector<u8>& state)
{
	CSE_ALifeDynamicObject* e = object_of(local_id);
	if (!e || e->m_bOnline)
		return false;
	Reader r(state.data(), state.size());
	Placed at;
	NET_Packet update;
	if (!read_one(r, at, update) || !ai().game_graph().valid_vertex_id(at.graph))
		return false;
	const entities::ClientApply apply;
	if (CSE_ALifeOnlineOfflineGroup* g = smart_cast<CSE_ALifeOnlineOfflineGroup*>(e))
		if (!bind_squad(*g))
			return false; // a member has not reached this machine yet: the squad goes back and is asked for later
	entities::place_offline(e, at.pos, at.ang, at.node, at.graph);
	set_money(e, at.money);
	read_update(e, update);
	if (!for_members(r, [](CSE_ALifeDynamicObject* m, const Placed& p, NET_Packet& u) { set_fields(m, p, u); }))
		return false;
	read_logic(r);
	clock_here(e);
	return true;
}

logic::Kind logic_kind(CSE_ALifeDynamicObject* e)
{
	logic::Kind k;
	k.keeps = e->keep_saved_data_anyway();
	k.item = smart_cast<CSE_ALifeInventoryItem*>(e) != nullptr;
	k.transfers = transfers(e);
	return k;
}

bool host_apply_logic(u16 id, const std::vector<u8>& bytes, LevelId level)
{
	CSE_ALifeDynamicObject* e = object_of(id);
	logic::Entity view;
	if (e)
	{
		view.exists = true;
		view.online = e->m_bOnline;
		view.kind = logic_kind(e);
		view.level = level_of_vertex(e->m_tGraphID);
	}
	const logic::Verdict verdict = logic::host_verdict(view, level);
	if (verdict != logic::Verdict::Take)
	{
		static u32 refused = 0;
		if (++refused <= 8 || refused % 100 == 0)
			::Msg("! [coop] levels: what object %u of level %u keeps is refused (%u so far): %s", u32(id), u32(level), refused,
			    verdict == logic::Verdict::NoObject ? "this world has no such object" :
			    verdict == logic::Verdict::Online   ? "it is online here" :
			    verdict == logic::Verdict::NotKept  ? "the next load would clear it" :
			                                          "it stands on another level");
		return false;
	}
	e->client_data.assign(bytes.begin(), bytes.end());
	static u32 kept = 0;
	if (++kept <= 16)
		::Msg("[coop] levels: '%s' of level %u keeps %u byte(s) of its logic in this world now", e->name_replace(), u32(level), u32(bytes.size()));
	return true;
}
} // namespace levels
} // namespace coop
