// Anomaly Coop - where a task's target is, for a client's frozen ALife (tasks audit C1): when the host sends a new place.
// Pure C++17, header-only, no engine dependencies: used by xrGame/coop/coop_story.cpp and by the unit-test exe
// (tests/test_task_target.cpp).
//
// A client's ALife does not run. The spot of a task target that is offline there, its edge pointer and the coop compass
// read the client's copy of the server entity, and nothing moves that copy while the entity stays offline, so a roaming
// target would stay where it stood when the client joined. The host sends the place of every running task's target:
// the first time, then when it moved far enough or to another game vertex, at most once per kMinGapMs.
#pragma once

#include <cstdint>

namespace coop
{
namespace task_target
{
constexpr float kMoveMetres = 5.f;
constexpr uint64_t kMinGapMs = 2000;

struct Place
{
	float x = 0, y = 0, z = 0;
	uint16_t game_vertex = 0xffff;
};

inline bool moved(const Place& a, const Place& b)
{
	if (a.game_vertex != b.game_vertex)
		return true;
	const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
	return dx * dx + dy * dy + dz * dz >= kMoveMetres * kMoveMetres;
}

// sent_at == 0: never sent.
inline bool due(const Place& sent, uint64_t sent_at, const Place& now, uint64_t now_ms)
{
	if (sent_at == 0)
		return true;
	return now_ms - sent_at >= kMinGapMs && moved(sent, now);
}

// --- client: what it does with a place the host sent
// What this machine has of the target.
struct Here
{
	bool known = false; // a server entity with that id
	bool online = false; // its m_bOnline
	bool held = false; // in a holder (ID_Parent): online it goes with its holder
	bool level_object = false; // a game object with that id on this level (Level().Objects)
	bool map_authority = false; // this process computes its level (a map server)
};

enum class Take : uint8_t
{
	Unknown, // no copy of the target here: nothing to place
	Follow, // the spot follows the game object (or the holder's)
	Own, // this machine's own update moves it
	Offline, // offline here: the copy goes to the host's place, between graph vertices too
	Online, // online here without a game object: the copy goes to the host's place; online it is in no graph registry
};

// Owner 2026-11-07 (GAMMA "General cleanup", the markers differ): a squad is online without a game object of its own
// (CSE_ALifeOnlineOfflineGroup::switch_online brings its members online, not itself). Its spot and the compass read the
// squad's entity, and on a client nothing moves an online squad's entity: the commander's binder that copies his place
// into it (sim_squad_scripted:update) is suspended there. A dominance task keeps its target squad online
// (squad.force_online), so the client's spot stood where the squad came online while the host's walked with it.
inline Take take(const Here& h)
{
	if (!h.known)
		return Take::Unknown;
	if (!h.online)
		return Take::Offline;
	if (h.level_object || h.held)
		return Take::Follow;
	return h.map_authority ? Take::Own : Take::Online;
}

// A target id the host sent and this machine does not have is logged once per kUnknownLogMs per id. last_at == 0: never.
constexpr uint64_t kUnknownLogMs = 60000;
inline bool log_due(uint64_t last_at, uint64_t now_ms) { return last_at == 0 || now_ms - last_at >= kUnknownLogMs; }
} // namespace task_target
} // namespace coop
