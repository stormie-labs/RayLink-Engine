#include "stdafx.h"
#include "coop_story.h"
#include "coop_manager.h"
#include "coop_net.h"
#include "coop_join.h"
#include "xrCoop/coop_rules.h"
#include "xrCoop/coop_limits.h" // F2 T14 fix 1: limits::Pacer

#include "../../xrEngine/XR_IOConsole.h"
#include "../../xrEngine/xr_ioc_cmd.h"
#include "Level.h"
#include "Actor.h"
#include "entity_alive.h"
#include "InventoryOwner.h"
#include "GameTaskManager.h"
#include "GameTask.h"
#include "map_manager.h"
#include "map_location.h"
#include "map_spot.h"
#include "relation_registry.h"
#include "character_community.h"
#include "alife_registry_wrappers.h"
#include "alife_registry_container.h"
#include "alife_simulator.h"
#include "alife_object_registry.h"
#include "xrServer_Objects_ALife_Monsters.h"
#include "UIGameCustom.h"
#include "ai_space.h"
#include "script_engine.h"
#include "ui/xrUIXmlParser.h"

extern CUIXml* g_uiSpotXml; // map_location.cpp: map_spots.xml, loaded on the first map spot

int g_coop_share_rewards = 0; // console coop_share_rewards (host setting, read by coop_tasks.script)

namespace coop
{
namespace story
{
namespace
{
constexpr u16 kNoId = 0xffff;
constexpr LPCSTR kAvatarSection = "coop_player_avatar"; // player characters in the host world
constexpr u64 kHashIntervalMs = 10000;
constexpr u64 kTaskScanIntervalMs = 200;
constexpr u64 kReconcileIntervalMs = 200;
constexpr u64 kProgressIntervalMs = 1000;
constexpr u64 kRequestRetryMs = 3000;
constexpr u64 kMapRetryMs = 5000;
constexpr u64 kSpotPendingMs = 5000;
constexpr size_t kMaxDeferred = 4096;

// Story domains (StoryHash order, StoryResync mask).
enum : u8
{
	DomInfos = 1,
	DomTasks = 2,
	DomRelations = 4,
	DomSpots = 8,
	DomProgress = 16,
	DomAll = 31,
};
constexpr u32 kHashDomains = 4; // infos, tasks, relations, spots

// First byte of InfoPortion / TaskUpdate / MapSpot / Relation payloads.
enum : u8
{
	InfoGive = 1, // both directions: {str id}
	InfoDisable = 2,
	InfoFull = 3, // host -> client: {u32 n, n * str}

	TaskUpsert = 1, // host -> client: {task record}
	TaskReqGive = 2, // client -> host: {str id, u16 giver, u32 time_to_complete, u32 timer_ttl, task record}
	TaskReqState = 3, // client -> host: {str id, u64 receive_time, u8 state, u8 explicit}

	SpotAdd = 1, // both directions: {str type, u16 id, str hint, u8 serializable, u32 author}
	SpotRemove = 2, // both: {str type, u16 id}
	SpotRemoveAll = 3, // both: {u16 id}
	SpotFull = 4, // host -> client: {u32 n, n * SpotAdd body}

	RelPersonal = 1, // host -> client: {u16 from, u16 to, i32 value} absolute
	RelCommunity = 2, // host -> client: {i32 community, u16 to, i32 value} absolute
	RelCommRelation = 3, // host -> client: {i32 from, i32 to, i32 value} absolute
	RelFull = 4, // host -> client: {u32 n, personal...}{u32 n, community...}{u32 n, n*n table}
	RelReputationDelta = 5, // host -> owning client: {u16 who (host id of its proxy), i32 delta} (W3c)
	RelReqPersonalSet = 11, // client -> host: {u16 from, u16 to, i32 value}
	RelReqPersonalChange = 12, // {u16 from, u16 to, i32 delta}
	RelReqPersonalForce = 13, // {u16 from, u16 to, i32 value}
	RelReqCommunitySet = 14, // {i32 community, u16 to, i32 value}
	RelReqCommunityChange = 15, // {i32 community, u16 to, i32 delta}
	RelReqCommRelation = 16, // {i32 from, i32 to, i32 value}
};

const char* cs(const shared_str& s) { return s.c_str() ? s.c_str() : ""; }
shared_str to_shared(const std::string& s) { return s.empty() ? shared_str() : shared_str(s.c_str()); }

// R4 1: UI / sleep info portions belong to one machine (coop_rules.h): not sent, not applied, not hashed.
bool machine_info(const shared_str& id) { return rules::is_machine_info(cs(id)); }

u16 host_id_of(u16 local) { return local == kNoId ? kNoId : join::to_host(local); }
u16 local_id_of(u16 host) { return host == kNoId ? kNoId : join::to_local(host); }

bool in_game() { return g_pGameLevel && g_actor && ai().get_alife(); }
bool client_live() { return mgr().is_live_client() && in_game(); }
CGameTaskManager& tasks() { return Level().GameTaskManager(); }
CMapManager& spots() { return Level().MapManager(); }

// Order-independent hash of a set: sum of per-item FNV-1a.
struct Fnv
{
	u64 h = 14695981039346656037ull;
	void bytes(const void* data, size_t size)
	{
		const u8* p = static_cast<const u8*>(data);
		for (size_t i = 0; i < size; ++i)
		{
			h ^= p[i];
			h *= 1099511628211ull;
		}
	}
	void str(const shared_str& s)
	{
		const char* p = cs(s);
		bytes(p, xr_strlen(p) + 1);
	}
	template <typename T>
	void pod(const T& v)
	{
		bytes(&v, sizeof(v));
	}
};

struct DomainHash
{
	u32 count = 0;
	u64 hash = 0;
	void add(u64 item)
	{
		++count;
		hash += item;
	}
	bool operator==(const DomainHash& o) const { return count == o.count && hash == o.hash; }
	bool operator!=(const DomainHash& o) const { return !(*this == o); }
};

// A player character: the local actor or a proxy/avatar entity. Same answer on every machine
// for the same entity (section check, not the connection registry).
bool is_player(u16 id)
{
	if (id == 0)
		return true;
	if (id == kNoId || !ai().get_alife())
		return false;
	const CSE_ALifeDynamicObject* e = ai().alife().objects().object(id, true);
	return e && !xr_strcmp(e->s_name, kAvatarSection);
}

// ------------------------------------------------------------------ stats

struct Stats
{
	u64 infos_out = 0, infos_in = 0;
	u64 tasks_out = 0, tasks_in = 0, tasks_created = 0, tasks_detached = 0;
	u64 rel_out = 0, rel_in = 0;
	u64 spots_out = 0, spots_in = 0;
	u64 progress_out = 0, progress_in = 0;
	u64 requests_out = 0, requests_in = 0;
	u64 derived_dropped = 0; // client: script reactions to a host broadcast (the host sends its own)
	u64 action_dropped = 0; // client: combat goodwill bookkeeping (the host computes it)
	u64 task_edits_dropped = 0; // client: own script edits of task spots (the host owns them)
	u64 spot_removes_refused = 0; // host: client removals of spots it did not place (R4 3)
	u64 hashes = 0, mismatches = 0, resyncs_out = 0, resyncs_in = 0;
	u64 deferred = 0;
	u64 proxy_actions = 0; // host: RELATION_REGISTRY::Action calls with a player proxy as the actor (R-3.4)
	u64 rep_deltas_out = 0, rep_deltas_in = 0; // reputation from host-side combat bookkeeping
} g_stats;

// ------------------------------------------------------------------ applying guard

enum class Kind : u8
{
	None,
	Info,
	Task,
	Spot,
};

struct ApplyState
{
	Kind kind = Kind::None;
	shared_str key;
	u16 id = kNoId;
} g_apply;

struct Applying
{
	ApplyState m_prev;
	Applying(Kind kind, const shared_str& key, u16 id = kNoId) : m_prev(g_apply)
	{
		g_apply.kind = kind;
		g_apply.key = key;
		g_apply.id = id;
	}
	~Applying() { g_apply = m_prev; }
	Applying(const Applying&) = delete;
	Applying& operator=(const Applying&) = delete;
};

bool applying_any() { return g_apply.kind != Kind::None; }
bool applying_this(Kind kind, const shared_str& key, u16 id = kNoId)
{
	return g_apply.kind == kind && g_apply.key == key && (id == kNoId || g_apply.id == id);
}

int g_action_depth = 0; // RELATION_REGISTRY::Action nesting
CGameTask* g_silent_task = nullptr; // client: task whose host state is being applied
PeerId g_exec_peer = kInvalidPeer; // host: client whose request is being executed

struct ExecFor
{
	PeerId m_prev;
	explicit ExecFor(PeerId p) : m_prev(g_exec_peer) { g_exec_peer = p; }
	~ExecFor() { g_exec_peer = m_prev; }
	ExecFor(const ExecFor&) = delete;
	ExecFor& operator=(const ExecFor&) = delete;
};

// ------------------------------------------------------------------ outbox

// Facts recorded by engine hooks (game thread or main thread, never concurrently with frame()),
// sent in order from frame(): host -> broadcast, client -> request to the host.
struct Out
{
	Msg type = Msg::InfoPortion;
	Writer w;
	bool spot_add = false; // payload built at send time from the live spot
	shared_str spot_type;
	u16 spot_id = kNoId; // local numbering
};
xr_vector<Out> g_outbox;
// F2 T14 fix 1 (review M1, Ruling F2-14): a client sends infos and spots only as fast as the host takes them
// (kClientRateRules InfoPortion, MapSpot): the host drops what exceeds its limit unseen, and a lost info or spot never
// comes back. What waits stays queued, in order; the queue is bounded (a broken mod must not fill the memory).
constexpr size_t kMaxOutbox = 4096;
limits::Pacer g_pacer;
u64 g_out_held = 0, g_out_dropped = 0;

bool outbox_room()
{
	if (g_outbox.size() < kMaxOutbox)
		return true;
	++g_out_dropped;
	return false;
}

void push_out(Msg type, Writer&& w)
{
	if (!outbox_room()) // F2 T14 fix 1
		return;
	Out o;
	o.type = type;
	o.w = std::move(w);
	g_outbox.push_back(std::move(o));
}

void send_out(Msg type, const Writer& w)
{
	if (mgr().is_host())
		net::host_broadcast(type, w);
	else
		net::client_send(type, w);
}

// Messages that arrived before this machine was ready for them (P6).
struct Deferred
{
	Msg type;
	PeerId from;
	std::vector<u8> data;
};
xr_vector<Deferred> g_deferred;

// ------------------------------------------------------------------ info portions

xr_map<std::pair<u8, shared_str>, u64> g_info_requests; // client: (op, id) -> sent at
// Client (W5): coop.story_hold_infos is on; infos given meanwhile are kept in g_local_infos (this machine
// only, not hashed, kept by a resync) until the host gives the same info.
bool g_hold_infos = false;
xr_set<shared_str> g_local_infos;
u64 g_infos_held = 0, g_holds_leaked = 0;

KNOWN_INFO_VECTOR* actor_infos()
{
	if (!g_actor || !g_actor->m_known_info_registry)
		return nullptr;
	return &g_actor->m_known_info_registry->registry().objects();
}

DomainHash hash_infos()
{
	DomainHash d;
	if (const KNOWN_INFO_VECTOR* v = actor_infos())
	{
		for (const shared_str& id : *v)
		{
			if (machine_info(id))
				continue; // R4 1: UI / sleep infos are not part of the group story
			if (g_local_infos.count(id))
				continue; // client: held by this machine only (the host does not have it)
			Fnv f;
			f.str(id);
			d.add(f.h);
		}
	}
	return d;
}

void client_apply_info(bool give, const shared_str& id)
{
	if (!g_actor || !id.size())
		return;
	if (machine_info(id))
		return; // R4 1: never applied from the network (the host's UI is not ours)
	g_info_requests.erase(std::make_pair(u8(give ? InfoGive : InfoDisable), id));
	g_local_infos.erase(id); // the host's word: shared from now on
	Applying guard(Kind::Info, id);
	if (give)
		g_actor->OnReceiveInfo(id);
	else
		g_actor->OnDisableInfo(id);
	++g_stats.infos_in;
}

void send_info_list(PeerId to)
{
	const KNOWN_INFO_VECTOR* v = actor_infos();
	if (!v)
		return;
	xr_vector<const shared_str*> shared; // R4 1: UI / sleep infos stay on the host
	shared.reserve(v->size());
	for (const shared_str& id : *v)
		if (!machine_info(id))
			shared.push_back(&id);
	Writer w(shared.size() * 32 + 8);
	w.u8(InfoFull);
	w.u32((u32)shared.size());
	for (const shared_str* id : shared)
		w.str(cs(*id));
	net::host_send(to, Msg::InfoPortion, w);
	++g_stats.infos_out;
}

void client_apply_info_list(Reader& r)
{
	const u32 n = r.u32();
	xr_set<shared_str> host;
	for (u32 i = 0; i < n && r.ok(); ++i)
	{
		const shared_str id = to_shared(r.str());
		if (!machine_info(id)) // R4 1: the host's UI / sleep infos are not ours
			host.insert(id);
	}
	if (!r.ok() || !g_actor)
		return;
	for (auto it = g_local_infos.begin(); it != g_local_infos.end();)
		it = host.count(*it) ? g_local_infos.erase(it) : std::next(it); // the host has it now: shared
	xr_vector<shared_str> drop, add;
	if (const KNOWN_INFO_VECTOR* v = actor_infos())
	{
		xr_set<shared_str> mine;
		for (const shared_str& id : *v)
			mine.insert(id);
		for (const shared_str& id : mine)
			if (!host.count(id) && !g_local_infos.count(id) && !machine_info(id)) // R4 1: ours alone, keep
				drop.push_back(id);
		for (const shared_str& id : host)
			if (id.size() && !mine.count(id))
				add.push_back(id);
	}
	for (const shared_str& id : drop)
		client_apply_info(false, id);
	for (const shared_str& id : add)
		client_apply_info(true, id);
	::Msg("[coop] story: info portions resynced from the host: %u total, %u added, %u removed", n, (u32)add.size(),
	    (u32)drop.size());
}

// ------------------------------------------------------------------ tasks

struct TaskKey
{
	shared_str id;
	u64 receive = 0; // the same task id can be given again later (repeatable tasks)
	bool operator<(const TaskKey& o) const { return id == o.id ? receive < o.receive : id < o.id; }
};

// Replicated engine fields of a CGameTask (what the host computed; R-7.3).
struct TaskRec
{
	shared_str id;
	u16 state = u16(eTaskStateDummy);
	u16 type = u16(eTaskTypeDummy);
	shared_str title, descr, icon, hint, map_location;
	u16 map_object_id = kNoId; // host numbering
	u32 priority = 0;
	u64 receive = 0, finish = 0, to_complete = 0, timer_finish = 0;
	u32 origin = kHostPeer; // peer whose request created it
	// client bookkeeping
	u64 next_map_retry = 0;

