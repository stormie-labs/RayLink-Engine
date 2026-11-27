// coop::story: what its files share (coop_story.cpp, _tasks, _targets, _relations, _spots, _progress). Private to them.
// coop_story.cpp: the wire codes, the outbox, the applying guard, the hashes, the dispatch and the info portions; each other file
// one domain of the story.
#pragma once

#include "coop_story.h"
#include "coop_net.h"
#include "xrCoop/coop_limits.h"
#include "xrCoop/coop_task_audience.h"
#include "xrCoop/coop_task_target.h"
#include "xrCoop/coop_fight_fact.h"
#include "xrCoop/coop_hash.h"
#include "xrCoop/coop_ids.h"
#include "GameTaskDefs.h"

class CGameTaskManager;
class CMapManager;
class CMapLocation;

namespace coop
{
namespace story
{
constexpr u64 kHashIntervalMs = 10000;
constexpr u64 kTaskScanIntervalMs = 200;
constexpr u64 kReconcileIntervalMs = 200;
constexpr u64 kProgressIntervalMs = 1000;
constexpr u64 kProgressRefreshMs = 30000; // rank and reputation are in no StoryHash domain
constexpr u64 kRequestRetryMs = 3000;
constexpr u64 kMapRetryMs = 5000;
constexpr u64 kSpotPendingMs = 5000;
constexpr u64 kTargetScanIntervalMs = 500;
constexpr u32 kTargetsPerMessage = 256;
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
	TaskTargets = 4, // host -> client: {u16 n, n * {u16 id, vec3 pos, u32 level vertex, u16 game vertex}}

	SpotAdd = 1, // both directions: {str type, u16 id, str hint, u8 serializable, u32 author}
	SpotRemove = 2, // both: {str type, u16 id}
	SpotRemoveAll = 3, // both: {u16 id}
	SpotFull = 4, // host -> client: {u32 n, n * SpotAdd body}

	RelPersonal = 1, // host -> client: {u16 from, u16 to, i32 value} absolute
	RelCommunity = 2, // host -> client: {i32 community, u16 to, i32 value} absolute
	RelCommRelation = 3, // host -> client: {i32 from, i32 to, i32 value} absolute
	RelFull = 4, // host -> client: {u32 n, personal...}{u32 n, community...}{u32 n, n*n table}
	RelReputationDelta = 5, // host -> owning client: {u16 who (host id of its proxy), i32 delta}
	RelReqPersonalSet = 11, // client -> host: {u16 from, u16 to, i32 value}
	RelReqPersonalChange = 12, // {u16 from, u16 to, i32 delta}
	RelReqPersonalForce = 13, // {u16 from, u16 to, i32 value}
	RelReqCommunitySet = 14, // {i32 community, u16 to, i32 value}
	RelReqCommunityChange = 15, // {i32 community, u16 to, i32 delta}
	RelReqCommRelation = 16, // {i32 from, i32 to, i32 value}
	RelUpFightFact = 17, // a map server -> the world server: a fight's reputation of a player (xrCoop/coop_fight_fact.h)
};
static_assert(RelUpFightFact == fight::kRelOp, "the op of a fight fact is the one its wire writes");

const char* cs(const shared_str& s);
shared_str to_shared(const std::string& s);
bool machine_info(const shared_str& id);
u16 host_id_of(u16 local);
u16 local_id_of(u16 host);
bool in_game();
bool client_live();
CGameTaskManager& tasks();
CMapManager& spots();
bool is_player(u16 id);

// Order-independent hash of a set: sum of per-item FNV-1a.
struct Fnv
{
	u64 h = kFnv64Basis;
	void bytes(const void* data, size_t size) { h = fnv1a64(static_cast<const u8*>(data), size, h); }
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
	u64 spot_removes_refused = 0; // host: client removals of spots it did not place
	u64 hashes = 0, mismatches = 0, resyncs_out = 0, resyncs_in = 0;
	u64 deferred = 0;
	u64 proxy_actions = 0; // host: RELATION_REGISTRY::Action calls with a player proxy as the actor
	u64 rep_deltas_out = 0, rep_deltas_in = 0; // reputation from host-side combat bookkeeping
	u64 rep_facts_up = 0, rep_facts_in = 0, rep_facts_refused = 0; // fight facts of a map server, the keeper's verdicts
};
extern Stats g_stats;

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
};
extern ApplyState g_apply;

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

bool applying_any();
bool applying_this(Kind kind, const shared_str& key, u16 id = kNoId);

extern int g_action_depth; // RELATION_REGISTRY::Action nesting
extern CGameTask* g_silent_task; // client: task whose host state is being applied
extern PeerId g_exec_peer; // host: client whose request is being executed

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
extern limits::Outbox<Out> g_outbox;
extern limits::Pacer g_pacer;
void queue_out(Out&& o, const limits::CommunityPair* pair = nullptr);
void push_out(Msg type, Writer&& w, const limits::CommunityPair* pair = nullptr);
void send_out(Msg type, const Writer& w);
void host_flush_all();
void host_send_full(PeerId to, u8 mask);
bool ready();

