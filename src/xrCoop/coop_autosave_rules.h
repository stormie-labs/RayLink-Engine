// Anomaly Coop — autosave rotation (F3, CHECKPOINTS_DEATH.md §7). The host writes emergency saves `coop_auto_1` ..
// `coop_auto_N` in turn: a new level, after a shared sleep, on a timer and at the end of the session. These are the
// rules only (names, which slot is next, when the timer is due); writing the files is the engine's job.
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
// "coop_auto_<index>", 1-based.
inline std::string slot_name(uint32_t index)
{
	return "coop_auto_" + std::to_string(index);
}

// A save file name without extension: "coop_auto_7" -> 7. Only what slot_name makes is a slot: lower case, a plain
// number from 1 (no sign, no leading zero, at most nine digits), nothing after it. `index` is left as it was on false.
inline bool parse_slot(const std::string& file_stem, uint32_t& index)
{
	static const char kPrefix[] = "coop_auto_";
	constexpr size_t kPrefixLen = sizeof(kPrefix) - 1;
	if (file_stem.size() <= kPrefixLen || file_stem.compare(0, kPrefixLen, kPrefix) != 0)
		return false;
	const size_t digits = file_stem.size() - kPrefixLen;
	if (digits > 9 || file_stem[kPrefixLen] == '0')
		return false;
	uint32_t v = 0;
	for (size_t i = kPrefixLen; i < file_stem.size(); ++i)
	{
		const char c = file_stem[i];
		if (c < '0' || c > '9')
			return false;
		v = v * 10 + uint32_t(c - '0');
	}
	index = v;
	return true;
}

// A save that exists on disk: its slot and when it was written.
struct SlotInfo
{
	uint32_t index;
	uint64_t file_time;
};

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
