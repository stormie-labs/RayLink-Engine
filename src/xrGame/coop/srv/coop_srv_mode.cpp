// Сервер: только сервер мира и серверы карт (docs/MODES.md)
// Anomaly Coop — the server's modes (xrCoop/coop_mode.h, docs/MODES.md section 4): the world server (-coop_dedicated: the world, no map
// of a player, its seat a ghost on fake_start), a map server (-coop_mapserver: leads its map for the players on it, a client of the
// world for the rest) and a player of a server world. And the server's boot: its own keys of the command line and its own starts.
//
// What moved here in S4 (docs/plans/2026-11-18-two-modes.md), as it was: the server's half of net::host_broadcast, host_stream and
// client_send (the level filter, the bridges, the map server's filter); the server's handler tables of Manager::install_handlers
// (Hosting for the world server, MapServer) with the transfers' tables and the in-filter of the levels; the world server's
// config of its session (its seat plays nobody, the operator's token); the dedicated autostart of Manager::autostart and the map
// server's start by its key; the levels' frame; the map server's weather, now a step of its own (worldstate::level_sky_frame).
// A-S4Sb: the operator's lines and a map server's level hosted through its bridge, out of the Manager (srv::SessionSrv).
// A-S6-IF (S6): the server's answers to the hooks of the S6 groups: a map server's side of its level (MapLevelSide, Mode::level()),
// the world server's level filter, links, P-B and join hooks, a server player's loading line, and the process's hooks of the layer
// (place_ghost, console_line), over the levels', the map server's and the dedicated host's functions they replace.
// A-S6J (S6 P-G): the server's part of the join is srv/coop_srv_join.cpp: the world server's handlers of its bridges' joins and of a
// player's LevelMove, its frame for the bridges that wait for a world, the facts its autostart waits for; a map server's SessionReset; a
// server player's feed lines.
#include "stdafx.h"
#include "coop/coop_manager.h"
#include "coop/srv/coop_dedicated.h"
#include "coop/coop_entities.h" // H-JOIN: the place of a joining player's character to the map server of its level
#include "coop/coop_items.h" // H-ITEMTELL
#include "coop/coop_join.h"
#include "coop/coop_levels.h"
#include "coop/srv/coop_mapserver.h"
#include "coop/coop_net.h"
#include "coop/coop_script.h" // H-DEATHMARK: the marker's Lua callback
#include "coop/coop_sync.h"
#include "coop/coop_world.h"
#include "coop/srv/coop_srv_join.h" // A-S6J: the server's part of the join, the feed lines
#include "coop_host_console.h" // the @rl lines of the dedicated autostart; H-SAY: the window
#include "ai_space.h" // place_ghost: the game graph
#include "alife_object_registry.h"
#include "game_graph.h"
#include "xrServer_Objects_ALife_Monsters.h"
#include "xrCoop/coop_death_items.h" // H-DEATHMARK: Marker, marker_kind
#include "xrCoop/coop_fight_fact.h" // H-FIGHTREP (INTEG-C): a fight fact up to the world server
#include "xrCoop/srv/coop_dedicated_rules.h" // H-SAY: log_shown
#include "xrCoop/srv/coop_ghost_rules.h" // H-GHOSTSAID: about_actor
#include "xrCoop/coop_mode.h"
#include "xrCoop/srv/coop_operator.h"
#include "xrCoop/coop_roles.h"
#include "xrCoop/srv/coop_world_server_rules.h"
#include "xrCoop/srv/coop_session_srv.h" // the server's part of the session: the operator's token, its lines, a map server's level

