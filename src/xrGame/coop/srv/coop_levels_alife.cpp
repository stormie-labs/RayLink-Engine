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
// (state_with_logic, coop_srv_levels_brain.script): the logic of an NPC, a squad's smart and target. A smart's own Lua state stays on each
// side: only the map server runs its jobs, only the world server its respawn.
#include "../coop_levels.h"
#include "coop_levels_alife.h"
#include "../coop_checkpoint_sources.h"
#include "../coop_entities.h"
#include "../coop_events.h" // R32 W3: the relay of a Parent or Died the world applies (apply_entry)
#include "../coop_items.h"
#include "../coop_join.h"
#include "../coop_manager.h"
#include "../coop_money.h"
#include "../coop_survival.h"
#include "../coop_writer.h"
#include "coop_mapserver.h"
#include "../coop_alife.h"
#include "../coop_world.h"
#include "xrCoop/coop_mode.h"
#include "xrCoop/srv/coop_levels_accept.h"
#include "xrCoop/srv/coop_levels_journal.h" // O6: object_note, the uses refusal's branch
#include "xrCoop/srv/coop_levels_keeper.h"

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
#include "alife_registry_container.h"
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
std::string g_logic; // what Lua hands the engine, and the other way (coop_srv_levels_brain.script)

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
	static const char* const script = "coop_srv_quests_far";
	CScriptEngine& lua = ai().script_engine();
	lua.process_file_if_exists(script, false);
	luabind::functor<void> tell;
	if (lua.namespace_loaded(script) && lua.functor("coop_srv_quests_far.on_death", tell))
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
	if (ai().script_engine().functor("coop_srv_levels_brain.logic_of", ask))
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
	const bool lua = n && ai().script_engine().functor("coop_srv_levels_brain.logic_in", take);
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

// ---------------------------------------------------------------- R32 W2b / W3: the world's one apply of a map server's journal

