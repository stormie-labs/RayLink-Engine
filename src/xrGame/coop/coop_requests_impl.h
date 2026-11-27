// Anomaly Coop - client requests and host arbitration: what the files of the module share (coop_requests*.cpp,
// coop_local_zones.cpp). Private to them; the rest of the game sees coop_requests.h. Ids are in local numbering unless a
// name says host.
#pragma once

#include "coop_requests.h"
#include "xrCoop/coop_clone_slot.h"
#include "xrCoop/coop_hits.h"
#include "xrCoop/coop_ids.h"
#include "xrCoop/coop_id_lease.h"
#include "xrCoop/coop_ledger.h"
#include "xrCoop/coop_local_zones.h"
#include "xrCoop/coop_lua_errors.h"
#include "xrCoop/coop_move_admission.h"
#include "xrCoop/coop_replacement.h"
#include "xrCoop/coop_spawn.h"

#include <array>
#include <optional>

#include "Hit.h"

namespace coop
{
namespace requests
{
constexpr u16 kActor = 0; // the local actor on every machine; on the host also the host player
constexpr u8 kSpawnRiderFailed = 0x80; // Action::flags of a Spawn: the clone's Lua activation failed after it was made here

// Values a fair client never sends. Counted (coop_status), the first 10 of each kind logged.
enum Bad : u8
{
	BadBone,
	BadSection,
	BadNumber,
	BadSpawnState,
	BadMoveRange,
	BadUpgrade, // an upgrade the next spawn of the item would be FATAL on
	BadCount
};

enum class Act : u8
{
	Spawn,
	Destroy,
	Use,
	Move,
	Hit,
	ItemEvent,
	Died,
};

// A request of this client, queued until frame() sends it.
struct Action
{
	Act kind = Act::Destroy;
	std::optional<u32> item_world; // leased creates and item mutations only
	u16 item = kNoId, from = kNoId, to = kNoId; // local numbering
	const CSE_Abstract* removal_entity = nullptr; // compare only, never dereference on a retry
	Fvector pos{};
	u8 flags = 0;
	bool open = false; // a reject a take of the same item can still complete (box -> actor)
	u16 ge_type = 0;
	SHit hit;
	xr_vector<u8> bytes; // ItemEvent: event bytes; Spawn: item state (coop_items blob)
	xr_string section; // Spawn
	// ItemEvent: the upgrade's mechanic (local numbering, kNoId: none), what was paid for it, and the key of that payment held
	// out of the money shown until the host answers (money::client_hold, 0: none)
	u16 mechanic = kNoId;
	u32 paid = 0, hold = 0;
	// Spawn: the leased id our item has here already (kNoId: the request path, nil to Lua), and the weapon a clone of ours
	// replaces (local numbering, kNoId: none)
	u16 lease = kNoId, source = kNoId;
};

struct ClientCounters
{
	u64 sent[16] = {};
	u64 accepted = 0, rejected = 0, timeouts = 0, not_live = 0, foreign_moves = 0, child_dups = 0, hits_in = 0;
	u64 lua_forwarded = 0, lua_refused = 0;
	u64 hits_perked = 0, hits_cancelled = 0, perk_errors = 0; // the before-hit Lua of our hits
};

struct LeasedTemp
{
	u16 id = kNoId;
	u16 source = kNoId; // a clone's weapon (local numbering)
};

using RemovalBook = lease::RemovalBook<const CSE_Abstract*>;

struct LeaseCounters
{
	u64 created = 0, temps = 0, clones = 0, confirmed = 0, retried = 0, rolled_back = 0, released_with_clone = 0;
	u64 acks = 0, given_back = 0, bad_leases = 0, stale_leases = 0, not_created = 0;
	u64 held = 0, held_dropped = 0, clones_refused = 0, temps_left = 0, quarantine_left = 0;
	u64 queued = 0, resent = 0, unanswered = 0, paced = 0, not_variant = 0;
	u64 zones_left = 0; // personal map zones a script let go of while their world was going (local_zone_release)
};

struct CarryCounters
{
	u64 sent = 0, too_long = 0, failed = 0; // client
	u64 taken = 0, written = 0, refused_long = 0, not_holder = 0, lua_failed = 0; // the process that applies moves
};

struct Rebuild
{
	u16 host_id = kNoId;
	xr_vector<u16> children; // local numbering, also used to cancel release marks
	u32 epoch = 0; // the world it belongs to
	u64 at = 0; // destroyed / last asked
	u32 asked = 0;
};

// The host's rate buckets of a player's requests.
enum Cat : u8
{
	CatSpawn,
	CatMove,
	CatDestroy,
	CatHit,
	CatMisc,
	CatCount
};

struct HostCounters
{
	u64 results[u32(Reason::Count)] = {};
	u64 dropped_joining = 0, malformed = 0, hits_applied = 0, proxy_hits = 0, player_hits_sent = 0, spawns_local = 0;
	u64 explosion_hits_sent = 0; // blasts / fragments no creature set off (a barrel, a mine, an anomaly)
	u64 friendly_fire_dropped = 0; // hits of one player on another dropped at the character (proxy or host actor)
	u64 explosions_as_player = 0; // charges set off as their planter (coop.explode_as)
};

// A player's request as the host handles it.
struct Req
{
	PeerId from = kInvalidPeer;
	u32 seq = 0;
	u16 proxy = kNoId;
	Msg type = Msg::ReqResult;
};

constexpr u32 kSpawnVerdicts = u32(spawn::SpawnVerdict::WrongParent) + 1;

struct HostLeaseCounters
{
	u64 messages = 0, spawns = 0, clones = 0, not_his = 0, acks = 0, ack_ids = 0, ack_not_his = 0, bad_acks = 0;
	u64 short_of_ids = 0;
};

struct AdmissionCounters
{
	u64 asked = 0, let_in = 0, refused = 0, too_many = 0, no_rule = 0, failed = 0, undone = 0;
	u64 player_asked = 0, player_refused = 0, player_failed = 0; // moves into a player's character
};

struct UpgradeCounters
{
	u64 verdicts[ledger::kUpgradeVerdictCount] = {};
	u64 without_mechanic = 0; // requests that named no mechanic (a script installed the upgrade)
	u64 bought = 0, charged = 0, already = 0, engine = 0, no_cost = 0, not_his = 0, not_alive = 0, not_mechanic = 0;
	u64 answered_dropped = 0; // requests host_begin drops silently, answered here
};

struct SilencerReceipt
{
	u32 seq;
	u16 item;
	std::string section;
	Reason reason;
};

struct Launch
{
	u16 launcher = kNoId, rocket = kNoId; // local numbering
	Fvector pos{}, dir{}, vel{}, ang{};
};

// Script moves of this process (host_transfer_item): refused with a line, done already, of an item being destroyed, to the holder
// the item has (the stock pair). coop_status.
struct ScriptMoveCounters
{
	u64 refused = 0, already = 0, dying = 0, in_place = 0;
};

// ---------------------------------------------------------------- coop_requests.cpp: the outbox and the host's gate
extern lua_errors::LogBudget g_log;
extern xrCriticalSection g_out_lock;
extern xr_vector<Action> g_outbox;
extern ClientCounters g_cl;
extern HostCounters g_h;
bool note_bad(Bad kind);
CGameObject* valid_go(u16 id);
bool is_player(u16 id);
void push(Action&& a);
u32 track(Msg type, u16 item);
bool is_removal(Act kind);
bool is_removal(Msg type);
bool allow(PeerId peer, Cat c);
void notify_queued(const Req& q, u16 id);
void reply(const Req& q, Reason why, u16 subject);
bool host_drops_requests_of(PeerId from);
bool host_begin(PeerId from, Reader& r, Msg type, Req& q);

// ---------------------------------------------------------------- coop_requests_hits.cpp
void write_hit(Writer& w, const SHit& h, u16 who, u16 weapon);
bool client_hit_perks(u16 local_target, SHit& h);
void client_on_player_hit(PeerId, Reader& r);
void host_on_hit(PeerId from, Reader& r);

// ---------------------------------------------------------------- coop_requests_leases.cpp
extern lease::ClientPool g_pool;
extern lease::SpecBook g_specs;
extern xr_map<u16, Action> g_spec_actions;
extern xr_map<CSE_Abstract*, LeasedTemp> g_temp_leases;
extern xr_vector<u16> g_give_back;
extern xr_vector<u16> g_give_back_wait;
extern u32 g_pool_world;
extern lease::Held<Action> g_held;
extern RemovalBook g_removals;
extern lease::DestroyPacer g_destroy_pacer;
extern lease::SpawnPacer g_spawn_pacer;
extern xr_deque<Action> g_spawn_backlog;
extern LeaseCounters g_lc;
extern xrCriticalSection g_quarantine_lock;
extern xr_set<CSE_Abstract*> g_quarantine_marks;
extern lease::Quarantine<CSE_Abstract*> g_quarantine;
extern lease::HostLeases g_leases;
extern HostLeaseCounters g_hl;
std::array<u16, 3> lease_targets(const Action& a);
void mark_lease_rollback_locked(u16 id, xr_vector<u32>& holds);
bool lease_world_ready();
void lease_sync_world_locked();
u16 take_lease(const char* section, u16 parent);
void name_with_id(CSE_Abstract* e);
bool create_leased(CSE_Abstract* e, u16 id, u16 source);
bool take_temp_lease(CSE_Abstract* e, LeasedTemp& out);
bool clone_source(u16 id);
bool can_queue_mutation_locked(u16 id, bool mirror_live);
void return_unused_lease(u16 id);
void rollback_leased(u16 id, const char* why);
void quarantine_expire(bool all);
void client_pump_leases();
void client_on_lease(PeerId from, Reader& r);
void host_sync_leases();
void host_free_ids(const std::vector<u16>& ids);
void host_pump_leases();
void host_on_lease_ack(PeerId from, Reader& r);
Reason host_lease_checks(const Req& q, u16 parent, u16 id, u16 source, const char* section, u32& source_cost);
void host_leased_made(const Req& q, u16 id, u16 created, u16 source);

// ---------------------------------------------------------------- coop_requests_clone.cpp
extern cloneslot::Book g_clone_slots;
extern xr_map<u16, replacement::Snapshot> g_clone_prepared;
extern xr_map<std::pair<PeerId, u16>, std::vector<u8>> g_clone_committed;
extern xr_set<u16> g_clone_confirmed, g_clone_canonical;
extern u32 g_clone_world;
bool clone_variant(LPCSTR source, LPCSTR clone);
void free_source_slot(u16 clone, u16 source);
void clone_slots_frame(u64 now);
bool clone_state(u16 source, u16 target, const u8* data, size_t size, std::vector<u8>& out);
Reason host_replace(const Req& q, CSE_Abstract* entity, u16 parent, u16& created, u16 target, u16 source,
    const u8* bytes, size_t size);
bool clone_committed(PeerId peer, u16 target, const u8* bytes, size_t size);

// ---------------------------------------------------------------- coop_requests_spawn.cpp
extern spawn::SpawnPolicy g_spawn_policy;
extern xr_set<shared_str> g_spawn_allow, g_spawn_deny;
extern spawn::BudgetBook g_spawn_budgets;
extern u64 g_spawn_refused[kSpawnVerdicts];
extern u64 g_spawn_quest_gifts;
extern spawn::SpawnQueue<spawn::QueuedRequest> g_spawn_queue;
extern u64 g_spawn_queued, g_spawn_dequeued, g_spawn_queue_dropped;
bool spawnable_section(const char* section);
spawn::Missing missing_item_line(const char* section, const spawn::ClassRule& rule);
CSE_Abstract* make_temp(const char* section, const Fvector& pos, u32 level_vertex, u16 game_vertex, u16 parent);
std::string spawn_budget_key(u16 proxy);
void load_spawn_settings();
spawn::SectionFacts spawn_facts(const char* section);
Reason host_spawn_holder(const Req& q, u16 parent);
Reason host_spawn(CSE_Abstract* e, u16 parent, u16& created, u16 leased = kNoId);
float talk_distance(u16 npc, u16 proxy);
CSE_Abstract* build_spawn(const Req& q, u16 parent, const char* section, const float pos[3], const u8* state,
    size_t state_size, Reason& why);
void drop_queued(spawn::QueuedRequest&);
void drop_leased_queued(PeerId peer);
void host_pump_spawns();
void host_on_spawn(PeerId from, Reader& r);

// ---------------------------------------------------------------- coop_requests_moves.cpp
extern CarryCounters g_carry;
extern admission::Ledger g_admission;
extern AdmissionCounters g_ac;
extern ScriptMoveCounters g_script_moves;
std::string client_carry(u16 local_item);
void host_on_move(PeerId from, Reader& r, Msg type);
Reason host_destroy(const Req& q, u16 item);
void host_on_destroy_req(PeerId from, Reader& r, Msg type);

// ---------------------------------------------------------------- coop_requests_item_events.cpp
extern xr_set<u16> g_detaching_silencers;
extern xr_map<u16, Rebuild> g_rebuilds;
extern u64 g_rebuilt, g_rebuild_lost;
extern UpgradeCounters g_upgrades;
extern xr_map<PeerId, xr_deque<SilencerReceipt>> g_silencer_receipts;
extern u32 g_silencer_world;
float silencer_state(CWeaponMagazined* weapon, bool commit);
void client_rebuild_item(u16 local_item);
void client_pump_rebuilds();
void host_on_item_event(PeerId from, Reader& r);

// ---------------------------------------------------------------- coop_requests_rockets.cpp
extern xr_vector<Launch> g_launches;
extern xr_map<PeerId, u64> g_last_launch;
void client_flush_launches();
void host_on_launch(PeerId from, Reader& r);

// ---------------------------------------------------------------- coop_requests_lua.cpp
extern xr_vector<CSE_Abstract*> g_lua_temps;
extern xr_set<CSE_Abstract*> g_lua_forwarded;
extern u32 g_temps_world;

// ---------------------------------------------------------------- coop_local_zones.cpp
extern zones::Book g_zones;
CSE_Abstract* local_zone_create(const char* section, const Fvector& pos, u32 level_vertex, u16 game_vertex);
bool local_zone_release(CSE_Abstract* object);
} // namespace requests
} // namespace coop
