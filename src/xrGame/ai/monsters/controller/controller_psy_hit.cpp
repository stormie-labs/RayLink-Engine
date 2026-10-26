#include "stdafx.h"
#include "controller_psy_hit.h"
#include "../BaseMonster/base_monster.h"
#include "controller.h"
#include "../control_animation_base.h"
#include "../control_direction_base.h"
#include "../control_movement_base.h"
#include "../../../level.h"
#include "../../../actor.h"
#include "../../../ActorEffector.h"
#include "../../../../xrEngine/CameraBase.h"
#include "../../../CharacterPhysicsSupport.h"
#include "../../../level_debug.h"
#include "../../../ActorCondition.h"
#include "../../../HudManager.h"
#include "../../../coop/coop_remote.h" // Anomaly Coop
#include "../../../coop/coop_ai.h" // Anomaly Coop: is_remote_player
#include "../../../coop/coop_monster_fx.h" // Anomaly Coop: the tube on a remote player's machine

namespace
{
// Anomaly Coop (audit A5), client: the controller whose tube holds our actor now (one at a time, as the stock
// camera effector check makes it on the host)
CControllerPsyHit* g_coop_tube = nullptr;
constexpr u32 kCoopTubeSlackMs = 5000; // a stage's end that never came: the actor is freed this much later
} // namespace

void CControllerPsyHit::load(LPCSTR section)
{
	m_min_tube_dist = pSettings->r_float(section, "tube_condition_min_distance");
}

void CControllerPsyHit::reinit()
{
	inherited::reinit();

	IKinematicsAnimated* skel = smart_cast<IKinematicsAnimated *>(m_object->Visual());
	m_stage[0] = skel->ID_Cycle_Safe("psy_attack_0");
	VERIFY(m_stage[0]);
	m_stage[1] = skel->ID_Cycle_Safe("psy_attack_1");
	VERIFY(m_stage[1]);
	m_stage[2] = skel->ID_Cycle_Safe("psy_attack_2");
	VERIFY(m_stage[2]);
	m_stage[3] = skel->ID_Cycle_Safe("psy_attack_3");
	VERIFY(m_stage[3]);
	m_current_index = 0;

	m_time_last_tube = 0;
	m_sound_state = eNone;
}


bool CControllerPsyHit::tube_ready() const
{
	u32 tube_condition_min_delay = 5000;
	if (CController* controller = smart_cast<CController*>(m_object))
		tube_condition_min_delay = controller->m_tube_condition_min_delay;

	return m_time_last_tube + tube_condition_min_delay < time();
}

bool CControllerPsyHit::check_start_conditions()
{
	if (coop::remote::is_remote_creature(m_object)) // Anomaly Coop: the host decides
		return false; // Anomaly Coop
	if (is_active())
		return false;

	if (m_man->is_captured_pure())
		return false;

	const CEntityAlive* who = pick_target(); // Anomaly Coop (audit A5): a remote player's character too
	if (!who)
		return false;

	if (who == Actor() ? !!Actor()->Cameras().GetCamEffector(eCEControllerPsyHit) : coop::monster_fx::host_held(who->ID()))
		return false;

	if (!m_object->EnemyMan.see_enemy_now(who))
		return false;

	if (!tube_ready())
		return false;

	if (m_object->Position().distance_to(who->Position()) < m_min_tube_dist)
		return false;

	return true;
}

void CControllerPsyHit::activate()
{
	m_man->capture_pure(this);
	m_man->subscribe(this, ControlCom::eventAnimationEnd);

	m_man->path_stop(this);
	m_man->move_stop(this);

	const CEntityAlive* who = pick_target();
	m_coop_target = who && who != Actor() ? who->ID() : u16(-1); // Anomaly Coop (audit A5)
	turn_to(who);
	if (remote_target())
	{
		coop::monster_fx::host_hold(m_coop_target, m_object->ID());
		coop::monster_fx::host_send(who, m_object, coop::monster_fx::kControllerTube, coopPrepare, 0.f);
	}

	m_current_index = 0;
	play_anim();

	m_blocked = false;

	set_sound_state(ePrepare);
}

