// Ядро: без проверок режима (docs/MODES.md)
// Anomaly Coop (TZ P101, 6.30, 2026-11-25): what a copy of a creature (a "puppet") plays on a bone partition that no usable
// motion of the decider reaches, and when a copy or the decider is written to the log as a T-pose suspect. Pure rules, no
// engine types (tests/test_mirror_pose_rules.cpp). The engine side: coop_entities.cpp drive_mirror / watch_pose (the copy),
// watch_decider (the decider: the Coop host, or the map server of the level, once a round), coop_remote.cpp apply (the book
// of one copy), coop_dialog.cpp (a talk the decider answers "gone").
//
// Live (owner's screenshot, Dev 64, Server mode, Cordon): the story NPC Wolf stood in a T-pose on a player's machine while
// the NPCs around him moved. What the code says (P101 review, 2026-11-25):
// - CCharacterPhysicsSupport::in_NetSpawn plays "death_init" on channel 0 on every partition of every live NPC. On GAMMA's
//   stalker models that motion is the bind pose (upper arms and forearms 0.0 deg from bind on all 52 frames): arms out.
// - The decider's own animation update replaces it at once. A copy never selects a motion (its animation manager, script
//   animations and SelectAnimation are off), so only drive_mirror replaces it: a partition no usable host motion reaches
//   stays in the T-pose.
// - The 26fe5b0b line could not see it: "death_init" is a live channel-0 blend, and a copy with no snapshot returned
//   before the line.
// Which trigger stopped the motions for Wolf is not known (no snapshot of the id, the decider dead or itself still in its
// placeholder, an id this model has not). The copy now stands in its own idle instead (this file decides when), and the
// lines below name the trigger next time.
#pragma once
#include <cstdint>

