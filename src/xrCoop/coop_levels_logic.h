// Anomaly Coop — split levels: what the binders of a level's static objects keep when its map server closes (docs/SPLIT_LEVELS.md §7a).
//
// The stock keeps the logic of an object in the client_data of its server entity: CLevel::ClientSave -> CGameObject::net_Save (the object's
// save(), then its binder's) -> xrServer::Process_save -> CSE_Abstract::load. The next CGameObject::net_Spawn gives it to the binder's load
// (xr_logic.load_obj), and the object goes on from its section instead of starting at the first one. A map server never saves: what its
// restrictors did was gone with its process, and the next one ran every one-shot of the level again (agru_poltergeists_spawns.ltx,
// on_game_timer = 20 | sr_idle@line_1 %=create_squad(...)%).
//
// So a map server does what a save does, for what no transfer carries, and only for what changed:
//   - watched(): the objects the stock itself keeps through a level change (keep_saved_data_anyway: se_restrictor, se_physic, se_lamp, se_car,
//     se_heli, se_trader, the anomalous zones). What is not on that list loses client_data in the stock too (CSE_ALifeDynamicObject::on_register).
//     Creatures that are transferred carry their own logic, items are the world server's;
//   - Book: the first look at an object comes before the first update of its binder, before its logic can have done anything. What it would
//     save then is the baseline. A look under a clock that stands still (kStillClock: xr_logic writes activation_time - time_global())
//     differs from the last one told: it changed, the real bytes go up in LevelEntity Logic (LevelLogicBody);
//   - Round: every kEveryMs, a few objects a frame, so that a save of the host holds what the level has done by then. The last look is at
//     the end of the hand-over, right before Stopped: nothing is sent later, and no binder runs after it;
//   - host_verdict(): the world server puts the bytes in the entity's client_data, as Process_save would. They reach a new map server in the
//     world save it starts from, and the host when he comes: both have loaded = true, no first section runs again.
//
// A crash of a map server loses what the last round did not catch. Pure C++17, header-only, no engine dependencies: used by xrGame
// (coop/coop_levels_xfer.cpp, coop/coop_levels_alife.cpp) and by the unit-test exe.
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <vector>

namespace coop
{
namespace levels
{
namespace logic
{
constexpr uint32_t kStillClock = 0x40000000u; // time_global() during a look: a time no session reaches, time_global() - start stays positive
constexpr uint32_t kEveryMs = 15000; // a round over the objects a map server watches, this often
constexpr uint32_t kPerFrame = 32; // ... this many in one frame

// What the stock says of an object: its entity's keep_saved_data_anyway, is it an inventory item, is it a creature that changes hands.
struct Kind
{
	bool keeps = false;
	bool item = false;
	bool transfers = false;
};

inline bool stays_with_level(const Kind& k) { return k.keeps && !k.item && !k.transfers; }

// A map server looks after it: `binder` it has a script binder, `saves` net_SaveRelevant.
inline bool watched(const Kind& k, bool binder, bool saves) { return binder && saves && stays_with_level(k); }

// FNV-1a. `skip`: bytes at the start that say nothing of its logic: a physics object begins with its shell's sleep (CPhysicsShellHolder::save).
inline uint64_t digest(const uint8_t* data, size_t size, size_t skip = 0)
{
	uint64_t h = 0xcbf29ce484222325ull;
	for (size_t i = skip; i < size; ++i)
	{
		h ^= data[i];
		h *= 0x100000001b3ull;
	}
	return h;
}

// What CGameObject::net_Save leaves in a packet: u16 size, then what the entity keeps as client_data. False: it is not exactly that.
inline bool chunk_body(const uint8_t* packet, size_t count, const uint8_t*& body, size_t& size)
{
	if (count < sizeof(uint16_t))
		return false;
	const size_t said = size_t(packet[0]) | (size_t(packet[1]) << 8);
	if (said + sizeof(uint16_t) != count)
		return false;
	body = packet + sizeof(uint16_t);
	size = said;
	return true;
}

// What a map server knows of the objects of its level, by id.
class Book
{
public:
	bool met(uint16_t id) const { return m_rows.find(id) != m_rows.end(); }

	// The first look, before its first logic update. Not `kept`: it is not looked at again.
	void meet(uint16_t id, bool kept, uint64_t digest_now) { m_rows[id] = Row{kept, digest_now}; }

	// What it would save now differs from what the world server has. An object never met: its binder has not run here.
	bool changed(uint16_t id, uint64_t digest_now) const
	{
		const auto it = m_rows.find(id);
		return it != m_rows.end() && it->second.kept && it->second.digest != digest_now;
	}

	void told(uint16_t id, uint64_t digest_now)
	{
		const auto it = m_rows.find(id);
		if (it != m_rows.end())
			it->second.digest = digest_now;
	}

	std::vector<uint16_t> ids() const
	{
		std::vector<uint16_t> out;
		for (const auto& it : m_rows)
			if (it.second.kept)
				out.push_back(it.first);
		return out;
	}

	size_t watched() const
	{
		size_t n = 0;
		for (const auto& it : m_rows)
			n += it.second.kept ? 1u : 0u;
		return n;
	}

	void clear() { m_rows.clear(); }

private:
	struct Row
	{
		bool kept = false;
		uint64_t digest = 0;
	};
	std::map<uint16_t, Row> m_rows;
};

// The look a few objects a frame.
class Round
{
public:
	bool due(uint64_t now) const { return now >= m_next; }

	// What to look at this frame of `ids`: the next at most `per_frame`; a new round `every_ms` after this one is over.
	std::vector<uint16_t> take(const std::vector<uint16_t>& ids, uint64_t now, uint32_t per_frame = kPerFrame, uint32_t every_ms = kEveryMs)
	{
		std::vector<uint16_t> out;
		if (!due(now))
			return out;
		while (m_at < ids.size() && out.size() < per_frame)
			out.push_back(ids[m_at++]);
		if (m_at >= ids.size())
		{
			m_at = 0;
			m_next = now + every_ms;
		}
		return out;
	}

private:
	size_t m_at = 0;
	uint64_t m_next = 0;
};

// The world server's view of the object a map server speaks of.
struct Entity
{
	bool exists = false;
	bool online = false; // here: it stands on the level of this process, whose own saves keep it
	Kind kind;
	uint16_t level = 0xFFFF; // the level of its game vertex, 0xFFFF for none
};

enum class Verdict : uint8_t
{
	Take,
	NoObject,
	Online,
	NotKept, // the next load of this world would clear it
	OtherLevel,
};

inline Verdict host_verdict(const Entity& e, uint16_t level)
{
	if (!e.exists)
		return Verdict::NoObject;
	if (e.online)
		return Verdict::Online;
	if (!stays_with_level(e.kind))
		return Verdict::NotKept;
	if (level == 0xFFFF || e.level != level)
		return Verdict::OtherLevel;
	return Verdict::Take;
}

// The save of a player's move, kept for the map server that starts for him, is older than the last word of the map server that left that
// level: he came back while it was going. `told`: the level said anything at all.
inline bool save_is_stale(uint64_t saved_at, bool told, uint64_t told_at) { return told && told_at >= saved_at; }
} // namespace logic
} // namespace levels
} // namespace coop
