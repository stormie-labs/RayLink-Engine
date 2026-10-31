// Anomaly Coop — autosave rotation (F3, CHECKPOINTS_DEATH.md §7). The host writes emergency saves `coop_auto_1` ..
// `coop_auto_N` in turn: a new level, after a shared sleep, on a timer and at the end of the session. With a game id the
// ring is that game's own: `coop_auto_<id>_<N>`. These are the rules only (names, which slot is next, when the timer is
// due); writing the files is the engine's job.
// Pure C++17, header-only, no engine dependencies: used by the engine and by the unit-test exe.
#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace coop
{
namespace autosave
{
// One ring per game. A game id puts the slots of the host's ring in a namespace of their own, so two games on one machine do
// not overwrite each other: "coop_auto_<id>_<index>". Without an id the ring is the old shared one, "coop_auto_<index>".
// An id is 1..kMaxGameIdLen characters of lower case [0-9a-z-]. It never holds '_' (that is the separator between the id
// and the number) or '.', '/', ':' (nothing that could leave the saves folder or end a name), so a name has exactly one
// reading: the id is what stands between "coop_auto_" and the single "_" that precedes the digits.
constexpr size_t kMaxGameIdLen = 32;

inline bool valid_game_id(const std::string& id)
{
	if (id.empty() || id.size() > kMaxGameIdLen)
		return false;
	for (const char c : id)
		if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || c == '-'))
			return false;
	return true;
}

// The id a name is made with: `id` when it is valid, else none (""): an id that is not valid is no id. The engine checks the
// id once, in set_game, and logs it; the name functions below are safe with any string all the same.
inline const std::string& effective_game(const std::string& id)
{
	static const std::string none;
	return valid_game_id(id) ? id : none;
}

// "coop_auto_<id>_<index>", or "coop_auto_<index>" without an id. Index is 1-based.
inline std::string slot_name(uint32_t index, const std::string& game = std::string())
{
	const std::string& g = effective_game(game);
	return g.empty() ? "coop_auto_" + std::to_string(index) : "coop_auto_" + g + "_" + std::to_string(index);
}

// A save file name without extension: "coop_auto_7" -> 7 in the old ring, "coop_auto_abc_7" -> 7 in the ring of game "abc".
// Only what slot_name makes in THAT namespace is a slot: lower case, a plain number from 1 (no sign, no leading zero, at
// most nine digits), nothing after it. The slots of another game, the files of the old ring (when a game is given) and
// the game files (when none is) are not slots here, and neither are the shared coop_auto_tmp / coop_auto_bak (no "_<digits>"
// after the prefix in any namespace; a game that is called "tmp" has coop_auto_tmp_<N>, a different name). The old rule
// that rejects everything but digits after "coop_auto_" is what keeps "coop_auto_5_1" (game "5") out of the old ring.
// `index` is left as it was on false.
inline bool parse_slot(const std::string& file_stem, uint32_t& index, const std::string& game = std::string())
{
	const std::string& g = effective_game(game);
	const std::string prefix = g.empty() ? std::string("coop_auto_") : "coop_auto_" + g + "_";
	if (file_stem.size() <= prefix.size() || file_stem.compare(0, prefix.size(), prefix) != 0)
		return false;
	const size_t digits = file_stem.size() - prefix.size();
	if (digits > 9 || file_stem[prefix.size()] == '0')
		return false;
	uint32_t v = 0;
	for (size_t i = prefix.size(); i < file_stem.size(); ++i)
	{
		const char c = file_stem[i];
		if (c < '0' || c > '9')
			return false;
		v = v * 10 + uint32_t(c - '0');
	}
	index = v;
	return true;
}

// A save file of the folder: its name without extension and when it was written.
struct NamedFile
{
	std::string stem;
	uint64_t file_time;
};

// A save that exists on disk: its slot and when it was written.
struct SlotInfo
{
	uint32_t index;
	uint64_t file_time;
};

// The slots of one game among the files of a folder: what the ring, next_slot, slots_to_prune and the Restore list see.
// The files of other games, of the old ring (when a game is given), the shared temporary pair and everything else are left out.
inline std::vector<SlotInfo> slots_of_game(const std::vector<NamedFile>& files, const std::string& game)
{
	std::vector<SlotInfo> slots;
	for (const NamedFile& f : files)
	{
		uint32_t index = 0;
		if (parse_slot(f.stem, index, game))
			slots.push_back({index, f.file_time});
	}
	return slots;
}

