// Ядро: без проверок режима (docs/MODES.md)
// Anomaly Coop - TZ P50 (Dev 58): players who appear on one spot or crowd at a level exit are not pushed into the walls and
// under the level by each other.
//
// The owner, 2026-11-22: in the narrow tunnel of Cordon -> Garbage "the ones that do not fit just spawn under the map"; his
// decision: at level exits and at the places where players appear, collision between players is off, and it comes back when
// the player moves a set distance away.
//
// Who collides with whom. Every machine that plays an actor simulates that actor's character (CPHActorCharacter) against the
// other players' characters, which are avatars here (puppets whose capsule coop_remote.cpp places from the snapshots every
// frame: a body that never yields). A pair that overlaps deeply is pushed apart on the actor's side alone: the contact of two
// bodies is ten times stiffer than the ground's (PHSimpleCharacter.cpp InitContact), the actor's restrictors add one-sided joints
// (PHActorCharacter.cpp InitContact) and CPHMovementControl::UpdateObjectBox runs extra world steps to push the actor out
// (PHSimpleCharacter.cpp UpdateRestrictionType). Between the walls of a tunnel the only way out is through the static
// geometry. Where two players overlap: a group arrival puts every mover whose ring spot is in the wall on the first free spot
// (coop_arrival.h place: the same spot for all of them), and an arrival alone takes the designer's point, where another
// player may stand.
//
// The rule, for one machine and one pair (its actor and one avatar), from what that machine sees:
//  - a pair starts off when either of the two appears in this machine's world (its object spawns here: an arrival, a join, a
//    load; appeared());
//  - a pair is off while either of the two is at a level exit (near_exit: 2 m of the exit's shape or inside it, away again past
//    4 m);
//  - an off pair comes on only when the two are kApart (3 m) apart and neither is at an exit; an on pair stays on however
//    close they come (two players bump into each other as in a usual game);
//  - the distance counts only once the avatar has been placed from its own player's snapshots since it appeared here
//    (apart_known): before that it stands where this machine's world spawned it, not where its player is.
// Only players: the engine marks the characters of players with a class of their own, an avatar whose pair is off passes
// that class alone (bodies_meet); NPCs, monsters, items and the level are not touched. Single player marks nothing.
// Pure C++17, header-only, no engine dependencies: used by xrGame (coop_player_touch.cpp) and by the unit-test exe.
#pragma once

#include <cmath>
#include <limits>

namespace coop
{
namespace player_touch
{
// Within this of a level exit's shape (or inside it) a player is at the exit: the queue in front of a narrow exit is two or
// three capsules deep.
constexpr float kExitNear = 2.f;
// ... and away from it again past this: 2 m of band, so a player standing at the edge does not switch every frame.
constexpr float kExitFar = 4.f;
// Two players whose pair is off get their bodies back this far apart: the host's ring of one transition is 1.5 m around the
// exit's point (3 m across), and two capsules touch at about 0.75 m, which leaves more than 2 m for the delay of an avatar.
constexpr float kApart = 3.f;

// One player's "at a level exit" this frame. exit_distance: metres to the nearest exit shape of the level, 0 inside, infinity
// when the level has none; not a number keeps what it was.
inline bool near_exit(bool was_near, float exit_distance)
{
	if (std::isnan(exit_distance))
		return was_near;
	return exit_distance <= (was_near ? kExitFar : kExitNear);
}

// One of the two appeared in this machine's world: its spot may be the other's.
inline void appeared(bool& off) { off = true; }

// The metres between this machine's actor and an avatar as the pair may use them. placed: the avatar has been put where its
// own player stands (from that player's snapshots) at least once since its object appeared here. Until then it stands where
// this machine's world spawned it: in a group arrival the host's blind ring spot (coop_join.cpp host_place_movers), up to
// 3.4 m from the spot its player's machine put him on (coop_arrival.h place), which may be this actor's own spot. That is no
// distance between the two players: not a number, so an off pair stays off (collides) until the avatar is where he is.
inline float apart_known(bool placed, float apart) { return placed ? apart : std::numeric_limits<float>::quiet_NaN(); }

// One frame of one pair: true when their bodies collide. off: the pair's state (appeared() sets it); my_near / his_near: each
// one's near_exit; apart: metres between the two (not a number never switches the pair on).
inline bool collides(bool& off, bool my_near, bool his_near, float apart)
{
	if (my_near || his_near)
	{
		off = true;
		return false;
	}
	if (off && !(apart >= kApart))
		return false;
	off = false;
	return true;
}

// The engine's test of two bodies under the player class (CPHCollideValidator: cbClassPlayer, and cbNCClassPlayer for an avatar
// whose pair is off): two bodies meet unless one passes players and the other is one.
inline bool bodies_meet(bool a_player, bool a_passes_players, bool b_player, bool b_passes_players)
{
	return !((a_passes_players && b_player) || (b_passes_players && a_player));
}
} // namespace player_touch
} // namespace coop
