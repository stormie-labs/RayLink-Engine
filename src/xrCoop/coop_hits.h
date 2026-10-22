// Anomaly Coop — which hits on players' characters the host passes on, and which the host player drops (R-5.1, R3 В2).
// The host computes the world's damage. A hit that lands on another player's character (its proxy on the host) goes to
// that player as PlayerHit; the character itself is never damaged on the host. Hits the player's own game computes
// (falls, anomalies, physics) stay on that game.
// Pure C++17, header-only, no engine dependencies: the engine classifies a hit (coop_requests.cpp), these rules decide.
#pragma once

#include <cstdint>

namespace coop
{
namespace hits
{
constexpr uint16_t kNoObject = 0xffff;
// The host's actor. Also the weapon id of a hit built without one (SHit(), Lua hit() with the default weapon_id).
constexpr uint16_t kHostActor = 0;

// Who set a hit off, seen from the character it landed on.
enum class Source : uint8_t
{
	Missing, // the initiator is not online any more
	Self, // the character itself: a fall, its own grenade or rocket, a barrel it shot
	Player, // another player's character: the host's actor or another player's proxy
	Creature, // any other creature: stalkers, monsters, vehicles, helicopters
	Object, // not a creature: a barrel or mine that went off by itself, an anomaly, a physics object, an artefact
};

// The weapon of a hit names the character's own explosive: not the character, not "none", and not the host's actor
// id 0 that a hit built without a weapon carries (R3 В2 review).
inline bool names_own_weapon(uint16_t weapon_id, uint16_t victim_id)
{
	return weapon_id != victim_id && weapon_id != kNoObject && weapon_id != kHostActor;
}

// A blast wave or fragment: the weapon is an explosive. Or its late fragment (R3 В2 review): an explosive that went off
// by itself is its own initiator, and with explode_duration 0 (mines, IEDs, RPG rockets) it is removed a few frames
// after the blast while its fragments still fly — then neither the initiator nor the weapon is online, and the hit
// names one object as both. Online objects that name themselves (artefacts, a poltergeist's flame, collisions) are not
// explosions; a bullet names its gun, not its shooter.
inline bool is_explosion(bool weapon_is_explosive, bool who_online, uint16_t who_id, uint16_t weapon_id)
{
	if (weapon_is_explosive)
		return true;
	return !who_online && who_id != kNoObject && who_id == weapon_id;
}

struct ProxyHit
{
	Source source = Source::Missing;
	bool explosion = false; // the weapon is an explosive: a blast wave or a fragment
	bool own_weapon = false; // the weapon id names something other than the character (and is not "none")
};

// Host: does a hit on a player's character go to that player as PlayerHit? false = dropped.
//   Self      its own explosive: blast and fragments name the explosive as the weapon (a fall names the character)
//   Player    only with friendly fire on
//   Creature  always
//   Object, Missing  only an explosion (R3 В2): a client's explosions are only a picture (WP7), so the player gets their
//             damage from here or not at all; anything else of that kind (anomalies, physics) its own game computes
inline bool to_owner(const ProxyHit& h, bool friendly_fire)
{
	switch (h.source)
	{
	case Source::Self:
		return h.own_weapon;
	case Source::Player:
		return friendly_fire;
	case Source::Creature:
		return true;
	case Source::Missing:
	case Source::Object:
		return h.explosion;
	}
	return false;
}

// Host: does the host's own player drop a hit (CActor::Hit)? Another player's damage with friendly fire off (R3 minor):
// its shots and blows are refused as ReqHit, but its grenade, rocket, charge or the barrel it shot explode on the host
// and hit the host's actor directly. A collision (the contact object names itself as the weapon: the host fell onto
// a client's character) is the host's own fall, not friendly fire.
inline bool host_actor_drops(Source source, bool collision, bool friendly_fire)
{
	return source == Source::Player && !collision && !friendly_fire;
}
} // namespace hits
} // namespace coop
