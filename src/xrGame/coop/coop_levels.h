// Anomaly Coop — split levels, "multilevel" (F3a, docs/SPLIT_LEVELS.md): the registry of who stands on which level.
//
// Every player may be on another map. The host's process is the world server: it holds the world, offline for every map, and
// computes the level the host stands on. Every other level with players is computed by a background map server on the host's PC
// (coop_mapserver.h): a client never computes anything, he mirrors the authority of his level. This module answers "which level is
// this?" and "who is on it?" for the rest of the coop layer; the roles of a process (who may act as the world server, as the
// authority of a map, as a player, as a mirror) are in xrCoop/coop_roles.h.
//
// The host keeps the table (xrCoop/coop_levels_rules.h Table) and sends it to the players and the map servers (LevelMap) when it
// changes and every 5 s. The marks "online at a map server" are coop_levels_alife.cpp, the transfers coop_levels_xfer.cpp. With
// coop_split_levels off nothing here runs and no LevelMap goes out: every question has the answer of a session with one level for
// everybody.
//
// Lua: coop.levels.split(), story_together(), my_level(), level_of_player(peer), players_on(level), led_by_host_process(level),
// online_at_server(id), holder_of(id), recall(squad[, ms]); coop.is_world_server(), is_map_authority(), is_player(), is_mirror() (the host's process is the world server,
// the authority of its map and a player; a client is a player and a mirror; a map server is the authority of its map).
// Console: coop_split_levels, coop_story_together, coop_max_map_servers, coop_mapserver_linger, coop_levels_status,
// coop_levels_audit.
#pragma once

#include "xrCoop/coop_levels_logic.h"
#include "xrCoop/coop_levels_wire.h"
#include "xrCoop/coop_protocol.h"

#include <string>
#include <vector>

struct lua_State;
class CGameObject;
class CSE_ALifeDynamicObject;