namespace coop
{
namespace worldstate
{
// coop_worldstate.cpp: one step of the sky of the map this process leads, for the players on it (a map server's own, apart from the
// co-op host's host_weather_frame). Declared here: coop_worldstate.h is included from alife_time_manager.h and is not this unit's.
void level_sky_frame(u64 now);
} // namespace worldstate

namespace survival
{
// coop_survival.cpp: the map server's PlayerWounded on its level session, and its PlayerWounded / PlayerRevive / PlayerRules from
// its world (A-S6-E-surv2b: wired here, in place of the Core installers' old dispatch). Declared here like level_sky_frame.
void install_level_server(Session& s);
void install_level_client(Endpoint& e);
} // namespace survival

namespace mapserver
{
// coop_trunk.cpp (VEH-DISC): how a player left this map server's level, as its world's LevelMap said when his virtual peer left
// (Disconnected when nothing was said of that player with that character). Declared here: coop_mapserver.h is not this unit's.
Departure departure_of(PeerId peer, u16 character);
} // namespace mapserver

namespace srv
{
void register_console(); // srv/coop_srv_console.cpp: srv_* and the old coop_* names
void register_lua(lua_State* L); // srv/coop_srv_script.cpp: the table srv.*

namespace
{
// tests/test_mode_answers.cpp reads these rows and compares them with the table of tests/support/fake_mode.h
constexpr ModeAnswers kWorldServer{/*actor_is_player*/ false, /*decides_world*/ true, /*leads_level*/ false,
    /*group_travels*/ false, dedicated_roles()};
constexpr ModeAnswers kMapServer{/*actor_is_player*/ false, /*decides_world*/ false, /*leads_level*/ true, /*group_travels*/ false,
    map_server_roles()};
constexpr ModeAnswers kServerClient{/*actor_is_player*/ true, /*decides_world*/ false, /*leads_level*/ false,
    /*group_travels*/ false, client_roles()};

// The players of this process's session that a broadcast of type `type` reaches (levels::passes_to: a stream of a map to a player
// on another one does not), queued while one loads the world.
void to_players(Msg type, const Writer& payload, Lane lane, PeerId except)
{
	for (const PeerInfo& p : mgr().session().peers())
		if (p.id != except && p.state == PeerState::Joined && levels::passes_to(type, p.id))
			net::to_peer(p.id, type, payload, lane);
}

void stream_to_players(Msg type, const Writer& payload, PeerId except)
{
	Session& s = mgr().session();
	if (!levels::split())
	{
		s.broadcast(Lane::Unreliable, type, payload, except);
		return;
	}
	for (const PeerInfo& p : s.peers())
		if (p.id != except && p.state == PeerState::Joined && levels::passes_to(type, p.id))
			s.send(p.id, Lane::Unreliable, type, payload);
}

// The world server: the map servers are clients of the world: what reaches every level reaches them, a bridge that loads its world
// gets it after (the join queue).
void to_bridges(Msg type, const Writer& payload, Lane lane, PeerId except)
{
	if (!mapserver::bridge_takes(type))
		return;
	for (const PeerId bridge : mapserver::host_bridges())
		if (bridge != except)
			net::to_peer(bridge, type, payload, lane);
}

// P-B (A-S6-IF): the bridge of the map server that leads the map player `peer` stands on, as the world server sees it; kInvalidPeer:
// his map is this process's, or no map server leads it now (coop_survival.cpp bridge_of_body before).
PeerId level_bridge(PeerId peer)
{
	return peer != kInvalidPeer && !levels::on_host_level(peer) ? mapserver::bridge_of(levels::level_of_player(peer)) : kInvalidPeer;
}

// H-LOADTEXT: a player of a server world (and a map server, a client of its world too) whose screen is held for a level the world
// server is not on, before the world server's word that its world is up, waits for the map server of that level
// (coop_join.cpp load_screen_line).
const char* server_load_text(u16 screen_level, bool world_up)
{
	return !world_up && screen_level != levels::level_of_player(kHostPeer) ? "st_coop_load_server" : nullptr;
}

// A map server's side of its level (xrCoop/coop_mode.h LevelSide): what the world server keeps in its books, it tells the world server;
// its players' money as the world server last named it (coop_money.cpp's shadow book before).
class MapLevelSide final : public LevelSide
{
public:
	void level_money_change(PeerId peer, s64 delta, bool trade) override
	{
		levels::up_money(peer, delta, trade); // a change of nothing makes the world server send his balance
		if (!delta)
			return;
		u32& balance = m_balances[peer];
		balance = u32(_max<s64>(0, s64(balance) + delta));
	}
	bool level_balance(PeerId peer, u32& out) const override
	{
		const auto it = m_balances.find(peer);
		out = it == m_balances.end() ? 0 : it->second;
		return it != m_balances.end();
	}
	void level_balance_heard(PeerId peer, u32 balance) override { m_balances[peer] = balance; }
	// The Core's ItemState to this level's players (items::level_item_tell): never route that function back through this one.
	void level_item_tell(u16 id, const std::string& section, const std::vector<u8>& blob) override
	{
		items::level_item_tell(id, section, blob);
	}
	bool level_tent_limit(size_t world_tents, size_t limit) const override { return world_tents >= limit; }
	void level_tent_placed(u16 tent, u16 character) override { levels::up_tent(tent, character); }
	bool level_tent_packed(u16) override { return true; } // the world server's sweep rolls the camp back on its word
	bool level_in_world(u16 character) const override { return join::host_player_in_world(character); }
	// A map server's marker goes to its players only and nothing brings it back: its own Lua sets the spot (the callback of
	// coop_death.cpp fire_marker_callback, in this process's numbering), coop_story takes it to the world.
	void level_marker(const death::Marker& m) override
	{
		script::queue_callback("coop_death_marker",
		    {script::cb_int(m.character), script::cb_int(m.object), script::cb_str(death::marker_kind(m.drop))});
	}
	u16 level_character_of(PeerId peer) const override
	{
		const auto& on = mapserver::trunk_peers().on();
		const auto it = on.find(peer);
		return it == on.end() ? u16(0xffff) : u16(it->second.proxy);
	}
	PeerId level_peer_of(u16 character) const override
	{
		for (const auto& it : mapserver::trunk_peers().on())
			if (it.second.proxy == character)
				return it.first;
		return kInvalidPeer;
	}
	// The ALife's thread may call it: the levels' functions take their lock.
	void level_entity_up(EntityUp kind, u16 id, u16 other) override
	{
		switch (kind)
		{
		case EntityUp::Spawned: levels::up_spawned(id); break;
		case EntityUp::Released: levels::xfer_on_released(id); break;
		case EntityUp::Parent: levels::up_parent(id, other); break;
		case EntityUp::Died: levels::up_died(id, other); break;
		}
	}
	// H-FIGHTREP (INTEG-C, F5-PRG-4): a fight on its level changed a player's reputation; his record is the world server's. The fact goes
	// up numbered in this process's order (the world server's script keeps the order per route) and his machine hears it from the world
	// server once it is applied there. Nobody of this level (its ghost, a stranger): nothing.
	void level_fight_reputation(u16 character, int delta) override
	{
		if (!delta || level_peer_of(character) == kInvalidPeer)
			return;
		fight::FactMsg m;
		m.epoch = mgr().session().world_epoch();
		m.fact = m_facts.next();
		m.character = join::to_host(character);
		m.delta = fight::bounded(delta);
		if (!fight::FactMsg::valid(m))
			return;
		Writer w(16);
		m.write(w);
		net::client_send(Msg::Relation, w);
	}

private:
	xr_map<PeerId, u32> m_balances;
	fight::Facts m_facts; // INTEG-C: the numbers of this process's fight facts
};

MapLevelSide& map_level_side()
{
	static MapLevelSide s;
	return s;
}

class WorldServerMode final : public ModeWithAnswers
{
public:
	WorldServerMode() : ModeWithAnswers(kWorldServer) {}
	const char* name() const override { return "world server"; }

