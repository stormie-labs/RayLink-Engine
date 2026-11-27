// coop::story, task targets: the host keeps a client's copy of each running task's target where the host has it, so the
// client's map spot and compass point at the right place.
#include "stdafx.h"
#include "coop_story_impl.h"
#include "coop_manager.h"
#include "coop_alife.h"
#include "xrCoop/coop_mode.h"

#include "Level.h"
#include "GameTaskManager.h"
#include "GameTask.h"
#include "map_manager.h"
#include "alife_simulator.h"
#include "alife_object_registry.h"
#include "alife_graph_registry.h"
#include "game_graph.h"
#include "xrServer_Objects_ALife_Monsters.h"
#include "ai_space.h"

namespace coop
{
namespace story
{

namespace
{
// --- where the targets are ---
// A client's ALife does not run and offline entities are mirrored only when they spawn or switch. CMapLocation and the coop
// compass read the client's copy of a target that is offline there, so the host keeps that copy where the target is.
struct TargetPlace
{
	u16 id = kNoId; // host numbering
	float pos[3] = {0, 0, 0};
	u32 node = 0;
	u16 game_vertex = 0xffff;
};
} // namespace

xr_map<u16, SentTarget> g_target_sent; // host: last place sent per target
u64 g_target_last_scan = 0;

namespace
{
constexpr size_t kMaxTargetRx = 256;
} // namespace

xr_map<u16, TargetRx> g_target_rx;
xr_map<u16, u64> g_target_unknown_logged; // host id -> logged at
u64 g_target_unknown = 0; // places for a target this machine has no copy of

namespace
{
const char* take_name(task_target::Take t)
{
	switch (t)
	{
	case task_target::Take::Unknown: return "unknown here";
	case task_target::Take::Follow: return "follows its game object";
	case task_target::Take::Own: return "moved by this machine";
	case task_target::Take::Offline: return "placed offline";
	case task_target::Take::Online: return "placed online (no game object)";
	}
	return "?";
}

bool target_place(u16 local_id, TargetPlace& out)
{
	const CSE_ALifeDynamicObject* e = ai().get_alife() ? ai().alife().objects().object(local_id, true) : nullptr;
	if (!e)
		return false;
	const Fvector p = e->draw_level_position(); // a monster's offline path too, as the host's own map draws it
	out.id = host_id_of(local_id);
	out.pos[0] = p.x;
	out.pos[1] = p.y;
	out.pos[2] = p.z;
	out.node = e->m_tNodeID;
	out.game_vertex = e->m_tGraphID;
	return true;
}

void write_targets(Writer& w, const TargetPlace* list, u32 n)
{
	w.u8(TaskTargets);
	w.u16(u16(n));
	for (u32 i = 0; i < n; ++i)
	{
		w.u16(list[i].id);
		w.vec3(list[i].pos);
		w.u32(list[i].node);
		w.u16(list[i].game_vertex);
	}
}

template <typename Send>
void send_targets(const xr_vector<TargetPlace>& list, Send send)
{
	for (u32 at = 0; at < list.size(); at += kTargetsPerMessage)
	{
		Writer w(256);
		write_targets(w, list.data() + at, _min<u32>(kTargetsPerMessage, u32(list.size()) - at));
		send(w);
		++g_stats.tasks_out;
	}
}

// The targets of the running tasks (with a spot): all of them for a resync, else those that moved. `for_peer`: only the targets
// of the tasks that peer may see (a resync to him); kInvalidPeer: every task's.
xr_vector<TargetPlace> collect_targets(bool all, xr_map<u16, SentTarget>* next, PeerId for_peer = kInvalidPeer)
{
	xr_vector<TargetPlace> out;
	const u64 now = mgr().now_ms();
	xr_set<u16> seen;
	const auto consider = [&](u16 local_id)
	{
		TargetPlace p;
		if (!target_place(local_id, p))
			return;
		task_target::Place place;
		place.x = p.pos[0];
		place.y = p.pos[1];
		place.z = p.pos[2];
		place.game_vertex = p.game_vertex;
		const auto it = g_target_sent.find(p.id);
		const SentTarget sent = it != g_target_sent.end() ? it->second : SentTarget();
		if (all || task_target::due(sent.place, sent.at, place, now))
		{
			out.push_back(p);
			if (next)
				(*next)[p.id] = SentTarget{place, now};
		}
		else if (next)
			(*next)[p.id] = sent;
	};
	for (const SGameTaskKey& k : tasks().GetGameTasks())
	{
		const CGameTask* t = k.game_task;
		if (!t || is_local_task(k.task_id) || t->GetTaskState() != eTaskStateInProgress || !t->m_map_location.size() ||
			t->m_map_object_id == kNoId || (for_peer != kInvalidPeer && !may_see(for_peer, k.task_id)) ||
			!seen.insert(t->m_map_object_id).second)
			continue;
		consider(t->m_map_object_id);
	}
	// A shared spot of a task script on a squad (an assault task's red_location on the enemy squad, tasks_assault.script):
	// a squad has no game object, every machine draws it at its own copy of the squad, which a client's ALife never moves.
	if (ai().get_alife())
		for (const SLocationKey& k : spots().Locations())
		{
			if (!k.location || k.object_id == kNoId || seen.count(k.object_id) || !spot_replicable(k.location, k.spot_type))
				continue;
			CSE_ALifeDynamicObject* e = ai().alife().objects().object(k.object_id, true);
			if (!e || !e->cast_online_offline_group())
				continue;
			seen.insert(k.object_id);
			consider(k.object_id);
		}
	return out;
}
} // namespace

void host_scan_targets()
{
	g_target_last_scan = mgr().now_ms();
	if (!in_game())
		return;
	xr_map<u16, SentTarget> next;
	const xr_vector<TargetPlace> moved = collect_targets(false, &next);
	g_target_sent.swap(next); // a target nothing points at any more is forgotten
	send_targets(moved, [](Writer& w) { net::host_broadcast(Msg::TaskUpdate, w); });
}

void send_targets_to(PeerId to)
{
	if (in_game())
		send_targets(collect_targets(true, nullptr, to), [to](Writer& w) { net::host_send(to, Msg::TaskUpdate, w); });
}

namespace
{
// The host sends targets this machine may never use. Count them all and log once a minute per target used locally.
void log_unknown_target(u16 host_id, u16 local, u64 now)
{
	++g_target_unknown;
	if (local == kNoId || !in_game())
		return;
	const auto it = g_target_unknown_logged.find(host_id);
	if (it != g_target_unknown_logged.end() && !task_target::log_due(it->second, now))
		return;
	const CGameTask* by = nullptr;
	for (const SGameTaskKey& k : tasks().GetGameTasks())
		if (k.game_task && k.game_task->m_map_object_id == local)
		{
			by = k.game_task;
			break;
		}
	if (!by)
		return;

	if (it == g_target_unknown_logged.end() && g_target_unknown_logged.size() >= kMaxTargetRx)
		g_target_unknown_logged.clear();
	g_target_unknown_logged[host_id] = now;
	::Msg("! [coop] story: task target %u of the host (task '%s') is unknown here: its spot has no place on this machine "
		  "(%llu such places so far)", host_id, cs(by->m_ID), g_target_unknown);
}

void note_target_rx(u16 host_id, task_target::Take take, u64 now)
{
	if (g_target_rx.size() >= kMaxTargetRx && g_target_rx.find(host_id) == g_target_rx.end())
		g_target_rx.clear(); // bookkeeping for coop_status only
	TargetRx& rx = g_target_rx[host_id];
	rx.at = now;
	++rx.n;
	rx.take = take;
}
} // namespace

void client_on_task_targets(Reader& r)
{
	const u16 n = r.u16();
	const u64 now = mgr().now_ms();
	for (u16 i = 0; i < n; ++i)
	{
		const u16 id = r.u16();
		float p[3];
		r.vec3(p);
		const u32 node = r.u32();
		const u16 game_vertex = r.u16();
		if (!r.ok())
			return;
		if (!ai().get_alife())
			continue;
		const u16 local = local_id_of(id);
		CSE_ALifeDynamicObject* e = ai().alife().objects().object(local, true);
		task_target::Here here;
		here.known = e != nullptr;
		here.online = e && e->m_bOnline;
		here.held = e && e->ID_Parent != kNoId;
		here.level_object = in_game() && Level().Objects.net_Find(local) != nullptr;
		here.map_authority = mode().decides_world() || mode().leads_level();
		const task_target::Take take = task_target::take(here);
		note_target_rx(id, take, now);
		if (take == task_target::Take::Unknown)
		{
			log_unknown_target(id, local, now);
			continue;
		}
		const bool offline = take == task_target::Take::Offline;
		if (!offline && take != task_target::Take::Online)
			continue; // the spot follows the game object / this machine's own update moves it
		// An online squad (Take::Online) has no game object: CMapLocation and the compass read its entity, which nothing here
		// moves. Online it is in no graph registry (CSE_ALifeOnlineOfflineGroup::switch_online), so only its place changes.
		const bool vertex_ok = ai().game_graph().valid_vertex_id(game_vertex);
		// as coop_entities place(): only what lives in the graph registry moves between its vertices
		if (offline && e->ID_Parent == kNoId && e->used_ai_locations() && e->m_tGraphID != game_vertex && vertex_ok &&
			ai().game_graph().valid_vertex_id(e->m_tGraphID))
			alife_rw().graph().change(e, e->m_tGraphID, game_vertex);
		e->o_Position.set(p[0], p[1], p[2]);
		e->m_tNodeID = node;
		if (offline || vertex_ok)
			e->m_tGraphID = game_vertex;
	}
}

// coop_status: where each running task with a spot points and what this machine has of that target. The host's line and a
// client's line for the same target name the same host id and, when the spot is in step, nearly the same place.
void print_task_targets()
{
	if (!in_game())
		return;
	const bool host = mode().decides_world();
	const u64 now = mgr().now_ms();
	u32 n = 0;
	for (const SGameTaskKey& k : tasks().GetGameTasks())
	{
		const CGameTask* t = k.game_task;
		if (!t || is_local_task(k.task_id) || t->GetTaskState() != eTaskStateInProgress || !t->m_map_location.size() ||
			t->m_map_object_id == kNoId)
			continue;
		if (++n > 32)
		{
			::Msg("[coop] story:   ... more task targets");
			break;
		}
		const u16 local = t->m_map_object_id;
		const u16 hid = host_id_of(local);
		const CSE_ALifeDynamicObject* e = ai().get_alife() ? ai().alife().objects().object(local, true) : nullptr;
		const bool object = Level().Objects.net_Find(local) != nullptr;
		string128 where = "no place";
		if (e)
		{
			const Fvector p = e->draw_level_position();
			xr_sprintf(where, "at %.1f %.1f %.1f gv %u", p.x, p.y, p.z, u32(e->m_tGraphID));
		}
		string128 sync = "";
		if (host)
		{
			const auto it = g_target_sent.find(hid);
			if (it != g_target_sent.end() && it->second.at)
				xr_sprintf(sync, "place sent %llu ms ago", now - it->second.at);
			else
				xr_strcpy(sync, "place not sent");
		}
		else
		{
			const auto it = g_target_rx.find(hid);
			if (it != g_target_rx.end())
				xr_sprintf(sync, "%u places from the host, last %llu ms ago: %s", it->second.n, now - it->second.at,
					take_name(it->second.take));
			else
				xr_strcpy(sync, "no place from the host");
		}
		::Msg("[coop] story:   task '%s' spot '%s' -> target %u (host %u): %s, %s, %s, %s; %s", cs(t->m_ID),
			cs(t->m_map_location), u32(local), u32(hid), e ? "known" : "UNKNOWN here", e && e->m_bOnline ? "online" : "offline",
			object ? "game object" : "no game object", where, sync);
	}
	if (!host)
		::Msg("[coop] story: task target places for ids unknown here %llu", g_target_unknown);
}
} // namespace story
} // namespace coop
