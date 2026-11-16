// Anomaly Coop - a task's audience: who sees a task in a split-levels world (docs/plans/2026-11-16-task-audience.md, B.1).
// Pure C++17, header-only, no engine dependencies: used by xrGame/coop/coop_story.cpp and by the unit-test exe
// (tests/test_task_audience.cpp).
//
// The host's Lua (coop_task_audience.script) decides, once per task and per character, who sees a task and gives the engine
// that set of character ids (the host's own numbering: 0 the host's actor, else a player's proxy). The engine then sends the
// task, the task list and the tasks domain of the drift hash to each player by that set instead of to everybody.
//   - A task with no entry is everybody's: byte for byte what was sent before.
//   - An empty table: the old broadcasts, unchanged (empty() is that switch). Without split levels no entry is ever made.
//   - A bridge (a map server) gets every task: it runs the logic of every task.
//   - A character that is not known yet (a peer with no character on the host) is out of a task that has an entry.
//   - An empty set is a valid audience: nobody but the bridges.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace coop
{
namespace task_audience
{
constexpr uint16_t kNoCharacter = 0xffff; // no character (kNoId): never a member
constexpr size_t kMaxMembers = 64; // per task
constexpr size_t kMaxIdLength = 256; // a task id, as coop.story_local_task takes it (story::set_local_task)
constexpr size_t kMaxEntries = 4096; // a broken script must not fill the memory

// Sorted, without repeats.
using Members = std::vector<uint16_t>;

// May `character` see a task whose audience is `audience` (nullptr: the task has no entry)? `bridge`: the receiver is a map
// server.
inline bool allowed(const Members* audience, uint16_t character, bool bridge)
{
	if (!audience || bridge)
		return true;
	if (character == kNoCharacter)
		return false;
	return std::binary_search(audience->begin(), audience->end(), character);
}

// A task id the table takes: not empty, at most kMaxIdLength bytes.
inline bool id_ok(std::string_view id) { return !id.empty() && id.size() <= kMaxIdLength; }

class Table
{
public:
	enum class Set : uint8_t
	{
		Added, // a new entry
		Replaced, // the entry of that id had other members
		Unchanged, // the same members again
		Refused, // a bad id, too many members, kNoCharacter among them, or the table is full: nothing changed
	};

	// Sets the audience of task `id` to `members` (any order, repeats allowed: they are dropped).
	Set set(std::string_view id, std::vector<uint16_t> members)
	{
		if (!id_ok(id) || members.size() > kMaxMembers)
			return Set::Refused;
		for (const uint16_t c : members)
			if (c == kNoCharacter)
				return Set::Refused;
		std::sort(members.begin(), members.end());
		members.erase(std::unique(members.begin(), members.end()), members.end());
		const auto it = m_entries.find(id);
		if (it != m_entries.end())
		{
			if (it->second == members)
				return Set::Unchanged;
			it->second = std::move(members);
			return Set::Replaced;
		}
		if (m_entries.size() >= kMaxEntries)
			return Set::Refused;
		m_entries.emplace(std::string(id), std::move(members));
		return Set::Added;
	}

	// The task is everybody's again. True if it had an entry.
	bool remove(std::string_view id)
	{
		const auto it = m_entries.find(id);
		if (it == m_entries.end())
			return false;
		m_entries.erase(it);
		return true;
	}

	// The audience of task `id`, nullptr if it has none.
	const Members* find(std::string_view id) const
	{
		if (m_entries.empty())
			return nullptr;
		const auto it = m_entries.find(id);
		return it == m_entries.end() ? nullptr : &it->second;
	}

	bool allowed(std::string_view id, uint16_t character, bool bridge) const
	{
		return task_audience::allowed(find(id), character, bridge);
	}

	// No entry at all: every task goes as it went before (one broadcast, one hash).
	bool empty() const { return m_entries.empty(); }
	size_t size() const { return m_entries.size(); }
	void clear() { m_entries.clear(); }

private:
	std::map<std::string, Members, std::less<>> m_entries;
};
} // namespace task_audience
} // namespace coop
