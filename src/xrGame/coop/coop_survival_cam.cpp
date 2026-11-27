// Anomaly Coop — player survival: the cameras of a player who is down (lying, spectator, overhead), his controls while down
// and the lying poses of the players' characters. See coop_survival.h.
#include "stdafx.h"
#include "coop_survival_state.h"
#include "coop_manager.h"
#include "coop_join.h"
#include "coop_health_ledger.h"

#include "../../xrEngine/XR_IOConsole.h"
#include "../../xrEngine/xr_input.h"
#include "Level.h"
#include "Actor.h"
#include "ActorCondition.h"
#include "ActorEffector.h"
#include "CameraEffector.h"
#include "ai/stalker/ai_stalker.h"
#include "Inventory.h"
#include "UIGameCustom.h"
#include "ui/UIPdaWnd.h"
#include "ui/UIActorMenu.h"
#include "MainMenu.h"
#include "../Include/xrRender/Kinematics.h"
#include "../Include/xrRender/animation_blend.h"

#include <algorithm>

namespace coop
{
namespace survival
{
// ---------------------------------------------------------------- state (coop_survival_state.h)

xr_map<u16, Pose> g_poses;
Pose g_my_pose;
std::atomic<const CActor*> g_pose_actor{nullptr};
bool g_my_twist_off = false;
SpecCam g_spec;
OverCam g_over;
DownInput g_input;

namespace
{
constexpr float kCamDrop = 1.1f; // eye height drop while lying
constexpr float kOverheadTurn = 0.08f; // the overhead camera turns this many radians a second

// Stalker animations for a player lying on the ground, by preference (stock stalker_animation set;
// the first one the avatar's visual has is used; none = the owner's motions keep playing).
const char* const kWoundedPoses[] = {"wounded_heavy_1_idle_0", "wounded_1_idle_0", "wounded_idle_0", "wounded_heavy_idle_0"};
const char* const kDeadPoses[] = {"wounded_heavy_2_idle_0", "wounded_heavy_3_idle_0", "wounded_heavy_1_idle_0",
                                  "wounded_1_idle_0", "wounded_idle_0"};

// ---------------------------------------------------------------- camera effectors

ECamEffectorType eff_down() { return (ECamEffectorType)0x434F4F01; }
ECamEffectorType eff_spectate() { return (ECamEffectorType)0x434F4F02; }
ECamEffectorType eff_overhead() { return (ECamEffectorType)0x434F4F03; }

// The camera manager puts a new effector on at the end of its update and takes one off only among those already on
// (CCameraManager::AddCamEffector, RemoveCamEffector: the waiting list is not looked at). A camera set and taken back with
// no update of the actor's camera in between (the level still starting, the actor not updated) came on afterwards and
// stayed for good: nothing takes off a camera the state does not know of, and a player back on his feet went on looking from
// the ground. Each camera of ours ends itself when it is not the one the state asks for; the line tells that it happened.
bool cam_stale(Cam cam, LPCSTR name)
{
	if (g_me.cam == cam)
		return false;
	::Msg("[coop] camera: a left-over %s camera is taken off (the state asks for %u)", name, u32(g_me.cam));
	return true;
}

// First-person camera of a player lying on the ground.
class DownCam : public CEffectorCam
{
public:
	DownCam() : CEffectorCam(eff_down(), 100000.f) { SetHudAffect(false); }
	BOOL ProcessCam(SCamEffectorInfo& info) override
	{
		if (cam_stale(Cam::Down, "lying"))
			return FALSE;
		fLifeTime = 100000.f;
		m_drop += (kCamDrop - m_drop) * _min(1.f, Device.fTimeDelta * 2.5f);
		info.p.y -= m_drop;
		Fmatrix roll;
		roll.rotation(info.d, m_drop * 0.2f);
		roll.transform_dir(info.n);
		return TRUE;
	}

private:
	float m_drop = 0.f;
};

// Third-person camera behind the watched teammate (hardcore spectator).
class SpectateCam : public CEffectorCam
{
public:
	SpectateCam() : CEffectorCam(eff_spectate(), 100000.f) { SetHudAffect(false); }
	bool AbsolutePositioning() override { return true; }
	BOOL ProcessCam(SCamEffectorInfo& info) override
	{
		if (cam_stale(Cam::Spectate, "spectator"))
			return FALSE;
		fLifeTime = 100000.f;
		if (!g_spec.valid)
			return TRUE;
		const float k = m_init ? _min(1.f, Device.fTimeDelta * 5.f) : 1.f;
		m_pos.lerp(m_pos, g_spec.pos, k);
		m_look.lerp(m_look, g_spec.look, k);
		m_init = true;
		Fvector d;
		d.sub(m_look, m_pos);
		if (d.square_magnitude() < EPS_L)
			return TRUE;
		d.normalize();
		info.p.set(m_pos);
		info.d.set(d);
		info.n.set(0.f, 1.f, 0.f);
		return TRUE;
	}

private:
	bool m_init = false;
	Fvector m_pos{}, m_look{};
};

// Evacuated with nobody alive to watch: straight down at the own body from kOverheadHeight, slowly turning.
class OverheadCam : public CEffectorCam
{
public:
	OverheadCam() : CEffectorCam(eff_overhead(), 100000.f) { SetHudAffect(false); }
	bool AbsolutePositioning() override { return true; }
	BOOL ProcessCam(SCamEffectorInfo& info) override
	{
		if (cam_stale(Cam::Overhead, "overhead"))
			return FALSE;
		fLifeTime = 100000.f;
		if (!g_over.valid)
			return TRUE;
		m_yaw += kOverheadTurn * Device.fTimeDelta;
		if (m_yaw > PI_MUL_2)
			m_yaw -= PI_MUL_2;
		info.p.set(g_over.body.x, g_over.body.y + g_over.height, g_over.body.z);
		info.d.set(0.f, -1.f, 0.f);
		info.n.set(_sin(m_yaw), 0.f, _cos(m_yaw)); // "up" on the screen turns: the view rotates about the body
		return TRUE;
	}

private:
	float m_yaw = 0.f;
};

// My actor gets its own motions and its aiming twist back (it stands up, the session ends, or its visual has no
// pose). CActor::g_SetAnimation plays a cycle only when its MotionID changes, and they did not change while it lay: its
// cache is cleared (CActor::ResetAnimationCache), or the actor would go on lying.
void own_pose_off(CActor* a)
{
	if (!a || g_pose_actor.load() != a)
		return;
	g_pose_actor.store(nullptr);
	if (g_my_twist_off)
		a->SetCallbacks();
	g_my_twist_off = false;
	a->ResetAnimationCache();
}

// Evacuated with nobody to watch: the overhead camera's target, the own body, and how high it may go (a ceiling).
void overhead_frame(CActor* a)
{
	Fvector from = a->Position();
	from.y += 0.5f;
	float dist = kOverheadHeight;
	collide::rq_result R;
	if (Level().ObjectSpace.RayPick(from, Fvector().set(0.f, 1.f, 0.f), dist, collide::rqtStatic, R, nullptr))
		dist = _max(1.f, R.range - 0.3f);
	g_over.body = a->Position();
	g_over.height = _min(kOverheadHeight, dist + 0.5f);
	g_over.valid = true;
}

bool blend_playing(IKinematicsAnimated* ka, MotionID m)
{
	for (u32 part = 0; part < 4; ++part)
		for (u32 i = 0, n = ka->LL_PartBlendsCount(part); i < n; ++i)
			if (CBlend* b = ka->LL_PartBlend(part, i))
				if (b->motionID == m)
					return true;
	return false;
}

MotionID find_pose(IKinematicsAnimated* ka, Life life)
{
	if (life == Life::Wounded)
	{
		for (const char* name : kWoundedPoses)
		{
			const MotionID m = ka->ID_Cycle_Safe(name);
			if (m.valid())
				return m;
		}
	}
	else
	{
		for (const char* name : kDeadPoses)
		{
			const MotionID m = ka->ID_Cycle_Safe(name);
			if (m.valid())
				return m;
		}
	}
	return MotionID();
}

// The lying pose of a player's character by his life (death::body_pose): the avatar of another player
// (avatar_pose_override) and my own actor (own_pose_frame) by one rule and the same motions. False: his life shows none
// (alive), or the visual has none of the motions (one '!' line until he stands up) - his own motions play on.
bool hold_pose(Pose& p, IKinematicsAnimated* ka, Life life, const char* who, u16 id)
{
	const death::BodyPose want = death::body_pose(u8(life));
	if (want == death::BodyPose::Standing)
		return false;
	const Life pose_life = want == death::BodyPose::Wounded ? Life::Wounded : Life::Dead;
	if (p.life != pose_life || p.visual != ka)
	{
		p.life = pose_life;
		p.visual = ka;
		p.motion = find_pose(ka, pose_life);
		p.missing = !p.motion.valid();
		p.since = mgr().now_ms();
		if (p.missing && !p.logged)
		{
			p.logged = true;
			::Msg("! [coop] %s %u: no %s animation in its visual, it stands (its own motions play)", who, id,
			    pose_life == Life::Wounded ? "wounded" : "lying");
		}
	}
	if (p.missing)
		return false;
	if (!blend_playing(ka, p.motion))
		ka->PlayCycle(p.motion, TRUE);
	if (death::body_still(want, p.since, mgr().now_ms())) // the last move is over: the dead body does not breathe
		for (u32 part = 0; part < 4; ++part)
			for (u32 i = 0, n = ka->LL_PartBlendsCount(part); i < n; ++i)
				if (CBlend* b = ka->LL_PartBlend(part, i))
					if (b->motionID == p.motion)
						b->speed = 0.f;
	return true;
}

} // namespace

bool action_down(EGameActions a)
{
	if (!pInput || (Console && Console->bVisible) || (MainMenu() && MainMenu()->IsActive()))
		return false;
	for (int i = 0; i < 2; ++i)
	{
		const int dik = get_action_dik(a, i);
		if (dik > 0 && pInput->iGetAsyncKeyState(dik))
			return true;
	}
	return false;
}

void set_cam(CActor* a, Cam cam)
{
	if (g_me.cam == cam)
		return;
	if (a)
	{
		if (g_me.cam == Cam::Down)
			a->Cameras().RemoveCamEffector(eff_down());
		else if (g_me.cam == Cam::Spectate)
			a->Cameras().RemoveCamEffector(eff_spectate());
		else if (g_me.cam == Cam::Overhead)
			a->Cameras().RemoveCamEffector(eff_overhead());
		if (cam == Cam::Down)
			a->Cameras().AddCamEffector(xr_new<DownCam>());
		else if (cam == Cam::Spectate)
		{
			g_spec.valid = false;
			a->Cameras().AddCamEffector(xr_new<SpectateCam>());
		}
		else if (cam == Cam::Overhead)
			a->Cameras().AddCamEffector(xr_new<OverheadCam>());
	}
	g_me.cam = cam;
}

// ---------------------------------------------------------------- controls


void close_menus()
{
	CUIGameCustom* ui = CurrentGameUI();
	if (!ui)
		return;
	if (ui->GetActorMenu().IsShown())
		ui->HideActorMenu();
	if (ui->GetPdaMenu().IsShown())
		ui->GetPdaMenu().HideDialog();
}

void hold_controls(CActor* a)
{
	if (!a->input_external_handler_installed())
	{
		g_input.install(a); // clears movement wishes, releases the trigger
		g_me.input_installed = true;
	}
	if (!g_me.slots_blocked)
	{
		a->inventory().SetSlotsBlocked(INV_STATE_BLOCK_ALL, true); // weapon goes away (local, no event)
		g_me.slots_blocked = true;
	}
	if (!g_me.inv_blocked)
	{
		// Fell in a grip (no input of ours: the grip holds the actor): the flag is the grip's and goes with it, not back after the revive
		g_me.inv_prev = g_me.input_installed && a->inventory_disabled();
		g_me.inv_blocked = true;
	}
	if (!a->inventory_disabled()) // the end of that grip took it off under us
		a->set_inventory_disabled(true);
	CActorCondition& c = a->conditions();
	g_hook_owner.store(&c);
	c.ClearWounds(); // the bleed-out timer is the bleeding: wounds would only shorten it twice
	if (c.GetHealth() != kDownHealth)
		health_ledger::coop_set_health(c, kDownHealth); // GodMode or no condition update this frame
}

void release_controls(CActor* a)
{
	own_pose_off(a);
	g_my_pose = Pose(); // the next fall looks its pose up again (and tells a missing one again)
	if (g_me.input_installed)
	{
		if (a && a->input_external_handler_installed())
			g_input.release();
		g_me.input_installed = false;
	}
	if (g_me.slots_blocked)
	{
		if (a)
			a->inventory().SetSlotsBlocked(INV_STATE_BLOCK_ALL, false);
		g_me.slots_blocked = false;
	}
	if (g_me.inv_blocked)
	{
		if (a)
			a->set_inventory_disabled(g_me.inv_prev);
		g_me.inv_blocked = false;
	}
	set_cam(a, Cam::Own);
	g_spec.valid = false;
	g_over.valid = false;
}

void spectate_frame(u64 t)
{
	const u16 watched = g_me.spectate;
	static xr_vector<u16> watchable; // every frame while watching: the buffer stays
	watchable.clear();
	each_player([](PeerId, u16 host_id, u16 local_id, const char*)
	{
		const Life l = life_by_host_id(host_id);
		if ((l == Life::Alive || l == Life::Wounded || l == Life::Respawning) && object_here(local_id))
			watchable.push_back(local_id);
	});
	if (watchable.empty())
	{
		g_me.spectate = kNoId;
		g_spec.valid = false;
		return;
	}
	auto cur = std::find(watchable.begin(), watchable.end(), g_me.spectate);
	const bool next = action_down(kWPN_FIRE), prev = action_down(kWPN_ZOOM);
	size_t idx = cur == watchable.end() ? 0 : size_t(cur - watchable.begin());
	if (next && !g_me.next_was_down)
		idx = (idx + 1) % watchable.size();
	else if (prev && !g_me.prev_was_down)
		idx = (idx + watchable.size() - 1) % watchable.size();
	g_me.next_was_down = next;
	g_me.prev_was_down = prev;
	g_me.spectate = watchable[idx];
	if (g_me.spectate != watched)
	{
		g_me.fade_at = t;
		g_me.fading = true;
	}
	if (g_me.fading)
	{
		const float volume = death::spectate_volume(t - g_me.fade_at);
		::Sound->set_master_volume(volume);
		g_me.fading = volume < 1.f;
	}

	CGameObject* o = object_here(g_me.spectate);
	Fvector head = o->Position();
	head.y += 1.6f;
	Fvector fwd = o->XFORM().k;
	fwd.y = 0.f;
	if (fwd.square_magnitude() < EPS_L)
		fwd.set(0.f, 0.f, 1.f);
	fwd.normalize();
	Fvector back;
	back.set(-fwd.x, 0.35f, -fwd.z);
	back.normalize();
	float dist = 3.2f;
	collide::rq_result R;
	if (Level().ObjectSpace.RayPick(head, back, dist, collide::rqtStatic, R, nullptr))
		dist = _max(0.4f, R.range - 0.3f);
	g_spec.pos.mad(head, back, dist);
	g_spec.look = head;
	g_spec.valid = true;
}

// Dead and evacuated: watch a living teammate (Fire / Aim switch), nobody alive: look down at the body.
void evacuated_frame(CActor* a, u64 t)
{
	spectate_frame(t);
	const Cam cam = g_spec.valid ? Cam::Spectate : Cam::Overhead;
	if (g_me.cam != cam)
	{
		set_cam(a, cam);
		if (cam == Cam::Spectate)
			spectate_frame(t); // set_cam starts the spectator camera without a target
	}
	if (cam == Cam::Overhead)
		overhead_frame(a);
	else
		g_over.valid = false;
}

// My actor lies as my avatar lies on the other machines (the overhead and spectator cameras show it). While the pose holds,
// the actor's own motions are off (own_pose_held: g_SetAnimation would play its idle, a turn or a landing over the pose
// whenever the MotionID changes - the weapon put away at the fall is one) and so is the aiming twist of the spine and head
// (not in a vehicle or at a mounted gun: those set their own bone callbacks). The first-person camera stands on the actor's
// position, not on a bone (CActor::cam_Update): the pose does not move it. own_pose_off gives it all back.
void own_pose_frame(CActor* a)
{
	IKinematicsAnimated* ka = smart_cast<IKinematicsAnimated*>(a->Visual());
	if (!ka || !hold_pose(g_my_pose, ka, g_me.life, "own actor", a->ID()))
	{
		own_pose_off(a); // standing, or no pose in the visual: the body stands as before
		return;
	}
	if (g_pose_actor.load() == a)
		return;
	g_pose_actor.store(a);
	// The step manager reads the legs blend of the last g_SetAnimation every frame and does not see the pose fade it out
	// (unlike the legs IK, which drops a freed blend): a fall while running would go on making footsteps from it.
	a->CStepManager::on_animation_start(MotionID(), nullptr); // no blend: no steps until g_SetAnimation plays legs again
	if (!a->Holder())
	{
		a->ResetCallbacks();
		g_my_twist_off = true;
	}
}

bool avatar_pose_override(CAI_Stalker* avatar)
{
	if (!avatar || !live())
		return false;
	const u16 id = avatar->ID();
	const Life l = life_of(id);
	auto it = g_poses.find(id);
	if (!is_down(l))
	{
		if (it != g_poses.end())
			g_poses.erase(it);
		return false;
	}
	IKinematicsAnimated* ka = smart_cast<IKinematicsAnimated*>(avatar->Visual());
	if (!ka)
		return false;
	return hold_pose(g_poses[id], ka, l, "avatar", id); // the same pose as my own actor's
}

bool own_pose_held(const CActor* actor) { return actor && g_pose_actor.load() == actor; }

} // namespace survival
} // namespace coop
