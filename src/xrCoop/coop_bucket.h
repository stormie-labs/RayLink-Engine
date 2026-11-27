// Anomaly Coop — a token bucket: a rate with a burst. Start full, refill by the time gone, cap at the burst, take one.
// Each user keeps its own refill formula (tokens a ms, a minute, an interval): the formula is passed in as it was written, so
// the doubles come out the same bit for bit. A clock that goes back refills nothing and keeps the last refill time.
// Pure C++17, header-only, no engine dependencies: used by the engine and by the unit-test exe.
#pragma once

#include <algorithm>
#include <cstdint>

namespace coop
{
struct TokenBucket
{
	double tokens = 0;
	uint64_t at = 0; // ms of the last refill
	bool started = false;

	// Up to `now_ms`: the first call fills it to `burst`; later, when the clock has moved on, `gained(elapsed_ms)` tokens come in,
	// capped at `burst`.
	template <class Gain> void refill(uint64_t now_ms, double burst, Gain gained)
	{
		if (!started)
		{
			started = true;
			tokens = burst;
			at = now_ms;
			return;
		}
		if (now_ms <= at)
			return;
		tokens = std::min(burst, tokens + gained(now_ms - at));
		at = now_ms;
	}

	// `cost` tokens out if there are as many; false leaves them alone.
	bool take(double cost = 1.)
	{
		if (tokens < cost)
			return false;
		tokens -= cost;
		return true;
	}
};
} // namespace coop
