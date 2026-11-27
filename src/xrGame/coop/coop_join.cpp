#include "stdafx.h"
#include "pch_script.h"
#include "coop_join_impl.h"
#include "coop_death.h"
#include "coop_debug.h"
#include "coop_manager.h"
#include "coop_sync.h"
#include "coop_hud_text.h"
#include "coop_world.h"
#include "coop_entities.h"
#include "coop_players.h"
#include "coop_objects.h"
#include "coop_net.h"
#include "coop_survival.h"
#include "coop_sync_api.h"
#include "coop_vehicles.h"
#include "xrCoop/coop_save_policy.h"
#include "xrCoop/coop_death_rules.h"
#include "xrCoop/coop_mode.h"

#include "../../xrEngine/XR_IOConsole.h"
#include "ai_space.h"
#include "Level.h"
#include "Actor.h"
#include "alife_simulator.h"
#include "alife_object_registry.h"
#include "UIGameCustom.h"
#include "xrCoop/coop_news_log.h"
#include "alife_graph_registry.h"
#include "xrServer_Objects_ALife_Monsters.h"
#include "../xrServerEntities/xrMessages.h"

#include <algorithm>
#include <atomic>
#include <cstdio>

extern string_path g_last_saved_game;

int g_coop_max_new_characters = int(coop::limits::kNewCharactersDefault);
int g_coop_diag_stacks = 0;
int g_coop_diag_fail_swap = 0;

