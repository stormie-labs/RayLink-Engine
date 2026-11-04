// Anomaly Coop — split levels: what a map server's logic says of "the actor" (docs/SPLIT_LEVELS.md §7a). Stock logic addresses goodwill to id 0
// (actor_enemy, set_squad_*_to_actor, relationship = enemy). On a map server that is its ghost, and on the world server id 0 is the host's
// character: the request hit the host and left the player on the level untouched. The players of the level stand for the ghost: every
// such word goes into a log, and each player's character is told what it has not heard yet, whenever he comes, once.
// Pure C++17, header-only, no engine dependencies: used by xrGame (coop/coop_story.cpp) and by the unit-test exe.
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>

namespace coop
{
namespace ghost
{
constexpr int32_t kActor = 0;

// One relation request as the story's wire has it: a pair of ids (a goodwill) or a community and an id (a community goodwill).
struct Said
{
	uint8_t op = 0;
	bool community = false; // first is a community, not an object
	int32_t first = 0;
	int32_t second = 0;
	int32_t value = 0;
};

inline bool about_actor(const Said& s) { return (!s.community && s.first == kActor) != (s.second == kActor); }

// The same request for a player's character. A character about himself is no request: false.
inline bool for_player(Said& s, uint16_t proxy)
{
	if (!s.community && s.first == kActor)
		s.first = proxy;
	if (s.second == kActor)
		s.second = proxy;
	return s.community || s.first != s.second;
}

class Log
{
public:
	static constexpr size_t kMax = 16384; // the oldest words go: a player who comes after that many hears the rest

	void add(const Said& s)
	{
		m_said.push_back(s);
		if (m_said.size() > kMax)
		{
			m_said.pop_front();
			++m_base;
		}
	}

	// Calls ask(Said) for what `proxy` has not heard, at most `budget` words; returns how many were looked at.
	template <class Ask>
	uint32_t tell(uint16_t proxy, uint32_t budget, Ask&& ask)
	{
		uint64_t& next = m_next[proxy];
		if (next < m_base)
			next = m_base;
		uint32_t done = 0;
		while (done < budget && next < m_base + m_said.size())
		{
			Said s = m_said[size_t(next - m_base)];
			++next;
			++done;
			if (for_player(s, proxy))
				ask(s);
		}
		return done;
	}

	bool pending(uint16_t proxy) const
	{
		const auto it = m_next.find(proxy);
		return (it == m_next.end() ? m_base : it->second) < m_base + m_said.size();
	}

	size_t size() const { return m_said.size(); }

	void clear()
	{
		m_said.clear();
		m_next.clear();
		m_base = 0;
	}

private:
	std::deque<Said> m_said;
	std::map<uint16_t, uint64_t> m_next; // character -> the number of the next word for him
	uint64_t m_base = 0; // the number of m_said.front()
};
} // namespace ghost
} // namespace coop
