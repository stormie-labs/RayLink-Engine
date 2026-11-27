// Anomaly Coop: a spawn whose id a game object on its way out still
// holds waits for it.
// The crash: a co-op client's own server sent its level GE_DESTROY and then M_SPAWN of one stalker id (its entity went
// offline and online again on the client's server: an EntitySwitch pair applied in one pump). GE_DESTROY only marks the
// game object (CObject::setDestroy); it stays under its id until the destroy queue (CObjectList::ProcessDestroyQueue, at
// the end of the frame or before the next one's render). The spawn ran in the same frame, the identity guard
// (coop_object_identity.h) saw no live object of the id and said Make, and the stock check in CGameObject::net_Spawn
// ("Object with ID already exists", any object under the id) failed. The stock soft fail (CLevel::g_sv_Spawn) then runs the
// whole net_Destroy chain of a half-made CAI_Stalker with id 65535 and deletes it: the client died there with no FATAL.
// Rule: on a mirror a spawn reaches the stock net_Spawn only when no game object holds its id, and (for a held item) when
// its holder's object is not on its way out (else the item's GE_OWNERSHIP_TAKE goes to the dying holder). Otherwise the
// whole M_SPAWN goes back into the level's event queue and runs again at a next frame, kMaxWaitFrames frames at most: then
// it is not made, logged (the stock net_Spawn would refuse it too, after half-making a game object).
// The decision is the same in both modes (a co-op client's and a server player's level are both mirrors); single player and
// the host keep the stock code (mirror = false). Pure C++17, header-only, no engine types: coop_events.cpp
// (client_spawn_gate) and tests/test_spawn_wait_rules.cpp.
#pragma once

#include <cstddef>
#include <cstdint>
#include <unordered_map>

#include "coop_object_identity.h"

namespace coop
{
namespace spawn_wait
{
// Frames a spawn may wait for its id. The destroy queue empties once a frame, so one is the rule; the rest is for a stale
// object of another kind, which goes only if something destroys it.
constexpr uint32_t kMaxWaitFrames = 30;

// The level's game object under an id when a spawn of that id comes.
enum class Holder : uint8_t
{
	None,     // no game object under the id
	Leaving,  // a game object marked for destruction (GE_DESTROY came), still registered under the id
	SameKind, // a game object of the spawn's section that stays: the object of this entity already
	OtherKind // a game object of another section that stays: a stale copy that outlived its entity
};

// present: Objects.net_Find(id) is not null; destroying: its getDestroy(); same_section: its section is the spawn's.
inline Holder holder_of(bool present, bool destroying, bool same_section)
{
	if (!present)
		return Holder::None;
	if (destroying)
		return Holder::Leaving;
	return same_section ? Holder::SameKind : Holder::OtherKind;
}

enum class Verdict : uint8_t
{
	Make,   // the stock path makes the game object now
	Wait,   // the whole M_SPAWN back into the level's event queue: decided again at a next frame
	GiveUp, // waited kMaxWaitFrames frames: not made, logged
	Refuse  // the identity guard refuses it (coop_object_identity.h: Stale, OtherKind, Duplicate): not made, as before
};

// mirror: join::world_is_mirror(); guard: identity::spawn_verdict for this spawn, with live_same_kind = (self ==
// SameKind); self: what holds the spawn's id; holder_leaving: the spawn has a holder (ID_Parent) whose game object is
// marked for destruction; frames_waited: the frames this id waited already (WaitBook::waited).
inline Verdict decide(bool mirror, identity::Spawn guard, Holder self, bool holder_leaving, uint32_t frames_waited)
{
	if (!mirror)
		return Verdict::Make; // the stock code, whatever the facts
	if (guard != identity::Spawn::Make)
		return Verdict::Refuse; // its entity is gone or of another kind, or its object is here already: waiting changes nothing
	// The stock net_Spawn refuses a spawn while ANY object holds the id: the leaving one goes at the frame's end, a stale
	// one of another kind maybe later
	const bool held = self == Holder::Leaving || self == Holder::OtherKind || self == Holder::SameKind;
	if (!held && !holder_leaving)
		return Verdict::Make;
	return frames_waited < kMaxWaitFrames ? Verdict::Wait : Verdict::GiveUp;
}

// A wait whose spawn never came back (the stock pipeline dropped the requeued M_SPAWN: its ALife object went offline in
// between) is forgotten after this many frames: the next spawn of the id starts from zero.
constexpr uint32_t kForgetAfterFrames = 120;

// Frames each id has waited, counted once per frame however many spawns of the id wait in it (an online-offline-online
// flip in one pump queues two). Ids are the level's (local numbering); frames are Device.dwFrame.
class WaitBook
{
public:
	uint32_t waited(uint16_t id, uint32_t frame) const
	{
		const auto it = m_ids.find(id);
		if (it == m_ids.end() || frame - it->second.last_frame > kForgetAfterFrames)
			return 0u;
		return it->second.frames;
	}
	// `id` waits in `frame`
	void wait(uint16_t id, uint32_t frame)
	{
		Entry& e = m_ids[id];
		if (e.frames != 0 && frame - e.last_frame > kForgetAfterFrames)
			e.frames = 0; // an old wait that never came back
		if (e.frames != 0 && e.last_frame == frame)
			return;
		++e.frames;
		e.last_frame = frame;
	}
	// `id` was made, refused or given up: a later wait counts from zero
	void done(uint16_t id) { m_ids.erase(id); }
	void reset() { m_ids.clear(); }
	// ids with a wait on record (forgotten ones included until their id spawns again or the session resets)
	size_t size() const { return m_ids.size(); }

private:
	struct Entry
	{
		uint32_t frames = 0;
		uint32_t last_frame = 0;
	};
	std::unordered_map<uint16_t, Entry> m_ids;
};
} // namespace spawn_wait
} // namespace coop
