// Anomaly Coop - personal map zones: a script_zone a client creates for himself.
// Personal Adjustable Waypoint (tasks_placeable_waypoints.script, a route "owner" script: coop_route_mods.ltx) puts a
// script_zone under every pin, waypoint and right click on the PDA map: alife_create("script_zone", pos, lvid, gvid) with no
// parent, and a map spot of the player's own on it (CMapLocation::Update needs a real server object under the spot's id).
// On a client that create went to the host's request path, which only knows items in the own inventory: nil, no right-click
// menu on the map, no pin. The host's zone would be everybody's too (its entity reaches all clients), and the spot is private.
// So the client makes the zone here and only here: under an id of the lease the host lent him (coop_id_lease.h ClientPool),
// registered in his ALife copy, never asked of the host, never in a roster or a save (flCanSave off). Releasing it takes it
// out of his copy, and the id goes back to his pool (reserved again in his generator) - not to the host: PAW makes many
// zones, and every id given back counts against the player's lease (kMaxGivenBack). The pool keeps kKeepForItems ids for the
// items he creates; with fewer to spare a zone is not made and the call answers nil, as before.
// Pure C++17, header-only, no engine dependencies: used by the engine and by the unit-test exe.
#pragma once

#include "coop_id_lease.h"

#include <cstddef>
#include <cstdint>
#include <set>
#include <string_view>
#include <utility>

namespace coop
{
namespace zones
{
constexpr uint16_t kNoId = lease::kNoId;
// Ids of the lease a zone never takes: the player's own items (alife():create into his actor) need them at once.
constexpr size_t kKeepForItems = 8;

// The section of a zone that stays on the machine of whoever made it.
inline bool local_section(std::string_view section) { return section == "script_zone"; }

// A create (alife():create(section, pos, lvid, gvid), the id of its parent kNoId when it has none) a client makes a personal
// zone of: no parent, a zone section. Into the actor (or any holder) it is an item's create, whatever its section.
inline bool wants(std::string_view section, uint16_t parent) { return parent == kNoId && local_section(section); }

// Where the id of a released zone went.
enum class Fate : uint8_t
{
	NotOurs, // not the id of a zone made here (or already released): nothing done
	Pool, // reserved again in the generator, back in the pool: the next zone or item takes it
	Lost, // the generator would not reserve it (something here holds it): neither pooled nor given back
};

class Book
{
public:
	struct Stats
	{
		uint64_t issued = 0, retired = 0, refused = 0, lost = 0;
	};

	// The id for a new zone out of `pool`, kNoId when the pool has none to spare (fewer than kKeepForItems would be left).
	uint16_t issue(lease::ClientPool& pool)
	{
		if (pool.size() <= kKeepForItems)
		{
			++m_stats.refused;
			return kNoId;
		}
		const uint16_t id = pool.take();
		if (id == kNoId)
		{
			++m_stats.refused;
			return kNoId;
		}
		m_ids.insert(id);
		++m_stats.issued;
		return id;
	}

	// An issued id the create could not use (no zone was made): out of the book. The caller returns the id itself.
	bool cancel(uint16_t id) { return m_ids.erase(id) != 0; }

	bool has(uint16_t id) const { return m_ids.count(id) != 0; }
	size_t size() const { return m_ids.size(); }
	const Stats& stats() const { return m_stats; }
	void clear() { m_ids.clear(); }

	// The zone under `id` is released (its object is out of the ALife copy and the generator has the id free):
	// reserve(id) takes it in the generator again (true: done). Then it is the pool's, last in line.
	template <class Reserve>
	Fate retire(lease::ClientPool& pool, uint16_t id, Reserve&& reserve)
	{
		if (m_ids.erase(id) == 0)
			return Fate::NotOurs;
		++m_stats.retired;
		if (!reserve(id))
		{
			++m_stats.lost;
			return Fate::Lost;
		}
		pool.put_back(id);
		return Fate::Pool;
	}

private:
	std::set<uint16_t> m_ids;
	Stats m_stats;
};
} // namespace zones
} // namespace coop