namespace coop
{
namespace diag
{
namespace
{
std::atomic<u32> g_destroyed_object_stacks{0}; // armed and not printed yet
} // namespace

void arm_destroyed_object_stacks()
{
	if (!g_coop_diag_stacks)
		return; // a stack walk stalls this thread for minutes on its first use (console coop_diag_stacks 1)
	g_destroyed_object_stacks.store(kDestroyedObjectStacks, std::memory_order_relaxed);
	::Msg("[coop] diagnostics: the next %u accesses to a destroyed object print a C++ stack (a player left)",
	    kDestroyedObjectStacks);
}

bool take_destroyed_object_stack()
{
	u32 left = g_destroyed_object_stacks.load(std::memory_order_relaxed);
	while (left != 0 && !g_destroyed_object_stacks.compare_exchange_weak(left, left - 1, std::memory_order_relaxed))
	{
	}
	return left != 0;
}
} // namespace diag

namespace join
{
namespace
{
constexpr LPCSTR kClientSaveName = save_policy::kClientStem;
} // namespace

JoinState g_join;

u32 g_world_generation = 0; // CALifeSimulator constructions in this process (outlives the session's state)

u32 g_mirror_generation = 0; // g_world_generation of the world loaded from a host's save (0: none), outlives the session's state

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

namespace
{
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
} // namespace

void save_path(string_path& out, LPCSTR name, LPCSTR ext)
{
	string_path file;
	xr_sprintf(file, "%s%s", name, ext);
	FS.update_path(out, "$game_saves$", file);
}

// Every client learns who plays each character in the world (Msg::AvatarList): the host's own actor and
// the character of every player who has loaded the world. Sent whole whenever that changes (a player is in the
// world, a player left); a client that is still loading gets it queued (net::host_broadcast) or ignores it, and the
// table of its own WorldLoaded follows.
void host_send_owners()
{
	if (!mode().decides_world())
		return; // a map server takes the table, it does not make it
	AvatarListMsg m;
	if (mode().actor_is_player()) // a ghost actor (the world server's) is nobody's
	{
		AvatarOwner host;
		host.avatar = 0;
		host.peer = kHostPeer;
		m.owners.push_back(host);
	}
	const bool served = g_join.world_seen == g_world_generation; // else the proxies are ids of the old world
	for (const auto& it : g_join.peers)
	{
		const u16 proxy = it.second.proxy;
		if (!served || it.second.loading || proxy == kNoId || proxy == 0)
			continue;
		bool listed = false;
		for (const AvatarOwner& o : m.owners)
			listed = listed || o.avatar == proxy;
		if (listed || it.first == kHostPeer || it.first >= kMaxPeerSlots || m.owners.size() >= kMaxPeerSlots)
			continue; // cannot happen (one character per player key, slots 1..16): clients would refuse the whole table
		AvatarOwner o;
		o.avatar = proxy;
		o.peer = it.first;
		m.owners.push_back(o);
	}
	Writer w(1 + m.owners.size() * 6);
	m.write(w);
	net::host_broadcast(Msg::AvatarList, w);
}

namespace
{

// The world for a player whose join the gate admitted: his character (found by key) and a fresh save of the world, or
// the offer of a new character, whose choice brings the world (host_on_char_choice). false if neither
// could be sent (the player is gone or was kicked with the reason).
// The operator (host_rights) without a character, in a world staged with a creator, gets his character made
// from the record instead of the offer (host_make_creator); everybody else, and he in every other case, gets the offer.
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
	if (!host_key_digest(from, *p, key_digest))
		return false;
	CSE_ALifeHumanStalker* proxy = find_proxy_by_key(key_digest);
	if (proxy)
	{
		::Msg("[coop] '%s' returns to character proxy id %u", p->nick.c_str(), proxy->ID);
		g_char.had_character.insert(key_digest); // his next new character costs nothing
		// His character stays where it stands, unless the group is one level or somebody else leads that one
		if (!levels::host_player_loads(from, levels::level_of_vertex(proxy->m_tGraphID)) && !host_fall_back_to_level(from, *proxy, p->nick.c_str()))
			return false;
		return host_send_save(from, *p, *proxy);
	}
	if (!host_admits_new_key(from, *p, key_digest))
		return false;
	if (p->host_rights && host_make_creator(from, *p))
		return true; // made (or, if that failed, the offer instead): host_make_new_character sent the world or the offer
	if (host_offer_character(from, *p, nullptr))
		return true; // the join stays in flight while he chooses
	if (!mode().leads_level())
	{
		mgr().session().kick(from, RejectReason::HostNotReady, "the server has no place for a new character, see its log");
		return false; // "next to the host" is the service level
	}
	proxy = create_proxy(*p, key_digest, StartPoint());
	if (!proxy)
	{
		mgr().session().kick(from, RejectReason::HostNotReady, "host could not create a character for you");
		return false;
	}
	host_count_new_character(key_digest);
	return host_send_save(from, *p, *proxy);
}

} // namespace

// Writes a fresh save of the world and reads it back, its files gone. False: it failed.
bool world_save(const char* name, std::vector<u8>& engine_part, std::vector<u8>& script_part)
{
	// Writing and reading back the save blocks this frame (seconds on a big world); the others must not time
	// us out meanwhile (the save hook arms it too; reading the files back is ours).
	on_blocking_work();
	{
		// save() records every name it writes as the last save; this file is deleted below, and
		// "load last save" must keep pointing at the host's real one.
		string_path last_save;
		xr_strcpy(last_save, g_last_saved_game);
		const syncapi::SavingForJoin join_save; // this save goes to him, coop.sync stays out (save_state)
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
	const bool saved = read_file(scop, engine_part) && !engine_part.empty();
	if (saved)
		read_file(scoc, script_part); // absent if Anomaly's script storage is disabled
	// The hold is not let go here. Manager::frame decides on the next frame (link_hold::keep_held): a join that
	// comes while the host's own level is still coming up must not cut the hold of that load short.
	if (!saved)
		::Msg("! [coop] world save failed (%s)", scop);
	FS.file_delete(scop);
	FS.file_delete(scoc);
	return saved;
}

namespace
{

bool host_precaching(); // below
} // namespace

// The save of the world for player `from`, whose character is `proxy`, streamed to him (the join is in flight until
// his WorldLoaded). false if it could not be sent (he was kicked with the reason).
bool host_send_save(PeerId from, const PeerInfo& p_ref, CSE_ALifeHumanStalker& proxy_ref)
{
	const PeerInfo* p = &p_ref;
	CSE_ALifeHumanStalker* proxy = &proxy_ref;
	HostPeer& hp = g_join.peers[from];
	hp.proxy = proxy->ID;
	// His seat, a new character too (a decider that leads no level has none to put him on)
	if (!levels::host_player_loads(from, levels::level_of_vertex(proxy->m_tGraphID)) && !mode().leads_level())
		return host_fall_back_to_level(from, *proxy, p->nick.c_str());
	set_player_name(*proxy, p->nick); // a returning character of an older save still has the stock name; nicks change
	hp.loading = true; // not in the world before WorldLoaded: the save he gets has his character offline
	world::host_mark_proxy(proxy->ID, from);

	// Fresh world save: engine part (.scop) and Anomaly's script part (.scoc).
	string64 name;
	xr_sprintf(name, "coop_join_%u", from);
	std::vector<u8> engine_part, script_part;
	if (!world_save(name, engine_part, script_part))
	{
		mgr().session().kick(from, RejectReason::HostNotReady, "host could not save the world");
		return false;
	}

	xr_vector<u16> online;
	if (mode().where(from) != Where::Away) // a player on another process's level gets the online set from it, when he is in the world
		entities::host_collect_online(online);
	levels::host_send_map_now();
	// The map his character stands on is led by another process (a map server of the world server): the mode tells it where his
	// character stands now, if it is up (its copy is as old as its world)
	const bool elsewhere = mode().join_save(from, *proxy);
	// That process is not up yet: he stands on his loading screen until it is, and the process that starts for him
	// loads the world he loads (held_save)
	const u16 level = levels::level_of_vertex(proxy->m_tGraphID);
	if (elsewhere && !mode().level_ready(level))
	{
		g_load.map_save.world = g_world_generation;
		g_load.map_save.at = mgr().now_ms();
		g_load.map_save.engine = engine_part;
		g_load.map_save.script = script_part;
		host_hold_for_server(from, level);
	}

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
	syncapi::host_send_full(from); // his coop.sync copy, before the world (Reliable lane: ahead of his changes)
	// A world save still queued or streaming to this player is replaced by this one (Session::send_blob)
	const u32 id = mgr().session().send_blob(from, BlobWorldSave, std::move(blob));
	hp.loading = true;
	hp.sent_at = mgr().now_ms();
	net::host_drop(from); // anything queued before this snapshot is already inside it
	sync::host_begin_join(from);
	::Msg("[coop] world for '%s': %u + %u bytes, blob %u", p->nick.c_str(), (u32)engine_part.size(),
	    (u32)script_part.size(), id);
	return true;
}

namespace
{
// The host's own level is still coming up (precache frames, the first update of every script): a world save now
// would stall those frames further, and they are held by the link hold of that load (Manager::frame). For
// link_hold::PrecacheWait::kLimitMs at most: precache counts down only in the focused window and starts again on a
// device reset: an unfocused host would serve no join at all.
bool host_precaching()
{
	const bool precaching = g_pGameLevel && Device.dwPrecacheFrame != 0;
	const bool waits = g_join.precache_clock.waiting(precaching, mgr().now_ms());
	if (g_join.precache_clock.take_gave_up())
		::Msg("[coop] the host's level is still precaching after %u s (window without focus?): joins go on",
		    u32(link_hold::PrecacheWait::kLimitMs / 1000));
	return waits;
}

// The gate for a JoinRequest of `from` that arrives now (or waited for the host's load, host_due_joins).
void host_join_request(PeerId from, const PeerInfo& peer)
{
	const PeerInfo* p = &peer;
	// A join during the host's own load waits like a Later until the host plays normally.
	// So does one that comes while the host hands a transition's saves to the network (the world is leaving: a
	// save of it now would be of the old level; he gets the SessionReset of the new one like everybody who is not a mover).
	if (g_load.load_pumping || host_precaching())
	{
		if (std::find(g_join.precache_wait.begin(), g_join.precache_wait.end(), from) == g_join.precache_wait.end())
			g_join.precache_wait.push_back(from);
		::Msg("[coop] '%s' asks for the world while the host's level is loading: it is sent when that is over",
		    p->nick.c_str());
		return;
	}
	// Every join costs a full world save on this thread. (A JoinRequest made in a world that is gone
	// never gets here: the session drops it by the world epoch.)
	switch (g_join.gate.request(from, mgr().now_ms()))
	{
	case JoinGate::Verdict::Admit:
		if (!host_send_world(from))
			g_join.gate.forget(from);
		return;
	case JoinGate::Verdict::Later:
		::Msg("[coop] '%s' asks for the world again within %u s of the last time: it is sent when that is over",
		    p->nick.c_str(), JoinGate::kIntervalMs / 1000);
		return;
	case JoinGate::Verdict::Refused:
		if (++g_join.joins_refused <= kRefusalsLogged)
			::Msg("! [coop] '%s' asks for the world while his join is under way: ignored", p->nick.c_str());
		return;
	}
}

} // namespace

void player_join_request(PeerId from, Reader&)
{
	if (!mode().decides_world())
		return; // the session drops it on a client already (msg_sender); handlers check it too
	if (mode().join_from_bridge(from, Msg::JoinRequest))
		return; // no player, a map server of this world: the server's join serves it (srv/coop_srv_join.cpp)
	// A mover of the simultaneous load whose save got lost asks for the world the usual way: he is no mover any more
	host_load_forget(from, "he asks for the world");
	if (const PeerInfo* p = mgr().session().peer(from))
		host_join_request(from, *p);
}

namespace
{

// Host: joins that waited for their turn (JoinGate) or for the host's own load, and whose time has come.
void host_due_joins()
{
	if (host_precaching())
		return; // the gate keeps its waiting ones until the host plays normally
	if (!g_join.precache_wait.empty())
	{
		const xr_vector<PeerId> waited = g_join.precache_wait;
		g_join.precache_wait.clear();
		for (PeerId id : waited)
			if (const PeerInfo* p = mgr().session().peer(id))
				host_join_request(id, *p);
	}
	for (PeerId id : g_join.gate.due(mgr().now_ms()))
		if (!host_send_world(id))
			g_join.gate.forget(id);
}

} // namespace

// --- host: WorldLoaded ---
void player_world_loaded(PeerId from, Reader&)
{
	if (!mode().decides_world())
		return; // as player_join_request
	if (mode().join_from_bridge(from, Msg::WorldLoaded))
		return; // a map server's world is up: the server's join ends that join (srv/coop_srv_join.cpp)
	auto it = g_join.peers.find(from);
	// Only the end of a join in flight. A repeated, unasked-for or old world's WorldLoaded would run the
	// catch-up again (or bring an id of the old world online).
	// Whether a world was sent to him is checked before the gate, since gate.loaded ends his join
	// in flight. A forged WorldLoaded while he chooses his character (no world sent yet) ended it, and his JoinRequest
	// was admitted again, with a new CharOffer, every 5 s.
	const bool world_sent = it != g_join.peers.end() && it->second.loading && it->second.proxy != kNoId &&
	    !g_char.choosers.in_choice(from);
	// A mover of the simultaneous load had no JoinRequest (the gate knows no join of his): his WorldLoaded comes
	// after LoadWait Enter (or after his screen let go alone), in the world served now (the session dropped one of another
	// epoch, and the world gate one made while the world was not up: WorldLoaded = Drop).
	const bool prepared = world_sent && it->second.prepared && g_join.world_seen == g_world_generation && !g_join.reset_pending;
	if (!world_sent || !(prepared || g_join.gate.loaded(from)))
	{
		if (++g_join.loaded_ignored <= kRefusalsLogged)
			::Msg("! [coop] #%u says it loaded the world, but no world is being sent to it: ignored", from);
		return;
	}
	it->second.loading = false;
	it->second.prepared = false;
	const PeerInfo* p = mgr().session().peer(from);
	if (prepared && g_load.load.is_mover(from) && !g_load.load.entered(from))
	{
		// his screen let go alone (no Enter reached him): he is in, the load does not wait for him any more
		::Msg("[coop] load: '%s' entered without the host's Enter (his screen let go alone)", p ? p->nick.c_str() : "?");
		g_load.load.drop(from);
	}
	::Msg("[coop] '%s' is in the world (load took %.1f s%s)", p ? p->nick.c_str() : "?",
	    float(mgr().now_ms() - it->second.sent_at) / 1000.f, prepared ? ", a simultaneous load" : "");
	levels::host_player_in_world(from);
	players::arrival_host_entered(from); // his first report kWindowMs from now goes into the log
	bring_online(it->second.proxy); // his character was kept out of the world while he was away or loading
	sync::host_end_join(from);
	mgr().session().send(from, Lane::Reliable, Msg::JoinComplete, Writer());
	if (prepared && mode().where(from) != Where::Away)
		entities::host_send_online_to(from); // his prepared save knew only the host's actor online
	host_send_owners(); // everybody learns whose character this is, the newcomer who plays the others
	players::equip_resend(); // the newcomer needs everybody's look
	if (mode().where(from) != Where::Away) // the map this process has loaded is his
		objects::host_on_peer_ready(from); // zone phases, door/physics poses: not all of it is in the save
}

namespace
{

// --- client ---
void client_on_join_complete(PeerId from, Reader&)
{
	if (from != kHostPeer || mode().decides_world())
		return; // only the host sends it (the session kicks a client that does, msg_sender)
	::Msg("[coop] join complete, live sync on");
	if (Device.Paused()) // a pause from before the session; in a session nothing can pause
		Device.Pause(FALSE, TRUE, TRUE, "coop_live");
	players::equip_resend();
}

void client_on_session_reset(PeerId from, Reader& r)
{
	// Only the host's: from a client it would wipe the host's sync state (avatars included), make the host a client in
	// ReceivingWorld with the client's epoch, and a 2-byte one would stop the session for everybody. The session kicks a
	// client that sends it (msg_sender); the handler checks it as well. A map server's SessionReset is the server's own
	// handler (srv/coop_srv_join.cpp join_install_map: its world is gone, its process ends): this is a player's.
	if (from != kHostPeer || mode().decides_world())
		return;
	const std::string why = r.str();
	const u32 epoch = r.u32();
	if (!r.ok())
	{
		::Msg("! [coop] malformed SessionReset from the host");
		mgr().stop("bad data from the host");
		return;
	}
	if (!strncmp(why.c_str(), "st_", 3))
		hud_text::say(why.c_str(), why.c_str()); // what happened to his map server
	::Msg("[coop] host replaced the world (%s, epoch %u), rejoining", why.c_str(), epoch);
	// The talk and trade windows of the world that goes close now, while its objects are whole and the session's state of
	// them (the talk lock, the open container) is here to be closed. Stock closes them only in CLevel::net_Stop of the load,
	// and a partner destroyed before that would be read freed.
	if (CUIGameCustom* ui = CurrentGameUI())
		ui->HideShownDialogs();
	// The epoch moves with every world the host makes (a level change, a loaded save). The same epoch: the
	// world goes on and only this player comes into it anew (his own exit in split worlds, his map server came up again):
	// his state goes with him, as in client_prepared. Another epoch: the world was replaced, maybe by a save the host
	// loaded (a rollback): what he had in the world that went is not his state in it, the host's record in that save is.
	// One reset keeps the epoch and still is no continuation: his character was retired and he
	// comes in as a new one (kRejoinNewCharacter): nothing of the old character goes into the new.
	if (epoch == mgr().session().world_epoch() && why != kRejoinNewCharacter)
		world_leaving(epoch);
	else
		players::condition_carry_drop();
	g_join.my_proxy = kNoId;
	g_join.load_issued = false;
	g_join.swap_done = false;
	transition_world_replaced(); // the host's board of the old world
	load_screen_off(); // a simultaneous load of ours is over, this join is the usual one
	// An offer of the old world is void; the window stays open until the host offers again (join_offer)
	g_char.char_step = CharState::CharStep::None;
	debug::keep_own_flags_through_next_reset(); // the session goes on: its debug too, until the join's DebugState
	sync::reset();
	g_join.receive_started = mgr().now_ms(); // the join time limit starts again
	mgr().set_phase(Phase::ReceivingWorld);
	// The session tells the host we are in the new world before our JoinRequest; a save of the old world
	// still on its way is dropped when it completes.
	mgr().session().set_world_epoch(epoch);
	mgr().session().send(kHostPeer, Lane::Reliable, Msg::JoinRequest, Writer());
}

// Who plays each player character, from the host (host numbering). With it every machine knows the
// owner of every avatar, not only of the host's: coop.players() avatar_id, coop.peer_of_avatar, map markers, colours,
// nick tags, giving items.
void client_on_avatar_list(PeerId from, Reader& r)
{
	if (from != kHostPeer || !mgr().is_live_client())
		return; // not in the world yet: the table of our own join comes after WorldLoaded
	AvatarListMsg m;
	if (!m.read(r))
	{
		if (++g_join.owners_bad <= kRefusalsLogged)
			::Msg("! [coop] malformed AvatarList from the host: ignored");
		return;
	}
	const PeerId self = mgr().self();
	xr_vector<AvatarOwner> owners;
	for (const AvatarOwner& o : m.owners)
	{
		const u16 local = to_local(o.avatar);
		if (local == 0 || o.peer == self)
			continue; // our own character is the local actor
		AvatarOwner mine = o;
		mine.avatar = local;
		owners.push_back(mine);
	}
	world::client_set_owners(owners);
}
} // namespace

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
	client_cache_path(scop, ".scop");
	client_cache_path(scoc, ".scoc");
	on_blocking_work();
	if (script_size && !write_file(scoc, script_part, script_size))
	{
		::Msg("! [coop] cannot write client script cache %s", scoc);
		mgr().stop("cannot write the world script cache");
		return;
	}
	if (!script_size && !DeleteFileA(scoc))
	{
		const DWORD error = GetLastError();
		if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND)
		{
			::Msg("! [coop] cannot remove stale script cache %s (%lu)", scoc, error);
			mgr().stop("cannot reset the world script cache");
			return;
		}
	}
	if (!write_file(scop, engine_part, engine_size))
	{
		::Msg("! [coop] cannot write %s", scop);
		mgr().stop("cannot write the world save");
		return;
	}

