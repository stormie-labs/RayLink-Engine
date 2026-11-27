////////////////////////////////////////////////////////////////////////////
//	Module 		: stalker_animation_manager_update.cpp
//	Created 	: 25.02.2003
//  Modified 	: 13.12.2006
//	Author		: Dmitriy Iassenev
//	Description : Stalker animation manager update cycle
////////////////////////////////////////////////////////////////////////////

#include "pch_script.h"
#include "stalker_animation_manager.h"
#include "ai/stalker/ai_stalker.h"
#include "game_object_space.h"
#include "script_callback_ex.h"
#include "profiler.h"
#include "stalker_movement_manager_smart_cover.h"
#include "Inventory.h"
#include "Weapon.h"
#include "stalker_animation_data.h"
#include "coop/coop_remote.h"
#include "coop/coop_ai.h" // coop: undrawn_tracks
#include "ik_anim_state.h" // coop: checked_motion_def

void CStalkerAnimationManager::play_delayed_callbacks()
{
	if (m_call_script_callback)
	{
		m_call_script_callback = false;
		object().callback(GameObject::eScriptAnimation)();
		return;
	}

	if (m_call_global_callback)
	{
		m_call_global_callback = false;
		if (m_global_callback)
			m_global_callback();
		return;
	}
}

IC bool CStalkerAnimationManager::script_callback() const
{
	if (script_animations().empty())
		return (false);

	return (object().callback(GameObject::eScriptAnimation));
}

IC bool CStalkerAnimationManager::need_update() const
{
	if (script_callback())
		return (true);

	return (non_script_need_update());
}

IC void CStalkerAnimationManager::update_tracks()
{
	// coop: stock is need_update() alone; a map server, which draws nothing, advances them every frame (undrawn_tracks)
	if (!coop::npc_ai::undrawn_tracks(need_update()))
		return;

	m_skeleton_animated->UpdateTracks();
}

#ifdef USE_HEAD_BONE_PART_FAKE
IC void CStalkerAnimationManager::play_script_impl()
{
	clear_unsafe_callbacks();
	global().reset();
	torso().reset();
	legs().reset();

	const CStalkerAnimationScript& selected = assign_script_animation();
	script().animation(selected.animation());
	if (selected.use_movement_controller())
	{
		script().target_matrix(selected.transform(object()));
		if (m_start_new_script_animation)
		{
			m_start_new_script_animation = false;
			if (selected.has_transform() && object().animation_movement())
				object().destroy_anim_mov_ctrl();
		}
	}

	script().play(
		m_skeleton_animated,
		script_play_callback,
		selected.use_movement_controller(),
		selected.local_animation(),
		false,
		m_script_bone_part_mask
	);

	head().animation(assign_head_animation());
	head().play(m_skeleton_animated, head_play_callback, false, false);
}
#else // USE_HEAD_BONE_PART_FAKE
IC	void CStalkerAnimationManager::play_script_impl			()
{
    clear_unsafe_callbacks	();
    global().reset			();
    head().reset			();
    torso().reset			();
    legs().reset			();

    const CStalkerAnimationScript	&selected = assign_script_animation();
    script().animation		(selected.animation());
    script().play			(
        m_skeleton_animated,
        script_play_callback,
        selected.use_movement_controller(),
        selected.local_animation(),
        false,
        m_script_bone_part_mask
        );
}
#endif // USE_HEAD_BONE_PART_FAKE

bool CStalkerAnimationManager::play_script()
{
	if (script_animations().empty())
	{
		m_start_new_script_animation = false;
		script().reset();
		return (false);
	}

	play_script_impl();

	return (true);
}

#ifdef USE_HEAD_BONE_PART_FAKE
IC void CStalkerAnimationManager::play_global_impl(const MotionID& animation, bool const& animation_movement_controller)
{
	torso().reset();
	legs().reset();

	global().animation(animation);
	global().play(
		m_skeleton_animated,
		global_play_callback,
		animation_movement_controller,
		true,
		false,
		m_script_bone_part_mask,
		true
	);

	if (m_global_modifier)
		m_global_modifier(global().blend());

	head().animation(assign_head_animation());
	head().play(m_skeleton_animated, head_play_callback, false, false);
}
#else // USE_HEAD_BONE_PART_FAKE
IC	void CStalkerAnimationManager::play_global_impl			(const MotionID &animation, bool const &animation_movement_controller)
{
    head().reset			();
    torso().reset			();
    legs().reset			();

    global().animation		(animation);
    global().play			(m_skeleton_animated,global_play_callback,false,false,false);
}
#endif // USE_HEAD_BONE_PART_FAKE

bool CStalkerAnimationManager::play_global()
{
	bool animation_movement_controller = false;
	const MotionID& global_animation = assign_global_animation(animation_movement_controller);
	// coop: a global motion that names no def of this visual is no global motion
	if (!global_animation || !checked_motion_def(m_skeleton_animated, global_animation))
	{
		clear_unsafe_callbacks();
		global().reset();
		return (false);
	}

	play_global_impl(global_animation, animation_movement_controller);

	return (true);
}