	// Its own seat plays nobody (the ghost); a player is on a map a map server leads, or not placed on one yet.
	Where where(PeerId peer) const override
	{
		if (peer == kHostPeer)
			return Where::Away;
		return levels::on_host_level(peer) ? Where::Joining : Where::Away;
	}
	// The map server of his level decides for a placed player; this process for one whose map nobody leads yet.
	Decider decider_of(u16 character) const override
	{
		if (character == 0 || join::peer_of_proxy(character) == kInvalidPeer)
			return Decider::Nobody; // the ghost, a stranger
		return levels::host_character_elsewhere(character) ? Decider::Other : Decider::This;
	}
	bool facts_of(u16 character, levels::BodyFacts& out) const override { return levels::body_facts(character, out); }

	void deliver(Msg type, const Writer& payload, Lane lane, PeerId except) override
	{
		to_players(type, payload, lane, except); // no host player: the link was detached at the start (install)
		to_bridges(type, payload, lane, except);
	}
	void stream(Msg type, const Writer& payload, PeerId except) override { stream_to_players(type, payload, except); }
	void up(Msg, const Writer&, Lane) override {} // it decides the world: nobody to ask (no host player here)

	// The operator's token has this one way in (A-S4Sb): armed in the session's server part for the host() that follows this configure
	// (Manager::start_host); no config holds it.
	void configure(SessionConfig& cfg) const override
	{
		cfg.host_plays = false; // protocol 31: its seat plays no character (Welcome.flags)
		cfg.mode = session_mode_of_seat(cfg.host_plays); // protocol 33: the Welcome's mode
		cfg.max_players = kMaxPlayers; // F5-SEAT-1 (TZ 6.7, R19): the server takes the wire's 16 seats, all of them clients
		op::Token token{};
		if (op::find_token(Core.Params, token))
		{
			// plan 2026-11-12 §2.2: the game that presents this token from this PC holds the host's rights. The token is never logged.
			::Msg("[coop] dedicated: an operator token is set (-coop_operator)");
		}
		SessionSrv(mgr().session()).arm_operator(token); // all zeros without the key: no operator
		SecureZeroMemory(token.data(), token.size()); // the copy on this stack; the session's part keeps its own
	}
	// Nobody plays in this process: no host player, no link (plan 12). The world server's tables, the map servers', the transfers'.
	void install(ModeTables& tables) override
	{
		tables.session.attach_local(nullptr); // peer 0 is nobody: players() has no such row, what is sent to it goes nowhere
		tables.session.set_in_filter(&mapserver::host_incoming); // a player's message for the map server of his level (coop_trunk.cpp)
		// plan 2026-11-12 stage 3: a line of the operator that passed the session's part runs on this server's console (dedicated::run_for
		// checks him and the line again)
		SessionSrv(tables.session).on_operator_command(&dedicated::run_for);
		join::install_server(tables.session);
		join_install_world(tables.session); // A-S6J: its bridges' JoinRequest / WorldLoaded first, a player's LevelMove (after the Core's)
		sync::install_server(tables.session);
		mapserver::install_server(tables.session);
		levels::xfer_install_server(tables.session);
	}
	void frame() override
	{
		levels::frame(); // the seats, LevelMap, the map servers, the transfers
		join_world_frame(); // A-S6J: the map servers that wait for a world to load
	}
	void session_over() override {}

