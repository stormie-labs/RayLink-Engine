// Anomaly Coop — split levels, "multilevel" (docs/SPLIT_LEVELS.md): the registry of who stands on which level.
//
// Every player may be on another map (the server mode only). The world server holds the world, offline
// for every map, and leads no level itself: every level with players is computed by a background map server on the world server's PC
// (coop_mapserver.h): a client never computes anything, he mirrors the authority of his level. This module answers "which level is
// this?" and "who is on it?" for the rest of the coop layer; the roles of a process (who may act as the world server, as the
// authority of a map, as a player, as a mirror) are in xrCoop/coop_roles.h.
//
// The host keeps the table (xrCoop/coop_levels_rules.h Table) and sends it to the players and the map servers (LevelMap) when it
// changes and every 5 s. The marks "online at a map server" are coop_levels_alife.cpp, the transfers coop_levels_xfer.cpp. In the
// co-op (no world server: split levels off) nothing here runs and no LevelMap goes out: every question has the answer of a session
// with one level for everybody.
//
// Lua: coop.levels.split(), my_level(), level_of_player(peer), players_on(level), online_at_server(id), holder_of(id),
// recall(squad[, ms]); coop.is_world_server(), is_player(), is_mirror() (the co-op host's process is the world server and a player;
// a client is a player and a mirror; a map server is the authority of its map).
// Console: the server's (srv/coop_srv_console.cpp): srv_max_map_servers, srv_mapserver_linger, srv_levels_status, srv_levels_audit.
#pragma once

#include "xrCoop/srv/coop_levels_logic.h"
#include "xrCoop/srv/coop_levels_wire.h"
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

namespace accept // xrCoop/srv/coop_levels_accept.h: the facts of a journal entry and the decision on a body report (host_take_body)
{
struct Facts;
struct BodyDecision;
} // namespace accept

// Machine settings (console variables).
// 0..7, default 6 (xrCoop/coop_levels_dynamic.h says why not 7): the most background map servers the host's PC may run (its ceiling);
// the cap in force is host_map_cap
extern int g_coop_max_map_servers;
// seconds the map emptied last stays after its last player left, default 600; every other empty map at most 60 s
// (xrCoop/coop_levels_dynamic.h linger_of)
extern int g_coop_mapserver_linger;
// Migrate the saved defaults in user.ltx once per machine to preserve subsequent player settings.
void settle_machine_settings();

// A client learns whether levels are split from the world's Welcome and answers false until then.
// The dedicated process answers true even before its session starts.
bool split();
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
// The name of a level, nullptr when the game has none.
const char* level_name(LevelId level);
// How many players of the session stand on `level` (the host among them); 0 outside a session.
u32 players_on(LevelId level);
// Host: the object is online at a map server (the world server holds it online without a game object): coop_levels_xfer.cpp.
bool online_at_server(u16 id);
// Host: the bridge of the map server that holds the object, or the squad it is a member of; kInvalidPeer when none does.
PeerId holder_of(u16 id);
// A map server: its holder authors the object (a unit granted to it or born under its slice, also while it is frozen for its
// release): the writer rule's Hold of a unit here. False anywhere else, and while its holder is of an earlier world. True also while it hands
// everything back: a unit on its way home is its until the world's Ack, as the world's keeper says. Any thread: it takes the transfers' lock.
bool xfer_authors(u16 local_id);
// A map server made this unit (a squad, a living creature) and its Spawn waits in this frame's journal: the holder
// registers it at the flush (Holder::spawned -> SpawnPrepare), until then it is its own from birth all the same (writer::Hold::Born).
// False anywhere else, once the holder has it (xfer_authors answers), and on the same terms as xfer_authors. Any thread.
bool xfer_born(u16 local_id);
// A map server: the id is of its own pool, its slice or a block its world gave it: an object under it was made here (writer::map_seat:
// what it made at no map, a psy phantom, is its map's resident). False anywhere else. Any thread: it takes the transfers' lock.
bool xfer_made_here(u16 local_id);
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
// Host: the levels that need a map server now, its players there (xrCoop/coop_levels_rules.h Table).
std::vector<LevelId> host_map_levels();
u32 host_map_players(LevelId level);
// Host: who stands on `level`, in the world.
std::vector<u32> host_players_on(LevelId level);
// Host: everybody who is on `level` or on his way to it, and has a session (host_rejoin_level).
std::vector<u32> host_players_of(LevelId level);
// Host: LevelMap goes out at the next frame.
void host_map_changed();
// Host: the broadcasts of what a map server reported: that level's players have heard it from their map server (LevelEntity).
struct EchoOf
{
	explicit EchoOf(LevelId level);
	~EchoOf();

private:
	LevelId m_outer; // one inside another (the journal's receiver around host_apply_fact) gives the outer one back
};
// The level an EchoOf names now, wire::kNoLevel outside one. A spawn the world server sends later (its events go out
// at its frame, coop_entities.cpp) keeps the EchoOf it was made under.
LevelId echo_now();
// Runs `f` under the EchoOf of `echo`; as it is when `echo` names no level (wire::kNoLevel).
template <class F>
void with_echo(LevelId echo, F&& f)
{
	if (echo == wire::kNoLevel)
	{
		f();
		return;
	}
	const EchoOf scope(echo);
	f();
}
// Host: a new world came up, every mark of the old one is gone with it.
void host_world_replaced();
// Host: the table goes out now: a player is on his way to a level, its map server takes him from the table.
void host_send_map_now();