	g_join.my_proxy = proxy;
	entities::client_set_initial_online(online); // needs my_proxy for the id mapping
	g_join.swap_done = false;
	g_join.load_issued = true;
	mgr().set_phase(Phase::LoadingWorld);
	::Msg("[coop] world received (%u + %u bytes), my character is proxy %u in host numbering, loading", engine_size,
	    script_size, proxy);

	client_char_closed("world"); // the choice window (main menu) closes before the load
	on_blocking_work(); // the load below does not pump the network; the engine hooks re-arm it per stage
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

std::string nick_of_peer(PeerId id)
{
	for (const PeerInfo& p : mgr().session().players())
		if (p.id == id)
			return p.nick;
	string32 n;
	xr_sprintf(n, "#%u", id);
	return n;
}

// The character of player `id` in the world served now, nullptr if he is not in it. The host player's (peer 0, over
// the local link) is the actor of this process; a client's is his proxy once his join is done.
CObject* player_body_here(PeerId id)
{
	if (!g_pGameLevel || !ai().get_alife() || g_join.world_seen != g_world_generation)
		return nullptr;
	if (id == kHostPeer)
		return g_actor ? Actor() : nullptr;
	const u16 proxy = proxy_of_peer(id);
	if (sync::host_peer_joining(id) || !host_player_in_world(proxy))
		return nullptr;
	return Level().Objects.net_Find(proxy);
}

// The one handler table split in two (Manager::install_handlers), registrations only
void install_server(Session& s)
{
	for (const limits::RateRule& rule : limits::kClientRateRules)
		s.limit_rate(rule.type, rule.burst, rule.refill_ms); // per client: the table and its reasons in xrCoop/coop_limits.h
	s.on(Msg::TransitionVote, &host_on_transition_vote);
	s.on(Msg::JoinRequest, &player_join_request);
	s.on(Msg::CharChoice, &host_on_char_choice);
	s.on(Msg::WorldLoaded, &player_world_loaded);
	s.on(Msg::LoadReady, &host_on_load_ready);
}

void install_client(Endpoint& e)
{
	e.on(Msg::TransitionState, &client_on_transition_state);
	e.on(Msg::LevelMoveResult, &client_on_level_move_result);
	e.on(Msg::CharOffer, &client_on_char_offer);
	e.on(Msg::JoinComplete, &client_on_join_complete);
	e.on(Msg::SessionReset, &client_on_session_reset);
	e.on(Msg::AvatarList, &client_on_avatar_list);
	e.on(Msg::LoadWait, &client_on_load_wait);
}

// --- the Core's parts of the server's join (srv/coop_srv_join.cpp is made of them)

bool world_save_ready() { return world_up() && !g_load.load_pumping && !host_precaching() && ai().alife().graph().actor(); }

HeldSave held_save()
{
	HeldSave out;
	const LoadState::MapSave& s = g_load.map_save;
	if (s.world != g_world_generation || s.engine.empty() || mgr().now_ms() - s.at >= kMapSaveMaxAgeMs)
		return out;
	out.at = s.at;
	out.engine = &s.engine;
	out.script = &s.script;
	return out;
}

bool group_moves() { return g_load.load.active() || g_board.going; }

bool host_can_start()
{
	if (!world_up() || !ai().alife().graph().actor()) // world gate: a level half built is no game to host
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

void reset_state()
{
	g_join.reset();
	g_char.reset();
	g_board.reset();
	g_load.reset();
}

void on_host_started()
{
	reset_state();
	g_join.world_seen = g_world_generation;
	mgr().session().set_world_epoch(g_join.epoch); // clients learn it from Welcome
	world::host_scan_proxies();
}

u32 host_world_epoch() { return g_join.epoch; }

void on_world_created()
{
	++g_world_generation;
	// The host's world epoch moves on right here, before the first frame of the new world pumps the session
	// (coop::on_frame handles messages before join::frame sees the new world): what clients made in the old world
	// is dropped from now on, and nothing of the new world goes to a client before its SessionReset.
	if (mode().decides_world())
	{
		if (g_load.load_world_pending)
			g_load.load_world_pending = false; // the transition's world: its epoch was given at the go (host_prepare)
		else
		{
			++g_join.epoch;
			mgr().session().set_world_epoch(g_join.epoch);
		}
	}
}

u32 world_generation() { return g_world_generation; }

// The epoch does not move: client_on_session_reset takes the one it is in (Session::set_world_epoch is a
// no-op for it, no WorldEpoch goes out) and so does the host's session for his JoinRequest. The gate is
// the one of every join: his join of this world ended with his WorldLoaded, forget voids one still in flight, and a
// JoinRequest within 5 s of his last admission waits (Later) until due() admits it - so nothing has to wait here.
bool host_rejoin_peer(PeerId peer, const char* why)
{
	if (!mode().decides_world() || peer == kHostPeer || !mgr().session().peer(peer))
		return false;
	auto it = g_join.peers.find(peer);
	if (it == g_join.peers.end())
		return false; // no join of his reached this world: there is nothing to do again
	host_load_forget(peer, "he joins again");
	g_load.held.erase(peer); // this join has its own hold, if it needs one
	g_join.gate.forget(peer);
	g_join.precache_wait.erase(std::remove(g_join.precache_wait.begin(), g_join.precache_wait.end(), peer), g_join.precache_wait.end());
	g_char.choosers.forget(peer);
	const u16 proxy = it->second.proxy;
	it->second.proxy = kNoId; // set again by his JoinRequest (his character by key, or a new one)
	it->second.loading = true;
	it->second.sent_at = mgr().now_ms(); // the join timeout measures this rejoin
	if (proxy != kNoId)
		world::host_park_proxy(proxy); // nobody plays it now (the caller may release it right after)
	sync::host_begin_join(peer); // queue / drop world traffic to him until he is back
	Writer w(96);
	w.str(why && *why ? why : "the host sends you the world again");
	w.u32(g_join.epoch);
	mgr().session().send(peer, Lane::Reliable, Msg::SessionReset, w);
	host_send_owners(); // nobody plays that character any more
	const PeerInfo* p = mgr().session().peer(peer);
	::Msg("[coop] '%s' joins the world again (%s, epoch %u), his character %u is nobody's now", p ? p->nick.c_str() : "?",
	    why && *why ? why : "-", g_join.epoch, u32(proxy));
	return true;
}

void host_rejoin_level(u16 level, const char* why)
{
	for (const u32 peer : levels::host_players_of(level))
		host_rejoin_peer(PeerId(peer), why);
}

// A map server: its players. Its world is the host's save, its numbering the world server's: a player's character here is what it
// is there, the same code as a joiner's on the host's process from his save on.
void server_player_came(PeerId peer, u16 proxy)
{
	HostPeer& hp = g_join.peers[peer];
	hp.proxy = proxy;
	hp.loading = true;
	g_join.world_seen = g_world_generation;
	world::host_mark_proxy(proxy, peer);
	sync::host_begin_join(peer);
}

void server_player_in_world(PeerId peer)
{
	const auto it = g_join.peers.find(peer);
	if (it == g_join.peers.end())
		return;
	it->second.loading = false;
	g_join.world_seen = g_world_generation;
	world::host_mark_proxy(it->second.proxy, peer);
	bring_online(it->second.proxy);
	sync::host_end_join(peer);
	entities::host_send_online_to(peer);
	players::equip_resend();
	objects::host_on_peer_ready(peer);
}

std::vector<Member> host_members()
{
	std::vector<Member> out;
	if (!mode().decides_world() || !ai().get_alife())
		return out;
	for (const auto& it : ai().alife().objects().objects())
	{
		const CSE_ALifeDynamicObject* e = it.second;
		if (!e || !world::is_player_character(e) || !e->m_ini_string.size())
			continue;
		if (death::is_retired(e->m_ini_string.c_str()) || survival::out_of_run(e->ID))
			continue;
		Member m;
		m.id = e->ID;
		for (const PeerInfo& p : mgr().session().peers())
			if (proxy_of_peer(p.id) == e->ID)
				m.peer = p.id;
		m.in_world = m.peer != kInvalidPeer && !sync::host_peer_joining(m.peer) && host_player_in_world(e->ID);
		out.push_back(m);
	}
	return out;
}

void on_peer_left(PeerId id)
{
	// Before the lookup below: a player who got Later and left before his world was sent has
	// no entry in g_join.peers; his waiting request must not outlive him (the next player in his slot would be Refused).
	g_join.gate.forget(id);
	g_join.precache_wait.erase(std::remove(g_join.precache_wait.begin(), g_join.precache_wait.end(), id), g_join.precache_wait.end());
	g_char.choosers.forget(id); // he left while choosing his character
	g_load.held.erase(id); // nobody waits for his screen
	g_board.board.forget(id); // nobody of his stands at an exit any more
	g_board.board_ticked = 0;
	host_load_forget(id, "he left"); // the simultaneous load does not wait for him
	auto it = g_join.peers.find(id);
	if (it == g_join.peers.end())
		return;
	diag::arm_destroyed_object_stacks(); // his character and its items leave the world now
	const u16 proxy = it->second.proxy;
	world::host_park_proxy(proxy);
	g_join.peers.erase(it);
	take_offline(proxy); // gone from the session = gone from the world (stays in the save)
	host_send_owners(); // nobody plays that character now
}

bool host_player_in_world(u16 proxy_id)
{
	// A world loaded since the last frame is not served yet (host_watch_world): its ids are not the peers' proxies.
	if (proxy_id == kNoId || g_join.world_seen != g_world_generation)
		return false; // a map server: server_player_came keeps world_seen
	for (const auto& it : g_join.peers)
		if (it.second.proxy == proxy_id)
			return !it.second.loading;
	return false;
}

u16 proxy_of_peer(PeerId id)
{
	auto it = g_join.peers.find(id);
	return it == g_join.peers.end() ? kNoId : it->second.proxy;
}

PeerId peer_of_proxy(u16 proxy_id)
{
	for (const auto& it : g_join.peers)
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
	transition_world_replaced(); // a new session: no board of an earlier one
	load_screen_off(); // nor a loading screen of an earlier one
	g_join.my_proxy = kNoId;
	g_join.load_issued = false;
	g_char.char_step = CharState::CharStep::None;
	g_join.receive_started = mgr().now_ms();
	mgr().set_phase(Phase::ReceivingWorld);
	mgr().session().send(kHostPeer, Lane::Reliable, Msg::JoinRequest, Writer());
}

u16 my_proxy() { return g_join.my_proxy; }

bool client_loading_world() { return mgr().phase() == Phase::LoadingWorld; }

void client_news_added(const GAME_NEWS_DATA& news) { news_log::on_news(mgr().active() && !mode().decides_world(), g_join.my_news, news); }

void client_take_news_log(xr_vector<GAME_NEWS_DATA>& actor_log)
{
	news_log::on_actor_spawn(mgr().active() && !mode().decides_world(), actor_log, g_join.my_news);
}

bool is_client_cache_save(LPCSTR name)
{
	return name && save_policy::is_client_cache(name);
}

void client_cache_path(string_path& out, LPCSTR extension)
{
	string_path relative;
	xr_sprintf(relative, "%s%s", save_policy::kCacheFile, extension);
	FS.update_path(out, "$app_data_root$", relative);
}

void prepare_client_cache()
{
	// The old reserved join snapshot lived among campaign saves. Archive it without deleting or replacing any file.
	const LPCSTR extensions[] = {".scop", ".scoc", ".dds"};
	string_path from[3], to[3];
	bool found = false;
	for (u32 i = 0; i < 3; ++i)
	{
		save_path(from[i], save_policy::kLegacyStem, extensions[i]);
		const DWORD attr = GetFileAttributesA(from[i]);
		found |= attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
	}
	if (!found) return;
	for (u32 slot = 0; slot < 100; ++slot)
	{
		bool free = true;
		for (u32 i = 0; i < 3; ++i)
		{
			string_path relative;
			xr_sprintf(relative, "raylink\\cache\\legacy_session_%u\\coop_session%s", slot, extensions[i]);
			FS.update_path(to[i], "$app_data_root$", relative);
			free &= GetFileAttributesA(to[i]) == INVALID_FILE_ATTRIBUTES;
		}
		if (!free) continue;
		bool moved = false;
		for (u32 i = 0; i < 3; ++i)
		{
			const DWORD attr = GetFileAttributesA(from[i]);
			if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_DIRECTORY)) continue;
			VerifyPath(to[i]);
			if (MoveFileExA(from[i], to[i], MOVEFILE_WRITE_THROUGH))
			{
				moved = true;
				::Msg("[coop] archived legacy join cache: %s", to[i]);
			}
			else
				::Msg("! [coop] cannot archive legacy join cache %s (%lu); file kept", from[i], GetLastError());
		}
		if (moved)
		{
			FS.get_path("$game_saves$")->m_Flags.set(FS_Path::flNeedRescan, TRUE);
			FS.m_Flags.set(CLocatorAPI::flNeedCheck, TRUE);
			FS.rescan_pathes();
		}
		return;
	}
	::Msg("! [coop] legacy join cache archive is full; files kept");
}