	// H5 (A-S4H): what a map server took of its world is online there, not here; it holds, updates and numbers nothing of a map's
	bool online_elsewhere(u16 id) const override { return levels::online_at_server(id); }
	bool binder_may_update(CGameObject*) override { return true; }
	void held_update() override {}
	void switched_offline(u16) override {}
	bool takes_own_ids() const override { return false; }
	u16 own_free_id() override { return 0xffff; }

	// A-S6-IF (S6): no map of a player here. A map's message reaches the players on that map only; what touches a player's character
	// goes to the map server of his map; the links are the bridges; a player of a map that has a map server joins there.
	LevelSide* level() override { return nullptr; }
	bool reaches(PeerId peer, Msg type) const override { return levels::passes_to(type, peer); }
	void deliver_links(Msg type, const Writer& payload, Lane lane) override
	{
		to_bridges(type, payload, lane, kInvalidPeer); // no host player's link (install detached it)
	}
	bool led_elsewhere(PeerId peer) const override { return level_bridge(peer) != kInvalidPeer; }
	bool to_level_of(PeerId peer, Msg type, const Writer& payload, Lane lane) override
	{
		const PeerId bridge = level_bridge(peer);
		if (bridge == kInvalidPeer)
			return false;
		net::to_peer(bridge, type, payload, lane); // queued for a bridge that loads its world
		return true;
	}
	bool join_from_bridge(PeerId from, Msg) override { return mapserver::is_bridge(from); }
	// The map server of his character's level hears where it stands (its copy is as old as its world); not up yet: he waits for it.
	bool join_save(PeerId from, CSE_ALifeHumanStalker& proxy) override
	{
		if (!levels::split() || levels::on_host_level(from))
			return false;
		const PeerId bridge = mapserver::bridge_of(levels::level_of_vertex(proxy.m_tGraphID));
		if (bridge != kInvalidPeer)
			entities::host_send_place(bridge, &proxy);
		return true;
	}
	bool level_ready(u16 level) const override { return mapserver::host_ready(level); }
	void on_world_live() override {}
	const char* load_text(u16, bool) const override { return nullptr; } // it holds nobody's loading screen of its own
	bool ghost_said(const ghost::Said&) const override { return false; } // its logic runs on no player's level
	// H-OPCMD (INTEG-C): its own console runs the operator's lines (dedicated::run_for); it sends none to a world server
	OperatorSend send_operator_command(const std::string&) override { return OperatorSend::Unreachable; }
	// VEH-DISC: its session is the world's; a move to another map keeps a player its peer (LevelMove): one who leaves it quit the
	// world. It leads no map, so nobody asks it for a seat
	Departure departure(PeerId, u16) const override { return Departure::Disconnected; }
};

class MapServerMode final : public ModeWithAnswers
{
public:
	MapServerMode() : ModeWithAnswers(kMapServer) {}
	const char* name() const override { return "map server"; }

