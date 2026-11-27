// Anomaly Coop — the server's part of the join (docs/contracts/coop_mode_api.md section 7 H-JOIN): what the join does for a world
// server, a map server and a player of a server world only:
// - the world server: its handlers on its session (join_install_world, after the Core's join::install_server). A bridge's JoinRequest
//   gets the world its map server loads (the save its held players loaded, or a new one), a bridge's WorldLoaded ends that join, and a
//   player's LevelMove is his move alone to another level (docs/SPLIT_LEVELS.md §2 decision 5, scenario 1). The facts of its world its
//   autostart waits for (world_facts).
// - a map server: its world server's SessionReset ends its process.
// - a player of a server world: the feed lines "went to" / "came from" (xrCoop/srv/coop_srv_feed_rules.h).
// The Core's join asks the session's mode where it did these (xrCoop/coop_mode.h H-JOIN: join_from_bridge, join_save, level_ready,
// on_world_live, load_text; Layer::place_ghost) and gives these parts what they are made of (coop_join.h, "the Core's parts of the
// server's join"). Only the server's modes call this file (srv/coop_srv_mode.cpp).
#include "stdafx.h"
#include "coop_srv_join.h"
#include "coop/coop_alife.h"
#include "coop/srv/coop_dedicated.h"
#include "coop/coop_join.h"
#include "coop/coop_levels.h"
#include "coop/coop_manager.h"
#include "coop/srv/coop_mapserver.h"
#include "coop/coop_net.h"
#include "coop/coop_survival.h"
#include "coop/coop_sync.h"
#include "coop/coop_sync_api.h"
#include "xrCoop/srv/coop_levels_wire.h"
#include "xrCoop/srv/coop_srv_departure_rules.h"
#include "xrCoop/coop_transition.h"
#include "xrCoop/srv/coop_srv_feed_rules.h"
#include "Actor.h"
#include "ai_space.h"
#include "alife_graph_registry.h"
#include "alife_object_registry.h"
#include "alife_simulator.h"
#include "game_graph.h"
#include "game_news.h"
#include "Level.h"
#include "string_table.h"
#include "xrServer_Objects_ALife_Monsters.h"

#include <algorithm>