bool may_save_world()
{
	return save_policy::may_save_world(mgr().active() && !mode().decides_world(), world_is_mirror());
}

bool world_is_mirror()
{
	if (mode().decides_world())
		return false; // a mirror kept after a dropped session and hosted from: the world is this machine's now
	return client_loading_world() || (g_mirror_generation != 0 && g_mirror_generation == g_world_generation);
}

void on_session_lost(const char*)
{
	// The players leave with the session (a client that was in the host's world leaves
	// it, the host takes the characters of its players out below). Called before the phase goes Off.
	if (mgr().is_live_client() || (mode().decides_world() && !g_join.peers.empty()))
		diag::arm_destroyed_object_stacks();
	// "the host closed the connection and the client stays in the game, talking to NPCs". A client's
	// world is the host's, mirrored: without the host it is nobody's (none of it is saved, its NPCs only stand there).
	// The client leaves it for the main menu; the co-op window tells why (script::on_session_end, before this call).
	if (mgr().active() && !mode().decides_world() && g_pGameLevel)
	{
		::Msg("[coop] the session is over: leaving the host's world for the main menu");
		Console->Execute("disconnect"); // the engine defers it (KERNEL:disconnect): safe from a network callback
	}
	client_char_closed("session"); // a choice window of this session closes with it (none: nothing)
	// A state held for a load of this session is nobody's in the next one (every host counts its epochs from 1: the number
	// alone would let it into another session's world)
	players::condition_carry_drop();
	// The session is over, nobody else is in the world. Clients take the avatars out through their own
	// switch vote (coop::switch_vote: no session = no player in the world).
	xr_vector<u16> proxies;
	for (const auto& it : g_join.peers)
		proxies.push_back(it.second.proxy);
	load_screen_line(false); // a held loading screen's line goes with the session
	load_screen_input(false); // and its hold of the level's input
	reset_state();
	for (u16 proxy : proxies)
		take_offline(proxy);
}

