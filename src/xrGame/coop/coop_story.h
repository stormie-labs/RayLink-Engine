// Anomaly Coop — shared group story after join: info portions, tasks, relations, PDA map spots,
// and each player's rank / reputation / community for its character on the other machines
// (SPEC R-7.1..R-7.8, R-1.7, R-3.4, P1, P5, P6).
//
// The join save gives a client the host's story as of the save. This module keeps it in sync after that.
// The host is the only authority (P1):
//   host change    -> applied on the host -> broadcast to every client.
//   client change  -> made by the client's own scripts (dialogs, PDA, binders) -> NOT applied there,
//                     sent to the host as a request -> host applies -> broadcast to everybody,
//                     the sender included.
// While a client applies a host broadcast it is inside the "applying" guard: the change being applied
// goes through, and anything the client's scripts do in reaction (callbacks) is dropped, because the
// host's own reaction arrives as its own broadcast. Nothing done inside the guard is sent to the host.
// Exceptions, by design:
//   - Map spots are applied locally at once (the engine API must return the new spot) and forwarded.
//   - Task texts, targets and spots are computed by the host only (R-7.3): a client's own script
//     updates of an existing task are dropped and overwritten with the host values.
//   - A client evaluates task completion only for tasks its own scripts track (tasks it had at join or
//     asked for itself); the result goes to the host as a request.
// Every 10 s the host sends a count + hash per domain; a client that differs asks for the full list
// (P5). Ids on the wire are in host numbering (join::to_host / join::to_local).
//
// This header is included from engine files (info portions, tasks, relations, map spots):
// it has no includes, only forward declarations and engine base types.
#pragma once

class CInventoryOwner;
class CGameTask;

namespace coop
{
class Session;

namespace story
{
void install(Session& s);
void frame(); // main thread, every frame while a session is active
void reset(); // session started / ended / world replaced
void print_status();

// True while this machine applies a host broadcast (client) — engine changes made now are not
// forwarded to the host.
bool applying();

// --- info portions (inventory_owner_info.cpp) ---
// Before an info portion of `owner` is given (give = true) or disabled. Only the local actor's
// infos are the group story. Returns true if the engine must not apply it on this machine.
bool on_info(const CInventoryOwner* owner, const shared_str& info_id, bool give);

// --- tasks (GameTaskManager.cpp, GameTask.cpp) ---
// GiveGameTaskToActor: true = do not give it here (a client asked the host; the object is kept
// because scripts hold it, and becomes the task when the host confirms).
bool on_give_task(CGameTask* t, u32 time_to_complete, u32 timer_ttl);
// SetTaskState: true = do not change it here. explicit_call = by id from a script, false = the
// engine's own completion check.
bool on_task_state(CGameTask* t, int state, bool explicit_call);
// UpdateTasks loop: true = do not evaluate this task's completion checks on this machine.
bool skip_task_check(CGameTask* t);
// CGameTask::SetTaskState right after the state is stored: true = return at once (a client applies a
// host result; infos, script functions and callbacks already ran on the host).
bool task_state_silent(CGameTask* t);
// ChangeMapLocation / RemoveMapLocations(false): true = ignore it (task spots belong to the host).
bool on_task_map_change(CGameTask* t);

// --- relations (relation_registry*.cpp) ---
enum class GoodwillOp : u8
{
	Set,
	Change,
	Force,
};
// Personal goodwill `from` -> `to`. Returns true if the engine must not write it here.
bool on_personal_goodwill(u16 from, u16 to, int value, GoodwillOp op);
// Goodwill of a community toward a character (op Set or Change).
bool on_community_goodwill(int community, u16 to, int value, GoodwillOp op);
// Community-to-community relation table.
bool on_community_relation(int community_from, int community_to, int value);
// Engine combat bookkeeping (RELATION_REGISTRY::Action): its goodwill changes are computed by the host
// from the replicated hit/kill, so a client drops its own.
struct RelationActionScope
{
	RelationActionScope();
	~RelationActionScope();
	RelationActionScope(const RelationActionScope&) = delete;
	RelationActionScope& operator=(const RelationActionScope&) = delete;
};

// --- PDA map spots (map_manager.cpp) ---
void on_spot_added(const shared_str& spot_type, u16 id); // after the spot is in the list
void on_spot_remove(const shared_str& spot_type, u16 id); // before it is removed
void on_spots_remove_all(u16 id); // before every spot of the object is removed

// R-7.6 support for reward routing (host): the client peer whose request finished the task last,
// 0xffffffff if no client did (the host's own completion). The host's scripts still run the reward;
// who receives it is a script-side decision.
u32 task_finisher(LPCSTR task_id);
} // namespace story
} // namespace coop
