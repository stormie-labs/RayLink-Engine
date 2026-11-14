// Anomaly Coop — split levels, the dynamic mode of the map servers (build 50; docs/SPLIT_LEVELS.md "Жизнь Mk"). The owner's idea: as
// many maps as players + 1; a map nobody stands on makes room when somebody wants another map; an empty map does not stay for ever.
// The lead's contract, the pure part of it:
//   - map_server_cap   cap = min(players now + 1, coop_max_map_servers (the PC's ceiling), kMaxBridges);
//   - decide_move      at the cap a move is not refused while a map server stands empty: the one empty the longest is stopped (a
//                      Stopping server holds no place, the new one starts at once). RefuseCap only when every map server has a player;
//   - linger_of        the map emptied last stays coop_mapserver_linger (600 s by default), every other empty map at most 60 s, and none
//                      at all when the PC is short of memory (memory_low).
// "Empty" is the table's word, not the life's: a player on his way to a level (host_player_moves sat him there) is a player of it, so
// the server he loads into is never stopped. A server that loads (Starting) is never stopped to make room, nor one kept through the
// host's move. Pure C++17, header-only: used by xrGame (coop/coop_levels.cpp, coop/coop_mapserver.cpp) and by the unit-test exe.
#pragma once

#include "coop_levels_rules.h"
#include "coop_mapserver_rules.h" // kMaxBridges: one slice of ids per map server

#include <algorithm>
#include <cstdint>
#include <vector>

namespace coop
{
namespace levels
{
// coop_max_map_servers: what this PC can carry. Not kMaxBridges by default: the world server keeps a slice of object ids out of its own
// generator for every server the setting allows (coop_levels_keeper.h leader_slots_kept: 4095 ids each from 0x9000), and GAMMA's world
// starts with some 27 300 objects. Seven slices would leave the world 9 500 free ids; six leave 13 600 and still give a group of five
// its "players + 1". A host with more players raises the setting to 7.
constexpr uint32_t kDefaultMaxMapServers = 6;
constexpr uint32_t kColdLingerMs = 60000; // every empty map but the one emptied last
constexpr uint64_t kLowMemoryBytes = uint64_t(2) * 1024 * 1024 * 1024; // less physical memory free: no empty map lingers
constexpr uint64_t kMemoryBackBytes = kLowMemoryBytes + uint64_t(512) * 1024 * 1024; // and back over this: they linger again

// How many map servers may run now. `players_now`: Table::players() (the mover of a join that is not seated yet counted too).
inline uint32_t map_server_cap(uint32_t players_now, uint32_t machine_max)
{
	uint32_t cap = players_now + 1;
	cap = std::min(cap, machine_max);
	return std::min(cap, mapserver::kMaxBridges);
}

// One map server as these rules see it.
struct MapSlot
{
	uint16_t level = kNoLevel;
	bool running = false; // it holds a place (ServerLife::running)
	bool empty = false; // loaded, nobody on it nor on his way to it, not kept: it may be stopped, its pause runs
	uint64_t empty_since = 0; // when it became empty (ServerLife::idle_since)
};

// `players` = Table::map_players(level) now: the table knows a mover before the life does (it is told at the next tick).
inline MapSlot slot_of(uint16_t level, const ServerLife& life, uint32_t players, bool kept)
{
	MapSlot s;
	s.level = level;
	s.running = life.running();
	s.empty = life.idle() && players == 0 && !kept;
	s.empty_since = s.empty ? life.idle_since() : 0;
	return s;
}

namespace detail
{
// a was emptied before b (the same moment: the lower level counts as the older, so the choice is the same every time)
inline bool emptied_before(const MapSlot& a, const MapSlot& b)
{
	return a.empty_since != b.empty_since ? a.empty_since < b.empty_since : a.level < b.level;
}
} // namespace detail

// Can one more map server start under `cap`? True with `stop` empty when there is a free place; true with `stop` = the levels whose
// servers must be stopped first (the longest empty first) when there is not; false with `stop` empty when there are not enough empty
// servers (or cap is 0: no map server may run at all).
inline bool room_for_one(const std::vector<MapSlot>& slots, uint32_t cap, std::vector<uint16_t>& stop)
{
	stop.clear();
	if (cap == 0)
		return false;
	uint32_t running = 0;
	std::vector<MapSlot> empties;
	for (const MapSlot& s : slots)
	{
		running += s.running ? 1u : 0u;
		if (s.running && s.empty)
			empties.push_back(s);
	}
	if (running < cap)
		return true;
	const size_t need = size_t(running - cap) + 1;
	if (empties.size() < need)
		return false;
	std::sort(empties.begin(), empties.end(), &detail::emptied_before);
	for (size_t i = 0; i < need; ++i)
		stop.push_back(empties[i].level);
	return true;
}

// The verdict on a move in the dynamic mode: decide_move (coop_levels_rules.h) with `running` counted from `slots`, and at the cap the
// servers to stop in `stop` instead of a refusal when room_for_one finds them. `stop` is empty for every other verdict.
inline MoveVerdict decide_move(Gate effective, bool needs_new_server, const std::vector<MapSlot>& slots, uint32_t cap,
    std::vector<uint16_t>& stop)
{
	stop.clear();
	uint32_t running = 0;
	for (const MapSlot& s : slots)
		running += s.running ? 1u : 0u;
	const MoveVerdict verdict = decide_move(effective, needs_new_server, running, cap);
	if (verdict != MoveVerdict::RefuseCap)
		return verdict;
	return room_for_one(slots, cap, stop) ? MoveVerdict::Go : MoveVerdict::RefuseCap;
}

// The empty map of `level` is the one emptied last: the warm one.
inline bool warm(const std::vector<MapSlot>& slots, uint16_t level)
{
	const MapSlot* me = nullptr;
	for (const MapSlot& s : slots)
		if (s.level == level)
			me = &s;
	if (!me || !me->empty)
		return false;
	for (const MapSlot& s : slots)
		if (s.empty && s.level != level && detail::emptied_before(*me, s))
			return false;
	return true;
}

// The pause of the server of `level` (ServerLife::linger_ms): `linger_ms` (coop_mapserver_linger) for the warm one, at most
// kColdLingerMs for every other empty one, 0 for all when the PC is short of memory. A server that is not empty: `linger_ms` (its
// pause does not run).
inline uint32_t linger_of(const std::vector<MapSlot>& slots, uint16_t level, uint32_t linger_ms, bool low_memory)
{
	if (low_memory)
		return 0;
	bool empty = false;
	for (const MapSlot& s : slots)
		empty = empty || (s.level == level && s.empty);
	if (!empty || warm(slots, level))
		return linger_ms;
	return std::min(linger_ms, kColdLingerMs);
}

// The PC is short of memory: under kLowMemoryBytes free; once short, until kMemoryBackBytes are free again (no flapping at the border).
inline bool memory_low(bool was_low, uint64_t available)
{
	return available < (was_low ? kMemoryBackBytes : kLowMemoryBytes);
}
} // namespace levels
} // namespace coop