void on_blob_progress(PeerId, u32, u32 kind, u64 done, u64 total)
{
	if (kind != BlobWorldSave && kind != BlobPreparedSave)
		return;
	const u64 now = mgr().now_ms();
	g_load.screen.progress(now); // the save of the transition still comes (a screen that is not waiting: no effect)
	if (now - g_join.last_progress_log < 500 && done != total)
		return;
	g_join.last_progress_log = now;
	::Msg("[coop] downloading world: %llu / %llu KB", done / 1024, total / 1024);
}

void on_blob_complete(PeerId from, u32, u32 kind, std::vector<u8>& data)
{
	if (from != kHostPeer || !mgr().active() || mode().decides_world())
		return;
	if (kind == BlobPreparedSave)
	{
		// The save of the simultaneous load (LoadWait Prepared came first; the session dropped one of another epoch)
		if (!prepared_save_awaited())
		{
			::Msg("! [coop] load: the save of a transition arrived while none was awaited (%s): ignored",
			    phase_name(mgr().phase()));
			return;
		}
		// His actor still sits in the car the host released him from: the save waits for the
		// normal detach of the next frames (client_seat_holds loads it), the old world stays up meanwhile
		if (g_load.save_hold.step(vehicles::local_seat_attached(), mgr().now_ms()) == transition::TravelStep::Wait)
		{
			::Msg("[coop] load: the save came %.1f s after the go (%u KB); it waits until this player is out of his car",
			    float(mgr().now_ms() - g_load.screen.since) / 1000.f, u32(data.size() / 1024));
			g_load.seated_save.swap(data);
			return;
		}
		g_load.screen.loading();
		::Msg("[coop] load: the save came %.1f s after the go (%u KB), loading it",
		    float(mgr().now_ms() - g_load.screen.since) / 1000.f, u32(data.size() / 1024));
		client_load_world(data);
		return;
	}
	if (kind != BlobWorldSave)
		return;
	// Only the world we asked for and are waiting for (the session already dropped saves of another epoch).
	if (mgr().phase() != Phase::ReceivingWorld || g_join.load_issued)
	{
		::Msg("! [coop] a world save arrived while none was awaited (%s): ignored", phase_name(mgr().phase()));
		return;
	}
	g_load.screen.loading(); // his screen stays for his map server (a screen that is not waiting: no effect)
	client_load_world(data);
}

