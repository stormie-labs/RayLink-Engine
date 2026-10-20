#include "stdafx.h"
#include "coop_join.h"
#include "coop_manager.h"
#include "coop_sync.h"
#include "coop_world.h"
#include "coop_entities.h"
#include "coop_alife.h"

#include "../../xrEngine/XR_IOConsole.h"
#include "Level.h"
#include "alife_simulator.h"
#include "alife_object_registry.h"
#include "alife_graph_registry.h"
#include "xrServer.h"
#include "xrServer_Objects_ALife_Monsters.h"
#include "xrServer_Objects_ALife_Items.h"
#include "../xrServerEntities/xrMessages.h"

#include <cstdio>

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
} g;

xr_string key_hex(const PlayerKey& k)
{
	static const char* digits = "0123456789abcdef";
	xr_string s;
	s.reserve(k.size() * 2);
	for (u8 b : k)
	{
		s.push_back(digits[b >> 4]);
		s.push_back(digits[b & 15]);
	}
	return s;
}

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
	FILE* f = std::fopen(path, "wb");
	if (!f)
		return false;
	const bool ok = std::fwrite(data, 1, size, f) == size;
	std::fclose(f);
	return ok;
}

void save_path(string_path& out, LPCSTR name, LPCSTR ext)
{
	string_path file;
	xr_sprintf(file, "%s%s", name, ext);
	FS.update_path(out, "$game_saves$", file);
}

// Key stored in the proxy's custom data: "[coop]\nkey = <hex>\nnick = <nick>\n".
bool proxy_has_key(const CSE_Abstract* e, const xr_string& hex)
{
	return e->m_ini_string.size() && strstr(e->m_ini_string.c_str(), hex.c_str()) != nullptr;
}

CSE_ALifeHumanStalker* find_proxy_by_key(const PlayerKey& key)
{
	const xr_string hex = key_hex(key);
	for (const auto& it : ai().alife().objects().objects())
	{
		CSE_ALifeDynamicObject* e = it.second;
		if (!e || xr_strcmp(e->s_name, kProxySection))
			continue;
		if (proxy_has_key(e, hex))
			return smart_cast<CSE_ALifeHumanStalker*>(e);
	}
	return nullptr;
}

CSE_ALifeHumanStalker* create_proxy(const PeerInfo& p)
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
	xr_sprintf(ini, "[coop]\nkey = %s\nnick = %s\n", key_hex(p.key).c_str(), p.nick.c_str());
	proxy->m_ini_string = ini;
	proxy->m_character_name = p.nick.c_str();
	::Msg("[coop] new character for '%s': proxy id %u at [%.1f %.1f %.1f]", p.nick.c_str(), proxy->ID, pos.x, pos.y,
	    pos.z);
	return proxy;
}

// --- host: JoinRequest ---
void host_on_join_request(PeerId from, Reader&)
{
	const PeerInfo* p = mgr().session().peer(from);
	if (!p)
		return;
	if (!host_can_start())
	{
		mgr().session().kick(from, RejectReason::HostNotReady, "host is not in a game right now, try again later");
		return;
	}

	CSE_ALifeHumanStalker* proxy = find_proxy_by_key(p->key);
	if (proxy)
		::Msg("[coop] '%s' returns to character proxy id %u", p->nick.c_str(), proxy->ID);
	else
		proxy = create_proxy(*p);
	if (!proxy)
	{
		mgr().session().kick(from, RejectReason::HostNotReady, "host could not create a character for you");
		return;
	}
	HostPeer& hp = g.peers[from];
	hp.proxy = proxy->ID;
	world::host_mark_proxy(proxy->ID, from);

	// Fresh world save: engine part (.scop) and Anomaly's script part (.scoc).
	string64 name;
	xr_sprintf(name, "coop_join_%u", from);
	{
		NET_Packet packet;
		packet.w_begin(M_SAVE_GAME);
		packet.w_stringZ(name);
		packet.w_u8(0); // keep the host's own current save name
		Level().Send(packet, net_flags(TRUE));
	}
	string_path scop, scoc;
	save_path(scop, name, ".scop");
	save_path(scoc, name, ".scoc");
	std::vector<u8> engine_part, script_part;
	if (!read_file(scop, engine_part) || engine_part.empty())
	{
		::Msg("! [coop] world save failed (%s)", scop);
		mgr().session().kick(from, RejectReason::HostNotReady, "host could not save the world");
		return;
	}
	read_file(scoc, script_part); // absent if Anomaly's script storage is disabled

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
	const u32 id = mgr().session().send_blob(from, BlobWorldSave, std::move(blob));
	hp.loading = true;
	hp.sent_at = mgr().now_ms();
	sync::host_begin_join(from);
	::Msg("[coop] world for '%s': %u + %u bytes, blob %u", p->nick.c_str(), (u32)engine_part.size(),
	    (u32)script_part.size(), id);

	FS.file_delete(scop);
	FS.file_delete(scoc);
}

// --- host: WorldLoaded ---
void host_on_world_loaded(PeerId from, Reader&)
{
	auto it = g.peers.find(from);
	if (it == g.peers.end())
		return;
	it->second.loading = false;
	const PeerInfo* p = mgr().session().peer(from);
	::Msg("[coop] '%s' is in the world (load took %.1f s)", p ? p->nick.c_str() : "?",
	    float(mgr().now_ms() - it->second.sent_at) / 1000.f);
	sync::host_end_join(from);
	mgr().session().send(from, Lane::Reliable, Msg::JoinComplete, Writer());
}

// --- client ---
void client_on_join_complete(PeerId, Reader&)
{
	::Msg("[coop] join complete, live sync on");
}

void client_on_session_reset(PeerId, Reader& r)
{
	const std::string why = r.str();
	::Msg("[coop] host replaced the world (%s), rejoining", why.c_str());
	g.my_proxy = kNoId;
	g.load_issued = false;
	g.swap_done = false;
	sync::reset();
	mgr().set_phase(Phase::Handshaked);
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

	if (g_pGameLevel)
		Console->Execute("disconnect");
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

} // namespace

void install(Session& s)
{
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
	world::host_scan_proxies();
}

void on_peer_joined(const PeerInfo&) {}

void on_peer_left(PeerId id)
{
	auto it = g.peers.find(id);
	if (it == g.peers.end())
		return;
	world::host_park_proxy(it->second.proxy);
	g.peers.erase(it);
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
	g = State();
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
	if (kind == BlobWorldSave && from == kHostPeer && mgr().is_client())
		client_load_world(data);
}

void frame()
{
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
		for (const auto& it : g.peers)
			::Msg("[coop]   peer #%u proxy %u%s", it.first, it.second.proxy, it.second.loading ? " (loading)" : "");
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

	// Where we stand and what we look like.
	swap_values(actor->o_Position, proxy->o_Position);
	swap_values(actor->o_Angle, proxy->o_Angle);
	swap_values(actor->m_tNodeID, proxy->m_tNodeID);
	swap_values(actor->m_tGraphID, proxy->m_tGraphID);
	{
		const float h = actor->get_health();
		actor->set_health(proxy->get_health());
		proxy->set_health(h);
	}

	// Who we are: money, community, rank, reputation, name.
	swap_values(actor->m_dwMoney, proxy->m_dwMoney);
	swap_values(actor->m_community_index, proxy->m_community_index);
	swap_values(actor->CSE_ALifeTraderAbstract::m_rank, proxy->CSE_ALifeTraderAbstract::m_rank);
	swap_values(actor->m_reputation, proxy->m_reputation);
	swap_values(actor->m_character_name, proxy->m_character_name);

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