	// The players of its map are the (virtual) peers of its session; its own actor is a ghost.
	Where where(PeerId peer) const override
	{
		const PeerInfo* p = mgr().session().peer(peer);
		if (!p || p->state != PeerState::Joined)
			return Where::Away;
		return sync::host_peer_joining(peer) ? Where::Joining : Where::Here;
	}
	Decider decider_of(u16 character) const override
	{
		if (character == 0)
			return Decider::Nobody; // the ghost
		const PeerId peer = join::peer_of_proxy(character);
		if (peer != kInvalidPeer && mgr().session().peer(peer))
			return Decider::This;
		return world::avatar_peer(character) != kInvalidPeer ? Decider::Other : Decider::Nobody;
	}
	bool facts_of(u16 character, levels::BodyFacts& out) const override { return levels::body_facts(character, out); }

	// To the players of its session only. It has no link: its bridge carries its level session, whose host() dropped the LocalLink
	// (Session::host_carried); sent up the bridge, a HostOnly message (EntitySpawn ...) would get it kicked by its world as a client's
	// malformed one (review A-REV-E-1, finding 2). What the world server hears of its map goes up through up().
	void deliver(Msg type, const Writer& payload, Lane lane, PeerId except) override { to_players(type, payload, lane, except); }
	void stream(Msg type, const Writer& payload, PeerId except) override { stream_to_players(type, payload, except); }
	void up(Msg type, const Writer& payload, Lane lane) override
	{
		if (!mapserver::map_server_sends(type))
			return; // its actor is a ghost: nobody's body, nobody's hands
		mgr().session().send(kHostPeer, lane, type, payload);
	}

	// The world server's process may be loading its own new world: it takes the Hello when it is up (plan §1.5 "the host's move").
	// Protocol 33 (SESSION-MODE): its level's host() says Server, as every start does (Session::host refuses none); the mode its
	// level's session answers is its world's Welcome's, its seats are its world's (no host_plays here: the seat rule is a listening
	// host's).
	void configure(SessionConfig& cfg) const override
	{
		cfg.connect_timeout_ms = mapserver::kConnectTimeoutMs;
		cfg.mode = SessionMode::Server;
	}
	// The host's handlers of a level for its virtual peers, the client's for the world on its bridge. The session's next host() (that of
	// Manager::start_map_server, once its bridge connects) is its level's, carried by the bridge: srv::SessionSrv::level_through.
	void install(ModeTables& tables) override
	{
		SessionSrv(tables.session).level_through(tables.world);
		tables.session.set_in_filter(nullptr); // every peer of its session stands on its level
		sync::install_server(tables.session);
		survival::install_level_server(tables.session); // its PlayerWounded over the Core's (the same place as the old dispatch)
		levels::xfer_install_server(tables.session);
		join::install_client(tables.world);
		join_install_map(tables.world); // A-S6J: the world server's SessionReset ends this process (after the Core's client tables)
		sync::install_client(tables.world);
		survival::install_level_client(tables.world); // its PlayerWounded / PlayerRevive / PlayerRules over the Core's
		mapserver::install_client(tables.world);
		levels::xfer_install_client(tables.world);
	}
	void frame() override
	{
		levels::frame();
		if (world_up())
			worldstate::level_sky_frame(mgr().now_ms()); // time is the world server's, the sky of its map its own
	}
	void session_over() override { mapserver::server_session_over(); } // a map server without its world is nobody's

	// H5 (A-S4H): the objects of its map it holds for the world server: their binders, their stock update offline, the way home, ids
	// of its own slice; nothing of its world is online at another map server as it sees it
	bool online_elsewhere(u16) const override { return false; }
	bool binder_may_update(CGameObject* object) override { return levels::logic_may_update(object); }
	void held_update() override { levels::xfer_scheduled(); }
	void switched_offline(u16 id) override { levels::xfer_on_switched_offline(id); }
	bool takes_own_ids() const override { return true; }
	u16 own_free_id() override { return levels::leader_free_id(); }