	TaskKey key() const { return TaskKey{id, receive}; }
	bool same(const TaskRec& o) const
	{
		return id == o.id && state == o.state && type == o.type && title == o.title && descr == o.descr &&
			icon == o.icon && hint == o.hint && map_location == o.map_location && map_object_id == o.map_object_id &&
			priority == o.priority && receive == o.receive && finish == o.finish && to_complete == o.to_complete &&
			timer_finish == o.timer_finish;
	}
};

// Personal tasks (coop.story_local_task, W5): this machine only. Process lifetime, like addon task ids.
constexpr size_t kMaxLocalTasks = 1024;
xr_set<shared_str> g_local_tasks;
u64 g_local_task_edits = 0; // engine task hooks that let a personal task through locally

bool is_local_task(const shared_str& id) { return id.size() && g_local_tasks.count(id) != 0; }

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

CGameTask* find_task(const TaskKey& key)
{
	for (const SGameTaskKey& k : tasks().GetGameTasks())
		if (k.game_task && k.task_id == key.id && k.game_task->m_ReceiveTime == key.receive)
			return k.game_task;
	return nullptr;
}

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
		TaskRec cur;
		capture(t, cur);
		auto it = g_host_sent.find(cur.key());
		if (it != g_host_sent.end())
		{
			if (it->second.same(cur))
				continue;
			cur.origin = it->second.origin;
		}
		else
			cur.origin = g_exec_peer != kInvalidPeer ? g_exec_peer : kHostPeer;
		g_host_sent[cur.key()] = cur;
		if (prime)
			continue;
		Writer w(256);
		w.u8(TaskUpsert);
		write_task(w, cur);
		net::host_broadcast(Msg::TaskUpdate, w);
		++g_stats.tasks_out;
	}
}

void send_task_to(PeerId to, const CGameTask* t)
{
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

void send_task_list(PeerId to)
{
	if (!in_game())
		return;
	const vGameTasks& v = tasks().GetGameTasks();
	u32 n = 0;
	for (const SGameTaskKey& k : v)
		if (!is_local_task(k.task_id))
			++n;
	Writer w(v.size() * 256 + 8);
	w.u32(n);
	for (const SGameTaskKey& k : v)
	{
		if (is_local_task(k.task_id))
			continue; // the host's personal task
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

// Runs a small Lua chunk with (string, integer|nil) arguments on this machine's script engine.
// Returns its string result, "error" on a Lua error.
xr_string run_lua(LPCSTR name, LPCSTR chunk, LPCSTR arg, int num, bool has_num)
{
	lua_State* L = ai().script_engine().lua();
	if (!L)
		return "error";
	const int top = lua_gettop(L);
	if (luaL_loadbuffer(L, chunk, xr_strlen(chunk), name))
	{
		::Msg("! [coop] story: %s does not compile: %s", name, lua_tostring(L, -1));
		lua_settop(L, top);
		return "error";
	}
	lua_pushstring(L, arg);
	if (has_num)
		lua_pushinteger(L, num);
	else
		lua_pushnil(L);
	if (lua_pcall(L, 2, 1, 0))
	{
		::Msg("! [coop] story: %s failed: %s", name, lua_tostring(L, -1));
		lua_settop(L, top);
		return "error";
	}
	xr_string res = lua_isstring(L, -1) ? lua_tostring(L, -1) : "";
	lua_settop(L, top);
	return res;
}

// Tells coop_tasks.script (W3c) which player took (kind "taker") or finished (kind "finisher") a task that
// a client asked for through these engine requests, so the reward goes to that player (R-7.6).
// Missing script or function: nothing happens.
constexpr LPCSTR kLuaNote = "local id, kind, peer = ...\n"
							"local ok, m = pcall(function() return coop_tasks end)\n"
							"if ok and type(m) == 'table' and type(m.host_note) == 'function' then pcall(m.host_note, id, kind, peer) end\n"
							"return 'ok'\n";

void lua_note(LPCSTR kind, const shared_str& task_id, PeerId peer)
{
	lua_State* L = ai().script_engine().lua();
	if (!L || !task_id.size())
		return;
	const int top = lua_gettop(L);
	if (luaL_loadbuffer(L, kLuaNote, xr_strlen(kLuaNote), "@coop_story_note") == 0)
	{
		lua_pushstring(L, cs(task_id));
		lua_pushstring(L, kind);
		lua_pushnumber(L, lua_Number(peer));
		if (lua_pcall(L, 3, 1, 0))
			::Msg("! [coop] story: task note failed: %s", lua_tostring(L, -1));
	}
	lua_settop(L, top);
}

// The host runs Anomaly's own task manager for a client's request, so its scripts track the task
// (texts, targets, completion) exactly as if the host had taken it.
constexpr LPCSTR kLuaGiveTask = "local id, giver = ...\n"
								"local ok, tm = pcall(function() return task_manager and task_manager.get_task_manager and task_manager.get_task_manager() end)\n"
								"if not ok or not tm or not tm.give_task then return 'nolua' end\n"
								"tm:give_task(id, giver)\n"
								"return 'ok'\n";
constexpr LPCSTR kLuaTaskState = "local id, st = ...\n"
								 "local ok, tm = pcall(function() return task_manager and task_manager.get_task_manager and task_manager.get_task_manager() end)\n"
								 "if not ok or not tm or not tm.task_info then return 'nolua' end\n"
								 "if not tm.task_info[id] then return 'unknown' end\n"
								 "if st == 2 then tm:set_task_completed(id) else tm:set_task_failed(id) end\n"
								 "return 'ok'\n";

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

bool spot_type_known(const shared_str& type); // map spots section below

// R4 5: a task spot type that this machine's map_spots.xml does not have (another mod set, a DXML type behind an MCM
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
		send_task_to(from, t); // already running: the client just catches up
		return;
	}
	{
		ExecFor ef(from);
		lua_note("taker", id, from);
		const xr_string res = run_lua("@coop_story_give_task", kLuaGiveTask, cs(id), giver, giver != kNoId);
		if (res == "nolua" && has_rec)
		{
			// No script task manager: build the engine task from the client's fields.
			CGameTask* t = xr_new<CGameTask>();
			t->m_ID = id;
			set_task_texts(t, rec);
			t->m_map_hint = rec.hint;
			t->m_map_location = task_spot_type(rec.map_location); // R4 5: the client's type may be unknown here
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
	if (t->GetTaskState() != eTaskStateInProgress)
	{
		send_task_to(from, t); // the client is behind
		return;
	}
	if (state != eTaskStateCompleted && state != eTaskStateFail)
		return;
	g_finisher[id] = from;
	lua_note("finisher", id, from); // the reward goes to this player (R-7.6)
	const PeerInfo* p = mgr().session().peer(from);
	::Msg("[coop] story: '%s' %s task '%s'", p ? p->nick.c_str() : "?", state == eTaskStateCompleted ? "completed" : "failed",
	    cs(id));
	if (!explicit_call && run_lua("@coop_story_task_state", kLuaTaskState, cs(id), state, true) == "ok")
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

void apply_record(TaskRec& rec, bool fresh)
{
	if (!in_game() || !rec.id.size())
		return;
	CGameTaskManager& m = tasks();
	const u16 obj = local_id_of(rec.map_object_id);
	const u64 now = mgr().now_ms();
	Applying guard(Kind::Task, rec.id);
	CGameTask* t = find_task(rec.key());
	if (!t)
	{
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
		t->m_map_location = task_spot_type(rec.map_location); // R4 5: no spot of a type unknown here
		note_spotless_task(rec.id, rec.map_location, t->m_map_location);
		t->m_map_object_id = obj;
		if (rec.state == eTaskStateInProgress)
		{
			if (!m.GiveGameTaskToActor(t, 0, false, 0))
			{
				::Msg("! [coop] story: cannot add task '%s' from the host", cs(rec.id));
				return;
			}
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
		// R4 5: compared with what this machine can show: a task without its (unknown) spot is not redone each retry.
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

// Puts the host values back over whatever the local scripts wrote (R-7.3).
void client_reconcile(bool full)
{
	if (!in_game() || g_client_recs.empty())
		return;
	g_last_reconcile = mgr().now_ms();
	xr_map<TaskKey, CGameTask*> local;
	for (const SGameTaskKey& k : tasks().GetGameTasks())
		if (k.game_task)
			local[TaskKey{k.task_id, k.game_task->m_ReceiveTime}] = k.game_task;
	const u64 now = mgr().now_ms();
	for (auto& it : g_client_recs)
	{
		TaskRec& rec = it.second;
		auto l = local.find(it.first);
		if (l == local.end())
		{
			if (full || now >= rec.next_map_retry)
				apply_record(rec, true); // lost here: bring it back
			continue;
		}
		CGameTask* t = l->second;
		if (t->m_Title != rec.title || t->m_Description != rec.descr || t->m_icon_texture_name != rec.icon ||
			t->m_priority != rec.priority)
			set_task_texts(t, rec);
		if (full || now >= rec.next_map_retry)
			apply_record(rec, false);
	}
}

// ------------------------------------------------------------------ relations

struct RelKey
{
	u8 kind; // RelPersonal / RelCommunity / RelCommRelation
	int a, b; // (from, to) / (community, to) / (community, community)
	bool operator<(const RelKey& o) const
	{
		if (kind != o.kind)
			return kind < o.kind;
		return a != o.a ? a < o.a : b < o.b;
	}
};
xr_set<RelKey> g_rel_dirty; // host: keys whose current value must be broadcast
xr_map<u16, int> g_rep_delta; // host: proxy -> reputation change from attacks / fight help (sent in frame)

RELATION_DATA& rel_data(u16 id) { return RELATION_REGISTRY::relation_registry().registry().objects(id); }

const CRelationRegistry::OBJECT_REGISTRY* rel_all()
{
	if (!ai().get_alife())
		return nullptr;
	return &ai().alife().registry().get<CRelationRegistry>().objects();
}

bool community_ok(int c) { return c >= 0 && c <= int(CHARACTER_COMMUNITY::GetMaxIndex()); }

int rel_value(const RelKey& k)
{
	switch (k.kind)
	{
	case RelPersonal: return RELATION_REGISTRY().GetGoodwill(u16(k.a), u16(k.b));
	case RelCommunity: return RELATION_REGISTRY().GetCommunityGoodwill(k.a, u16(k.b));
	default: return RELATION_REGISTRY().GetCommunityRelation(k.a, k.b);
	}
}

void host_flush_relations()
{
	if (g_rel_dirty.empty() || !ai().get_alife())
		return;
	xr_set<RelKey> dirty;
	dirty.swap(g_rel_dirty);
	for (const RelKey& k : dirty)
	{
		Writer w(16);
		w.u8(k.kind);
		if (k.kind == RelPersonal)
		{
			w.u16(u16(k.a));
			w.u16(u16(k.b));
		}
		else if (k.kind == RelCommunity)
		{
			w.i32(k.a);
			w.u16(u16(k.b));
		}
		else
		{
			w.i32(k.a);
			w.i32(k.b);
		}
		w.i32(rel_value(k));
		net::host_broadcast(Msg::Relation, w);
		++g_stats.rel_out;
	}
}

void host_add_reported_reputation(PeerId peer, int delta); // progress section

// Host: the engine changed a player's reputation on its proxy (Action: attack, fight help). The player's
// machine owns its reputation (the proxy's value is put back from its reports), so the change goes to it.
void host_flush_rep_deltas()
{
	if (g_rep_delta.empty())
		return;
	xr_map<u16, int> list;
	list.swap(g_rep_delta);
	for (const auto& it : list)
	{
		const PeerId peer = join::peer_of_proxy(it.first);
		if (peer == kInvalidPeer || peer == kHostPeer || !it.second)
			continue;
		host_add_reported_reputation(peer, it.second);
		Writer w(16);
		w.u8(RelReputationDelta);
		w.u16(it.first);
		w.i32(it.second);
		net::host_send(peer, Msg::Relation, w);
		++g_stats.rep_deltas_out;
	}
}

// Raw writes on a client: the value is the host's, already clamped; no hooks run.
void client_set_personal(u16 host_from, u16 host_to, int v)
{
	rel_data(local_id_of(host_from)).personal[local_id_of(host_to)].SetGoodwill(v);
}
void client_set_community(int community, u16 host_to, int v)
{
	if (community_ok(community))
		rel_data(local_id_of(host_to)).communities[community].SetGoodwill(v);
}
void client_set_comm_relation(int a, int b, int v)
{
	if (community_ok(a) && community_ok(b))
		CHARACTER_COMMUNITY::set_relation(a, b, v);
}

DomainHash hash_relations()
{
	DomainHash d;
	const CRelationRegistry::OBJECT_REGISTRY* all = rel_all();
	if (!all)
		return d;
	for (const auto& it : *all)
	{
		const u16 from = it.first;
		const bool from_player = is_player(from);
		for (const auto& p : it.second.personal)
		{
			if (p.second.Goodwill() == NEUTRAL_GOODWILL || !(from_player || is_player(p.first)))
				continue;
			Fnv f;
			f.pod(u8(RelPersonal));
			f.pod(host_id_of(from));
			f.pod(host_id_of(p.first));
			f.pod(p.second.Goodwill());
			d.add(f.h);
		}
		if (!from_player)
			continue;
		for (const auto& c : it.second.communities)
		{
			if (c.second.Goodwill() == NEUTRAL_GOODWILL)
				continue;
			Fnv f;
			f.pod(u8(RelCommunity));
			f.pod(int(c.first));
			f.pod(host_id_of(from));
			f.pod(c.second.Goodwill());
			d.add(f.h);
		}
	}
	const int n = int(CHARACTER_COMMUNITY::GetMaxIndex()) + 1;
	for (int a = 0; a < n; ++a)
		for (int b = 0; b < n; ++b)
		{
			Fnv f;
			f.pod(u8(RelCommRelation));
			f.pod(a);
			f.pod(b);
			f.pod(RELATION_REGISTRY().GetCommunityRelation(a, b));
			d.add(f.h);
		}
	return d;
}

void send_relation_list(PeerId to)
{
	const CRelationRegistry::OBJECT_REGISTRY* all = rel_all();
	if (!all)
		return;
	struct P
	{
		u16 from, to;
		int v;
	};
	struct C
	{
		int comm;
		u16 to;
		int v;
	};
	xr_vector<P> personal;
	xr_vector<C> community;
	for (const auto& it : *all)
	{
		const bool from_player = is_player(it.first);
		for (const auto& p : it.second.personal)
			if (p.second.Goodwill() != NEUTRAL_GOODWILL && (from_player || is_player(p.first)))
				personal.push_back(P{it.first, p.first, p.second.Goodwill()});
		if (from_player)
			for (const auto& c : it.second.communities)
				if (c.second.Goodwill() != NEUTRAL_GOODWILL)
					community.push_back(C{c.first, it.first, c.second.Goodwill()});
	}
	const int n = int(CHARACTER_COMMUNITY::GetMaxIndex()) + 1;
	Writer w(personal.size() * 8 + community.size() * 10 + n * n * 4 + 16);
	w.u8(RelFull);
	w.u32((u32)personal.size());
	for (const P& p : personal)
	{
		w.u16(p.from);
		w.u16(p.to);
		w.i32(p.v);
	}
	w.u32((u32)community.size());
	for (const C& c : community)
	{
		w.i32(c.comm);
		w.u16(c.to);
		w.i32(c.v);
	}
	w.u32(u32(n));
	for (int a = 0; a < n; ++a)
		for (int b = 0; b < n; ++b)
			w.i32(RELATION_REGISTRY().GetCommunityRelation(a, b));
	net::host_send(to, Msg::Relation, w);
	++g_stats.rel_out;
}

void client_apply_relation_list(Reader& r)
{
	struct P
	{
		u16 from, to;
		int v;
	};
	struct C
	{
		int comm;
		u16 to;
		int v;
	};
	xr_vector<P> personal;
	xr_vector<C> community;
	const u32 np = r.u32();
	for (u32 i = 0; i < np && r.ok(); ++i)
	{
		P p;
		p.from = r.u16();
		p.to = r.u16();
		p.v = r.i32();
		personal.push_back(p);
	}
	const u32 nc = r.u32();
	for (u32 i = 0; i < nc && r.ok(); ++i)
	{
		C c;
		c.comm = r.i32();
		c.to = r.u16();
		c.v = r.i32();
		community.push_back(c);
	}
	const u32 n = r.u32();
	xr_vector<int> table;
	if (n <= 256)
	{
		table.reserve(n * n);
		for (u32 i = 0; i < n * n && r.ok(); ++i)
			table.push_back(r.i32());
	}
	if (!r.ok() || !ai().get_alife())
		return;

	// Replace every player-related entry; NPC-to-NPC relations stay the host world's business.
	xr_vector<u16> ids;
	if (const CRelationRegistry::OBJECT_REGISTRY* all = rel_all())
		for (const auto& it : *all)
			ids.push_back(it.first);
	for (u16 id : ids)
	{
		RELATION_DATA& d = rel_data(id);
		if (is_player(id))
		{
			d.personal.clear();
			d.communities.clear();
			continue;
		}
		for (auto it = d.personal.begin(); it != d.personal.end();)
		{
			if (is_player(it->first))
				it = d.personal.erase(it);
			else
				++it;
		}
	}
	for (const P& p : personal)
		client_set_personal(p.from, p.to, p.v);
	for (const C& c : community)
		client_set_community(c.comm, c.to, c.v);
	if (n == u32(CHARACTER_COMMUNITY::GetMaxIndex()) + 1 && table.size() == size_t(n) * n)
		for (u32 a = 0; a < n; ++a)
			for (u32 b = 0; b < n; ++b)
				client_set_comm_relation(int(a), int(b), table[a * n + b]);
	++g_stats.rel_in;
	::Msg("[coop] story: relations resynced from the host: %u personal, %u community", np, nc);
}

// Right after join the client's relation registry is the host's, in host numbering: the actor (0)
// and our proxy are swapped like the characters were (coop_join), so personal attitudes and
// community goodwill follow the right player.
void client_swap_relations()
{
	const u16 p = join::my_proxy();
	if (p == kNoId || !ai().get_alife())
		return;
	{
		RELATION_DATA mine = rel_data(p);
		RELATION_DATA host = rel_data(0);
		rel_data(0) = mine;
		rel_data(p) = host;
	}
	xr_vector<u16> ids;
	if (const CRelationRegistry::OBJECT_REGISTRY* all = rel_all())
		for (const auto& it : *all)
			ids.push_back(it.first);
	u32 swapped = 0;
	for (u16 id : ids)
	{
		PERSONAL_RELATION_MAP& m = rel_data(id).personal;
		auto i0 = m.find(0);
		auto ip = m.find(p);
		const bool h0 = i0 != m.end(), hp = ip != m.end();
		if (!h0 && !hp)
			continue;
		const SRelation r0 = h0 ? i0->second : SRelation();
		const SRelation rp = hp ? ip->second : SRelation();
		m.erase(0);
		m.erase(p);
		if (hp)
			m[0] = rp;
		if (h0)
			m[p] = r0;
		++swapped;
	}
	::Msg("[coop] story: relations swapped to this player (actor <-> proxy %u, %u personal lists)", p, swapped);
}

bool client_relation_request(u8 op, int a, int b, int v)
{
	if (g_action_depth > 0)
	{
		++g_stats.action_dropped;
		return true;
	}
	if (applying_any())
	{
		++g_stats.derived_dropped;
		return true;
	}
	Writer w(16);
	w.u8(op);
	if (op == RelReqCommunitySet || op == RelReqCommunityChange || op == RelReqCommRelation)
		w.i32(a);
	else
		w.u16(host_id_of(u16(a)));
	if (op == RelReqCommRelation)
		w.i32(b);
	else
		w.u16(host_id_of(u16(b)));
	w.i32(v);
	push_out(Msg::Relation, std::move(w));
	++g_stats.requests_out;
	return true;
}

void host_on_relation_request(PeerId from, u8 op, Reader& r)
{
	++g_stats.requests_in;
	ExecFor ef(from);
	switch (op)
	{
	case RelReqPersonalSet:
	case RelReqPersonalChange:
	case RelReqPersonalForce:
	{
		const u16 a = r.u16(), b = r.u16();
		const int v = r.i32();
		if (!r.ok() || a == kNoId || b == kNoId || !(is_player(a) || is_player(b)))
			return;
		if (op == RelReqPersonalSet)
			RELATION_REGISTRY().SetGoodwill(a, b, v);
		else if (op == RelReqPersonalChange)
			RELATION_REGISTRY().ChangeGoodwill(a, b, v);
		else
			RELATION_REGISTRY().ForceSetGoodwill(a, b, v);
		break;
	}
	case RelReqCommunitySet:
	case RelReqCommunityChange:
	{
		const int c = r.i32();
		const u16 to = r.u16();
		const int v = r.i32();
		if (!r.ok() || !community_ok(c) || !is_player(to))
			return;
		if (op == RelReqCommunitySet)
			RELATION_REGISTRY().SetCommunityGoodwill(c, to, v);
		else
			RELATION_REGISTRY().ChangeCommunityGoodwill(c, to, v);
		break;
	}
	case RelReqCommRelation:
	{
		const int a = r.i32(), b = r.i32(), v = r.i32();
		if (!r.ok() || !community_ok(a) || !community_ok(b))
			return;
		RELATION_REGISTRY().SetCommunityRelation(a, b, v);
		break;
	}
	default: break;
	}
}

// ------------------------------------------------------------------ map spots

struct SpotKey
{
	shared_str type;
	u16 id; // host numbering
	bool operator<(const SpotKey& o) const { return type == o.type ? id < o.id : type < o.type; }
};
struct SpotAuthor
{
	PeerId author = kHostPeer;
	shared_str hint; // untagged
};
xr_map<SpotKey, SpotAuthor> g_spot_authors; // host: spots placed at a client's request
xr_map<SpotKey, u64> g_spot_pending; // client: spots sent to the host, not confirmed yet

// Purely local engine spots: own position markers, hit sectors, relation markers, debug, and every
// type with the prefix "coop_" (markers the co-op scripts place per machine, e.g. player positions).
bool spot_type_local(LPCSTR type)
{
	static const char* const kLocal[] = {"actor_location", "actor_location_p", "mp_hit_sector_location", "debug_stalker",
		"deadbody_location", "friend_location", "neutral_location", "enemy_location"};
	if (!type || !*type || !strncmp(type, "coop_", 5))
		return true;
	for (const char* l : kLocal)
		if (!xr_strcmp(type, l))
			return true;
	return false;
}

// CMapLocation R_ASSERTs on a spot type that map_spots.xml does not have (another mod set, a bad request):
// check before AddMapLocation for anything that came from the network.
bool spot_type_known(const shared_str& type)
{
	if (!type.size())
		return false;
	if (!g_uiSpotXml)
	{
		g_uiSpotXml = xr_new<CUIXml>(); // the same lazy load as CMapLocation::LoadSpot
		g_uiSpotXml->Load(CONFIG_PATH, UI_PATH, "map_spots.xml");
	}
	if (g_uiSpotXml->NavigateToNode(type.c_str(), 0))
		return true;
	static xr_set<shared_str> s_logged; // R4 5: tasks and resyncs ask again every few seconds; once per type
	if (s_logged.insert(type).second)
		::Msg("! [coop] story: map spot type '%s' is unknown here (different mods?), its spots and task spots are skipped",
		    type.c_str());
	return false;
}

// Spots that belong to the shared PDA: not task-linked (tasks carry their own), not relation markers.
bool spot_replicable(CMapLocation* ml, const shared_str& type)
{
	if (!ml || spot_type_local(cs(type)) || ml->m_owner_task_id.size())
		return false;
	return smart_cast<CRelationMapLocation*>(ml) == nullptr;
}

// R4 3, host: a client may remove only a shared spot that the host placed at its own request (g_spot_authors):
// never a task spot, nor a spot of the host's or another player's scripts (both ran the same script: the host's).
bool spot_placed_by(PeerId peer, CMapLocation* ml, const shared_str& type, u16 id)
{
	if (!spot_replicable(ml, type))
		return false;
	auto it = g_spot_authors.find(SpotKey{type, id});
	return it != g_spot_authors.end() && it->second.author == peer;
}

xr_string tagged_hint(const shared_str& hint, PeerId author)
{
	xr_string h = cs(hint);
	if (author == kHostPeer || author == mgr().self() || author == kInvalidPeer)
		return h;
	const PeerInfo* p = mgr().session().peer(author);
	if (!p)
		return h;
	if (!h.empty())
		h += " ";
	h += "(";
	h += p->nick.c_str();
	h += ")";
	return h;
}

void write_spot(Writer& w, const shared_str& type, u16 host_id, const xr_string& hint, bool ser, PeerId author)
{
	w.str(cs(type));
	w.u16(host_id);
	w.str(hint.c_str());
	w.boolean(ser);
	w.u32(author);
}

// Host: spot body for broadcast / full list.
void host_spot_body(Writer& w, CMapLocation* ml, const shared_str& type, u16 id)
{
	auto it = g_spot_authors.find(SpotKey{type, id});
	xr_string hint;
	PeerId author = kHostPeer;
	if (it != g_spot_authors.end())
	{
		hint = cs(it->second.hint);
		author = it->second.author;
	}
	else if (LPCSTR h = ml->GetHint())
		hint = h;
	write_spot(w, type, id, hint, ml->Serializable(), author);
}

void flush_outbox()
{
	if (g_outbox.empty())
		return;
	xr_vector<Out> out;
	out.swap(g_outbox);
	const bool host = mgr().is_host();
	const u64 now = mgr().now_ms();
	for (size_t i = 0; i < out.size(); ++i)
	{
		Out& o = out[i];
		if (!host && !g_pacer.take(o.type, now)) // F2 T14 fix 1: the rest waits for a later frame, in order
		{
			g_outbox.insert(g_outbox.begin(), std::make_move_iterator(out.begin() + ptrdiff_t(i)),
			    std::make_move_iterator(out.end()));
			++g_out_held;
			return;
		}
		if (!o.spot_add)
		{
			send_out(o.type, o.w);
			continue;
		}
		if (!in_game())
			continue;
		CMapLocation* ml = spots().GetMapLocation(o.spot_type, o.spot_id);
		if (!spot_replicable(ml, o.spot_type))
			continue; // gone already, or turned out to be a task spot
		Writer w(96);
		w.u8(SpotAdd);
		if (host)
		{
			host_spot_body(w, ml, o.spot_type, o.spot_id);
			net::host_broadcast(Msg::MapSpot, w);
			++g_stats.spots_out;
		}
		else
		{
			const u16 hid = host_id_of(o.spot_id);
			LPCSTR h = ml->GetHint();
			write_spot(w, o.spot_type, hid, h ? xr_string(h) : xr_string(), ml->Serializable(), mgr().self());
			net::client_send(Msg::MapSpot, w);
			g_spot_pending[SpotKey{o.spot_type, hid}] = mgr().now_ms();
			++g_stats.requests_out;
		}
	}
}

void apply_spot_add(const shared_str& type, u16 host_id, const xr_string& hint, bool ser, PeerId author)
{
	const u16 id = local_id_of(host_id);
	if (!type.size() || id == kNoId || spot_type_local(cs(type)))
		return; // a local type (e.g. "coop_*") is never taken from the network
	g_spot_pending.erase(SpotKey{type, host_id});
	Applying guard(Kind::Spot, type, id);
	CMapLocation* ml = spots().GetMapLocation(type, id);
	if (!ml)
	{
		if (!spot_type_known(type))
			return;
		ml = spots().AddMapLocation(type, id);
	}
	const xr_string text = tagged_hint(shared_str(hint.c_str()), author);
	if (!text.empty())
		ml->SetHint(shared_str(text.c_str()));
	ml->SetSerializable(ser);
}

void remove_replicable_spots_of(u16 id)
{
	xr_vector<CMapLocation*> list;
	spots().GetMapLocations(id, list);
	for (CMapLocation* ml : list)
	{
		const shared_str type = ml->spot_type;
		if (spot_replicable(ml, type))
		{
			Applying guard(Kind::Spot, type, id);
			spots().RemoveMapLocation(ml);
		}
	}
}

DomainHash hash_spots()
{
	DomainHash d;
	if (!in_game())
		return d;
	const u64 now = mgr().now_ms();
	xr_set<SpotKey> keys;
	for (const SLocationKey& k : spots().Locations())
	{
		if (!k.location || !k.location->Serializable() || !spot_replicable(k.location, k.spot_type))
			continue;
		const SpotKey key{k.spot_type, host_id_of(k.object_id)};
		auto p = g_spot_pending.find(key);
		if (p != g_spot_pending.end() && now - p->second < kSpotPendingMs)
			continue; // ours, not confirmed yet
		keys.insert(key);
	}
	for (const SpotKey& k : keys)
	{
		Fnv f;
		f.str(k.type);
		f.pod(k.id);
		d.add(f.h);
	}
	return d;
}

void send_spot_list(PeerId to)
{
	if (!in_game())
		return;
	Writer w(4096);
	w.u8(SpotFull);
	u32 n = 0;
	Writer body(4096);
	for (const SLocationKey& k : spots().Locations())
	{
		if (!spot_replicable(k.location, k.spot_type))
			continue;
		host_spot_body(body, k.location, k.spot_type, k.object_id);
		++n;
	}
	w.u32(n);
	if (body.size())
		w.bytes(body.ptr(), body.size());
	net::host_send(to, Msg::MapSpot, w);
	++g_stats.spots_out;
}

void client_apply_spot_list(Reader& r)
{
	struct S
	{
		shared_str type;
		u16 id;
		xr_string hint;
		bool ser;
		PeerId author;
	};
	const u32 n = r.u32();
	xr_vector<S> list;
	for (u32 i = 0; i < n && r.ok(); ++i)
	{
		S s;
		s.type = to_shared(r.str());
		s.id = r.u16();
		s.hint = r.str().c_str();
		s.ser = r.boolean();
		s.author = r.u32();
		list.push_back(s);
	}
	if (!r.ok() || !in_game())
		return;
	xr_set<SpotKey> host;
	for (const S& s : list)
	{
		host.insert(SpotKey{s.type, s.id});
		apply_spot_add(s.type, s.id, s.hint, s.ser, s.author);
	}
	// Story spots the host does not have go away; transient local ones are left alone.
	const u64 now = mgr().now_ms();
	xr_vector<CMapLocation*> gone;
	for (const SLocationKey& k : spots().Locations())
	{
		if (!k.location || !k.location->Serializable() || !spot_replicable(k.location, k.spot_type))
			continue;
		const SpotKey key{k.spot_type, host_id_of(k.object_id)};
		auto p = g_spot_pending.find(key);
		if (host.count(key) || (p != g_spot_pending.end() && now - p->second < kSpotPendingMs))
			continue;
		gone.push_back(k.location);
	}
	for (CMapLocation* ml : gone)
	{
		Applying guard(Kind::Spot, shared_str(ml->spot_type), ml->ObjectID());
		spots().RemoveMapLocation(ml);
	}
	++g_stats.spots_in;
	::Msg("[coop] story: map spots resynced from the host: %u spots, %u removed here", n, (u32)gone.size());
}

void host_on_spot_request(PeerId from, u8 op, Reader& r)
{
	++g_stats.requests_in;
	ExecFor ef(from);
	if (op == SpotAdd)
	{
		const shared_str type = to_shared(r.str());
		const u16 id = r.u16();
		const xr_string hint = r.str().c_str();
		const bool ser = r.boolean();
		r.u32(); // author: the sender
		if (!r.ok() || !type.size() || id == kNoId || spot_type_local(cs(type)))
			return;
		if (CMapLocation* ml = spots().GetMapLocation(type, id))
		{
			if (ser && !ml->Serializable())
				ml->SetSerializable(true);
			return; // the host has it already (both ran the same script)
		}
		if (!spot_type_known(type))
			return;
		CMapLocation* ml = spots().AddMapLocation(type, id); // the hook broadcasts it (at the next flush)
		// R4 3: after AddMapLocation, whose hook clears a stale author of this key.
		SpotAuthor& a = g_spot_authors[SpotKey{type, id}];
		a.author = from;
		a.hint = shared_str(hint.c_str());
		const xr_string text = tagged_hint(a.hint, from);
		if (!text.empty())
			ml->SetHint(shared_str(text.c_str()));
		ml->SetSerializable(ser);
	}
	else if (op == SpotRemove)
	{
		const shared_str type = to_shared(r.str());
		const u16 id = r.u16();
		if (!r.ok() || !type.size() || id == kNoId)
			return;
		if (!spot_placed_by(from, spots().GetMapLocation(type, id), type, id))
		{
			++g_stats.spot_removes_refused; // the next spot resync gives it back to the client
			return;
		}
		spots().RemoveMapLocation(type, id); // the hook broadcasts it
	}
	else if (op == SpotRemoveAll)
	{
		const u16 id = r.u16();
		if (!r.ok() || id == kNoId)
			return;
		// R4 3: only the client's own spots of the object, one by one (each removal is broadcast by the hook).
		xr_vector<CMapLocation*> list;
		spots().GetMapLocations(id, list);
		xr_vector<shared_str> own;
		for (CMapLocation* ml : list)
		{
			const shared_str type = ml->spot_type;
			if (spot_placed_by(from, ml, type, id))
				own.push_back(type);
			else
				++g_stats.spot_removes_refused;
		}
		for (const shared_str& type : own)
			spots().RemoveMapLocation(type, id);
	}
}

// ------------------------------------------------------------------ progress (rank, reputation, community)

struct Progress
{
	int rank = 0, reputation = 0, community = -1;
	bool valid = false;
	bool operator!=(const Progress& o) const
	{
		return rank != o.rank || reputation != o.reputation || community != o.community || valid != o.valid;
	}
};
Progress g_own_sent; // last own progress sent
u64 g_last_progress = 0;
xr_map<PeerId, Progress> g_reported; // host: last values reported by each client

// The proxy already carries the change: keep it when the reported values are put back, until the
// player's next report (which includes it) arrives.
void host_add_reported_reputation(PeerId peer, int delta)
{
	auto it = g_reported.find(peer);
	if (it != g_reported.end())
		it->second.reputation += delta;
}

Progress own_progress()
{
	Progress p;
	if (!g_actor)
		return p;
	p.rank = g_actor->Rank();
	p.reputation = g_actor->Reputation();
	p.community = g_actor->Community();
	p.valid = true;
	return p;
}

void write_progress(Writer& w, u16 who, const Progress& p)
{
	w.u16(who);
	w.i32(p.rank);
	w.i32(p.reputation);
	w.i32(p.community);
}

// Puts a player's progress on the character that stands for that player here.
void apply_progress(u16 local_id, const Progress& p)
{
	if (local_id == 0 || local_id == kNoId || !in_game())
		return; // our own actor is ours
	CInventoryOwner* io = smart_cast<CInventoryOwner*>(Level().Objects.net_Find(local_id));
	if (io)
	{
		if (io->Rank() != p.rank)
			io->SetRank(p.rank);
		if (io->Reputation() != p.reputation)
			io->SetReputation(p.reputation);
		if (community_ok(p.community) && io->Community() != p.community)
			io->SetCommunity(p.community);
		return;
	}
	CSE_ALifeTraderAbstract* tr = smart_cast<CSE_ALifeTraderAbstract*>(ai().alife().objects().object(local_id, true));
	if (!tr)
		return;
	tr->m_rank = p.rank;
	tr->m_reputation = p.reputation;
	if (community_ok(p.community))
		tr->m_community_index = p.community;
}

void progress_frame()
{
	const u64 now = mgr().now_ms();
	if (now - g_last_progress < kProgressIntervalMs || !in_game())
		return;
	g_last_progress = now;
	const Progress mine = own_progress();
	if (mine.valid && mine != g_own_sent)
	{
		Writer w(16);
		write_progress(w, host_id_of(0), mine);
		send_out(Msg::Progress, w);
		g_own_sent = mine;
		++g_stats.progress_out;
	}
	if (!mgr().is_host())
		return;
	// The host's own engine may change a proxy's rank/reputation (combat bookkeeping): the player's
	// machine owns them, so the reported values are put back. Slots of players who left are reused.
	for (auto it = g_reported.begin(); it != g_reported.end();)
	{
		const u16 proxy = join::proxy_of_peer(it->first);
		if (proxy == kNoId)
		{
			it = g_reported.erase(it);
			continue;
		}
		apply_progress(proxy, it->second);
		++it;
	}
}

void send_progress_all(PeerId to)
{
	const Progress mine = own_progress();
	if (mine.valid)
	{
		Writer w(16);
		write_progress(w, 0, mine);
		net::host_send(to, Msg::Progress, w);
	}
	for (const auto& it : g_reported)
	{
		const u16 proxy = join::proxy_of_peer(it.first);
		if (it.first == to || proxy == kNoId)
			continue;
		Writer w(16);
		write_progress(w, proxy, it.second);
		net::host_send(to, Msg::Progress, w);
	}
	++g_stats.progress_out;
}

// ------------------------------------------------------------------ hashes and resync

u64 g_last_hash = 0;
u32 g_streak[kHashDomains] = {};

void hash_all(DomainHash out[kHashDomains])
{
	out[0] = hash_infos();
	out[1] = hash_tasks();
	out[2] = hash_relations();
	out[3] = hash_spots();
}

void host_send_full(PeerId to, u8 mask)
{
	if (mask & DomInfos)
		send_info_list(to);
	if (mask & DomTasks)
		send_task_list(to);
	if (mask & DomRelations)
		send_relation_list(to);
	if (mask & DomSpots)
		send_spot_list(to);
	if (mask & DomProgress)
		send_progress_all(to);
}

void host_flush_all()
{
	flush_outbox();
	host_flush_relations();
	host_scan_tasks();
}

void request_resync(u8 mask, const char* why)
{
	if (mgr().is_host())
	{
		host_flush_all();
		u32 n = 0;
		for (const PeerInfo& p : mgr().session().peers())
		{
			if (p.state != PeerState::Joined)
				continue;
			host_send_full(p.id, mask);
			++n;
		}
		::Msg("[coop] story: pushed full story state (mask %u) to %u client(s) (%s)", mask, n, why);
		return;
	}
	if (!mgr().is_live_client())
	{
		::Msg("! [coop] story: not in a live session");
		return;
	}
	Writer w(2);
	w.u8(mask);
	net::client_send(Msg::StoryResync, w);
	++g_stats.resyncs_out;
	::Msg("[coop] story: asked the host for the full story state (mask %u, %s)", mask, why);
}

void host_hash_frame()
{
	const u64 now = mgr().now_ms();
	if (now - g_last_hash < kHashIntervalMs)
		return;
	g_last_hash = now;
	if (!in_game())
		return;
	bool any = false;
	for (const PeerInfo& p : mgr().session().peers())
		any = any || p.state == PeerState::Joined;
	if (!any)
		return;
	host_flush_all(); // the hash must describe what was sent before it
	DomainHash h[kHashDomains];
	hash_all(h);
	Writer w(64);
	w.u8(u8(kHashDomains));
	for (const DomainHash& d : h)
	{
		w.u32(d.count);
		w.u64(d.hash);
	}
	net::host_broadcast(Msg::StoryHash, w);
	++g_stats.hashes;
}

void client_on_hash(Reader& r)
{
	const u8 n = r.u8();
	DomainHash host[kHashDomains];
	for (u32 i = 0; i < n && i < kHashDomains; ++i)
	{
		host[i].count = r.u32();
		host[i].hash = r.u64();
	}
	if (!r.ok() || n < kHashDomains)
		return;
	++g_stats.hashes;
	client_reconcile(false);
	DomainHash mine[kHashDomains];
	hash_all(mine);
	static const char* const kNames[kHashDomains] = {"info portions", "tasks", "relations", "map spots"};
	u8 mask = 0;
	for (u32 i = 0; i < kHashDomains; ++i)
	{
		if (mine[i] == host[i])
		{
			g_streak[i] = 0;
			continue;
		}
		++g_streak[i];
		++g_stats.mismatches;
		// Persistent differences (e.g. a spot on an object this machine never got) back off to 1/min.
		if (g_streak[i] > 3 && g_streak[i] % 6)
			continue;
		::Msg("[coop] story: %s differ from the host (%u/%016llx here, %u/%016llx host)%s", kNames[i], mine[i].count,
		    mine[i].hash, host[i].count, host[i].hash, g_streak[i] > 3 ? ", persistent" : "");
		mask |= u8(1u << i);
	}
	if (mask)
		request_resync(mask, "hash mismatch");
}

// ------------------------------------------------------------------ dispatch

bool g_client_inited = false;

void ensure_client_init()
{
	if (g_client_inited || !client_live())
		return;
	g_client_inited = true;
	client_swap_relations();
	g_known.clear();
	for (const SGameTaskKey& k : tasks().GetGameTasks())
		if (k.game_task)
			g_known.insert(TaskKey{k.task_id, k.game_task->m_ReceiveTime});
	g_own_sent = Progress();
	g_last_progress = 0;
	::Msg("[coop] story: live, local scripts track %u task(s)", (u32)g_known.size());
	request_resync(DomAll, "joined");
}

bool ready()
{
	if (mgr().is_host())
	{
		if (!in_game())
			return false;
		if (g_host_level != g_pGameLevel || g_host_epoch != join::host_world_epoch())
		{
			// Hosting started or the world was replaced (clients get that one from a fresh save):
			// remember the current task list without broadcasting it, before any request runs.
			// (The epoch also catches a reloaded level that got the old CLevel's address.)
			g_host_level = g_pGameLevel;
			g_host_epoch = join::host_world_epoch();
			g_host_sent.clear();
			g_host_prime = true;
			host_scan_tasks();
		}
		return true;
	}
	if (!client_live())
		return false;
	ensure_client_init();
	return true;
}

void handle_host(Msg type, PeerId from, Reader& r)
{
	switch (type)
	{
	case Msg::InfoPortion:
	{
		const u8 op = r.u8();
		const shared_str id = to_shared(r.str());
		if (!r.ok() || !id.size() || (op != InfoGive && op != InfoDisable) || !g_actor)
			return;
		if (machine_info(id))
			return; // R4 1: a client's UI / sleep info is its own (an old client may still send it)
		++g_stats.requests_in;
		const bool give = op == InfoGive;
		ExecFor ef(from);
		if (g_actor->HasInfo(id) != give)
			g_actor->TransferInfo(id, give); // the hook broadcasts it to everybody
		else
		{
			Writer w(64); // nothing changed here: the client is behind, correct it
			w.u8(give ? InfoGive : InfoDisable);
			w.str(cs(id));
			net::host_send(from, Msg::InfoPortion, w);
		}
		break;
	}
	case Msg::TaskUpdate:
	{
		const u8 op = r.u8();
		if (op == TaskReqGive)
			host_on_task_give(from, r);
		else if (op == TaskReqState)
			host_on_task_state(from, r);
		break;
	}
	case Msg::MapSpot: host_on_spot_request(from, r.u8(), r); break;
	case Msg::Relation: host_on_relation_request(from, r.u8(), r); break;
	case Msg::Progress:
	{
		r.u16(); // the sender's own idea of its id: the host knows its proxy
		Progress p;
		p.rank = r.i32();
		p.reputation = r.i32();
		p.community = r.i32();
		p.valid = true;
		const u16 proxy = join::proxy_of_peer(from);
		if (!r.ok() || proxy == kNoId)
			return;
		g_reported[from] = p;
		apply_progress(proxy, p);
		Writer w(16);
		write_progress(w, proxy, p);
		net::host_broadcast(Msg::Progress, w, Lane::Reliable, from);
		++g_stats.progress_in;
		break;
	}
	case Msg::StoryResync:
	{
		const u8 mask = r.u8();
		if (!r.ok())
			return;
		++g_stats.resyncs_in;
		host_flush_all();
		host_send_full(from, mask);
		break;
	}
	default: break;
	}
}

void handle_client(Msg type, Reader& r)
{
	switch (type)
	{
	case Msg::InfoPortion:
	{
		const u8 op = r.u8();
		if (op == InfoFull)
		{
			client_apply_info_list(r);
			return;
		}
		const shared_str id = to_shared(r.str());
		if (r.ok() && id.size() && (op == InfoGive || op == InfoDisable))
			client_apply_info(op == InfoGive, id);
		break;
	}
	case Msg::TaskList: client_on_task_list(r); break;
	case Msg::TaskUpdate:
		if (r.u8() == TaskUpsert)
			client_on_task_upsert(r);
		break;
	case Msg::MapSpot:
	{
		const u8 op = r.u8();
		if (op == SpotFull)
		{
			client_apply_spot_list(r);
			return;
		}
		if (op == SpotAdd)
		{
			const shared_str type = to_shared(r.str());
			const u16 id = r.u16();
			const xr_string hint = r.str().c_str();
			const bool ser = r.boolean();
			const PeerId author = r.u32();
			if (r.ok())
				apply_spot_add(type, id, hint, ser, author);
		}
		else if (op == SpotRemove)
		{
			const shared_str type = to_shared(r.str());
			const u16 hid = r.u16();
			const u16 id = local_id_of(hid);
			if (!r.ok() || !type.size() || id == kNoId)
				return;
			g_spot_pending.erase(SpotKey{type, hid});
			Applying guard(Kind::Spot, type, id);
			spots().RemoveMapLocation(type, id);
		}
		else if (op == SpotRemoveAll)
		{
			const u16 id = local_id_of(r.u16());
			if (r.ok() && id != kNoId)
				remove_replicable_spots_of(id);
		}
		++g_stats.spots_in;
		break;
	}
	case Msg::Relation:
	{
		const u8 op = r.u8();
		if (op == RelFull)
		{
			client_apply_relation_list(r);
			return;
		}
		if (op == RelPersonal)
		{
			const u16 a = r.u16(), b = r.u16();
			const int v = r.i32();
			if (r.ok() && a != kNoId && b != kNoId)
				client_set_personal(a, b, v);
		}
		else if (op == RelCommunity)
		{
			const int c = r.i32();
			const u16 to = r.u16();
			const int v = r.i32();
			if (r.ok() && to != kNoId)
				client_set_community(c, to, v);
		}
		else if (op == RelCommRelation)
		{
			const int a = r.i32(), b = r.i32(), v = r.i32();
			if (r.ok())
				client_set_comm_relation(a, b, v);
		}
		else if (op == RelReputationDelta)
		{
			// The host counted an attack / fight help of ours (R-3.4): our own reputation changes here,
			// and the next progress report tells everybody.
			const u16 who = r.u16();
			const int delta = r.i32();
			if (r.ok() && delta && g_actor && local_id_of(who) == 0)
			{
				g_actor->ChangeReputation(delta);
				++g_stats.rep_deltas_in;
			}
			return;
		}
		++g_stats.rel_in;
		break;
	}
	case Msg::Progress:
	{
		const u16 who = r.u16();
		Progress p;
		p.rank = r.i32();
		p.reputation = r.i32();
		p.community = r.i32();
		p.valid = true;
		if (r.ok())
			apply_progress(local_id_of(who), p);
		++g_stats.progress_in;
		break;
	}
	case Msg::StoryHash: client_on_hash(r); break;
	default: break;
	}
}

void handle(Msg type, PeerId from, Reader& r)
{
	if (mgr().is_host())
		handle_host(type, from, r);
	else
		handle_client(type, r);
}

void dispatch(Msg type, PeerId from, Reader& r)
{
	if (!mgr().active())
		return;
	// Clients talk only to the host; the host only takes requests from clients.
	if (mgr().is_host() ? from == kHostPeer : from != kHostPeer)
		return;
	if (!ready())
	{
		if (g_deferred.size() >= kMaxDeferred)
		{
			::Msg("! [coop] story: deferred queue full, dropping %s", msg_name(type));
			return;
		}
		Deferred d;
		d.type = type;
		d.from = from;
		d.data.assign(r.cursor(), r.cursor() + r.remaining());
		g_deferred.push_back(std::move(d));
		++g_stats.deferred;
		return;
	}
	handle(type, from, r);
}

void replay_deferred()
{
	if (g_deferred.empty() || !ready())
		return;
	xr_vector<Deferred> list;
	list.swap(g_deferred);
	for (Deferred& d : list)
	{
		Reader r(d.data.data(), d.data.size());
		handle(d.type, d.from, r);
	}
}

// ------------------------------------------------------------------ console

class CCC_CoopTasksResync : public IConsole_Command
{
public:
	CCC_CoopTasksResync(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = true; }
	void Execute(LPCSTR args) override
	{
		if (!mgr().active())
		{
			::Msg("! [coop] not in a session");
			return;
		}
		const bool all = args && strstr(args, "all");
		request_resync(all ? u8(DomAll) : u8(DomTasks), "console");
	}
	void Info(TInfo& I) override
	{
		xr_strcpy(I, "co-op: reload the task list from the host (client) or push it to all clients (host); 'all' = whole story");
	}
};

bool g_commands_registered = false;

void register_commands()
{
	if (g_commands_registered || !Console)
		return;
	g_commands_registered = true;
	CMD1(CCC_CoopTasksResync, "coop_tasks_resync");
	// Host setting (R-7.6): 0 = task money to the finisher, 1 = split among online players.
	CMD4(CCC_Integer, "coop_share_rewards", &g_coop_share_rewards, 0, 1);
}

const char* peer_name(PeerId id)
{
	if (id == kHostPeer)
		return "host";
	const PeerInfo* p = mgr().session().peer(id);
	return p ? p->nick.c_str() : "?";
}
} // namespace

// ================================================================== public

void install(Session& s)
{
	const Msg types[] = {Msg::InfoPortion, Msg::TaskList, Msg::TaskUpdate, Msg::MapSpot, Msg::Relation, Msg::Progress,
		Msg::StoryHash, Msg::StoryResync};
	for (Msg t : types)
		s.on(t, [t](PeerId from, Reader& r) { dispatch(t, from, r); });
	register_commands();
}

void reset()
{
	g_outbox.clear();
	g_pacer.reset(); // F2 T14 fix 1
	g_deferred.clear();
	g_info_requests.clear();
	g_hold_infos = false;
	g_local_infos.clear(); // a new world brings its own infos (personal tasks stay: process lifetime)
	g_host_sent.clear();
	g_host_prime = true;
	g_host_level = nullptr;
	g_host_epoch = 0;
	g_host_last_scan = 0;
	g_finisher.clear();
	g_client_recs.clear();
	g_known.clear();
	g_pending.clear(); // the objects stay alive: scripts may hold them
	g_state_requests.clear();
	g_last_reconcile = 0;
	g_rel_dirty.clear();
	g_rep_delta.clear();
	g_spot_authors.clear();
	g_spot_pending.clear();
	g_own_sent = Progress();
	g_last_progress = 0;
	g_reported.clear();
	g_last_hash = 0;
	for (u32& s : g_streak)
		s = 0;
	g_client_inited = false;
	g_apply = ApplyState();
	g_action_depth = 0;
	g_silent_task = nullptr;
	g_exec_peer = kInvalidPeer;
}

void frame()
{
	register_commands();
	if (g_hold_infos)
	{
		// coop.story_hold_infos(true) without the matching false (a Lua error between them): never
		// carry a hold into the next frame. Lua runs synchronously, never across this call.
		g_hold_infos = false;
		if (++g_holds_leaked <= 10)
			::Msg("! [coop] story: info hold left on by a script, released");
	}
	if (!mgr().active())
		return;
	if (mgr().is_host())
	{
		if (!ready())
			return;
		replay_deferred();
		flush_outbox();
		host_flush_relations();
		host_flush_rep_deltas();
		if (mgr().now_ms() - g_host_last_scan >= kTaskScanIntervalMs)
			host_scan_tasks();
		progress_frame();
		host_hash_frame();
		return;
	}
	if (!client_live())
	{
		g_outbox.clear(); // loading: local changes are healed by the resync at live
		return;
	}
	ensure_client_init();
	replay_deferred();
	flush_outbox();
	if (mgr().now_ms() - g_last_reconcile >= kReconcileIntervalMs)
		client_reconcile(false);
	progress_frame();
	const u64 now = mgr().now_ms();
	for (auto it = g_info_requests.begin(); it != g_info_requests.end();)
		it = now - it->second > kRequestRetryMs ? g_info_requests.erase(it) : std::next(it);
	for (auto it = g_state_requests.begin(); it != g_state_requests.end();)
		it = now - it->second > kRequestRetryMs ? g_state_requests.erase(it) : std::next(it);
	for (auto it = g_spot_pending.begin(); it != g_spot_pending.end();)
		it = now - it->second > kSpotPendingMs ? g_spot_pending.erase(it) : std::next(it);
}

void print_status()
{
	DomainHash h[kHashDomains];
	if (in_game())
		hash_all(h);
	::Msg("[coop] story: infos %u/%016llx, tasks %u/%016llx, relations %u/%016llx, map spots %u/%016llx", h[0].count,
	    h[0].hash, h[1].count, h[1].hash, h[2].count, h[2].hash, h[3].count, h[3].hash);
	::Msg("[coop] story: out infos %llu tasks %llu rel %llu spots %llu progress %llu requests %llu | in infos %llu tasks %llu "
	      "rel %llu spots %llu progress %llu requests %llu",
	    g_stats.infos_out, g_stats.tasks_out, g_stats.rel_out, g_stats.spots_out, g_stats.progress_out, g_stats.requests_out,
	    g_stats.infos_in, g_stats.tasks_in, g_stats.rel_in, g_stats.spots_in, g_stats.progress_in, g_stats.requests_in);
	::Msg("[coop] story: tasks created %llu detached %llu, known %u, pending %u | dropped: reactions %llu, combat goodwill %llu, "
	      "task spot edits %llu, foreign spot removals %llu | hashes %llu, mismatches %llu, resyncs out %llu in %llu, "
	      "deferred %llu",
	    g_stats.tasks_created, g_stats.tasks_detached, (u32)g_known.size(), (u32)g_pending.size(), g_stats.derived_dropped,
	    g_stats.action_dropped, g_stats.task_edits_dropped, g_stats.spot_removes_refused, g_stats.hashes,
	    g_stats.mismatches, g_stats.resyncs_out, g_stats.resyncs_in, g_stats.deferred);
	::Msg("[coop] story: player proxies in combat bookkeeping %llu, reputation deltas out %llu in %llu, share rewards %d",
	    g_stats.proxy_actions, g_stats.rep_deltas_out, g_stats.rep_deltas_in, g_coop_share_rewards);
	::Msg("[coop] story: held infos %llu (kept here %u, holds left on %llu) | personal tasks %u, local task edits %llu",
	    g_infos_held, (u32)g_local_infos.size(), g_holds_leaked, (u32)g_local_tasks.size(), g_local_task_edits);
	::Msg("[coop] story: outbox %u, held for the host's rate %llu frames, dropped (queue full) %llu", // F2 T14 fix 1
	    (u32)g_outbox.size(), g_out_held, g_out_dropped);
	for (const auto& it : g_finisher)
		::Msg("[coop] story:   task '%s' finished at the request of %s", cs(it.first), peer_name(it.second));
	if (in_game()) // the Lua side of shared tasks (coop_tasks.script, W3c)
		run_lua("@coop_story_status",
		    "local ok, m = pcall(function() return coop_tasks end)\n"
		    "if ok and type(m) == 'table' and type(m.print_status) == 'function' then pcall(m.print_status) end\n"
		    "return 'ok'\n",
		    "", 0, false);
}

bool applying() { return applying_any(); }

// ------------------------------------------------------------------ hooks: info portions

bool on_info(const CInventoryOwner* owner, const shared_str& info_id, bool give)
{
	if (!owner || !info_id.size() || !mgr().active() || owner->object_id() != 0)
		return false;
	if (machine_info(info_id))
		return false; // R4 1: UI / sleep info of this machine: applied here, never sent, never held or dropped
	if (owner->HasInfo(info_id) == give)
		return false; // nothing changes: let the engine run its no-op path
	const u8 op = give ? InfoGive : InfoDisable;
	if (mgr().is_host())
	{
		Writer w(64);
		w.u8(op);
		w.str(cs(info_id));
		push_out(Msg::InfoPortion, std::move(w));
		++g_stats.infos_out;
		return false;
	}
	if (!mgr().is_live_client())
		return false; // loading the world: healed by the resync at live
	if (applying_this(Kind::Info, info_id))
		return false;
	if (applying_any())
	{
		++g_stats.derived_dropped;
		return true;
	}
	if (g_hold_infos || g_local_infos.count(info_id))
	{
		// Held by a condlist (or one of ours): applied here only, never sent to the host.
		if (give)
			g_local_infos.insert(info_id);
		else
			g_local_infos.erase(info_id);
		++g_infos_held;
		return false;
	}
	const auto key = std::make_pair(op, info_id);
	const u64 now = mgr().now_ms();
	auto it = g_info_requests.find(key);
	if (it != g_info_requests.end() && now - it->second < kRequestRetryMs)
		return true; // already asked (TransferInfo also echoes through the local server)
	g_info_requests[key] = now;
	Writer w(64);
	w.u8(op);
	w.str(cs(info_id));
	push_out(Msg::InfoPortion, std::move(w));
	++g_stats.requests_out;
	return true;
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
	if (!t || !mgr().is_client())
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

// ------------------------------------------------------------------ hooks: relations

bool acts_as_actor(const CEntityAlive* who)
{
	// Section check, like sync::is_remote_driven: the same answer on every machine, in a session or not.
	return who && !smart_cast<const CActor*>(who) && !xr_strcmp(who->cNameSect(), kAvatarSection);
}

RelationActionScope::RelationActionScope(CEntityAlive* from, int action)
{
	// Only the outermost call (ATTACK may call itself for FIGHT_HELP) measures the whole change. A kill is
	// counted by the killer's own machine (its CEntityAlive::Die runs Action for its actor).
	if (g_action_depth++ != 0 || action == RELATION_REGISTRY::KILL || !mgr().is_host() || !acts_as_actor(from))
		return;
	m_owner = smart_cast<CInventoryOwner*>(from);
	if (!m_owner)
		return;
	m_id = from->ID();
	m_reputation = m_owner->Reputation();
	++g_stats.proxy_actions;
}

RelationActionScope::~RelationActionScope()
{
	--g_action_depth;
	if (!m_owner)
		return;
	const int delta = m_owner->Reputation() - m_reputation;
	if (delta)
		g_rep_delta[m_id] += delta; // sent from frame() (this may run on the game thread)
}

bool on_personal_goodwill(u16 from, u16 to, int value, GoodwillOp op)
{
	if (!mgr().active() || !ai().get_alife() || !(is_player(from) || is_player(to)))
		return false;
	if (mgr().is_host())
	{
		g_rel_dirty.insert(RelKey{RelPersonal, from, to});
		return false;
	}
	if (!mgr().is_live_client())
		return false;
	const u8 req = op == GoodwillOp::Set ? RelReqPersonalSet : op == GoodwillOp::Change ? RelReqPersonalChange : RelReqPersonalForce;
	return client_relation_request(req, from, to, value);
}

bool on_community_goodwill(int community, u16 to, int value, GoodwillOp op)
{
	if (!mgr().active() || !ai().get_alife() || !is_player(to))
		return false;
	if (mgr().is_host())
	{
		g_rel_dirty.insert(RelKey{RelCommunity, community, to});
		return false;
	}
	if (!mgr().is_live_client())
		return false;
	return client_relation_request(op == GoodwillOp::Change ? RelReqCommunityChange : RelReqCommunitySet, community, to, value);
}

bool on_community_relation(int community_from, int community_to, int value)
{
	if (!mgr().active())
		return false;
	if (mgr().is_host())
	{
		g_rel_dirty.insert(RelKey{RelCommRelation, community_from, community_to});
		return false;
	}
	if (!mgr().is_live_client())
		return false;
	return client_relation_request(RelReqCommRelation, community_from, community_to, value);
}

// ------------------------------------------------------------------ hooks: map spots

void on_spot_added(const shared_str& spot_type, u16 id)
{
	if (!mgr().active() || spot_type_local(cs(spot_type)))
		return;
	if (mgr().is_host())
		g_spot_authors.erase(SpotKey{spot_type, id}); // R4 3: a new spot has no author until a client's request sets it
	else
	{
		if (!client_live())
			return;
		if (applying_any())
		{
			if (!applying_this(Kind::Spot, spot_type, id) && g_apply.kind != Kind::Task)
				++g_stats.derived_dropped;
			return;
		}
	}
	if (!outbox_room()) // F2 T14 fix 1
		return;
	Out o;
	o.type = Msg::MapSpot;
	o.spot_add = true;
	o.spot_type = spot_type;
	o.spot_id = id;
	g_outbox.push_back(std::move(o));
}

void on_spot_remove(const shared_str& spot_type, u16 id)
{
	if (!mgr().active() || !g_pGameLevel || spot_type_local(cs(spot_type)))
		return;
	if (!mgr().is_host() && (!client_live() || applying_any()))
		return;
	CMapLocation* ml = spots().GetMapLocation(spot_type, id);
	if (!spot_replicable(ml, spot_type))
		return;
	Writer w(48);
	w.u8(SpotRemove);
	w.str(cs(spot_type));
	w.u16(host_id_of(id));
	if (mgr().is_host())
	{
		g_spot_authors.erase(SpotKey{spot_type, id});
		++g_stats.spots_out;
	}
	else
		++g_stats.requests_out;
	push_out(Msg::MapSpot, std::move(w));
}

void on_spots_remove_all(u16 id)
{
	if (!mgr().active() || !g_pGameLevel)
		return;
	if (!mgr().is_host() && (!client_live() || applying_any()))
		return;
	Writer w(8);
	w.u8(SpotRemoveAll);
	w.u16(host_id_of(id));
	push_out(Msg::MapSpot, std::move(w));
	if (mgr().is_host())
	{
		for (auto it = g_spot_authors.begin(); it != g_spot_authors.end();) // R4 3: authors go with the spots
			it = it->first.id == id ? g_spot_authors.erase(it) : std::next(it);
		++g_stats.spots_out;
	}
	else
		++g_stats.requests_out;
}

u32 task_finisher(LPCSTR task_id)
{
	if (!task_id)
		return kInvalidPeer;
	auto it = g_finisher.find(shared_str(task_id));
	return it == g_finisher.end() ? kInvalidPeer : it->second;
}

// ------------------------------------------------------------------ Lua (W5)

bool spot_is_local(LPCSTR type) { return spot_type_local(type); }

bool hold_infos(bool on)
{
	if (!on)
	{
		g_hold_infos = false;
		return true;
	}
	if (!mgr().is_live_client())
		return false; // the host's infos are the story; a loading client has nothing to hold
	g_hold_infos = true;
	return true;
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
} // namespace story
} // namespace coop