namespace coop
{
namespace srv
{
namespace
{
constexpr u16 kNoId = 0xffff;
constexpr u32 kRefusalsLogged = 5;
// Metres the world server accepts between a player and the exit he asks to leave by. A body of his here: the margin of a claim at the
// waiting point (transition::kReachM, twice: he runs on meanwhile); on a map server's level his place comes once a second.
constexpr float kMoveReachM = 2.f * transition::kReachM;
constexpr float kMoveReachFarM = 40.f;

// The world server's part of a session (join_install_world starts it anew).
struct WorldJoin
{
	std::vector<PeerId> map_wait; // map servers that asked for their world while there was none to give
	u32 moves_refused = 0; // LevelMove from too far (the first few logged)
};
WorldJoin g_world;

// A player's machine: the feed lines (feed_start starts it anew).
struct Feed
{
	feed::Watch watch;
	u64 next_at = 0;
};
Feed g_feed;
constexpr u64 kFeedStepMs = 250;

std::string nick_of(PeerId id)
{
	for (const PeerInfo& p : mgr().session().players())
		if (p.id == id)
			return p.nick;
	string32 n;
	xr_sprintf(n, "#%u", id);
	return n;
}

// The stock name of game graph level `id` ("l01_escape"), empty when the game graph has none.
std::string level_name_by_id(u16 id)
{
	const char* name = levels::level_name(levels::LevelId(id));
	return name ? std::string(name) : std::string();
}

// The stock name of the level game vertex `gv` is on, empty if unreadable.
std::string level_name_of(GameGraph::_GRAPH_ID gv) { return level_name_by_id(levels::level_of_vertex(u16(gv))); }

// The text of string-table id `id` on this machine; the id itself when the table has none.
std::string translated(const char* id)
{
	const shared_str s = CStringTable().translate(id);
	return s.size() ? std::string(s.c_str()) : std::string(id);
}

// ---------------------------------------------------------------- the world server: a map server's world

// A map server uses the client join flow without a character and loads its players' save or a fresh one.
// sync::host_begin_join queues world changes from the start so the map server receives them after loading.
void serve_bridge(PeerId bridge)
{
	// a world that is not up yet: it waits for it, as a player's join does
	if (!join::world_save_ready())
	{
		if (std::find(g_world.map_wait.begin(), g_world.map_wait.end(), bridge) == g_world.map_wait.end())
			g_world.map_wait.push_back(bridge);
		return;
	}
	std::vector<u8> engine_part, script_part;
	const join::HeldSave held = join::held_save();
	const bool shared = held.engine && held.script &&
	    !levels::logic_came_since(mapserver::level_of_bridge(bridge), held.at); // the last map server of this level left after that save
	if (shared)
	{
		engine_part = *held.engine;
		script_part = *held.script;
	}
	else
	{
		string64 name;
		xr_sprintf(name, "coop_join_ms%u", mapserver::bridge_index(bridge));
		if (!join::world_save(name, engine_part, script_part))
		{
			mgr().session().kick(bridge, RejectReason::HostNotReady, "host could not save the world");
			return;
		}
		net::host_drop(bridge); // anything queued before this snapshot is already inside it
	}
	Writer w(engine_part.size() + script_part.size() + 32);
	w.u16(kNoId); // no character: a ghost (Layer::place_ghost)
	w.u32(0);
	w.u32((u32)engine_part.size());
	w.bytes(engine_part.data(), engine_part.size());
	w.u32((u32)script_part.size());
	if (!script_part.empty())
		w.bytes(script_part.data(), script_part.size());
	std::vector<u8> blob(w.data().begin(), w.data().end());
	syncapi::host_send_full(bridge); // the world's coop.sync: the weather's cycle, whatever its Lua reads
	survival::host_send_rules(bridge); // what it judges a revive by
	const u32 id = mgr().session().send_blob(bridge, join::BlobWorldSave, std::move(blob));
	::Msg("[coop] world for map server %u: %u + %u bytes (%s), blob %u", mapserver::bridge_index(bridge), (u32)engine_part.size(),
	    (u32)script_part.size(), shared ? "the save of its players" : "a new save", id);
}

// JoinRequest on the world server: a bridge's is its map server's world; a player's is the Core's join.
void world_on_join_request(PeerId from, Reader& r)
{
	if (!mgr().is_host())
		return; // the session drops it on a client already (msg_sender)
	if (mapserver::is_bridge(from))
	{
		serve_bridge(from);
		return;
	}
	join::player_join_request(from, r);
}

// WorldLoaded on the world server: a bridge's ends its map server's join (what the world did while it loaded; its Ready follows,
// coop_mapserver.cpp); a player's is the Core's join.
void world_on_world_loaded(PeerId from, Reader& r)
{
	if (!mgr().is_host())
		return;
	if (mapserver::is_bridge(from))
	{
		sync::host_end_join(from);
		mgr().session().send(from, Lane::Reliable, Msg::JoinComplete, Writer());
		join::host_send_owners();
		return;
	}
	join::player_world_loaded(from, r);
}

// ---------------------------------------------------------------- the world server: one player leaves his level, nobody else notices

// The exit `id` of the level `level`. On the level this process has loaded the level changer's game object says whether it is enabled;
// on another level only this process's copy of it is here.
CSE_ALifeLevelChanger* level_changer_on(u16 id, levels::LevelId level)
{
	if (id == kNoId || !ai().get_alife())
		return nullptr;
	if (level == levels::my_level())
		return join::exit_here(id);
	CSE_ALifeLevelChanger* lc = smart_cast<CSE_ALifeLevelChanger*>(ai().alife().objects().object(id, true));
	if (!lc || levels::level_of_vertex(lc->m_tGraphID) != level || !ai().game_graph().valid_vertex_id(lc->m_tNextGraphID))
		return nullptr;
	return lc;
}

void send_move_result(PeerId to, u32 seq, u8 verdict, u8 reason, u16 level)
{
	levels::wire::LevelMoveResultMsg m;
	m.seq = seq;
	m.verdict = verdict;
	m.reason = reason;
	m.level = level;
	Writer w(12);
	m.write(w);
	mgr().session().send(to, Lane::Reliable, Msg::LevelMoveResult, w);
}

// A player asks to leave his level, through an exit of it (its id, in the world server's numbering) or on a trip (a guide, a fast travel,
// a story ChangeLevel: the point he goes to). What he sent is a wish: the exit is the world server's own entity and he must stand at it;
// the verdict is the world server's (levels::host_judge_move); a move that goes puts his character on the new level and he joins again.
void on_level_move(PeerId from, Reader& r)
{
	using Result = levels::wire::LevelMoveResultMsg;
	if (!mgr().is_host())
		return;
	levels::wire::LevelMoveMsg m;
	if (!m.read(r))
	{
		::Msg("! [coop] '%s' asked to leave the level with a message that does not read", nick_of(from).c_str());
		return;
	}
	const u16 proxy_id = join::proxy_of_peer(from);
	const levels::LevelId here = levels::level_of_player(from);
	const bool trip = m.kind == levels::wire::LevelMoveMsg::Trip; // the same rules, no exit
	const CSE_ALifeLevelChanger* lc = !trip ? level_changer_on(m.changer, here) : nullptr;
	CSE_ALifeDynamicObject* proxy = (proxy_id != kNoId && ai().get_alife()) ? ai().alife().objects().object(proxy_id, true) : nullptr;
	Fvector trip_to, trip_dir;
	trip_to.set(m.pos[0], m.pos[1], m.pos[2]);
	trip_dir.set(m.dir[0], m.dir[1], m.dir[2]);
	const bool trip_ok = trip && proxy && ai().game_graph().valid_vertex_id(GameGraph::_GRAPH_ID(m.graph)) && _valid(trip_to) && _valid(trip_dir);
	// in the world served now and done loading it (join::host_player_in_world), no group's move under way, his character free
	if (!levels::split() || (!lc && !trip_ok) || !proxy || proxy->ID_Parent != kNoId || !join::host_player_in_world(proxy_id) ||
	    join::group_moves())
	{
		send_move_result(from, m.seq, Result::Refused, Result::Invalid, levels::wire::kNoLevel);
		return;
	}
	// Stand b57h creator10_server: a trip asked as he went down went out after his death; it moved a dead player (srv::departure::may_leave).
	const survival::Life life = survival::life_of(proxy_id);
	if (!srv::departure::may_leave(u8(life)))
	{
		if (++g_world.moves_refused <= kRefusalsLogged)
			::Msg("! [coop] '%s' asks to leave the level, %s: refused (a level is left standing)", nick_of(from).c_str(),
			    survival::life_name(life));
		send_move_result(from, m.seq, Result::Refused, Result::Invalid, levels::wire::kNoLevel);
		return;
	}
	const CObject* body = join::player_body_here(from);
	if (lc)
	{
		const float dist = join::exit_distance(*lc, body ? body->Position() : proxy->o_Position);
		if (dist > (body ? kMoveReachM : kMoveReachFarM))
		{
			if (++g_world.moves_refused <= kRefusalsLogged)
				::Msg("! [coop] '%s' asks to leave through level exit %u from %.1f m away: ignored", nick_of(from).c_str(), m.changer, dist);
			send_move_result(from, m.seq, Result::Refused, Result::Invalid, levels::wire::kNoLevel);
			return;
		}
	}
	const GameGraph::_GRAPH_ID to_graph = lc ? lc->m_tNextGraphID : GameGraph::_GRAPH_ID(m.graph);
	const u32 to_node = lc ? lc->m_dwNextNodeID : (m.node != levels::wire::kNoNode ? m.node : ai().game_graph().vertex(to_graph)->level_vertex_id());
	const Fvector to_pos = lc ? lc->m_tNextPosition : trip_to;
	const Fvector to_dir = lc ? lc->m_tAngles : trip_dir;
	const levels::LevelId target = levels::level_of_vertex(to_graph);
	switch (levels::host_judge_move(from, target))
	{
	case levels::MoveVerdict::Go: break;
	case levels::MoveVerdict::Board:
		// A level entered together waits at the group's waiting point (the co-op's). A server world has none:
		// effective_gate folds Together into BetterTogether and decide_move never says Board here. Were it ever to,
		// the move is refused rather than left waiting for a board nobody runs.
		::Msg("! [coop] '%s' asks to leave for level '%s', which the group enters together: no waiting point in a server world, refused",
		    nick_of(from).c_str(), level_name_of(to_graph).c_str());
		send_move_result(from, m.seq, Result::Refused, Result::Invalid, target);
		return;
	case levels::MoveVerdict::RefuseCap: send_move_result(from, m.seq, Result::Refused, Result::Cap, target); return;
	case levels::MoveVerdict::RefuseInvalid: send_move_result(from, m.seq, Result::Refused, Result::Gate, target); return;
	}
	// Return the player's held units first, then teleport the character so the rejoin snapshot shows the new level.
	levels::host_player_moves(from, target);
	alife_rw().teleport_object(proxy->ID, to_graph, to_node, to_pos);
	proxy->o_Angle = to_dir;
	send_move_result(from, m.seq, Result::Go, Result::None, target);
	::Msg("[coop] '%s' leaves for level '%s' (%s), the world stays", nick_of(from).c_str(), level_name_by_id(target).c_str(),
	    trip ? "a trip" : "a level exit");
	join::host_rejoin_peer(from, "you went to another level");
}

// ---------------------------------------------------------------- a map server

// The world server replaced its world (a load, a level change of its own): this map server exits even if the reason or epoch does not read.
void map_on_session_reset(PeerId from, Reader& r)
{
	if (from != kHostPeer || !mgr().is_client())
		return; // only the world server sends it (the session kicks anybody else, msg_sender)
	r.str();
	r.u32();
	if (!r.ok())
		::Msg("! [coop] malformed SessionReset from the host");
	mapserver::server_session_over(); // its world is gone: a new process starts for the new one
}

// ---------------------------------------------------------------- a player of a server world: the feed lines

// One line into this player's PDA feed (and the log): the strings of st_srv_feed.xml filled, never formatted: the nick is the other
// player's own text.
void feed_show(const feed::Line& line)
{
	const bool went = line.kind == feed::Kind::WentTo;
	const std::string nick = nick_of(PeerId(line.peer));
	const std::string tech = level_name_by_id(line.level);
	const std::string shown_level = tech.empty() ? std::string("?") : translated(tech.c_str());
	const std::string text = feed::fill(translated(went ? "st_srv_feed_went" : "st_srv_feed_came"), feed::shown_nick(nick), shown_level);
	::Msg("[srv] feed: '%s' %s %s (level %u)", nick.c_str(), went ? "went to" : "came from", tech.empty() ? "?" : tech.c_str(),
	    u32(line.level));
	CActor* actor = smart_cast<CActor*>(g_actor); // not Actor(): it asserts without a game (host_actor_faction, GS-2)
	if (!actor || !g_pGameLevel)
		return;
	GAME_NEWS_DATA news;
	news.m_type = GAME_NEWS_DATA::eNews;
	news.news_caption = translated("st_srv_feed_caption").c_str();
	news.news_text = text.c_str();
	news.texture_name = "ui_iconsTotal_grouping"; // the co-op's lines (coop_main.on_chat)
	actor->AddGameNews(news);
}
} // namespace

// ---------------------------------------------------------------- the world server

dedicated::WorldFacts world_facts()
{
	dedicated::WorldFacts f;
	f.level = g_pGameLevel != nullptr;
	f.ready = f.level && g_pGameLevel->bReady;
	f.precache = Device.dwPrecacheFrame != 0;
	f.alife = ai().get_alife() != nullptr;
	const CSE_ALifeCreatureActor* actor = f.alife ? ai().alife().graph().actor() : nullptr;
	f.alife_actor = actor != nullptr;
	f.client_actor = g_actor != nullptr;
	f.alive = actor && actor->get_health() > 0.f;
	return f;
}

void join_install_world(Session& s)
{
	g_world = WorldJoin();
	s.on(Msg::JoinRequest, &world_on_join_request);
	s.on(Msg::WorldLoaded, &world_on_world_loaded);
	s.on(Msg::LevelMove, &on_level_move); // the world decides whether a player can move alone
}

void join_world_frame()
{
	PROF_EVENT("coop/coop_srv_join/join_world_frame");
	if (g_world.map_wait.empty())
		return;
	const std::vector<PeerId> waiting = g_world.map_wait;
	g_world.map_wait.clear();
	for (const PeerId bridge : waiting)
		if (mgr().session().joined(bridge)) // still connected (A-S4Sb: the session's own question, no server's part)
			serve_bridge(bridge);
}

// ---------------------------------------------------------------- a map server

void join_install_map(Endpoint& world) { world.on(Msg::SessionReset, &map_on_session_reset); }

// ---------------------------------------------------------------- a player of a server world

void feed_start()
{
	g_feed.watch.reset();
	g_feed.next_at = 0;
}

void feed_frame()
{
	PROF_EVENT("coop/coop_srv_join/feed_frame");
	const u64 now = mgr().now_ms();
	if (now < g_feed.next_at)
		return;
	g_feed.next_at = now + kFeedStepMs;
	// his own level: none while his machine loads a world (no actor yet): the table is compared when he is in one again
	const levels::LevelId mine = mgr().is_live_client() && g_actor && world_up() ? levels::my_level() : levels::LevelId(feed::kNoLevel);
	const PeerId self = mgr().self();
	std::vector<feed::Row> rows;
	for (const PeerInfo& p : mgr().session().peers())
		if (p.id != kHostPeer && p.id != self && p.plays) // the world server's seat plays nobody
			rows.push_back(feed::Row{u32(p.id), u16(levels::level_of_player(p.id))});
	for (const feed::Line& line : g_feed.watch.step(rows, u16(mine)))
		feed_show(line);
}
} // namespace srv
} // namespace coop