// ------------------------------------------------------------------ tasks (coop_story_tasks.cpp)

struct TaskKey
{
	shared_str id;
	u64 receive = 0; // the same task id can be given again later (repeatable tasks)
	bool operator<(const TaskKey& o) const { return id == o.id ? receive < o.receive : id < o.id; }
};

// Replicated engine fields of a CGameTask (what the host computed).
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
};

extern xr_set<shared_str> g_local_tasks;
extern u64 g_local_task_edits;
extern task_audience::Table g_audience;
extern xr_map<TaskKey, TaskRec> g_host_sent;
extern bool g_host_prime;
extern const void* g_host_level;
extern u32 g_host_epoch;
extern u64 g_host_last_scan;
extern xr_map<shared_str, PeerId> g_finisher;
extern xr_map<TaskKey, TaskRec> g_client_recs;
extern xr_set<TaskKey> g_known;
extern xr_map<shared_str, CGameTask*> g_pending;
extern xr_map<std::pair<shared_str, u16>, u64> g_state_requests;
extern u64 g_last_reconcile;

bool is_local_task(const shared_str& id);
bool may_see(PeerId peer, const shared_str& id);
DomainHash hash_tasks();
DomainHash hash_tasks_for(PeerId peer);
void host_scan_tasks();
void send_task_list(PeerId to);
void host_on_task_give(PeerId from, Reader& r);
void host_on_task_state(PeerId from, Reader& r);
void client_on_task_upsert(Reader& r);
void client_on_task_list(Reader& r);
void client_reconcile(bool full);

// ------------------------------------------------------------------ task targets (coop_story_targets.cpp)

struct SentTarget
{
	task_target::Place place;
	u64 at = 0;
};

// Client: what became of the host's places (coop_status), per target in host numbering.
struct TargetRx
{
	u64 at = 0; // the last place from the host
	u32 n = 0; // places from the host
	task_target::Take take = task_target::Take::Unknown; // what was done with the last one
};

extern xr_map<u16, SentTarget> g_target_sent;
extern u64 g_target_last_scan;
extern xr_map<u16, TargetRx> g_target_rx;
extern xr_map<u16, u64> g_target_unknown_logged;
extern u64 g_target_unknown;

void host_scan_targets();
void send_targets_to(PeerId to);
void client_on_task_targets(Reader& r);
void print_task_targets();

// ------------------------------------------------------------------ relations (coop_story_relations.cpp)

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
extern DomainHash g_rel_host_hash;
extern bool g_rel_diag_wanted;

bool community_ok(int c);
void host_flush_relations();
void host_flush_rep_deltas();
void client_set_personal(u16 host_from, u16 host_to, int v);
void client_set_community(int community, u16 host_to, int v);
void client_set_comm_relation(int a, int b, int v);
DomainHash hash_relations();
void send_relation_list(PeerId to);
void client_apply_relation_list(Reader& r);
void client_swap_relations();
void host_on_relation_request(PeerId from, u8 op, Reader& r);
void ghost_frame();
void reset_relations(); // the dirty keys, the ghost's words and the reputation changes of a session forgotten

// ------------------------------------------------------------------ map spots (coop_story_spots.cpp)

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
extern xr_map<SpotKey, SpotAuthor> g_spot_authors;
extern xr_map<SpotKey, u64> g_spot_pending;

bool spot_type_local(LPCSTR type);
bool spot_type_known(const shared_str& type);
bool spot_replicable(CMapLocation* ml, const shared_str& type);
void write_spot(Writer& w, const shared_str& type, u16 host_id, const xr_string& hint, bool ser, PeerId author);
void host_spot_body(Writer& w, CMapLocation* ml, const shared_str& type, u16 id);
void apply_spot_add(const shared_str& type, u16 host_id, const xr_string& hint, bool ser, PeerId author);
void remove_replicable_spots_of(u16 id);
DomainHash hash_spots();
void send_spot_list(PeerId to);
void client_apply_spot_list(Reader& r);
void host_on_spot_request(PeerId from, u8 op, Reader& r);

// ------------------------------------------------------------------ progress (coop_story_progress.cpp)

struct Progress
{
	int rank = 0, reputation = 0, community = -1;
	bool valid = false;
	bool operator!=(const Progress& o) const
	{
		return rank != o.rank || reputation != o.reputation || community != o.community || valid != o.valid;
	}
};
extern Progress g_own_sent;
extern u64 g_last_progress;
extern xr_map<PeerId, Progress> g_reported;

void host_add_reported_reputation(PeerId peer, int delta);
void write_progress(Writer& w, u16 who, const Progress& p);
void apply_progress(u16 local_id, const Progress& p);
void progress_frame();
void send_progress_all(PeerId to);
} // namespace story
} // namespace coop
