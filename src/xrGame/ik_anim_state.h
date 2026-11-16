#pragma once
class CBlend;
class ENGINE_API motion_marks;
class IKinematicsAnimated;
class CMotionDef;
struct MotionID;

// Anomaly Coop (RL-4BXZ-R24J, build 51): the motion def of a MotionID, or NULL when it names no def of this skeleton
// (LL_GetMotionDef / PlayCycle index the slots and the defs unchecked).
CMotionDef* checked_motion_def(IKinematicsAnimated* K, const MotionID& id);
// The same for a blend, NULL also when the blend is free.
CMotionDef* ik_blend_motion_def(IKinematicsAnimated* K, const CBlend* b);

class ik_anim_state
{
	bool is_step;
	bool do_glue;
	bool is_idle;
	bool is_blending;
	const CBlend* current_blend;
public:
	ik_anim_state(): is_step(false), do_glue(true), is_idle(false), is_blending(false), current_blend(0)
	{
	};
	void update(IKinematicsAnimated* K, const CBlend* b, u16 interval);
	IC bool step() const { return is_step; }
	IC bool glue() const { return do_glue; }
	IC bool idle() const { return is_idle; }
	IC bool blending() const { return is_blending; }
	IC bool auto_unstuck() const { return true /*|| idle() || blending()*/; }
	static bool time_step_begin(IKinematicsAnimated* K, const CBlend& B, u16 limb_id, float& time);
};
