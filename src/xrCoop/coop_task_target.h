// Anomaly Coop - where a task's target is, for a client's frozen ALife (tasks audit C1): when the host sends a new place.
// Pure C++17, header-only, no engine dependencies: used by xrGame/coop/coop_story.cpp and by the unit-test exe
// (tests/test_task_target.cpp).
//
// A client's ALife does not run. The spot of a task target that is offline there, its edge pointer and the coop compass
// read the client's copy of the server entity, and nothing moves that copy while the entity stays offline, so a roaming
// target would stay where it stood when the client joined. The host sends the place of every running task's target:
// the first time, then when it moved far enough or to another game vertex, at most once per kMinGapMs.
#pragma once

#include <cstdint>

namespace coop
{
namespace task_target
{
constexpr float kMoveMetres = 5.f;
constexpr uint64_t kMinGapMs = 2000;

struct Place
{
	float x = 0, y = 0, z = 0;
	uint16_t game_vertex = 0xffff;
};

inline bool moved(const Place& a, const Place& b)
{
	if (a.game_vertex != b.game_vertex)
		return true;
	const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
	return dx * dx + dy * dy + dz * dz >= kMoveMetres * kMoveMetres;
}

// sent_at == 0: never sent.
inline bool due(const Place& sent, uint64_t sent_at, const Place& now, uint64_t now_ms)
{
	if (sent_at == 0)
		return true;
	return now_ms - sent_at >= kMinGapMs && moved(sent, now);
}
} // namespace task_target
} // namespace coop