// --- the gates: a level's gate as the world server's LevelMap carries it (Gate::Free when it names none)
Gate gate_of_level(LevelId level);

// Module lifecycle, as every coop module (sync::install_server / install_client ...).
void install_server(Session& s); // the world server's handlers (on the hosting session)
void install_client(Endpoint& e); // the client's (a remote client's session, the host player's LocalLink)
void reset(); // session started / ended (sync::on_session_started, sync::reset)
void on_peer_left(PeerId id); // sync::on_peer_left
void frame(); // main thread, every frame (sync::frame)
void print_status(); // coop_status section (sync::print_status), console srv_levels_status (srv/coop_srv_console.cpp)
// Fills coop.levels and the role functions of coop.*; called by script::register_raw once the `coop` table exists.
void register_lua(lua_State* L);
// coop.levels.my_level() and coop.my_level() -> my_level(), nil for wire::kNoLevel (the Core's scripts ask coop.my_level, without
// the server's table coop.levels).
int lua_my_level(lua_State* L);

// --- coop_levels_xfer.cpp, the transfers (see there)
void xfer_install_server(Session& s);
void xfer_install_client(Endpoint& e);
void xfer_reset();
void xfer_host_frame();
void xfer_client_frame();
void xfer_host_connect(PeerId bridge); // a map server leads its level from now on
void xfer_host_disconnect(PeerId bridge); // it does not: what it held comes home at its last state
// The world server: what the map server of his level saw of a player's body, the last few seconds. False: nothing of late.
struct BodyFacts
{
	bool fight = false;
	bool talking = false;
	// The NPC he talks to there, the world server's numbering; 0xffff when he talks to nobody (Lua
	// coop.levels.talk_partner)
	u16 partner = 0xffff;
	u8 place = 0; // checkpoints::sources::Place
	u16 source = 0xffff;
	// The base whose ground he stands on, as the map server's geometry says, checked by the world
	// server against its own tables when it came (checkpoints::sources::host_footing); kind none when nothing or refused.
	checkpoint::base::Footing footing;
};
bool body_facts(u16 proxy, BodyFacts& out);
// The world server: the keeper generation of a unit while the map server behind `bridge` holds it (0: it does not), and the
// gen its SpawnPrepare registered the unit under when that server made it (0: it did not).
u32 xfer_held_gen(u16 root, PeerId bridge, u32& spawn_gen);
// The world server: may the map server behind `bridge` make an object under `id`? Of its slice, or of a block this
// world gave it for an IdAsk. The acceptance of its Spawn asks it.
bool host_id_is_senders(PeerId bridge, u16 id);
// The world server: the map server behind `bridge` holds `squad`, and `member` left it there alive (its journal's Left,
// accept::leave_plan Adopt):
// the member is held by that map server from now on, under the squad's gen, with `state` (the world's copy of it now). False: that
// map server does not hold the squad, or the member is somebody else's (the Left then takes today's way home).
bool host_adopt_member(PeerId bridge, u16 squad, u16 member, const std::vector<u8>& state);
// The world server: a creature the map server behind `bridge` held on its own joined a squad it writes (its journal's
// Joined): its own hold ends, its mark goes without bringing it home (it is a member of a squad held away now). No hold: nothing.
void host_member_joined(PeerId bridge, u16 member);
// The world server: a map server's report of a player's body (the State layout after its op: proxy, placement, LevelBodyFacts) with
// the acceptance's facts of its entry (srv/coop_levels_alife.cpp host_apply_fact). It reads the report, decides it by
// accept::body_report and places his character when Taken; a Crossed or a Refused by the verdict on the body writes its own line
// (a Refused by the acceptance is the caller's).
accept::BodyDecision host_take_body(PeerId from, LevelId level, Reader& r, const accept::Facts& f);
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
// A map server counts what it holds and hands it all back to the world server. It stops only when nothing remains.
u32 xfer_held();
void xfer_hand_back(u64 now);
// What the binders of a level's static objects keep (xrCoop/coop_levels_logic.h).
// A map server, right before an object's binder updates: the first look at the object comes here, before its logic can have done
// anything. False: its binder does not run, the hand-over is over. True anywhere else.
bool logic_may_update(CGameObject* object);
// A map server, the last thing of its hand-over: what changed since the last round goes to the world server, no binder runs after it.
void logic_hand_over();
bool logic_looking(); // Lua coop.levels.logic_look: the save a binder is asked for now is a look, not a save
// The world server: `level` said what its objects keep at `at` or later, or gave a Done record of a numbered op then: a save of that moment does not have it, so the level's next map server is served a fresh save.
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
void host_unmark(u16 id, const std::vector<u8>& state); // it comes home with this state
void host_bind_squads(u16 id); // an object of a map server came: a squad's members are bound as they come
void host_apply_state(u16 id, const std::vector<u8>& state); // a state from its map server while it stays its
void host_gone(u16 id); // its map server destroyed it
// Host: what a map server reports.
void host_apply_parent(u16 item, u16 parent, const Fvector& pos);
void host_apply_death(u16 victim, u16 killer, const std::vector<u8>& state);
bool host_body_stays(u16 id); // host_apply_death of this object is running and its body stands on a map server
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
