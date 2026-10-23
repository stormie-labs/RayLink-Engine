// Anomaly Coop: "viewers" of the host's scheduler (owner, live test of build 10, 2026-10-24).
//
// The bug: "far from the host A-Life lives wrongly: when a client walks up to somebody, they walk slowly". The host
// simulates the whole world and a client only mirrors it, but the engine decides how often an object is updated by its
// distance to ONE point, the host's camera:
//   - CObject::shedule_Scale() = distance to the camera / 200; the scheduler turns the scale into the time between two
//     updates of the object: from t_min (>= 30 ms) up to (1000 + t_max) / 2 ms. A stalker or a mutant (t_min 100,
//     t_max 250) is thought about every 100 ms next to the camera and every 625 ms from 200 m on.
//   - Movement is integrated in UpdateCL (CMovementManager::on_frame with client_update_fdelta(), which is capped at
//     100 ms). UpdateCL runs every frame only for "crows": objects within 30 m of the camera, or visible within 60 m
//     (CObject::UpdateCL). All others get one UpdateCL after each scheduler step. So an NPC 300 m from the host's camera
//     but next to a client walks 100 ms per 625 ms = 16% of its speed, and the client's own character on the host (which
//     the AI of every NPC looks at) moves in jumps for the same reason.
//
// The fix: the host's camera plus the character of every player in the world are viewers. An object is updated by its
// distance to the NEAREST viewer (the rule below), and an NPC within kDefaultFrameRadius of a viewer becomes a crow like
// within the camera's circle. Without a session, or with nobody else in the world, the list is empty and every answer is
// the stock one, bit for bit.
//
// Pure functions, no engine types: xrEngine/sheduler_viewers.h keeps the list, xrGame/coop/coop_ai.cpp fills it once a
// frame (host only), CObject::shedule_Scale, CCustomMonster::AlwaysTheCrow and CStepManager::update ask. Tests:
// tests/test_viewers.cpp.
#pragma once

#include <cmath>

namespace coop
{
namespace viewers
{
struct Point
{
	float x, y, z;
};

constexpr unsigned kMaxViewers = 8; // a host plus seven clients (kMaxPlayers); the host's camera is not in the list
constexpr float kScaleDistance = 200.f; // CObject::shedule_Scale: distance / 200
constexpr float kDefaultRange = 100.f; // m: a viewer matters for objects this close to it (console coop_viewer_range)
constexpr float kMaxRange = 300.f;
constexpr float kDefaultFrameRadius = 60.f; // m: NPCs this close are updated every frame (CROW_RADIUS2: the camera's circle)
constexpr float kMaxFrameRadius = 150.f; // the engine's online radius (alife switch_distance) is 150 m
constexpr unsigned kFreshMs = 1000; // the list is rewritten every frame; older than this it is not used
constexpr unsigned kMovementStepCapMs = 100; // CCustomMonster::UpdateCL: one movement step is at most this long

inline float distance_sqr(const Point& a, const Point& b)
{
	const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
	return dx * dx + dy * dy + dz * dz;
}

// Distance an object at `at` is updated by: `camera_distance` (to the host's camera, as stock) or the distance to the
// nearest viewer that is within `range` of it, whichever is smaller. range <= 0 (or NaN) = no viewer counts.
inline float nearest_distance(float camera_distance, const Point* viewers, unsigned count, const Point& at, float range)
{
	float best = camera_distance;
	if (!(range > 0.f) || !viewers)
		return best;
	const float range_sqr = range * range;
	for (unsigned i = 0; i < count; ++i)
	{
		const float d2 = distance_sqr(viewers[i], at);
		if (d2 > range_sqr)
			continue;
		const float d = std::sqrt(d2);
		if (d < best)
			best = d;
	}
	return best;
}

// CObject::shedule_Scale for that distance (stock: Device.vCameraPosition.distance_to(Position()) / 200).
inline float shedule_scale(float nearest_distance_m) { return nearest_distance_m / kScaleDistance; }

// Is any viewer within `radius` of `at`? (NPCs there are updated every frame.) radius <= 0 (or NaN): never.
inline bool within(const Point* viewers, unsigned count, const Point& at, float radius)
{
	if (!(radius > 0.f) || !viewers)
		return false;
	const float radius_sqr = radius * radius;
	for (unsigned i = 0; i < count; ++i)
		if (distance_sqr(viewers[i], at) <= radius_sqr)
			return true;
	return false;
}

// The every-frame radius actually used: never wider than the range, 0 when either is off.
inline float frame_radius(float range, float frame)
{
	if (!(range > 0.f) || !(frame > 0.f))
		return 0.f;
	return frame < range ? frame : range;
}

// A list written at `stamp_ms` is still current at `now_ms` (unsigned millisecond clock; wraps after 49 days).
inline bool fresh(unsigned now_ms, unsigned stamp_ms) { return unsigned(now_ms - stamp_ms) <= kFreshMs; }
} // namespace viewers
} // namespace coop
