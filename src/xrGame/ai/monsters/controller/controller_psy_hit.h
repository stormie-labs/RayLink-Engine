#pragma once
#include "../control_combase.h"
#include "../../../../Include/xrRender/KinematicsAnimated.h"
#include "xrCoop/coop_monster_fx_rules.h"

class CPsyHitEffectorCam;
class CEntityAlive;
class CPsyHitEffectorPP;

class CControllerPsyHit : public CControl_ComCustom<>
{
	typedef CControl_ComCustom<> inherited;

	MotionID m_stage[4];
	u8 m_current_index;

	CPsyHitEffectorCam* m_effector_cam;
	CPsyHitEffectorPP* m_effector_pp;

	enum ESoundState
	{
		ePrepare,
		eStart,
		ePull,
		eHit,
		eNone
	} m_sound_state;


	float m_min_tube_dist;

	// internal flag if weapon was hidden
	bool m_blocked;

	u32 m_time_last_tube;

public:
	virtual void load(LPCSTR section);
	virtual void reinit();
	virtual void update_frame();
	virtual bool check_start_conditions();
	virtual void activate();
	virtual void deactivate();

	virtual void on_event(ControlCom::EEventType, ControlCom::IEventData*);

	void on_death();
	bool tube_ready() const;

	// coop: the tube on a remote player's character. The host runs the stages and hits the
	// character (the player gets it as PlayerHit); the player's machine plays each stage on its actor here.
	enum ECoopPhase : u8
	{
		coopPrepare,
		coopStart,
		coopHit,
		coopEnd,
		coopCancel,
	};
	void coop_play(u8 phase, float value);
	void coop_abort(); // client: the puppet died or left mid-tube, or the end never came: the actor is ours again
	void coop_update(); // client: every frame, the tube's watchdog

private:

	void stop();

	void play_anim();
	void death_glide_start();
	void death_glide_end();

	void set_sound_state(ESoundState state);
	void hit();
	bool check_conditions_final();

	// coop: a tube on another player's machine
	u16 m_coop_target = u16(-1); // host: the remote player's character the tube is on; -1: the actor (stock)
	coop::monster_fx::ActorHold m_coop_hold; // client: a host tube plays on our actor
	bool remote_target() const { return m_coop_target != u16(-1); }
	const CEntityAlive* pick_target() const; // the enemy when it is a remote player seen now, else the actor
	const CEntityAlive* target() const; // whom the running tube is on
	void turn_to(const CEntityAlive* who);
	void glide_actor(float motion_time); // the pull on this machine's actor
};
