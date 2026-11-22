// Ядро: без проверок режима (docs/MODES.md)
// Anomaly Coop — the interface of the two modes (docs/MODES.md section 4): where the Core gets the answers that depend on the mode.
//
// The Core never asks which mode it runs in. It asks mode() about a player, a map or its own actor, sends through the mode's
// delivery and runs the mode's frame. Two layers implement Mode: the co-op (hostplay/coop_hostplay_mode.cpp: the co-op host, a co-op
// client) and the server (srv/coop_srv_mode.cpp: the world server, a map server, a player of a server world). Outside a session the
// answers are those of NoneMode below (single player).
//
// Mode& mode() is set once per session by the Manager's starts (coop_manager.cpp): start_host (the menu's, the console's, -coop_host,
// the world server's own start), start_map_server, and a client's Welcome (protocol 33: its byte says the session's mode before any
// game message of the session runs). set_phase(Off) puts none back. The two layers are named in one place only, the switchboard of
// xrGame/coop/coop_mode.cpp: it runs their boots (each reads its own keys of the command line and makes its own starts) and asks them
// which mode a start of the Core is.
//
// Pure C++17, no engine dependencies: the unit tests use it (tests/test_mode_answers.cpp, the fixed answer sets of
// tests/support/fake_mode.h). Main thread, except mode() and the H5 hooks, which the ALife may ask from its own thread: the holder is
// an atomic pointer, set and cleared on the main thread.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "coop_protocol.h"
#include "coop_writer_rules.h"

class CGameObject;
class CALifeObjectRegistry;
class CSE_ALifeHumanStalker;
struct lua_State;

namespace coop
{
class Endpoint;
class LocalLink;
class Session;
class Writer;
struct SessionConfig;
namespace levels
{
struct BodyFacts; // xrGame/coop/coop_levels.h: what the map server of his level saw of a player's body
} // namespace levels
namespace death
{
struct Marker; // xrCoop/coop_death_items.h: the marker of a player's dropped things
} // namespace death
namespace ghost
{
struct Said; // xrCoop/coop_ghost_rules.h: a relation request a level's logic made
} // namespace ghost

// Is that player's character on the map this process has loaded? Joining: he is in the session, his character is not placed in the
// world yet (he loads it, or the world server has not seated him on a map).
enum class Where : uint8_t
{
	Here,
	Away,
	Joining,
};

// Who decides for that player's character now: this process, another process, nobody (no such player, or a ghost).
enum class Decider : uint8_t
{
	This,
	Other,
	Nobody,
};

// VEH-DISC (docs/VEHICLES.md section 9; review rv_F5-VEH-7, blocker 2): how a player whose peer is no peer of this process's session
// any more left it, for the releases of what his character held here (a seat in a car). Whoever leaves the co-op host's session quit
// the game. The session of a map server's level also drops the virtual peer of a player who went to another map of its world; the
// map server tells the two apart by the table of seats its world sends (LevelMap) when it drops him.
enum class Departure : uint8_t
{
	Disconnected, // his player left the session of the world
	LeftLevel, // his player went to another map of the world
	Replaced, // his player stays on this map with another character, or loads the world again for one
};

// H-OPCMD (INTEG-C, the deferred rows of A-S6-E-story): what came of an operator's console line sent to the world server.
enum class OperatorSend : uint8_t
{
	Refused, // not a line an operator may send (op::allowed: one blank between words, no ';', a known command)
	Unreachable, // not sent: no world server to send it to, or the session would not take it now
	Sent,
};

// How this process enters a session: the question of the switchboard to the layers, never asked by the Core's code.
enum class Entry : uint8_t
{
	Host, // Manager::start_host: the menu's or the console's host, -coop_host, the world server's start
	Client, // a client's Welcome (with the session's mode it says)
	MapServer, // Manager::start_map_server
};

// The endpoints a mode installs its handler tables on (Manager: they are cleared first, as the old install_handlers did).
struct ModeTables
{
	Session& session; // the hosting session, a client's session, a map server's session of its level
	Session& world; // a map server's bridge to the world server (unused by the others)
	LocalLink& link; // the co-op host player's channel to his own process (unused by the others)
};

// The answers of a mode that depend on no player: each mode's own row, written in its layer's file.
struct ModeAnswers
{
	bool actor_is_player; // this process's own actor is a player (false: a server's ghost)
	bool decides_world; // the world: offline, time, story, money, the save
	bool leads_level; // the map this process has loaded: its objects, AI, physics
	bool group_travels; // the group moves between maps together
	uint8_t roles; // the ProcessRole flags of this process in the session (xrCoop/coop_roles.h)
};

// H-ENTITYUP: what happened to an object of a level that its world hears of (LevelSide::level_entity_up). R32 (W3, §8): the kinds
// of the map server's journal; the first four keep their values.
enum class EntityUp : uint8_t
{
	Spawned, // registered here (other: unused)
	Released, // gone here (other: unused)
	Parent, // changed hands (other: the new parent, 0xffff for none)
	Died, // died (other: the killer, 0xffff for none)
	Joined, // R32 W3: a member joined a squad (id: the squad, other: the member; stock register_member, host_on_member)
	Left, // R32 W3: a member left a squad (id: the squad, other: the member; stock unregister_member)
	Info, // R32 W3: an info portion of a non-actor object was given or taken (id: the object); the name does not ride this call
	ItemFacts, // R32 W8: the state of an item the map server writes changed (id: the item); the state does not ride this call
};

// A-S6-IF (S6): the side of a level that a process leads apart from its world: what the Core does on its own books in a process that
// decides the world, such a process does through these (a map server's: srv/coop_srv_mode.cpp, over the levels' and the map server's
// functions they replace). Mode::level() gives it; nullptr in every other process (the co-op host and client, the world server, a
// server's player, none): the Core then does it in its own books. The ids are this process's own numbering. Main thread, except
// level_entity_up, which the ALife may call from its thread (the functions behind it take their lock).
struct LevelSide
{
	virtual ~LevelSide() = default;