namespace coop
{
class Session;
class Endpoint;

namespace levels
{
// A game graph level id (CGameGraph level_id(), a u8 in the engine) as the protocol carries it (u16, xrCoop/coop_levels_wire.h).
// "No level" is wire::kNoLevel (0xffff); coop_levels_rules.h has a kNoLevel of its own in this namespace, so this one is
// not repeated here.
using LevelId = u16;

// Game rules and machine settings (console variables; the rules are set per game by coop_game_schema, plan §3).
extern int g_coop_split_levels; // 1 = every player on his own map, 0 = one map for the group, as in F3 (default)
extern int g_coop_story_together; // 1 = the story levels are entered together (default), 0 = apart as any level
// 0..7, default 6 (xrCoop/coop_levels_dynamic.h says why not 7): the most background map servers the host's PC may run (its ceiling);
// the cap in force is host_map_cap
extern int g_coop_max_map_servers;
// seconds the map emptied last stays after its last player left, default 600; every other empty map at most 60 s
// (xrCoop/coop_levels_dynamic.h linger_of)
extern int g_coop_mapserver_linger;
// The two settings above are saved: the old defaults (3 servers, 60 s) of a machine that ran an earlier build move to build 50's, once.
void settle_machine_settings();
extern int g_coop_soft_host_move; // 1 = the host's move reloads nobody on a map server's level (default), 0 = everybody, as before

// Split levels are on: the host's rule; a client knows it from the host's LevelMap (off until one comes).
bool split();
bool story_together(); // the story levels are entered together (meaningful with split())
// This process is a map server (coop_manager.h is_map_server): it computes its level and asks the world server for the rest.
bool leads_my_level();

// The level this process has loaded: wire::kNoLevel outside a loaded level.
LevelId my_level();
// The level a player stands on (a peer of the session; the host is kHostPeer): wire::kNoLevel if he is none of ours or his level is
// not known. Without split levels every player is on my_level().
LevelId level_of_player(PeerId peer);
// The level of a game graph vertex / of an object of the world (by its id in the world server's numbering):
// wire::kNoLevel when unknown.
LevelId level_of_vertex(u16 game_vertex);
LevelId level_of_object(u16 id);
// The host's process leads `level` (it is the level the host stands on).
bool led_by_host_process(LevelId level);
// The name of a level, nullptr when the game has none.
const char* level_name(LevelId level);
// How many players of the session stand on `level` (the host among them); 0 outside a session.
u32 players_on(LevelId level);
// Host: the object is online at a map server (the world server holds it online without a game object): coop_levels_xfer.cpp.
bool online_at_server(u16 id);
// Host: the bridge of the map server that holds the object, or the squad it is a member of; kInvalidPeer when none does.
PeerId holder_of(u16 id);
// A map server: the world server recalls a squad (coop_npc_companions.script on_recall, the world server's word only): it goes offline
// here now whoever stands by it, goes home, and is not asked for again for `ms` (a pin, kept short so that nothing stays offline for
// good). `ms` 0 lifts the pin. False anywhere else, or for an object that is no creature or squad. Main thread or the game thread.
constexpr u32 kRecallPinMs = 10000;
constexpr u32 kRecallPinMaxMs = 30000;
bool recall(u16 local_id, u32 ms);

// --- the registry, the host's side (no effect without split levels)
// Host: the player stands on the host's level (also when he is not in the table: a joining player of a session without split
// levels). The test of every broadcast of the host's level (net::host_broadcast).
bool on_host_level(PeerId peer);
// Host: a player's character stands on another level: it has no game object here, its server entity is all there is.
bool host_character_elsewhere(u16 character);
// Host: does a broadcast of type `type` go to `peer`?
bool passes_to(Msg type, PeerId peer);
// Host: a join reached the player's character, which stands on `level`. True: he stays there and loads it. False: the group is
// one (split levels off), or that level cannot have a map server now: he goes to the host's level (join::bring_proxy_to_level).
bool host_player_loads(PeerId peer, LevelId level);
// Host: he finished loading (WorldLoaded).
void host_player_in_world(PeerId peer);
// Host: the verdict on a move of `peer` to `target` (xrCoop/coop_levels_dynamic.h decide_move: a new map server, the cap). At the cap
// the map server empty the longest is told to stop now and the move goes; refused only when every map server has a player.
MoveVerdict host_judge_move(PeerId peer, LevelId target);
// Host: how many map servers may run now: min(players + 1, coop_max_map_servers, 7) (xrCoop/coop_levels_dynamic.h map_server_cap).
// `mover`: a player who is not seated yet (a join) counts too. host_map_players_now: the players of that count, the mover apart.
u32 host_map_cap(PeerId mover = kInvalidPeer);
u32 host_map_players_now();
// Host: his character goes to `target` now.
void host_player_moves(PeerId peer, LevelId target);
// Host: the host himself goes to `level`: his seat goes first, so that a level he goes to needs no map server, and stays there until the
// new world is up. wire::kNoLevel: he stays after all.
void host_going_to(LevelId level);
// Host: the levels that need a map server now, its players there (xrCoop/coop_levels_rules.h Table).
std::vector<LevelId> host_map_levels();
u32 host_map_players(LevelId level);
// Host: who stands on `level`, in the world.
std::vector<u32> host_players_on(LevelId level);
// Host: everybody who is on `level` or on his way to it, and has a session (host_rejoin_level).
std::vector<u32> host_players_of(LevelId level);
// Host: how many map servers the world needs when the host stands on `level`.
u32 host_servers_if_host_on(LevelId level);
// Host: the host may go away alone through an exit to `level`: split levels are on and that level is not entered together.
bool host_goes_alone(LevelId level);
// Host: a player's seat is on his own level again, he loads it.
void host_player_reloads(PeerId peer);
// Host: LevelMap goes out at the next frame.
void host_map_changed();
// Host: the broadcasts of what a map server reported: that level's players have heard it from their map server (LevelEntity).
struct EchoOf
{
	explicit EchoOf(LevelId level);
	~EchoOf();
};
// Host: a new world came up, every mark of the old one is gone with it.
void host_world_replaced();
// Host: the table goes out now: a player is on his way to a level, its map server takes him from the table.
void host_send_map_now();

// --- the gates (Lua: coop.levels.set_gate, gate_of)
Gate gate_of_level(LevelId level);

// Module lifecycle, as every coop module (sync::install_server / install_client ...).
void install_server(Session& s); // F3a T4: the world server's handlers (on the hosting session)
void install_client(Endpoint& e); // F3a T4: the client's (a remote client's session, the host player's LocalLink)
void reset(); // session started / ended (sync::on_session_started, sync::reset)
void on_peer_left(PeerId id); // sync::on_peer_left
void frame(); // main thread, every frame (sync::frame)
void print_status(); // coop_status section (sync::print_status), console coop_levels_status
void register_console(); // from coop::register_console_commands
// Fills coop.levels and the role functions of coop.*; called by script::register_raw once the `coop` table exists.
void register_lua(lua_State* L);

// --- coop_levels_xfer.cpp, the transfers (see there)
void xfer_install_server(Session& s);
void xfer_install_client(Endpoint& e);
void xfer_reset();
void xfer_host_frame();
void xfer_client_frame();
void xfer_host_connect(PeerId bridge); // a map server leads its level from now on
void xfer_host_disconnect(PeerId bridge); // it does not: what it held comes home at its last state
// The host's move (xrCoop/coop_soft_move.h, coop_xfer.h R13): a new simulator under the book that stays. End of its constructor: what the
// map servers hold is marked away before its first update, and the journal of its save is not read.
void xfer_host_carry_over();
// A map server: the world server is away: nothing about the objects goes up but what has to (the stream, the bodies, the audit).
void xfer_quiet(bool quiet);
// The world server: what the map server of his level saw of a player's body, the last few seconds. False: nothing of late.
struct BodyFacts
{
	bool fight = false;
	bool talking = false;
	u8 place = 0; // checkpoints::sources::Place
	u16 source = 0xffff;
};
bool body_facts(u16 proxy, BodyFacts& out);
void xfer_print_status();
void xfer_save(IWriter& out); // requirement 2: the journal of the transfers in flight, a chunk of the save (CALifeStorageManager::save)
void xfer_load(IReader& in); // of the save being loaded; the world server takes it on its first frame in that world
void xfer_audit_now(); // console coop_levels_audit: every map server's books are compared now
// The switch manager's vote on a map server (coop::client_switch_vote): true = the stock rules decide, false = offline.
// Asks the world server for a creature near a player. Game thread.
bool xfer_stock_rules(u16 local_id);
// Engine hooks on a map server (alife_switch_manager.cpp): an object went offline / left the world here.
void xfer_on_switched_offline(u16 local_id);
void xfer_on_released(u16 local_id);
// A map server, from the ALife update (it has no scheduler of its own, alife_frozen): the stock update of the offline objects it holds. An
// online squad is updated by its commander's binder as in the stock game; one that is held and offline has nobody else: the host left
// it the moment it was marked. Without this update nothing says it may not come online (sim_squad_scripted:check_online_status).
void xfer_scheduled();
// A map server: what it holds, and everything of it goes back to the world server (plan §1.1: it stops only with nothing left).
u32 xfer_held();
void xfer_hand_back(u64 now);
// What the binders of a level's static objects keep (xrCoop/coop_levels_logic.h).
// A map server, right before an object's binder updates: the first look at the object comes here, before its logic can have done
// anything. False: its binder does not run, the hand-over is over. True anywhere else.
bool logic_may_update(CGameObject* object);
// A map server, the last thing of its hand-over: what changed since the last round goes to the world server, no binder runs after it.
void logic_hand_over();
bool logic_looking(); // Lua coop.levels.logic_look: the save a binder is asked for now is a look, not a save
// The world server: `level` said what its objects keep at `at` or later: a save of that moment does not have it.
bool logic_came_since(LevelId level, u64 at);
// A map server: what happened on its level (the engine's hooks of the host). No effect anywhere else, nor while it applies what the
// world server told it.
void up_parent(u16 item, u16 new_parent);
void up_died(u16 victim, u16 killer);
void up_spawned(u16 id);
void up_money(PeerId peer, s64 delta, bool trade); // a deal of a player of its level: the world server's book
void up_tent(u16 tent, u16 character); // a tent it put up for a player of its level: after the tent's own spawn, in that order
void up_stash(u16 character, bool things_stay); // his retirement here is over: after what became of his things, in that order
// xrServer::PerformIDgen: this process takes new ids from its own slice (xrCoop/coop_levels_keeper.h leader_ids_of).
bool leader_takes_ids();
u16 leader_free_id();

// --- coop_levels_alife.cpp, "online at a map server" on the host's ALife and the state of a transferred object
// Does the host simulate this kind of object offline, so that a map server has to take it? A living creature or a squad.
bool transfers(const CSE_ALifeDynamicObject* e);
logic::Kind logic_kind(CSE_ALifeDynamicObject* e); // what the rules of coop_levels_logic.h ask of an entity
// The state a transfer carries (placement, M_UPDATE of the object, the same of a squad's members).
std::vector<u8> state_of(CSE_ALifeDynamicObject* e);
// state_of with the Lua logic of the object and its members (coop_levels_brain.script): when it changes hands, not in the stream.
std::vector<u8> state_with_logic(CSE_ALifeDynamicObject* e);
void logic_put(const char* bytes, size_t size); // Lua: coop.levels.logic_put
const std::string& logic_bytes(); // Lua: coop.levels.logic_bytes
std::vector<u16> squads_at(PeerId bridge); // the world server: the squads that map server holds
// Host. May the map server of `level` take it now? It is a top-level object offline here, on that level.
bool host_may_give(const CSE_ALifeDynamicObject* e, LevelId level);
void host_mark(u16 id); // it leaves the host's offline simulation, no game object
void host_mark_in_new_world(u16 id); // the same from the end of a simulator's constructor: no level is up yet (world_up), the registries are
void host_unmark(u16 id, const std::vector<u8>& state); // it comes home with this state
void host_bind_squads(u16 id); // an object of a map server came: a squad's members are bound as they come
void host_apply_state(u16 id, const std::vector<u8>& state); // a state from its map server while it stays its
void host_gone(u16 id); // its map server destroyed it
// Host: what a map server reports.
void host_apply_parent(u16 item, u16 parent, const Fvector& pos);
void host_apply_death(u16 victim, u16 killer, const std::vector<u8>& state);
bool host_body_stays(u16 id); // host_apply_death of this object is running and its body stands on a map server
void host_apply_release(u16 id, LevelId level);
// Host: what the binder of a static object of `level` would save goes into its entity, as xrServer::Process_save would put it there. False:
// it does not (the line in the log says why).
bool host_apply_logic(u16 id, const std::vector<u8>& bytes, LevelId level);
// Host: the ids of the map servers' slices leave its generator (once per world).
void host_reserve_leader_ids(bool say_so); // the slices coop_max_map_servers allows and those in use (leader_slots_kept)
// A map server: the world server's state of a creature it was granted, into its copy (it stays offline here until the stock rules).
bool client_apply_state(u16 local_id, const std::vector<u8>& state);
// What is taken for this object: itself, or the squad that lists it when the squad came over the wire: its members are ordinary
// objects here until the squad is bound (client_apply_state binds it), the host's members go with their squad.
u16 unit_of(const CSE_ALifeDynamicObject* e);
} // namespace levels
} // namespace coop
