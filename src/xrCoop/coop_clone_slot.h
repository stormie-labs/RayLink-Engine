// Anomaly Coop — the slot a client's clone of its weapon takes (scope, upgrade kit, detach: alife_clone_weapon).
// Live report 2026-11-08 (stable 41.6, G.A.M.M.A.): a client fitted a scope to the SR-3M in his slot and the new weapon
// went into the backpack. Stock _g.alife_clone_weapon releases the old weapon before it registers the clone, so in single
// player the clone finds the slot free and takes it (and the hands, when they held the old one). On a client the clone is
// made at once under a leased id while the old weapon's release goes with it to the host (coop_requests.cpp
// create_leased): the old weapon held the slot until the host's destroy came, the engine put the clone into the backpack.
// Now the old weapon leaves the slot for the backpack when the clone is made (it goes with the clone), the clone takes
// that slot when it spawns here, and a clone the host refuses gives the slot back to the old weapon.
// Pure C++17, header-only: used by the engine and by the unit-test exe.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace coop
{
namespace cloneslot
{
constexpr std::uint16_t kNone = 0xFFFF;
constexpr std::uint64_t kKeepMs = 60000; // a refused, removed clone leaves this long to restore its source

class Book
{
public:
	// Our clone `clone` replaces `source`, which sat in `slot` (it goes to the backpack now).
	void plan(std::uint16_t clone, std::uint16_t source, std::uint16_t slot, std::uint64_t now)
	{
		drop(clone);
		m_plans.push_back(Plan{clone, source, slot, now});
	}

	// The clone spawns in our inventory: the slot it takes, kNone for the engine's own default (no plan, or the slot was
	// taken meanwhile: a slot is never overwritten). Once.
	std::uint16_t spawn_slot(std::uint16_t clone, bool slot_free)
	{
		for (auto it = m_plans.begin(); it != m_plans.end(); ++it)
			if (it->clone == clone && !it->spawned && !it->refused && !it->removed)
			{
				const std::uint16_t slot = it->slot;
				it->spawned = true;
				if (it->confirmed)
					m_plans.erase(it);
				return slot_free ? slot : kNone;
			}
		return kNone;
	}

	// The slot planned for `clone` (kNone: none), without using the plan.
	std::uint16_t peek(std::uint16_t clone) const
	{
		for (const Plan& p : m_plans)
			if (p.clone == clone && !p.spawned && !p.refused && !p.removed)
				return p.slot;
		return kNone;
	}

	// The host accepted the clone. A delayed local spawn still needs its planned slot once.
	void confirmed(std::uint16_t clone)
	{
		for (auto it = m_plans.begin(); it != m_plans.end(); ++it)
			if (it->clone == clone && !it->refused)
			{
				it->confirmed = true;
				if (it->spawned || it->removed)
					m_plans.erase(it);
				return;
			}
	}

	// The host refused the clone. Its source waits for the clone's actual removal before restore.
	bool refused(std::uint16_t clone, std::uint64_t now)
	{
		for (Plan& p : m_plans)
			if (p.clone == clone && !p.refused && !p.confirmed)
			{
				p.refused = true;
				p.at = now;
				return true;
			}
		return false;
	}

	// Called only once neither a server entity nor a game object holds the clone's id.
	void removed(std::uint16_t clone, std::uint64_t now)
	{
		for (auto it = m_plans.begin(); it != m_plans.end(); ++it)
			if (it->clone == clone && !it->removed)
			{
				it->removed = true;
				it->at = now;
				if (it->confirmed)
					m_plans.erase(it);
				return;
			}
	}

	// A refused clone is gone here: its source may take a free slot. The caller retries while the slot is occupied.
	bool restore(std::uint16_t& source, std::uint16_t& slot) const
	{
		for (const Plan& p : m_plans)
			if (p.refused && p.removed)
			{
				source = p.source;
				slot = p.slot;
				return true;
			}
		return false;
	}

	void restored(std::uint16_t source)
	{
		for (auto it = m_plans.begin(); it != m_plans.end(); ++it)
			if (it->refused && it->removed && it->source == source)
			{
				m_plans.erase(it);
				return;
			}
	}

	void expire(std::uint64_t now)
	{
		for (auto it = m_plans.begin(); it != m_plans.end();)
		{
			const bool finished = it->refused && it->removed;
			if (finished && now >= it->at && now - it->at > kKeepMs)
				it = m_plans.erase(it);
			else
				++it;
		}
	}

	void clear() { m_plans.clear(); }
	std::size_t size() const { return m_plans.size(); }

private:
	struct Plan
	{
		std::uint16_t clone, source, slot;
		std::uint64_t at;
		bool spawned = false;
		bool confirmed = false;
		bool refused = false;
		bool removed = false;
	};

	void drop(std::uint16_t clone)
	{
		for (auto it = m_plans.begin(); it != m_plans.end();)
			it = it->clone == clone ? m_plans.erase(it) : it + 1;
	}

	std::vector<Plan> m_plans;
};
} // namespace cloneslot
} // namespace coop