	// --- H-MONEY (coop_money.cpp): the book is the world's. The level keeps the last balance its world named for each player and
	// sends every change up at once.
	// A deal or a grant of `delta` for player `peer` of this level (`trade`: a trade's): up to the world, and into the kept balance.
	// A change of nothing asks the world for his balance.
	virtual void level_money_change(PeerId peer, int64_t delta, bool trade) = 0;
	// His balance as the level keeps it; false: his world named none yet (`out` 0).
	virtual bool level_balance(PeerId peer, uint32_t& out) const = 0;
	// His world named his balance (its table of the level's players: coop_trunk.cpp).
	virtual void level_balance_heard(PeerId peer, uint32_t balance) = 0;

	// --- H-ITEMTELL (coop_items.cpp): the state of an item of this level (`section`, the ItemState blob) to the players of the level.
	virtual void level_item_tell(uint16_t id, const std::string& section, const std::vector<uint8_t>& blob) = 0;

	// --- H-TENT (coop_checkpoint_sources.cpp): the camps are the world's book.
	// The tent limit of a player: `world_tents` of his stand in this process's copy of the world (the level's book has only its
	// own); true: `limit` is reached.
	virtual bool level_tent_limit(size_t world_tents, size_t limit) const = 0;
	// A tent `tent` was put up for `character`: the world hears of it (after the tent's own spawn) and makes the camp.
	virtual void level_tent_placed(uint16_t tent, uint16_t character) = 0;
	// Tent `tent` was packed up: true when the world rolls the camp on it back (its sweep), not this process.
	virtual bool level_tent_packed(uint16_t tent) = 0;
	// Is `character` in the world on this level (his join here is over)?
	virtual bool level_in_world(uint16_t character) const = 0;

	// --- H-DEATHMARK (coop_death.cpp): a death marker this level sent its players; its own Lua sets the spot (the story takes it up).
	virtual void level_marker(const death::Marker& m) = 0;

	// --- H-TRUNKPEER (coop_survival.cpp): the players of the level as its world's table names them.
	virtual uint16_t level_character_of(PeerId peer) const = 0; // 0xffff: nobody of this level
	virtual PeerId level_peer_of(uint16_t character) const = 0; // kInvalidPeer: nobody of this level

	// --- H-ENTITYUP (coop_entities.cpp, coop_events.cpp): the world hears what happened to an object of the level.
	virtual void level_entity_up(EntityUp kind, uint16_t id, uint16_t other) = 0;

	// R32 (design R32-final.md section 8a A7): the ups that carry data. The map server, the writer of these objects, puts them in
	// its level journal (Info: an info portion of a non-actor object given or taken; ItemFacts: the condition, uses, upgrades or
	// addon flags of an item it writes changed - the journal reads the item's facts from its own object when it is written).
	// Empty by default: no other process sends them.
	virtual void level_info_up(uint16_t id, const char* info, bool on) { (void)id; (void)info; (void)on; }
	virtual void level_item_facts_up(uint16_t id) { (void)id; }

