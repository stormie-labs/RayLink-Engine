#include "stdafx.h"
#include "pch_script.h"
#include "coop_objects.h"
#include "coop_manager.h"
#include "coop_net.h"
#include "coop_join.h"
#include "coop_world.h"
#include "coop_sync.h"
#include "coop_entities.h"
#include "coop_events.h"
#include "coop_alife.h"
#include "coop_story.h"
#include "coop_checkpoint_sources.h"
#include "xrCoop/coop_rules.h"
#include "xrCoop/coop_validate.h"

#include "ai_space.h"
#include "script_engine.h"
#include "Level.h"
#include "Actor.h"
#include "ActorEffector.h"
#include "CameraEffector.h"
#include "GameObject.h"
#include "entity_alive.h"
#include "PhysicsShellHolder.h"
#include "PhysicObject.h"
#include "DestroyablePhysicsObject.h"
#include "BreakableObject.h"
#include "CustomZone.h"
#include "ZoneCampfire.h"
#include "Artefact.h"
#include "Missile.h"
#include "Grenade.h"
#include "Bolt.h"
#include "Explosive.h"
#include "CustomRocket.h"
#include "WeaponStatMgun.h"
#include "helicopter.h"
#include "Inventory.h"
#include "InventoryOwner.h"
#include "CharacterPhysicsSupport.h"
#include "PHMovementControl.h"
#include "PHSynchronize.h"
#include "ParticlesObject.h"
#include "HudSound.h"
#include "Hit.h"
#include "alife_simulator.h"
#include "alife_object_registry.h"
#include "xrServer_Objects_ALife.h"
#include "xrServer_Objects_ALife_Items.h"
#include "../xrServerEntities/xrMessages.h"
#include "../xrphysics/PhysicsShell.h"
#include "../xrphysics/PhysicsCommon.h"
#include "../Include/xrRender/animation_blend.h"
#include "../xrEngine/motion.h"

namespace coop
{
namespace objects
{
namespace
{
constexpr u16 kNoId = 0xffff;
constexpr u16 kActor = 0;
constexpr u32 kStreamIntervalMs = 66; // ~15 Hz physics samples
constexpr u32 kInterpDelayMs = 100;
constexpr u32 kMaxExtrapolateMs = 150;
constexpr u32 kScanIntervalMs = 500; // client: make new world bodies kinematic
constexpr u32 kPushIntervalMs = 100;
constexpr u32 kCleanupIntervalMs = 2000;
constexpr u32 kTorridResendMs = 10000;
constexpr u32 kExplodeWaitMs = 300; // client: wait this long for the object before the fallback effect
constexpr u32 kPendingStateTtlMs = 300000;
constexpr u32 kThrowApplyTimeoutMs = 3000;
constexpr u32 kConsumeAfterMs = 3000;
constexpr u32 kPacketBudget = 1100;
constexpr u16 kMaxElements = 64;
constexpr float kMaxThrowSpeed = 45.f;
constexpr float kMaxReach = 4.5f; // request validation: player character to object

enum class StateKind : u8
{
	Samples = 1,
	Rest = 2,
	Anim = 3,
	Destroyed = 4,
	Broken = 5,
	Explode = 6,
};
enum class ZoneKind : u8
{
	Zones = 1,
};
enum class RequestKind : u8
{
	Use = 1,
	Push = 2,
	Throw = 3,
	Activate = 4,
};
enum SampleFlags : u8
{
	SfMoving = 1,
	SfPoseOnly = 2,
};
enum ThrowFlags : u8
{
	TfConsume = 1,
	TfCanTake = 2,
	TfRandomSpin = 4,
};

enum class ObjKind : u8
{
	None,
	Body, // host-simulated rigid bodies (kinematic on clients)
	Animated, // physic object driven by its own animation: phase sync
	Pose, // flying helicopter: pose only
};

struct Pose
{
	Fvector p;
	Fquaternion q;
	Fvector v; // linear velocity
	Fvector w; // angular velocity
};

struct ZoneEntry
{
	u16 id = kNoId;
	u8 state = 0;
	u32 elapsed = 0;
	u8 campfire = 0xff;
	float anim = -1.f;
};

struct AnimEntry
{
	u16 id = kNoId;
	bool playing = false;
	float speed = 0.f;
	float time = 0.f;
};

struct DestroyedEntry
{
	u16 id = kNoId;
	u16 bone = 0;
	u16 hit_type = 0;
	Fvector dir{};
	float impulse = 0.f;
	Fvector p_in_bone{};
};

struct ExplodeEntry
{
	u16 id = kNoId;
	u16 initiator = kNoId;
	Fvector pos{};
	Fvector normal{};
	shared_str section;
};

struct Counters
{
	// host
	u64 sample_packets = 0, sample_entries = 0, rests_sent = 0, zone_msgs = 0, anim_msgs = 0, destroyed_sent = 0,
	    broken_sent = 0, explode_sent = 0, fragments_relayed = 0, uses = 0, pushes = 0, throws_ok = 0,
	    throws_refused = 0, activations = 0, consumed = 0, zone_hits_dropped = 0, projectiles = 0,
	    throws_malformed = 0, consume_forced = 0; // R3 В7
	// client
	u64 samples_in = 0, rests_in = 0, poses_applied = 0, rests_applied = 0, mismatch = 0, zones_applied = 0,
	    explode_applied = 0, explode_fallback = 0, destroyed_applied = 0, broken_applied = 0, uses_sent = 0, sleeping_bags_used = 0, tents_used = 0,
	    pushes_sent = 0, throws_sent = 0, activations_sent = 0, forced_online = 0, explode_blocked = 0,
	    switches_blocked = 0;
	u64 dropped_before_live = 0; // R2 I2: EntityState / ZoneState that came before we were live
	u64 uses_echo_dropped = 0;   // R4 4: obj:use() on a world object from a reaction to the host's story
};

xrCriticalSection g_lock;
Counters g_c;

// ---------------------------------------------------------------- shared helpers

u32 hash_str(const char* s)
{
	u32 h = 2166136261u;
	for (; s && *s; ++s)
		h = (h ^ u8(*s)) * 16777619u;
	return h;
}

u64 now_ms() { return mgr().now_ms(); }

// R2 I2: a world message that comes before we are live is dropped (rules::live_only): it is from the window between
// Welcome and JoinRequest or from a world we have left. Kept, it would be applied to the world we load.
bool before_live(Msg type)
{
	if (!rules::live_only(type) || mgr().is_live_client())
		return false;
	++g_c.dropped_before_live;
	return true;
}

CGameObject* find_go(u16 id)
{
	if (!g_pGameLevel || id == kNoId)
		return nullptr;
	CGameObject* go = smart_cast<CGameObject*>(Level().Objects.net_Find(id));
	return go && !go->getDestroy() ? go : nullptr;
}

void put_vec(Writer& w, const Fvector& v)
{
	const float a[3] = {v.x, v.y, v.z};
	w.vec3(a);
}

Fvector get_vec(Reader& r)
{
	float a[3];
	r.vec3(a);
	Fvector v;
	v.set(a[0], a[1], a[2]);
	return v;
}

u16 pack_unit(float f) { return u16(s16(iFloor(clampr(f, -1.f, 1.f) * 32767.f))); }
float unpack_unit(u16 v) { return float(s16(v)) / 32767.f; }

void put_quat(Writer& w, const Fquaternion& q)
{
	w.u16(pack_unit(q.x));
	w.u16(pack_unit(q.y));
	w.u16(pack_unit(q.z));
	w.u16(pack_unit(q.w));
}

Fquaternion get_quat(Reader& r)
{
	Fquaternion q;
	q.x = unpack_unit(r.u16());
	q.y = unpack_unit(r.u16());
	q.z = unpack_unit(r.u16());
	q.w = unpack_unit(r.u16());
	const float m = _sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
	if (m < EPS_S)
		q.identity();
	else
	{
		q.x /= m;
		q.y /= m;
		q.z /= m;
		q.w /= m;
	}
	return q;
}

void put_v16(Writer& w, const Fvector& v)
{
	w.u16(u16(s16(iFloor(clampr(v.x * 100.f, -32767.f, 32767.f)))));
	w.u16(u16(s16(iFloor(clampr(v.y * 100.f, -32767.f, 32767.f)))));
	w.u16(u16(s16(iFloor(clampr(v.z * 100.f, -32767.f, 32767.f)))));
}

Fvector get_v16(Reader& r)
{
	Fvector v;
	v.x = float(s16(r.u16())) / 100.f;
	v.y = float(s16(r.u16())) / 100.f;
	v.z = float(s16(r.u16())) / 100.f;
	return v;
}

// Packs entries of one message kind into packets of at most kPacketBudget bytes.
class Batcher
{
public:
	Batcher(Msg type, u8 kind, bool timed, Lane lane, PeerId to, bool count16 = false) :
		m_type(type), m_kind(kind), m_timed(timed), m_lane(lane), m_to(to), m_count16(count16)
	{
	}
	~Batcher() { flush(); }

	void add(const Writer& entry)
	{
		if (m_count && (m_body.size() + entry.size() > kPacketBudget || m_count >= (m_count16 ? 4000u : 255u)))
			flush();
		m_body.bytes(entry.ptr(), entry.size());
		++m_count;
	}

	void flush()
	{
		if (!m_count)
			return;
		Writer out(m_body.size() + 16);
		out.u8(m_kind);
		if (m_timed)
			out.u32(u32(now_ms()));
		if (m_count16)
			out.u16(u16(m_count));
		else
			out.u8(u8(m_count));
		out.bytes(m_body.ptr(), m_body.size());
		if (m_to == kInvalidPeer)
			net::host_broadcast(m_type, out, m_lane);
		else
			net::host_send(m_to, m_type, out, m_lane);
		++m_packets;
		m_body.clear();
		m_count = 0;
	}

