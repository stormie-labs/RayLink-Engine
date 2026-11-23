// Ядро: без проверок режима (docs/MODES.md)
// Anomaly Coop - weapon state riders follow the concrete weapon (F4 G3-1c, the engine part of group G3: findings m01:2,
// the 3DSS camo of a weapon, and m27:5, the Lua jam type of arti_jamming; temp/b58/out/G3-1b.md, RV-G3-1.md).
//
// gamedata/scripts/coop_mods_item_state.script keeps both as item riders: the holder's machine writes them and sends them in
// the economy's se channel, whose receiver on the deciding process checks the sender against the item's holder. Two engine
// facts broke that:
//  1. A client's ReqDrop / ReqTransfer is applied when it arrives (coop_requests.cpp host_move), its script events wait in
//     the Lua inbox (coop_script.cpp deliver_inbox), and a client sends the requests of a frame before its script events
//     (sync::frame: requests::frame, then script::frame). The deciding Lua saw the next holder and dropped the former
//     holder's last change (a camo, a jam, a jam cleared), whatever the order of the two on the wire.
//     Carry: the client appends the weapon's riders (coop_mods_item_state.carry) to the request, and the process that
//     applies the move hands them to coop_mods_item_state.carried before apply_move, under the move's own holder check.
//     That process writes them as its own facts through the economy: the co-op host relays them to the players; a map
//     server's go up to the world server, which keeps them and relays them as a map's facts.
//  2. The engine names an ordinary item after its section and id, and the host reuses a freed id at once: a rider that
//     waited in the inbox while its weapon was released and replaced reached the replacement under the same name.
//     Ticks: one counter orders every event that reaches this process's Lua inbox and every release of an entity here
//     (coop_entities.cpp host_on_entity_released). Lua drops a rider whose weapon was released after the rider came
//     (coop.event_tick, coop.released_after), and forgets what it held for an instance the engine released when its own
//     unregister callback did not run.
// Both modes run this code unchanged: the process that applies a move (the co-op host, the map server of the player's
// level) and the one that keeps the world (the co-op host, the world server) are the ones the requests and the economy
// reach already.
// Pure C++17, header-only, no engine dependencies: used by the engine and by the unit-test exe.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace coop
{
namespace riders
{
// Bytes of riders one move carries: the camo and the jam of one weapon, each at most 2048 packed bytes in the adapter's
// own bound (coop_mods_item_state.script), with their batch keys and instances.
constexpr size_t kMaxCarry = 4096;
constexpr uint64_t kMaxTick = (uint64_t(1) << 53); // exact in a Lua number

// A client's move of an item of its character: whether the request takes the item's riders. A weapon (the riders are a
// weapon's state) leaving our character: a drop, a gift to a player, into a box, to an NPC. Not a move inside our
// character, not a take, not what this machine applied already (a thrown missile, a launched rocket: kMoveLocal).
inline bool carries(bool weapon, bool from_ours, bool to_ours, bool applied_here)
{
	return weapon && from_ours && !to_ours && !applied_here;
}

// The text coop_mods_item_state.carry gave a client: sent with the request, or not.
enum class Text : uint8_t
{
	None, // nothing to carry
	Fits, // goes with the request
	TooLong // over kMaxCarry: the request goes without it (the economy's channel still carries the riders)
};

inline Text text_of(size_t size)
{
	if (size == 0)
		return Text::None;
	return size <= kMaxCarry ? Text::Fits : Text::TooLong;
}

// What the process that applies a move does with the riders the request carried.
enum class Carried : uint8_t
{
	None, // the request carried none
	Take, // to coop_mods_item_state.carried, before apply_move
	TooLong, // over kMaxCarry: never reaches Lua
	NotHolder // the requester's character does not hold the item: never reaches Lua
};

// size: the bytes the request carried; holds: the requester's character holds the item now (host_move's own check of the
// move's source, before apply_move).
inline Carried judge(size_t size, bool holds)
{
	if (size == 0)
		return Carried::None;
	if (size > kMaxCarry)
		return Carried::TooLong;
	return holds ? Carried::Take : Carried::NotHolder;
}

// Arguments of coop.released_after from Lua: an entity id and a tick, whole numbers in range.
inline bool valid_id(double id)
{
	return std::isfinite(id) && id >= 0.0 && id <= 65535.0 && std::floor(id) == id;
}

inline bool valid_tick(double tick)
{
	return std::isfinite(tick) && tick >= 0.0 && tick <= double(kMaxTick) && std::floor(tick) == tick;
}

// The last release of each entity id on this process, as a tick of the counter that also numbers the arrivals of the Lua
// inbox. One slot per id (65536 x 8 bytes, made at the first release): no eviction, so an old release is never forgotten
// while a rider that came before it still waits.
class ReleaseBook
{
public:
	// A client's local rebuild can release a copy of a weapon that still exists on the host.
	void keep_next(uint16_t id)
	{
		if (m_keep.size() < kMaxKeep && std::find(m_keep.begin(), m_keep.end(), id) == m_keep.end())
			m_keep.push_back(id);
	}

	bool unkeep(uint16_t id)
	{
		const auto it = std::find(m_keep.begin(), m_keep.end(), id);
		if (it == m_keep.end())
			return false;
		m_keep.erase(it);
		return true;
	}

	void clear_kept() { m_keep.clear(); }

	// The entity under `id` was released at `tick`. An older tick never replaces a newer one.
	void note(uint16_t id, uint64_t tick)
	{
		if (unkeep(id))
			return;
		if (m_last.empty())
			m_last.assign(kIds, 0);
		if (tick > m_last[id])
			m_last[id] = tick;
		++m_notes;
	}

	// The entity under `id` was released here after `tick` (the arrival of a rider, or the moment Lua began to hold one
	// for the id's instance). An id never released: false.
	bool released_after(uint16_t id, uint64_t tick) const { return !m_last.empty() && m_last[id] > tick; }

	// The tick of the id's last release, 0 when none.
	uint64_t last(uint16_t id) const { return m_last.empty() ? 0 : m_last[id]; }

	uint64_t notes() const { return m_notes; }

	void clear()
	{
		m_last.clear();
		m_last.shrink_to_fit();
		clear_kept();
		m_notes = 0;
	}

private:
	static constexpr size_t kIds = 65536;
	static constexpr size_t kMaxKeep = 64;
	std::vector<uint64_t> m_last;
	std::vector<uint16_t> m_keep;
	uint64_t m_notes = 0;
};
} // namespace riders
} // namespace coop
