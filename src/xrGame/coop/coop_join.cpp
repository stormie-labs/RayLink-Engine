#include "stdafx.h"
#include "coop_join.h"
#include "coop_manager.h"
#include "coop_sync.h"
#include "coop_world.h"
#include "coop_entities.h"
#include "coop_players.h"
#include "coop_objects.h"
#include "coop_alife.h"
#include "coop_net.h"
#include "coop_script.h"

#include "../../xrEngine/XR_IOConsole.h"
#include "Level.h"
#include "level_changer.h"
#include "alife_simulator.h"
#include "alife_object_registry.h"
#include "alife_graph_registry.h"
#include "xrServer.h"
#include "xrServer_Objects_ALife_Monsters.h"
#include "xrServer_Objects_ALife_Items.h"
#include "../xrServerEntities/xrMessages.h"
#include "game_graph.h"
#include "character_community.h"

#include <cstdio>

extern string_path g_last_saved_game;

namespace coop
{
namespace join
{
namespace
{
constexpr LPCSTR kProxySection = "coop_player_avatar";
constexpr LPCSTR kClientSaveName = "coop_session";
constexpr u16 kNoId = 0xffff;

struct HostPeer
{
	u16 proxy = kNoId;
	bool loading = false; // world sent, waiting for WorldLoaded
	u64 sent_at = 0;
};

struct State
{
	// host
	xr_map<PeerId, HostPeer> peers;
	// client
	u16 my_proxy = kNoId;
	u64 receive_started = 0;
	bool load_issued = false;
	bool swap_done = false;
	u64 last_progress_log = 0;
	// host: the world we serve (world_generation() when it was seen); a load or a level change
	// replaces it (epoch++ in on_world_created, the session's world epoch with it: R2 I1)
	u32 world_seen = 0;
	u32 epoch = 1;
	bool reset_pending = false;
	// host: R1 I4, R2 I4: one join in flight per player, one per 5 s
	JoinGate gate;
	u32 joins_refused = 0; // diagnostics: refused JoinRequest / WorldLoaded / TransitionVote (the first few logged)
	u32 loaded_ignored = 0;
	u32 votes_refused = 0;
	// host: level exit asked by a client (R2 I3: the id of a level changer of the host's own level), waiting for
	// coop_accept_transition
	u16 pending_changer = kNoId;
	PeerId transition_from = kInvalidPeer;
	u64 transition_at = 0; // last vote (a repeat within 3 s does not notify Lua or the chat again)
} g;

constexpr u32 kRefusalsLogged = 5;
// R2 I4: StateDumpRequest = a client asks for an entity it misses (P3); a modified one could ask for the whole
// world in a loop, each answer a full spawn packet.
constexpr u32 kStateDumpBurst = 64;
constexpr u32 kStateDumpRefillMs = 50; // 20 per second after a burst

u32 g_world_generation = 0; // CALifeSimulator constructions in this process (outlives State)

// R1 I1: a player's character keeps in its custom data the SHA-256 of its player's key, never the key itself: the
// world save goes to every client, and a key is all it takes to play someone's character. Format, strict parse
// and migration of saves from before: coop::find_key_value / migrate_key_value (xrCoop/coop_session.h).

bool read_file(LPCSTR path, std::vector<u8>& out)
{
	out.clear();
	FILE* f = std::fopen(path, "rb");
	if (!f)
		return false;
	std::fseek(f, 0, SEEK_END);
	const long size = std::ftell(f);
	std::fseek(f, 0, SEEK_SET);
	if (size > 0)
	{
		out.resize(size_t(size));
		if (std::fread(out.data(), 1, out.size(), f) != out.size())
			out.clear();
	}
	std::fclose(f);
	return size >= 0;
}

bool write_file(LPCSTR path, const u8* data, size_t size)
{
	// FS writer creates missing folders (a fresh appdata has no savedgames yet).
	IWriter* w = FS.w_open(path);
	if (!w)
		return false;
	w->w(data, u32(size));
	FS.w_close(w);
	return FS.exist(path) != nullptr;
}

void save_path(string_path& out, LPCSTR name, LPCSTR ext)
{
	string_path file;
	xr_sprintf(file, "%s%s", name, ext);
	FS.update_path(out, "$game_saves$", file);
}

// The character of the player whose key has this digest, nullptr if he has none yet. On the way every character
// from an older save gets the digest of its key instead of the key (R1 I1): the world save made for the joining
// player right after carries nobody's key.
CSE_ALifeHumanStalker* find_proxy_by_key(const std::string& digest)
{
	CSE_ALifeHumanStalker* found = nullptr;
	for (const auto& it : ai().alife().objects().objects())
	{
		CSE_ALifeDynamicObject* e = it.second;
		if (!e || xr_strcmp(e->s_name, kProxySection) || !e->m_ini_string.size())
			continue;
		std::string ini = e->m_ini_string.c_str();
		if (migrate_key_value(ini))
		{
			e->m_ini_string = ini.c_str();
			::Msg("[coop] character %u: the player key in its custom data is replaced by its digest (older save)", e->ID);
		}
		size_t at = 0, len = 0;
		if (!found && find_key_value(ini, at, len) && ini.compare(at, len, digest) == 0)
			found = smart_cast<CSE_ALifeHumanStalker*>(e);
	}
	return found;
}

CSE_ALifeHumanStalker* create_proxy(const PeerInfo& p, const std::string& key_digest)
{
	CSE_ALifeCreatureActor* actor = ai().alife().graph().actor();
	if (!actor)
		return nullptr;
	Fvector pos = actor->o_Position;
	pos.x += 1.5f;
	CSE_Abstract* e = alife_rw().spawn_item(kProxySection, pos, actor->m_tNodeID, actor->m_tGraphID, kNoId);
	CSE_ALifeHumanStalker* proxy = smart_cast<CSE_ALifeHumanStalker*>(e);
	if (!proxy)
	{
		::Msg("! [coop] section [%s] is not a stalker — check gamedata (mod_system_coop.ltx)", kProxySection);
		if (e)
			alife_rw().release(e, true);
		return nullptr;
	}
	string512 ini;
	xr_sprintf(ini, "[coop]\nkey = %s\nnick = %s\n", key_digest.c_str(), p.nick.c_str()); // R1 I1: the digest
	proxy->m_ini_string = ini;
	proxy->m_character_name = p.nick.c_str();
	::Msg("[coop] new character for '%s': proxy id %u at [%.1f %.1f %.1f]", p.nick.c_str(), proxy->ID, pos.x, pos.y,
	    pos.z);
	return proxy;
}

// The group lives on one level. A character left behind (its player was away when the host
// changed level) is moved next to the host: the client loads the level of its character.
void bring_proxy_to_level(CSE_ALifeHumanStalker& proxy, LPCSTR nick)
{
	CSE_ALifeCreatureActor* actor = ai().alife().graph().actor();
	if (!actor || proxy.ID_Parent != kNoId)
		return;
	const CGameGraph& gg = ai().game_graph();
	if (!gg.valid_vertex_id(proxy.m_tGraphID) ||
	    gg.vertex(proxy.m_tGraphID)->level_id() == gg.vertex(actor->m_tGraphID)->level_id())
		return;
	Fvector pos = actor->o_Position;
	pos.x += 1.5f;
	alife_rw().teleport_object(proxy.ID, actor->m_tGraphID, actor->m_tNodeID, pos);
	::Msg("[coop] character of '%s' was on another level, moved to the group", nick);
}

// W9: a player who is not in the session has no character in the world. The proxy keeps its place, inventory
// and vitals in ALife (and in the save); coop::switch_vote keeps it offline until its player is back in the
// world. Taking it out now instead of waiting for the switch manager's next pass over it: the EntitySwitch the
// host sends makes it vanish on every client in the same moment.
void take_offline(u16 proxy)
{
	if (proxy == kNoId || !ai().get_alife())
		return;
	CSE_ALifeDynamicObject* e = ai().alife().objects().object(proxy, true);
	if (!e || !e->m_bOnline || e->ID_Parent != kNoId)
		return;
	alife_rw().switch_offline(e);
	::Msg("[coop] character %u is out of the world until its player returns", proxy);
}

// W9: the player has loaded the world: his character comes in now, through the switch manager's own checks
// (coop::switch_vote lets it, observer_distance keeps it online), instead of on its next pass, so it is there
// before the player's first request needs it. Until this moment it was offline, also in the save he loaded.
void bring_online(u16 proxy)
{
	if (proxy == kNoId || !ai().get_alife())
		return;
	CSE_ALifeDynamicObject* e = ai().alife().objects().object(proxy, true);
	if (!e || e->m_bOnline || e->ID_Parent != kNoId)
		return;
	const CGameGraph& gg = ai().game_graph();
	if (!gg.valid_vertex_id(e->m_tGraphID) || gg.vertex(e->m_tGraphID)->level_id() != ai().alife().graph().level().level_id())
		return; // not on this level (cannot happen after bring_proxy_to_level): ALife keeps it where it is
	alife_rw().switch_object(e);
	if (e->m_bOnline)
		::Msg("[coop] character %u is in the world", proxy);
}

// --- host: JoinRequest ---
// The world for a player whose join the gate admitted: his character (found by key, or a new one), a fresh save of
// the world, the blob. false if it could not be sent (the player is gone or was kicked with the reason).
bool host_send_world(PeerId from)
{
	const PeerInfo* p = mgr().session().peer(from);
	if (!p)
		return false;
	if (!host_can_start())
	{
		mgr().session().kick(from, RejectReason::HostNotReady, "host is not in a game right now, try again later");
		return false;
	}

	std::string key_digest;
	if (!key_digest_hex(p->key, key_digest))
	{
		::Msg("! [coop] cannot hash the player key of '%s' (BCrypt)", p->nick.c_str());
		mgr().session().kick(from, RejectReason::HostNotReady, "host could not check your player key");
		return false;
	}
	CSE_ALifeHumanStalker* proxy = find_proxy_by_key(key_digest);
	if (proxy)
	{
		::Msg("[coop] '%s' returns to character proxy id %u", p->nick.c_str(), proxy->ID);
		bring_proxy_to_level(*proxy, p->nick.c_str());
	}
	else
		proxy = create_proxy(*p, key_digest);
	if (!proxy)
	{
		mgr().session().kick(from, RejectReason::HostNotReady, "host could not create a character for you");
		return false;
	}
	HostPeer& hp = g.peers[from];
	hp.proxy = proxy->ID;
	hp.loading = true; // W9: not in the world before WorldLoaded: the save he gets has his character offline
	world::host_mark_proxy(proxy->ID, from);

	// Fresh world save: engine part (.scop) and Anomaly's script part (.scoc).
	string64 name;
	xr_sprintf(name, "coop_join_%u", from);
	// W9: writing and reading back the save blocks this frame (seconds on a big world); the others must not time
	// us out meanwhile (the save hook arms it too; reading the files back is ours).
	on_blocking_work();
	{
		// save() records every name it writes as the last save; this file is deleted below, and
		// "load last save" must keep pointing at the host's real one.
		string_path last_save;
		xr_strcpy(last_save, g_last_saved_game);
		NET_Packet packet;
		packet.w_begin(M_SAVE_GAME);
		packet.w_stringZ(name);
		packet.w_u8(0); // keep the host's own current save name
		Level().Send(packet, net_flags(TRUE));
		xr_strcpy(g_last_saved_game, last_save);
	}
	string_path scop, scoc;
	save_path(scop, name, ".scop");
	save_path(scoc, name, ".scoc");
	std::vector<u8> engine_part, script_part;
	const bool saved = read_file(scop, engine_part) && !engine_part.empty();
	if (saved)
		read_file(scoc, script_part); // absent if Anomaly's script storage is disabled
	mgr().session().hold_connection(0); // W9: done blocking (Manager::frame would release it next frame anyway)
	if (!saved)
	{
		::Msg("! [coop] world save failed (%s)", scop);
		mgr().session().kick(from, RejectReason::HostNotReady, "host could not save the world");
		return false;
	}

	xr_vector<u16> online;
	entities::host_collect_online(online);

	Writer w(engine_part.size() + script_part.size() + online.size() * 2 + 64);
	w.u16(proxy->ID);
	w.u32((u32)online.size());
	for (u16 id : online)
		w.u16(id);
	w.u32((u32)engine_part.size());
	w.bytes(engine_part.data(), engine_part.size());
	w.u32((u32)script_part.size());
	if (!script_part.empty())
		w.bytes(script_part.data(), script_part.size());
	std::vector<u8> blob(w.data().begin(), w.data().end());
	// R1 I4: a world save still queued or streaming to this player is replaced by this one (Session::send_blob)
	const u32 id = mgr().session().send_blob(from, BlobWorldSave, std::move(blob));
	hp.loading = true;
	hp.sent_at = mgr().now_ms();
	net::host_drop(from); // anything queued before this snapshot is already inside it
	sync::host_begin_join(from);
	::Msg("[coop] world for '%s': %u + %u bytes, blob %u", p->nick.c_str(), (u32)engine_part.size(),
	    (u32)script_part.size(), id);

	FS.file_delete(scop);
	FS.file_delete(scoc);
	return true;
}

void host_on_join_request(PeerId from, Reader&)
{
	const PeerInfo* p = mgr().session().peer(from);
	if (!p)
		return;
	// R1 I4, R2 I4: every join costs a full world save on this thread. (A JoinRequest made in a world that is gone
	// never gets here: the session drops it by the world epoch, R2 I1.)
	switch (g.gate.request(from, mgr().now_ms()))
	{
	case JoinGate::Verdict::Admit:
		if (!host_send_world(from))
			g.gate.forget(from);
		return;
	case JoinGate::Verdict::Later:
		::Msg("[coop] '%s' asks for the world again within %u s of the last time: it is sent when that is over",
		    p->nick.c_str(), JoinGate::kIntervalMs / 1000);
		return;
	case JoinGate::Verdict::Refused:
		if (++g.joins_refused <= kRefusalsLogged)
			::Msg("! [coop] '%s' asks for the world while his join is under way: ignored", p->nick.c_str());
		return;
	}
}

// Host: joins that waited for their turn (JoinGate) and whose time has come.
void host_due_joins()
{
	for (PeerId id : g.gate.due(mgr().now_ms()))
		if (!host_send_world(id))
			g.gate.forget(id);
}

// --- host: WorldLoaded ---
void host_on_world_loaded(PeerId from, Reader&)
{
	auto it = g.peers.find(from);
	// R2 I4: only the end of a join in flight. A repeated, unasked-for or old world's WorldLoaded would run the
	// catch-up again (or bring an id of the old world online).
	if (!g.gate.loaded(from) || it == g.peers.end() || !it->second.loading || it->second.proxy == kNoId)
	{
		if (++g.loaded_ignored <= kRefusalsLogged)
			::Msg("! [coop] #%u says it loaded the world, but no world is being sent to it: ignored", from);
		return;
	}
	it->second.loading = false;
	const PeerInfo* p = mgr().session().peer(from);
	::Msg("[coop] '%s' is in the world (load took %.1f s)", p ? p->nick.c_str() : "?",
	    float(mgr().now_ms() - it->second.sent_at) / 1000.f);
	bring_online(it->second.proxy); // W9: his character was kept out of the world while he was away or loading
	sync::host_end_join(from);
	mgr().session().send(from, Lane::Reliable, Msg::JoinComplete, Writer());
	players::equip_resend(); // the newcomer needs everybody's look
	objects::host_on_peer_ready(from); // zone phases, door/physics poses: not all of it is in the save
}

// --- client ---
void client_on_join_complete(PeerId, Reader&)
{
	::Msg("[coop] join complete, live sync on");
	if (Device.Paused()) // a pause from before the session; in a session nothing can pause (G18)
		Device.Pause(FALSE, TRUE, TRUE, "coop_live");
	players::equip_resend();
}

void client_on_session_reset(PeerId, Reader& r)
{
	const std::string why = r.str();
	const u32 epoch = r.u32();
	if (!r.ok())
	{
		::Msg("! [coop] malformed SessionReset from the host");
		mgr().stop("bad data from the host");
		return;
	}
	::Msg("[coop] host replaced the world (%s, epoch %u), rejoining", why.c_str(), epoch);
	g.my_proxy = kNoId;
	g.load_issued = false;
	g.swap_done = false;
	sync::reset();
	g.receive_started = mgr().now_ms(); // the join time limit starts again
	mgr().set_phase(Phase::ReceivingWorld);
	// R2 I1: the session tells the host we are in the new world before our JoinRequest; a save of the old world
	// still on its way is dropped when it completes.
	mgr().session().set_world_epoch(epoch);
	mgr().session().send(kHostPeer, Lane::Reliable, Msg::JoinRequest, Writer());
}

void client_load_world(std::vector<u8>& data)
{
	Reader r(data.data(), data.size());
	const u16 proxy = r.u16();
	const u32 online_count = r.u32();
	xr_vector<u16> online;
	online.reserve(_min<u32>(online_count, 65536));
	for (u32 i = 0; i < online_count && r.ok(); ++i)
		online.push_back(r.u16());
	const u32 engine_size = r.u32();
	const u8* engine_part = r.cursor();
	r.skip(engine_size);
	const u32 script_size = r.u32();
	const u8* script_part = r.cursor();
	r.skip(script_size);
	if (!r.ok() || !engine_size)
	{
		::Msg("! [coop] world blob is malformed");
		mgr().stop("bad world data");
		return;
	}

	string_path scop, scoc;
	save_path(scop, kClientSaveName, ".scop");
	save_path(scoc, kClientSaveName, ".scoc");
	if (!write_file(scop, engine_part, engine_size))
	{
		::Msg("! [coop] cannot write %s", scop);
		mgr().stop("cannot write the world save");
		return;
	}
	if (script_size)
		write_file(scoc, script_part, script_size);
	else
		FS.file_delete(scoc);

	g.my_proxy = proxy;
	entities::client_set_initial_online(online); // needs my_proxy for the id mapping
	g.swap_done = false;
	g.load_issued = true;
	mgr().set_phase(Phase::LoadingWorld);
	::Msg("[coop] world received (%u + %u bytes), my character is proxy %u in host numbering, loading", engine_size,
	    script_size, proxy);

	on_blocking_work(); // W9: the load below does not pump the network; the engine hooks re-arm it per stage
	if (g_pGameLevel)
		Console->Execute("disconnect");
	// As the stock new game / load do: a pause left by the menu would outlive the load ("Paused" after join)
	if (Device.Paused())
		Device.Pause(FALSE, TRUE, TRUE, "coop_join_load");
	string256 cmd;
	xr_sprintf(cmd, "start server(%s/single/alife/load) client(localhost)", kClientSaveName);
	Console->Execute(cmd);
	Console->Execute("main_menu off");
}

template <typename T>
void swap_values(T& a, T& b)
{
	T t = a;
	a = b;
	b = t;
}

// R2 I3, host: a level changer of the level the host is on, from its own ALife, nullptr if `id` is none (or one the
// host's scripts have switched off). The level change is built from its data, never from a client's bytes.
CSE_ALifeLevelChanger* level_changer_here(u16 id)
{
	if (id == kNoId || !g_pGameLevel || !ai().get_alife())
		return nullptr;
	CSE_ALifeLevelChanger* lc = smart_cast<CSE_ALifeLevelChanger*>(ai().alife().objects().object(id, true));
	if (!lc)
		return nullptr;
	const CGameGraph& gg = ai().game_graph();
	if (!gg.valid_vertex_id(lc->m_tGraphID) || !gg.valid_vertex_id(lc->m_tNextGraphID) ||
	    gg.vertex(lc->m_tGraphID)->level_id() != ai().alife().graph().level().level_id())
		return nullptr;
	CObject* object = Level().Objects.net_Find(id);
	const CLevelChanger* changer = object ? object->cast_level_changer() : nullptr;
	if (changer && !changer->IsLevelChangerEnabled())
		return nullptr;
	return lc;
}

// R2 I3, client: the level changer of this level whose destination our engine wrote into a level change packet
// (CLevelChanger and its dialog write the changer's own m_tNext* values), kNoId if none (jump_to_level and the like).
u16 level_changer_of(NET_Packet& P)
{
	constexpr u32 kBody = sizeof(GameGraph::_GRAPH_ID) + sizeof(u32) + 2 * sizeof(Fvector);
	if (!ai().get_alife() || P.B.count < sizeof(u16) + kBody)
		return kNoId;
	const u32 read_at = P.r_pos;
	P.r_pos = sizeof(u16); // after the message type
	GameGraph::_GRAPH_ID graph;
	u32 node;
	Fvector position, angles;
	P.r(&graph, sizeof(graph));
	P.r(&node, sizeof(node));
	P.r_vec3(position);
	P.r_vec3(angles);
	P.r_pos = read_at;
	const CGameGraph& gg = ai().game_graph();
	const GameGraph::_LEVEL_ID here = ai().alife().graph().level().level_id();
	for (const auto& it : ai().alife().objects().objects())
	{
		CSE_ALifeLevelChanger* lc = smart_cast<CSE_ALifeLevelChanger*>(it.second);
		if (lc && lc->m_tNextGraphID == graph && lc->m_dwNextNodeID == node && lc->m_tNextPosition.similar(position) &&
		    lc->m_tAngles.similar(angles) && gg.valid_vertex_id(lc->m_tGraphID) &&
		    gg.vertex(lc->m_tGraphID)->level_id() == here)
			return lc->ID;
	}
	return kNoId;
}

} // namespace

void host_on_transition_vote(PeerId from, Reader& r)
{
	if (!mgr().is_host())
		return;
	const u16 changer = r.u16();
	if (!r.ok())
		return;
	const PeerInfo* p = mgr().session().peer(from);
	const char* nick = p ? p->nick.c_str() : "?";
	// R2 I3: the client names a level exit; the host goes only through a level changer of its own level.
	if (!level_changer_here(changer))
	{
		if (++g.votes_refused <= kRefusalsLogged)
			::Msg("! [coop] '%s' asks to leave through object %u, which is no level exit here: ignored", nick, changer);
		return;
	}
	const u64 now = mgr().now_ms();
	const bool repeat = g.transition_from == from && g.pending_changer == changer && now - g.transition_at < 3000;
	g.pending_changer = changer;
	g.transition_from = from;
	g.transition_at = now;
	if (repeat)
		return; // the same player at the same exit again: Lua and the chat know already
	::Msg("[coop] '%s' stands at a level exit (%u) and wants the group to move. Host: coop_accept_transition to go",
	      nick, changer);
	script::on_transition_vote(nick); // the host's Lua (clients get the chat line below)
	Writer w(128);
	string256 text;
	xr_sprintf(text, "%s wants to move the group to the next level", p ? p->nick.c_str() : "a player");
	w.str(text);
	net::host_broadcast(Msg::Chat, w);
}

void install(Session& s)
{
	s.limit_rate(Msg::StateDumpRequest, kStateDumpBurst, kStateDumpRefillMs); // R2 I4 (host only, per client)
	s.on(Msg::TransitionVote, &host_on_transition_vote);
	s.on(Msg::JoinRequest, &host_on_join_request);
	s.on(Msg::WorldLoaded, &host_on_world_loaded);
	s.on(Msg::JoinComplete, &client_on_join_complete);
	s.on(Msg::SessionReset, &client_on_session_reset);
}

bool host_can_start()
{
	if (!g_pGameLevel || !ai().get_alife() || !ai().alife().graph().actor())
	{
		::Msg("! [coop] start or load a game first, then coop_host");
		return false;
	}
	if (ai().alife().graph().actor()->get_health() <= 0.f)
	{
		::Msg("! [coop] the actor is dead");
		return false;
	}
	return true;
}

void on_host_started()
{
	g = State();
	g.world_seen = g_world_generation;
	mgr().session().set_world_epoch(g.epoch); // R2 I1: clients learn it from Welcome
	world::host_scan_proxies();
}

u32 host_world_epoch() { return g.epoch; }

void on_world_created()
{
	++g_world_generation;
	// R2 I1: the host's world epoch moves on right here, before the first frame of the new world pumps the session
	// (coop::on_frame handles messages before join::frame sees the new world): what clients made in the old world
	// is dropped from now on, and nothing of the new world goes to a client before its SessionReset.
	if (mgr().is_host())
	{
		++g.epoch;
		mgr().session().set_world_epoch(g.epoch);
	}
}

u32 world_generation() { return g_world_generation; }

bool accept_transition()
{
	if (!mgr().is_host() || g.pending_changer == kNoId || !g_pGameLevel)
	{
		::Msg("! [coop] no pending transition");
		return false;
	}
	const u16 id = g.pending_changer;
	g.pending_changer = kNoId;
	const CSE_ALifeLevelChanger* lc = level_changer_here(id);
	if (!lc)
	{
		::Msg("! [coop] the level exit %u asked for is not usable any more", id);
		return false;
	}
	// R2 I3: the level change the stock level changer sends, made from the host's own entity.
	NET_Packet P;
	P.w_begin(M_CHANGE_LEVEL);
	P.w(&lc->m_tNextGraphID, sizeof(lc->m_tNextGraphID));
	P.w(&lc->m_dwNextNodeID, sizeof(lc->m_dwNextNodeID));
	P.w_vec3(lc->m_tNextPosition);
	P.w_vec3(lc->m_tAngles);
	::Msg("[coop] moving the group to the next level (level exit %u)", id);
	Level().Send(P, net_flags(TRUE)); // the host's own level change; clients follow via SessionReset
	return true;
}

void client_request_level_change(NET_Packet& P)
{
	if (!mgr().is_live_client())
		return;
	// R2 I3: the host builds the level change itself; it only learns which of its level changers we stand in.
	const u16 changer = level_changer_of(P);
	if (changer == kNoId)
	{
		::Msg("! [coop] level exit: no level changer of this level leads there, the host is not asked");
		return;
	}
	Writer w(4);
	w.u16(to_host(changer));
	net::client_send(Msg::TransitionVote, w);
	::Msg("[coop] level exit %u: asked the host to move the group", changer);
}

void on_peer_joined(const PeerInfo&) {}

void on_peer_left(PeerId id)
{
	auto it = g.peers.find(id);
	if (it == g.peers.end())
		return;
	const u16 proxy = it->second.proxy;
	world::host_park_proxy(proxy);
	g.peers.erase(it);
	g.gate.forget(id);
	take_offline(proxy); // W9: gone from the session = gone from the world (stays in the save)
}

bool host_player_in_world(u16 proxy_id)
{
	// A world loaded since the last frame is not served yet (host_watch_world): its ids are not the peers' proxies.
	if (proxy_id == kNoId || g.world_seen != g_world_generation)
		return false;
	for (const auto& it : g.peers)
		if (it.second.proxy == proxy_id)
			return !it.second.loading;
	return false;
}

u16 proxy_of_peer(PeerId id)
{
	auto it = g.peers.find(id);
	return it == g.peers.end() ? kNoId : it->second.proxy;
}

PeerId peer_of_proxy(u16 proxy_id)
{
	for (const auto& it : g.peers)
		if (it.second.proxy == proxy_id)
			return it.first;
	return kInvalidPeer;
}

bool is_proxy(u16 id)
{
	return world::is_player_proxy(id);
}

void on_welcome(const WelcomeMsg&)
{
	g.my_proxy = kNoId;
	g.load_issued = false;
	g.receive_started = mgr().now_ms();
	mgr().set_phase(Phase::ReceivingWorld);
	mgr().session().send(kHostPeer, Lane::Reliable, Msg::JoinRequest, Writer());
}

u16 my_proxy() { return g.my_proxy; }

bool client_loading_world() { return mgr().phase() == Phase::LoadingWorld; }

void on_session_lost(const char*)
{
	// W9, host: the session is over, nobody else is in the world. Clients take the avatars out through their own
	// switch vote (coop::switch_vote: no session = no player in the world).
	xr_vector<u16> proxies;
	for (const auto& it : g.peers)
		proxies.push_back(it.second.proxy);
	g = State();
	for (u16 proxy : proxies)
		take_offline(proxy);
}

void on_blob_progress(PeerId, u32, u32 kind, u64 done, u64 total)
{
	if (kind != BlobWorldSave)
		return;
	const u64 now = mgr().now_ms();
	if (now - g.last_progress_log < 500 && done != total)
		return;
	g.last_progress_log = now;
	::Msg("[coop] downloading world: %llu / %llu KB", done / 1024, total / 1024);
}

void on_blob_complete(PeerId from, u32, u32 kind, std::vector<u8>& data)
{
	if (kind != BlobWorldSave || from != kHostPeer || !mgr().is_client())
		return;
	// R2 I1: only the world we asked for and are waiting for (the session already dropped saves of another epoch).
	if (mgr().phase() != Phase::ReceivingWorld || g.load_issued)
	{
		::Msg("! [coop] a world save arrived while none was awaited (%s): ignored", phase_name(mgr().phase()));
		return;
	}
	client_load_world(data);
}

// Host: a load or a level change replaced the world under the session. Everybody rejoins
// from a fresh snapshot of the new world; requests made in the old one are dropped (epoch).
void host_watch_world()
{
	// A generation counter, not the simulator pointer: a same-level quick load deletes and
	// recreates the simulator in one call, usually at the same address.
	if (g.world_seen != g_world_generation)
	{
		g.world_seen = g_world_generation;
		objects::reset(); // per-world state of physics/zones belongs to the old world
		// The epoch moved on in on_world_created (R2 I1). Joins into the old world are void, everybody rejoins
		// after SessionReset; a level exit asked for in it means nothing here.
		g.gate.world_replaced();
		g.pending_changer = kNoId;
		g.reset_pending = true;
		for (auto& it : g.peers)
		{
			it.second.loading = true;
			it.second.sent_at = 0; // review R2 C1: the join timer restarts at SessionReset, not at the first join
			it.second.proxy = kNoId; // an id of the old world: set again by their JoinRequest
			sync::host_begin_join(it.first); // queue/drop world traffic until they rejoin
		}
	}
	if (!g.reset_pending || !g_pGameLevel || !g_actor || Device.dwPrecacheFrame || !ai().get_alife())
		return;
	g.reset_pending = false;
	world::host_scan_proxies();
	Writer w(64);
	w.str("the host loaded a save or changed level");
	w.u32(g.epoch); // R2 I1: the client's session tells it back before its JoinRequest
	const u64 now = mgr().now_ms();
	for (const PeerInfo& p : mgr().session().peers())
	{
		mgr().session().send(p.id, Lane::Reliable, Msg::SessionReset, w);
		auto it = g.peers.find(p.id);
		if (it != g.peers.end())
			it->second.sent_at = now; // the rejoin into the new world is what the timeout measures
	}
	::Msg("[coop] world replaced (epoch %u): %u player(s) rejoin", g.epoch, (u32)mgr().session().peers().size());
}

constexpr u64 kJoinTimeoutMs = 5 * 60 * 1000; // R-2.3: a join is limited in time

// Host: a client that never finishes loading the world is dropped with a clear reason.
void host_watch_joins()
{
	const u64 now = mgr().now_ms();
	xr_vector<PeerId> stuck;
	for (const auto& it : g.peers)
	{
		if (!it.second.loading)
			continue;
		if (net::host_queue_overflow(it.first) || (it.second.sent_at && now - it.second.sent_at > kJoinTimeoutMs))
			stuck.push_back(it.first);
	}
	for (PeerId id : stuck)
	{
		::Msg("! [coop] #%u did not finish loading the world in time, dropping it", id);
		mgr().session().kick(id, RejectReason::Timeout, "loading the host's world took too long, try to join again");
	}
}

void frame()
{
	if (mgr().is_host())
	{
		host_watch_world();
		host_watch_joins();
		host_due_joins();
		return;
	}
	// Client: downloading or loading the world must end in time (R-2.3).
	if ((mgr().phase() == Phase::ReceivingWorld || mgr().phase() == Phase::LoadingWorld) && g.receive_started &&
	    mgr().now_ms() - g.receive_started > kJoinTimeoutMs)
	{
		::Msg("! [coop] joining the host's world took too long");
		mgr().stop("joining the host's world took too long");
		return;
	}
	if (mgr().phase() != Phase::LoadingWorld || !g.swap_done)
		return;
	// Level is up and the local actor exists: tell the host.
	if (!g_pGameLevel || !Level().game || !g_actor || Device.dwPrecacheFrame)
		return;
	mgr().set_phase(Phase::Live);
	mgr().session().send(kHostPeer, Lane::Reliable, Msg::WorldLoaded, Writer());
	sync::on_client_live();
}

void print_status()
{
	if (mgr().is_host())
	{
		::Msg("[coop]   world epoch %u; refused: %u join(s), %u WorldLoaded, %u level exit(s)", g.epoch,
		    g.joins_refused, g.loaded_ignored, g.votes_refused);
		for (const auto& it : g.peers)
			::Msg("[coop]   peer #%u proxy %u%s%s", it.first, it.second.proxy, it.second.loading ? " (loading)" : "",
			    g.gate.waiting(it.first) ? " (join waits for its turn)" : "");
	}
	else if (mgr().is_client())
		::Msg("[coop]   my proxy (host numbering) %u, swap %s", g.my_proxy, g.swap_done ? "done" : "pending");
}

void on_alife_objects_loaded(CALifeObjectRegistry& objects)
{
	if (!client_loading_world() || g.my_proxy == kNoId)
		return;

	CSE_ALifeCreatureActor* actor = smart_cast<CSE_ALifeCreatureActor*>(objects.object(0, true));
	CSE_ALifeHumanStalker* proxy = smart_cast<CSE_ALifeHumanStalker*>(objects.object(g.my_proxy, true));
	if (!actor || !proxy)
	{
		::Msg("! [coop] role swap impossible: actor %p, proxy %u %p", actor, g.my_proxy, proxy);
		mgr().stop("the world from the host has no character for us");
		return;
	}

	// Where we stand and what we look like. CActor::net_Spawn takes its view (r_model_yaw, camera)
	// from o_torso, not from o_Angle.
	swap_values(actor->o_Position, proxy->o_Position);
	swap_values(actor->o_Angle, proxy->o_Angle);
	swap_values(actor->o_model, proxy->o_model);
	swap_values(actor->o_torso, proxy->o_torso);
	swap_values(actor->m_tNodeID, proxy->m_tNodeID);
	swap_values(actor->m_tGraphID, proxy->m_tGraphID);
	{
		const float h = actor->get_health();
		actor->set_health(proxy->get_health());
		proxy->set_health(h);
	}
	// The host may sit in a vehicle; our character does not (net_Spawn would seat us in it).
	actor->m_holderID = u16(-1);

	// Who we are: money, community, rank, reputation, name.
	swap_values(actor->m_dwMoney, proxy->m_dwMoney);
	swap_values(actor->m_community_index, proxy->m_community_index);
	swap_values(actor->CSE_ALifeTraderAbstract::m_rank, proxy->CSE_ALifeTraderAbstract::m_rank);
	swap_values(actor->m_reputation, proxy->m_reputation);
	swap_values(actor->m_character_name, proxy->m_character_name);
	// The proxy is an NPC of community X; a player character uses actor_X (game_relations.ltx has
	// both, relations and scripts tell them apart).
	if (actor->m_community_index != NO_COMMUNITY_INDEX)
	{
		const shared_str npc = CHARACTER_COMMUNITY::IndexToId(actor->m_community_index, shared_str(), true);
		if (npc.size() && strncmp(npc.c_str(), "actor", 5))
		{
			string128 player;
			xr_sprintf(player, "actor_%s", npc.c_str());
			const CHARACTER_COMMUNITY_INDEX index =
			    CHARACTER_COMMUNITY::IdToIndex(shared_str(player), NO_COMMUNITY_INDEX, true);
			if (index != NO_COMMUNITY_INDEX)
				actor->m_community_index = index;
		}
	}

	// What we carry: items keep their ids, only parents swap. Children lists are rebuilt from
	// ID_Parent when ALife registers the loaded objects right after this hook.
	u32 to_proxy = 0, to_actor = 0;
	for (auto& it : objects.objects())
	{
		CSE_ALifeDynamicObject* e = it.second;
		if (!e)
			continue;
		if (e->ID_Parent == 0)
		{
			e->ID_Parent = g.my_proxy;
			++to_proxy;
		}
		else if (e->ID_Parent == g.my_proxy)
		{
			e->ID_Parent = 0;
			++to_actor;
		}
	}
	actor->children.clear();
	proxy->children.clear();

	g.swap_done = true;
	::Msg("[coop] role swap done: actor <-> proxy %u (%u items to me, %u to the host avatar)", g.my_proxy, to_actor,
	    to_proxy);
	world::client_mark_host_avatar(g.my_proxy);
}
} // namespace join
} // namespace coop
