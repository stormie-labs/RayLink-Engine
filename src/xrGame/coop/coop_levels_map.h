// Anomaly Coop - which seats of the world server's table a LevelMap lists (docs/plans/2026-11-12-split-levels-native.md §1.3).
// A seat that plays no character (Seat::plays false: the world server's own seat on a dedicated host) is no player: it gets no player
// row and its level no server row of its own, so a client's level_of_player(0) is "no level" there (no "Fake start" in the HUD).
// Pure C++17, header-only, no engine dependencies: used by xrGame (coop/coop_levels.cpp, host_send_map) and by the unit-test exe.
#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

#include "../../xrCoop/coop_levels_rules.h"

namespace coop
{
namespace levels
{
namespace map_rows
{
// A seat is a player row of the map when it plays.
inline bool listed(const Seat& s) { return s.plays; }

// The seats the map lists, in the table's order.
inline std::vector<Seat> listed_seats(const std::vector<Seat>& seats)
{
	std::vector<Seat> out;
	out.reserve(seats.size());
	for (const Seat& s : seats)
		if (listed(s))
			out.push_back(s);
	return out;
}

// The levels the map has a server row for: the levels of the listed seats, each once, in the order of their first seat; no level left out.
inline std::vector<uint16_t> levels_of(const std::vector<Seat>& seats)
{
	std::vector<uint16_t> out;
	for (const Seat& s : seats)
		if (listed(s) && s.level != kNoLevel && std::find(out.begin(), out.end(), s.level) == out.end())
			out.push_back(s.level);
	return out;
}
} // namespace map_rows
} // namespace levels
} // namespace coop
