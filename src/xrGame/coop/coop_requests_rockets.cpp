// Anomaly Coop - requests: a client's rocket or launcher grenade (ReqLaunch). The client launches its copy at once (a
// visual, it never detonates there) and sends the launch; the host launches its own copy the way the weapons do, so the
// host flies and detonates it (the explosion is relayed), and its destroy removes every copy (EntityLocal).
#include "stdafx.h"
#include "coop_requests_impl.h"
#include "coop_events.h"
#include "coop_manager.h"
#include "coop_net.h"
#include "coop_join.h"
#include "coop_world.h"
#include "coop_wire_vec.h"
#include "xrCoop/coop_validate.h"

#include "GameObject.h"
#include "CustomRocket.h"
#include "RocketLauncher.h"
#include "Explosive.h"
#include "../xrServerEntities/xrMessages.h"

namespace coop
{
namespace requests
{
namespace
{
constexpr float kLaunchReach = 6.f; // player character <-> launch point, with lag slack
constexpr float kMaxLaunchSpeed = 400.f; // stock launchers: 32..121 m/s
constexpr float kMaxLaunchSpin = 50.f; // rad/s; stock launchers use 0
constexpr u32 kLaunchIntervalMs = 100;

// A refused launch: the requester's copy already flies, so the rocket must not stay in its launcher here.
// The destroy reaches every client (EntityLocal op 1) and removes their copies. Only while it is still
// in the requester's hands (a free one is already flying: a repeated request).
void host_discard_rocket(const Req& q, u16 rocket)
{
	CSE_Abstract* e = world::server_entity(rocket);
	if (!e || e->ID_Parent == kNoId || events::root_of(rocket) != q.proxy || !world::object_here(rocket))
		return;
	NET_Packet P;
	CGameObject::u_EventGen(P, GE_DESTROY, rocket);
	CGameObject::u_EventSend(P);
}

Reason host_launch(const Req& q, u16 launcher, u16 rocket, Fvector pos, Fvector dir, Fvector vel, Fvector ang)
{
	// Nothing from the wire reaches the physics world unchecked (coop::validate). The direction is a unit
	// vector: a huge one normalizes to zero and the launch matrix to NaN.
	if (!validate::vec_finite(pos) || !validate::vec_speed_ok(dir, validate::kMaxDirLength) ||
	    !validate::vec_finite(vel) || !validate::vec_finite(ang) || dir.magnitude() < EPS_L)
	{
		if (note_bad(BadNumber))
			::Msg("! [coop] ReqLaunch #%u of rocket %u from player #%u refused: a vector is not a number", q.seq, rocket,
			    q.from);
		return Reason::Malformed;
	}
	const u64 now = mgr().now_ms();
	u64& last = g_last_launch[q.from];
	if (now - last < kLaunchIntervalMs)
		return Reason::RateLimited;
	CSE_Abstract* re = world::server_entity(rocket);
	if (!re || !world::server_entity(launcher))
		return Reason::Missing;
	if (re->ID_Parent != launcher)
		return Reason::Moved; // launched already, or not loaded in that launcher
	if (events::root_of(launcher) != q.proxy)
		return Reason::NotOwner;
	CGameObject* launcher_go = valid_go(launcher);
	CCustomRocket* rocket_go = smart_cast<CCustomRocket*>(valid_go(rocket));
	CGameObject* me = world::object_here(q.proxy);
	if (!launcher_go || !rocket_go || !me)
		return Reason::Offline;
	if (!smart_cast<CRocketLauncher*>(launcher_go) || rocket_go->H_Parent() != launcher_go)
		return Reason::NotAllowed; // not a launcher, or the rocket is not loaded in it here
	// The character here trails the player by the snapshot delay; a launch point out of its reach starts at it.
	if (me->Position().distance_to(pos) > kLaunchReach)
		pos.set(me->Position()).y += 1.5f;
	dir.normalize();
	if (!validate::vec_speed_ok(vel, kMaxLaunchSpeed))
		vel.normalize_safe().mul(kMaxLaunchSpeed);
	if (!validate::vec_speed_ok(ang, kMaxLaunchSpin))
		ang.normalize_safe().mul(kMaxLaunchSpin);
	Fmatrix xf; // as the weapons build it (CWeaponRPG7::switch2_Fire, CWeaponMagazinedWGrenade::LaunchGrenade)
	xf.identity();
	xf.k.set(dir);
	Fvector::generate_orthonormal_basis(xf.k, xf.j, xf.i);
	xf.c.set(pos);
	// CRocketLauncher::LaunchRocket on this very rocket: the host's launcher copy may also hold a rocket it
	// spawned itself in net_Spawn, so its "current" rocket is not necessarily the client's. The launched-list
	// bookkeeping LaunchRocket adds only feeds the second detach branch in DetachRocket.
	rocket_go->SetLaunchParams(xf, vel, ang);
	if (CExplosive* ex = rocket_go->cast_explosive())
		ex->SetInitiator(q.proxy); // the blast is the player's (kills, PlayerHit for its own character)
	NET_Packet P;
	CGameObject::u_EventGen(P, GE_LAUNCH_ROCKET, launcher);
	P.w_u16(rocket);
	CGameObject::u_EventSend(P); // server: detach + EntityParent relay; launcher: DetachRocket(rocket, true)
	if (re->ID_Parent != kNoId)
		return Reason::Engine;
	last = now;
	events::host_relay_launch(rocket, launcher, pos, dir, vel, ang, q.from);
	return Reason::Ok;
}
} // namespace

xr_vector<Launch> g_launches; // under g_out_lock: the router may run on the game thread
xr_map<PeerId, u64> g_last_launch;

void client_flush_launches()
{
	xr_vector<Launch> out;
	{
		xrCriticalSectionGuard guard(g_out_lock);
		out.swap(g_launches);
	}
	for (const Launch& l : out)
	{
		Writer w(64);
		w.u32(track(Msg::ReqLaunch, l.rocket));
		w.u16(join::to_host(l.launcher));
		w.u16(join::to_host(l.rocket));
		put_vec(w, l.pos);
		put_vec(w, l.dir);
		put_vec(w, l.vel);
		put_vec(w, l.ang);
		net::client_send(Msg::ReqLaunch, w);
	}
}

void host_on_launch(PeerId from, Reader& r)
{
	Req q;
	if (!host_begin(from, r, Msg::ReqLaunch, q))
		return;
	const u16 launcher = r.u16();
	const u16 rocket = r.u16();
	const Fvector pos = get_vec(r);
	const Fvector dir = get_vec(r);
	const Fvector vel = get_vec(r);
	const Fvector ang = get_vec(r);
	if (!r.ok())
		return reply(q, Reason::Malformed, kNoId);
	const Reason why = host_launch(q, launcher, rocket, pos, dir, vel, ang);
	if (why != Reason::Ok)
		host_discard_rocket(q, rocket);
	reply(q, why, rocket);
}

bool client_launch(u16 local_launcher, u16 local_rocket)
{
	if (client_lease_waiting(local_launcher))
		return true;
	if (!is_live_mirror())
	{
		++g_cl.not_live;
		return true; // nothing goes out before the world is live
	}
	CCustomRocket* rocket = smart_cast<CCustomRocket*>(world::object_here(local_rocket));
	if (!rocket)
		return false;
	// The launcher has just called LaunchRocket on it (the event follows the call in every weapon).
	Fmatrix xf;
	Launch l;
	rocket->coop_launch_params(xf, l.vel, l.ang);
	l.launcher = local_launcher;
	l.rocket = local_rocket;
	l.pos = xf.c;
	l.dir = xf.k;
	xrCriticalSectionGuard guard(g_out_lock);
	g_launches.push_back(l);
	return true;
}
} // namespace requests
} // namespace coop
