// Anomaly Coop — call router gate (MOD_LAYER.md §2.3, F2 T12). The one place where a client decides what an engine
// function called from Lua does there: run it (the stock code), send it to the host (the function's request path) or
// refuse it. The classes are xrCoop/coop_calls.h (kCalls, a row for every export) with the [call_classes] overrides of
// configs\coop_route*.ltx and coop_compat_*.ltx; this module finds the target's owner, checks the arguments, loads the
// overrides, counts and logs.
//
// A gated engine function asks first and does what the answer says:
//   switch (callgate::client_gate("game_object:give_money", &object())) { Run: stock; Request: path; Refuse: return }
// Gated now (each hook says "Anomaly Coop (F2 T12 ...)"):
//   alife_simulator_script.cpp  alife:create, create_ammo, clone_weapon, register, release, kill_entity,
//                               teleport_object, set_switch_online, set_switch_offline (coop_requests.cpp lua_*);
//                               round 1: give_info, disable_info (our actor: the story path), set_interactive,
//                               add/remove_in/out_restriction, remove_all_restrictions
//   level_script.cpp            level:set_weather, set_weather_fx, set_weather_smooth, set_weather_weight,
//                               start_weather_fx_from_time, stop_weather_fx, change_game_time (coop_worldstate.cpp);
//                               round 1: set_game_difficulty, spawn_item, spawn_phantom
//   script_game_object*.cpp     game_object:transfer_item, give_money, kill, set_npc_position, give_info_portion,
//                               disable_info_portion, set_relation, set_goodwill, force_set_goodwill, change_goodwill,
//                               set_community_goodwill, attach_vehicle (a car, coop_events.cpp client_refuse_vehicle);
//                               round 1: transfer_money, take_item, enable_level_changer, the properties health,
//                               psy_health, power, satiety, radiation, morale, bleeding, change_health, set_health_ex,
//                               buy_supplies, destroy_object
// Every other export runs as in single player; its row only documents it until it gets a hook.
//
// Host, single player and a client before the host's world (its own world still runs while the host's downloads):
// always Run, nothing counted. A client in the host's world (requests::lua_client: LoadingWorld, Live): calls::decide.
//   - Arguments that fail (a position that is not a number, an id this world does not have) are refused.
//   - While the client applies a host command (entities::client_applying, story::applying) a call runs: it shows the
//     host's change (the event router still drops echoes, R4 4). Args::runs_while_applying = false keeps the class
//     rules then (the alife() spawn and ALife gates, weather and time: their behaviour before T12).
//   - Else the class matrix: Run, Request or Refuse.
// Each Request and Refuse is counted by name; the first 10 refusals of each name are logged with the name, the rest
// only counted (coop_status: "calls: refused N, requested N" and the ten most refused names). Requested counts what
// went down a request path; a path that still refuses (note_refused) counts as refused as well.
//
// Threading: Lua calls come from one Lua thread at a time (main or the GameThread worker); print_status comes from the
// console. One lock guards the overrides and the counters.
#pragma once

#include "xrCoop/coop_calls.h"

class CObject;

namespace coop
{
namespace callgate
{
using calls::Gate;

// Arguments the gate checks on a client before the call runs or goes to the host (P8, review R6 I-2: NaN passed the
// clamps and the id checks). Unset fields are not checked.
struct Args
{
	const Fvector* pos = nullptr; // must be finite (validate::vec_finite)
	u16 id = u16(-1);             // must name an object of this machine's world (events::find_entity)
	const char* what = nullptr;   // for the log line only: the section of a spawn, ...
	bool runs_while_applying = true; // a host command being applied runs (false: the class rules still apply)
};

// The decision for a call on `target`: own when the target is our actor or under it (an item in our inventory, an
// attachment of it); nullptr = no target, the call acts on this machine (level functions): own.
Gate client_gate(const char* name, CObject* target, const Args& args = Args());
// The same by object id (alife() functions): 0xffff = no target (own).
Gate client_gate_id(const char* name, u16 target, const Args& args = Args());
// The same with the owner decided by the caller (transfer_item: the item or the recipient is ours; alife:create: the
// parent is ours).
Gate client_gate_own(const char* name, bool own_target, const Args& args = Args());

// Our actor or an object under it (an item in our inventory). False for 0xffff and for objects not in this world.
bool is_own(u16 id);
// destroy_object's own target (calls::own_destroy): as the event router's on_destroy - ours, a free item (no parent, not
// a creature) or an object outside the host's world.
bool own_destroy(u16 id);

// A request path refused a call the gate let through (alife:register of an object the host did not create, ...):
// counted and logged like the gate's own refusals.
void note_refused(const char* name, const char* why);

// Once per Manager: no overrides loaded yet (the first client decision reads them), counters zero.
void install();
// Session started or ended: the counters and the log budget start again. The overrides stay.
void reset();
// coop_status: overrides, refused / requested / ran, the ten most refused names.
void print_status();
// Reads [call_classes] of the ltx files unless done already. Called by the first client decision; safe to call again.
void load_overrides();
} // namespace callgate
} // namespace coop
