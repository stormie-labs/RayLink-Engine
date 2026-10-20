#include "stdafx.h"
#include "coop_entities.h"
#include "coop_manager.h"
#include "coop_net.h"
#include "coop_join.h"
#include "coop_world.h"
#include "coop_sync.h"

#include "Level.h"
#include "GameObject.h"
#include "entity_alive.h"
#include "coop_alife.h"
#include "ai/stalker/ai_stalker.h"
#include "ai/monsters/basemonster/base_monster.h"
#include "alife_simulator.h"
#include "alife_object_registry.h"
#include "alife_graph_registry.h"
#include "xrServer.h"
#include "xrServer_Objects_ALife.h"
#include "xrServer_Objects_ALife_Monsters.h"
#include "../xrServerEntities/xrMessages.h"
#include "../Include/xrRender/Kinematics.h"
#include "../Include/xrRender/KinematicsAnimated.h"
#include "../Include/xrRender/animation_blend.h"

namespace coop
{
namespace entities
{
namespace
{
constexpr u16 kNoId = 0xffff;
constexpr u32 kSnapshotIntervalMs = 66; // ~15 Hz
constexpr u32 kInterpDelayMs = 120;
constexpr u32 kMaxPartitions = 4;
constexpr size_t kSnapshotBudget = 1100; // bytes per EntitySnapshot packet

// ---------------------------------------------------------------- host side

enum class HostEventKind : u8
{
	Registered,
	Released,
	Switched,
};
struct HostEvent
{
	HostEventKind kind;
	u16 id;
	bool online;
};
// Filled from ALife/server hooks (game thread or main thread, never concurrently with frame()).
xr_vector<HostEvent> g_host_events;
u64 g_last_snapshot = 0;
u64 g_host_spawns = 0, g_host_releases = 0, g_host_switches = 0, g_host_events_sent = 0, g_snapshots_sent = 0;

void write_packet(Writer& w, const NET_Packet& p)
{
	w.u16(u16(p.B.count));
	w.bytes(p.B.data, p.B.count);
}

bool read_packet(Reader& r, NET_Packet& p)
{
	const u16 n = r.u16();
	if (!r.ok() || n > sizeof(p.B.data))
		return false;
	p.B.count = n;
	p.r_pos = 0;
	return r.bytes(p.B.data, n);
}

void write_placement(Writer& w, const CSE_ALifeDynamicObject* e)
{
	float pos[3] = {e->o_Position.x, e->o_Position.y, e->o_Position.z};
	w.vec3(pos);
	float ang[3] = {e->o_Angle.x, e->o_Angle.y, e->o_Angle.z};
	w.vec3(ang);
	w.u32(e->m_tNodeID);
	w.u16(e->m_tGraphID);
}

void host_send_spawn(CSE_ALifeDynamicObject* e)
{
	NET_Packet spawn, update;
	e->Spawn_Write(spawn, FALSE);
	update.w_begin(M_UPDATE);
	e->UPDATE_Write(update);

	Writer w(spawn.B.count + update.B.count + 32);
	w.u16(e->ID);
	w.u16(e->ID_Parent);
	w.boolean(e->m_bOnline);
	write_placement(w, e);
	write_packet(w, spawn);
	write_packet(w, update);
	net::host_broadcast(Msg::EntitySpawn, w);
	++g_host_spawns;
}

void host_send_switch(CSE_ALifeDynamicObject* e, bool online)
{
	Writer w(256);
	w.u16(e->ID);
	w.boolean(online);
	write_placement(w, e);
	NET_Packet update;
	update.w_begin(M_UPDATE);
	e->UPDATE_Write(update);
	write_packet(w, update);
	net::host_broadcast(Msg::EntitySwitch, w);
	++g_host_switches;
}

void host_process_events()
{
	if (g_host_events.empty())
		return;
	xr_vector<HostEvent> events;
	events.swap(g_host_events);
	for (const HostEvent& ev : events)
	{
		switch (ev.kind)
		{
		case HostEventKind::Registered:
			if (CSE_ALifeDynamicObject* e = ai().alife().objects().object(ev.id, true))
				host_send_spawn(e);
			break;
		case HostEventKind::Released:
		{
			Writer w(4);
			w.u16(ev.id);
			net::host_broadcast(Msg::EntityDestroy, w);
			++g_host_releases;
			break;
		}
		case HostEventKind::Switched:
			if (CSE_ALifeDynamicObject* e = ai().alife().objects().object(ev.id, true))
				host_send_switch(e, ev.online);
			break;
		}
	}
}

// Generic animation state: per bone partition the newest blend's motion and phase.
struct AnimState
{
	u32 motion[kMaxPartitions] = {};
	u16 time_ms[kMaxPartitions] = {};
	u8 count = 0;
};

void capture_anims(CGameObject* obj, AnimState& a)
{
	a.count = 0;
	IKinematicsAnimated* ka = obj->Visual() ? smart_cast<IKinematicsAnimated*>(obj->Visual()) : nullptr;
	if (!ka)
		return;
	for (u32 part = 0; part < kMaxPartitions; ++part)
	{
		const u32 n = ka->LL_PartBlendsCount(part);
		CBlend* best = nullptr;
		for (u32 i = 0; i < n; ++i)
		{
			CBlend* b = ka->LL_PartBlend(part, i);
			if (b && (!best || b->blendAmount >= best->blendAmount))
				best = b;
		}
		a.motion[part] = best ? best->motionID.val : 0;
		a.time_ms[part] = best ? u16(_min(best->timeCurrent, 65.f) * 1000.f) : 0;
		a.count = u8(part + 1);
	}
}

bool is_creature(CGameObject* obj)
{
	return smart_cast<CAI_Stalker*>(obj) || smart_cast<CBaseMonster*>(obj);
}

void host_send_snapshots()
{
	const u64 now = mgr().now_ms();
	if (now - g_last_snapshot < kSnapshotIntervalMs)
		return;
	g_last_snapshot = now;

	Writer w(kSnapshotBudget + 128);
	u16 in_packet = 0;
	auto flush = [&]()
	{
		if (!in_packet)
			return;
		Writer out(w.size() + 8);
		out.u32(u32(now));
		out.u16(in_packet);
		out.bytes(w.ptr(), w.size());
		net::host_broadcast(Msg::EntitySnapshot, out, Lane::Unreliable);
		++g_snapshots_sent;
		w.clear();
		in_packet = 0;
	};

	for (u32 i = 0, n = Level().Objects.o_count(); i < n; ++i)
	{
		CGameObject* obj = smart_cast<CGameObject*>(Level().Objects.o_get_by_iterator(i));
		if (!obj || obj->getDestroy() || !is_creature(obj) || world::is_player_proxy(obj->ID()))
			continue;
		CEntityAlive* alive = smart_cast<CEntityAlive*>(obj);
		float h, p, b;
		obj->XFORM().getHPB(h, p, b);
		const Fvector& pos = obj->Position();

		w.u16(obj->ID());
		float v[3] = {pos.x, pos.y, pos.z};
		w.vec3(v);
		w.angle16(angle_normalize(h));
		w.u8(u8(clampr(alive ? alive->GetfHealth() : 1.f, 0.f, 1.f) * 255.f));
		AnimState a;
		capture_anims(obj, a);
		w.u8(a.count);
		for (u8 k = 0; k < a.count; ++k)
		{
			w.u32(a.motion[k]);
			w.u16(a.time_ms[k]);
		}
		if (++in_packet >= 255 || w.size() >= kSnapshotBudget)
			flush();
	}
	flush();
}

// ---------------------------------------------------------------- client side

xr_set<u16> g_online; // local ids the host has online
bool g_applying = false; // true while we execute a host command on our ALife

struct MirrorSample
{
	u64 local_time = 0;
	Fvector pos{};
	float yaw = 0.f;
	float health = 1.f;
	AnimState anims;
};
struct MirrorTrack
{
	xr_deque<MirrorSample> samples;
	s64 clock_offset = 0;
	bool have_offset = false;
	u32 last_remote = 0;
	u32 applied[kMaxPartitions] = {};
};
xr_map<u16, MirrorTrack> g_tracks;
u64 g_client_spawns = 0, g_client_releases = 0, g_client_switches = 0, g_missing = 0, g_blocked_spawns = 0,
    g_forwarded_spawns = 0;

struct ApplyGuard
{
	ApplyGuard() { g_applying = true; }
	~ApplyGuard() { g_applying = false; }
};

void read_placement(Reader& r, Fvector& pos, Fvector& ang, u32& node, u16& graph)
{
	float p[3], a[3];
	r.vec3(p);
	r.vec3(a);
	pos.set(p[0], p[1], p[2]);
	ang.set(a[0], a[1], a[2]);
	node = r.u32();
	graph = r.u16();
}

void place(CSE_ALifeDynamicObject* e, const Fvector& pos, const Fvector& ang, u32 node, u16 graph_id)
{
	if (!e->m_bOnline && e->ID_Parent == kNoId && e->m_tGraphID != graph_id &&
	    ai().game_graph().valid_vertex_id(graph_id))
		alife_rw().graph().change(e, e->m_tGraphID, graph_id);
	e->o_Position = pos;
	e->o_Angle = ang;
	e->m_tNodeID = node;
	e->m_tGraphID = graph_id;
}

void apply_update(CSE_ALifeDynamicObject* e, NET_Packet& update)
{
	u16 type;
	update.r_begin(type);
	if (type == M_UPDATE)
		e->UPDATE_Read(update);
}

void client_on_spawn(PeerId, Reader& r)
{
	const u16 id = join::to_local(r.u16());
	const u16 parent_host = r.u16();
	const u16 parent = parent_host == kNoId ? kNoId : join::to_local(parent_host);
	const bool online = r.boolean();
	Fvector pos, ang;
	u32 node;
	u16 graph_id;
	read_placement(r, pos, ang, node, graph_id);
	NET_Packet spawn, update;
	if (!read_packet(r, spawn) || !read_packet(r, update))
		return;
	if (ai().alife().objects().object(id, true))
		return; // already have it (P6: repeat is harmless)

	u16 type;
	spawn.r_begin(type);
	string64 section;
	spawn.r_stringZ(section);
	CSE_Abstract* abstract = F_entity_Create(section);
	CSE_ALifeDynamicObject* e = smart_cast<CSE_ALifeDynamicObject*>(abstract);
	if (!e)
	{
		::Msg("! [coop] cannot create mirrored entity %u [%s]", id, section);
		F_entity_Destroy(abstract);
		return;
	}
	spawn.r_pos = 0;
	e->Spawn_Read(spawn);
	apply_update(e, update);
	ApplyGuard guard;
	e->ID = alife_rw().server().PerformIDgen(id);
	if (e->ID != id)
	{
		::Msg("! [coop] id %u is taken locally, mirrored entity [%s] got %u", id, section, e->ID);
		alife_rw().release(e, false);
		return;
	}
	e->ID_Parent = parent;
	place(e, pos, ang, node, graph_id);
	e->m_bOnline = false;
	alife_rw().register_object(e, true);
	if (online && parent == kNoId)
		g_online.insert(id);
	++g_client_spawns;
}

void client_on_destroy(PeerId, Reader& r)
{
	const u16 id = join::to_local(r.u16());
	if (id == 0)
		return;
	g_online.erase(id);
	g_tracks.erase(id);
	CSE_ALifeDynamicObject* e = ai().alife().objects().object(id, true);
	if (!e)
		return;
	ApplyGuard guard;
	alife_rw().release(e, true); // the standard chain: also destroys the game object (E3)
	++g_client_releases;
}

void client_on_switch(PeerId, Reader& r)
{
	const u16 id = join::to_local(r.u16());
	const bool online = r.boolean();
	Fvector pos, ang;
	u32 node;
	u16 graph_id;
	read_placement(r, pos, ang, node, graph_id);
	NET_Packet update;
	if (!read_packet(r, update) || id == 0)
		return;
	CSE_ALifeDynamicObject* e = ai().alife().objects().object(id, true);
	if (!e)
	{
		++g_missing; // the host will not resend: ask for it (P3: find first, then create)
		Writer w(4);
		w.u16(join::to_host(id));
		net::client_send(Msg::StateDumpRequest, w);
		return;
	}
	ApplyGuard guard;
	if (online)
	{
		g_online.insert(id);
		if (!e->m_bOnline)
		{
			place(e, pos, ang, node, graph_id);
			apply_update(e, update);
			alife_rw().switch_online(e);
		}
	}
	else
	{
		g_online.erase(id);
		g_tracks.erase(id);
		if (e->m_bOnline)
			alife_rw().switch_offline(e);
		place(e, pos, ang, node, graph_id);
		apply_update(e, update);
	}
	++g_client_switches;
}

void client_on_snapshot(PeerId, Reader& r)
{
	const u32 remote_time = r.u32();
	const u16 count = r.u16();
	const u64 now = mgr().now_ms();
	for (u16 i = 0; i < count && r.ok(); ++i)
	{
		const u16 id = join::to_local(r.u16());
		MirrorSample s;
		float p[3];
		r.vec3(p);
		s.pos.set(p[0], p[1], p[2]);
		s.yaw = r.angle16();
		s.health = float(r.u8()) / 255.f;
		s.anims.count = _min<u8>(r.u8(), u8(kMaxPartitions));
		for (u8 k = 0; k < s.anims.count; ++k)
		{
			s.anims.motion[k] = r.u32();
			s.anims.time_ms[k] = r.u16();
		}
		if (!r.ok() || id == 0)
			break;
		MirrorTrack& t = g_tracks[id];
		if (t.have_offset && s32(remote_time - t.last_remote) <= 0)
			continue; // latest wins
		const s64 offset = s64(now) - s64(remote_time);
		if (!t.have_offset || offset < t.clock_offset)
			t.clock_offset = offset;
		else
			t.clock_offset += 1;
		t.have_offset = true;
		t.last_remote = remote_time;
		s.local_time = u64(s64(remote_time) + t.clock_offset);
		t.samples.push_back(s);
		while (t.samples.size() > 24)
			t.samples.pop_front();
	}
}

// Host: a client asks for an entity it does not have.
void host_on_entity_request(PeerId from, Reader& r)
{
	const u16 id = r.u16();
	CSE_ALifeDynamicObject* e = ai().alife().objects().object(id, true);
	if (!e)
		return;
	NET_Packet spawn, update;
	e->Spawn_Write(spawn, FALSE);
	update.w_begin(M_UPDATE);
	e->UPDATE_Write(update);
	Writer w(spawn.B.count + update.B.count + 32);
	w.u16(e->ID);
	w.u16(e->ID_Parent);
	w.boolean(e->m_bOnline);
	write_placement(w, e);
	write_packet(w, spawn);
	write_packet(w, update);
	net::host_send(from, Msg::EntitySpawn, w);
}

bool interpolate(MirrorTrack& t, u64 render_time, MirrorSample& out)
{
	if (t.samples.empty())
		return false;
	if (t.samples.size() == 1 || render_time <= t.samples.front().local_time)
	{
		out = t.samples.front();
		return true;
	}
	for (size_t i = 1; i < t.samples.size(); ++i)
	{
		const MirrorSample& b = t.samples[i];
		if (b.local_time < render_time)
			continue;
		const MirrorSample& a = t.samples[i - 1];
		const float span = float(b.local_time - a.local_time);
		const float k = span > 0.f ? float(render_time - a.local_time) / span : 1.f;
		out = k < 0.5f ? a : b;
		out.pos.lerp(a.pos, b.pos, k);
		out.yaw = angle_lerp(a.yaw, b.yaw, k);
		return true;
	}
	out = t.samples.back();
	return true;
}
} // namespace

// ---------------------------------------------------------------- API

void install(Session& s)
{
	s.on(Msg::EntitySpawn, [](PeerId from, Reader& r) { if (mgr().is_client()) client_on_spawn(from, r); });
	s.on(Msg::EntityDestroy, [](PeerId from, Reader& r) { if (mgr().is_client()) client_on_destroy(from, r); });
	s.on(Msg::EntitySwitch, [](PeerId from, Reader& r) { if (mgr().is_client()) client_on_switch(from, r); });
	s.on(Msg::EntitySnapshot, [](PeerId from, Reader& r) { if (mgr().is_live_client()) client_on_snapshot(from, r); });
	s.on(Msg::StateDumpRequest, [](PeerId from, Reader& r) { if (mgr().is_host()) host_on_entity_request(from, r); });
}

void reset()
{
	g_host_events.clear();
	g_online.clear();
	g_tracks.clear();
	g_last_snapshot = 0;
}

void on_peer_left(PeerId) {}

void host_begin_join(PeerId) {}
void host_end_join(PeerId) {}

void host_collect_online(xr_vector<u16>& out)
{
	out.clear();
	if (!ai().get_alife())
		return;
	for (const auto& it : ai().alife().objects().objects())
	{
		const CSE_ALifeDynamicObject* e = it.second;
		if (e && e->m_bOnline && e->ID_Parent == kNoId && e->ID != 0)
			out.push_back(e->ID);
	}
}

void host_on_entity_registered(CSE_ALifeDynamicObject* e)
{
	if (mgr().is_host() && e)
		g_host_events.push_back({HostEventKind::Registered, e->ID, false});
}

void host_on_entity_released(u16 id)
{
	if (mgr().is_host())
		g_host_events.push_back({HostEventKind::Released, id, false});
}

void host_on_switch(CSE_ALifeDynamicObject* e, bool online)
{
	if (mgr().is_host() && e && e->ID != 0)
		g_host_events.push_back({HostEventKind::Switched, e->ID, online});
}

void host_on_event(NET_Packet&) {}

void client_set_initial_online(const xr_vector<u16>& host_ids)
{
	g_online.clear();
	for (u16 id : host_ids)
		g_online.insert(join::to_local(id));
	g_online.erase(0);
	::Msg("[coop] host has %u objects online", (u32)g_online.size());
}

bool client_wants_online(u16 id) { return g_online.count(id) != 0; }

void on_client_live() { g_tracks.clear(); }

bool is_mirrored(u16 id) { return mgr().is_client() && id != 0; }

bool client_applying() { return g_applying; }

bool intercept_local_spawn(CSE_Abstract* entity)
{
	if (!mgr().is_client() || g_applying || !entity)
		return false;
	// Only the host creates entities. Things our player caused (parent is our actor or one of
	// its items) go to the host as a request; world-caused spawns (loot on death, smart
	// terrain population) are the host's business and are dropped here.
	u16 owner = entity->ID_Parent;
	for (int depth = 0; owner != kNoId && owner != 0 && depth < 4; ++depth)
	{
		CSE_ALifeDynamicObject* p = ai().get_alife() ? ai().alife().objects().object(owner, true) : nullptr;
		owner = p ? p->ID_Parent : kNoId;
	}
	if (owner == 0)
	{
		Writer w(64);
		w.str(entity->s_name.c_str());
		w.u16(join::to_host(entity->ID_Parent));
		float pos[3] = {entity->o_Position.x, entity->o_Position.y, entity->o_Position.z};
		w.vec3(pos);
		net::client_send(Msg::ReqSpawn, w);
		++g_forwarded_spawns;
	}
	else
		++g_blocked_spawns;
	return true;
}

bool drive_mirror(CGameObject* obj)
{
	auto it = g_tracks.find(obj->ID());
	if (it == g_tracks.end())
		return false;
	MirrorSample s;
	if (!interpolate(it->second, mgr().now_ms() - kInterpDelayMs, s))
		return false;
	Fmatrix xform;
	xform.setHPB(s.yaw, 0.f, 0.f);
	xform.c.set(s.pos);
	obj->XFORM().set(xform);

	IKinematicsAnimated* ka = obj->Visual() ? smart_cast<IKinematicsAnimated*>(obj->Visual()) : nullptr;
	if (ka)
	{
		for (u8 part = 0; part < s.anims.count; ++part)
		{
			const u32 value = s.anims.motion[part];
			if (!value || value == it->second.applied[part])
				continue;
			MotionID m;
			m.val = value;
			if (m.slot >= ka->LL_MotionsSlotCount())
				continue;
			if (CBlend* b = ka->LL_PlayCycle(u16(part), m, TRUE, nullptr, nullptr))
				b->timeCurrent = float(s.anims.time_ms[part]) / 1000.f;
			it->second.applied[part] = value;
		}
	}
	return true;
}

void frame()
{
	if (mgr().is_host())
	{
		host_process_events();
		if (g_pGameLevel)
			host_send_snapshots();
	}
}

void print_status()
{
	if (mgr().is_host())
		::Msg("[coop]   world out: %llu spawns, %llu releases, %llu switches, %llu snapshot packets", g_host_spawns,
		    g_host_releases, g_host_switches, g_snapshots_sent);
	else
		::Msg("[coop]   world in: %u online, %u tracked, %llu spawns, %llu releases, %llu switches, %llu missing, "
		    "local spawns %llu forwarded / %llu blocked",
		    (u32)g_online.size(), (u32)g_tracks.size(), g_client_spawns, g_client_releases, g_client_switches,
		    g_missing, g_forwarded_spawns, g_blocked_spawns);
}
} // namespace entities
} // namespace coop