namespace
{
struct AcceptStats
{
	u32 applied[journal::kLastOp + 1] = {};
	u32 refused[journal::kLastWhy + 1] = {};
	u32 repeats = 0;
};
AcceptStats g_accept;

ApplyVerdict applied(uint8_t op)
{
	if (op <= journal::kLastOp)
		++g_accept.applied[op];
	return ApplyVerdict::Applied;
}

// O6 (reports #1644-#1649): what a refusal line says of its object besides the id, so a refusal that needs data carries it: the section
// crc its entry's head named (0: none; the line names the section as this world has it when the object is here) and, for a move, where
// the entry puts it.
struct Seen
{
	uint16_t crc = 0;
	bool move = false;
	u16 to = wire::kNoObject;
};

// One log line for the first refusals of each reason, then every 100th. It names the object as this world sees it now (its section and
// parent, journal::object_note), or the section crc of its entry when this world has no such object. O6 (#1644/#1645): the item engine
// names both refusals of an ItemFacts by the uses store with one text; the world's object tells which branch it was.
ApplyVerdict refuse(PeerId author, uint8_t op, u16 id, uint8_t why_code, uint8_t& why, const char* detail = nullptr,
    const Seen& seen = Seen())
{
	why = why_code;
	const u32 n = why_code <= journal::kLastWhy ? ++g_accept.refused[why_code] : 0;
	if (n <= 8 || n % 100 == 0)
	{
		const CSE_ALifeDynamicObject* here = id != wire::kNoObject ? object_of(id) : nullptr;
		if (op == wire::LevelEntityHead::Item && journal::is_uses_refusal(detail))
			detail = journal::uses_refusal_text(journal::uses_refusal_of(here && !here->client_data.empty()));
		const std::string note = journal::object_note(here != nullptr, here ? here->s_name.c_str() : nullptr, seen.crc,
		    here ? here->ID_Parent : u16(wire::kNoObject), seen.move, seen.to);
		::Msg("! [coop] levels: %s of object %u from map server %u refused (%u so far): %s%s%s; %s", journal::op_name(op), u32(id),
		    mapserver::bridge_index(author), n, journal::why_name(why_code), detail ? ", " : "", detail ? detail : "", note.c_str());
	}
	return ApplyVerdict::Refused;
}

ApplyVerdict refused_by(PeerId author, uint8_t op, u16 id, const accept::Verdict& v, uint8_t& why, const Seen& seen = Seen())
{
	return refuse(author, op, id, v.why, why, nullptr, seen);
}

uint16_t crc_of(const CSE_ALifeDynamicObject* e) { return journal::section_crc(e->s_name.c_str()); }

// The world's view of an object's root: what it is, who writes it, whether it is the ghost.
struct RootSeen
{
	writer::RootFacts facts{writer::RootKind::None, writer::kNoId, writer::kNoLevel};
	writer::Writer writer = writer::nobody();
	bool ghost = false;
};

RootSeen root_seen(u16 id)
{
	RootSeen s;
	s.facts = writing::root_facts_of(id);
	s.writer = mode().writer_of(s.facts);
	s.ghost = id == writer::kGhostId || (s.facts.kind == writer::RootKind::Character && s.facts.root == writer::kGhostId);
	return s;
}

// The object here against a head (A6): its section crc, and the keeper generation of the unit at its root as `author` holds it.
bool same_as(const CSE_ALifeDynamicObject* e, const RootSeen& root, const journal::ObjectHead& h, PeerId author)
{
	u32 spawn_gen = 0;
	const u32 gen_here = root.facts.kind == writer::RootKind::Unit ? xfer_held_gen(root.facts.root, author, spawn_gen) : 0;
	return accept::identity(crc_of(e), h.crc, gen_here, h.gen, spawn_gen);
}

// The facts of an object the entry names, through its root.
void object_facts(accept::Facts& f, const CSE_ALifeDynamicObject* e, const journal::ObjectHead& h, PeerId author, RootSeen& root)
{
	f.exists = e != nullptr;
	if (!e)
		return;
	root = root_seen(e->ID);
	f.ghost = root.ghost;
	f.same = same_as(e, root, h, author);
	f.writer = root.writer;
	f.fence = host_fence_of(author, root.facts.root);
}

// An info portion of a non-actor object in the world's registry, as the stock alife():give_info / disable_info write it (without
// the gate of a client). An object that was never online in this world has no row yet: it gets one, as CInventoryOwner gives it.
void set_info(u16 id, const std::string& info, bool on)
{
	CInfoPortionRegistry& reg = alife_rw().registry().get<CInfoPortionRegistry>();
	KNOWN_INFO_VECTOR* known = reg.object(id, true);
	if (!known)
	{
		if (!on)
			return;
		KNOWN_INFO_VECTOR empty;
		reg.add(id, empty, true);
		known = reg.object(id, true);
		if (!known)
			return;
	}
	const shared_str name(info.c_str());
	const auto it = std::find(known->begin(), known->end(), name);
	if (on && it == known->end())
		known->push_back(name);
	else if (!on && it != known->end())
		known->erase(it);
}

// The world releases what a map server that writes it destroyed (W2b: whatever m_bOnline says, W3). A member of a squad that server
// holds is marked online elsewhere with its squad: it is taken off that mark first, and the stock release takes it out of its squad
// (on_unregister), into the graph at a vertex and out again. Without a vertex for it nothing is released (the graph would be read
// out of its range). The decision is accept::release_plan (fix A5: tests/test_levels_accept.cpp).
bool release_here(CSE_ALifeDynamicObject* e, const char*& why)
{
	CSE_ALifeMonsterAbstract* m = smart_cast<CSE_ALifeMonsterAbstract*>(e);
	CSE_ALifeOnlineOfflineGroup* squad =
	    m && m->m_group_id != 0xffff ? smart_cast<CSE_ALifeOnlineOfflineGroup*>(object_of(m->m_group_id)) : nullptr;
	accept::ReleaseFacts f;
	f.player = e->ID == writer::kGhostId || world::is_player_character(e);
	f.online = e->m_bOnline;
	f.member = squad != nullptr;
	f.own_vertex = ai().game_graph().valid_vertex_id(e->m_tGraphID);
	f.squad_vertex = squad && ai().game_graph().valid_vertex_id(squad->m_tGraphID);
	const accept::ReleasePlan plan = accept::release_plan(f);
	why = plan.why;
	if (!plan.release)
		return false;
	if (plan.take_squad_vertex)
		e->m_tGraphID = squad->m_tGraphID;
	if (plan.clear_online)
		e->m_bOnline = false;
	alife_rw().release(e, true);
	return true;
}

// A player's money from a map server: a deal of a player of its map (Review A-S6-E-econ2: a larger delta than a u32 balance takes
// is capped, so that money::adjust's s64(balance) + delta cannot overflow).
ApplyVerdict apply_money(PeerId author, LevelId level, const journal::Entry& e, uint8_t& why)
{
	constexpr s64 kMaxLevelMoneyDelta = s64(0xffffffffu);
	Reader r(e.body.data(), e.body.size());
	const u32 peer = r.u32();
	const s64 delta = s64(r.u64());
	const u8 trade = r.u8();
	if (!r.ok() || trade > 1)
		return refuse(author, e.op, wire::kNoObject, journal::Malformed, why);
	if (level_of_player(PeerId(peer)) != level)
		return refuse(author, e.op, wire::kNoObject, journal::Refused, why, "the player is not on its map");
	const s64 bounded = delta > kMaxLevelMoneyDelta ? kMaxLevelMoneyDelta : delta < -kMaxLevelMoneyDelta ? -kMaxLevelMoneyDelta : delta;
	money::host_adjust(PeerId(peer), bounded, trade ? "trade" : "map server");
	return applied(e.op);
}

// Who raised an entry the world applies (fix round 2 of package A, verifier finding 4): the map server's own journal
// (host_apply_fact), or the audit's heal of a member list in its name (host_apply_heal), which its Holder never adopted anybody at.
enum class Origin : uint8_t
{
	Journal,
	Heal,
};

// A member left a squad on the map server that writes the squad (its Left, accepted: the squad is a unit that map server holds). The
// plan is accept::leave_plan (tests/test_levels_accept.cpp accept_fixa_leave_plan):
//   R32 §8a A17: a living member stays with that map server, which holds it on its own from now on under the squad's gen (its Holder
//   adopted it at the same Left: Holder::adopt; here Ledger::adopt): online elsewhere, out of the squad, in no registry of this world,
//   so the world never simulates a creature that map server has, and it comes home once, by its hand-back or the loss of that map
//   server, like any held unit. One the keeper cannot give it (somebody else's, in a transfer of its own) takes today's way, a
//   creature of its own at home here (coop_leave_member); the audit heals that map server's side.
//   Fix round 2, finding 2: a member a newer squad bound already (that squad's Spawn listed it: the map server made the squad and moved
//   the member in one frame) leaves only this squad's list (coop_unlist_member): its binding, its m_bOnline and its registries are its
//   new squad's.
//   Fix round 2, finding 4: the audit's heal takes the member home (coop_leave_member): the map server has no hold of it.
// False: it was no member here.
bool leave_squad(PeerId author, CSE_ALifeOnlineOfflineGroup& squad, CSE_ALifeMonsterAbstract& member, Origin origin)
{
	const auto it = squad.squad_members().find(member.ID);
	accept::LeaveFacts f;
	f.listed = it != squad.squad_members().end();
	f.bound = f.listed && it->second != nullptr;
	f.bound_elsewhere = member.m_group_id != 0xffff && member.m_group_id != squad.ID;
	f.living = member.g_Alive() && member.ID_Parent == 0xffff;
	f.heal = origin == Origin::Heal;
	switch (accept::leave_plan(f))
	{
	case accept::Leave::NotMember: return false;
	case accept::Leave::Unlist: return squad.coop_unlist_member(member.ID);
	case accept::Leave::Adopt:
	{
		if (!ai().game_graph().valid_vertex_id(member.m_tGraphID) && ai().game_graph().valid_vertex_id(squad.m_tGraphID))
			member.m_tGraphID = squad.m_tGraphID; // where it comes home (host_unmark) if nothing newer is said of it
		if (host_adopt_member(author, squad.ID, member.ID, state_of(&member)))
			return squad.coop_leave_member_held(member.ID);
		static u32 not_adopted = 0;
		if (++not_adopted <= 8 || not_adopted % 100 == 0)
			::Msg("! [coop] levels: member %u left squad %u of map server %u, which cannot hold it on its own (%u so far): it is this "
			      "world's",
			    u32(member.ID), u32(squad.ID), mapserver::bridge_index(author), not_adopted);
		break;
	}
	case accept::Leave::Home: break;
	}
	return squad.coop_leave_member(member.ID);
}
} // namespace