	// A-S6-IF (S6): its side of its level (the books are the world server's); every peer of its session stands on its map; it has no
	// link (deliver above: its reports go up through up(), map_server_sends); its world going live parks the characters of that world
	// until their players are on this level; its actor is a ghost, so what its logic says of that actor is its players' word.
	LevelSide* level() override { return &map_level_side(); }
	bool reaches(PeerId peer, Msg type) const override { return levels::passes_to(type, peer); }
	void deliver_links(Msg, const Writer&, Lane) override {}
	bool led_elsewhere(PeerId) const override { return false; }
	bool to_level_of(PeerId, Msg, const Writer&, Lane) override { return false; }
	bool join_from_bridge(PeerId, Msg) override { return false; }
	bool join_save(PeerId, CSE_ALifeHumanStalker&) override { return false; }
	bool level_ready(u16) const override { return true; }
	void on_world_live() override { world::host_scan_proxies(); }
	const char* load_text(u16 screen_level, bool world_up) const override { return server_load_text(screen_level, world_up); }
	bool ghost_said(const ghost::Said& s) const override { return ghost::about_actor(s); }
	// H-OPCMD (INTEG-C): no operator console here (its session to the world server is a bridge's)
	OperatorSend send_operator_command(const std::string&) override { return OperatorSend::Unreachable; }
	// VEH-DISC (review rv_F5-VEH-7, blocker 2): its level session drops the virtual peer of a player who quit the world and of one who
	// went to another map alike; what its world's LevelMap said when it dropped him tells them apart (coop_trunk.cpp server_on_map,
	// xrCoop/srv/coop_srv_departure_rules.h)
	Departure departure(PeerId peer, u16 character) const override { return mapserver::departure_of(peer, character); }
};

// A player of a server world: the map server of his map decides for him, the world server for the world.
class ServerClientMode final : public ModeWithAnswers
{
public:
	ServerClientMode() : ModeWithAnswers(kServerClient) {}
	const char* name() const override { return "server player"; }

	Where where(PeerId peer) const override
	{
		if (peer == mgr().self())
			return Where::Here;
		if (peer == kHostPeer)
			return Where::Away; // the world server's seat plays nobody
		const levels::LevelId his = levels::level_of_player(peer);
		return his != levels::wire::kNoLevel && his == levels::my_level() ? Where::Here : Where::Away;
	}
	Decider decider_of(u16 character) const override
	{
		return character == 0 || world::avatar_peer(character) != kInvalidPeer ? Decider::Other : Decider::Nobody;
	}
	bool facts_of(u16 character, levels::BodyFacts& out) const override { return levels::body_facts(character, out); }

	void deliver(Msg, const Writer&, Lane, PeerId) override {}
	void stream(Msg, const Writer&, PeerId) override {}
	void up(Msg type, const Writer& payload, Lane lane) override { mgr().session().send(kHostPeer, lane, type, payload); }

	void configure(SessionConfig&) const override {} // a client's config is made before its Welcome
	// The Core's client tables are there from its start (Manager::start_client); a new session's feed knows nobody yet (A-S6J).
	void install(ModeTables&) override { feed_start(); }
	void frame() override
	{
		levels::frame();
		feed_frame(); // A-S6J (S5): "went to" / "came from" in his PDA feed, from the world server's table
	}
	void session_over() override {}

	// H5 (A-S4H): a player's machine mirrors; no transfers here (the answers of none)
	bool online_elsewhere(u16) const override { return false; }
	bool binder_may_update(CGameObject*) override { return true; }
	void held_update() override {}
	void switched_offline(u16) override {}
	bool takes_own_ids() const override { return false; }
	u16 own_free_id() override { return 0xffff; }