void CControllerPsyHit::deactivate()
{
	m_man->release_pure(this);
	m_man->unsubscribe(this, ControlCom::eventAnimationEnd);

	if (m_blocked)
	{
		NET_Packet P;

		Actor()->u_EventGen(P, GEG_PLAYER_WEAPON_HIDE_STATE, Actor()->ID());
		P.w_u16(INV_STATE_BLOCK_ALL);
		P.w_u8(u8(false));
		Actor()->u_EventSend(P);
	}

	set_sound_state(eNone);

	if (remote_target()) // Anomaly Coop (audit A5): the player's machine lets its actor go
	{
		coop::monster_fx::host_send(target(), m_object, coop::monster_fx::kControllerTube, coopCancel, 0.f);
		coop::monster_fx::host_unhold(m_coop_target, m_object->ID());
		m_coop_target = u16(-1);
	}
}

void CControllerPsyHit::on_event(ControlCom::EEventType type, ControlCom::IEventData* data)
{
	if (type == ControlCom::eventAnimationEnd)
	{
		if (m_current_index < 3)
		{
			m_current_index++;
			play_anim();

			switch (m_current_index)
			{
			case 1: death_glide_start();
				break;
			case 2: hit();
				break;
			case 3: death_glide_end();
				break;
			}
		}
		else
		{
			m_man->deactivate(this);
			return;
		}
	}
}

void CControllerPsyHit::play_anim()
{
	SControlAnimationData* ctrl_anim = (SControlAnimationData*)m_man->data(this, ControlCom::eControlAnimation);
	VERIFY(ctrl_anim);

	ctrl_anim->global.set_motion(m_stage[m_current_index]);
	ctrl_anim->global.actual = false;
}

namespace detail
{
	bool check_actor_visibility(const Fvector trace_from,
	                            const Fvector trace_to,
	                            CObject* object)
	{
		const float dist = trace_from.distance_to(trace_to);
		Fvector trace_dir;
		trace_dir.sub(trace_to, trace_from);

		//DBG().level_info(this).add_item	(trace_from,trace_to,D3DCOLOR_XRGB(0,150,150));


		collide::rq_result l_rq;
		l_rq.O = NULL;
		Level().ObjectSpace.RayPick(trace_from,
		                            trace_dir,
		                            dist,
		                            collide::rqtBoth,
		                            l_rq,
		                            object);

		return l_rq.O == Actor() || (l_rq.range >= dist - 0.1f);
	}
} // namespace detail

extern CActor* g_actor;

const CEntityAlive* CControllerPsyHit::pick_target() const
{
	const CEntityAlive* enemy = m_object->EnemyMan.get_enemy();
	if (enemy && coop::npc_ai::is_remote_player(enemy) && m_object->EnemyMan.see_enemy_now(enemy))
		return enemy;
	return Actor();
}

const CEntityAlive* CControllerPsyHit::target() const
{
	if (!remote_target())
		return Actor();
	return smart_cast<const CEntityAlive*>(Level().Objects.net_Find(m_coop_target));
}

void CControllerPsyHit::turn_to(const CEntityAlive* who)
{
	if (!who)
		return;
	SControlDirectionData* ctrl_dir = (SControlDirectionData*)m_man->data(this, ControlCom::eControlDir);
	VERIFY(ctrl_dir);
	ctrl_dir->heading.target_speed = 3.f;
	ctrl_dir->heading.target_angle = m_man->direction().angle_to_target(who->Position());
}

#if 0 // the stock visibility test, kept for reference (Anomaly Coop: see_enemy_now(target()) is used)
bool CControllerPsyHit_see_enemy()
{
	// 	using namespace detail;
	// 	Fvector const self_head = get_head_position(m_object);
	// 	Fvector actor_center;
	// 	Actor()->Center(actor_center);
	// 	Fvector self_center;
	// 	m_object->Center(self_center);
	// 
	// 	if ( check_actor_visibility(self_head, get_head_position(Actor()), m_object) &&
	// 		 check_actor_visibility(self_head, actor_center, m_object) &&
	// 		 check_actor_visibility(self_center, get_head_position(Actor()), m_object) &&
	// 		 check_actor_visibility(self_center, actor_center, m_object) )
	// 	{
	// 		return true;
	// 	}
	// 
	// 	return false;
}
#endif