// Host: a load or a level change replaced the world under the session. Everybody rejoins
// from a fresh snapshot of the new world; requests made in the old one are dropped (epoch).
void host_watch_world()
{
	PROF_EVENT("coop/coop_join/host_watch_world");
	// A generation counter, not the simulator pointer: a same-level quick load deletes and
	// recreates the simulator in one call, usually at the same address.
	if (g_join.world_seen != g_world_generation)
	{
		g_join.world_seen = g_world_generation;
		g_load.held.clear(); // their joins are void, whoever waited for a map server joins again
		objects::reset(); // per-world state of physics/zones belongs to the old world
		levels::host_world_replaced(); // every mark of the old simulator went with it
		// The epoch moved on in on_world_created. Joins into the old world are void, everybody rejoins
		// after SessionReset; a level exit asked for in it means nothing here.
		g_join.gate.world_replaced();
		g_join.precache_wait.clear(); // requests of the old world: everybody rejoins after SessionReset
		g_char.choosers.world_replaced(); // a player choosing is asked again after his SessionReset
		transition_world_replaced(); // a wait at an exit of the old world means nothing here
		g_join.reset_pending = true;
		// The world of a simultaneous load (the epoch given at the go, kept by on_world_created) keeps its movers;
		// any other world (a load meanwhile) calls that load off
		const bool prepared_world = g_load.load.active() && g_load.load.epoch() == g_join.epoch;
		if (g_load.load.active() && !prepared_world)
			host_load_void("another world was loaded");
		for (auto& it : g_join.peers)
		{
			if (prepared_world && it.second.prepared)
			{
				// He loads this very save: his character has the same id here. What the old world left queued for him is
				// older than his save; what this one does from now on waits for his WorldLoaded (host_end_join flushes it):
				// the objects this world brings online after the save are among it.
				it.second.loading = true;
				it.second.sent_at = 0;
				net::host_drop(it.first);
				sync::host_begin_join(it.first);
				continue;
			}
			it.second.prepared = false;
			it.second.loading = true;
			it.second.sent_at = 0; // the join timer restarts at SessionReset, not at the first join
			it.second.proxy = kNoId; // an id of the old world: set again by their JoinRequest
			sync::host_begin_join(it.first); // queue/drop world traffic until they rejoin
		}
	}
	// The same "level is up" as for joins (host_precaching): an unfocused host sent no SessionReset either.
	if (!g_join.reset_pending || !world_up() || host_precaching()) // world gate: bReady too, not just g_pGameLevel
		return;
	g_join.reset_pending = false;
	// A join admitted since the world changed (a player who connected meanwhile) is void
	// as well: he rejoins after this SessionReset, and a join left in flight would refuse his JoinRequest.
	g_join.gate.world_replaced();
	world::host_scan_proxies();
	Writer w(64);
	w.str("the host loaded a save or changed level");
	w.u32(g_join.epoch); // the client's session tells it back before its JoinRequest
	const u64 now = mgr().now_ms();
	u32 resets = 0, movers = 0;
	for (const PeerInfo& p : mgr().session().peers())
	{
		auto it = g_join.peers.find(p.id);
		if (it != g_join.peers.end() && it->second.prepared)
		{
			// No SessionReset for a mover: his world is this one already (the transition's save)
			world::host_mark_proxy(it->second.proxy, p.id);
			syncapi::host_send_full(p.id); // his coop.sync copy, as at a join (LoadWait Prepared reset his)
			it->second.sent_at = now; // his load into this world is what the join's time limit measures
			++movers;
			continue;
		}
		mgr().session().send(p.id, Lane::Reliable, Msg::SessionReset, w);
		if (it != g_join.peers.end())
			it->second.sent_at = now; // the rejoin into the new world is what the timeout measures
		++resets;
	}
	if (g_load.load.active())
	{
		g_load.load.world_up(now); // the host player is ready with it; the others' minute starts now
		g_load.load_up_at = now;
		::Msg("[coop] load: the host's world is up %.1f s after the go; %u player(s) loaded it with the host",
		    float(now - g_load.load_go_at) / 1000.f, movers);
	}
	::Msg("[coop] world replaced (epoch %u): %u player(s) rejoin", g_join.epoch, resets);
}