	// A-S6-IF (S6): a player's machine sends nobody anything of the world and joins nobody; its loading screen waits for the map
	// server of its level.
	LevelSide* level() override { return nullptr; }
	bool reaches(PeerId, Msg) const override { return true; }
	void deliver_links(Msg, const Writer&, Lane) override {}
	bool led_elsewhere(PeerId) const override { return false; }
	bool to_level_of(PeerId, Msg, const Writer&, Lane) override { return false; }
	bool join_from_bridge(PeerId, Msg) override { return false; }
	bool join_save(PeerId, CSE_ALifeHumanStalker&) override { return false; }
	bool level_ready(u16) const override { return true; }
	void on_world_live() override {}
	const char* load_text(u16 screen_level, bool world_up) const override { return server_load_text(screen_level, world_up); }
	bool ghost_said(const ghost::Said&) const override { return false; }
	// H-OPCMD (INTEG-C, the deferred rows of A-S6-E-story): the operator's console line goes to his world server, checked here before
	// it leaves (the world server checks it again when it runs it: op::command_verdict)
	OperatorSend send_operator_command(const std::string& line) override
	{
		if (!op::allowed(line))
			return OperatorSend::Refused;
		return mgr().session().send_operator_command(line) ? OperatorSend::Sent : OperatorSend::Unreachable;
	}
	// VEH-DISC: it seats nobody; a peer that leaves its world server's session quit the world
	Departure departure(PeerId, u16) const override { return Departure::Disconnected; }
};

WorldServerMode& world_mode()
{
	static WorldServerMode m;
	return m;
}

MapServerMode& map_mode()
{
	static MapServerMode m;
	return m;
}

ServerClientMode& client_mode()
{
	static ServerClientMode m;
	return m;
}

// The server's boot: the keys -coop_dedicated (dedicated::args) and -coop_mapserver (mapserver::key), read once, and the starts they make.
class ServerLayer final : public Layer
{
public:
	const char* name() const override { return "server"; }

	bool boot_frame() override
	{
		dedicated::frame(); // every frame from the first one: the world, the lines of the window, the quit
		if (!m_parsed)
		{
			m_parsed = true;
			m_world = dedicated::on();
			if (m_world)
			{
				m_port = dedicated::args().port; // when its world is up
				::Msg("[coop] autostart: the world server on port %u (-coop_dedicated)", u32(m_port));
			}
			m_map = mapserver::parsed_key();
			if (m_map && !mgr().start_map_server(mapserver::key().port, mapserver::key().token))
				mapserver::server_session_over();
		}
		if (m_world)
			world_autostart();
		// F3a T5: a server's process (-coop_mapserver, -coop_dedicated: x_ray.cpp sets g_coop_headless) logs what it costs every 60 s,
		// from its first frame, in a session or not (A-S4H: from the Core's on_frame)
		if (g_coop_headless)
			mapserver_stats_frame();
		return m_world || m_map;
	}

	Mode* mode_of(Entry entry, SessionMode said) override
	{
		switch (entry)
		{
		case Entry::Host: return dedicated::on() ? &world_mode() : nullptr; // the autostart below, or the operator's coop_host
		case Entry::Client: return said == SessionMode::Server ? &client_mode() : nullptr;
		case Entry::MapServer: return &map_mode();
		}
		return nullptr;
	}

	// In every process: user.ltx keeps a machine's settings whatever it runs, and the server's layer script asks srv.* everywhere.
	void register_console() override { srv::register_console(); }
	void register_lua(lua_State* L) override { srv::register_lua(L); }

	// A-S6-IF, H-JOIN (coop_join.cpp on_alife_objects_loaded): the actor of the save is the ghost of a map server (the world it loads
	// from the world server) and of the world server (from its start, before its session: its mode is still none then).
	bool place_ghost(CALifeObjectRegistry& objects) override
	{
		if (is_map_server() && join::client_loading_world())
			return place_map_ghost(objects);
		if (!dedicated::on())
			return false;
		// Plan 12: as on a map server, the actor of the save decides the level: the host's character, whoever played it, plays nothing here.
		if (!dedicated::place_ghost(objects))
			FATAL("[coop] dedicated: this game has no service level");
		return true;
	}
	// A-S6-IF, H-SAY (coop_debug.cpp say): the world server's window shows a log line its level lets through (dedicated::log_shown); any
	// other is written to it here, once; a line of the operator's command (dedicated::run_for) goes back to him too. Another process has
	// no window and no operator.
	void console_line(const char* line) override
	{
		if (!dedicated::on())
			return;
		if (!dedicated::log_shown(line, coop_console::log_level()))
			coop_console::print(line);
		dedicated::answer_line(line);
	}

private:
	// Plan §1.6: the actor of the save decides which level a process loads. A map server's stands on its level: it is a ghost, the host's
	// character is on no level here. Nobody swaps: its numbering is the world server's. A world without a place for it stops the session.
	static bool place_map_ghost(CALifeObjectRegistry& objects)
	{
		CSE_ALifeCreatureActor* actor = smart_cast<CSE_ALifeCreatureActor*>(objects.object(0, true));
		const u16 vertex = mapserver::key().graph;
		if (!actor || !ai().game_graph().valid_vertex_id(vertex))
		{
			::Msg("! [coop] map server: no ghost for level %u: actor %p, vertex %u", u32(mapserver::key().level), actor, u32(vertex));
			mgr().stop("the world has no place for this map server");
			return true;
		}
		const auto* v = ai().game_graph().vertex(vertex);
		actor->o_Position = v->level_point();
		actor->m_tNodeID = v->level_vertex_id();
		actor->m_tGraphID = vertex;
		actor->m_holderID = u16(-1);
		actor->set_health(1.f);
		::Msg("[coop] map server: the ghost stands on level %u, vertex %u", u32(v->level_id()), u32(vertex));
		return true;
	}