	// --- H-FIGHTREP (coop_story.cpp, F5-PRG-4; INTEG-C): a fight on this level changed the reputation of `character` (this process's
	// numbering) by `delta`. His record is the world's: a player of this level gets a fact up to it, numbered in order
	// (xrCoop/coop_fight_fact.h); the world applies it and tells him. Nobody of this level: nothing.
	virtual void level_fight_reputation(uint16_t character, int delta) = 0;
};

class Mode
{
public:
	virtual ~Mode() = default;
	// For the log and coop_status: "co-op host", "world server", ... (a fixed text).
	virtual const char* name() const = 0;

	// --- P-C: where is he, who decides for him
	virtual Where where(PeerId peer) const = 0;
	// `character`: the id of a player's character in this process's own numbering (a client's own actor is 0).
	virtual Decider decider_of(uint16_t character) const = 0;
	// What the decider of his level reported of his body (the world server: the map server's facts); false: nothing of late, or this
	// process reads the object itself.
	virtual bool facts_of(uint16_t character, levels::BodyFacts& out) const = 0;

	// --- P-D, P-A: this process (ModeWithAnswers gives them from the mode's row)
	virtual bool actor_is_player() const = 0;
	virtual bool decides_world() const = 0;
	virtual bool leads_level() const = 0;
	virtual bool group_travels() const = 0;
	virtual uint8_t roles() const = 0;

	// --- P-B: delivery (net::host_broadcast, host_stream, client_send forward here; their callers do not change)
	// A world change to everyone who sees it (reliable lanes wait in the join queue of a loading player).
	virtual void deliver(Msg type, const Writer& payload, Lane lane, PeerId except) = 0;
	// An unreliable state stream, the same audience rule.
	virtual void stream(Msg type, const Writer& payload, PeerId except) = 0;
	// What this process asks of whoever decides for it.
	virtual void up(Msg type, const Writer& payload, Lane lane) = 0;

	// --- P-E: the start, the handler tables, the frame
	// The session config of a start (host_plays and the mode of the Welcome, the operator's token, a map server's timeout).
	virtual void configure(SessionConfig& cfg) const = 0;
	// The mode's own handler tables (a client's: nothing, the Core's client tables are there from its start).
	virtual void install(ModeTables& tables) = 0;
	// Every frame of the session (sync::frame).
	virtual void frame() = 0;
	// The session ended (rejected, disconnected): what a process of this mode does then.
	virtual void session_over() = 0;

	// --- H5: the stock hooks (alife_switch_manager.cpp, alife_update_manager.cpp, GameObject.cpp, xrServer.h) ask these; a mode that
	// has no map server's transfers answers "not mine". The ALife may ask them from its thread.
	virtual bool online_elsewhere(uint16_t id) const = 0; // the object is online at a map server, no game object here
	virtual bool binder_may_update(CGameObject* object) = 0; // false: the hand-over of a map server is over for its binder
	virtual void held_update() = 0; // the stock update of the offline objects a map server holds
	virtual void switched_offline(uint16_t id) = 0; // an object went offline here
	virtual bool takes_own_ids() const = 0; // new ids come from this process's own slice
	virtual uint16_t own_free_id() = 0; // the next id of that slice, 0xffff when none is free

