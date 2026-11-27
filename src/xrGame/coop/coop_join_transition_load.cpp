#include "stdafx.h"
#include "pch_script.h"
#include "coop_join_impl.h"
#include "coop_debug.h"
#include "coop_manager.h"
#include "coop_sync.h"
#include "coop_hud_text.h"
#include "coop_players.h"
#include "coop_alife.h"
#include "coop_net.h"
#include "coop_script.h"
#include "coop_vehicles.h"
#include "xrCoop/coop_mode.h"
#include "xrCoop/coop_ui_rules.h"

#include "../../xrEngine/x_ray.h"
#include "string_table.h"
#include "../../xrEngine/xr_input.h"
#include "ai_space.h"
#include "Level.h"
#include "Actor.h"
#include "ActorEffector.h"
#include "PostprocessAnimator.h"
#include "alife_simulator.h"
#include "alife_object_registry.h"
#include "xrServer.h"
#include "../xrServerEntities/xrMessages.h"

#include <algorithm>

extern bool g_bDisableAllInput; // Level_input.cpp: the level takes no input (level.disable_input)

namespace coop
{
namespace join
{
LoadState g_load;

namespace
{
// ---------------------------------------------------------------- the simultaneous load of a group transition
// xrCoop/coop_transition.h (PreparedLoad, LoadScreen). On "go" the host's own level change writes the shared
// save of the transition (CALifeUpdateManager::change_level, coop_auto_N) with every moving client's character already on
// the new level (host_place_movers). The world server gives it the next world epoch and sends every mover LoadWait
// {Prepared} and that save as a blob BEFORE its own reload (the engine reconnects a frame or two later), and hands the
// saves to the network first (host_pump_saves): the session tops a peer's Bulk lane up to 512 KB only from update(), which
// a level load does not call. Everybody loads the same save at once; a mover's screen stays (hold_loading_screen) until
// LoadWait {Enter}; his WorldLoaded after it ends his join as every join ends (host_end_join, JoinComplete). No SessionReset
// and no JoinRequest for a mover: the world is already his. Whoever is not in the world at the go (loading, choosing a
// character) rejoins after the host's load as before. The log lines "[coop] load:" carry the times of each step.

constexpr u64 kPreparedPumpMs = 8000; // the longest the host's reload waits for the saves to be handed to the network

constexpr u64 kFreshSaveSlackMs = 2000; // the file time of the level change's save may lag the clock this much (FAT)

// Where the group arrives: the point the actor's level change puts him at.
struct Arrival
{
	GameGraph::_GRAPH_ID graph = GameGraph::_GRAPH_ID(-1);
	u32 node = u32(-1);
	Fvector position = {0.f, 0.f, 0.f};
};

// A client who goes with the group, and where his character stood (put back if the level change wrote nothing).
struct Mover
{
	PeerId peer = kInvalidPeer;
	u16 proxy = kNoId;
	GameGraph::_GRAPH_ID graph = 0;
	u32 node = 0;
	Fvector position = {0.f, 0.f, 0.f};
};

// The clients who go with the group: in the world, their character known and free (one that sits in something, or a
// player still loading or choosing, rejoins after the host's load as before). The co-op: everybody stands on the host's level.
xr_vector<Mover> host_pick_movers()
{
	xr_vector<Mover> out;
	if (!ai().get_alife())
		return out;
	for (const PeerInfo& p : mgr().session().peers())
	{
		const auto it = g_join.peers.find(p.id);
		transition::MoverFacts facts;
		facts.joined = p.state == PeerState::Joined;
		facts.known = it != g_join.peers.end();
		facts.loading = facts.known && it->second.loading;
		facts.proxy = facts.known && it->second.proxy != kNoId;
		facts.joining = sync::host_peer_joining(p.id);
		facts.choosing = g_char.choosers.in_choice(p.id);
		if (!transition::goes_with_the_group(facts))
			continue;
		const CSE_ALifeDynamicObject* e = ai().alife().objects().object(it->second.proxy, true);
		if (!e || e->ID_Parent != kNoId)
			continue;
		Mover m;
		m.peer = p.id;
		m.proxy = e->ID;
		m.graph = e->m_tGraphID;
		m.node = e->m_tNodeID;
		m.position = e->o_Position;
		out.push_back(m);
	}
	return out;
}

// The movers' characters go where the group arrives (around the actor's point, as bring_proxy_to_level puts a late one),
// so the level change's save has them on the new level. teleport_object takes one that is online offline: the old world
// goes within a frame or two, and its player's world is frozen from his LoadWait on.
void host_place_movers(const xr_vector<Mover>& movers, const Arrival& at)
{
	static const float kAround[][2] = {
	    {1.5f, 0.f}, {0.f, 1.5f}, {-1.5f, 0.f}, {0.f, -1.5f}, {1.5f, 1.5f}, {-1.5f, -1.5f}, {1.5f, -1.5f}};
	constexpr size_t kPlaces = sizeof(kAround) / sizeof(kAround[0]);
	const levels::LevelId level = levels::level_of_vertex(at.graph);
	size_t i = 0;
	for (const Mover& m : movers)
	{
		Fvector pos = at.position;
		pos.x += kAround[i % kPlaces][0];
		pos.z += kAround[i % kPlaces][1];
		++i;
		alife_rw().teleport_object(m.proxy, at.graph, at.node, pos);
		mark_arrival(m.proxy); // the point's height, not the ground's: his machine puts him on the ground
		players::arrival_host_leaves(m.peer, m.proxy, nick_of_peer(m.peer).c_str(), level_name_by_id(level).c_str(),
		    at.position.x, at.position.y, at.position.z);
	}
}

// The level change wrote no save: the movers' characters go back where they stood, into the world again.
void host_unplace_movers(const xr_vector<Mover>& movers)
{
	for (const Mover& m : movers)
	{
		if (!ai().alife().objects().object(m.proxy, true))
			continue;
		alife_rw().teleport_object(m.proxy, m.graph, m.node, m.position);
		bring_online(m.proxy);
	}
}

// The save of the level change that just ran: its name (what the new level loads, the server's command line up to the
// first '/') and whether its file was written since `before` (else the level change wrote nothing, its stock refusal).
bool level_change_save(const FILETIME& before, std::string& name)
{
	name.clear();
	const shared_str* line = ai().get_alife() ? ai().alife().server_command_line() : nullptr;
	if (!line || !line->size())
		return false;
	const char* text = line->c_str();
	const char* slash = strchr(text, '/');
	name.assign(text, slash ? size_t(slash - text) : size_t(xr_strlen(text)));
	if (name.empty())
		return false;
	string_path scop;
	save_path(scop, name.c_str(), ".scop");
	WIN32_FILE_ATTRIBUTE_DATA fa;
	if (!GetFileAttributesExA(scop, GetFileExInfoStandard, &fa))
		return false;
	ULARGE_INTEGER wrote, since;
	wrote.LowPart = fa.ftLastWriteTime.dwLowDateTime;
	wrote.HighPart = fa.ftLastWriteTime.dwHighDateTime;
	since.LowPart = before.dwLowDateTime;
	since.HighPart = before.dwHighDateTime;
	return wrote.QuadPart + kFreshSaveSlackMs * 10000ull >= since.QuadPart; // FILETIME: 100 ns
}

// A prepared world for the player whose character is `proxy`: the layout of a join's world save (client_load_world). Its
// list of online objects is the host's actor alone (on the client he is the host's avatar and gets no EntitySwitch ever,
// host_collect_online): the host's new world brings its objects online after this save, and those switches reach the mover
// with the catch-up of his join (queued from the host's world change on, flushed by host_end_join).
std::vector<u8> prepared_blob(u16 proxy, const std::vector<u8>& engine_part, const std::vector<u8>& script_part)
{
	Writer w(engine_part.size() + script_part.size() + 32);
	w.u16(proxy);
	w.u32(1);
	w.u16(0);
	w.u32((u32)engine_part.size());
	w.bytes(engine_part.data(), engine_part.size());
	w.u32((u32)script_part.size());
	if (!script_part.empty())
		w.bytes(script_part.data(), script_part.size());
	return std::vector<u8>(w.data().begin(), w.data().end());
}

levels::wire::LoadWaitMsg load_wait_msg(u8 state, u32 epoch)
{
	levels::wire::LoadWaitMsg m;
	m.state = state;
	m.epoch = epoch;
	m.level = g_load.load_level;
	if (state == levels::wire::LoadWaitMsg::Waiting || state == levels::wire::LoadWaitMsg::Enter)
		for (const auto& row : g_load.load.rows())
		{
			if (m.rows.size() >= kMaxPeerSlots)
				break;
			levels::wire::LoadWaitMsg::Row r;
			r.peer = row.first;
			r.ready = row.second ? 1 : 0;
			m.rows.push_back(r);
		}
	return m;
}

// To a remote mover, or to the host player over the local link (peer 0).
void host_load_send(PeerId to, const levels::wire::LoadWaitMsg& m)
{
	Writer w(16 + 5 * m.rows.size());
	if (m.write(w)) // false only for more rows than players: never (load_wait_msg)
		mgr().session().send(to, Lane::Reliable, Msg::LoadWait, w);
}

// The prepared saves go to the network before the host's reload: the session's update() hands a peer's blob to ENet
// 512 KB at a time and a level load calls no update() for tens of seconds (only the keep-alive thread runs ENet then),
// so the host pumps the session here until every save is handed over, kPreparedPumpMs at most; what is left then goes
// after the host's load. The players' requests made meanwhile are of the old epoch (dropped by the session); a new join
// waits (load_pumping, host_join_request).
// When is every mover's save handed over? The session tells no peer's outgoing progress, only what it sent of each
// message type. After the go's epoch move only the movers' saves stream: every other blob was cut by the move and ends with
// no chunk more, and no new one starts while load_pumping holds the joins. So the data of the BlobChunks sent since the pump
// began reaching the movers' total means each mover's save is out (none sends more than its own size; transition::
// saves_handed), whatever another peer's cut blob does. The BlobEnd of a save may wait for room in its lane (512 KB alive)
// after its last chunk, and a cut blob's BlobEnd counts in the same total: the pump goes on while fewer than `blobs` BlobEnds
// went out, and a settle time of two round trips of the slowest mover after both hold, for a lagging one to follow.
void host_pump_saves(const xr_vector<PeerId>& movers, u32 blobs, u64 bytes)
{
	Session& s = mgr().session();
	const uint64_t chunk_bytes0 = s.stats(u16(Msg::BlobChunk)).sent_bytes;
	const uint64_t chunks0 = s.stats(u16(Msg::BlobChunk)).sent_count;
	const uint64_t ends0 = s.stats(u16(Msg::BlobEnd)).sent_count;
	u32 rtt = 0;
	for (const PeerInfo& p : s.peers())
		if (std::find(movers.begin(), movers.end(), p.id) != movers.end())
			rtt = _max(rtt, p.rtt_ms);
	const u64 settle = _min<u64>(1000, 2 * u64(rtt) + 100);
	const u64 start = mgr().now_ms();
	u64 out_at = 0;
	bool handed = false;
	g_load.load_pumping = true;
	while (mgr().now_ms() - start < kPreparedPumpMs)
	{
		s.update(mgr().now_ms());
		const MsgStats& chunk = s.stats(u16(Msg::BlobChunk));
		const bool data = transition::saves_handed(chunk.sent_bytes - chunk_bytes0, chunk.sent_count - chunks0, bytes);
		if (data && s.stats(u16(Msg::BlobEnd)).sent_count >= ends0 + blobs)
		{
			if (out_at == 0)
				out_at = mgr().now_ms();
			if (mgr().now_ms() - out_at >= settle)
			{
				handed = true;
				break;
			}
		}
		Sleep(1);
	}
	g_load.load_pumping = false;
	::Msg("[coop] load: %u save(s), %u KB, %s the network in %u ms%s", blobs, u32(bytes / 1024),
	    handed ? "handed to" : "partly handed to", u32(mgr().now_ms() - start),
	    handed ? " (the last 512 KB of each leave during the host's load)" : ", the rest follows when the host's world is up");
}

// The "go" of a group with movers, right after the host's level change (host_release). See the section's header.
void host_prepare(const xr_vector<Mover>& movers, u16 level, const FILETIME& before, u64 save_ms)
{
	std::string name;
	if (!level_change_save(before, name))
	{
		::Msg("! [coop] load: the level change wrote no save (%s): no simultaneous load, the players rejoin the usual way",
		    name.empty() ? "no name" : name.c_str());
		host_unplace_movers(movers);
		return;
	}
	string_path scop, scoc;
	save_path(scop, name.c_str(), ".scop");
	save_path(scoc, name.c_str(), ".scoc");
	std::vector<u8> engine_part, script_part;
	if (!read_file(scop, engine_part) || engine_part.empty())
	{
		// the characters are on the new level in that save already: their usual rejoin finds them there
		::Msg("! [coop] load: cannot read the save of the level change (%s): the players rejoin the usual way", scop);
		return;
	}
	read_file(scoc, script_part); // absent if Anomaly's script storage is disabled
	Session& s = mgr().session();
	const u64 now = mgr().now_ms();
	const u32 epoch = g_join.epoch + 1;
	levels::wire::LoadWaitMsg prepared;
	prepared.state = levels::wire::LoadWaitMsg::Prepared;
	prepared.epoch = epoch;
	prepared.level = level;
	prepared.reason = levels::wire::LoadWaitMsg::Group;
	// 0. every seat in a car is released while the movers still hear this world (later the message would be queued to a
	//    joining mover and dropped by his world gate)
	vehicles::release_all_seats(vehicles::Release::LeftLevel);
	// 1. LoadWait Prepared to every mover while his session is still in this epoch (nothing goes to a client of another)
	for (const Mover& m : movers)
		host_load_send(m.peer, prepared);
	// 2. the world epoch of the transition, now: the host's process keeps it for its new world (on_world_created)
	g_join.epoch = epoch;
	s.set_world_epoch(epoch);
	g_load.load_world_pending = true;
	// 3. the movers' joins (their world traffic waits for their WorldLoaded) and their saves, queued after the epoch moved
	//    (a blob carries the epoch it was queued in; one queued before would be cancelled by the move)
	for (auto& it : g_join.peers)
		it.second.prepared = false; // a mover of an earlier load who is not in yet rejoins the usual way
	std::vector<uint32_t> ids{kHostPeer};
	u64 bytes = 0;
	std::string names;
	for (const Mover& m : movers)
	{
		HostPeer& hp = g_join.peers[m.peer];
		hp.prepared = true;
		hp.loading = true;
		hp.sent_at = 0; // the join's time limit starts when the host's world is up (host_watch_world)
		sync::host_begin_join(m.peer);
		std::vector<u8> blob = prepared_blob(m.proxy, engine_part, script_part);
		bytes += blob.size();
		s.send_blob(m.peer, BlobPreparedSave, std::move(blob));
		ids.push_back(m.peer);
		names += names.empty() ? "" : ", ";
		names += nick_of_peer(m.peer);
	}
	g_load.load_level = level;
	g_load.load.start(epoch, ids, now);
	g_load.load_go_at = now;
	g_load.load_up_at = 0;
	g_load.load_sent_at = 0;
	g_load.load_sent_sig = 0;
	// 4. the host player over the local link, after the epoch moved (a link packet of an older epoch is dropped at the pump)
	host_load_send(kHostPeer, prepared);
	::Msg("[coop] load: the transition's save '%s' was written in %u ms (%u + %u bytes); epoch %u to '%s': %s load it "
	      "with the host",
	    name.c_str(), u32(save_ms), u32(engine_part.size()), u32(script_part.size()), epoch,
	    level_name_by_id(level).c_str(), names.c_str());
	// 5. the saves to the network before the reload
	xr_vector<PeerId> mover_ids;
	for (const Mover& m : movers)
		mover_ids.push_back(m.peer);
	host_pump_saves(mover_ids, u32(movers.size()), bytes);
	g_load.load_pumped_at = mgr().now_ms(); // the reload comes a frame or two from now (host_load_watch)
}
} // namespace

// A mover is no mover any more (he left, asks for the world the usual way, joins again).
void host_load_forget(PeerId peer, const char* why)
{
	const auto it = g_join.peers.find(peer);
	const bool was = (it != g_join.peers.end() && it->second.prepared) || g_load.load.is_mover(peer);
	if (it != g_join.peers.end())
		it->second.prepared = false;
	if (g_load.load.is_mover(peer))
		g_load.load.drop(peer);
	if (was)
		::Msg("[coop] load: '%s' is no mover of the simultaneous load any more (%s)", nick_of_peer(peer).c_str(), why);
}

// Another world than the transition's came (a load meanwhile): its movers rejoin the usual way, the host player's screen
// does not wait for it (his link takes this message: it is pushed in the epoch that is current now).
void host_load_void(const char* why)
{
	if (!g_load.load.active())
		return;
	::Msg("! [coop] load: the simultaneous load of epoch %u is off (%s): its players rejoin the usual way", g_load.load.epoch(),
	    why);
	host_load_send(kHostPeer, load_wait_msg(levels::wire::LoadWaitMsg::Cancel, g_load.load.epoch()));
	g_load.load.clear();
	g_load.load_world_pending = false;
	g_load.load_pumped_at = 0;
	for (auto& it : g_join.peers)
		it.second.prepared = false;
}

// Host, every frame: the reload follows the go within a frame or two (MakeReconnect is a deferred engine event); frames
// of the go's world that still run kReloadWatchMs after its saves went out mean the reload is not coming (a level being
// built runs frames as well, without a world up: those say nothing). The prepared load is called off (so no later world is
// taken for the transition's), and everybody rejoins this
// world as after a world change (host_rejoin_peer one by one would miss a player of the old epoch, who must hear the new
// one): the epoch moved at the go, the movers' characters were put on the other level in this world (their JoinRequest
// brings them back, bring_proxy_to_level).
void host_load_watch(u64 now)
{
	if (!transition::reload_overdue(g_load.load_world_pending, g_load.load_pumped_at, now, world_up()))
		return;
	::Msg("! [coop] load: no reload followed the go in %u s: the simultaneous load is off, everybody rejoins this world",
	    u32(transition::kReloadWatchMs / 1000));
	host_load_void("no reload followed the go");
	g_join.gate.world_replaced();
	g_join.precache_wait.clear();
	g_char.choosers.world_replaced();
	transition_world_replaced();
	for (auto& it : g_join.peers)
	{
		it.second.loading = true;
		it.second.sent_at = 0;
		it.second.proxy = kNoId; // set again by their JoinRequest (the character by key)
		sync::host_begin_join(it.first);
	}
	g_join.reset_pending = true; // host_watch_world sends the SessionReset of the epoch the go gave
}

// Host, every frame: the barrier of the load. Enter to whoever may leave his screen now, Waiting (who is ready) to the
// others on a change and every kStateEveryMs, the end of the load when everybody is in.
void host_load_step(u64 now)
{
	if (!g_load.load.active() || !g_load.load.world_is_up())
		return;
	const std::vector<uint32_t> entering = g_load.load.take_entering(now);
	if (!entering.empty())
	{
		const levels::wire::LoadWaitMsg enter = load_wait_msg(levels::wire::LoadWaitMsg::Enter, g_load.load.epoch());
		std::string names;
		for (uint32_t peer : entering)
		{
			host_load_send(peer, enter);
			names += names.empty() ? "" : ", ";
			names += nick_of_peer(peer);
		}
		::Msg("[coop] load: Enter for %s: %.1f s after the go, %.1f s after the host's world came up", names.c_str(),
		    float(now - g_load.load_go_at) / 1000.f, float(now - g_load.load_up_at) / 1000.f);
	}
	if (g_load.load.finished())
	{
		::Msg("[coop] load: done, every player is in (%.1f s after the go)", float(now - g_load.load_go_at) / 1000.f);
		g_load.load.clear();
		return;
	}
	u32 sig = 17u;
	for (const auto& row : g_load.load.rows())
		sig = sig * 31u + row.first * 2u + (row.second ? 1u : 0u);
	const bool changed = sig != g_load.load_sent_sig;
	if (now - g_load.load_sent_at < (changed ? 100u : u64(transition::kStateEveryMs)))
		return;
	g_load.load_sent_sig = sig;
	g_load.load_sent_at = now;
	const levels::wire::LoadWaitMsg waiting = load_wait_msg(levels::wire::LoadWaitMsg::Waiting, g_load.load.epoch());
	for (uint32_t peer : g_load.load.movers())
		if (!g_load.load.entered(peer))
			host_load_send(peer, waiting);
}

// A player's level has no map server up yet. His screen stays after his world is up (the world epoch does not move, he
// has his save as every joiner has: LoadWait Prepared only says "stay"), the barrier waits for him and for the server, and no Waiting
// goes to him: his screen must not let go by itself onto a map nobody leads. A server that never comes ends it by a rejoin.
void host_hold_for_server(PeerId from, u16 level)
{
	LoadState::Held& h = g_load.held[from];
	h.level = level;
	h.barrier.start(g_join.epoch, {from}, true, mgr().now_ms());
	levels::wire::LoadWaitMsg m;
	m.state = levels::wire::LoadWaitMsg::Prepared;
	m.epoch = g_join.epoch;
	m.level = level;
	host_load_send(from, m);
	::Msg("[coop] load: '%s' waits on his loading screen for the map server of level '%s'", nick_of_peer(from).c_str(),
	    level_name_by_id(level).c_str());
}

void host_held_step(u64 now)
{
	if (!g_load.map_save.engine.empty() && now - g_load.map_save.at >= kMapSaveMaxAgeMs)
		g_load.map_save = LoadState::MapSave(); // tens of MB: no server came for it
	for (auto it = g_load.held.begin(); it != g_load.held.end();)
	{
		LoadState::Held& h = it->second;
		if (mode().level_ready(h.level)) // the process that leads his level is up
			h.barrier.server_ready(g_join.epoch);
		else if (const auto peer = g_join.peers.find(it->first); peer != g_join.peers.end())
			peer->second.sent_at = now; // the server's own limits end this wait (ServerLife), not the time limit of a join
		if (!h.barrier.enters(it->first, now))
		{
			++it;
			continue;
		}
		levels::wire::LoadWaitMsg enter;
		enter.state = levels::wire::LoadWaitMsg::Enter;
		enter.epoch = g_join.epoch;
		enter.level = h.level;
		host_load_send(it->first, enter);
		::Msg("[coop] load: Enter for '%s': the map server of level '%s' is up", nick_of_peer(it->first).c_str(),
		    level_name_by_id(h.level).c_str());
		it = g_load.held.erase(it);
	}
}

// --- a player's side: his loading screen (a remote client, or the host player in the host's process)

// The header line of the loading screen while it is held: "Waiting for: <nicks>" (pApp->ls_header, put back after).
void load_screen_line(bool on)
{
	if (!pApp)
		return;
	if (!on)
	{
		if (g_load.screen_line)
			xr_strcpy(pApp->ls_header, g_load.screen_line_was.c_str());
		g_load.screen_line = false;
		return;
	}
	if (!g_load.screen_line)
	{
		g_load.screen_line_was = pApp->ls_header;
		g_load.screen_line = true;
	}
	// before the world server's word that its world is up, it is the host the screen waits for
	std::vector<uint32_t> who = g_load.screen.waiting_for;
	if (who.empty() && g_load.screen.host_up_at == 0)
		who.push_back(kHostPeer);
	// Or the line of the mode (a server's player: the map server of his level, a level the world server is not on)
	const char* const mode_line = mode().load_text(g_load.screen_level, g_load.screen.host_up_at != 0);
	std::string text;
	if (mode_line)
		text = CStringTable().translate(mode_line).c_str();
	else if (who.empty())
		text = CStringTable().translate("st_coop_load_all").c_str();
	else
	{
		text = CStringTable().translate("st_coop_load_wait").c_str();
		for (size_t i = 0; i < who.size(); ++i)
		{
			text += i ? ", " : " ";
			text += nick_of_peer(who[i]);
		}
	}
	if (xr_strcmp(pApp->ls_header, text.c_str()))
		xr_strcpy(pApp->ls_header, text.c_str());
}

// While the screen is held the level takes no input (the stock "press any key" grabs it and pauses; a session never
// pauses): the actor stands where he arrived. The keys held at that moment are let go (as level.disable_input does), and the
// flag a script had set is given back after (Esc still opens the main menu: CLevel::IR_OnKeyboardPress). The screen takes
// the input on the old level, in the frames after the go: a mod's animation with level.disable_input on would be read here
// and given back in the new world, where nothing clears it: only Esc would work, and every way back from the main menu would
// lead to a game that does not answer. The new actor's net_spawn clears the flag under the screen, hence every frame.
void load_screen_input(bool held)
{
	if (held)
	{
		if (!g_load.screen_input_off)
		{
			g_load.screen_input_was = g_bDisableAllInput;
			g_load.screen_input_world = g_world_generation;
			if (pInput)
				pInput->DeactivateSoft();
			g_load.screen_input_off = true;
		}
		g_bDisableAllInput = true;
		return;
	}
	if (!g_load.screen_input_off)
		return;
	g_bDisableAllInput = ui_rules::input_off_after_hold(g_load.screen_input_was, g_load.screen_input_world, g_world_generation);
	g_load.screen_input_off = false;
}

void load_screen_off()
{
	g_load.screen.reset();
	load_screen_line(false);
	load_screen_input(false);
}

namespace
{
void send_load_ready()
{
	levels::wire::LoadReadyMsg m;
	m.epoch = g_load.screen.epoch;
	Writer w(4);
	m.write(w);
	net::client_send(Msg::LoadReady, w); // the host player's over the local link
}
} // namespace

// Client: in the host's world: tell the host (its catch-up follows, host_end_join).
void client_go_live()
{
	mgr().set_phase(Phase::Live);
	mode().on_world_live(); // a map server parks the characters of this world until their players are on its level
	mgr().session().send(kHostPeer, Lane::Reliable, Msg::WorldLoaded, Writer());
	sync::on_client_live();
}

// A player, every frame: LoadReady when his world of the transition is up (and again while the screen is held), leave
// the screen at Enter (a client goes live), let go alone if no Enter comes, ask for the world the usual way if the save
// got lost, and the line on the screen.
void player_load_step(u64 now)
{
	if (g_load.screen.stage == transition::Load::None)
	{
		load_screen_line(false);
		load_screen_input(false);
		return;
	}
	const bool mirror = mgr().active() && !mode().decides_world();
	// a client: loaded and its roles swapped, as for its WorldLoaded; the host player: his process's world. In a
	// world created after the Prepared (LoadScreen::world_up checks the generation)
	const bool up = world_up() && !Device.dwPrecacheFrame &&
	    (!mirror || (mgr().phase() == Phase::LoadingWorld && g_join.swap_done));
	if (up && g_load.screen.world_up(now, g_world_generation))
	{
		if (mirror)
			g_join.receive_started = now; // the join's time limit measured his download and load; the held screen starts it again
		send_load_ready();
		::Msg("[coop] load: my world is up %.1f s after the go, waiting for the others", float(now - g_load.screen.since) / 1000.f);
	}
	else if (g_load.screen.repeat(now))
	{
		if (mirror)
			g_join.receive_started = now; // the host ends this wait: his own limits, or his link
		send_load_ready();
	}
	if (g_load.screen.alone(now))
	{
		::Msg("! [coop] load: no Enter from the host %u s after its world came up: entering alone",
		    u32((transition::kLoadWaitMs + transition::kLoadAloneGraceMs) / 1000));
		g_load.screen.enter(g_load.screen.epoch, now);
	}
	if (g_load.screen.stage == transition::Load::Entering && (up || !mirror))
	{
		::Msg("[coop] load: in, %.1f s after the go (%.1f s on the held screen)", float(now - g_load.screen.since) / 1000.f,
		    g_load.screen.up_at ? float(now - g_load.screen.up_at) / 1000.f : 0.f);
		load_screen_off();
		if (mirror)
			client_go_live();
		return;
	}
	if (mirror && g_load.screen.stalled(now))
	{
		::Msg("! [coop] load: the host's save stopped coming %u s ago: asking for the world the usual way",
		    u32(transition::kLoadBlobStallMs / 1000));
		load_screen_off();
		mgr().session().send(kHostPeer, Lane::Reliable, Msg::JoinRequest, Writer());
		return;
	}
	load_screen_line(g_load.screen.holds());
	load_screen_input(g_load.screen.holds());
}

// Client: a transition save continues the same world. While the player is live in the departing world,
// players::condition_carry_take preserves the player's condition, and coop_main.on_world_leaving lets scripts
// preserve per-player data in the session stash.
void world_leaving(u32 to_epoch)
{
	players::condition_carry_take(to_epoch);
	const script::CallResult r = script::call_now("coop_main", "on_world_leaving",
	    [](lua_State* L, const void* ctx) -> int
	    {
		    lua_pushnumber(L, lua_Number(*static_cast<const u32*>(ctx)));
		    return 1;
	    },
	    &to_epoch);
	if (r != script::CallResult::Done)
		::Msg("[coop] load: the scripts did not keep their per-player state for this load (%s): the last report is used",
		    r == script::CallResult::NotReady ? "Lua is not up" : "coop_main.on_world_leaving is missing or failed");
}

namespace
{
// The release of his seat may have come in the same update as what replaces his
// world (the host releases every seat right before LoadWait Prepared). The vehicle steps of this frame run now, once more (sync::frame
// runs them every frame): a released driver gets out by the stock exit, a released passenger stands at the decider's exit point, with
// the exit sound, while his world and its scripts are still up. Nobody sits: nothing runs.
void client_seat_steps_now()
{
	if (!vehicles::local_seat_attached())
		return;
	vehicles::client_frame();
	vehicles::ride_frame();
}

// Client: LoadWait Prepared: the group moves, the save of the transition follows as a blob. As a SessionReset, but the
// world comes by itself: no JoinRequest.
void client_prepared(const levels::wire::LoadWaitMsg& m, u64 now)
{
	if (!mgr().is_live_client())
	{
		::Msg("! [coop] load: the host prepares a world while this player is not in its world (%s): ignored",
		    phase_name(mgr().phase()));
		return;
	}
	if (m.epoch <= mgr().session().world_epoch() || !g_load.screen.prepared(m.epoch, true, now, g_world_generation))
	{
		::Msg("! [coop] load: a prepared world of epoch %u, this player is in epoch %u: ignored", m.epoch,
		    mgr().session().world_epoch());
		return;
	}
	g_load.screen_level = m.level;
	arrival_begin("group transition", m.level);
	// Out of his car first, by the normal detach, if its release is here already (the save waits
	// for a detach still to come: on_blob_complete)
	client_seat_steps_now();
	// The save that follows continues this world, and this player's own state goes through the load with him, as a
	// single player's does: the engine keeps his condition, the scripts keep theirs (the per-player keys go into the session
	// stash as they are this moment, not as they were at the last 15 s report). Both while his world and his actor are up.
	world_leaving(m.epoch);
	::Msg("[coop] load: the group moves to '%s' (epoch %u): the host's save comes now, everybody loads it at once",
	    level_name_by_id(m.level).c_str(), m.epoch);
	g_join.my_proxy = kNoId;
	g_join.load_issued = false;
	g_join.swap_done = false;
	transition_world_replaced(); // the host's board of the old world
	g_char.char_step = CharState::CharStep::None;
	debug::keep_own_flags_through_next_reset(); // the group's load: the session and its debug go on
	sync::reset();
	g_join.receive_started = now; // the join time limit starts again
	mgr().set_phase(Phase::ReceivingWorld);
	// The host is in the new epoch already (it moved right after this message); the save carries it
	mgr().session().set_world_epoch(m.epoch);
}
} // namespace

// Host: the level change of `goal`, the one the stock exit sends (built from the host's own entity, or from what the
// host's game asked for in a guide trip). The gate lets it by (g_board.releasing).
bool host_release(u32 goal)
{
	NET_Packet P;
	P.w_begin(M_CHANGE_LEVEL);
	Arrival at; // where the movers' characters go in the transition's save
	if (transition::is_guide(goal))
	{
		if (!g_board.guide.set)
		{
			::Msg("! [coop] transition: the guide trip is gone, nowhere to move the group");
			return false;
		}
		P.w(&g_board.guide.graph, sizeof(g_board.guide.graph));
		P.w(&g_board.guide.node, sizeof(g_board.guide.node));
		P.w_vec3(g_board.guide.position);
		P.w_vec3(g_board.guide.angles);
		at.graph = g_board.guide.graph;
		at.node = g_board.guide.node;
		at.position = g_board.guide.position;
		::Msg("[coop] moving the group to '%s' (guide trip)", g_board.guide.level.c_str());
	}
	else
	{
		const CSE_ALifeLevelChanger* lc = level_changer_of_goal(u16(goal));
		if (!lc)
		{
			::Msg("! [coop] the level exit %u is not usable any more", goal);
			return false;
		}
		P.w(&lc->m_tNextGraphID, sizeof(lc->m_tNextGraphID));
		P.w(&lc->m_dwNextNodeID, sizeof(lc->m_dwNextNodeID));
		P.w_vec3(lc->m_tNextPosition);
		P.w_vec3(lc->m_tAngles);
		at.graph = lc->m_tNextGraphID;
		at.node = lc->m_dwNextNodeID;
		at.position = lc->m_tNextPosition;
		::Msg("[coop] moving the group to '%s' (level exit %u)", level_name_of(lc->m_tNextGraphID).c_str(), goal);
	}
	// The clients in the world load the transition's save with the host (host_prepare); the others rejoin after.
	const xr_vector<Mover> movers = host_has_company() ? host_pick_movers() : xr_vector<Mover>();
	const levels::LevelId arrival = levels::level_of_vertex(at.graph);
	if (!movers.empty())
		host_place_movers(movers, at);
	arrival_begin(transition::is_guide(goal) ? "guide trip (host player)" : "level exit (host player)", arrival);
	FILETIME before;
	GetSystemTimeAsFileTime(&before);
	const u64 save_from = mgr().now_ms();
	g_board.releasing = true;
	Level().Send(P, net_flags(TRUE)); // the host's own level change: its save is written here, the reload comes a frame later
	g_board.releasing = false;
	const u64 save_ms = mgr().now_ms() - save_from;
	g_board.guide.set = false;
	g_board.released_at = mgr().now_ms();
	if (!movers.empty())
	{
		host_prepare(movers, level_id_of(at.graph), before, save_ms);
		g_board.released_at = mgr().now_ms(); // the old world's last frames are after the saves went out (host_pump_saves)
	}
	return true;
}

// He stands in an exit (host numbering). Once per exit and five seconds; refused, not again until he
// leaves its zone.
// The stock route darkens the screen before it asks to change the level (ui_sr_teleport, sleep_fade.ppe, id 1313): if the level
// does not change after all, nothing else takes that away.
void client_stays_on_level()
{
	constexpr int kRouteFade = 1313;
	if (!g_actor)
		return;
	if (CPostprocessAnimator* pp = smart_cast<CPostprocessAnimator*>(g_actor->Cameras().GetPPEffector(EEffectorPPType(kRouteFade))))
		pp->Stop(1.f);
}

namespace
{
// His LevelMove goes to the world server now (the exit he stands in, or a trip).
void move_out(const levels::wire::LevelMoveMsg& m, u64 now)
{
	Writer w(48);
	m.write(w);
	net::client_send(Msg::LevelMove, w);
	g_load.move_sent_at = now;
}
} // namespace

// A move while his actor sits in a managed seat waits. The world server would put him on another map
// at once, before the map server of this level (the decider of his seat) could release him and the release reach him. He is asked
// out (coop_vehicles.exit_for_travel: his VehicleReq exit goes to that map server while he is still its player); client_seat_holds
// sends the move once the release came and the normal detach ran here, or kSeatDetachHoldMs later. True: it went out now.
bool client_send_move(const levels::wire::LevelMoveMsg& m, u64 now)
{
	if (g_load.move_hold.step(vehicles::local_seat_attached(), now) == transition::TravelStep::Wait)
	{
		g_load.held_move = m;
		g_load.move_held = true;
		const script::CallResult asked = script::call_now("coop_vehicles", "exit_for_travel",
		    [](lua_State*, const void*) -> int { return 0; }, nullptr);
		::Msg("[coop] level move: this player sits in a car, out of it first (%s); the move waits for the detach",
		    asked == script::CallResult::Done ? "asked out" : "the scripts could not ask out");
		return false;
	}
	move_out(m, now);
	return true;
}

void client_level_move(u16 changer, u64 now)
{
	constexpr u64 kMoveAgainMs = 5000;
	if (g_load.move_held || changer == g_load.move_refused || !mgr().is_live_client() ||
	    (changer == g_load.move_changer && g_load.move_sent_at != 0 && now - g_load.move_sent_at < kMoveAgainMs))
		return;
	levels::wire::LevelMoveMsg m;
	m.seq = ++g_load.move_seq;
	m.kind = levels::wire::LevelMoveMsg::Exit;
	m.changer = changer;
	g_load.move_changer = changer;
	if (client_send_move(m, now))
		::Msg("[coop] level exit %u: asked the host to let me go", changer);
}

// A player's loading screen of the simultaneous load is up (the host player's over the local link; his
// readiness is the server's world as well, host_watch_world). It waits in the world gate while the server's world is not
// up (coop_world_gate.h LoadReady = Hold) and runs when it is, in the same epoch.
void host_on_load_ready(PeerId from, Reader& r)
{
	if (!mode().decides_world())
		return;
	levels::wire::LoadReadyMsg m;
	if (!m.read(r))
	{
		if (++g_load.loads_ignored <= kRefusalsLogged)
			::Msg("! [coop] load: a malformed LoadReady from #%u: ignored", from);
		return;
	}
	const auto held = g_load.held.find(from); // on his screen for his map server
	if (held != g_load.held.end())
		held->second.barrier.ready(from, m.epoch);
	if (!g_load.load.active() || m.epoch != g_load.load.epoch() || !g_load.load.is_mover(from))
		return; // a repeat after his Enter, or of a load that is over: nothing to do
	if (g_load.load.ready(from, m.epoch))
		::Msg("[coop] load: '%s' is ready, %.1f s after the go", nick_of_peer(from).c_str(),
		    float(mgr().now_ms() - g_load.load_go_at) / 1000.f);
}

// The world server's word on the simultaneous load (a remote client from his session, the host player
// from the local link).
void client_on_load_wait(PeerId from, Reader& r)
{
	if (from != kHostPeer || !mgr().active())
		return;
	levels::wire::LoadWaitMsg m;
	if (!m.read(r))
	{
		if (++g_load.load_waits_bad <= kRefusalsLogged)
			::Msg("! [coop] load: an unreadable LoadWait from the host: ignored");
		return;
	}
	const u64 now = mgr().now_ms();
	switch (m.state)
	{
	case levels::wire::LoadWaitMsg::Prepared:
		if (!mode().decides_world() && m.epoch == mgr().session().world_epoch() && mgr().phase() != Phase::Live)
		{
			// He is on his way in a join: his world comes as a joiner's, his screen stays until his map server is up
			if (g_load.screen.prepared(m.epoch, true, now, g_world_generation))
			{
				g_load.screen_level = m.level;
				if (g_join.load_issued)
					g_load.screen.loading();
			}
		}
		else if (!mode().decides_world())
			client_prepared(m, now); // the save follows: this machine loads it
		else if (g_load.screen.prepared(m.epoch, false, now, g_world_generation)) // the host player: his process reloads by itself
		{
			g_load.screen_level = m.level;
			if (!g_arrival.active())
				arrival_begin("group transition (host player)", m.level);
			else
				g_arrival.set_to(level_name_by_id(m.level).c_str());
			::Msg("[coop] load: the group moves to '%s' (epoch %u): this screen waits for the others too",
			    level_name_by_id(m.level).c_str(), m.epoch);
		}
		return;
	case levels::wire::LoadWaitMsg::Waiting:
	{
		std::vector<std::pair<uint32_t, bool>> rows;
		for (const levels::wire::LoadWaitMsg::Row& row : m.rows)
			rows.emplace_back(row.peer, row.ready != 0);
		g_load.screen.waiting(m.epoch, rows, now);
		return;
	}
	case levels::wire::LoadWaitMsg::Enter:
		if (g_load.screen.enter(m.epoch, now))
			::Msg("[coop] load: Enter from the host, %.1f s after the go", float(now - g_load.screen.since) / 1000.f);
		return;
	case levels::wire::LoadWaitMsg::Cancel:
		if (g_load.screen.cancel(m.epoch))
		{
			load_screen_line(false);
			::Msg("[coop] load: the host called the simultaneous load of epoch %u off", m.epoch);
		}
		return;
	default: // Away, Back: no decider sends them
		return;
	}
}

// --- one player leaves his level, nobody else notices (docs/SPLIT_LEVELS.md)
// The server decides whether a player can move alone. A session that only moves as a group can install this refusal.
void refuse_level_move(PeerId from, Reader& r)
{
	levels::wire::LevelMoveMsg request;
	if (!request.read(r))
	{
		::Msg("! [coop] '%s' asked to leave the level with a message that does not read", nick_of_peer(from).c_str());
		return;
	}
	using Result = levels::wire::LevelMoveResultMsg;
	const Result result{request.seq, Result::Refused, Result::Invalid, levels::wire::kNoLevel};
	Writer w(8);
	result.write(w);
	mgr().session().send(from, Lane::Reliable, Msg::LevelMoveResult, w);
}

// A player's ask (client_level_move, client_level_trip) and its answer.
void client_on_level_move_result(PeerId from, Reader& r)
{
	using Result = levels::wire::LevelMoveResultMsg;
	Result m;
	if (from != kHostPeer || mode().decides_world() || !m.read(r) || m.seq != g_load.move_seq)
		return;
	g_load.move_sent_at = 0;
	switch (m.verdict)
	{
	case Result::Go: ::Msg("[coop] level exit: the host lets me go to level %u", u32(m.level)); break;
	case Result::Together:
		g_load.move_board = true; // this exit is entered together: the board, as where the group travels together
		::Msg("[coop] level exit: level %u is entered together, the group waits here", u32(m.level));
		break;
	case Result::Refused:
		g_load.move_refused = g_load.move_changer;
		g_board.guide_yes = false; // a trip of his that was not taken is no yes to the one that waits
		::Msg("[coop] level exit: the host refused (reason %u, level %u)", u32(m.reason), u32(m.level));
		client_stays_on_level();
		hud_text::say(m.reason == Result::Cap ? "st_coop_levels_refused_cap" : "st_coop_levels_refused_invalid",
		    m.reason == Result::Cap ? "The host's PC cannot run another map server now." : "This exit cannot be used right now.");
		break;
	}
}

// The save of a simultaneous load is the one this machine waits for (LoadWait Prepared came, nothing loads yet).
bool prepared_save_awaited()
{
	return g_load.screen.stage == transition::Load::Waiting && mgr().phase() == Phase::ReceivingWorld && !g_join.load_issued;
}

// A player's machine, every frame before sync::frame (whose vehicle steps detach him): what waits
// for his actor to leave its seat goes once he is out, or kSeatDetachHoldMs after it began to wait (transition::SeatHold).
void client_seat_holds(u64 now)
{
	if (!g_load.seated_save.empty())
	{
		const transition::TravelStep step = g_load.save_hold.step(vehicles::local_seat_attached(), now);
		if (step != transition::TravelStep::Wait)
		{
			std::vector<u8> save;
			save.swap(g_load.seated_save);
			if (!prepared_save_awaited())
				::Msg("! [coop] load: the held save of the transition is not awaited any more (%s): dropped", phase_name(mgr().phase()));
			else
			{
				if (step == transition::TravelStep::GoLate)
					::Msg("! [coop] load: still in a car %u s after the save came: loading it anyway",
					    u32(transition::kSeatDetachHoldMs / 1000));
				else
					::Msg("[coop] load: out of the car, loading the save of the transition (%u KB)", u32(save.size() / 1024));
				g_load.screen.loading();
				client_load_world(save);
			}
		}
	}
	if (g_load.move_held)
	{
		const transition::TravelStep step = g_load.move_hold.step(vehicles::local_seat_attached(), now);
		if (step != transition::TravelStep::Wait)
		{
			g_load.move_held = false;
			if (!mgr().is_live_client())
				::Msg("! [coop] level move: the move held for the detach is void, this player left his world meanwhile");
			else
			{
				if (step == transition::TravelStep::GoLate)
					::Msg("! [coop] level move: still in a car after %u s, asking to move anyway",
					    u32(transition::kSeatDetachHoldMs / 1000));
				else
					::Msg("[coop] level move: out of the car, asking to move");
				move_out(g_load.held_move, now);
			}
		}
	}
}
} // namespace join

bool hold_loading_screen() { return mgr().active() && join::g_load.screen.holds(); }
} // namespace coop