namespace coop { namespace mirror_pose {

constexpr uint32_t kNoMotion = 0xffff; // MotionID::invalidate (instrument::kNoMotion)
constexpr uint32_t kMaxParts = 4; // MAX_PARTS of the engine; a stalker model has 3 (legs, torso, head)
// The first moments of a copy: a snapshot comes every 66 ms and is shown 120 ms late, so a copy that plays no host motion
// after this long and this many frames has none coming. Both, so that one long frame (a level loading) is not enough.
constexpr uint64_t kGraceMs = 2000;
constexpr uint32_t kGraceFrames = 20;
// The decider streams every creature it has every 66 ms: a newest sample older than this means it stopped streaming this one.
constexpr uint64_t kStaleMs = 3000;
// The decider's round over its stalkers and the server entities of its level (watch_decider).
constexpr uint64_t kRoundMs = 10000;

inline uint8_t bit(uint32_t part) { return part < kMaxParts ? uint8_t(1u << part) : uint8_t(0); }

// One copy with one skeleton. The owner (coop_remote.cpp Puppet) starts it again when the copy's visual changes: a new
// skeleton starts with no blend at all (in_ChangeVisual plays no placeholder) and with its own motion ids.
struct Book
{
	uint64_t since_ms = 0; // the first frame of this skeleton here
	uint32_t frames = 0; // frames tried since then
	bool placeholder_known = false;
	uint32_t placeholder = kNoMotion; // this skeleton's "death_init" (a MotionID value), kNoMotion when it has none
	uint8_t driven = 0; // partitions a host motion other than the placeholder was started on
	uint8_t fallback = 0; // partitions given this copy's own idle
	uint8_t fallback_tried = 0; // partitions the idle was looked up for (once per skeleton)
	bool said = false; // the episode now under way is in the log already
};

// Past the first moments of the copy.
inline bool settled(const Book& b, uint64_t now_ms)
{
	return b.frames >= kGraceFrames && now_ms >= b.since_ms + kGraceMs;
}

// What one partition of a copy is, this frame.
struct PartFacts
{
	bool named = false; // the decider named a channel-0 motion for it (not kNoMotion)
	bool valid = false; // the id exists in this skeleton's tables (slot and index in range: entities::motion_valid)
	bool placeholder = false; // that id is this skeleton's "death_init"
	bool driven = false; // a host motion other than the placeholder was started on it already
	bool bare = false; // its channel 0 plays nothing, or nothing but the placeholder
};

enum class PartPlay : uint8_t
{
	Host, // play (or keep playing) the motion the decider named
	Keep, // leave the partition as it is
	Fallback, // play this copy's own standing idle on it (looked up by name here, never a host id)
};

inline PartPlay part_play(const PartFacts& f, bool settled_now)
{
	// The decider's own placeholder is mirrored as it is: a decider that is dead or never ran its animation update must
	// not be hidden behind a living idle. The line says so instead (Cause::HostPlaceholder, Cause::HostDead).
	if (f.named && f.valid)
		return PartPlay::Host;
	// A part a host motion drove keeps it (b29: a part with nothing but a flinch names no motion for a moment). A part that
	// plays something else (the idle already) is left alone.
	if (f.driven || !f.bare || !settled_now)
		return PartPlay::Keep;
	return PartPlay::Fallback;
}

namespace detail
{
inline char lower(char c) { return c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c; }
inline bool same_name(const char* a, const char* b) // as LL_PartID compares partition names (stricmp)
{
	if (!a || !b)
		return false;
	while (*a && *b && lower(*a) == lower(*b))
	{
		++a;
		++b;
	}
	return !*a && !*b;
}
} // namespace detail

// This copy's own standing idle for a partition, by the partition's name in the model; nullptr for any other partition
// (monster models, a stalker's 4th part) and for no name. From GAMMA's stalker_animation.omf, which all 182 models of
// actors\stalker_neutral refer to (slot 0 in 160 of them): legs 13 "norm_idle_0" (part 0, loops), torso 650
// "norm_torso_0_idle_0" (part 1), head 778 "head_idle_0" (part 2). Their arms are 37-91 deg from the bind pose at frame 0
// (temp/r101/server/omfpose.py): none is a T-pose. The engine looks the name up in the copy's own model (ID_Cycle_Safe).
inline const char* fallback_motion(const char* partition)
{
	struct Row
	{
		const char* part;
		const char* motion;
	};
	static const Row rows[] = {{"legs", "norm_idle_0"}, {"torso", "norm_torso_0_idle_0"}, {"head", "head_idle_0"}};
	for (const Row& r : rows)
		if (detail::same_name(r.part, partition))
			return r.motion;
	return nullptr;
}

// The decider's stream reaches this process (P101 review, 2026-11-26): a snapshot sample of some creature was taken here within
// kStaleMs, and after this copy appeared (Book::since_ms). `last_ms` is when the newest one was taken (entities::client_on_snapshot),
// 0 for none this session. Without it, "no snapshot of this id" says nothing about the id: every copy of the level lacks one.
inline bool stream_reaches(uint64_t last_ms, uint64_t since_ms, uint64_t now_ms)
{
	return last_ms != 0 && last_ms >= since_ms && now_ms <= last_ms + kStaleMs;
}

// Why a live copy may stand in its spawn pose (the first that holds, in this order).
enum class Cause : uint8_t
{
	None, // every partition plays a host motion, and the snapshots are fresh; or no partition is idle (a frozen copy, a monster)
	NoStream, // no stream reaches this process at all (stream_reaches): not this copy's matter, no line of its own (say_now)
	NoSnapshot, // the decider streams others here, but no snapshot of this id came since the copy appeared, or since its track
	            // was dropped (Wolf's signature: an id the decider does not stream, e.g. no game object of it there)
	Stale, // the snapshots of this id stopped while others still come: the newest is older than kStaleMs
	HostDead, // the decider's newest sample says health 0: a corpse there, alive here
	HostPlaceholder, // the decider itself still plays "death_init" on a partition (its animation update never ran)
	NotInThisModel, // the decider names a motion this skeleton has not (another model or motion table on one side)
	HostNamesNone, // the decider names no channel-0 motion for the partition
	NotStarted, // named, in this model, not the placeholder, and still not driven
};

inline const char* cause_name(Cause c)
{
	switch (c)
	{
	case Cause::None: return "none";
	case Cause::NoStream: return "no snapshot stream to this client";
	case Cause::NoSnapshot: return "no snapshot of this id while the decider streams others";
	case Cause::Stale: return "the snapshots of this id stopped while others still come";
	case Cause::HostDead: return "dead on the decider";
	case Cause::HostPlaceholder: return "the decider itself plays its spawn placeholder";
	case Cause::NotInThisModel: return "the decider names motions this model has not";
	case Cause::HostNamesNone: return "the decider names no motion";
	case Cause::NotStarted: return "the named motion did not start";
	}
	return "?";
}

struct WatchFacts
{
	bool stream = false; // stream_reaches: the decider's snapshots of some creature reach this process
	bool track = false; // a snapshot track of this id exists here
	uint32_t samples = 0; // samples in it
	uint64_t newest_age_ms = 0; // age of the newest sample
	bool decider_dead = false; // the newest sample's health is 0
	uint8_t present = 0; // partitions of this skeleton
	uint8_t driven = 0; // Book::driven
	uint8_t named = 0, valid = 0, placeholder = 0; // per partition, as PartFacts (the sample shown now)
};

inline Cause cause_of(const WatchFacts& f)
{
	// A copy every partition of which a host motion drove holds its last pose through a gap: no T-pose, and no line (a
	// decider that stalls for seconds, a save or a level load, would fill the budget with all its copies). That comes first
	// (P101 review): a track dropped after the copy was driven leaves it frozen, not in its spawn pose. A copy with no
	// partition that counts (a monster's, a trader's: WatchFacts::present 0) has no line either.
	const uint8_t idle = uint8_t(f.present & ~f.driven);
	if (!idle)
		return Cause::None;
	// No snapshot of this id, or none for kStaleMs: the id's matter only while the decider's stream still reaches this process
	// (P101 review: before, every copy of a client the stream had not reached yet, or no longer reached, said "no snapshot").
	if (!f.track || !f.samples)
		return f.stream ? Cause::NoSnapshot : Cause::NoStream;
	if (f.newest_age_ms > kStaleMs)
		return f.stream ? Cause::Stale : Cause::NoStream;
	if (f.decider_dead)
		return Cause::HostDead;
	if (idle & f.placeholder)
		return Cause::HostPlaceholder;
	if (idle & f.named & ~f.valid)
		return Cause::NotInThisModel;
	if (idle & ~f.named)
		return Cause::HostNamesNone;
	return Cause::NotStarted;
}

// Whether the copy's line is written now: once per episode, after the first moments; an episode ends when the copy is
// fine again (then a new one may be said). No stream to this process (Cause::NoStream) neither starts nor ends an episode:
// every copy of the level is in it at once (the engine writes one line for the gap instead), and a copy the stream then still
// leaves out says so once it comes; a copy that said its cause already is not said again after a decider's stall.
inline bool say_now(Book& b, Cause c, bool settled_now)
{
	if (c == Cause::None)
	{
		b.said = false;
		return false;
	}
	if (c == Cause::NoStream || !settled_now || b.said)
		return false;
	b.said = true;
	return true;
}

// The session's budget of one kind of line: the first 40, then every 50th (as the 26fe5b0b line had).
inline bool line_allowed(uint32_t& n)
{
	++n;
	return n <= 40 || n % 50 == 0;
}

// The decider: a creature met in two rounds in a row (kRoundMs apart) is said once; a streak that breaks starts again.
inline bool second_round(uint8_t& streak)
{
	if (streak < 255)
		++streak;
	return streak == 2;
}

} } // namespace coop::mirror_pose
