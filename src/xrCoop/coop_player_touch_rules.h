// Anomaly Coop - players who appear on one spot or crowd at a level exit are not pushed into the walls and
// under the level by each other.
//
// in the narrow tunnel of Cordon -> Garbage "the ones that do not fit just spawn under the map"; his
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
//  - a pair is off while either of the two is at a level exit (near_exit: within 4 m of the exit's shape or inside it);
//  - a pair is off while either of the two is at an arrival place of the level (near_spawn: within 8 m of it; a zone
//    of its own): where a level exit leading to this level puts who comes (the stock exit's destination point, the host's ring
//    around it). The places are read from the game's own data (the level changers of all.spawn), the same on every machine:
//    a spawn place each machine took from what it saw (who came later, where the other stood then) would be a
//    place one machine had and the other not, as soon as the first one walked off before the second one's machine looked, or
//    one sat in a car; one player would walk through the other while the other one's actor was pushed out of an avatar that never
//    yields;
//  - both flags are where the player stands now and nothing else: no band (in at one distance, out at
//    another) and so no memory. A band remembers how he got there, and two machines remember it differently: his own machine
//    saw him walk out from the place, the machine of one whose pair with him started later (who came by the same exit, a
//    latecomer or the laggard of a group transition, one back from the next map) did not, and had him away at 6-8 m while his
//    own machine had him at the place: the pair on on one machine and off on the other while the bodies touch;
//  - an off pair comes on only when the two are kApart (3 m) apart and neither is at an exit or at an arrival place; an on
//    pair stays on however close they come (two players bump into each other as in a usual game);
//  - the distance counts only once the avatar has been placed from its own player's snapshots since it appeared here
//    (apart_known): before that it stands where this machine's world spawned it, not where its player is.
// Only players: the engine marks the characters of players with a class of their own, an avatar whose pair is off passes
// that class alone (bodies_meet); NPCs, monsters, items and the level are not touched. Single player marks nothing.
// Pure C++17, header-only, no engine dependencies: used by xrGame (coop_player_touch.cpp) and by the unit-test exe.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace coop
{
namespace player_touch
{
// Within this of a level exit's shape (or inside it) a player is at the exit, past it he is away: the queue in front of a narrow
// exit is two or three capsules deep. No band: one at the edge may
// switch his flag from frame to frame, which switches no pair on while the two touch (an off pair comes on only kApart apart).
constexpr float kExitFar = 4.f;
// Two players whose pair is off get their bodies back this far apart: the host's ring of one transition is 1.5 m around the
// exit's point (3 m across), and two capsules touch at about 0.75 m, which leaves more than 2 m for the delay of an avatar.
constexpr float kApart = 3.f;

// One player's "at a level exit" this frame: where he stands, not how he came there (the same on his machine and on every other
// one). exit_distance: metres to the nearest exit shape of the level, 0 inside, infinity when the level has none; not a number
// (no position to tell) keeps what it was.
inline bool near_exit(bool was_near, float exit_distance)
{
	if (std::isnan(exit_distance))
		return was_near;
	return exit_distance <= kExitFar;
}

// The spot where a
// player appears (the exit's destination point, the host's ring around it) is not at the exit: of the 124 arrival points of the
// game none is within 2 m of a stock exit's shape on its level (the nearest 2.2 m), 7 are within kExitFar, 29 within 8 m, the
// median is 14 m (a modded all.spawn, each level changer's shapes placed as the engine places them;
// route restrictors of coop_transition_route.script not counted). So an arrival
// place is a zone of its own: within kSpawnFar of it a player is at it, past it he is away (no band, as at the exits). The host's
// ring puts a crowd within 2.12 m of the point (4.24 m of each other; Server: all on the point), so one who has left the place
// is at least 8 - 4.24 = 3.76 m (more than kApart) from every spot of the arrival, and the pair that comes on is not one in the
// other.
constexpr float kSpawnFar = 8.f;

// One player's "at an arrival place" this frame: a zone of its own, not the exits' flag, and where he stands
// alone, as near_exit. spawn_distance: metres to the nearest arrival place (to_places); not a number (no position to tell: an
// avatar its player has not placed yet) keeps what it was.
inline bool near_spawn(bool was_near, float spawn_distance)
{
	if (std::isnan(spawn_distance))
		return was_near;
	return spawn_distance <= kSpawnFar;
}

inline float distance3(const float a[3], const float b[3])
{
	const float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
	return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// Metres from p to the nearest of `count` arrival places (x, y, z one after the other); infinity when the level has none.
inline float to_places(const float* places, size_t count, const float p[3])
{
	float best = std::numeric_limits<float>::infinity();
	for (size_t i = 0; i < count; ++i)
	{
		const float d = distance3(p, places + 3 * i);
		if (d < best)
			best = d;
	}
	return best;
}

// A snapshot that may place an avatar which appeared here at appeared_ms. players::drive_avatar holds
// the newest snapshot it has; one of a stream that ended before the avatar appeared (no state for stream_gap_ms: his load of a
// level, the host's own load) tells where he stood on the level before, and that first distance switched the pair on. A
// snapshot of the stream he sends now counts, even one that came a moment before his object did.
inline bool snapshot_counts(uint64_t snapshot_ms, uint64_t appeared_ms, uint64_t stream_gap_ms)
{
	return snapshot_ms + stream_gap_ms >= appeared_ms;
}

// The snapshots of an avatar that appeared at appeared_ms, less the ones that may not place it (players::on_object_spawn).
// time_of(snapshot): its time on this machine's clock.
template <class Snapshots, class TimeOf>
void drop_stale(Snapshots& snapshots, uint64_t appeared_ms, uint64_t stream_gap_ms, TimeOf time_of)
{
	snapshots.erase(std::remove_if(snapshots.begin(), snapshots.end(),
	                    [&](const typename Snapshots::value_type& s)
	                    { return !snapshot_counts(time_of(s), appeared_ms, stream_gap_ms); }),
	    snapshots.end());
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

// One pair on one machine: this machine's actor and one avatar (coop_player_touch.cpp keeps one per avatar).
struct Pair
{
	bool off = false; // no contacts between the two bodies
	bool at_exit = false; // the avatar is at a level exit (near_exit)
	bool at_place = false; // the avatar is at an arrival place (near_spawn)
};

// One of the two appeared here (the actor: for every avatar already here; an avatar: its own pair).
inline void pair_start(Pair& p)
{
	p = Pair();
	appeared(p.off);
}

// One frame of one pair (coop_player_touch.cpp frame()): true when the bodies collide. me: this machine's actor; my_near: it is at
// a level exit or at an arrival place; him: the avatar; placed: players::placed (apart_known); his_exit / his_place: metres from
// the avatar to the nearest level exit / arrival place. Both flags of the avatar are where it stands this frame (near_exit,
// near_spawn: no memory), so they are what his own machine has for its actor, as late as his avatar is. Where an avatar that has
// not been placed stands says nothing of its player (apart_known): its arrival flag keeps what it was, and the pair is off until
// it is placed (it started off when the avatar appeared, and an unknown distance never switches it on).
inline bool pair_frame(Pair& p, const float me[3], bool my_near, const float him[3], bool placed, float his_exit, float his_place)
{
	p.at_exit = near_exit(p.at_exit, his_exit);
	p.at_place = near_spawn(p.at_place, placed ? his_place : std::numeric_limits<float>::quiet_NaN());
	return collides(p.off, my_near, p.at_exit || p.at_place, apart_known(placed, distance3(me, him)));
}

// The engine's test of two bodies under the player class (CPHCollideValidator: cbClassPlayer, and cbNCClassPlayer for an avatar
// whose pair is off): two bodies meet unless one passes players and the other is one.
inline bool bodies_meet(bool a_player, bool a_passes_players, bool b_player, bool b_passes_players)
{
	return !((a_passes_players && b_player) || (b_passes_players && a_player));
}
} // namespace player_touch
} // namespace coop