constexpr u64 kJoinTimeoutMs = 5 * 60 * 1000; // a join is limited in time

// Host: a client that never finishes loading the world is dropped with a clear reason.
void host_watch_joins()
{
	PROF_EVENT("coop/coop_join/host_watch_joins");
	const u64 now = mgr().now_ms();
	xr_vector<PeerId> stuck;
	for (const auto& it : g_join.peers)
	{
		if (!it.second.loading || g_char.choosers.in_choice(it.first))
			continue; // a player choosing his character (after a world change) has the limit below
		if (net::host_queue_overflow(it.first) || (it.second.sent_at && now - it.second.sent_at > kJoinTimeoutMs))
			stuck.push_back(it.first);
	}
	for (PeerId id : stuck)
	{
		::Msg("! [coop] #%u did not finish loading the world in time, dropping it", id);
		mgr().session().kick(id, RejectReason::Timeout, "loading the host's world took too long, try to join again");
	}
	// A player who keeps the choice window open holds a slot and a join in flight.
	for (PeerId id : g_char.choosers.expired(now))
	{
		::Msg("! [coop] #%u did not choose a character in %u min, dropping it", id, u32(newchar::kChoiceTimeoutMs / 60000));
		g_char.choosers.forget(id);
		mgr().session().kick(id, RejectReason::Timeout, "you did not choose a character in time, join again");
	}
}