// The ring the host rotates: whose slots, and what this session learned about them. A slot number means a file in ONE
// game's namespace, so the memory of slots that could not be replaced (read-only, held by another process) belongs to the
// game and goes when the game changes.
struct Ring
{
	std::string game; // "" = the old shared ring
	std::vector<uint32_t> bad_slots;
};

struct SetGameResult
{
	bool accepted; // false: the id was not valid and the ring is the old one
	bool changed; // the ring is another one than before (the memory of bad slots was dropped)
};

// Sets the game of the ring. Safe at any time and as often as wanted: the same game again changes nothing and keeps the
// memory; another game (or none) drops it. "" = no game, the old ring. An id that is not valid is no id (the old ring) and
// is reported as not accepted.
inline SetGameResult ring_set_game(Ring& r, const std::string& wanted)
{
	const bool accepted = wanted.empty() || valid_game_id(wanted);
	const std::string now = accepted ? wanted : std::string();
	const bool changed = now != r.game;
	if (changed)
	{
		r.game = now;
		r.bad_slots.clear();
	}
	return {accepted, changed};
}

// The bad slots of `game`: those of the ring when it is the ring's game, else none. A save that began in another game (the
// game changed under it) neither reads nor writes the memory of this one.
inline const std::vector<uint32_t>& ring_bad(const Ring& r, const std::string& game)
{
	static const std::vector<uint32_t> none;
	return effective_game(game) == r.game ? r.bad_slots : none;
}

inline void ring_mark_bad(Ring& r, const std::string& game, uint32_t slot)
{
	if (slot < 1 || effective_game(game) != r.game)
		return;
	if (std::find(r.bad_slots.begin(), r.bad_slots.end(), slot) == r.bad_slots.end())
		r.bad_slots.push_back(slot);
}

// The slot the next save goes to: the first free one of 1..count, and when all are taken the one with the oldest
// file (the lowest index among equals). Files of slots above `count` (the host keeps fewer than before) are not
// counted. count < 1 is taken as 1.
inline uint32_t next_slot(const std::vector<SlotInfo>& existing, uint32_t count)
{
	if (count < 1)
		count = 1;

	// One entry per slot inside the range; a slot listed twice keeps its newer time.
	std::vector<SlotInfo> in_range;
	for (const SlotInfo& s : existing)
	{
		if (s.index < 1 || s.index > count)
			continue;
		bool merged = false;
		for (SlotInfo& r : in_range)
			if (r.index == s.index)
			{
				if (s.file_time > r.file_time)
					r.file_time = s.file_time;
				merged = true;
				break;
			}
		if (!merged)
			in_range.push_back(s);
	}

	// A free slot, if any, is at most one past the number of taken ones.
	const size_t limit = in_range.size() + 1 < size_t(count) ? in_range.size() + 1 : size_t(count);
	for (uint32_t i = 1; i <= limit; ++i)
	{
		bool used = false;
		for (const SlotInfo& r : in_range)
			if (r.index == i)
			{
				used = true;
				break;
			}
		if (!used)
			return i;
	}

	const SlotInfo* oldest = nullptr;
	for (const SlotInfo& r : in_range)
		if (!oldest || r.file_time < oldest->file_time || (r.file_time == oldest->file_time && r.index < oldest->index))
			oldest = &r;
	return oldest ? oldest->index : 1;
}

// next_slot, except the slots in `skip` (the old files of a slot could not be replaced: read-only, held by another
// process). A skipped slot counts as taken by the newest file there is, so the free slot or the oldest of the rest is
// chosen. 0 when nothing is left. Numbers outside 1..count in `skip` change nothing.
inline uint32_t next_slot_skipping(const std::vector<SlotInfo>& existing, uint32_t count, const std::vector<uint32_t>& skip)
{
	if (count < 1)
		count = 1;
	auto skipped = [&skip](uint32_t index)
	{
		return std::find(skip.begin(), skip.end(), index) != skip.end();
	};
	std::vector<SlotInfo> seen;
	for (const SlotInfo& s : existing)
		if (!skipped(s.index))
			seen.push_back(s);
	for (uint32_t k = 1; k <= count; ++k)
		if (skipped(k))
			seen.push_back({k, UINT64_MAX});
	const uint32_t chosen = next_slot(seen, count);
	return skipped(chosen) ? 0 : chosen; // every place is one that failed
}

