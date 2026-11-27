// How long a monster's ability may hold the local actor while this machine plays the host's ability on it: an end the
// host sends may never come (the monster left, a message was lost), so each hold also ends by its deadline.
// Pure C++17, header-only, no engine dependencies: used by the engine and by the unit-test exe.
#pragma once

#include <cstdint>

namespace coop
{
namespace monster_fx
{
// A controller's tube from its preparation until its start comes.
constexpr uint32_t kTubePrepareMs = 15000;
// A controller's tube after its stage's own length, and after its end: the cancel follows with the last animation.
constexpr uint32_t kTubeSlackMs = 5000;
// A bloodsucker's grip: the hold itself is 4 s, the approach and the last blow a few more.
constexpr uint32_t kGripMaxMs = 12000;

// One ability's hold of the local actor: on from start() until stop(), and expired once `now` passes its deadline.
class ActorHold
{
public:
	void start(uint32_t now, uint32_t ms)
	{
		m_on = true;
		m_deadline = now + ms;
	}
	// A later stage of the same hold: a new deadline, the hold stays as it is.
	void extend(uint32_t now, uint32_t ms) { m_deadline = now + ms; }
	void stop() { m_on = false; }
	bool on() const { return m_on; }
	bool expired(uint32_t now) const { return m_on && now > m_deadline; }

private:
	bool m_on = false;
	uint32_t m_deadline = 0;
};
} // namespace monster_fx
} // namespace coop
