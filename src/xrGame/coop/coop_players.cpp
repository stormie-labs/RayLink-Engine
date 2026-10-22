#include "stdafx.h"
#include "coop_players.h"
#include "coop_manager.h"
#include "coop_join.h"
#include "coop_world.h"
#include "coop_survival.h"
#include "coop_entities.h"
#include "coop_sound.h"
#include "xrCoop/coop_validate.h"

#include "Level.h"
#include "Actor.h"
#include "ai/stalker/ai_stalker.h"
#include "Inventory.h"
#include "CharacterPhysicsSupport.h"
#include "PHMovementControl.h"
#include "../Include/xrRender/Kinematics.h"
#include "../Include/xrRender/KinematicsAnimated.h"

namespace coop
{
namespace players
{
namespace
{
constexpr u32 kSendIntervalMs = 50; // 20 Hz
constexpr u32 kInterpDelayMs = 100; // render this far in the past to hide jitter
constexpr size_t kMaxSamples = 32;
// W9: no state for this long = the stream ended (player left, avatar out of the world). The next state starts a
// new one: its clock may be another process's (the player restarted the game), and old samples are useless.
constexpr u64 kStreamGapMs = 3000;
// R3 K1: a peer's numbers reach XFORM, the proxy's physics and o_Position in the host save.
constexpr float kMaxSpeed = 100.f; // m/s; a sprinting actor is ~8, a vehicle well under this
constexpr float kLevelMargin = 50.f; // m around the level's geometry box (falling, jumping off the edge)

struct Sample
{
	State s;
	u64 local_time = 0; // sender time mapped to our clock
};

struct Track
{
	xr_deque<Sample> samples;
	s64 clock_offset = 0; // local - remote, min over recent samples
	bool have_offset = false;
	u32 last_remote_time = 0;
	u32 applied_legs = 0, applied_torso = 0, applied_head = 0;
	u64 received = 0, dropped_old = 0;
	u64 last_rx = 0; // our clock, last state received
	// W5: what the avatar was last driven with (read by coop_sound, coop_equip)
	bool driven = false;
	u32 mstate = 0;
	float view_yaw = 0.f, view_pitch = 0.f;
};

// Keyed by avatar id in local numbering.
xr_map<u16, Track> g_tracks;
u64 g_last_send = 0;
u64 g_sent = 0;

void push_sample(u16 avatar, const State& s)
{
	Track& t = g_tracks[avatar];
	++t.received;
	const u64 now = mgr().now_ms();
	if (t.have_offset && now - t.last_rx > kStreamGapMs)
	{
		t.samples.clear();
		t.have_offset = false;
		t.applied_legs = t.applied_torso = t.applied_head = 0; // a respawned avatar replays the motions
		t.driven = false;
		t.mstate = 0;
	}
	t.last_rx = now;
	if (t.have_offset && s32(s.time_ms - t.last_remote_time) <= 0)
	{
		++t.dropped_old; // reordered or duplicate: latest wins (R-12.3)
		return;
	}
	const s64 offset = s64(now) - s64(s.time_ms);
	if (!t.have_offset || offset < t.clock_offset)
		t.clock_offset = offset;
	else
		t.clock_offset += 1; // slow drift upwards so one lucky packet does not pin it forever
	t.have_offset = true;
	t.last_remote_time = s.time_ms;

	Sample smp;
	smp.s = s;
	smp.local_time = u64(s64(s.time_ms) + t.clock_offset);
	t.samples.push_back(smp);
	while (t.samples.size() > kMaxSamples)
		t.samples.pop_front();
}

bool capture(State& s)
{
	CActor* actor = smart_cast<CActor*>(Level().CurrentEntity());
	if (!actor || !actor->g_Alive())
		return false;
	s.time_ms = u32(mgr().now_ms());
	s.pos = actor->Position();
	if (CCharacterPhysicsSupport* ps = actor->character_physics_support())
		if (ps->movement())
			s.vel = ps->movement()->GetVelocity();
	s.body_yaw = angle_normalize(actor->coop_model_yaw());
	s.torso_yaw = angle_normalize(actor->coop_torso().yaw);
	s.torso_pitch = angle_normalize_signed(actor->coop_torso().pitch);
	s.mstate = actor->MovingState();
	s.legs = actor->m_current_legs.val;
	s.torso = actor->m_current_torso.val;
	s.head = actor->m_current_head.val;
	s.active_slot = u8(actor->inventory().GetActiveSlot());
	PIItem active = actor->inventory().ActiveItem();
	s.active_item = active ? join::to_host(active->object_id()) : 0xffff;
	return true;
}

// Host: a client sent its own state.
void host_on_player_state(PeerId from, Reader& r)
{
	r.u16(); // avatar id claimed by the client: ignored, the host knows who it is
	State s;
	if (!s.read(r))
		return;
	const u16 proxy = join::proxy_of_peer(from);
	if (proxy == 0xffff)
		return;
	push_sample(proxy, s);

	Writer w(64);
	w.u16(proxy);
	s.write(w);
	mgr().session().broadcast(Lane::Unreliable, Msg::PlayerState, w, from);
}

// Client: state of the host (avatar 0) or of another client (its proxy id), host numbering.
void client_on_player_state(PeerId, Reader& r)
{
	const u16 host_id = r.u16();
	State s;
	if (!s.read(r))
		return;
	const u16 local = join::to_local(host_id);
	if (local == 0)
		return; // our own state echoed back: never applied
	if (!world::is_player_proxy(local))
		world::client_mark_avatar(local, kInvalidPeer);
	push_sample(local, s);
}

void on_player_state(PeerId from, Reader& r)
{
	if (mgr().is_host())
		host_on_player_state(from, r);
	else if (mgr().is_live_client())
		client_on_player_state(from, r);
}

bool interpolate(Track& t, u64 render_time, State& out)
{
	if (t.samples.empty())
		return false;
	if (t.samples.size() == 1 || render_time <= t.samples.front().local_time)
	{
		out = t.samples.front().s;
		return true;
	}
	for (size_t i = 1; i < t.samples.size(); ++i)
	{
		const Sample& b = t.samples[i];
		if (b.local_time < render_time)
			continue;
		const Sample& a = t.samples[i - 1];
		const float span = float(b.local_time - a.local_time);
		const float k = span > 0.f ? float(render_time - a.local_time) / span : 1.f;
		out = k < 0.5f ? a.s : b.s; // discrete fields from the nearer sample
		out.pos.lerp(a.s.pos, b.s.pos, k);
		out.vel.lerp(a.s.vel, b.s.vel, k);
		out.body_yaw = angle_lerp(a.s.body_yaw, b.s.body_yaw, k);
		out.torso_yaw = angle_lerp(a.s.torso_yaw, b.s.torso_yaw, k);
		out.torso_pitch = a.s.torso_pitch + (b.s.torso_pitch - a.s.torso_pitch) * k;
		return true;
	}
	// Past the newest sample: hold it, extrapolate a little with velocity (max 200 ms).
	out = t.samples.back().s;
	const float ahead = float(_min<u64>(render_time - t.samples.back().local_time, 200)) / 1000.f;
	out.pos.mad(out.vel, ahead);
	return true;
}
} // namespace

void State::write(Writer& w) const
{
	w.u32(time_ms);
	float p[3] = {pos.x, pos.y, pos.z};
	w.vec3(p);
	float v[3] = {vel.x, vel.y, vel.z};
	w.vec3(v);
	w.angle16(body_yaw);
	w.angle16(torso_yaw);
	w.angle16(angle_normalize(torso_pitch));
	w.u16(u16(mstate & 0xffff));
	w.u32(legs);
	w.u32(torso);
	w.u32(head);
	w.u8(active_slot);
	w.u16(active_item);
}

bool State::read(Reader& r)
{
	time_ms = r.u32();
	float p[3], v[3];
	r.vec3(p);
	r.vec3(v);
	pos.set(p[0], p[1], p[2]);
	vel.set(v[0], v[1], v[2]);
	body_yaw = r.angle16();
	torso_yaw = r.angle16();
	torso_pitch = angle_normalize_signed(r.angle16());
	mstate = r.u16();
	legs = r.u32();
	torso = r.u32();
	head = r.u32();
	active_slot = r.u8();
	active_item = r.u16();
	// R3 K1: finite numbers, a sane speed, a position inside the level's box grown by kLevelMargin.
	if (!r.ok() || !validate::finite3(p) || !validate::speed_ok(v, kMaxSpeed))
		return false;
	if (!g_pGameLevel)
		return false;
	const Fbox& box = Level().ObjectSpace.GetBoundingVolume();
	const float lo[3] = {box.x1, box.y1, box.z1};
	const float hi[3] = {box.x2, box.y2, box.z2};
	return validate::in_box(p, lo, hi, kLevelMargin);
}

void install(Session& s)
{
	s.on(Msg::PlayerState, &on_player_state);
	equip_install(s);
}

void reset()
{
	g_tracks.clear();
	g_last_send = 0;
	equip_reset();
}

void on_peer_left(PeerId id)
{
	const u16 avatar = mgr().is_host() ? join::proxy_of_peer(id) : world::avatar_of(id);
	if (avatar != 0xffff)
		g_tracks.erase(avatar);
}

void frame()
{
	const bool live = mgr().is_host() || mgr().is_live_client();
	if (!live || !g_pGameLevel)
		return;
	equip_frame();
	const u64 now = mgr().now_ms();
	if (now - g_last_send < kSendIntervalMs)
		return;
	g_last_send = now;

	State s;
	if (!capture(s))
		return;
	Writer w(64);
	w.u16(mgr().is_host() ? u16(0) : join::my_proxy());
	s.write(w);
	if (mgr().is_host())
		mgr().session().broadcast(Lane::Unreliable, Msg::PlayerState, w);
	else
		mgr().session().send(kHostPeer, Lane::Unreliable, Msg::PlayerState, w);
	++g_sent;
}

bool drive_avatar(CAI_Stalker* avatar)
{
	auto it = g_tracks.find(avatar->ID());
	if (it == g_tracks.end())
		return false;
	Track& t = it->second;
	State s;
	if (!interpolate(t, mgr().now_ms() - kInterpDelayMs, s))
		return false;

	// Body placement.
	Fmatrix xform;
	xform.rotateY(-s.body_yaw);
	xform.c.set(s.pos);
	avatar->XFORM().set(xform);
	const u32 mstate_before = t.mstate;
	const bool was_driven = t.driven;
	t.driven = true;
	t.mstate = s.mstate;
	t.view_yaw = s.torso_yaw;
	t.view_pitch = s.torso_pitch;
	if (was_driven && mstate_before != s.mstate)
		sound::on_avatar_move(avatar, mstate_before, s.mstate); // W5 C2.2: jumps and landings
	if (survival::avatar_pose_override(avatar)) // WP8: wounded / dead pose
	{
		t.applied_legs = t.applied_torso = t.applied_head = 0; // replay the owner's motions after standing up
		return true;
	}

	// Exact motions the owner's character plays (same visual => same MotionIDs).
	IKinematicsAnimated* ka = smart_cast<IKinematicsAnimated*>(avatar->Visual());
	if (ka)
	{
		auto play = [ka](u32 value, u32& applied)
		{
			if (!value || value == applied)
				return;
			MotionID m;
			m.val = value;
			if (entities::motion_valid(ka, value)) // the avatar's visual may not be the owner's yet
				ka->PlayCycle(m);
			applied = value;
		};
		play(s.legs, t.applied_legs);
		play(s.torso, t.applied_torso);
		play(s.head, t.applied_head);
	}
	return true;
}

u32 avatar_mstate(u16 avatar_id)
{
	auto it = g_tracks.find(avatar_id);
	return it != g_tracks.end() && it->second.driven ? it->second.mstate : 0;
}

bool avatar_view(u16 avatar_id, float& yaw, float& pitch)
{
	auto it = g_tracks.find(avatar_id);
	if (it == g_tracks.end() || !it->second.driven)
		return false;
	yaw = it->second.view_yaw;
	pitch = it->second.view_pitch;
	return true;
}

void print_status()
{
	::Msg("[coop]   player states sent: %llu", g_sent);
	equip_print_status();
	for (const auto& it : g_tracks)
		::Msg("[coop]   avatar %u: %llu states (%llu out of order), buffer %u", it.first, it.second.received,
		    it.second.dropped_old, (u32)it.second.samples.size());
}
} // namespace players
} // namespace coop
