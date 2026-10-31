// Anomaly Coop — the rules behind the fixes of the enemy audit (2026-10-31): what a monster does about a remote player.
// Pure C++17, no engine dependencies. Tests: tests/test_enemy_rules.cpp.
#pragma once

#include <cmath>
#include <cstdint>

#include "coop_protocol.h" // CreatureFlags

namespace coop
{
namespace enemy_rules
{
constexpr float kViewCos = 0.57f; // half view cone of a player ~55 degrees (screen plus a little side vision)
constexpr float kAwareDistance = 2.f; // that close a player notices a creature whatever he looks at

// F1: a player notices a monster. The stock asks the player's own eyes, never the monster's: a monster that sleeps or
// has its back turned sees nothing, and it is the one this alerts. A proxy has no eyes, so its reported view stands in
// (distance and view_cos from the player's head to the monster; an unknown view counts as looking).
inline bool player_notices(float distance, bool view_known, float view_cos, bool line_clear)
{
	return line_clear && (distance <= kAwareDistance || !view_known || view_cos >= kViewCos);
}

struct Point
{
	float x, y, z;
};

inline float distance(const Point& a, const Point& b)
{
	const float x = a.x - b.x, y = a.y - b.y, z = a.z - b.z;
	return std::sqrt(x * x + y * y + z * z);
}

// The point of the step a -> b nearest to p: a thrown object covers a metre and more between two samples.
inline Point closest_on_segment(const Point& a, const Point& b, const Point& p)
{
	const float dx = b.x - a.x, dy = b.y - a.y, dz = b.z - a.z;
	const float len2 = dx * dx + dy * dy + dz * dz;
	if (len2 <= 0.f)
		return a;
	float t = ((p.x - a.x) * dx + (p.y - a.y) * dy + (p.z - a.z) * dz) / len2;
	t = t < 0.f ? 0.f : t > 1.f ? 1.f : t;
	return Point{a.x + dx * t, a.y + dy * t, a.z + dz * t};
}

// F4: a proxy takes no collision damage (a puppet skips the movement update that applies it), so the host measures a
// thrown object's flight past him.
constexpr float kThrowReach = 1.2f; // from the middle of the body: the capsule plus the object's own size
constexpr float kThrowBodyHeight = 1.f;
constexpr uint32_t kThrowFlightMs = 3000; // FIRE_TIME of CTelekineticObject
constexpr float kThrowPowerBySpeed = -1.f; // no power of its own: the burer's throw, as hard as it flies

// CPHSimpleCharacter::UpdateDynamicDamage for a body at rest: the speed of the energy it takes in.
inline float contact_speed(float object_speed, float object_mass, float body_mass)
{
	if (object_mass <= 0.f || body_mass <= 0.f)
		return 0.f;
	return object_speed * std::sqrt(object_mass / (object_mass + body_mass));
}

// The hit of that contact: under half the crash speed nothing (the stock hit callbacks), else `fixed_power`, or
// without one what CPHMovementControl::UpdateCollisionDamage computes. The speed here is measured from two samples, not
// taken from a real contact, so it is capped at one full crash.
inline float thrown_power(float contact, float min_crash, float max_crash, float fixed_power)
{
	if (max_crash <= min_crash || contact <= min_crash * 0.5f)
		return 0.f;
	if (fixed_power >= 0.f)
		return fixed_power;
	if (contact <= min_crash)
		return 0.f;
	const float power = (contact - min_crash) / (max_crash - min_crash);
	return power < 1.f ? power : 1.f;
}

// F5: how a monster looks, which only its AI on the host knows: a bloodsucker's cloak, a poltergeist or a psy dog's phantom
// out of sight. It rides in the snapshot's creature flags, in the two bits that mean something for stalkers only and
// bring no data after the anims (CreatureHolds and CreatureAims do, for any creature).
enum class MonsterLook : uint8_t
{
	Shown,
	CloakPartial,
	CloakFull,
	Hidden,
};

inline uint8_t look_flags(MonsterLook look)
{
	switch (look)
	{
	case MonsterLook::CloakPartial: return uint8_t(CreatureTorchLit);
	case MonsterLook::CloakFull: return uint8_t(CreatureStrapped);
	case MonsterLook::Hidden: return uint8_t(CreatureTorchLit | CreatureStrapped);
	default: return 0;
	}
}

inline MonsterLook look_of(uint8_t flags)
{
	const bool one = (flags & CreatureTorchLit) != 0, two = (flags & CreatureStrapped) != 0;
	return one && two ? MonsterLook::Hidden : one ? MonsterLook::CloakPartial : two ? MonsterLook::CloakFull : MonsterLook::Shown;
}

// F3c: the host hears when a client's reload starts (PlayerShot kind Reload), not when it ends. `reported_ms` 0: never.
constexpr uint32_t kReloadShownMs = 2500;

inline bool reload_shown(uint32_t now_ms, uint32_t reported_ms)
{
	return reported_ms != 0 && now_ms - reported_ms < kReloadShownMs;
}

// F10: a psy dog's phantom vanishes at a shot by its target or by any player. The host decides.
inline bool phantom_dispelled(bool has_author, bool author_is_its_enemy, bool author_is_player, bool puppet)
{
	return !puppet && has_author && (author_is_its_enemy || author_is_player);
}
} // namespace enemy_rules
} // namespace coop