	u32 packets() const { return m_packets; }

private:
	Msg m_type;
	u8 m_kind;
	bool m_timed;
	Lane m_lane;
	PeerId m_to;
	bool m_count16;
	Writer m_body{kPacketBudget + 256};
	u32 m_count = 0;
	u32 m_packets = 0;
};

// What kind of world object `go` is for this module (same rule on host and clients).
ObjKind classify(CGameObject* go)
{
	if (!go || go->getDestroy() || go->H_Parent() || go->ID() == kActor)
		return ObjKind::None;
	if (world::is_player_proxy(go->ID()))
		return ObjKind::None; // player characters belong to their players
	if (smart_cast<CCustomZone*>(go) || smart_cast<CCustomRocket*>(go) || smart_cast<CWeaponStatMgun*>(go))
		return ObjKind::None; // zones: state sync; rockets: WP2/WP9 launch path; mounted guns: holders
	CPhysicsShellHolder* sh = go->cast_physics_shell_holder();
	if (!sh)
		return ObjKind::None;
	CPhysicsShell* shell = sh->PPhysicsShell();
	if (CHelicopter* heli = smart_cast<CHelicopter*>(go))
	{
		// a flying helicopter keeps an active but disabled shell (SpawnInitPhysics): pose only
		if (heli->g_Alive())
			return ObjKind::Pose;
		return shell && shell->isActive() ? ObjKind::Body : ObjKind::None;
	}
	CEntityAlive* alive = smart_cast<CEntityAlive*>(go);
	if (alive && alive->g_Alive())
		return ObjKind::None; // living creatures: coop_entities / coop_players
	if (!shell || !shell->isActive())
		return ObjKind::None;
	if (shell->PPhysicsShellAnimator())
		return ObjKind::Animated;
	if (alive && !shell->isFullActive())
		return ObjKind::None; // ragdoll still being built
	return ObjKind::Body;
}

bool capture(CPhysicsShellHolder* sh, xr_vector<Pose>& out)
{
	CPhysicsShell* shell = sh->PPhysicsShell();
	if (!shell || !shell->isActive())
		return false;
	const u16 n = sh->PHGetSyncItemsNumber();
	if (!n || n > kMaxElements || n != shell->get_ElementsNumber())
		return false;
	out.resize(n);
	for (u16 i = 0; i < n; ++i)
	{
		CPhysicsElement* e = shell->get_ElementByStoreOrder(i);
		CPHSynchronize* sync = sh->PHGetSyncItem(i);
		if (!e || !e->isActive() || !sync)
			return false;
		SPHNetState st;
		sync->get_State(st);
		out[i].p = st.position;
		out[i].q = st.quaternion;
		out[i].v = st.linear_vel;
		out[i].w = st.angular_vel;
	}
	return true;
}

void write_body(Writer& w, u16 id, u8 flags, const xr_vector<Pose>& poses, bool velocities)
{
	w.u16(id);
	w.u8(flags);
	w.u8(u8(poses.size()));
	for (const Pose& p : poses)
	{
		put_vec(w, p.p);
		put_quat(w, p.q);
		if (velocities)
		{
			put_v16(w, p.v);
			put_v16(w, p.w);
		}
	}
}

void write_zone(Writer& w, const ZoneEntry& e)
{
	w.u16(e.id);
	w.u8(e.state);
	w.u32(e.elapsed);
	w.u8(e.campfire);
	w.f32(e.anim);
}

void write_anim(Writer& w, const AnimEntry& a)
{
	w.u16(a.id);
	w.u8(a.playing ? 1 : 0);
	w.f32(a.speed);
	w.f32(a.time);
}

u8 campfire_flag(CCustomZone* zone)
{
	CZoneCampfire* c = smart_cast<CZoneCampfire*>(zone);
	return c ? u8(c->is_on() ? 1 : 0) : u8(0xff);
}

// ---------------------------------------------------------------- host state

// Recorded by hooks (any thread), consumed by frame() on the main thread.
xr_vector<ZoneEntry> g_ev_zones;
xr_vector<AnimEntry> g_ev_anims;
xr_vector<DestroyedEntry> g_ev_destroyed;
xr_vector<u16> g_ev_broken;
xr_vector<ExplodeEntry> g_ev_explode;
xr_vector<u16> g_ev_fragments;
xr_vector<std::pair<u16, u64>> g_ev_projectiles;

xr_map<u16, u64> g_zone_switch_ms; // host clock of the last state switch of a zone
struct TorridPhase
{
	float time = 0.f;
	float length = 0.f;
};
xr_map<u16, TorridPhase> g_torrid_host;
xr_map<u16, AnimEntry> g_anim_host; // last phase sent per animated physic object
xr_map<u16, u64> g_anim_host_ms;

struct HostBody
{
	bool moving = false;
	u64 last_move = 0;
};
xr_map<u16, HostBody> g_host_bodies; // bodies that moved since the level loaded
struct HostPose
{
	Fvector p{};
	Fquaternion q{};
	u64 sent = 0;
};
xr_map<u16, HostPose> g_host_poses;

struct PendingThrow
{
	PeerId peer = kInvalidPeer;
	u16 proxy = kNoId;
	Fvector pos{};
	Fvector dir{};
	Fvector vel{};
	u8 flags = 0;
	u64 created = 0;
};
xr_map<u16, PendingThrow> g_pending_throws; // spawned projectile id -> throw to apply
struct Consume
{
	u16 proxy = kNoId;
	u64 deadline = 0;
	// R3 В7: the thrown item itself. Its id may be reused once it is gone, and a freed entity's memory too (ABA):
	// both the pointer and the section must still match.
	const CSE_Abstract* entity = nullptr;
	shared_str section;
};
xr_map<u16, Consume> g_consume; // thrown hand item -> host removes it if the owner did not
xr_map<PeerId, u64> g_last_throw;
xr_map<u32, u64> g_last_push; // (peer << 16 | object) -> time

u64 g_last_stream = 0, g_last_cleanup = 0, g_last_torrid = 0;

// ---------------------------------------------------------------- client state

struct Sample
{
	u64 local_time = 0;
	u32 remote = 0;
	u8 flags = 0;
	xr_vector<Pose> poses;
};

struct ClientBody
{
	xr_deque<Sample> samples;
	bool pose_only = false;
	bool has_rest = false;
	bool rest_pending = false;
	u32 rest_remote = 0;
	u32 section_hash = 0;
	xr_vector<Pose> rest;
	u64 last_input = 0;
	u64 last_online_try = 0;
};
xr_map<u16, ClientBody> g_bodies; // local ids

s64 g_clock_offset = 0;
bool g_have_offset = false;

struct FixedRec
{
	CPhysicsShell* shell = nullptr;
	xr_vector<u16> elements; // elements this module fixed (released on reset)
};
xr_map<u16, FixedRec> g_fixed; // local ids of bodies made kinematic here
u64 g_fixed_elements = 0;

struct PendingZone
{
	ZoneEntry e;
	u64 received = 0;
};
xr_map<u16, PendingZone> g_zone_pending; // local ids
struct ClientPhase
{
	float time = 0.f;
	u64 received = 0;
};
xr_map<u16, ClientPhase> g_torrid_client;
struct ClientAnim
{
	AnimEntry a;
	u64 received = 0;
};
xr_map<u16, ClientAnim> g_anim_client;
struct PendingDestroyed
{
	DestroyedEntry d;
	u64 received = 0;
};
xr_map<u16, PendingDestroyed> g_destroyed_pending; // local ids
xr_map<u16, u64> g_broken_pending; // local id -> received
struct PendingExplode
{
	ExplodeEntry x;
	u64 received = 0;
};
xr_vector<PendingExplode> g_explode_pending;
xr_map<shared_str, HUD_SOUND_COLLECTION_LAYERED*> g_explode_sounds;
xr_map<u16, u64> g_push_sent; // local id -> time

u64 g_last_scan = 0, g_last_push_check = 0;

// Apply scopes (main thread only).
int g_zone_apply = 0;
u16 g_zone_apply_id = kNoId;
u32 g_zone_apply_elapsed = 0;
int g_hit_apply = 0;

struct ZoneApply
{
	ZoneApply(u16 id, u32 elapsed)
	{
		++g_zone_apply;
		g_zone_apply_id = id;
		g_zone_apply_elapsed = elapsed;
	}
	~ZoneApply()
	{
		--g_zone_apply;
		if (!g_zone_apply)
			g_zone_apply_id = kNoId;
	}
};

// ---------------------------------------------------------------- client: kinematic bodies

// Fixes every active element of a host-simulated body: no gravity, practically infinite mass, so
// nothing local (the actor, local scripts and joint motors, zone impulses) moves it. Idempotent.
bool ensure_fixed(CGameObject* go)
{
	CPhysicsShellHolder* sh = go->cast_physics_shell_holder();
	CPhysicsShell* shell = sh ? sh->PPhysicsShell() : nullptr;
	if (!shell || !shell->isActive())
		return false;
	FixedRec& f = g_fixed[go->ID()];
	if (f.shell != shell)
	{
		f.shell = shell;
		f.elements.clear();
	}
	const u16 n = shell->get_ElementsNumber();
	for (u16 i = 0; i < n; ++i)
	{
		CPhysicsElement* e = shell->get_ElementByStoreOrder(i);
		if (!e || !e->isActive() || e->isFixed())
			continue;
		e->Fix();
		f.elements.push_back(i);
		++g_fixed_elements;
	}
	return true;
}

void release_fixed()
{
	for (auto& it : g_fixed)
	{
		CGameObject* go = find_go(it.first);
		CPhysicsShellHolder* sh = go ? go->cast_physics_shell_holder() : nullptr;
		CPhysicsShell* shell = sh ? sh->PPhysicsShell() : nullptr;
		if (!shell || shell != it.second.shell || !shell->isActive())
			continue;
		for (u16 i : it.second.elements)
		{
			if (i >= shell->get_ElementsNumber())
				continue;
			CPhysicsElement* e = shell->get_ElementByStoreOrder(i);
			if (e && e->isActive() && e->isFixed())
				e->ReleaseFixed();
		}
	}
	g_fixed.clear();
}

bool apply_poses(CGameObject* go, const xr_vector<Pose>& poses, bool moving)
{
	CPhysicsShellHolder* sh = go->cast_physics_shell_holder();
	CPhysicsShell* shell = sh ? sh->PPhysicsShell() : nullptr;
	if (!shell || !shell->isActive())
		return false;
	const u16 n = sh->PHGetSyncItemsNumber();
	if (n != poses.size() || n != shell->get_ElementsNumber())
	{
		++g_c.mismatch;
		return false;
	}
	for (u16 i = 0; i < n; ++i)
	{
		CPhysicsElement* e = shell->get_ElementByStoreOrder(i);
		if (!e || !e->isActive())
			return false;
	}
	ensure_fixed(go);
	for (u16 i = 0; i < n; ++i)
	{
		CPHSynchronize* sync = sh->PHGetSyncItem(i);
		if (!sync)
			continue;
		SPHNetState st;
		sync->get_State(st);
		st.position = poses[i].p;
		st.previous_position = poses[i].p;
		st.quaternion = poses[i].q;
		st.previous_quaternion = poses[i].q;
		if (moving)
		{
			st.linear_vel = poses[i].v;
			st.angular_vel = poses[i].w;
		}
		else
		{
			st.linear_vel.set(0.f, 0.f, 0.f);
			st.angular_vel.set(0.f, 0.f, 0.f);
		}
		st.force.set(0.f, 0.f, 0.f);
		st.torque.set(0.f, 0.f, 0.f);
		st.enabled = true; // keeps the collision space current; a still body falls asleep by itself
		sync->set_State(st);
	}
	// CPHElement::set_LinearVel / set_AngularVel skip fixed elements: what contacts and joints push into a 1e8 kg body
	// would stay and grow frame after frame (contact effects scale with sqrt(mass), the body drifts once the poses stop).
	// FixBody zeroes it again (live 2026-10-29: a dead NPC's gun in the ground "screamed").
	for (u16 i = 0; i < n; ++i)
		if (CPhysicsElement* e = shell->get_ElementByStoreOrder(i))
			if (e->isFixed())
				e->Fix();
	return true;
}

void blend_poses(const xr_vector<Pose>& a, const xr_vector<Pose>& b, float k, xr_vector<Pose>& out)
{
	out.resize(a.size());
	const float t = clampr(k, 0.f, 1.f);
	for (size_t i = 0; i < a.size(); ++i)
	{
		out[i].p.lerp(a[i].p, b[i].p, t);
		out[i].q.slerp(a[i].q, b[i].q, t);
		out[i].v.lerp(a[i].v, b[i].v, t);
		out[i].w.lerp(a[i].w, b[i].w, t);
	}
}

// Pose set at local render time `t` from the sample buffer (latest-wins, short extrapolation).
bool sample_at(const ClientBody& b, u64 t, xr_vector<Pose>& out)
{
	if (b.samples.empty())
		return false;
	const Sample& first = b.samples.front();
	if (t <= first.local_time || b.samples.size() == 1)
	{
		if (b.samples.size() == 1 && t > first.local_time)
		{
			out = first.poses;
			const float dt = float(_min<u64>(t - first.local_time, kMaxExtrapolateMs)) / 1000.f;
			for (Pose& p : out)
				p.p.mad(p.v, dt);
			return true;
		}
		out = first.poses;
		return true;
	}
	for (size_t i = 1; i < b.samples.size(); ++i)
	{
		const Sample& s1 = b.samples[i];
		if (s1.local_time < t)
			continue;
		const Sample& s0 = b.samples[i - 1];
		if (s0.poses.size() != s1.poses.size())
		{
			out = s1.poses;
			return true;
		}
		const float span = float(s1.local_time - s0.local_time);
		blend_poses(s0.poses, s1.poses, span > 0.f ? float(t - s0.local_time) / span : 1.f, out);
		return true;
	}
	const Sample& last = b.samples.back();
	out = last.poses;
	if (last.flags & SfMoving)
	{
		const float dt = float(_min<u64>(t - last.local_time, kMaxExtrapolateMs)) / 1000.f;
		for (Pose& p : out)
			p.p.mad(p.v, dt);
	}
	return true;
}

// Client: the host has this object online but our copy is still offline: bring it in now (the
// host's online set is the rule here, coop_alife.cpp), otherwise a projectile shows up too late.
void force_online(u16 id, ClientBody& b)
{
	const u64 now = now_ms();
	if (now - b.last_online_try < 500 || !ai().get_alife())
		return;
	b.last_online_try = now;
	CSE_ALifeDynamicObject* e = ai().alife().objects().object(id, true);
	if (!e || e->m_bOnline || e->ID_Parent != kNoId || !entities::client_wants_online(id))
		return;
	entities::ClientApply scope;
	alife_rw().switch_online(e);
	++g_c.forced_online;
}

void client_scan()
{
	const u64 now = now_ms();
	if (now - g_last_scan < kScanIntervalMs)
		return;
	g_last_scan = now;
	for (u32 i = 0, n = Level().Objects.o_count(); i < n; ++i)
	{
		CGameObject* go = smart_cast<CGameObject*>(Level().Objects.o_get_by_iterator(i));
		if (!go || classify(go) != ObjKind::Body)
			continue;
		// A body the host never streamed is fixed only once it sleeps: one still settling after the
		// load would otherwise hang where it is. Host data (samples / rest) fixes it at once.
		if (g_bodies.count(go->ID()) || !go->cast_physics_shell_holder()->PPhysicsShell()->isEnabled())
			ensure_fixed(go);
	}
	for (auto it = g_fixed.begin(); it != g_fixed.end();)
	{
		CGameObject* go = find_go(it->first);
		CPhysicsShellHolder* sh = go ? go->cast_physics_shell_holder() : nullptr;
		if (!sh || sh->PPhysicsShell() != it->second.shell)
			it = g_fixed.erase(it);
		else
			++it;
	}
}

void client_apply_bodies()
{
	const u64 now = now_ms();
	const u64 render = now > kInterpDelayMs ? now - kInterpDelayMs : 0;
	xr_vector<Pose> poses;
	for (auto it = g_bodies.begin(); it != g_bodies.end();)
	{
		const u16 id = it->first;
		ClientBody& b = it->second;
		CGameObject* go = find_go(id);
		if (!go)
		{
			if (!b.samples.empty())
				force_online(id, b);
			const bool stale = now - b.last_input > kPendingStateTtlMs;
			const bool gone = ai().get_alife() && !ai().alife().objects().object(id, true);
			if (stale || (gone && now - b.last_input > 5000))
				it = g_bodies.erase(it);
			else
				++it;
			continue;
		}
		if (b.pose_only)
		{
			if (sample_at(b, render, poses) && !poses.empty())
			{
				Fmatrix m;
				m.rotation(poses[0].q);
				m.c.set(poses[0].p);
				go->XFORM().set(m);
			}
			++it;
			continue;
		}
		if (!b.samples.empty())
		{
			if (sample_at(b, render, poses) && apply_poses(go, poses, true))
				++g_c.poses_applied;
			if (b.samples.back().local_time + 1000 < now)
				b.samples.clear(); // the host went quiet without a rest: keep the last pose
		}
		else if (b.rest_pending)
		{
			if (b.section_hash && b.section_hash != hash_str(go->cNameSect().c_str()))
				b.rest_pending = false; // the id belongs to another object now
			else if (apply_poses(go, b.rest, false))
			{
				++g_c.rests_applied;
				b.rest_pending = false;
			}
			else if (now - b.last_input > 10000)
				b.rest_pending = false; // shape never matched (e.g. a body split on the host)
		}
		++it;
	}
}

// A push of our actor into a kinematic body: ask the host to push it.
void client_push_check()
{
	const u64 now = now_ms();
	if (now - g_last_push_check < kPushIntervalMs)
		return;
	g_last_push_check = now;
	CActor* actor = Actor();
	if (!actor || !actor->g_Alive() || !actor->character_physics_support() || !actor->character_physics_support()->movement())
		return;
	Fvector vel = actor->character_physics_support()->movement()->GetVelocity();
	vel.y = 0.f;
	const float speed = vel.magnitude();
	if (speed < 0.8f)
		return;
	Fvector dir = vel;
	dir.div(speed);
	Fvector chest = actor->Position();
	chest.y += 0.9f;
	for (const auto& it : g_fixed)
	{
		CGameObject* go = find_go(it.first);
		if (!go || go->Position().distance_to(chest) > 6.f || smart_cast<CEntityAlive*>(go) ||
			smart_cast<CInventoryItem*>(go))
			continue; // corpses and loose items are not walked aside
		CPhysicsShellHolder* sh = go->cast_physics_shell_holder();
		CPhysicsShell* shell = sh ? sh->PPhysicsShell() : nullptr;
		if (!shell || !shell->isActive() || shell->getMass() > 400.f || shell->getMass() < 5.f)
			continue;
		float best = flt_max;
		u16 best_i = 0;
		Fvector best_c{};
		for (u16 i = 0, n = shell->get_ElementsNumber(); i < n; ++i)
		{
			CPhysicsElement* e = shell->get_ElementByStoreOrder(i);
			if (!e || !e->isActive())
				continue;
			Fvector sz, c;
			e->get_Box(sz, c);
			const float dx = _max(_abs(chest.x - c.x) - sz.x * 0.5f, 0.f);
			const float dy = _max(_abs(chest.y - c.y) - sz.y * 0.5f - 0.9f, 0.f);
			const float dz = _max(_abs(chest.z - c.z) - sz.z * 0.5f, 0.f);
			const float d = _sqrt(dx * dx + dy * dy + dz * dz);
			if (d < best)
			{
				best = d;
				best_i = i;
				best_c = c;
			}
		}
		if (best > 0.45f)
			continue;
		Fvector to;
		to.sub(best_c, chest);
		to.y = 0.f;
		if (to.magnitude() > EPS_L)
		{
			to.normalize();
			if (to.dotproduct(dir) < 0.2f)
				continue; // walking away or alongside
		}
		u64& last = g_push_sent[it.first];
		if (now - last < kPushIntervalMs)
			continue;
		last = now;
		Writer w(32);
		w.u8(u8(RequestKind::Push));
		w.u16(join::to_host(it.first));
		w.u8(u8(best_i));
		put_vec(w, dir);
		w.f32(speed);
		net::client_send(Msg::EntityEvent, w);
		++g_c.pushes_sent;
	}
}

// ---------------------------------------------------------------- client: zones

void apply_zone(CCustomZone* zone, const ZoneEntry& e)
{
	if (e.campfire != 0xff)
	{
		CZoneCampfire* c = smart_cast<CZoneCampfire*>(zone);
		if (c && c->is_on() != (e.campfire != 0))
		{
			ZoneApply scope(zone->ID(), 0);
			if (e.campfire)
				c->turn_on_script();
			else
				c->turn_off_script();
		}
	}
	if (e.anim >= 0.f)
	{
		ClientPhase& ph = g_torrid_client[zone->ID()];
		ph.time = e.anim;
		ph.received = now_ms();
	}
	if (e.state >= u8(CCustomZone::eZoneStateMax))
		return;
	if (u8(zone->ZoneState()) == e.state)
		return; // already there (bulk / phase resend): replaying would restart its particles and sounds
	NET_Packet P;
	P.write_start();
	P.w_u8(e.state);
	P.r_pos = 0;
	ZoneApply scope(zone->ID(), e.elapsed);
	zone->OnEvent(P, GE_ZONE_STATE_CHANGE);
	++g_c.zones_applied;
}

void client_apply_zones()
{
	const u64 now = now_ms();
	for (auto it = g_zone_pending.begin(); it != g_zone_pending.end();)
	{
		CCustomZone* zone = smart_cast<CCustomZone*>(find_go(it->first));
		if (zone)
		{
			ZoneEntry e = it->second.e;
			const u64 waited = now - it->second.received;
			const bool still = e.state == u8(CCustomZone::eZoneStateIdle) || e.state == u8(CCustomZone::eZoneStateDisabled);
			if (!still)
				e.elapsed += u32(_min<u64>(waited, 600000));
			apply_zone(zone, e);
			it = g_zone_pending.erase(it);
		}
		else if (now - it->second.received > kPendingStateTtlMs)
			it = g_zone_pending.erase(it);
		else
			++it;
	}
}

// ---------------------------------------------------------------- client: destruction and explosions

bool apply_destroyed(const DestroyedEntry& d, u16 local_id)
{
	CGameObject* go = find_go(local_id);
	if (!go)
		return false;
	CDestroyablePhysicsObject* dpo = smart_cast<CDestroyablePhysicsObject*>(go);
	if (!dpo || dpo->Destroyed())
		return true;
	Fvector dir = d.dir;
	if (dir.magnitude() < EPS_L)
		dir.set(0.f, -1.f, 0.f);
	else
		dir.normalize();
	SHit hit(1000000.f, dir, go, d.bone, d.p_in_bone, d.impulse, ALife::EHitType(d.hit_type), 0.f, false);
	hit.whoID = go->ID();
	hit.weaponID = go->ID();
	++g_hit_apply;
	go->Hit(&hit);
	--g_hit_apply;
	if (dpo->Destroyed() && dpo->PPhysicsShell())
		dpo->PhysicallyRemoveSelf(); // no collision while the host's fragments are on their way
	++g_c.destroyed_applied;
	return true;
}

bool apply_broken(u16 local_id)
{
	CGameObject* go = find_go(local_id);
	if (!go)
		return false;
	if (!smart_cast<CBreakableObject*>(go))
		return true;
	Fvector dir;
	dir.set(0.f, -1.f, 0.f);
	SHit hit(1.f, dir, go, 0, Fvector().set(0.f, 0.f, 0.f), 0.f, ALife::eHitTypeStrike, 0.f, false);
	hit.whoID = go->ID();
	hit.weaponID = go->ID();
	go->Hit(&hit); // CheckHitBreak -> Break(); a broken object ignores it
	++g_c.broken_applied;
	return true;
}

void explode_fallback(const ExplodeEntry& x)
{
	LPCSTR sect = x.section.c_str();
	if (!sect || !*sect || !pSettings->section_exist(sect))
		return;
	Fmatrix m;
	m.identity();
	Fvector n = x.normal;
	if (n.magnitude() < EPS_L)
		n.set(0.f, 1.f, 0.f);
	n.normalize();
	m.j.set(n);
	Fvector::generate_orthonormal_basis(m.j, m.i, m.k);
	m.c.set(x.pos);
	m.c.y += 0.1f;
	if (pSettings->line_exist(sect, "explode_particles"))
	{
		intrusive_ptr<CParticlesObject> ps = Particles::Details::Create(pSettings->r_string(sect, "explode_particles"), TRUE);
		if (ps.get())
		{
			ps->UpdateParent(m, Fvector().set(0.f, 0.f, 0.f));
			ps->Play(false);
		}
	}
	if (pSettings->line_exist(sect, "snd_explode"))
	{
		HUD_SOUND_COLLECTION_LAYERED*& snd = g_explode_sounds[x.section];
		if (!snd)
		{
			snd = xr_new<HUD_SOUND_COLLECTION_LAYERED>();
			snd->LoadSound(sect, "snd_explode", "sndExplode", false, sg_SourceType);
		}
		snd->PlaySound("sndExplode", m.c, nullptr, false, false, u8(-1));
	}
	CActor* actor = Actor();
	if (actor)
	{
		const float max_dist = 30.f;
		const float d = actor->Position().distance_to(x.pos);
		shared_str eff;
		if (pSettings->line_exist(sect, "explode_effector_sect_name"))
			eff = pSettings->r_string(sect, "explode_effector_sect_name");
		else if (pSettings->line_exist("explode_effector", "effect_sect_name"))
			eff = pSettings->r_string("explode_effector", "effect_sect_name");
		if (d < max_dist && eff.size() && eff != "$no_effector")
			AddEffector(actor, effExplodeHit, eff, (max_dist - d) / max_dist);
	}
	++g_c.explode_fallback;
}

// Plays the host's detonation on our copy of the object. False if the object is not here yet.
bool apply_explode(const ExplodeEntry& x)
{
	CGameObject* go = find_go(join::to_local(x.id));
	CExplosive* ex = go ? go->cast_explosive() : nullptr;
	if (!ex)
		return false;
	if (!ex->IsExploding())
	{
		NET_Packet P;
		P.write_start();
		P.w_u16(x.initiator == kNoId ? kNoId : join::to_local(x.initiator));
		P.w_vec3(x.pos);
		P.w_vec3(x.normal);
		P.r_pos = 0;
		go->OnEvent(P, GE_GRENADE_EXPLODE);
		++g_c.explode_applied;
	}
	return true;
}

void client_apply_events()
{
	const u64 now = now_ms();
	for (auto it = g_destroyed_pending.begin(); it != g_destroyed_pending.end();)
	{
		if (apply_destroyed(it->second.d, it->first) || now - it->second.received > 30000)
			it = g_destroyed_pending.erase(it);
		else
			++it;
	}
	for (auto it = g_broken_pending.begin(); it != g_broken_pending.end();)
	{
		if (apply_broken(it->first) || now - it->second > 30000)
			it = g_broken_pending.erase(it);
		else
			++it;
	}
	for (auto it = g_explode_pending.begin(); it != g_explode_pending.end();)
	{
		if (apply_explode(it->x))
			it = g_explode_pending.erase(it);
		else if (now - it->received > kExplodeWaitMs)
		{
			explode_fallback(it->x);
			it = g_explode_pending.erase(it);
		}
		else
			++it;
	}
}

// ---------------------------------------------------------------- client: message handlers

u64 to_local_time(u32 remote)
{
	const u64 now = now_ms();
	const s64 offset = s64(now) - s64(remote);
	if (!g_have_offset || offset < g_clock_offset)
		g_clock_offset = offset;
	else
		g_clock_offset += 1; // let a stale minimum drift up slowly
	g_have_offset = true;
	return u64(s64(remote) + g_clock_offset);
}

bool read_poses(Reader& r, u8 n, bool velocities, xr_vector<Pose>& out)
{
	out.resize(n);
	for (u8 i = 0; i < n; ++i)
	{
		out[i].p = get_vec(r);
		out[i].q = get_quat(r);
		if (velocities)
		{
			out[i].v = get_v16(r);
			out[i].w = get_v16(r);
		}
		else
		{
			out[i].v.set(0.f, 0.f, 0.f);
			out[i].w.set(0.f, 0.f, 0.f);
		}
	}
	return r.ok();
}

void client_on_samples(Reader& r)
{
	const u32 remote = r.u32();
	const u8 count = r.u8();
	const u64 local = to_local_time(remote);
	const u64 now = now_ms();
	for (u8 k = 0; k < count && r.ok(); ++k)
	{
		const u16 id = join::to_local(r.u16());
		const u8 flags = r.u8();
		const u8 n = r.u8();
		Sample s;
		s.local_time = local;
		s.remote = remote;
		s.flags = flags;
		if (flags & SfPoseOnly)
		{
			s.poses.resize(1);
			s.poses[0].p = get_vec(r);
			s.poses[0].q = get_quat(r);
			s.poses[0].v.set(0.f, 0.f, 0.f);
			s.poses[0].w.set(0.f, 0.f, 0.f);
		}
		else if (!read_poses(r, n, true, s.poses))
			break;
		if (!r.ok() || id == kActor)
			break;
		ClientBody& b = g_bodies[id];
		b.last_input = now;
		b.pose_only = (flags & SfPoseOnly) != 0;
		if (b.has_rest && s32(remote - b.rest_remote) <= 0)
			continue; // older than the rest state we already have
		if (!b.samples.empty() && s32(remote - b.samples.back().remote) <= 0)
			continue; // latest wins
		b.samples.push_back(std::move(s));
		while (b.samples.size() > 16)
			b.samples.pop_front();
		b.rest_pending = false; // it moves again: the older rest pose must not be applied after these
		++g_c.samples_in;
	}
}

void client_on_rest(Reader& r)
{
	const u32 remote = r.u32();
	const u8 count = r.u8();
	const u64 now = now_ms();
	for (u8 k = 0; k < count && r.ok(); ++k)
	{
		const u16 id = join::to_local(r.u16());
		const u32 hash = r.u32();
		const u8 n = r.u8();
		xr_vector<Pose> poses;
		if (!read_poses(r, n, false, poses) || id == kActor)
			break;
		ClientBody& b = g_bodies[id];
		b.last_input = now;
		if (b.has_rest && s32(remote - b.rest_remote) < 0)
			continue;
		b.has_rest = true;
		b.rest_remote = remote;
		b.section_hash = hash;
		b.rest = std::move(poses);
		b.rest_pending = true;
		b.pose_only = false;
		// samples taken before this rest are history now
		while (!b.samples.empty() && s32(b.samples.front().remote - remote) <= 0)
			b.samples.pop_front();
		b.rest_pending = b.samples.empty(); // newer motion already here: its own rest will follow
		++g_c.rests_in;
	}
}

void client_on_state(PeerId, Reader& r)
{
	if (!mgr().is_client())
		return;
	// Every kind waits for Live: samples, rests and animations of the old world or our own would be applied to the
	// bodies of the world we load (pending until then), destroyed / broken / explosions are stale visuals (the
	// release itself arrives as EntityDestroy).
	if (before_live(Msg::EntityState))
		return;
	const StateKind kind = StateKind(r.u8());
	switch (kind)
	{
	case StateKind::Samples:
		client_on_samples(r);
		break;
	case StateKind::Rest:
		client_on_rest(r);
		break;
	case StateKind::Anim:
	{
		const u8 count = r.u8();
		for (u8 k = 0; k < count && r.ok(); ++k)
		{
			AnimEntry a;
			a.id = join::to_local(r.u16());
			a.playing = r.u8() != 0;
			a.speed = r.f32();
			a.time = r.f32();
			if (!r.ok())
				break;
			ClientAnim& c = g_anim_client[a.id];
			c.a = a;
			c.received = now_ms();
		}
		break;
	}
	case StateKind::Destroyed:
	{
		DestroyedEntry d;
		d.id = r.u16();
		d.bone = r.u16();
		d.hit_type = r.u16();
		d.dir = get_vec(r);
		d.impulse = r.f32();
		d.p_in_bone = get_vec(r);
		if (!r.ok())
			break;
		PendingDestroyed& p = g_destroyed_pending[join::to_local(d.id)];
		p.d = d;
		p.received = now_ms();
		break;
	}
	case StateKind::Broken:
	{
		const u16 id = r.u16();
		if (r.ok())
			g_broken_pending[join::to_local(id)] = now_ms();
		break;
	}
	case StateKind::Explode:
	{
		PendingExplode p;
		p.x.id = r.u16();
		p.x.initiator = r.u16();
		p.x.pos = get_vec(r);
		p.x.normal = get_vec(r);
		const std::string sect = r.str();
		if (!r.ok())
			break;
		p.x.section = sect.c_str();
		p.received = now_ms();
		g_explode_pending.push_back(p);
		break;
	}
	default:
		break;
	}
}

void client_on_zones(PeerId, Reader& r)
{
	if (!mgr().is_client())
		return;
	if (before_live(Msg::ZoneState))
		return; // a zone phase of another world: the host sends ours when we are in (host_on_peer_ready)
	if (ZoneKind(r.u8()) != ZoneKind::Zones)
		return;
	const u16 count = r.u16();
	const u64 now = now_ms();
	for (u16 k = 0; k < count && r.ok(); ++k)
	{
		ZoneEntry e;
		e.id = join::to_local(r.u16());
		e.state = r.u8();
		e.elapsed = r.u32();
		e.campfire = r.u8();
		e.anim = r.f32();
		if (!r.ok())
			break;
		PendingZone& p = g_zone_pending[e.id]; // latest state wins
		p.e = e;
		p.received = now;
	}
}

// ---------------------------------------------------------------- host: requests

CGameObject* proxy_go_of(PeerId peer, u16& proxy)
{
	proxy = join::proxy_of_peer(peer);
	return proxy == kNoId ? nullptr : find_go(proxy);
}

bool in_reach(CGameObject* proxy, const Fvector& p, float reach)
{
	return proxy && proxy->Position().distance_to(p) <= reach;
}

void host_on_use(PeerId from, Reader& r)
{
	const u16 id = r.u16();
	if (!r.ok())
		return;
	u16 proxy;
	CGameObject* who = proxy_go_of(from, proxy);
	CGameObject* go = find_go(id);
	if (!who || !go || !smart_cast<CPhysicObject*>(go))
		return;
	if (!in_reach(who, go->Position(), kMaxReach + go->Radius()))
	{
		::Msg("! [coop] use of [%s] by #%u refused: too far", go->cName().c_str(), from);
		return;
	}
	go->use(who); // the use callback runs with the player's character as `who`
	++g_c.uses;
}

void host_on_push(PeerId from, Reader& r)
{
	const u16 id = r.u16();
	const u8 elem = r.u8();
	Fvector dir = get_vec(r);
	const float speed = r.f32();
	if (!r.ok() || !_valid(dir) || !_valid(speed))
		return; // malformed
	u16 proxy;
	CGameObject* who = proxy_go_of(from, proxy);
	CGameObject* go = find_go(id);
	if (!who || !go || classify(go) != ObjKind::Body)
		return;
	CPhysicsShell* shell = go->cast_physics_shell_holder()->PPhysicsShell();
	if (elem >= shell->get_ElementsNumber())
		return;
	CPhysicsElement* e = shell->get_ElementByStoreOrder(elem);
	if (!e || !e->isActive() || !in_reach(who, e->XFORM().c, kMaxReach))
		return;
	u64& last = g_last_push[(u32(from) << 16) | id];
	const u64 now = now_ms();
	if (now - last < kPushIntervalMs / 2)
		return;
	last = now;
	dir.y = 0.f;
	if (dir.magnitude() < EPS_L)
		return;
	dir.normalize();
	const float v = clampr(speed, 0.f, 6.f);
	// a walking character (~80 kg) leaning into the body, never faster than the character itself
	const float impulse = _min(80.f * v * 0.3f, e->getMass() * v);
	shell->Enable();
	e->applyImpulse(dir, impulse);
	++g_c.pushes;
}

// Host: remove a server entity the normal way. An online object goes through GE_DESTROY so its game
// object goes too (as alife():release does); ALife release alone would leave it standing.
void host_destroy_entity(CSE_Abstract* e)
{
	if (!e)
		return;
	CSE_ALifeObject* o = smart_cast<CSE_ALifeObject*>(e);
	if (o && !o->m_bOnline && ai().get_alife() && ai().alife().objects().object(e->ID, true))
	{
		alife_rw().release(e, true);
		return;
	}
	NET_Packet P;
	P.w_begin(M_EVENT);
	P.w_u32(Level().timeServer());
	P.w_u16(GE_DESTROY);
	P.w_u16(e->ID);
	Level().Send(P, net_flags(TRUE, TRUE));
}

u32 fuse_ms(LPCSTR section, bool bolt)
{
	const u32 t = READ_IF_EXISTS(pSettings, r_u32, section, "destroy_time", 3000);
	return bolt ? u32(float(t) / (phTimefactor > EPS_S ? phTimefactor : 1.f)) : t;
}

void host_on_throw(PeerId from, Reader& r)
{
	const u16 hand = r.u16();
	PendingThrow t;
	t.pos = get_vec(r);
	t.dir = get_vec(r);
	t.vel = get_vec(r);
	t.flags = r.u8();
	// Malformed: nothing from the wire reaches the physics world unchecked (R3: through coop::validate). The
	// direction is a unit vector: a huge one normalizes to zero and the throw matrix to NaN.
	if (!r.ok() || !validate::vec_finite(t.pos) || !validate::vec_speed_ok(t.dir, validate::kMaxDirLength) ||
	    !validate::vec_finite(t.vel))
	{
		if (r.ok() && ++g_c.throws_malformed <= 10)
			::Msg("! [coop] throw of item %u by #%u refused: a vector is not a number", hand, from);
		return;
	}
	auto refuse = [&](const char* why)
	{
		++g_c.throws_refused;
		::Msg("! [coop] throw of item %u by #%u refused: %s", hand, from, why);
	};
	u16 proxy;
	CGameObject* who = proxy_go_of(from, proxy);
	if (!who)
		return refuse("no character");
	const u64 now = now_ms();
	// R3 В7: the missile is taken from the character's own inventory on the host, and the host decides what the
	// throw uses up: a grenade (any missile but a bolt) always leaves the inventory, the client's "consume" flag is
	// ignored (a client that keeps it loses it after kConsumeAfterMs). A bolt follows the thrower's limited-bolts
	// option (CBolt__State runs on its machine): a bolt that stays is what stock gives anyway.
	CSE_Abstract* he = events::find_entity(hand);
	if (!he || he->ID_Parent != proxy)
		return refuse("not in the player's inventory");
	LPCSTR section = he->s_name.c_str();
	if (!pSettings->line_exist(section, "destroy_time") || !pSettings->line_exist(section, "throw_point"))
		return refuse("not a missile");
	if (!smart_cast<CSE_ALifeItemBolt*>(he))
	{
		if (!(t.flags & TfConsume) && ++g_c.consume_forced <= 10)
			::Msg("! [coop] throw of [%s] %u by #%u: the client keeps it, the host uses it up", section, hand, from);
		t.flags |= TfConsume;
	}
	if ((t.flags & TfConsume) && g_consume.count(hand))
		return refuse("already thrown"); // a grenade leaves the hand once
	u64& last = g_last_throw[from];
	if (now - last < 150)
		return refuse("rate");
	if (!in_reach(who, t.pos, 4.f))
		t.pos.set(who->Position()).y += 1.5f;
	if (!validate::vec_speed_ok(t.vel, kMaxThrowSpeed))
		t.vel.normalize_safe().mul(kMaxThrowSpeed);
	if (t.dir.magnitude() < EPS_L)
		t.dir.set(0.f, 0.f, 1.f);
	t.dir.normalize();
	CSE_ALifeDynamicObject* pe = ai().alife().objects().object(proxy, true);
	if (!pe)
		return refuse("character offline");
	CSE_Abstract* abstract = alife_rw().spawn_item(section, t.pos, pe->m_tNodeID, pe->m_tGraphID, kNoId);
	CSE_ALifeDynamicObject* e = smart_cast<CSE_ALifeDynamicObject*>(abstract);
	if (!e)
		return refuse("spawn failed");
	e->m_flags.set(CSE_ALifeObject::flCanSave, FALSE); // a projectile is never part of a save (no dupes)
	if (!e->m_bOnline)
		alife_rw().switch_online(e);
	last = now;
	t.peer = from;
	t.proxy = proxy;
	t.created = now;
	g_pending_throws[e->ID] = t;
	if (t.flags & TfConsume)
	{
		Consume& c = g_consume[hand];
		c.proxy = proxy;
		c.deadline = now + kConsumeAfterMs;
		c.entity = he;
		c.section = he->s_name;
	}
	++g_c.throws_ok;
}

void host_on_activate(PeerId from, Reader& r)
{
	const u16 id = r.u16();
	if (!r.ok())
		return;
	u16 proxy;
	CGameObject* who = proxy_go_of(from, proxy);
	CArtefact* af = smart_cast<CArtefact*>(find_go(id));
	if (!who || !af || !af->H_Parent() || events::root_of(id) != proxy)
		return;
	af->ActivateArtefact();
	++g_c.activations;
}

void host_on_request(PeerId from, Reader& r)
{
	if (!mgr().is_host() || !world_up() || sync::host_peer_joining(from)) // level-change review: second line behind the session's world gate
		return;
	switch (RequestKind(r.u8()))
	{
	case RequestKind::Use: host_on_use(from, r);
		break;
	case RequestKind::Push: host_on_push(from, r);
		break;
	case RequestKind::Throw: host_on_throw(from, r);
		break;
	case RequestKind::Activate: host_on_activate(from, r);
		break;
	default:
		break;
	}
}

// ---------------------------------------------------------------- host: frame

void host_apply_throws()
{
	const u64 now = now_ms();
	for (auto it = g_pending_throws.begin(); it != g_pending_throws.end();)
	{
		const PendingThrow& t = it->second;
		CMissile* m = smart_cast<CMissile*>(find_go(it->first));
		CPhysicsShell* shell = m ? m->PPhysicsShell() : nullptr;
		if (!shell || !shell->isActive())
		{
			if (now - t.created > kThrowApplyTimeoutMs)
			{
				host_destroy_entity(events::find_entity(it->first));
				::Msg("! [coop] thrown projectile %u never came online, removed", it->first);
				it = g_pending_throws.erase(it);
			}
			else
				++it;
			continue;
		}
		Fmatrix xf;
		xf.identity();
		xf.k.set(t.dir);
		Fvector::generate_orthonormal_basis(xf.k, xf.j, xf.i);
		xf.c.set(t.pos);
		Fvector spin;
		if (t.flags & TfRandomSpin)
		{
			const float fi = ::Random.randF(0.f, 2.f * PI), teta = ::Random.randF(0.f, PI),
			            rr = ::Random.randF(2.f * PI, 3.f * PI);
			const float rxy = rr * _sin(teta);
			spin.set(rxy * _cos(fi), rxy * _sin(fi), rr * _cos(teta));
		}
		else
			spin.set(0.f, 0.f, 0.f);
		m->XFORM().set(xf);
		shell->SetGlTransformDynamic(xf);
		shell->Enable();
		shell->set_LinearVel(t.vel);
		shell->set_AngularVel(spin);
		shell->SetAirResistance(0.f, 0.f);
		shell->set_DynamicScales(1.f, 1.f);
		shell->SetAllGeomTraced();
		CGameObject* thrower = find_go(t.proxy);
		if (CPhysicsShellHolder* holder = thrower ? thrower->cast_physics_shell_holder() : nullptr)
		{
			shell->add_ObjectContactCallback(CMissile::ExitContactCallback);
			shell->set_CallbackData(holder);
		}
		const bool bolt = smart_cast<CBolt*>(m) != nullptr;
		m->set_destroy_time(fuse_ms(m->cNameSect().c_str(), bolt));
		if (CBolt* b = smart_cast<CBolt*>(m))
		{
			b->SetInitiator(t.proxy);
			b->SetCanTake((t.flags & TfCanTake) ? TRUE : FALSE);
		}
		else if (CExplosive* ex = m->cast_explosive())
			ex->SetInitiator(t.proxy);
		m->processing_activate();
		++g_c.projectiles;
		it = g_pending_throws.erase(it);
	}
	for (auto it = g_consume.begin(); it != g_consume.end();)
	{
		if (now < it->second.deadline)
		{
			++it;
			continue;
		}
		// R3 В7: wherever the thrown item went meanwhile (dropped, stashed, handed over), it was thrown.
		CSE_Abstract* e = events::find_entity(it->first);
		if (e && e == it->second.entity && e->s_name == it->second.section)
		{
			host_destroy_entity(e); // the owner never removed the thrown item itself
			++g_c.consumed;
		}
		it = g_consume.erase(it);
	}
}

void host_unsave_projectiles()
{
	xr_vector<std::pair<u16, u64>> list;
	{
		xrCriticalSectionGuard guard(g_lock);
		list.swap(g_ev_projectiles);
	}
	const u64 now = now_ms();
	for (const auto& p : list)
	{
		CSE_ALifeDynamicObject* e = ai().get_alife() ? ai().alife().objects().object(p.first, true) : nullptr;
		if (e)
		{
			e->m_flags.set(CSE_ALifeObject::flCanSave, FALSE); // replicated, but never saved (no dupes)
			++g_c.projectiles;
		}
		else if (now - p.second < 2000)
		{
			xrCriticalSectionGuard guard(g_lock);
			g_ev_projectiles.push_back(p); // ALife has not taken it yet
		}
	}
}

void host_flush_events()
{
	xr_vector<ZoneEntry> zones;
	xr_vector<AnimEntry> anims;
	xr_vector<DestroyedEntry> destroyed;
	xr_vector<u16> broken;
	xr_vector<ExplodeEntry> explode;
	xr_vector<u16> fragments;
	{
		xrCriticalSectionGuard guard(g_lock);
		zones.swap(g_ev_zones);
		anims.swap(g_ev_anims);
		destroyed.swap(g_ev_destroyed);
		broken.swap(g_ev_broken);
		explode.swap(g_ev_explode);
		fragments.swap(g_ev_fragments);
	}
	if (!zones.empty())
	{
		Batcher b(Msg::ZoneState, u8(ZoneKind::Zones), false, Lane::Reliable, kInvalidPeer, true);
		for (const ZoneEntry& e : zones)
		{
			Writer w(16);
			write_zone(w, e);
			b.add(w);
		}
		b.flush();
		g_c.zone_msgs += b.packets();
	}
	if (!anims.empty())
	{
		Batcher b(Msg::EntityState, u8(StateKind::Anim), false, Lane::Reliable, kInvalidPeer);
		for (const AnimEntry& a : anims)
		{
			Writer w(16);
			write_anim(w, a);
			b.add(w);
		}
		b.flush();
		g_c.anim_msgs += b.packets();
	}
	for (const DestroyedEntry& d : destroyed)
	{
		Writer w(48);
		w.u8(u8(StateKind::Destroyed));
		w.u16(d.id);
		w.u16(d.bone);
		w.u16(d.hit_type);
		put_vec(w, d.dir);
		w.f32(d.impulse);
		put_vec(w, d.p_in_bone);
		net::host_broadcast(Msg::EntityState, w);
		++g_c.destroyed_sent;
	}
	for (u16 id : broken)
	{
		Writer w(4);
		w.u8(u8(StateKind::Broken));
		w.u16(id);
		net::host_broadcast(Msg::EntityState, w);
		++g_c.broken_sent;
	}
	for (const ExplodeEntry& x : explode)
	{
		Writer w(64);
		w.u8(u8(StateKind::Explode));
		w.u16(x.id);
		w.u16(x.initiator);
		put_vec(w, x.pos);
		put_vec(w, x.normal);
		w.str(x.section.c_str() ? x.section.c_str() : "");
		net::host_broadcast(Msg::EntityState, w);
		++g_c.explode_sent;
	}
	for (u16 id : fragments)
	{
		if (ai().get_alife() && ai().alife().objects().object(id, true))
			continue; // ALife object: coop_entities already sends it
		if (CSE_Abstract* e = events::find_entity(id))
		{
			events::host_relay_local_spawn(e);
			++g_c.fragments_relayed;
		}
	}
}

void host_stream()
{
	const u64 now = now_ms();
	if (now - g_last_stream < kStreamIntervalMs)
		return;
	g_last_stream = now;
	Batcher samples(Msg::EntityState, u8(StateKind::Samples), true, Lane::Unreliable, kInvalidPeer);
	Batcher rests(Msg::EntityState, u8(StateKind::Rest), true, Lane::Reliable, kInvalidPeer);
	xr_vector<Pose> poses;
	for (u32 i = 0, n = Level().Objects.o_count(); i < n; ++i)
	{
		CGameObject* go = smart_cast<CGameObject*>(Level().Objects.o_get_by_iterator(i));
		if (!go || go->getDestroy() || go->H_Parent())
			continue;
		// cheap filter first: a sleeping body that never moved, or a shell holder without a shell that
		// is not a helicopter, needs nothing
		CPhysicsShellHolder* holder = go->cast_physics_shell_holder();
		if (!holder)
			continue;
		CPhysicsShell* ph = holder->PPhysicsShell();
		const bool heli = smart_cast<CHelicopter*>(go) != nullptr;
		if (ph && !ph->isEnabled() && !heli)
		{
			auto hb = g_host_bodies.find(go->ID());
			if (hb == g_host_bodies.end() || !hb->second.moving)
				continue; // asleep, and its rest pose went out already
		}
		else if (!ph && !heli)
			continue;
		const ObjKind kind = classify(go);
		if (kind == ObjKind::Pose)
		{
			HostPose& hp = g_host_poses[go->ID()];
			Fquaternion q;
			q.set(go->XFORM());
			const bool moved = hp.p.distance_to(go->Position()) > 0.02f || _abs(hp.q.w - q.w) > 1e-4f ||
				_abs(hp.q.x - q.x) > 1e-4f || _abs(hp.q.y - q.y) > 1e-4f || _abs(hp.q.z - q.z) > 1e-4f;
			if (!moved && now - hp.sent < 1000)
				continue;
			hp.p = go->Position();
			hp.q = q;
			hp.sent = now;
			Writer w(32);
			w.u16(go->ID());
			w.u8(SfPoseOnly | SfMoving);
			w.u8(1);
			put_vec(w, hp.p);
			put_quat(w, hp.q);
			samples.add(w);
			++g_c.sample_entries;
			continue;
		}
		if (kind != ObjKind::Body)
			continue;
		CPhysicsShellHolder* sh = go->cast_physics_shell_holder();
		const bool moving = sh->PPhysicsShell()->isEnabled();
		auto it = g_host_bodies.find(go->ID());
		if (moving)
		{
			if (!capture(sh, poses))
				continue;
			HostBody& hb = it == g_host_bodies.end() ? g_host_bodies[go->ID()] : it->second;
			hb.moving = true;
			hb.last_move = now;
			Writer w(16 + poses.size() * 32);
			write_body(w, go->ID(), SfMoving, poses, true);
			samples.add(w);
			++g_c.sample_entries;
		}
		else if (it != g_host_bodies.end() && it->second.moving)
		{
			it->second.moving = false;
			if (!capture(sh, poses))
				continue;
			Writer w(16 + poses.size() * 20);
			w.u16(go->ID());
			w.u32(hash_str(go->cNameSect().c_str()));
			w.u8(u8(poses.size()));
			for (const Pose& p : poses)
			{
				put_vec(w, p.p);
				put_quat(w, p.q);
			}
			rests.add(w);
			++g_c.rests_sent;
		}
	}
	samples.flush();
	rests.flush();
	g_c.sample_packets += samples.packets();

	if (now - g_last_cleanup > kCleanupIntervalMs)
	{
		g_last_cleanup = now;
		for (auto it = g_host_bodies.begin(); it != g_host_bodies.end();)
			it = find_go(it->first) ? std::next(it) : g_host_bodies.erase(it);
		for (auto it = g_host_poses.begin(); it != g_host_poses.end();)
			it = find_go(it->first) ? std::next(it) : g_host_poses.erase(it);
		for (auto it = g_anim_host.begin(); it != g_anim_host.end();)
		{
			if (find_go(it->first))
				++it;
			else
			{
				g_anim_host_ms.erase(it->first);
				it = g_anim_host.erase(it);
			}
		}
		xrCriticalSectionGuard guard(g_lock);
		for (auto it = g_zone_switch_ms.begin(); it != g_zone_switch_ms.end();)
			it = find_go(it->first) ? std::next(it) : g_zone_switch_ms.erase(it);
		for (auto it = g_torrid_host.begin(); it != g_torrid_host.end();)
			it = find_go(it->first) ? std::next(it) : g_torrid_host.erase(it);
	}
}

ZoneEntry zone_entry_now(CCustomZone* zone, u64 now)
{
	ZoneEntry e;
	e.id = zone->ID();
	e.state = u8(zone->ZoneState());
	e.campfire = campfire_flag(zone);
	xrCriticalSectionGuard guard(g_lock);
	auto sw = g_zone_switch_ms.find(e.id);
	if (sw != g_zone_switch_ms.end() && now > sw->second)
		e.elapsed = u32(_min<u64>(now - sw->second, 0xffffffffull));
	auto ph = g_torrid_host.find(e.id);
	if (ph != g_torrid_host.end())
		e.anim = ph->second.time;
	return e;
}

// Animator phases of moving zones drift with frame timing: refresh them now and then (P5).
void host_resend_torrid()
{
	const u64 now = now_ms();
	if (now - g_last_torrid < kTorridResendMs)
		return;
	g_last_torrid = now;
	xr_vector<u16> ids;
	{
		xrCriticalSectionGuard guard(g_lock);
		for (const auto& it : g_torrid_host)
			ids.push_back(it.first);
	}
	if (ids.empty())
		return;
	Batcher b(Msg::ZoneState, u8(ZoneKind::Zones), false, Lane::Reliable, kInvalidPeer, true);
	for (u16 id : ids)
	{
		CCustomZone* zone = smart_cast<CCustomZone*>(find_go(id));
		if (!zone)
			continue;
		Writer w(16);
		write_zone(w, zone_entry_now(zone, now));
		b.add(w);
	}
	b.flush();
	g_c.zone_msgs += b.packets();
}

void host_frame()
{
	host_flush_events();
	host_unsave_projectiles();
	host_apply_throws();
	host_stream();
	host_resend_torrid();
}

void client_frame()
{
	client_scan();
	client_apply_zones();
	client_apply_bodies();
	client_apply_events();
	client_push_check();
}
} // namespace

// ---------------------------------------------------------------- API

void install_server(Session& s)
{
	s.on(Msg::EntityEvent, [](PeerId from, Reader& r) { host_on_request(from, r); });
}

void install_client(Endpoint& e)
{
	e.on(Msg::EntityState, [](PeerId from, Reader& r) { client_on_state(from, r); });
	e.on(Msg::ZoneState, [](PeerId from, Reader& r) { client_on_zones(from, r); });
}

void frame()
{
	if (!g_pGameLevel || !ai().get_alife())
		return;
	if (mgr().is_host())
		host_frame();
	else if (mgr().is_live_client())
		client_frame();
}

void reset()
{
	if (g_pGameLevel)
		release_fixed();
	g_fixed.clear();
	{
		xrCriticalSectionGuard guard(g_lock);
		g_ev_zones.clear();
		g_ev_anims.clear();
		g_ev_destroyed.clear();
		g_ev_broken.clear();
		g_ev_explode.clear();
		g_ev_fragments.clear();
		g_ev_projectiles.clear();
		g_zone_switch_ms.clear();
		g_torrid_host.clear();
	}
	g_anim_host.clear();
	g_anim_host_ms.clear();
	g_host_bodies.clear();
	g_host_poses.clear();
	g_pending_throws.clear();
	g_consume.clear();
	g_last_throw.clear();
	g_last_push.clear();
	g_bodies.clear();
	g_zone_pending.clear();
	g_torrid_client.clear();
	g_anim_client.clear();
	g_destroyed_pending.clear();
	g_broken_pending.clear();
	g_explode_pending.clear();
	g_push_sent.clear();
	for (auto& it : g_explode_sounds)
		xr_delete(it.second);
	g_explode_sounds.clear();
	g_have_offset = false;
	g_clock_offset = 0;
	g_last_stream = g_last_cleanup = g_last_torrid = g_last_scan = g_last_push_check = 0;
	g_zone_apply = 0;
	g_zone_apply_id = kNoId;
	g_hit_apply = 0;
}

void host_on_peer_ready(PeerId id)
{
	if (!mgr().is_host() || !world_up()) // level-change review: it walks Level().Objects, which a level in the making has not filled
		return;
	const u64 now = now_ms();
	u32 zones = 0, bodies = 0, anims = 0;
	{
		Batcher b(Msg::ZoneState, u8(ZoneKind::Zones), false, Lane::Reliable, id, true);
		for (u32 i = 0, n = Level().Objects.o_count(); i < n; ++i)
		{
			CCustomZone* zone = smart_cast<CCustomZone*>(Level().Objects.o_get_by_iterator(i));
			if (!zone || zone->getDestroy())
				continue;
			Writer w(16);
			write_zone(w, zone_entry_now(zone, now));
			b.add(w);
			++zones;
		}
	}
	{
		Batcher b(Msg::EntityState, u8(StateKind::Rest), true, Lane::Reliable, id);
		xr_vector<Pose> poses;
		for (const auto& it : g_host_bodies)
		{
			CGameObject* go = find_go(it.first);
			if (!go || classify(go) != ObjKind::Body || !capture(go->cast_physics_shell_holder(), poses))
				continue;
			Writer w(16 + poses.size() * 20);
			w.u16(go->ID());
			w.u32(hash_str(go->cNameSect().c_str()));
			w.u8(u8(poses.size()));
			for (const Pose& p : poses)
			{
				put_vec(w, p.p);
				put_quat(w, p.q);
			}
			b.add(w);
			++bodies;
		}
	}
	{
		Batcher b(Msg::EntityState, u8(StateKind::Anim), false, Lane::Reliable, id);
		for (const auto& it : g_anim_host)
		{
			if (!find_go(it.first))
				continue;
			AnimEntry a = it.second;
			auto t = g_anim_host_ms.find(it.first);
			if (a.playing && t != g_anim_host_ms.end())
				a.time += a.speed * float(now - t->second) / 1000.f;
			Writer w(16);
			write_anim(w, a);
			b.add(w);
			++anims;
		}
	}
	::Msg("[coop] objects -> #%u: %u zone states, %u moved bodies, %u animated objects", id, zones, bodies, anims);
}

bool is_host_driven(const CGameObject* obj)
{
	return obj && mgr().is_client() && g_fixed.count(obj->ID()) != 0;
}

void print_status()
{
	if (mgr().is_host())
	{
		::Msg("[coop]   objects out: %llu sample packets (%llu entries), %llu rests, %llu zone msgs, %llu anim msgs, "
		    "%u moved bodies",
		    g_c.sample_packets, g_c.sample_entries, g_c.rests_sent, g_c.zone_msgs, g_c.anim_msgs,
		    (u32)g_host_bodies.size());
		::Msg("[coop]   objects out: %llu explosions, %llu destroyed, %llu broken, %llu fragments relayed, "
		    "%llu zone hits on players dropped",
		    g_c.explode_sent, g_c.destroyed_sent, g_c.broken_sent, g_c.fragments_relayed, g_c.zone_hits_dropped);
		::Msg("[coop]   objects req: %llu uses, %llu pushes, %llu throws ok / %llu refused / %llu malformed, "
		    "%llu projectiles, %llu consumed (%llu kept by the client), %llu artefact activations, %u throws pending",
		    g_c.uses, g_c.pushes, g_c.throws_ok, g_c.throws_refused, g_c.throws_malformed, g_c.projectiles,
		    g_c.consumed, g_c.consume_forced, g_c.activations, (u32)g_pending_throws.size());
	}
	else if (mgr().is_client())
	{
		::Msg("[coop]   objects in: %u bodies tracked, %u kinematic (%llu elements fixed), %llu samples, %llu rests "
		    "(%llu applied), %llu poses applied, %llu shape mismatches, %llu forced online",
		    (u32)g_bodies.size(), (u32)g_fixed.size(), g_fixed_elements, g_c.samples_in, g_c.rests_in,
		    g_c.rests_applied, g_c.poses_applied, g_c.mismatch, g_c.forced_online);
		::Msg("[coop]   objects in: %llu zone states applied (%u pending), %llu explosions (+%llu fallback), "
		    "%llu destroyed, %llu broken, %llu local detonations / %llu zone switches blocked",
		    g_c.zones_applied, (u32)g_zone_pending.size(), g_c.explode_applied, g_c.explode_fallback,
		    g_c.destroyed_applied, g_c.broken_applied, g_c.explode_blocked, g_c.switches_blocked);
		::Msg("[coop]   objects req: %llu uses (%llu dropped as story echo), %llu pushes, %llu throws, "
		    "%llu artefact activations sent; %llu sleeping bags and %llu tents used here",
		    g_c.uses_sent, g_c.uses_echo_dropped, g_c.pushes_sent, g_c.throws_sent, g_c.activations_sent,
		    g_c.sleeping_bags_used, g_c.tents_used);
		::Msg("[coop]   objects in: %llu state / zone messages dropped before live (R2 I2)", g_c.dropped_before_live);
	}
}

// ---------------------------------------------------------------- hooks: physics objects

void physic_anim(CGameObject* obj, CBlend* blend)
{
	if (!obj || !blend || !mgr().active())
		return;
	const u64 now = now_ms();
	const u16 id = obj->ID();
	if (mgr().is_host())
	{
		AnimEntry cur;
		cur.id = id;
		cur.playing = !!blend->playing;
		cur.speed = blend->speed;
		cur.time = blend->timeCurrent;
		auto it = g_anim_host.find(id);
		bool changed = it == g_anim_host.end();
		if (!changed)
		{
			const AnimEntry& last = it->second;
			float expected = last.time;
			if (last.playing)
				expected += last.speed * float(now - g_anim_host_ms[id]) / 1000.f;
			changed = last.playing != cur.playing || _abs(last.speed - cur.speed) > 1e-3f ||
				_abs(expected - cur.time) > (cur.playing ? 0.3f : 0.01f);
		}
		else if (!cur.playing && cur.time < EPS_L)
		{
			g_anim_host[id] = cur; // the startup pose: nothing to tell anybody
			g_anim_host_ms[id] = now;
			return;
		}
		if (!changed)
			return;
		g_anim_host[id] = cur;
		g_anim_host_ms[id] = now;
		xrCriticalSectionGuard guard(g_lock);
		g_ev_anims.push_back(cur);
		return;
	}
	if (!mgr().is_client())
		return;
	auto it = g_anim_client.find(id);
	if (it == g_anim_client.end())
		return;
	const AnimEntry& a = it->second.a;
	float expected = a.time;
	if (a.playing)
		expected += a.speed * float(now - it->second.received) / 1000.f;
	expected = clampr(expected, 0.f, blend->timeTotal);
	if (!!blend->playing != a.playing)
	{
		blend->playing = a.playing ? TRUE : FALSE;
		if (a.playing)
			blend->stop_at_end_callback = TRUE;
	}
	if (_abs(blend->speed - a.speed) > 1e-3f)
		blend->speed = a.speed;
	if (_abs(blend->timeCurrent - expected) > 0.25f)
		blend->timeCurrent = expected;
}

bool client_intercept_use(CGameObject* obj, CGameObject* who)
{
	if (!obj || !who || !mgr().is_client() || entities::client_applying())
		return false;
	if (who->ID() != kActor || !smart_cast<CPhysicObject*>(obj) || obj->H_Parent())
		return false;
	// Live 2026-10-25: a client could not sleep on a sleeping bag ("F does not press"). The stock sleep is the user's own
	// window (item_tent physic_object_on_use_callback -> ui_sleep_dialog.sleep_bag), which a Use sent to the host never
	// opens here. A sleeping bag's use stays on this machine: Lua opens the window, the host decides the sleep (coop_rest:
	// the place, one sleeper per bag). A tent's too: F on his own tent asks the host to pack it up (coop_tent_guard).
	const bool bag = checkpoints::sources::is_sleeping_bag(obj->cNameSect());
	if (bag || checkpoints::sources::is_tent(obj->cNameSect()))
	{
		::luabind::functor<void> use_here;
		if (ai().script_engine().functor(bag ? "coop_rest.use_sleep_place" : "coop_tent_guard.use_tent", use_here))
		{
			use_here(obj->lua_game_object());
			++(bag ? g_c.sleeping_bags_used : g_c.tents_used);
		}
		return true;
	}
	// the host runs the same story reaction on its side: a second Use from us would toggle the object twice
	if (rules::client_echo(story::applying(), false, false) == rules::Echo::Drop)
	{
		++g_c.uses_echo_dropped;
		return true;
	}
	if (mgr().is_live_client())
	{
		Writer w(4);
		w.u8(u8(RequestKind::Use));
		w.u16(join::to_host(obj->ID()));
		net::client_send(Msg::EntityEvent, w);
		++g_c.uses_sent;
	}
	return true;
}

void client_usable_tip(CUsableScriptObject* usable, CGameObject* obj)
{
	if (!usable || !obj || !mgr().is_client() || usable->tip_text())
		return;
	if (checkpoints::sources::is_sleeping_bag(obj->cNameSect()))
		usable->set_tip_text("st_sleep"); // the stock ph_sleepbag.ltx tip
}

bool client_drive_pose(CGameObject* obj)
{
	if (!obj || !mgr().is_live_client())
		return false;
	auto it = g_bodies.find(obj->ID());
	if (it == g_bodies.end() || !it->second.pose_only || it->second.samples.empty())
		return false;
	const u64 now = now_ms();
	xr_vector<Pose> poses;
	if (!sample_at(it->second, now > kInterpDelayMs ? now - kInterpDelayMs : 0, poses) || poses.empty())
		return false;
	Fmatrix m;
	m.rotation(poses[0].q);
	m.c.set(poses[0].p);
	obj->XFORM().set(m);
	return true;
}

// ---------------------------------------------------------------- hooks: destroyables and breakables

bool world_hit_blocked(CGameObject* obj)
{
	// WP2 forwards our own hits as claims and swallows the rest; this drops direct local calls too
	// (collision damage). The host's destruction is replayed through g_hit_apply.
	return obj && mgr().is_client() && g_hit_apply == 0;
}

bool ph_destroy(CGameObject* obj, const SHit& fatal_hit)
{
	if (!obj)
		return false;
	if (mgr().is_client())
		return true; // fragments are host entities; they arrive through WP2's EntityLocal relay
	if (!mgr().is_host() || !smart_cast<CDestroyablePhysicsObject*>(obj))
		return false;
	DestroyedEntry d;
	d.id = obj->ID();
	d.bone = fatal_hit.boneID;
	d.hit_type = u16(fatal_hit.hit_type);
	d.dir = fatal_hit.dir;
	d.impulse = fatal_hit.impulse;
	d.p_in_bone = fatal_hit.p_in_bone_space;
	xrCriticalSectionGuard guard(g_lock);
	g_ev_destroyed.push_back(d);
	return false;
}

bool fragment_from_host(CGameObject* source, CGameObject* part)
{
	if (!source || !part)
		return false;
	if (mgr().is_client())
		return true;
	if (mgr().is_host())
	{
		xrCriticalSectionGuard guard(g_lock);
		g_ev_fragments.push_back(part->ID());
	}
	return false;
}

void on_break(CGameObject* obj)
{
	if (!obj || !mgr().is_host())
		return;
	xrCriticalSectionGuard guard(g_lock);
	g_ev_broken.push_back(obj->ID());
}

bool host_destroys(CGameObject* obj) { return obj && mgr().is_client(); }

// ---------------------------------------------------------------- hooks: zones

bool zone_switch_blocked(CCustomZone* zone)
{
	if (!zone || !mgr().is_client() || g_zone_apply > 0)
		return false;
	++g_c.switches_blocked;
	return true;
}

int on_zone_state(CCustomZone* zone, u8 new_state)
{
	if (!zone)
		return 0;
	if (mgr().is_host())
	{
		ZoneEntry e;
		e.id = zone->ID();
		e.state = new_state;
		e.campfire = campfire_flag(zone);
		xrCriticalSectionGuard guard(g_lock);
		g_zone_switch_ms[e.id] = now_ms();
		auto ph = g_torrid_host.find(e.id);
		if (ph != g_torrid_host.end())
			e.anim = ph->second.time;
		g_ev_zones.push_back(e);
		return 0;
	}
	if (mgr().is_client() && g_zone_apply > 0 && g_zone_apply_id == zone->ID())
		return int(_min<u32>(g_zone_apply_elapsed, 0x7fffffff));
	return 0;
}

bool zone_hit_blocked(CCustomZone* zone, u16 target_id)
{
	if (!zone || !mgr().active())
		return false;
	if (mgr().is_client())
		return target_id != kActor; // the local actor's own anomaly damage; the host hurts the rest
	if (target_id != kActor && world::is_player_proxy(target_id))
	{
		++g_c.zone_hits_dropped; // that player's own game computes it
		return true;
	}
	return false;
}

bool campfire_turn_blocked(CGameObject* campfire, bool on)
{
	if (!campfire)
		return false;
	if (mgr().is_client())
		return g_zone_apply == 0; // the host lights and puts out fires
	if (mgr().is_host())
	{
		ZoneEntry e;
		e.id = campfire->ID();
		CCustomZone* zone = smart_cast<CCustomZone*>(campfire);
		e.state = zone ? u8(zone->ZoneState()) : u8(0);
		e.campfire = on ? 1 : 0;
		xrCriticalSectionGuard guard(g_lock);
		g_ev_zones.push_back(e);
	}
	return false;
}

float zone_anim_dt(CCustomZone* zone, const SAnimParams& anim, float dt)
{
	if (!zone || !mgr().active())
		return dt;
	const float length = anim.max_t - anim.min_t;
	const float local = anim.t_current - anim.min_t;
	if (mgr().is_host())
	{
		xrCriticalSectionGuard guard(g_lock);
		TorridPhase& p = g_torrid_host[zone->ID()];
		p.time = local;
		p.length = length;
		return dt;
	}
	if (!mgr().is_client() || length <= EPS_L)
		return dt;
	auto it = g_torrid_client.find(zone->ID());
	if (it == g_torrid_client.end())
		return dt;
	float host = it->second.time + float(now_ms() - it->second.received) / 1000.f;
	host = fmodf(host, length);
	float diff = host - local;
	if (diff > length * 0.5f)
		diff -= length;
	else if (diff < -length * 0.5f)
		diff += length;
	if (_abs(diff) > 0.5f)
		return _max(dt + diff, 0.f); // jump onto the host's phase
	return _max(dt + diff * 0.1f, 0.f); // drift towards it
}

// ---------------------------------------------------------------- hooks: artefacts

bool client_forward_artefact_activation(CGameObject* artefact)
{
	if (!artefact || !mgr().is_client() || entities::client_applying())
		return false;
	if (mgr().is_live_client())
	{
		Writer w(4);
		w.u8(u8(RequestKind::Activate));
		w.u16(join::to_host(artefact->ID()));
		net::client_send(Msg::EntityEvent, w);
		++g_c.activations_sent;
	}
	return true;
}

// ---------------------------------------------------------------- hooks: missiles and explosions

bool skip_fake_missile(CGameObject* missile) { return missile && mgr().is_client(); }

void host_mark_projectile(CSE_Abstract* projectile)
{
	if (!projectile || !mgr().is_host())
		return;
	// The thrown copy becomes an ALife object when it leaves the hand (game_sv_Single::OnDetach), so
	// coop_entities replicates its spawn and release; on_projectile_free() clears the flag again so it
	// is never written to a save.
	if (CSE_ALifeObject* o = smart_cast<CSE_ALifeObject*>(projectile))
		o->m_flags.set(CSE_ALifeObject::flCanSave, TRUE);
}

void on_projectile_free(CGameObject* missile)
{
	if (!missile || !mgr().is_host() || !smart_cast<CMissile*>(missile->H_Parent()))
		return;
	xrCriticalSectionGuard guard(g_lock);
	g_ev_projectiles.emplace_back(missile->ID(), now_ms());
}

bool client_throw(CMissile* missile)
{
	if (!missile || !mgr().is_client() || entities::client_applying())
		return false;
	CActor* actor = smart_cast<CActor*>(missile->H_Parent());
	if (!actor || actor->ID() != kActor || !mgr().is_live_client())
		return false; // no fake missile here, so the engine throw stops by itself
	Fvector pos, dir;
	if (actor->inventory().ActiveItem() == static_cast<CInventoryItem*>(missile))
		actor->g_fireParams(missile, pos, dir);
	else
	{
		pos = missile->XFORM().c;
		dir = missile->XFORM().k;
	}
	dir.normalize_safe();
	float force;
	if (actor->use_default_throw_force())
		force = missile->IsConstPower() ? missile->GetConstForce() : missile->GetThrowForce();
	else
		force = actor->missile_throw_force();
	Fvector vel;
	vel.set(dir).mul(force);
	if (actor->character_physics_support() && actor->character_physics_support()->movement())
	{
		Fvector own;
		actor->character_physics_support()->movement()->GetCharacterVelocity(own);
		vel.add(own);
	}
	u8 flags = actor->use_throw_randomness() ? TfRandomSpin : 0;
	if (smart_cast<CBolt*>(missile))
	{
		// Anomaly's limited-bolts option: a counted bolt can be picked up again and leaves the inventory.
		::luabind::functor<bool> limited;
		if (ai().script_engine().functor("_G.CBolt__State", limited) && limited(actor->ID()))
			flags |= TfConsume | TfCanTake;
	}
	else
		flags |= TfConsume;
	Writer w(48);
	w.u8(u8(RequestKind::Throw));
	w.u16(join::to_host(missile->ID()));
	put_vec(w, pos);
	put_vec(w, dir);
	put_vec(w, vel);
	w.u8(flags);
	net::client_send(Msg::EntityEvent, w);
	++g_c.throws_sent;
	return true;
}

bool explode_blocked(CGameObject* explosive)
{
	if (!explosive || !mgr().is_client())
		return false;
	++g_c.explode_blocked;
	return true;
}

void on_explode(CGameObject* explosive, u16 initiator, const Fvector& pos, const Fvector& normal)
{
	if (!explosive || !mgr().is_host())
		return;
	ExplodeEntry x;
	x.id = explosive->ID();
	x.initiator = initiator;
	x.pos = pos;
	x.normal = normal;
	x.section = explosive->cNameSect();
	xrCriticalSectionGuard guard(g_lock);
	g_ev_explode.push_back(x);
}

bool explosion_visual_only(CGameObject* explosive) { return explosive && mgr().is_client(); }
} // namespace objects
} // namespace coop