uint32_t host_fence_of(PeerId, uint16_t) { return writer::kNoFence; }

namespace
{
// host_apply_fact and host_apply_heal: one acceptance, one apply; `origin` decides only how a Left leaves (leave_squad).
ApplyVerdict apply_entry(PeerId author, const journal::Entry& e, uint8_t& why, uint32_t epoch, Origin origin)
{
	using H = wire::LevelEntityHead;
	why = 0;
	if (!mgr().is_host() || !world_up() || !ai().get_alife())
	{
		why = journal::Refused;
		return ApplyVerdict::Refused;
	}
	if (!journal::body_ok(e.op, e.body))
		return refuse(author, e.op, wire::kNoObject, journal::Malformed, why);
	const LevelId level = mapserver::level_of_bridge(author);
	const EchoOf echo(level); // its players have heard this from it
	accept::Facts f;
	f.op = e.op;
	f.sender = author;
	f.sender_level = level;
	f.epoch = epoch;

	switch (accept::kind_of(e.op))
	{
	case accept::Kind::Own:
		if (e.op == H::Money)
			return apply_money(author, level, e, why);
		why = journal::Refused; // Carry, Done, Flushed, IdAsk: the journal's receiver applies them by their own rules
		return ApplyVerdict::Refused;
	case accept::Kind::Ask:
	{
		Reader r(e.body.data(), e.body.size());
		journal::AskBody b;
		if (!journal::reads_whole(r, b))
			return refuse(author, e.op, wire::kNoObject, journal::Malformed, why);
		CSE_ALifeDynamicObject* obj = object_of(b.id);
		RootSeen root;
		f.exists = obj != nullptr;
		if (obj)
		{
			root = root_seen(b.id);
			f.ghost = root.ghost;
			f.same = crc_of(obj) == b.crc;
			f.writer = root.writer;
			f.root_level = root.facts.level;
			f.fence = host_fence_of(author, root.facts.root);
		}
		const accept::Verdict v = accept::accepts(f);
		if (!v.ok)
			return refused_by(author, e.op, b.id, v, why);
		if (b.what == journal::AskBody::Release)
		{
			const char* no = nullptr;
			if (!release_here(obj, no))
				return refuse(author, e.op, b.id, journal::Refused, why, no);
			return applied(e.op);
		}
		if (world::is_player_character(obj))
			return refuse(author, e.op, b.id, journal::Refused, why, "a player's infos are his story's");
		set_info(b.id, b.info, b.what == journal::AskBody::GiveInfo);
		return applied(e.op);
	}
	case accept::Kind::Fact: break;
	}

	// Joined, Left, Info: no head
	if (e.op == H::Joined || e.op == H::Left)
	{
		Reader r(e.body.data(), e.body.size());
		journal::MemberBody b;
		if (!journal::reads_whole(r, b))
			return refuse(author, e.op, wire::kNoObject, journal::Malformed, why);
		CSE_ALifeOnlineOfflineGroup* squad = smart_cast<CSE_ALifeOnlineOfflineGroup*>(object_of(b.squad));
		CSE_ALifeMonsterAbstract* member = smart_cast<CSE_ALifeMonsterAbstract*>(object_of(b.member));
		f.exists = squad && member;
		if (f.exists)
		{
			const RootSeen of_squad = root_seen(b.squad);
			f.fence = host_fence_of(author, of_squad.facts.root);
			if (e.op == H::Joined)
			{
				f.writer = root_seen(b.member).writer; // where it comes from
				f.two_roots = true;
				f.writer_new = of_squad.writer;
			}
			else
				f.writer = of_squad.writer;
		}
		const accept::Verdict v = accept::accepts(f);
		if (!v.ok)
			return refused_by(author, e.op, b.member, v, why);
		if (e.op == H::Joined)
		{
			if (!squad->coop_join_member(b.member))
				return refuse(author, e.op, b.member, journal::Refused, why, "no living creature of its own to join");
			host_member_joined(author, b.member); // §8a A17: a creature it held on its own goes with the squad now (Holder::forget there)
			return applied(e.op);
		}
		if (!leave_squad(author, *squad, *member, origin))
			++g_accept.repeats; // it was no member here: nothing to change
		return applied(e.op);
	}
	if (e.op == H::Info)
	{
		Reader r(e.body.data(), e.body.size());
		journal::InfoBody b;
		if (!journal::reads_whole(r, b))
			return refuse(author, e.op, wire::kNoObject, journal::Malformed, why);
		CSE_ALifeDynamicObject* obj = object_of(b.id);
		RootSeen root;
		f.exists = obj != nullptr;
		if (obj)
		{
			root = root_seen(b.id);
			f.ghost = root.ghost;
			f.writer = root.writer;
			f.fence = host_fence_of(author, root.facts.root);
		}
		const accept::Verdict v = accept::accepts(f);
		if (!v.ok)
			return refused_by(author, e.op, b.id, v, why);
		if (world::is_player_character(obj))
			return refuse(author, e.op, b.id, journal::Refused, why, "a player's infos are his story's");
		set_info(b.id, b.info, b.on != 0);
		return applied(e.op);
	}

	// the per-object ops: the head, then today's layout
	journal::ObjectHead h;
	if (!journal::object_of(e, h))
		return refuse(author, e.op, wire::kNoObject, journal::Malformed, why);
	Reader lr = journal::layout_of(e);
	RootSeen root;
	Seen seen; // O6: the refusal lines name the section crc of the head
	seen.crc = h.crc;

	if (e.op == H::Spawn)
	{
		const u16 id = lr.u16();
		const u16 parent = lr.u16();
		lr.u8(); // online at the sender
		float pos[3], ang[3];
		lr.vec3(pos);
		lr.vec3(ang);
		lr.u32(); // node
		const u16 graph = lr.u16();
		if (!lr.ok() || id != h.id)
			return refuse(author, e.op, h.id, journal::Malformed, why, nullptr, seen);
		const CSE_ALifeDynamicObject* here = object_of(id);
		f.id_in_slice = host_id_is_senders(author, id); // W9 (fix A1): its slice, or a block this world gave it
		f.exists = here != nullptr;
		f.same = here && crc_of(here) == h.crc;
		if (parent != 0xffff)
		{
			if (!object_of(parent))
				return refuse(author, e.op, id, journal::Unknown, why, "its holder is not here", seen);
			root = root_seen(parent);
			f.writer = root.writer;
			f.ghost = root.ghost;
			f.fence = host_fence_of(author, root.facts.root);
		}
		else
		{
			// Fix A2 (W1.2c, §8a A4): what stands nowhere but where it is made is written by the server that leads that map (or stops),
			// as its residents are: a map server still Starting writes none, and one that leads another map writes no object here
			const LevelId at = ai().game_graph().valid_vertex_id(graph) ? level_of_vertex(graph) : wire::kNoLevel;
			const writer::Writer of_senders_map = level != wire::kNoLevel ?
			    mode().writer_of(writer::RootFacts{writer::RootKind::Resident, id, level}) : writer::this_process();
			f.rootless = true;
			f.spawn_level = at;
			f.sender_leads = of_senders_map == writer::other(author);
			f.writer = accept::spawn_on_senders_map(at, level) ? of_senders_map :
			                                                     mode().writer_of(writer::RootFacts{writer::RootKind::Resident, id, at});
			f.fence = host_fence_of(author, id);
		}
		const accept::Verdict v = accept::accepts(f);
		if (!v.ok)
			return refused_by(author, e.op, id, v, why, seen);
		if (v.repeat)
		{
			++g_accept.repeats;
			return applied(e.op);
		}
		// its id is kept out of this generator (the slices): it is freed here for this object alone
		xrServer& server = alife_rw().server();
		if (!server.IsFreeID(id) && !server.ID_to_entity(id))
			server.FreeID(id, Device.TimerAsync());
		Reader spawn = journal::layout_of(e);
		entities::host_spawn_from_leader(spawn);
		host_bind_squads(id);
		if (!object_of(id))
			return refuse(author, e.op, id, journal::Refused, why, "it was not made here", seen);
		return applied(e.op);
	}

	CSE_ALifeDynamicObject* obj = object_of(h.id);
	object_facts(f, obj, h, author, root);
	switch (e.op)
	{
	case H::Destroy:
	{
		const accept::Verdict v = accept::accepts(f);
		if (!v.ok)
			return refused_by(author, e.op, h.id, v, why, seen);
		const CSE_ALifeMonsterAbstract* m = smart_cast<const CSE_ALifeMonsterAbstract*>(obj);
		const bool member = m && m->m_group_id != 0xffff;
		if (!member && online_at_server(h.id))
			return applied(e.op); // a unit its holder writes: its Destroy on the transfer lane releases it here (host_gone)
		const char* no = nullptr;
		if (!release_here(obj, no))
			return refuse(author, e.op, h.id, journal::Refused, why, no, seen);
		return applied(e.op);
	}
	case H::Died:
	{
		const u16 victim = lr.u16();
		const u16 killer = lr.u16();
		std::vector<u8> st(lr.remaining());
		if (!st.empty())
			lr.bytes(st.data(), st.size());
		if (!lr.ok())
			return refuse(author, e.op, h.id, journal::Malformed, why, nullptr, seen);
		const accept::Verdict v = accept::accepts(f);
		if (!v.ok)
			return refused_by(author, e.op, victim, v, why, seen);
		const CSE_ALifeCreatureAbstract* dying = smart_cast<const CSE_ALifeCreatureAbstract*>(object_of(victim));
		const bool was_alive = dying && dying->g_Alive();
		host_apply_death(victim, killer, st); // a body that is dead here already: nothing changes
		// R32 W3 (D finding 1): the stock on_death runs no server event; recorded here, under the EchoOf above, the death reaches the
		// players and map servers of the other levels at the flush, and not the level that reported it (to_bridges, passes_to)
		if (was_alive)
			events::host_on_death(victim, killer);
		return applied(e.op);
	}
	case H::Parent:
	{
		const u16 item = lr.u16();
		const u16 parent = lr.u16();
		float p[3];
		lr.vec3(p);
		Fvector pos;
		pos.set(p[0], p[1], p[2]);
		if (!lr.ok() || !_valid(pos))
			return refuse(author, e.op, item, journal::Malformed, why, nullptr, seen);
		Seen to_seen = seen; // O6 (#1646/#1649): where its entry puts it
		to_seen.move = true;
		to_seen.to = parent;
		f.two_roots = true;
		if (parent != 0xffff)
		{
			if (!object_of(parent))
				return refuse(author, e.op, item, journal::Unknown, why, "its new holder is not here", to_seen);
			const RootSeen to = root_seen(parent);
			f.writer_new = to.writer;
			f.ghost = f.ghost || to.ghost;
		}
		else
			f.writer_new = writer::other(author); // put down: it lies on the sender's map, where it was dropped
		const accept::Verdict v = accept::accepts(f);
		if (!v.ok)
			return refused_by(author, e.op, item, v, why, to_seen);
		const CSE_ALifeDynamicObject* moved = object_of(item);
		const u16 old_parent = moved ? moved->ID_Parent : u16(0xffff);
		host_apply_parent(item, parent, pos);
		// R32 W3 (D finding 1): the stock attach / detach run no server event; a real change is recorded here, under the EchoOf above,
		// for the other levels (the reporting one has it: to_bridges, passes_to)
		const u16 now_parent = moved ? moved->ID_Parent : u16(0xffff);
		if (now_parent != old_parent)
			events::host_on_parent_changed(item, old_parent, now_parent);
		return applied(e.op);
	}
	case H::State:
	{
		const accept::Verdict v = accept::accepts(f);
		if (!v.ok)
			return refused_by(author, e.op, h.id, v, why, seen);
		if (!host_take_body(author, level, lr))
		{
			why = journal::Refused; // host_take_body wrote its own line
			return ApplyVerdict::Refused;
		}
		return applied(e.op);
	}
	case H::Item:
	{
		journal::ItemFactsBody b;
		if (!journal::reads_whole(lr, b))
			return refuse(author, e.op, h.id, journal::Malformed, why, nullptr, seen);
		const accept::Verdict v = accept::accepts(f);
		if (!v.ok)
			return refused_by(author, e.op, h.id, v, why, seen);
		const char* no = nullptr;
		// W8.3: the writer's deliberate write (Decider) or the owner's report it took (Owner) goes into the world's entity, players'
		// items included (the acceptance above checked that the author writes the item); loot keeps its wear-only path. Decider and
		// Owner are taken alike: the blob is the writer's capture of its own copy. A fact the world cannot store is Refused whole.
		const bool taken = b.source == journal::ItemFactsBody::Loot
		    ? items::host_take_level_state(b.item, b.section.c_str(), b.blob.data(), b.blob.size(), no)
		    : items::host_take_item_facts(b.item, b.section.c_str(), b.blob.data(), b.blob.size(), no);
		if (!taken)
			return refuse(author, e.op, h.id, journal::Refused, why, no); // refuse() tells the uses refusal's branch (O6)
		return applied(e.op);
	}
	case H::Tent:
	{
		lr.u16(); // the tent: the head's object
		const u16 character = lr.u16();
		if (!lr.ok())
			return refuse(author, e.op, h.id, journal::Malformed, why, nullptr, seen);
		const accept::Verdict v = accept::accepts(f);
		if (!v.ok)
			return refused_by(author, e.op, h.id, v, why, seen);
		// P1: the map server that writes the tent says whose it is; his seat may have moved on since it put it up
		checkpoints::sources::host_tent_up(h.id, character);
		return applied(e.op);
	}
	case H::Stash:
	{
		lr.u16(); // the character: the head's object
		const bool things_stay = lr.u8() != 0;
		if (!lr.ok())
			return refuse(author, e.op, h.id, journal::Malformed, why, nullptr, seen);
		const accept::Verdict v = accept::accepts(f);
		if (!v.ok)
			return refused_by(author, e.op, h.id, v, why, seen);
		survival::host_stash_over(h.id, things_stay);
		return applied(e.op);
	}
	case H::Logic:
	{
		wire::LevelLogicBody b;
		if (!journal::reads_whole(lr, b))
			return refuse(author, e.op, h.id, journal::Malformed, why, nullptr, seen);
		const accept::Verdict v = accept::accepts(f);
		if (!v.ok)
			return refused_by(author, e.op, h.id, v, why, seen);
		if (!host_apply_logic(b.id, b.bytes, level))
		{
			why = journal::Refused; // host_apply_logic wrote its own line
			return ApplyVerdict::Refused;
		}
		return applied(e.op);
	}
	default: break;
	}
	return refuse(author, e.op, h.id, journal::Malformed, why, nullptr, seen);
}
} // namespace

