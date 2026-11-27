// Anomaly Coop — the world server's memory guard for its map servers (the physical look and the commit look). Pure C++17,
// header-only: the engine (srv/coop_mapserver.cpp check_memory, host_frame) and the unit-test exe use it.
//
// The guard must not read the physical memory alone (ullAvailPhys): a world server can die of out-of-memory with commit nearly
// gone and RAM free. Windows refuses an allocation when the commit limit (RAM plus page file) is reached, whatever RAM is free; a
// map server commits 1.0-1.7 GB when it is up. So the guard also reads the commit left (ullAvailPageFile, which Windows gives as
// what this process may still commit):
//   physical low  no empty map server lingers (coop_levels_dynamic.h memory_low);
//   commit low    no empty map server lingers either, and no map server starts: the start waits (it is no failure, it does not count
//                 towards giving its level up) until the commit is back over kCommitBackBytes.
// The numbers are an estimate, not a measured limit (not checked on a 16 GB machine with a small page
// file).
//
// start_room below:
//   loading ones  a map server takes its commit gradually over its 30-90 s load, the look is up to 2 s old and host_frame launches every
//                 level that wants one in the same pass: each server still loading (Starting) counts as if it had its
//                 kMapServerCommitBytes now, so two starts in one load window no longer pass one look together;
//   a time limit  ullAvailPageFile is the room under the commit limit of now. A page file Windows manages grows only when the commit
//                 charge comes near that limit (AutomaticManagedPagefile), so a commit left under the floor may be room nobody
//                 asked Windows for yet and a wait could last for good. A wait for the commit alone (nothing else loads) ends after kCommitWaitMs: the start goes ahead,
//                 unless it would leave this PC under 0.5 GB (kCommitHardBytes): that one keeps waiting until the
//                 commit is back. Not checked on a real machine.
#pragma once

#include "coop_levels_dynamic.h" // memory_low, kLowMemoryBytes

#include <cstdint>

namespace coop
{
namespace levels
{
constexpr uint64_t kMapServerCommitBytes = uint64_t(1536) * 1024 * 1024; // what a map server commits when it is up
constexpr uint64_t kCommitHeadroomBytes = uint64_t(1024) * 1024 * 1024; // what the world server and the running ones may still commit
// Under this much commit left no map server starts (it would leave the world server less than the headroom), and no empty one lingers.
constexpr uint64_t kCommitFloorBytes = kMapServerCommitBytes + kCommitHeadroomBytes;
constexpr uint64_t kCommitBackBytes = kCommitFloorBytes + uint64_t(512) * 1024 * 1024; // and back over this: no flapping at the border

// The commit left is low: under kCommitFloorBytes; once low, until kCommitBackBytes are left again.
inline bool commit_low(bool was_low, uint64_t commit_available)
{
	return commit_available < (was_low ? kCommitBackBytes : kCommitFloorBytes);
}

// The guard's last look: each border with its own band.
struct MemoryLook
{
	bool phys_low = false;
	bool commit_low = false;
};

inline MemoryLook look_memory(const MemoryLook& was, uint64_t phys_available, uint64_t commit_available)
{
	MemoryLook now;
	now.phys_low = memory_low(was.phys_low, phys_available);
	now.commit_low = commit_low(was.commit_low, commit_available);
	return now;
}

// Short of either: no empty map server lingers (linger_of's low_memory).
inline bool short_of_memory(const MemoryLook& m) { return m.phys_low || m.commit_low; }

// A new map server may start as far as the look goes: the commit is not low (short of RAM alone, Windows pages; out of commit, a process
// dies). start_room adds the servers still loading and the time limit of a wait.
inline bool may_start_map_server(const MemoryLook& m) { return !m.commit_low; }

// ---------------------------------------------------------------- one start, asked every frame while it waits

// Under this much commit left a start never goes by itself: it would leave this PC under 0.5 GB (with 0.6 GB a world server has died).
constexpr uint64_t kCommitHardBytes = kMapServerCommitBytes + uint64_t(512) * 1024 * 1024;
// A wait for the commit alone ends after this: an empty map server stops at once while the commit is short and is gone within its
// kStopTimeoutMs (15 s); a minute covers that and the looks after it, and stays well under the load its players wait for anyway.
constexpr uint32_t kCommitWaitMs = 60000;

// The commit left once the servers still loading have taken theirs (each as if it had its kMapServerCommitBytes now; what it took so far
// is in the look as well, so this errs on the side of a wait, at most until it is up).
inline uint64_t commit_after_starting(uint64_t commit_available, uint32_t starting)
{
	const uint64_t reserved = uint64_t(starting) * kMapServerCommitBytes;
	return commit_available > reserved ? commit_available - reserved : 0;
}

enum class StartRoom : uint8_t
{
	Go, // room for it: it starts
	Loading, // the servers still loading would take the room, or the commit is low while one loads: it waits for them (each is up or
	         // ended within kStartTimeoutMs)
	Commit, // the commit left is low and nothing else loads: it waits, at most kCommitWaitMs
	GoUnsure, // still low after kCommitWaitMs, at least kCommitHardBytes left: it starts (a page file Windows manages grows when needed)
	HardFloor, // still low after kCommitWaitMs, under kCommitHardBytes: it waits until the commit is back
};

inline bool starts(StartRoom r) { return r == StartRoom::Go || r == StartRoom::GoUnsure; }

// One start's verdict: the look `m`, the commit left in bytes of that look, the map servers still loading (Starting) now, and how long
// this start has waited for the commit alone.
inline StartRoom start_room(const MemoryLook& m, uint64_t commit_available, uint32_t starting, uint64_t waited_ms)
{
	if (starting > 0)
		return may_start_map_server(m) && commit_after_starting(commit_available, starting) >= kCommitFloorBytes ? StartRoom::Go :
		                                                                                                           StartRoom::Loading;
	if (may_start_map_server(m))
		return StartRoom::Go;
	if (waited_ms < kCommitWaitMs)
		return StartRoom::Commit;
	return commit_available >= kCommitHardBytes ? StartRoom::GoUnsure : StartRoom::HardFloor;
}

// The clock of one start's wait for the commit: it runs from the first frame of the wait and starts over at every frame another map
// server loads (the time limit is for the commit alone, never past a load); a start ends the wait, the next one begins at 0. The clock
// is the caller's (ms, any origin); a value older than the start of the wait counts as no time.
struct StartWait
{
	StartRoom step(const MemoryLook& m, uint64_t commit_available, uint32_t starting, uint64_t now)
	{
		if (!waiting_ || starting > 0)
			since_ = now;
		waiting_ = true;
		waited_ = now > since_ ? now - since_ : 0;
		const StartRoom room = start_room(m, commit_available, starting, waited_);
		if (starts(room))
			waiting_ = false;
		return room;
	}
	bool waiting() const { return waiting_; }
	// How long the last step had waited for the commit alone (what the line of a start after the time limit says).
	uint64_t waited_ms() const { return waited_; }

private:
	uint64_t since_ = 0;
	uint64_t waited_ = 0;
	bool waiting_ = false;
};
} // namespace levels
} // namespace coop