bool CControllerPsyHit::check_conditions_final()
{
	if (!m_object->g_Alive())
		return false;

	if (!g_actor)
		return false;

	// 	if (m_object->EnemyMan.get_enemy() != Actor())	
	// 		return false;

	const CEntityAlive* who = target(); // Anomaly Coop (audit A5): the actor or a remote player's character
	if (!who)
		return false;

	if (!m_object->EnemyMan.is_enemy(who))
		return false;

	if (!who->g_Alive())
		return false;

	if (m_object->Position().distance_to_xz(who->Position()) < m_min_tube_dist - 2)
		return false;

	return m_object->EnemyMan.see_enemy_now(who);
}

void CControllerPsyHit::death_glide_start()
{
	if (!check_conditions_final())
	{
		m_man->deactivate(this);
		return;
	}

	const float motion_time = m_man->animation().motion_time(m_stage[1], m_object->Visual());
	smart_cast<CController *>(m_object)->draw_fire_particles();
	if (remote_target())
		// Anomaly Coop (audit A5): the player's machine plays the pull on its actor
		coop::monster_fx::host_send(target(), m_object, coop::monster_fx::kControllerTube, coopStart, motion_time);
	else
		glide_actor(motion_time);

	turn_to(target());
}

void CControllerPsyHit::glide_actor(float motion_time)
{
	HUD().SetRenderable(false);

	if (CController* controller = smart_cast<CController*>(m_object))
	{
		controller->CControlledActor::install();
		controller->CControlledActor::dont_need_turn();
	}

	// Start effector
	CEffectorCam* ce = Actor()->Cameras().GetCamEffector(eCEControllerPsyHit);
	VERIFY(!ce);

    // demonized: replace m_object->Position() with position of eye bone
	Fvector src_pos = Actor()->cam_Active()->vPosition;
    IKinematics* k = m_object->Visual() ? m_object->Visual()->dcast_PKinematics() : nullptr;
    u16 bone_id = BI_NONE;
    if (k)
    {
        bone_id = k->LL_BoneID("eye_left");
        if (bone_id == BI_NONE)
            bone_id = k->LL_BoneID("eye_right");
        if (bone_id == BI_NONE)
            bone_id = k->LL_BoneID("bip01_head");
        if (bone_id == BI_NONE)
            k = nullptr; // Use m_object position
    }

    Fmatrix m;
    Fvector target_pos = k ? m.mul_43(m_object->XFORM(), k->LL_GetTransform_safed(bone_id)).c : m_object->Position();
	target_pos.y += k ? 0.f : 1.4f;

	Fvector dir;
	dir.sub(target_pos, src_pos);

	float dist = dir.magnitude();
	dir.normalize();

    // demonized: disable actor_psy_immunity affecting camera behaviour of controller attack, very buggy
    // Finetune target_pos a bit
	//float const actor_psy_immunity = Actor()->conditions().GetHitImmunity(ALife::eHitTypeTelepatic);
    //target_pos.mad(src_pos, dir, 0.01f + actor_psy_immunity * (dist - 4.8f));
    //float const base_fov = g_fov;
    //float const dest_fov = g_fov - (g_fov - 10.f) * actor_psy_immunity;

	target_pos.mad(target_pos, dir, -_min(3.5f, dist * 0.75f));
	float const base_fov = g_fov;
	float const dest_fov = 10.f;

	Actor()->Cameras().AddCamEffector(xr_new<CControllerPsyHitCamEffector>(eCEControllerPsyHit, src_pos, target_pos,
	                                                                       motion_time, base_fov, dest_fov));

	dir.sub(src_pos, target_pos);
	dir.normalize();
	float h, p;
	dir.getHP(h, p);
	dir.setHP(h, p + PI_DIV_3);
	Actor()->character_physics_support()->movement()->ApplyImpulse(dir, Actor()->GetMass() * 530.f);

	set_sound_state(eStart);

	NET_Packet P;
	Actor()->u_EventGen(P, GEG_PLAYER_WEAPON_HIDE_STATE, Actor()->ID());
	P.w_u16(INV_STATE_BLOCK_ALL);
	P.w_u8(u8(true));
	Actor()->u_EventSend(P);

	m_blocked = true;
}