ApplyVerdict host_apply_fact(PeerId author, const journal::Entry& e, uint8_t& why, uint32_t epoch)
{
	return apply_entry(author, e, why, epoch, Origin::Journal);
}

ApplyVerdict host_apply_heal(PeerId author, const journal::Entry& e, uint8_t& why, uint32_t epoch)
{
	return apply_entry(author, e, why, epoch, Origin::Heal);
}

void host_accept_print_status()
{
	u32 taken = 0, refused = 0;
	for (const u32 n : g_accept.applied)
		taken += n;
	for (const u32 n : g_accept.refused)
		refused += n;
	::Msg("[coop] levels: the journal's entries: %u taken (%u repeats), %u refused", taken, g_accept.repeats, refused);
	for (uint8_t op = 0; op <= journal::kLastOp; ++op)
		if (g_accept.applied[op])
			::Msg("[coop] levels:   %s taken %u", journal::op_name(op), g_accept.applied[op]);
	for (uint8_t w = 1; w <= journal::kLastWhy; ++w)
		if (g_accept.refused[w])
			::Msg("[coop] levels:   refused, %s: %u", journal::why_name(w), g_accept.refused[w]);
}
} // namespace levels
} // namespace coop

// Anomaly Coop (R32 §8a A17), declared in xrServer_Objects_ALife_Monsters.h: the world server, a member left this squad on the map
// server that holds it and is held by that map server on its own now (coop_levels_alife.cpp leave_squad). Out of the squad as
// unregister_member takes it, but not into this world's graph and scheduler, and still online elsewhere: the world does not simulate
// it. Its vertex is set by the caller (where it comes home); it is in no registry here, as a marked object is (mark_object).
bool CSE_ALifeOnlineOfflineGroup::coop_leave_member_held(ALife::_OBJECT_ID member_id)
{
	const MEMBERS::iterator I = m_members.find(member_id);
	if (I == m_members.end() || !(*I).second)
		return false;
	MEMBER* m = (*I).second;
	m->m_group_id = 0xffff;
	m->m_bOnline = true;
	m_members.erase(I);
	if (m_members.empty())
		m_flags.set(flUsedAI_Locations, FALSE);
	if (alife().scheduled().object(member_id, true))
		alife().scheduled().remove(m);
	const GameGraph::_GRAPH_ID vertex = m->m_tGraphID;
	if (ai().game_graph().valid_vertex_id(vertex))
	{
		const auto& here = alife().graph().objects()[vertex].objects().objects();
		if (here.find(member_id) != here.end())
			alife().graph().remove(m, vertex, false);
	}
	return true;
}

// Anomaly Coop (R32 §8a A17, fix round 2 of package A, verifier finding 2), declared in xrServer_Objects_ALife_Monsters.h: the world
// server, a Left of a member another squad bound before it (that squad's Spawn listed it and coop_bind_members bound it there: the map
// server made that squad and moved the member into it in one frame). Only this squad's list forgets it; the member is left as its new
// squad has it (m_group_id, m_bOnline, out of the graph and the scheduler). False: it is not listed.
bool CSE_ALifeOnlineOfflineGroup::coop_unlist_member(ALife::_OBJECT_ID member_id)
{
	const MEMBERS::iterator I = m_members.find(member_id);
	if (I == m_members.end())
		return false;
	m_members.erase(I);
	if (m_members.empty())
		m_flags.set(flUsedAI_Locations, FALSE);
	return true;
}