IC void CStalkerAnimationManager::play_head()
{
	head().animation(assign_head_animation());
	head().play(m_skeleton_animated, head_play_callback, false, false);
}

IC void CStalkerAnimationManager::play_torso()
{
	// coop: a torso motion the visual has not (a shotgun's tri-state reload_1 / reload_2, an empty table) comes as an
	// invalid or garbage MotionID and faults in PlayCycle; the stock catch(...) reset the pairs every frame and the IK met
	// the freed blends. Such a motion is replaced with the one the stalker plays with nothing in hands; a visual without
	// even that keeps its torso still this frame.
	MotionID animation = assign_torso_animation();
	if (!checked_motion_def(m_skeleton_animated, animation))
	{
		string128 motion;
		if (animation.valid())
			xr_sprintf(motion, "MotionID %u:%u", u32(animation.slot), u32(animation.idx));
		else
			xr_strcpy(motion, "name unavailable");

		// The selection has already discarded the name. A non-panic reload has a known naming scheme and stage.
		CWeapon* weapon = object().inventory().ActiveItem() ? m_weapon : nullptr;
		if (!animation.valid() && m_skeleton_animated && weapon && weapon->GetState() == CWeapon::eReload &&
			object().movement().mental_state() != MonsterSpace::eMentalStatePanic &&
			!object().CObjectHandler::weapon_strapped(weapon))
		{
			const u32 state = u32(body_state());
			const u32 slot = weapon->animation_slot();
			const u32 stage = u32(weapon->GetReloadState());
			const auto& states = m_data_storage->m_part_animations.A;
			if (state < states.size() && slot < states[state].m_torso.A.size() && stage <= CWeapon::eSubstateReloadEnd)
			{
				string128 candidate;
				xr_sprintf(candidate, "%storso_%sreload_%u", state_names[state], weapon_names[slot], stage);
				if (!m_skeleton_animated->ID_Cycle_Safe(candidate) && !m_skeleton_animated->ID_FX_Safe(candidate))
					xr_strcpy(motion, candidate);
			}
		}

		const shared_str& visual = object().cNameVisual();
		if (m_missing_torso_visual != visual || xr_strcmp(m_missing_torso_motion, motion))
		{
			m_missing_torso_visual = visual;
			xr_strcpy(m_missing_torso_motion, motion);
			static xr_set<std::pair<shared_str, shared_str>> reported;
			if (reported.emplace(visual, motion).second)
				Msg("[coop] stalker [%s] visual [%s]: torso motion [%s] is missing", object().cName().c_str(),
				    visual.c_str(), motion);
		}
		animation = no_object_animation(body_state());
		if (!checked_motion_def(m_skeleton_animated, animation))
		{
			torso().reset();
			return;
		}
	}
	torso().animation(animation);
	torso().play(m_skeleton_animated, torso_play_callback, false, false);
}

void CStalkerAnimationManager::play_legs()
{
	float speed = 0.f;
	bool first_time = !legs().animation();
	bool result = legs().animation(assign_legs_animation());

	if (!first_time && !result && legs().blend())
	{
		float amount = legs().blend()->blendAmount;
		m_previous_speed = (m_target_speed - m_previous_speed) * amount + m_previous_speed;
	}

	legs().play(m_skeleton_animated, legs_play_callback, false, false, !fis_zero(m_target_speed));

	if (result && legs().blend())
	{
		float amount = legs().blend()->blendAmount;
		speed = (m_target_speed - m_previous_speed) * amount + m_previous_speed;
	}

	if (fis_zero(speed))
		return;

	if (!legs().blend())
		return;

	object().movement().setup_speed_from_animation(speed);
}

void CStalkerAnimationManager::update_impl()
{
	if (!object().g_Alive())
		return;

	update_tracks();
	play_delayed_callbacks();
#ifdef HOLDERCUSTOM_NEW
	if (object().Holder())
		return;
#endif

	if (play_script())
		return;

	if (play_global())
		return;

	play_head();
	play_torso();
	play_legs();

	torso().synchronize(m_skeleton_animated, m_legs);
}

void CStalkerAnimationManager::update()
{
	if (coop::remote::is_remote_creature(m_object)) return; // coop: puppets play host motions
	START_PROFILE("stalker/client_update/animations")
		try
		{
			update_impl();
		}
		catch (...)
		{
			Msg("! error in stalker [%s], profile [%s] with visual [%s]",
				object().cNameSect().c_str(),
				object().cast_inventory_owner()->CharacterInfo().GetSpecificCharacterId().c_str(),
				object().cNameVisual().c_str());

			for (auto& invItem : m_object->cast_inventory_owner()->m_inventory->m_all)
			{
				Msg("# Inventory item: [%s] - %s", invItem->m_section_id.c_str(), invItem->m_name.c_str());
			}

			/* avo: prevent game from crashing */
			global().reset();
			head().reset();
			torso().reset();
			legs().reset();
			return;
			//throw;
			/* avo: end */
		}
	STOP_PROFILE
}