void CControllerPsyHit::death_glide_end()
{
	CController* monster = smart_cast<CController *>(m_object);
	monster->draw_fire_particles();

	if (remote_target())
	{
		// Anomaly Coop (audit A5): the character takes the psy hit here, its player gets it as PlayerHit; his machine
		// plays the end of the tube on its actor
		if (const CEntityAlive* who = target())
		{
			m_object->Hit_Psy(const_cast<CEntityAlive*>(who), monster->m_tube_damage);
			coop::monster_fx::host_send(who, m_object, coop::monster_fx::kControllerTube, coopEnd, 0.f);
		}
		m_time_last_tube = Device.dwTimeGlobal;
		return;
	}

	monster->m_sound_tube_hit_left.play_at_pos(Actor(), Fvector().set(-1.f, 0.f, 1.f), sm_2D);
	monster->m_sound_tube_hit_right.play_at_pos(Actor(), Fvector().set(1.f, 0.f, 1.f), sm_2D);

	m_object->Hit_Psy(Actor(), monster->m_tube_damage);

	m_time_last_tube = Device.dwTimeGlobal;
	stop();
}

void CControllerPsyHit::update_frame()
{
	//if (m_sound_state == eStart) {
	//	CController *monster = smart_cast<CController *>(m_object);
	//	if (!monster->m_sound_tube_start._feedback()) {
	//		m_sound_state = ePull;
	//		monster->m_sound_tube_pull.play_at_pos(Actor(), Fvector().set(0.f, 0.f, 0.f), sm_2D);
	//	}
	//}
}

void CControllerPsyHit::set_sound_state(ESoundState state)
{
	CController* monster = smart_cast<CController *>(m_object);
	if (remote_target() && state != eNone) // Anomaly Coop (audit A5): the player's machine plays them
	{
		m_sound_state = state;
		return;
	}
	if (state == ePrepare)
	{
		monster->m_sound_tube_prepare.play_at_pos(Actor(), Fvector().set(0.f, 0.f, 0.f), sm_2D);
	}
	else if (state == eStart)
	{
		if (monster->m_sound_tube_prepare._feedback()) monster->m_sound_tube_prepare.stop();

		monster->m_sound_tube_start.play_at_pos(Actor(), Fvector().set(0.f, 0.f, 0.f), sm_2D);
		monster->m_sound_tube_pull.play_at_pos(Actor(), Fvector().set(0.f, 0.f, 0.f), sm_2D);
	}
	else if (state == eHit)
	{
		if (monster->m_sound_tube_start._feedback()) monster->m_sound_tube_start.stop();
		if (monster->m_sound_tube_pull._feedback()) monster->m_sound_tube_pull.stop();

		//monster->m_sound_tube_hit_left.play_at_pos(Actor(), Fvector().set(-1.f, 0.f, 1.f), sm_2D);
		//monster->m_sound_tube_hit_right.play_at_pos(Actor(), Fvector().set(1.f, 0.f, 1.f), sm_2D);
	}
	else if (state == eNone)
	{
		if (monster->m_sound_tube_start._feedback()) monster->m_sound_tube_start.stop();
		if (monster->m_sound_tube_pull._feedback()) monster->m_sound_tube_pull.stop();
		if (monster->m_sound_tube_prepare._feedback()) monster->m_sound_tube_prepare.stop();
	}

	m_sound_state = state;
}

void CControllerPsyHit::hit()
{
	//CController *monster	= smart_cast<CController *>(m_object);

	set_sound_state(eHit);
	//m_object->Hit_Psy		(Actor(), monster->m_tube_damage);
	if (remote_target()) // Anomaly Coop (audit A5)
		coop::monster_fx::host_send(target(), m_object, coop::monster_fx::kControllerTube, coopHit, 0.f);
}