	// --- A-S6-IF (S6): the hooks the S6 groups ask. Each answer below is "not mine", the answer of none (NoneMode keeps it); each
	// mode of the two layers writes its own answer, the same one where it has nothing to do (tests/test_mode_s6_answers.cpp).
	// The side of a level this process leads apart from its world (LevelSide above); nullptr: the Core's own books.
	virtual LevelSide* level() { return nullptr; }
	// H-REACH: does a message of type `type` this process sends to player `peer` reach him (the level filter of a decider without
	// the player's map: a map's message goes to the players on that map only)? True: no filter of the mode's.
	virtual bool reaches(PeerId, Msg) const { return true; }
	// A-S5S: the part of deliver() that goes to no player: the host player's link, the map servers' bridges that take `type` (the
	// world server; queued for a bridge that loads its world). A map server has none: its reports go up through up().
	virtual void deliver_links(Msg, const Writer&, Lane) {}
	// P-B (A-S6-E-surv): what touches player `peer`'s character is done by the process that leads the map it stands on.
	// True when another process of this world leads it now (a map server, as the world server sees it).
	virtual bool led_elsewhere(PeerId) const { return false; }
	// The same, and `type` goes there: true when it was sent; false: his map is this process's, or nobody leads it now (the caller
	// does it here, or refuses).
	virtual bool to_level_of(PeerId, Msg, const Writer&, Lane) { return false; }
	// H-JOIN (coop_join.cpp; A-S6J moves the server's bodies behind these):
	// A JoinRequest / WorldLoaded (`type`) from `from`: true when `from` is no player but a map server of this world (the server's
	// join serves it), the Core's player path stays out.
	virtual bool join_from_bridge(PeerId, Msg) { return false; }
	// The world save for player `from`, whose character is `proxy`, goes out: the mode's part. True: the map his character stands
	// on is led by another process (it hears his place now, when it is up); the Core holds his loading screen until
	// level_ready(his level). False: his character comes into this process's world (the Core's path).
	virtual bool join_save(PeerId, CSE_ALifeHumanStalker&) { return false; }
	// Is the process that leads `level` up, so that a player held for it may enter? True: nobody to wait for.
	virtual bool level_ready(uint16_t) const { return true; }
	// This process's world went live (its WorldLoaded to its host): what the mode does then.
	virtual void on_world_live() {}
	// H-LOADTEXT: the string-table key of the loading screen's line while it is held for the level `screen_level` and the world's
	// decider is not up yet (`world_up` false); nullptr: the Core's line (who it waits for).
	virtual const char* load_text(uint16_t, bool) const { return nullptr; }
	// H-GHOSTSAID: a relation request the logic of this process's level made of its own actor: true when that actor is a ghost and
	// the word is the players' of the level (coop_story.cpp keeps it for them, it never goes up as it is).
	virtual bool ghost_said(const ghost::Said&) const { return false; }
	// H-OPCMD (coop_debug.cpp, INTEG-C): this process's operator console line `line` goes to the world server (Msg::OperatorCommand).
	// A player of a server world checks it (op::allowed) and sends it; every other kind has no world server to send it to.
	virtual OperatorSend send_operator_command(const std::string&) { return OperatorSend::Unreachable; }

	// --- VEH-DISC (Departure above): how player `peer`, whose character here was `character` (this process's numbering), left this
	// process's session; asked once his peer is gone from it. Disconnected is none's answer and every mode's but a map server's, which
	// answers what its world's LevelMap said when his virtual peer left (tests/test_vehicle_departure.cpp).
	virtual Departure departure(PeerId, uint16_t) const { return Departure::Disconnected; }