// The slots to delete after a save so that no more than `count` slot files stay: the oldest beyond that, oldest first.
// `existing` lists the slots on disk with the save just made (slot `keep`) in it. The slot just written is the newest by
// definition and always stays; the others are ordered by file time, equal times with the higher index as the newer (the
// reverse of the order next_slot overwrites in). Slots above `count` are not special: they leave when they are older than
// `count` others, so a host that keeps fewer than before drops the surplus over the next saves, newest worlds last.
// Index 0 is not a slot (never listed for deletion). count < 1 is taken as 1.
inline std::vector<uint32_t> slots_to_prune(const std::vector<SlotInfo>& existing, uint32_t count, uint32_t keep)
{
	if (count < 1)
		count = 1;

	// One entry per slot; a slot listed twice keeps its newer time.
	std::vector<SlotInfo> slots;
	for (const SlotInfo& s : existing)
	{
		if (s.index < 1)
			continue;
		bool merged = false;
		for (SlotInfo& r : slots)
			if (r.index == s.index)
			{
				if (s.file_time > r.file_time)
					r.file_time = s.file_time;
				merged = true;
				break;
			}
		if (!merged)
			slots.push_back(s);
	}
	if (slots.size() <= size_t(count))
		return {};

	// Newest first: the slot just written, then by file time, equal times the higher index first.
	std::sort(slots.begin(), slots.end(), [keep](const SlotInfo& a, const SlotInfo& b)
	{
		if ((a.index == keep) != (b.index == keep))
			return a.index == keep;
		if (a.file_time != b.file_time)
			return a.file_time > b.file_time;
		return a.index > b.index;
	});

	std::vector<uint32_t> gone;
	for (size_t i = slots.size(); i > size_t(count); --i)
		gone.push_back(slots[i - 1].index); // from the oldest
	return gone;
}

// A pause of the autosaves that has to be renewed (F3 T12 fix round 1, review I1). The Restore window holds it while it
// is open, so the timer cannot write into the slot the host is about to pick; the window renews it every frame, so a
// window closed in any way (a button, Escape, the main menu switched off) ends the pause by itself a lease later.
struct Pause
{
	bool held = false;
	uint64_t renewed_ms = 0;
	uint32_t lease_ms = 0;
};

// Holds the pause (or renews it) until `lease_ms` after now_ms.
inline void pause_hold(Pause& p, uint64_t now_ms, uint32_t lease_ms)
{
	p.held = true;
	p.renewed_ms = now_ms;
	p.lease_ms = lease_ms;
}

// True while the pause is held and its lease is fresh. A clock that went back ends it.
inline bool pause_holds(const Pause& p, uint64_t now_ms)
{
	return p.held && now_ms >= p.renewed_ms && now_ms - p.renewed_ms < p.lease_ms;
}

// A held pause whose lease ran out: cleared, and true once, so the caller can start afresh.
inline bool pause_lapsed(Pause& p, uint64_t now_ms)
{
	if (!p.held || pause_holds(p, now_ms))
		return false;
	p.held = false;
	return true;
}

// Lets the pause go on purpose; true if it was held.
inline bool pause_release(Pause& p)
{
	const bool was = p.held;
	p.held = false;
	return was;
}

// Why a save is written.
enum class Reason : uint8_t
{
	Timer,
	Sleep,
	LevelChange,
	SessionEnd,
	Manual
};

// State of the timer. last_ms = host clock of the last save (0 = the start of the clock); busy_since = when the save
// first came due while the host was busy (0 = not postponed).
struct Schedule
{
	uint64_t last_ms = 0;
	uint64_t busy_since = 0;
};

// True when a timed save has to be written now; then last_ms = now. interval_min 0 = off. While `busy` (the host's
// own character is dead or in a fight, a transition is under way) the save is postponed, but by no more than
// max_postpone_ms: after that it is due anyway. When the host is free again it is due at once.
inline bool timer_due(Schedule& s, uint64_t now_ms, uint32_t interval_min, bool busy, uint32_t max_postpone_ms)
{
	if (interval_min == 0)
	{
		s.busy_since = 0;
		return false;
	}
	if (now_ms < s.last_ms) // the clock went back (a loaded save): count from here
	{
		s.last_ms = now_ms;
		s.busy_since = 0;
		return false;
	}
	if (now_ms - s.last_ms < uint64_t(interval_min) * 60000u)
	{
		s.busy_since = 0;
		return false;
	}
	if (busy)
	{
		if (s.busy_since == 0 || now_ms < s.busy_since)
			s.busy_since = now_ms;
		if (now_ms - s.busy_since < max_postpone_ms)
			return false;
	}
	s.last_ms = now_ms;
	s.busy_since = 0;
	return true;
}
} // namespace autosave
} // namespace coop
