// Anomaly Coop — the stamp of the mode's answers (coop.mode_stamp in Lua, docs/MODES.md section 4).
//
// Two reads of the stamp give the same number only if no answer of the mode facade can have changed between them: the questions
// without arguments (actor_is_player, decides_world, leads_here, ...) and where / decider for a given id. A caller may keep such an
// answer for as long as the stamp stays the same. The stamp only grows, so two different frames never read the same value.
// facts_of is not covered: a server mode answers it by the age of a body report against the clock, which no event marks.
//
// Pure C++17, header-only, no engine dependencies (tests/test_mode_stamp.cpp). One instance per process: g_mode_stamp below, an inline
// variable of the module that uses it (only xrGame's code bumps and reads it). Bumps run on the main thread, reads anywhere; a read is
// one relaxed atomic load (a plain field read on x64: no lock, no allocation). The list of the places that bump it is at
// Manager::frame (xrGame/coop/coop_manager.cpp).
#pragma once

#include <atomic>
#include <cstdint>

namespace coop
{
class ModeStamp
{
public:
	// The stamp now. Starts at 1, so 0 is never a stamp.
	uint64_t now() const { return m_value.load(std::memory_order_relaxed); }
	// Something an answer depends on may have changed (a frame began, a session event): every later read differs from every earlier one.
	void bump() { m_value.fetch_add(1, std::memory_order_relaxed); }

private:
	std::atomic<uint64_t> m_value{1};
};
static_assert(std::atomic<uint64_t>::is_always_lock_free, "a read of the stamp is a field read");

inline ModeStamp g_mode_stamp;
} // namespace coop
