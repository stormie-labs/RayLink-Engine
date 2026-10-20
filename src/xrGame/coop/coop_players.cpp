#include "stdafx.h"
#include "coop_players.h"
#include "coop_manager.h"
#include "coop_join.h"
#include "coop_world.h"

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
};

// Keyed by avatar id in local numbering.
xr_map<u16, Track> g_tracks;
u64 g_last_send = 0;
u64 g_sent = 0;

void push_sample(u16 avatar, const State& s)
{
	Track& t = g_tracks[avatar];
	++t.received;
	if (t.have_offset && s32(s.time_ms - t.last_remote_time) <= 0)
	{
		++t.dropped_old; // reordered or duplicate: latest wins (R-12.3)
		return;
	}
	const u64 now = mgr().now_ms();
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
	return r.ok();
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
			if (m.slot < ka->LL_MotionsSlotCount())
				ka->PlayCycle(m);
			applied = value;
		};
		play(s.legs, t.applied_legs);
		play(s.torso, t.applied_torso);
		play(s.head, t.applied_head);
	}
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
