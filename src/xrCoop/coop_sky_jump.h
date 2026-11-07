// Anomaly Coop — the sky after a jump of the game clock (live report 41.4, 2026-11-08: after the shared sleep the clock said
// 10:30 and the sky was night until about 11:00).
//
// CEnvironment (xrEngine/Environment.cpp) blends two env sections of the weather cycle, Current[0] and Current[1], by the time
// of day. With sections already chosen, SelectEnvs(gt) only ever steps one section on, and only once the time of day passed
// Current[1]'s time; TimeWeight gives 0 for a time outside their window, so the sky then shows Current[0] whole. A jump of hours
// keeps the old sections: past midnight (20:30 + 14 h = 10:30, sections 20:00 / 21:00) the sky is the 20:00 one until gt passes
// 21:00 again; inside one day (01:30 + 9 h) it steps once and blends the 02:00 night into 11:00. Only a forced
// level.set_weather chooses both sections again (Invalidate, then SelectEnvs' "first or forced start").
//
// The stock sleep gets that forced change at once: ui_sleep_dialog.dream_callback moves the clock and sends actor_on_sleep in
// one call, surge_manager's actor_on_sleep asks level_weathers for a forced change, and its next hour tick (3 s) forces. In a
// session the host skips the time when the sleep starts (coop_rest.cpp skip_time) and actor_on_sleep comes at dream_mid,
// seconds later: the hour tick has already selected without force, the forced change waits for the next hour. A client mirrors
// the host's sections (WeatherSync) and never forces (its level_weathers selects nothing, level.set_weather is refused).
//
// So every co-op jump of the clock is noted (JumpNote, any thread), and the next frame of the main thread chooses both sections
// again for the new time when reselect_after_jump says so (coop_worldstate.cpp), with pick_pair: the same pair SelectEnvs picks.
// Pure C++17, header-only, no engine dependencies: used by the engine and by the unit-test exe (tests/test_sky_jump.cpp).
#pragma once

#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace coop
{
namespace sky
{
constexpr float kDaySec = 24.f * 60.f * 60.f; // DAY_LENGTH
// A jump of the clock this long chooses both sections again. Shorter: a client following the host (seconds), the host's move
// (about a game minute), a script's minute steps. The stock's own step crosses one section boundary right, and a step back of a
// few seconds shows the section just passed (Current[0]): nothing to choose, and nothing to pop out of a running cycle change.
constexpr long long kReselectJumpMs = 30LL * 60 * 1000;

// The time of day of a sky clock that may stand past a day (CEnvironment::ChangeGameTime wraps only once) or before 0.
inline float day_time(float t)
{
	float d = std::fmod(t, kDaySec);
	if (d < 0.f)
		d += kDaySec;
	if (d >= kDaySec)
		d = 0.f;
	return d;
}

// After the clock moved by jump_ms (all jumps since the last frame, each counted by its size): choose both env sections again.
// frozen: a weather fx plays (its sections are its own, the host's WeatherSync aligns its position) or the weather is paused
// (the weather editor): their sections are left alone.
inline bool reselect_after_jump(long long jump_ms, bool frozen)
{
	if (frozen)
		return false;
	const long long size = jump_ms < 0 ? -jump_ms : jump_ms;
	return size >= kReselectJumpMs;
}

// Indices of the two sections of a cycle for the time of day gt: CEnvironment::SelectEnvs(envs, e0, e1, gt) (lower_bound by
// exec time; envs are sorted by it): e1 the first section at or after gt (the first one past the end), e0 the one before it
// (the last one before the first). exec_of(section) gives its exec time.
struct Pair
{
	bool ok = false; // false: an empty cycle
	size_t i0 = 0;
	size_t i1 = 0;
};

template <class Envs, class ExecOf>
Pair pick_pair(const Envs& envs, ExecOf exec_of, float gt)
{
	Pair p;
	const size_t n = envs.size();
	if (!n)
		return p;
	size_t lo = 0, hi = n; // lower_bound
	while (lo < hi)
	{
		const size_t mid = lo + (hi - lo) / 2;
		if (exec_of(envs[mid]) < gt)
			lo = mid + 1;
		else
			hi = mid;
	}
	p.ok = true;
	if (lo == n)
	{
		p.i0 = n - 1;
		p.i1 = 0;
	}
	else
	{
		p.i1 = lo;
		p.i0 = lo == 0 ? n - 1 : lo - 1;
	}
	return p;
}

// The jumps of the clock since the last frame of the main thread. note() runs where the clock moves (level.change_game_time may
// run on the script worker thread), take() on the main thread.
class JumpNote
{
public:
	void note(long long delta_ms)
	{
		if (!delta_ms)
			return;
		m_total.fetch_add(delta_ms < 0 ? -delta_ms : delta_ms, std::memory_order_relaxed);
	}
	// The sum of the jumps' sizes since the last take; false when the clock did not jump.
	bool take(long long& total_ms)
	{
		total_ms = m_total.exchange(0, std::memory_order_relaxed);
		return total_ms != 0;
	}
	void clear() { m_total.store(0, std::memory_order_relaxed); }

private:
	std::atomic<long long> m_total{0};
};
} // namespace sky
} // namespace coop
