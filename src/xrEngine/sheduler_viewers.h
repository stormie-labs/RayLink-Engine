// Anomaly Coop: "viewers" of the scheduler (owner, live test of build 10, 2026-10-24: "far from the host, A-Life lives
// wrongly: when a client walks up to somebody, they walk slowly").
//
// The engine updates an object by its distance to ONE point, the camera of the machine that runs the world:
//   - CObject::shedule_Scale() (distance / 200) sets the time between two scheduler steps of the object;
//   - CObject::UpdateCL() makes an object a "crow" (UpdateCL every frame) only within 30 m of the camera (60 m if visible);
//     CCustomMonster integrates its movement in UpdateCL with a step capped at 100 ms, so an NPC that is not a crow walks
//     100 ms per scheduler step (625 ms far away) = ~1/6 of its speed.
// In a co-op session the host runs the world for everybody, so the host's camera is not the only place that is looked at.
// The co-op code (xrGame/coop/coop_ai.cpp, host only) lists the position of every other player's character here once a
// frame. The rule is the pure code of xrCoop/coop_viewers.h (tests/test_viewers.cpp); this file only keeps the list.
//
// With an empty list (single player, no session, a client, a host nobody has joined, a list not refreshed for a second)
// every answer is the stock one, bit for bit.
//
// Threading: the writer runs on the main thread inside the frame (FrameMove), where the game thread (scheduler) does not
// run; the readers run on the game thread and on the main thread. The count is published last; a torn position read
// would mean one slightly wrong distance, nothing more.
#pragma once

namespace shedule_viewers
{
// Writer (host, main thread, once a frame): replaces the list. `range` (m): a viewer counts for objects this close to it,
// 0 = viewers off. `frame_radius` (m): NPCs this close to a viewer are updated every frame, 0 = scale only. More than
// coop::viewers::kMaxViewers positions are cut.
ENGINE_API void set(const Fvector* positions, u32 count, float range, float frame_radius);
// Writer: no viewers (the list is also ignored once it is a second old).
ENGINE_API void clear();

// Reader: the distance an object at `at` is "looked at" from: to the host's camera, or to the nearest viewer within the
// range if that is closer. CObject::shedule_Scale divides it by 200; CStepManager compares it with its 50 m (steps of an
// NPC are played, and so heard by the other NPCs, only within 50 m of the camera).
ENGINE_API float nearest_distance(const Fvector& at);
// Reader: is an NPC at `at` within the every-frame radius of a viewer (CCustomMonster::AlwaysTheCrow)?
ENGINE_API bool frame_update(const Fvector& at);

struct Stats
{
	u32 viewers = 0; // in the current list (0 when stale)
	float range = 0.f;
	float frame_radius = 0.f;
	u64 scale_queries = 0; // shedule_Scale calls while viewers were listed
	u64 scale_boosted = 0; // ...where a viewer was closer than the camera (the interval got shorter)
	u64 frame_queries = 0; // AlwaysTheCrow calls of NPCs while viewers were listed
	u64 frame_boosted = 0; // ...that made an NPC outside the camera's 30 m circle a crow
};
ENGINE_API Stats stats();
} // namespace shedule_viewers
