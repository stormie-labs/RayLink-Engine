////////////////////////////////////////////////////////////////////////////
//	Module 		: stalker_torso_animation.cpp
//	Created 	: 19.11.2004
//  Modified 	: 19.11.2004
//	Author		: Dmitriy Iassenev
//	Description : Torso animations for monster "Stalker"
////////////////////////////////////////////////////////////////////////////

#include "stdafx.h"
#include "stalker_animation_manager.h"
#include "ai/stalker/ai_stalker.h"
#include "inventory.h"
#include "weapon.h"
#include "missile.h"
#include "object_handler_space.h"
#include "object_handler_planner.h"
#include "stalker_movement_manager_smart_cover.h"
#include "entitycondition.h"
#include "stalker_animation_data.h"
#include "stalker_animation_manager_impl.h"

const u32 need_look_back_time_interval = 2000;

// coop: CAniVector::Load keeps only the motions the visual has, so A[i] of a missing one
// (a shotgun's tri-state reload_1 / reload_2 on a visual with reload_0 only) read past the vector: a garbage MotionID that
// faulted in PlayCycle (the stock catch(...) of CStalkerAnimationManager::update, 151 times) and then in the IK. A motion
// that is not there is an invalid MotionID now; play_torso falls back to no_object_animation.
IC MotionID motion_at(const ANIM_VECTOR& animations, u32 index)
{
	return index < animations.size() ? animations[index] : MotionID();
}

// The torso tables of a weapon's animation_slot (11 of them, weapon_names): an item's ltx may name any slot.
IC bool torso_slot_known(const CStalkerAnimationData& data, const EBodyState& body_state, u32 slot)
{
	return u32(body_state) < data.m_part_animations.A.size() && slot < data.m_part_animations.A[body_state].m_torso.A.size();
}

MotionID CStalkerAnimationManager::aim_animation(const u32& slot, const xr_vector<CAniVector>& animation,
                                                 const u32& index) const
{
	if (!m_special_danger_move)
		return (motion_at(animation[6].A, index));

	if (slot != 2)
		return (motion_at(animation[6].A, index));

	if (animation[6].A.size() < 7)
	{
		//Alundaio: No need for message as many npcs have special danger move and very few don't
#ifdef DEBUG
		Msg							(
			"! cannot find special danger animations for object with visual %s",
			object().cNameVisual().c_str()
		);
#endif
		//-Alundaio
		return (motion_at(animation[6].A, index));
	}

	switch (index)
	{
	case 0: return (motion_at(animation[6].A, 4));
	case 2: return (motion_at(animation[6].A, 5));
	case 3: return (motion_at(animation[6].A, 6));
	default: NODEFAULT;
	}

#ifdef DEBUG
	return							(MotionID());
#endif // DEBUG
}

void CStalkerAnimationManager::torso_play_callback(CBlend* blend)
{
	CAI_Stalker* object = (CAI_Stalker*)blend->CallbackParam;
	VERIFY(object);

	CStalkerAnimationManager& animation = object->animation();
	CStalkerAnimationPair& pair = animation.torso();
	pair.on_animation_end();

	if (animation.m_looking_back)
	{
		animation.m_change_direction_time = Device.dwTimeGlobal + need_look_back_time_interval;
		animation.m_looking_back = 0;
	}
}

MotionID CStalkerAnimationManager::no_object_animation(const EBodyState& body_state) const
{
	const CAI_Stalker& stalker = object();
	const stalker_movement_manager_smart_cover& movement = stalker.movement();
	const xr_vector<CAniVector>& animation = m_data_storage->m_part_animations.A[body_state].m_torso.A[0].A;

	if (eMentalStateFree == movement.mental_state())
	{
		VERIFY3(
			eBodyStateStand == movement.body_state(),
			"Cannot run FREE animations, when body state is not stand!",
			*stalker.cName()
		);

		if (standing())
			return (motion_at(animation[9].A, 1));

		return (motion_at(animation[7 + movement.movement_type()].A, 1));
	}

	if (standing())
		return (aim_animation(0, animation, 0));

	if (eMovementTypeWalk == movement.movement_type())
		return (aim_animation(0, animation, 2));

	VERIFY(eMovementTypeRun == movement.movement_type());
	return (aim_animation(0, animation, 3));
}

