// Anomaly Coop - one id, one object (live RL-UGYE-3WGE 2026-11-09, a client at X-16: FATAL "Beer from the creator of
// 'AMMO_S'" in xrServer::Process_update, object 52480, 3 bytes read where the game object wrote 1).
// A client's level writes the update of every local game object under its id (CObjectList::net_Export) and its own
// server reads it with the entity that has that id now (xrServer::Process_update). A game object that outlived its
// entity (a stale spawn, an orphan) writes in the format of ITS class: once the id belongs to another entity (the host
// reuses ids at once, our server takes them as the host sends them), the read runs off the chunk and the stock code is
// FATAL. Stock SPAWN_ANTIFREEZE makes such objects: an M_SPAWN waits for its models while the events run, the
// GE_DESTROY of that id finds no object and is dropped, and the postponed spawn checks only that SOME ALife object with
// that id is online when it finally runs (CLevel::ProcessSpawnEvents) - the new one.
// On a mirror (a co-op client's world, join::world_is_mirror) the client level and its server are kept one object per id:
//   spawn  - a game object is made only for the entity the server has under that id now, of that section, and only
//            when no live game object holds the id (CLevel::cl_Process_Spawn);
//   export - only the object the level has registered under the id, of the section of the entity there, writes its
//            update (CLevel::ClientSend);
//   update - a length that does not match is skipped with the entity as it was, once logged per id, and the host is
//            asked for the object (xrServer::Process_update): the last line, never FATAL on a mirror.
// Single player and the host keep the stock behaviour (mirror = false everywhere).
// Pure C++17, header-only, no engine dependencies: used by the engine (coop_events.cpp, the hooks) and the unit-test exe.
#pragma once

#include <bitset>
#include <cstdint>

namespace coop
{
namespace identity
{
// What the client level does with an M_SPAWN it is about to turn into a game object.
enum class Spawn : uint8_t
{
	Make,      // the object of the entity the server has under this id now
	Stale,     // the server has no entity under this id any more: the spawn outlived its destroy
	OtherKind, // the id belongs to an entity of another section now: the spawn outlived its destroy and the id's reuse
	Duplicate  // a live game object of this section holds the id already: a second one would never get its destroy
};

// mirror: join::world_is_mirror(); entity_here: the level's server has an entity under the spawn's id;
// same_section: its section is the spawn's; live_same_kind: the level has a game object of the spawn's section under the
// id, not being destroyed. A live object of ANOTHER section under the id does not hold the new one back: it is a stale
// object that outlived its entity; the new one registers under the id and the stale one falls silent (export_verdict).
inline Spawn spawn_verdict(bool mirror, bool entity_here, bool same_section, bool live_same_kind)
{
	if (!mirror)
		return Spawn::Make;
	if (!entity_here)
		return Spawn::Stale;
	if (!same_section)
		return Spawn::OtherKind;
	return live_same_kind ? Spawn::Duplicate : Spawn::Make;
}

// What a game object does in the client's update export.
enum class Export : uint8_t
{
	Send,          // its update goes to the server
	NotRegistered, // the level has another object under its id: a stale copy, the registered one speaks for the id
	OtherKind      // the server's entity under its id has another section: it would write in another format
};

// registered: Objects.net_Find(id) is this very object; entity_here: the server has an entity under its id;
// same_section: that entity's section is this object's. An object with no entity is sent as the stock code does (the
// server skips the chunk of an unknown id).
inline Export export_verdict(bool mirror, bool registered, bool entity_here, bool same_section)
{
	if (!mirror)
		return Export::Send;
	if (!registered)
		return Export::NotRegistered;
	return entity_here && !same_section ? Export::OtherKind : Export::Send;
}

// What xrServer::Process_update does when an entity read `read` bytes of a chunk of `size`.
enum class Update : uint8_t
{
	Ok,    // the lengths match
	Fatal, // the stock "Beer from the creator" (single player, the host)
	Skip   // a mirror: the entity gets back its state from before the read, the chunk is skipped, the host is asked
};

inline Update update_verdict(bool mirror, uint32_t read, uint32_t size)
{
	if (read == size)
		return Update::Ok;
	return mirror ? Update::Skip : Update::Fatal;
}

// One log line per id (and per kind of trouble, by the caller's instance) until reset(): a stale object would repeat its
// trouble every frame.
class OncePerId
{
public:
	bool first(uint16_t id)
	{
		if (m_seen.test(id))
			return false;
		m_seen.set(id);
		return true;
	}
	void reset() { m_seen.reset(); }

private:
	std::bitset<65536> m_seen;
};
} // namespace identity
} // namespace coop