	// --- R32 (temp/r30/design/R32-final.md §8, docs/contracts/coop_mode_api.md section 8): one writer per object.
	// W1: who writes the object whose root the Core found (writer::root_of, its kind and map). A question about an object, as
	// online_elsewhere is: each mode answers it with writer::writer_of from its own tables (who leads a map, who holds a unit,
	// whose move fence is open). none and the co-op host: This; a co-op client: Other(its host); the world server and a map
	// server: by W1.2 (srv/coop_srv_mode.cpp).
	virtual writer::Writer writer_of(const writer::RootFacts&) const { return writer::this_process(); }
	// W5.1: a map server repairs its copy towards the world: client_reconcile over the ids it does not write. Nothing elsewhere.
	virtual void reconcile_world() {}
	// W4.5: may the world be saved now? The world server: its flush round is done or timed out. True elsewhere.
	virtual bool save_ready(uint64_t /*now*/) { return true; }
};

// A mode with its row of answers (each layer derives from it).
class ModeWithAnswers : public Mode
{
public:
	explicit ModeWithAnswers(const ModeAnswers& answers) : m_answers(answers) {}
	bool actor_is_player() const final { return m_answers.actor_is_player; }
	bool decides_world() const final { return m_answers.decides_world; }
	bool leads_level() const final { return m_answers.leads_level; }
	bool group_travels() const final { return m_answers.group_travels; }
	uint8_t roles() const final { return m_answers.roles; }

private:
	ModeAnswers m_answers;
};

// No session (single player, a menu, a process between sessions): nobody else exists, nothing is sent, nothing is installed.
// The S6 hooks of Mode answer "not mine" already: NoneMode keeps them.
constexpr ModeAnswers kNoSessionAnswers{/*actor_is_player*/ true, /*decides_world*/ false, /*leads_level*/ false,
    /*group_travels*/ false, /*roles*/ 0};

class NoneMode final : public ModeWithAnswers
{
public:
	NoneMode() : ModeWithAnswers(kNoSessionAnswers) {}
	const char* name() const override { return "none"; }
	Where where(PeerId) const override { return Where::Away; }
	Decider decider_of(uint16_t) const override { return Decider::Nobody; }
	bool facts_of(uint16_t, levels::BodyFacts&) const override { return false; }
	void deliver(Msg, const Writer&, Lane, PeerId) override {}
	void stream(Msg, const Writer&, PeerId) override {}
	void up(Msg, const Writer&, Lane) override {}
	void configure(SessionConfig&) const override {}
	void install(ModeTables&) override {}
	void frame() override {}
	void session_over() override {}
	bool online_elsewhere(uint16_t) const override { return false; }
	bool binder_may_update(CGameObject*) override { return true; }
	void held_update() override {}
	void switched_offline(uint16_t) override {}
	bool takes_own_ids() const override { return false; }
	uint16_t own_free_id() override { return 0xffff; }
};

// The holder: none until a mode is installed; once a mode is in, another one is refused until reset() (set once per session).
class ModeSlot
{
public:
	Mode& get()
	{
		Mode* m = m_mode.load();
		return m ? *m : m_none;
	}
	const Mode& get() const
	{
		const Mode* m = m_mode.load();
		return m ? *m : m_none;
	}
	// True when `m` is the mode now (it was put in, or it already was).
	bool install(Mode& m)
	{
		Mode* expected = nullptr;
		return m_mode.compare_exchange_strong(expected, &m) || expected == &m;
	}
	void reset() { m_mode.store(nullptr); }
	bool installed() const { return m_mode.load() != nullptr; }

private:
	std::atomic<Mode*> m_mode{nullptr};
	NoneMode m_none;
};

// A mode layer as the switchboard sees it (xrGame/coop/coop_mode.cpp; the layers: hostplay::layer(), srv::layer()).
class Layer
{
public:
	virtual ~Layer() = default;
	virtual const char* name() const = 0;
	// Every frame, in a session or not, before the Core's autostart: the layer reads its own keys of the command line once and makes
	// its own starts. True: the sessions of this process are the layer's (the Core's autostart and the boots after it stay out).
	virtual bool boot_frame() = 0;
	// The mode of a start of this process (`said`: the mode a client's Welcome says, None for the other entries); nullptr: not this
	// layer's.
	virtual Mode* mode_of(Entry entry, SessionMode said) = 0;
	// Once at the start of the process, with the Core's console commands (coop_console.cpp, before user.ltx is read), in every process:
	// the layer's own commands. None by default.
	virtual void register_console() {}
	// Every time the script engine makes its Lua state, with the Core's coop.* (coop_script.cpp), in every process: the layer's own Lua
	// table. None by default.
	virtual void register_lua(lua_State*) {}
	// A-S6-IF: the two hooks of the process, not of its session: the world server loads its world and has its console before its
	// session starts, when mode() is still none. The switchboard asks them below; none by default.
	// H-JOIN: the actor of the save this process loads (`objects`) is its ghost, not a player: the layer puts it on its level (a
	// map server's at its key's vertex, the world server's on its service level). True: taken (a map server that cannot place it
	// stopped its session), the Core's role swap stays out.
	virtual bool place_ghost(CALifeObjectRegistry&) { return false; }
	// H-SAY: a line of the debug's answers also goes to this process's own console window and to the operator whose command runs
	// now (the world server's). Main thread.
	virtual void console_line(const char*) {}
	// INTEG-C (rv_A-S6-E-net fix 1): every frame, in a session or not, before the Core's autostart starts anything: an address this
	// process was asked from outside the game to join ("ip:port"; the co-op's: a Steam invite the launcher accepted) and `via`, what
	// asked (for the log). True: the Core's autostart takes it into its retry queue (the same address while in that game changes
	// nothing). None by default.
	virtual bool join_request(std::string&, std::string&) { return false; }
};

// --- xrGame/coop/coop_mode.cpp
// The mode of this process's session; none outside one.
Mode& mode();
// The Manager's starts only: true when `m` is the session's mode now (refused while another is in).
bool install_mode(Mode& m);
// set_phase(Off), a start that failed: none again.
void reset_mode();
// The switchboard: the mode of a start, from the layers (the server layer first: its keys are its own); nullptr when no layer has one.
Mode* mode_for(Entry entry, SessionMode said);
// The switchboard: every layer's boot_frame, the server's first; true when one of them owns this process's sessions.
bool boot_layers();
// The switchboard: every layer's console commands (coop_console.cpp) and Lua tables (coop_script.cpp), the server's first.
void register_layers_console();
void register_layers_lua(lua_State* L);
// The switchboard (A-S6-IF): the process's hooks of the layers (Layer::place_ghost, console_line), in a session or not, the server's
// first; place_ghost is true when a layer took the actor.
bool place_ghost(CALifeObjectRegistry& objects);
void console_line(const char* line);
// The switchboard (INTEG-C): Layer::join_request of every layer, in a session or not, the server's first; true, `address` and `via`
// from the first that has one.
bool layer_join_request(std::string& address, std::string& via);
} // namespace coop