MotionID CStalkerAnimationManager::unknown_object_animation(u32 slot, const EBodyState& body_state) const
{
	// animation shortcuts
	typedef CStalkerAnimationState STATE;
	if (!torso_slot_known(*m_data_storage, body_state, slot) || !torso_slot_known(*m_data_storage, eBodyStateStand, slot))
		return (no_object_animation(body_state)); // coop: a motion the visual has not
	const xr_vector<STATE>& part_animations = m_data_storage->m_part_animations.A;
	const xr_vector<CAniVector>& animation = part_animations[body_state].m_torso.A[slot].A;
	const xr_vector<CAniVector>& animation_stand = part_animations[eBodyStateStand].m_torso.A[slot].A;

	// stalker shortcuts
	const CAI_Stalker& stalker = object();
	const stalker_movement_manager_smart_cover& movement = stalker.movement();
	u32 id = stalker.CObjectHandler::planner().current_action_state_id();

	switch (id)
	{
	case ObjectHandlerSpace::eWorldOperatorFire1:
	case ObjectHandlerSpace::eWorldOperatorFire2:
	case ObjectHandlerSpace::eWorldOperatorAim1:
	case ObjectHandlerSpace::eWorldOperatorAim2:
	case ObjectHandlerSpace::eWorldOperatorAimingReady1:
	case ObjectHandlerSpace::eWorldOperatorAimingReady2:
	case ObjectHandlerSpace::eWorldOperatorQueueWait1:
	case ObjectHandlerSpace::eWorldOperatorQueueWait2:
		{
			if (standing())
				return (aim_animation(slot, animation, 0));

			if (eMovementTypeWalk == movement.movement_type())
			{
				if ((body_state == eBodyStateStand) && (slot == 2) && need_look_back())
					return (motion_at(animation[13 + m_looking_back - 1].A, 1));
				else
					return (aim_animation(slot, animation, 0));
			}

			if ((body_state == eBodyStateStand) && (slot == 2) && need_look_back())
				return (motion_at(animation[13 + m_looking_back - 1].A, 0));

			VERIFY(eMovementTypeRun == movement.movement_type());
			return (aim_animation(slot, animation, 3));
		}

	case ObjectHandlerSpace::eWorldOperatorStrapping:
		return (motion_at(animation_stand[11].A, 0));
	case ObjectHandlerSpace::eWorldOperatorUnstrapping:
		return (motion_at(animation_stand[12].A, 0));
	case ObjectHandlerSpace::eWorldOperatorStrapping2Idle:
		return (motion_at(animation_stand[11].A, 1));
	case ObjectHandlerSpace::eWorldOperatorUnstrapping2Idle:
		return (motion_at(animation_stand[12].A, 1));
	}

	if (eMentalStateFree == movement.mental_state())
	{
		VERIFY3(
			eBodyStateStand == movement.body_state(),
			"Cannot run FREE animation when body state is not stand!",
			*object().cName()
		);

		if (standing())
			return (motion_at(animation[9].A, 1));

		return (motion_at(animation[7 + movement.movement_type()].A, 1));
	}

	if (standing())
		return (aim_animation(slot, animation, 0));

	if (eMovementTypeWalk == movement.movement_type())
		return (aim_animation(slot, animation, 2));

	VERIFY(eMovementTypeRun == movement.movement_type());

	if (eBodyStateStand == movement.body_state())
		return (aim_animation(slot, animation, 3));

	return (aim_animation(slot, animation, 3));
}

MotionID CStalkerAnimationManager::weapon_animation(u32 slot, const EBodyState& body_state)
{
	if (!torso_slot_known(*m_data_storage, body_state, slot))
		return (no_object_animation(body_state)); // coop: a motion the visual has not
	const xr_vector<CAniVector>& animation = m_data_storage->m_part_animations.A[body_state].m_torso.A[slot].A;

	//Alun: Fix stalker sprint
	if (eMentalStatePanic == object().movement().mental_state() && eMovementTypeRun == object()
	                                                                                   .movement().movement_type() &&
		body_state == eBodyStateStand && !fis_zero(
			object().movement().speed(object().character_physics_support()->movement())))
	{
		return (motion_at(animation[15].A, 0));
	}

	switch (m_weapon->GetState())
	{
	case CWeapon::eReload:
		{
			switch (m_weapon->GetReloadState())
			{
			case CWeapon::eSubstateReloadBegin:
				return (motion_at(animation[4].A, 0));
			case CWeapon::eSubstateReloadInProcess:
				return (motion_at(animation[4].A, 1));
			case CWeapon::eSubstateReloadEnd:
				return (motion_at(animation[4].A, 2));

			default: NODEFAULT;
			}
#ifdef DEBUG
			return					(motion_at(animation[4].A, 0));
#endif
		}
	case CWeapon::eShowing:
		return (torso().select(animation[0].A));
	case CWeapon::eHiding:
		return (torso().select(animation[3].A));
	case CWeapon::eHidden:
		return (no_object_animation(body_state));
	case CWeapon::eFire:
	case CWeapon::eFire2:
		{
			CAI_Stalker& stalker = object();
			stalker_movement_manager_smart_cover& movement = stalker.movement();
			if (standing())
				return (motion_at(animation[1].A, 0));

			if (eMovementTypeWalk == movement.movement_type())
			{
				if ((body_state == eBodyStateStand) && (slot == 2) && need_look_back())
					return (motion_at(animation[13 + m_looking_back - 1].A, 1/**1/**/));
				else
					return (motion_at(animation[1].A, 0/**2/**/));
			}

			if ((body_state == eBodyStateStand) && (slot == 2) && need_look_back())
				return (motion_at(animation[13 + m_looking_back - 1].A, 0));

			VERIFY(eMovementTypeRun == movement.movement_type());
			return (motion_at(animation[1].A, 3));
		}
	}

	return (unknown_object_animation(slot, body_state));
}