	// One try was all an ordinary host gets, and a dedicated host that is refused once (the world not quite up in the first frame of a
	// new GAMMA world, a busy port) stayed without a session and said it once (2026-11-01). Its own window may be nobody's: it tries again.
	void world_autostart()
	{
		if (m_done || mgr().active())
			return;
		if (dedicated::quitting())
			return; // the level is going away with the process: "waiting for the world (no level yet)" after "the host goes down" was noise (live 2026-11-12)
		const u64 now = mgr().now_ms();
		const dedicated::WorldFacts facts = world_facts(); // A-S6J: srv/coop_srv_join.cpp
		const dedicated::HostAutostart::Step step = m_host.tick(now, dedicated::world_wait(facts));
		// plan 2026-11-12 stage 2: the same moments as @rl lines for the parent of a -coop_hidden server (nothing without its output)
		if (step.act == dedicated::HostAutostart::Do::GiveUp)
		{
			m_done = true;
			::Msg("! [coop] dedicated: gave up waiting for the world after %llu s: %s. coop_host %u starts the session by hand", step.waited_ms / 1000,
			    dedicated::wait_text(step.reason), u32(m_port));
			string256 why;
			xr_sprintf(why, "gave up waiting for the world after %llu s: %s", step.waited_ms / 1000, dedicated::wait_text(step.reason));
			coop_console::rl_line(world_server::format(world_server::Record::of_error("gave_up", why)).c_str());
		}
		else if (step.say)
		{
			::Msg("[coop] dedicated: waiting for the world (%s): level %d ready %d precache %d alife %d alife-actor %d g_actor %d alive %d, %llu s",
			    dedicated::wait_text(step.reason), facts.level, facts.ready, facts.precache, facts.alife, facts.alife_actor, facts.client_actor,
			    facts.alive, step.waited_ms / 1000);
			coop_console::rl_line(
			    world_server::format(world_server::Record::of_wait(dedicated::wait_code(step.reason), dedicated::wait_text(step.reason))).c_str());
		}
		if (step.act != dedicated::HostAutostart::Do::Try)
			return;
		if (!m_session_said)
		{
			m_session_said = true;
			coop_console::rl_line(world_server::format(world_server::Record::of_phase(world_server::Phase::Session)).c_str());
		}
		if (mgr().start_host(m_port))
		{
			m_done = true;
			m_host.up();
			::Msg("[coop] dedicated: the session is up on port %u, %llu s after the level came", u32(m_port), step.waited_ms / 1000);
			coop_console::rl_line(world_server::format(world_server::Record::of_ready(m_port)).c_str());
			coop_console::rl_line(world_server::format(world_server::Record::of_phase(world_server::Phase::Ready)).c_str());
		}
		else
			m_host.refused(now);
	}

	bool m_parsed = false;
	bool m_world = false; // -coop_dedicated
	bool m_map = false; // -coop_mapserver
	u16 m_port = kDefaultPort;
	bool m_done = false;
	bool m_session_said = false;
	dedicated::HostAutostart m_host; // plan 2026-11-12 stage 1: the dedicated host tries until its world is up
};
} // namespace

Layer& layer()
{
	static ServerLayer l;
	return l;
}
} // namespace srv
} // namespace coop