void frame()
{
	PROF_EVENT("coop/coop_join/frame");
	if (mode().decides_world())
	{
		host_watch_world();
		// The clock is read again after every step that may block for long: the board's go writes the transition's save,
		// a due join writes the world's.
		const u64 now = mgr().now_ms();
		host_load_watch(now); // a go whose reload never came
		transition_step(now); // the host player's zone and vote, his hold at the edge, the placard's numbers
		player_load_step(now); // the host player's loading screen
		host_board_step(now);
		const u64 after_board = mgr().now_ms();
		host_load_step(after_board); // the barrier of a simultaneous load
		host_held_step(after_board); // the players waiting for their map server
		host_watch_joins();
		host_due_joins();
		arrival_step(mgr().now_ms()); // the host player's arrival journal
		return;
	}
	// Client: downloading or loading the world must end in time. Choosing a character has the
	// host's limit (it drops us first; a minute more here for a host that is gone silent).
	const bool choosing = g_char.char_step == CharState::CharStep::Offered || g_char.char_step == CharState::CharStep::Shown;
	const u64 limit = choosing ? newchar::kChoiceTimeoutMs + 60000 : kJoinTimeoutMs;
	if ((mgr().phase() == Phase::ReceivingWorld || mgr().phase() == Phase::LoadingWorld) && g_join.receive_started &&
	    mgr().now_ms() - g_join.receive_started > limit)
	{
		::Msg("! [coop] joining the host's world took too long");
		mgr().stop("joining the host's world took too long");
		return;
	}
	client_char_frame(); // the offer of a new character and the choice
	if (!mgr().active())
		return; // the window is gone (stopped above)
	client_seat_holds(mgr().now_ms()); // a save or a move held for the detach of his seat (it may load the save now)
	const u64 now = mgr().now_ms();
	if (mgr().is_live_client())
		transition_step(now); // his zone, his hold at the edge of the level, the placard's numbers
	player_load_step(now); // LoadReady, the held screen, Enter (live comes with it)
	arrival_step(mgr().now_ms()); // this player's arrival journal
	if (!mgr().active() || mgr().phase() != Phase::LoadingWorld || !g_join.swap_done)
		return;
	// Level is up and the local actor exists: tell the host.
	if (!g_pGameLevel || !Level().game || !g_actor || Device.dwPrecacheFrame)
		return;
	if (g_load.screen.stage != transition::Load::None)
		return; // a simultaneous load: this player goes live with the host's Enter (player_load_step)
	client_go_live();
}

void print_status()
{
	if (mode().decides_world())
	{
		::Msg("[coop]   world epoch %u; refused: %u join(s), %u WorldLoaded, %u level exit(s); %u join(s) wait for the "
		      "host's load",
		    g_join.epoch, g_join.joins_refused, g_join.loaded_ignored, g_board.votes_refused, u32(g_join.precache_wait.size()));
		// The log names a dropped type once per client; the count is here every time
		for (const limits::RateRule& rule : limits::kClientRateRules)
			::Msg("[coop]   rate limit %s: %llu dropped (more than %u in a row, then one per %u ms)", msg_name(rule.type),
			    mgr().session().stats(u16(rule.type)).rate_dropped, rule.burst, rule.refill_ms);
		::Msg("[coop]   new characters this session: %u of %u (coop_max_new_characters), %u new key(s) refused",
		    u32(g_char.new_characters.count()), u32(g_coop_max_new_characters), g_char.characters_refused);
		::Msg("[coop]   new characters before the world (protocol 8): %u made, %u choosing now, %u choice(s) refused, "
		      "%u ignored",
		    g_char.characters_made, u32(g_char.choosers.count()), g_char.choices_refused, g_char.choices_ignored);
		const transition::StateMsg& b = g_board.board_view;
		if (b.state != transition::State::Idle)
			::Msg("[coop]   transition: %s at %s (exit %u), %u of %u ready%s", b.state == transition::State::Going ? "going" : "waiting",
			    b.level.c_str(), u32(b.changer), u32(b.ready), u32(b.total), b.kind == 1 ? " (a guide trip)" : "");
		// The simultaneous load of the group's transition
		if (g_load.load.active())
		{
			std::string rows;
			for (const auto& row : g_load.load.rows())
			{
				rows += rows.empty() ? "" : ", ";
				rows += nick_of_peer(row.first) + (g_load.load.entered(row.first) ? " in" : (row.second ? " ready" : " loading"));
			}
			::Msg("[coop]   load: epoch %u to '%s', %s, %.1f s since the go: %s", g_load.load.epoch(),
			    level_name_by_id(g_load.load_level).c_str(), g_load.load.world_is_up() ? "the host's world is up" : "the host is loading",
			    float(mgr().now_ms() - g_load.load_go_at) / 1000.f, rows.c_str());
		}
		for (const auto& it : g_join.peers)
			::Msg("[coop]   peer #%u proxy %u%s%s%s", it.first, it.second.proxy, it.second.loading ? " (loading)" : "",
			    it.second.prepared ? " (simultaneous load)" : "", g_join.gate.waiting(it.first) ? " (join waits for its turn)" : "");
	}
	else if (mgr().active())
	{
		::Msg("[coop]   my proxy (host numbering) %u, swap %s", g_join.my_proxy, g_join.swap_done ? "done" : "pending");
		::Msg("[coop]   transition: %s%s%s, %u vote(s) sent, held at the edge %u time(s), %u route(s) known",
		    g_board.view.state == transition::State::Idle ? "nobody waits" : "the host's board is on",
		    g_board.want ? ", I am ready" : "", g_board.consent ? " (I go with the group)" : "", g_board.votes_sent, g_board.holds_applied,
		    u32(g_board.routes.all().size()));
		if (g_board.trips_refused) // the log names the first one only
			::Msg("[coop]   level changes refused here: %u (no level changer of this level leads there)", g_board.trips_refused);
		if (g_char.char_step != CharState::CharStep::None)
			::Msg("[coop]   new character: %s", g_char.char_step == CharState::CharStep::Sent ? "chosen, waiting for the world"
			                                                                       : "choosing (window)");
	}
	// This player's screen of a simultaneous load (a client, or the host player)
	if (mgr().active() && g_load.screen.stage != transition::Load::None)
	{
		static const char* const kStage[] = {"none", "the save comes", "loading", "held, waiting for the others", "entering"};
		::Msg("[coop]   load screen: epoch %u to '%s', %s, %u still waited for", g_load.screen.epoch,
		    level_name_by_id(g_load.screen_level).c_str(), kStage[size_t(g_load.screen.stage)], u32(g_load.screen.waiting_for.size()));
	}
}
} // namespace join
} // namespace coop