MotionID CStalkerAnimationManager::missile_animation(u32 slot, const EBodyState& body_state)
{
	VERIFY(m_missile);

	//	if (body_state == eBodyStateCrouch)
	//		slot						= 0;

	if (!torso_slot_known(*m_data_storage, body_state, slot))
		return (no_object_animation(body_state)); // coop: a motion the visual has not
	const xr_vector<CAniVector>& animation = m_data_storage->m_part_animations.A[body_state].m_torso.A[slot].A;
	//	const xr_vector<CAniVector>		&animation = m_data_storage->m_part_animations.A[eBodyStateStand].m_torso.A[slot].A;

	switch (m_missile->GetState())
	{
	case CMissile::eShowing:
		{
#ifdef DEBUG
			if (animation[0].A.empty()) {
				Msg					("! visual %s", object().cNameVisual().c_str());
			}
#endif // #ifdef DEBUG
			return (torso().select(animation[0].A));
		}
	case CMissile::eHiding:
		{
#ifdef DEBUG
			if (animation[3].A.empty()) {
				Msg					("! visual %s", object().cNameVisual().c_str());
			}
#endif // #ifdef DEBUG
			return (torso().select(animation[3].A));
		}
	case CMissile::eThrowStart:
		{
			//			Msg						("CMissile::eThrowStart");
#ifdef DEBUG
			if (animation[1].A.empty()) {
				Msg					("! visual %s", object().cNameVisual().c_str());
			}
#endif // #ifdef DEBUG
			return (motion_at(animation[1].A, 0));
		}
	case CMissile::eReady:
		{
			//			Msg						("CMissile::eReady");
#ifdef DEBUG
			if (animation[1].A.size() < 2) {
				Msg					("! visual %s", object().cNameVisual().c_str());
			}
#endif // #ifdef DEBUG
			return (motion_at(animation[1].A, 1));
		}
	case CMissile::eThrow:
		{
			//			Msg						("CMissile::eThrow");
#ifdef DEBUG
			if (animation[1].A.size() < 3) {
				Msg					("! visual %s", object().cNameVisual().c_str());
			}
#endif // #ifdef DEBUG
			return (motion_at(animation[1].A, 2));
		}
	case CMissile::eThrowEnd:
		{
#ifdef DEBUG
			if (animation[6].A.empty()) {
				Msg					("! visual %s", object().cNameVisual().c_str());
			}
#endif // #ifdef DEBUG
			//			Msg						("CMissile::eThrowEnd");
			return (motion_at(animation[6].A, 0));
		}
	case CMissile::eBore:
		{
#ifdef DEBUG
			if (animation[1].A.size() < 2) {
				Msg					("! visual %s", object().cNameVisual().c_str());
			}
#endif // #ifdef DEBUG
			return (motion_at(animation[1].A, 1));
		}
	case CMissile::eHidden:
		{
#ifdef DEBUG
			if (animation[6].A.empty()) {
				Msg					("! visual %s", object().cNameVisual().c_str());
			}
#endif // #ifdef DEBUG
			return (motion_at(animation[6].A, 0));
		}
	case CMissile::eIdle:
	default:
		{
			CAI_Stalker& stalker = object();
			stalker_movement_manager_smart_cover& movement = stalker.movement();
			if (standing())
			{
#ifdef DEBUG
				if (animation[6].A.empty()) {
					Msg				("! visual %s", object().cNameVisual().c_str());
				}
#endif // #ifdef DEBUG
				return (motion_at(animation[6].A, 0));
			}

			if (eMovementTypeWalk == movement.movement_type())
			{
#ifdef DEBUG
				if (animation[6].A.size() < 3) {
					Msg				("! visual %s", object().cNameVisual().c_str());
				}
#endif // #ifdef DEBUG
				return (motion_at(animation[6].A, 2));
			}

#ifdef DEBUG
			if (animation[6].A.size() < 4) {
				Msg					("! visual %s", object().cNameVisual().c_str());
			}
#endif // #ifdef DEBUG
			return (motion_at(animation[6].A, 3));
		}
	}
}

MotionID CStalkerAnimationManager::assign_torso_animation()
{
	EBodyState body_state = this->body_state();

	if (!object().inventory().ActiveItem())
		return (no_object_animation(body_state));

	fill_object_info();

	if (m_weapon)
	{
		if (!strapped())
			return (weapon_animation(object_slot(), body_state));

		return (no_object_animation(body_state));
	}

	if (m_missile)
		return (missile_animation(object_slot(), body_state));

	return (unknown_object_animation(object_slot(), body_state));
}