void CControllerPsyHit::stop()
{
	if (remote_target())
		return; // Anomaly Coop (audit A5): the host's actor was never pulled
	HUD().SetRenderable(true);

	if (CController* controller = smart_cast<CController*>(m_object))
		if (controller->CControlledActor::is_controlling())
			controller->CControlledActor::release();

	// Stop camera effector
	CEffectorCam* ce = Actor()->Cameras().GetCamEffector(eCEControllerPsyHit);
	if (ce)
		Actor()->Cameras().RemoveCamEffector(eCEControllerPsyHit);
}

void CControllerPsyHit::on_death()
{
	if (!is_active())
	{
		coop_abort(); // Anomaly Coop (audit A5): a client's puppet died mid-tube
		return;
	}

	stop();

	m_man->deactivate(this);
}

// ---------------------------------------------------------------- Anomaly Coop (audit A5): client side

void CControllerPsyHit::coop_play(u8 phase, float value)
{
	CController* monster = smart_cast<CController*>(m_object);
	if (!monster || !Actor())
		return;
	if ((phase == coopPrepare || phase == coopStart) && !Actor()->g_Alive())
		return;
	if (g_coop_tube && g_coop_tube != this)
		return; // another controller's tube holds our actor
	if (phase != coopPrepare && phase != coopStart && !m_coop_playing)
		return; // a tube we did not start (another one held us then)
	switch (phase)
	{
	case coopPrepare:
		if (Actor()->Cameras().GetCamEffector(eCEControllerPsyHit))
			return;
		g_coop_tube = this;
		m_coop_playing = true;
		m_coop_deadline = Device.dwTimeGlobal + 15000;
		set_sound_state(ePrepare);
		break;
	case coopStart:
		if (!m_coop_playing && Actor()->Cameras().GetCamEffector(eCEControllerPsyHit))
			return;
		g_coop_tube = this;
		m_coop_playing = true;
		m_coop_deadline = Device.dwTimeGlobal + iFloor(_max(value, 0.f) * 1000.f) + kCoopTubeSlackMs;
		monster->draw_fire_particles_to(Actor());
		glide_actor(value);
		break;
	case coopHit:
		set_sound_state(eHit);
		break;
	case coopEnd:
		monster->draw_fire_particles_to(Actor());
		monster->m_sound_tube_hit_left.play_at_pos(Actor(), Fvector().set(-1.f, 0.f, 1.f), sm_2D);
		monster->m_sound_tube_hit_right.play_at_pos(Actor(), Fvector().set(1.f, 0.f, 1.f), sm_2D);
		stop(); // the damage comes from the host as PlayerHit
		m_coop_deadline = Device.dwTimeGlobal + kCoopTubeSlackMs; // the cancel follows with the last animation
		break;
	default:
		coop_abort();
		break;
	}
}

void CControllerPsyHit::coop_abort()
{
	if (remote_target()) // host: the controller leaves mid-tube; the player's machine lets its actor go
	{
		coop::monster_fx::host_send(target(), m_object, coop::monster_fx::kControllerTube, coopCancel, 0.f);
		coop::monster_fx::host_unhold(m_coop_target, m_object->ID());
		m_coop_target = u16(-1);
	}
	if (!m_coop_playing)
		return;
	m_coop_playing = false;
	if (g_coop_tube == this)
		g_coop_tube = nullptr;
	if (m_blocked && Actor())
	{
		NET_Packet P;
		Actor()->u_EventGen(P, GEG_PLAYER_WEAPON_HIDE_STATE, Actor()->ID());
		P.w_u16(INV_STATE_BLOCK_ALL);
		P.w_u8(u8(false));
		Actor()->u_EventSend(P);
	}
	m_blocked = false;
	set_sound_state(eNone);
	if (Actor())
		stop();
}

void CControllerPsyHit::coop_update()
{
	if (m_coop_playing && Device.dwTimeGlobal > m_coop_deadline)
		coop_abort();
}
