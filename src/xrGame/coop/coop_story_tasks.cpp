// coop::story, tasks: the host scans its task list and sends what changed, by each task's audience; a client applies the
// host's records over what its own scripts did, and routes its scripts' task requests to the host.
#include "stdafx.h"
#include "pch_script.h"
#include "coop_story_impl.h"
#include "coop_manager.h"
#include "coop_world.h"
#include "xrCoop/coop_mode.h"

#include "Level.h"
#include "Actor.h"
#include "GameTaskManager.h"
#include "GameTask.h"
#include "map_location.h"
#include "map_spot.h"
#include "UIGameCustom.h"
#include "ai_space.h"
#include "script_engine.h"
#include "xrCoop/coop_lua_fn.h"

namespace coop
{
namespace story
{

namespace
{
// ------------------------------------------------------------------ tasks

// Personal tasks (coop.story_local_task): this machine only. Process lifetime, like addon task ids.
constexpr size_t kMaxLocalTasks = 1024;
} // namespace

xr_set<shared_str> g_local_tasks;
u64 g_local_task_edits = 0; // engine task hooks that let a personal task through locally

bool is_local_task(const shared_str& id) { return id.size() && g_local_tasks.count(id) != 0; }

// Who sees a task (xrCoop/coop_task_audience.h). Host only: the host's Lua sets it
// (coop.story_task_audience) in a split-levels world. A task without an entry, and every task while the table is empty, goes
// to everybody exactly as before.
task_audience::Table g_audience;

namespace
{
u64 g_audience_refused = 0; // entries the table refused (full)

// The character a peer plays on the host, as coop.avatar_of gives it to Lua: 0 for the host peer, else his proxy;
// task_audience::kNoCharacter (0xffff) while he has none (such a peer is out of every task that has an audience).
u16 character_of(PeerId peer) { return peer == kHostPeer ? u16(0) : world::avatar_of(peer); }

// A connection of this session that is none of its players (Session: joined, yet in no peers()). A co-op session has none; in a
// server world it is a map server's connection, and it runs the logic of every task for its map. A player who left is neither.
// The session says it, not a server function (docs/MODES.md section 3: the Core names none; tests/test_story_bridges.cpp).
bool no_player_connection(PeerId peer)
{
	const Session& s = mgr().session();
	return peer != kHostPeer && s.joined(peer) && !s.peer(peer);
}
} // namespace

// May task `id` be sent to `peer`? A connection that is no player (a map server's) gets every task: it runs the logic of every task,
// and an audience names players' characters.
// The host player (peer 0 of a listen world server) is NOT filtered here, by design: he reads the authority's own task
// manager, and TaskUpdate / TaskList / StoryHash do not go over his LocalLink, so nothing sent from here could hide a task
// from him (plan "Known limits": phase 2, a filter in the PDA of the host process).
bool may_see(PeerId peer, const shared_str& id)
{
	if (g_audience.empty() || peer == kHostPeer)
		return true;
	return g_audience.allowed(cs(id), character_of(peer), no_player_connection(peer));
}

namespace
{
// Task `id` has an audience: it goes per peer (send_by_audience), not by host_broadcast.
bool has_audience(const shared_str& id) { return !g_audience.empty() && g_audience.find(cs(id)) != nullptr; }

// A task message by its audience: filter joined players by reach and membership,
// then send to the mode's other links with their usual delivery rules.
void send_by_audience(Msg type, const Writer& w, const shared_str& id)
{
	if (!mode().decides_world() && !mode().leads_level())
		return;
	for (const PeerInfo& p : mgr().session().peers())
		if (p.state == PeerState::Joined && mode().reaches(p.id, type) && may_see(p.id, id))
			net::host_send(p.id, type, w); // queued for a peer that is still loading, as host_broadcast does
	mode().deliver_links(type, w, Lane::Reliable);
}

void capture(const CGameTask* t, TaskRec& r)
{
	r.id = t->m_ID;
	r.state = u16(t->GetTaskState());
	r.type = u16(t->GetTaskType());
	r.title = t->m_Title;
	r.descr = t->m_Description;
	r.icon = t->m_icon_texture_name;
	r.hint = t->m_map_hint;
	r.map_location = t->m_map_location;
	r.map_object_id = r.map_location.size() ? host_id_of(t->m_map_object_id) : kNoId;
	r.priority = t->m_priority;
	r.receive = t->m_ReceiveTime;
	r.finish = t->m_FinishTime;
	r.to_complete = t->m_TimeToComplete;
	r.timer_finish = t->m_timer_finish;
}

void write_task(Writer& w, const TaskRec& r)
{
	w.str(cs(r.id));
	w.u16(r.state);
	w.u16(r.type);
	w.str(cs(r.title));
	w.str(cs(r.descr));
	w.str(cs(r.icon));
	w.str(cs(r.hint));
	w.str(cs(r.map_location));
	w.u16(r.map_object_id);
	w.u32(r.priority);
	w.u64(r.receive);
	w.u64(r.finish);
	w.u64(r.to_complete);
	w.u64(r.timer_finish);
	w.u32(r.origin);
}

bool read_task(Reader& rd, TaskRec& r)
{
	r.id = to_shared(rd.str());
	r.state = rd.u16();
	r.type = rd.u16();
	r.title = shared_str(rd.str().c_str());
	r.descr = shared_str(rd.str().c_str());
	r.icon = to_shared(rd.str());
	r.hint = to_shared(rd.str());
	r.map_location = to_shared(rd.str());
	r.map_object_id = rd.u16();
	r.priority = rd.u32();
	r.receive = rd.u64();
	r.finish = rd.u64();
	r.to_complete = rd.u64();
	r.timer_finish = rd.u64();
	r.origin = rd.u32();
	return rd.ok() && r.id.size();
}

// Does the record still say what capture() would take from task `t`? Compared in place: a scan copies nothing of a task that
// did not change. Every field is compared, a Lua setter (set_title, set_description, ...) changes a task without any hook.
bool unchanged(const TaskRec& r, const CGameTask* t)
{
	return r.id == t->m_ID && r.state == u16(t->GetTaskState()) && r.type == u16(t->GetTaskType()) && r.title == t->m_Title &&
		r.descr == t->m_Description && r.icon == t->m_icon_texture_name && r.hint == t->m_map_hint &&
		r.map_location == t->m_map_location && r.priority == t->m_priority && r.receive == t->m_ReceiveTime &&
		r.finish == t->m_FinishTime && r.to_complete == t->m_TimeToComplete && r.timer_finish == t->m_timer_finish &&
		r.map_object_id == (t->m_map_location.size() ? host_id_of(t->m_map_object_id) : kNoId);
}

u64 task_item_hash(const CGameTask* t)
{
	Fnv f;
	f.str(t->m_ID);
	f.pod(u64(t->m_ReceiveTime));
	f.pod(u16(t->GetTaskState()));
	f.pod(u16(t->GetTaskType()));
	f.str(t->m_Title);
	f.str(t->m_Description);
	f.str(t->m_icon_texture_name);
	f.pod(u32(t->m_priority));
	return f.h;
}
} // namespace

DomainHash hash_tasks()
{
	DomainHash d;
	if (!in_game())
		return d;
	for (const SGameTaskKey& k : tasks().GetGameTasks())
		if (k.game_task && !is_local_task(k.task_id))
			d.add(task_item_hash(k.game_task));
	return d;
}

// Host: the tasks domain as `peer` should have it, without the tasks he may not see (what send_task_list sends him).
DomainHash hash_tasks_for(PeerId peer)
{
	DomainHash d;
	if (!in_game())
		return d;
	for (const SGameTaskKey& k : tasks().GetGameTasks())
		if (k.game_task && !is_local_task(k.task_id) && may_see(peer, k.task_id))
			d.add(task_item_hash(k.game_task));
	return d;
}

namespace
{
CGameTask* find_task(const TaskKey& key, CGameTask** last = nullptr)
{
	CGameTask* first = nullptr;
	if (last)
		*last = nullptr;
	for (const SGameTaskKey& k : tasks().GetGameTasks())
	{
		if (!k.game_task || k.task_id != key.id || k.game_task->m_ReceiveTime != key.receive)
			continue;
		if (!last)
			return k.game_task;
		if (!first)
			first = k.game_task;
		*last = k.game_task;
	}
	return first;
}
} // namespace

// --- host ---
xr_map<TaskKey, TaskRec> g_host_sent; // last broadcast state per task
bool g_host_prime = true; // record the current list without broadcasting (after hosting start / load)
const void* g_host_level = nullptr;
u32 g_host_epoch = 0; // join::host_world_epoch() of g_host_level
u64 g_host_last_scan = 0;
xr_map<shared_str, PeerId> g_finisher; // task id -> peer whose request finished it

void host_scan_tasks()
{
	if (!in_game())
		return;
	g_host_last_scan = mgr().now_ms();
	const bool prime = g_host_prime;
	g_host_prime = false;
	for (const SGameTaskKey& k : tasks().GetGameTasks())
	{
		CGameTask* t = k.game_task;
		if (!t || !t->m_ID.size() || is_local_task(t->m_ID))
			continue;
		const auto it = g_host_sent.find(TaskKey{t->m_ID, t->m_ReceiveTime});
		if (it != g_host_sent.end() && unchanged(it->second, t))
			continue;
		TaskRec cur;
		capture(t, cur);
		cur.origin = it != g_host_sent.end() ? it->second.origin : g_exec_peer != kInvalidPeer ? g_exec_peer : kHostPeer;
		g_host_sent[cur.key()] = cur;
		if (prime)
			continue;
		Writer w(256);
		w.u8(TaskUpsert);
		write_task(w, cur);
		if (has_audience(cur.id))
			send_by_audience(Msg::TaskUpdate, w, cur.id); // only to those who may see it (and the bridges)
		else
			net::host_broadcast(Msg::TaskUpdate, w);
		++g_stats.tasks_out;
	}
}

namespace
{
void send_task_to(PeerId to, const CGameTask* t)
{
	if (!may_see(to, t->m_ID))
		return; // not his task (its audience): he learns nothing of it
	TaskRec r;
	capture(t, r);
	auto it = g_host_sent.find(r.key());
	if (it != g_host_sent.end())
		r.origin = it->second.origin;
	Writer w(256);
	w.u8(TaskUpsert);
	write_task(w, r);
	net::host_send(to, Msg::TaskUpdate, w);
	++g_stats.tasks_out;
}
} // namespace

void send_task_list(PeerId to)
{
	if (!in_game())
		return;
	const vGameTasks& v = tasks().GetGameTasks();
	// A task he may not see is left out (its audience); one he holds here is then detached by client_on_task_list.
	u32 n = 0;
	for (const SGameTaskKey& k : v)
		if (!is_local_task(k.task_id) && may_see(to, k.task_id))
			++n;
	Writer w(v.size() * 256 + 8);
	w.u32(n);
	for (const SGameTaskKey& k : v)
	{
		if (is_local_task(k.task_id) || !may_see(to, k.task_id))
			continue; // the host's personal task, or not his to see
		TaskRec r;
		if (k.game_task)
			capture(k.game_task, r);
		auto it = g_host_sent.find(r.key());
		if (it != g_host_sent.end())
			r.origin = it->second.origin;
		write_task(w, r);
	}
	net::host_send(to, Msg::TaskList, w);
	++g_stats.tasks_out;
}

namespace
{
// Tells coop_tasks.host_note which player took (kind "taker") or finished (kind "finisher") a task that a client asked for through
// these engine requests, so the reward goes to that player. No script or function, an error in it: nothing happens.
void lua_note(LPCSTR kind, const shared_str& task_id, PeerId peer)
{
	if (!task_id.size())
		return;
	std::string error;
	luafn::call_named(ai().script_engine().lua(), "coop_tasks", "host_note", nullptr,
	    [&](lua_State* L)
	    {
		    lua_pushstring(L, cs(task_id));
		    lua_pushstring(L, kind);
		    lua_pushnumber(L, lua_Number(peer));
		    return 3;
	    },
	    0, [](lua_State*) {}, error);
}

// The host's checks of a client's TaskReqState live in coop_tasks.host_state_verdict: a turn-in passes the checks of the Lua
// request (ops.complete: money, items), a fail comes from the player who took the task. Whether a failed check refuses or is
// only logged is coop_trust's setting. The verdict also gets whether the request is explicit (a script's set_task_state) for its
// log line; a turn-in needs the host's own check of the task to say it is done, explicit or not. Only a plain false refuses: no
// script or function, an error in it, any other answer lets the request through.
bool lua_task_state_ok(const shared_str& task_id, u8 state, PeerId peer, bool bridge, bool timed_out, bool explicit_call)
{
	if (!task_id.size())
		return true;
	bool ok = true;
	std::string error;
	luafn::call_named(ai().script_engine().lua(), "coop_tasks", "host_state_verdict", nullptr,
	    [&](lua_State* L)
	    {
		    lua_pushstring(L, cs(task_id));
		    lua_pushinteger(L, int(state));
		    lua_pushnumber(L, lua_Number(peer));
		    lua_pushboolean(L, bridge ? 1 : 0);
		    lua_pushboolean(L, timed_out ? 1 : 0);
		    lua_pushboolean(L, explicit_call ? 1 : 0);
		    return 6;
	    },
	    1, [&ok](lua_State* L) { ok = !(lua_isboolean(L, -1) && !lua_toboolean(L, -1)); }, error);
	return ok;
}

// The engine's own fail of a task with a time limit (CGameTask::UpdateState) as this host's clock sees it. A client's clock may
// run a little ahead of the host's: a fail asked up to kTaskTimeSlack game ms before the host's limit counts as run out.
constexpr ALife::_TIME_ID kTaskTimeSlack = 5 * 60 * 1000; // five game minutes
bool task_time_ran_out(const CGameTask* t)
{
	if (t->m_ReceiveTime == t->m_TimeToComplete)
		return false; // no time limit
	return Level().GetGameTime() + kTaskTimeSlack > t->m_TimeToComplete;
}

// The host runs Anomaly's own task manager, task_manager.get_task_manager(), for a client's request, so its scripts track the task
// (texts, targets, completion) exactly as if the host had taken it. A request is a C function run under lua_pcall with the manager,
// the task id and a number (nil for none); it answers "ok", "nolua" (the manager has no such part) or "unknown" (no such task).
struct TaskManagerCall
{
	LPCSTR name; // in the log line of a request that raised
	lua_CFunction body;
};

// tm:give_task(id, giver)
int give_task_body(lua_State* L)
{
	lua_getfield(L, 1, "give_task");
	if (!lua_toboolean(L, -1))
	{
		lua_pushstring(L, "nolua");
		return 1;
	}
	lua_pushvalue(L, 1);
	lua_pushvalue(L, 2);
	lua_pushvalue(L, 3);
	lua_call(L, 3, 0);
	lua_pushstring(L, "ok");
	return 1;
}
constexpr TaskManagerCall kGiveTask{"@coop_story_give_task", &give_task_body};

// tm:set_task_completed(id) for state 2 (eTaskStateCompleted), else tm:set_task_failed(id), for a task the manager tracks.
int task_state_body(lua_State* L)
{
	lua_getfield(L, 1, "task_info");
	if (!lua_toboolean(L, -1))
	{
		lua_pushstring(L, "nolua");
		return 1;
	}
	lua_pushvalue(L, 2);
	lua_gettable(L, -2);
	if (!lua_toboolean(L, -1))
	{
		lua_pushstring(L, "unknown");
		return 1;
	}
	lua_getfield(L, 1, lua_tointeger(L, 3) == eTaskStateCompleted ? "set_task_completed" : "set_task_failed");
	lua_pushvalue(L, 1);
	lua_pushvalue(L, 2);
	lua_call(L, 2, 0);
	lua_pushstring(L, "ok");
	return 1;
}
constexpr TaskManagerCall kTaskState{"@coop_story_task_state", &task_state_body};

// The answer of request `c` for task `id` and number `num` (nil when !has_num): "nolua" when there is no task manager, "error"
// (logged) when the request raised or there is no Lua state.
xr_string task_manager_call(const TaskManagerCall& c, LPCSTR id, int num, bool has_num)
{
	lua_State* L = ai().script_engine().lua();
	if (!L)
		return "error";
	xr_string res = "nolua";
	std::string error;
	luafn::call_named(L, "task_manager", "get_task_manager", nullptr, [](lua_State*) { return 0; }, 1,
	    [&](lua_State* L)
	    {
		    if (!lua_toboolean(L, -1))
			    return; // no manager
		    const int tm = lua_gettop(L);
		    lua_pushcfunction(L, c.body);
		    lua_pushvalue(L, tm);
		    lua_pushstring(L, id);
		    if (has_num)
			    lua_pushinteger(L, num);
		    else
			    lua_pushnil(L);
		    if (lua_pcall(L, 3, 1, 0))
		    {
			    ::Msg("! [coop] story: %s failed: %s", c.name, lua_tostring(L, -1));
			    res = "error";
		    }
		    else
			    res = lua_isstring(L, -1) ? lua_tostring(L, -1) : "";
	    },
	    error);
	return res;
}

void set_task_texts(CGameTask* t, const TaskRec& r)
{
	if (t->m_Title != r.title)
		t->m_Title = r.title;
	if (t->m_Description != r.descr)
		t->m_Description = r.descr;
	if (t->m_icon_texture_name != r.icon)
		t->m_icon_texture_name = r.icon;
	if (t->m_priority != r.priority)
		t->m_priority = r.priority;
	if (u16(t->GetTaskType()) != r.type)
		t->SetType_script(int(r.type)); // eTaskTypeDummy is u16(-1) too
}

void fix_spot_timer(CGameTask* t)
{
	if (CMapLocation* ml = t->LinkedMapLocation())
		if (CComplexMapSpot* spot = ml->complex_spot())
			spot->SetTimerFinish(t->m_timer_finish);
}

// A task spot type that this machine's map_spots.xml does not have (another mod set, a DXML type behind an MCM
// option) would R_ASSERT in CMapLocation::LoadSpot when the task creates its spot. Such a task is kept without its
// spot (spot_type_known logs the type once); the record keeps the host's type.
shared_str task_spot_type(const shared_str& type)
{
	return (!type.size() || spot_type_known(type)) ? type : shared_str();
}

// Where a task's spot is set (not on every retry): a task left without its spot is logged.
void note_spotless_task(const shared_str& task_id, const shared_str& type, const shared_str& spot)
{
	if (spot != type)
		::Msg("! [coop] story: task '%s' is shown without its map spot: type '%s' is unknown here", cs(task_id), cs(type));
}
} // namespace

void host_on_task_give(PeerId from, Reader& r)
{
	const shared_str id = to_shared(r.str());
	const u16 giver = r.u16();
	const u32 time_to_complete = r.u32();
	const u32 timer_ttl = r.u32();
	TaskRec rec;
	const bool has_rec = read_task(r, rec);
	if (!id.size() || is_local_task(id))
		return; // a personal task of the host is not given out
	++g_stats.requests_in;
	CGameTaskManager& m = tasks();
	if (CGameTask* t = m.HasGameTask(id, true))
	{
		if (may_see(from, id)) // `from` is the transport's sender
			send_task_to(from, t); // already running: the client just catches up
		return; // running and not his to see (its audience): nothing, he learns nothing of it
	}
	{
		ExecFor ef(from);
		lua_note("taker", id, from);
		const xr_string res = task_manager_call(kGiveTask, cs(id), giver, giver != kNoId);
		if (res == "nolua" && has_rec)
		{
			// No script task manager: build the engine task from the client's fields.
			CGameTask* t = xr_new<CGameTask>();
			t->m_ID = id;
			set_task_texts(t, rec);
			t->m_map_hint = rec.hint;
			t->m_map_location = task_spot_type(rec.map_location); // the client's type may be unknown here
			note_spotless_task(id, rec.map_location, t->m_map_location);
			t->m_map_object_id = rec.map_object_id;
			if (!m.GiveGameTaskToActor(t, time_to_complete, false, timer_ttl))
				xr_delete(t);
		}
		host_scan_tasks(); // broadcast now, with the requester as origin
	}
	const PeerInfo* p = mgr().session().peer(from);
	if (m.HasGameTask(id, true))
		::Msg("[coop] story: task '%s' given at the request of '%s'", cs(id), p ? p->nick.c_str() : "?");
	else
		::Msg("! [coop] story: host scripts did not give task '%s' asked by '%s'", cs(id), p ? p->nick.c_str() : "?");
}

void host_on_task_state(PeerId from, Reader& r)
{
	const shared_str id = to_shared(r.str());
	const u64 receive = r.u64();
	const u8 state = r.u8();
	const bool explicit_call = r.boolean();
	if (!r.ok() || !id.size() || is_local_task(id))
		return;
	++g_stats.requests_in;
	CGameTask* t = find_task(TaskKey{id, receive});
	if (!t)
		t = tasks().HasGameTask(id, true);
	if (!t)
		return;
	if (!may_see(from, t->m_ID))
		return; // not his to see (its audience): he can neither finish it nor learn of it
	if (t->GetTaskState() != eTaskStateInProgress)
	{
		send_task_to(from, t); // the client is behind
		return;
	}
	if (state != eTaskStateCompleted && state != eTaskStateFail)
		return;
	// The host's checks (coop_trust: "log" lets it through), before anything of the request is done, explicit or not.
	// `bridge`: the word of a connection that is no player (a map server's logic), not a player's (asked of the session)
	if (!lua_task_state_ok(t->m_ID, state, from, no_player_connection(from), state == eTaskStateFail && task_time_ran_out(t), explicit_call))
	{
		send_task_to(from, t); // refused: his view back
		return;
	}
	g_finisher[id] = from;
	lua_note("finisher", id, from); // the reward goes to this player
	const PeerInfo* p = mgr().session().peer(from);
	::Msg("[coop] story: '%s' %s task '%s'", p ? p->nick.c_str() : "?", state == eTaskStateCompleted ? "completed" : "failed",
	    cs(id));
	if (!explicit_call && task_manager_call(kTaskState, cs(id), state, true) == "ok")
		return; // the host's own task check finishes it next frame, with its scripts' rewards and news
	ExecFor ef(from);
	tasks().SetTaskState(t, ETaskState(state));
	host_scan_tasks();
}

// --- client ---
xr_map<TaskKey, TaskRec> g_client_recs; // last host state per task
xr_set<TaskKey> g_known; // tasks this client's scripts track (had them at join, or asked for them)
xr_map<shared_str, CGameTask*> g_pending; // tasks our scripts asked for, waiting for the host
xr_map<std::pair<shared_str, u16>, u64> g_state_requests; // (id, state) -> sent at
u64 g_last_reconcile = 0;

namespace
{
// The local tasks by key for one reconcile, its memory kept between them. A stable sort keeps tasks with equal keys in the
// list's order: the first is find_task's, the last is the one a map built from the list holds.
struct IndexedTask
{
	TaskKey key;
	CGameTask* task;
};
struct IndexOrder
{
	bool operator()(const IndexedTask& a, const IndexedTask& b) const { return a.key < b.key; }
	bool operator()(const IndexedTask& a, const TaskKey& b) const { return a.key < b; }
	bool operator()(const TaskKey& a, const IndexedTask& b) const { return a < b.key; }
};
xr_vector<IndexedTask> g_task_index;

void set_state_silent(CGameTask* t, u16 state)
{
	if (state != eTaskStateFail && state != eTaskStateCompleted)
		return;
	Applying guard(Kind::Task, t->m_ID);
	CGameTask* prev = g_silent_task;
	g_silent_task = t;
	tasks().SetTaskState(t, ETaskState(state));
	g_silent_task = prev;
}

// Removes a task the host does not have. The object is kept alive: scripts may still hold it.
void detach_task(CGameTask* t)
{
	vGameTasks& v = tasks().GetGameTasks();
	for (auto it = v.begin(); it != v.end(); ++it)
	{
		if (it->game_task != t)
			continue;
		{
			Applying guard(Kind::Task, t->m_ID);
			t->RemoveMapLocations(false);
		}
		if (g_active_task_id == t->m_ID)
			g_active_task_id = "";
		g_known.erase(TaskKey{t->m_ID, t->m_ReceiveTime});
		v.erase(it);
		++g_stats.tasks_detached;
		return;
	}
}

// Puts host record `rec` on its task here, `t` (find_task of its key, the caller's lookup), or makes the task when there is none.
void apply_record(TaskRec& rec, bool fresh, CGameTask* t)
{
	CGameTaskManager& m = tasks();
	const u16 obj = local_id_of(rec.map_object_id);
	const u64 now = mgr().now_ms();
	Applying guard(Kind::Task, rec.id);
	if (!t)
	{
		const shared_str active = g_active_task_id; // before detach_task: the copy may be the active one
		// Another running copy of the same id would block the engine: the host's view wins.
		if (rec.state == eTaskStateInProgress)
			if (CGameTask* other = m.HasGameTask(rec.id, true))
				detach_task(other);
		bool own = false;
		auto p = g_pending.find(rec.id);
		if (p != g_pending.end() && rec.state == eTaskStateInProgress)
		{
			t = p->second; // the object our scripts created: they keep working with the real task
			g_pending.erase(p);
			own = true;
		}
		else
		{
			t = xr_new<CGameTask>();
			t->m_ID = rec.id;
		}
		set_task_texts(t, rec);
		t->m_map_hint = rec.hint;
		t->m_map_location = task_spot_type(rec.map_location); // no spot of a type unknown here
		note_spotless_task(rec.id, rec.map_location, t->m_map_location);
		t->m_map_object_id = obj;
		if (rec.state == eTaskStateInProgress)
		{
			if (!m.GiveGameTaskToActor(t, 0, false, 0))
			{
				::Msg("! [coop] story: cannot add task '%s' from the host", cs(rec.id));
				return;
			}
			// The engine makes every new task the active one. The active task is each player's own: another player's
			// task leaves it alone (a task of his own that came through coop_tasks is made active there).
			if (!own && rec.origin != mgr().self() && active.size() && m.HasGameTask(active, true))
				g_active_task_id = active;
		}
		else
		{
			// Finished: straight into the list, silently (no spot, no callbacks, no active-task change).
			m.GetGameTasks().push_back(SGameTaskKey(rec.id));
			m.GetGameTasks().back().game_task = t;
			CGameTask* prev = g_silent_task;
			g_silent_task = t;
			t->SetTaskState(ETaskState(rec.state));
			g_silent_task = prev;
		}
		t->m_ReceiveTime = rec.receive;
		t->m_TimeToComplete = rec.to_complete;
		t->m_timer_finish = rec.timer_finish;
		t->m_FinishTime = rec.finish;
		fix_spot_timer(t);
		if (own)
			g_known.insert(rec.key());
		rec.next_map_retry = now + kMapRetryMs;
		++g_stats.tasks_created;
		return;
	}

	set_task_texts(t, rec);
	if (t->m_TimeToComplete != rec.to_complete)
		t->m_TimeToComplete = rec.to_complete;
	if (t->m_timer_finish != rec.timer_finish)
	{
		t->m_timer_finish = rec.timer_finish;
		fix_spot_timer(t);
	}
	if (!fresh && now < rec.next_map_retry)
		return;
	rec.next_map_retry = now + kMapRetryMs;
	if (rec.state == eTaskStateInProgress)
	{
		// Compared with what this machine can show: a task without its (unknown) spot is not redone each retry.
		const shared_str spot = task_spot_type(rec.map_location);
		const bool moved = t->m_map_location != spot || t->m_map_object_id != obj;
		if (t->GetTaskState() != eTaskStateInProgress || moved)
		{
			t->m_map_hint = rec.hint;
			note_spotless_task(rec.id, rec.map_location, spot);
			t->ChangeMapLocation(spot.c_str(), obj); // also brings it back in progress
			fix_spot_timer(t);
		}
		else if (t->m_map_hint != rec.hint)
		{
			t->m_map_hint = rec.hint;
			if (CMapLocation* ml = t->LinkedMapLocation())
				ml->SetHint(rec.hint);
		}
	}
	else if (u16(t->GetTaskState()) != rec.state)
	{
		set_state_silent(t, rec.state);
		t->m_FinishTime = rec.finish;
		g_state_requests.erase(std::make_pair(rec.id, rec.state));
	}
}

void apply_record(TaskRec& rec, bool fresh)
{
	if (in_game() && rec.id.size())
		apply_record(rec, fresh, find_task(rec.key()));
}
} // namespace

void client_on_task_upsert(Reader& r)
{
	TaskRec rec;
	if (!read_task(r, rec) || is_local_task(rec.id))
		return; // our personal task id: never mirrored
	TaskRec& slot = g_client_recs[rec.key()];
	slot = rec;
	apply_record(slot, true);
	++g_stats.tasks_in;
}

void client_on_task_list(Reader& r)
{
	const u32 n = r.u32();
	xr_vector<TaskRec> recs;
	recs.reserve(_min<u32>(n, 4096));
	for (u32 i = 0; i < n && r.ok(); ++i)
	{
		TaskRec rec;
		if (read_task(r, rec) && !is_local_task(rec.id))
			recs.push_back(rec);
	}
	if (!r.ok() || !in_game())
		return;
	g_client_recs.clear();
	for (const TaskRec& rec : recs)
		g_client_recs[rec.key()] = rec;
	xr_vector<CGameTask*> strays;
	for (const SGameTaskKey& k : tasks().GetGameTasks())
		if (k.game_task && !is_local_task(k.task_id) &&
			!g_client_recs.count(TaskKey{k.task_id, k.game_task->m_ReceiveTime}))
			strays.push_back(k.game_task);
	for (CGameTask* t : strays)
		detach_task(t);
	for (auto& it : g_client_recs)
		apply_record(it.second, true);
	if (CurrentGameUI())
		CurrentGameUI()->UpdatePda();
	++g_stats.tasks_in;
	::Msg("[coop] story: task list resynced from the host: %u tasks, %u removed here", (u32)recs.size(), (u32)strays.size());
}

// Puts the host values back over whatever the local scripts wrote.
void client_reconcile(bool full)
{
	if (!in_game() || g_client_recs.empty())
		return;
	g_last_reconcile = mgr().now_ms();
	const vGameTasks& list = tasks().GetGameTasks();
	g_task_index.clear();
	for (const SGameTaskKey& k : list)
		if (k.game_task)
			g_task_index.push_back(IndexedTask{TaskKey{k.task_id, k.game_task->m_ReceiveTime}, k.game_task});
	std::stable_sort(g_task_index.begin(), g_task_index.end(), IndexOrder());
	bool index_valid = true;
	const u64 now = mgr().now_ms();
	for (auto& it : g_client_recs)
	{
		TaskRec& rec = it.second;
		CGameTask* first = nullptr;
		CGameTask* last = nullptr;
		if (index_valid)
		{
			const auto found = std::equal_range(g_task_index.begin(), g_task_index.end(), it.first, IndexOrder());
			if (found.first != found.second)
			{
				first = found.first->task;
				last = (found.second - 1)->task;
			}
		}
		else
		{
			if (!in_game())
				break;
			first = find_task(it.first, &last);
		}
		if (last && (last->m_Title != rec.title || last->m_Description != rec.descr ||
			last->m_icon_texture_name != rec.icon || last->m_priority != rec.priority))
			set_task_texts(last, rec);
		if (full || now >= rec.next_map_retry)
		{
			// apply_record can run callbacks that change which tasks are in the list without changing its size.
			index_valid = false;
			apply_record(rec, !first, first);
		}
	}
	g_task_index.clear();
}
// ------------------------------------------------------------------ hooks: tasks

bool on_give_task(CGameTask* t, u32 time_to_complete, u32 timer_ttl)
{
	if (!t || !t->m_ID.size() || !client_live())
		return false;
	if (is_local_task(t->m_ID))
	{
		++g_local_task_edits; // personal task: given here, the host never hears of it
		return false;
	}
	if (applying_this(Kind::Task, t->m_ID))
		return false;
	if (tasks().HasGameTask(t->m_ID, true))
		return false; // the engine refuses a running task itself
	g_pending[t->m_ID] = t; // a previous object for the same id stays alive for the scripts that hold it
	if (applying_any())
	{
		++g_stats.derived_dropped;
		return true;
	}
	u16 giver = kNoId;
	if (g_actor)
		if (CInventoryOwner* partner = g_actor->GetTalkPartner())
			giver = host_id_of(partner->object_id());
	TaskRec rec;
	capture(t, rec);
	Writer w(256);
	w.u8(TaskReqGive);
	w.str(cs(t->m_ID));
	w.u16(giver);
	w.u32(time_to_complete);
	w.u32(timer_ttl);
	write_task(w, rec);
	push_out(Msg::TaskUpdate, std::move(w));
	++g_stats.requests_out;
	::Msg("[coop] story: asked the host for task '%s'", cs(t->m_ID));
	return true;
}

bool on_task_state(CGameTask* t, int state, bool explicit_call)
{
	if (!t || !client_live())
		return false;
	if (is_local_task(t->m_ID))
	{
		++g_local_task_edits;
		return false;
	}
	if (applying_this(Kind::Task, t->m_ID))
		return false;
	if (applying_any())
	{
		++g_stats.derived_dropped;
		return true;
	}
	if (state != eTaskStateCompleted && state != eTaskStateFail)
		return true; // only the host brings a task back
	const auto key = std::make_pair(t->m_ID, u16(state));
	const u64 now = mgr().now_ms();
	auto it = g_state_requests.find(key);
	if (it != g_state_requests.end() && now - it->second < kRequestRetryMs)
		return true; // asked already, waiting for the host
	g_state_requests[key] = now;
	Writer w(64);
	w.u8(TaskReqState);
	w.str(cs(t->m_ID));
	w.u64(t->m_ReceiveTime);
	w.u8(u8(state));
	w.boolean(explicit_call);
	push_out(Msg::TaskUpdate, std::move(w));
	++g_stats.requests_out;
	return true;
}

bool skip_task_check(CGameTask* t)
{
	if (!t || !mgr().active() || mode().decides_world())
		return false;
	if (!mgr().is_live_client())
		return true; // still loading the host's world
	if (is_local_task(t->m_ID))
		return false; // personal task: its checks run here
	return g_known.count(TaskKey{t->m_ID, t->m_ReceiveTime}) == 0;
}

bool task_state_silent(CGameTask* t)
{
	if (!t || t != g_silent_task)
		return false;
	const ETaskState s = t->GetTaskState();
	if (s == eTaskStateFail || s == eTaskStateCompleted)
		t->RemoveMapLocations(false);
	return true;
}

bool on_task_map_change(CGameTask* t)
{
	if (!t || !mgr().is_live_client() || is_local_task(t->m_ID))
		return false;
	if (applying_this(Kind::Task, t->m_ID))
		return false;
	++g_stats.task_edits_dropped;
	return true;
}
u32 task_finisher(LPCSTR task_id)
{
	if (!task_id)
		return kInvalidPeer;
	auto it = g_finisher.find(shared_str(task_id));
	return it == g_finisher.end() ? kInvalidPeer : it->second;
}
bool set_local_task(LPCSTR id, bool on)
{
	if (!id || !*id || xr_strlen(id) > 256)
		return false;
	const shared_str key(id);
	if (!on)
	{
		g_local_tasks.erase(key);
		return true;
	}
	if (!g_local_tasks.count(key) && g_local_tasks.size() >= kMaxLocalTasks)
	{
		::Msg("! [coop] story: too many personal tasks, '%s' refused", id);
		return false;
	}
	g_local_tasks.insert(key);
	// A host copy mirrored under this id is not ours to keep in step any more.
	for (auto it = g_client_recs.begin(); it != g_client_recs.end();)
		it = it->first.id == key ? g_client_recs.erase(it) : std::next(it);
	return true;
}

bool set_task_audience(LPCSTR id, const u16* members, u32 count)
{
	if (!mode().decides_world() || !id || !task_audience::id_ok(id) || count > task_audience::kMaxMembers || (count && !members))
		return false;
	const u32 before = u32(g_audience.size());
	switch (g_audience.set(id, std::vector<u16>(members, members + count)))
	{
	case task_audience::Table::Set::Unchanged: return true;
	case task_audience::Table::Set::Refused:
		if (g_audience.size() >= task_audience::kMaxEntries && ++g_audience_refused <= 10)
			::Msg("! [coop] story: task audiences are full (%u), '%s' stays everybody's", before, id);
		return false;
	default: break;
	}
	const task_audience::Members* m = g_audience.find(id);
	::Msg("[coop] story: task '%s' is seen by %u character(s) only (%u task(s) with an audience)", id, m ? u32(m->size()) : 0u,
	    u32(g_audience.size()));
	return true;
}

bool clear_task_audience(LPCSTR id)
{
	if (!mode().decides_world() || !id || !task_audience::id_ok(id))
		return false;
	if (g_audience.remove(id))
		::Msg("[coop] story: task '%s' is seen by everybody again (%u task(s) with an audience)", id, u32(g_audience.size()));
	return true;
}

bool resend_tasks(u32 peer)
{
	if (!mode().decides_world() || peer == kHostPeer || !ready())
		return false;
	// one of the session's players only: a map server's connection is in no peers() (no bridge test of its own)
	const PeerInfo* p = mgr().session().peer(PeerId(peer));
	if (!p || p->state != PeerState::Joined)
		return false;
	host_flush_all(); // what is pending goes first: the list must not be followed by an older upsert
	host_send_full(PeerId(peer), DomTasks); // the list by his audience (a task he lost is detached there) and its targets
	return true;
}
} // namespace story
} // namespace coop
