// Anomaly Coop — ALife hooks: who keeps the world online, and who decides switching.
// Called from ALife code, which in the MT build runs on the game thread between frames
// (never concurrently with the network pump in FrameMove).
#pragma once

class CALifeSimulator;
class CSE_ALifeDynamicObject;

namespace coop
{
// Mutable ALife (ai().alife() is const).
CALifeSimulator& alife_rw();

// Squared-free distance from `pos` to the nearest player: the local actor and, on a host, every
// connected player's character. Replaces "distance to the actor" in online/offline switching
// so the world lives around each player (R-4.2).
float observer_distance(const Fvector& pos);

enum class SwitchVote : s8
{
	Default = -1, // not a coop client: normal ALife rules
	Offline = 0,
	Online = 1,
};
// Client: online/offline is decided by the host, never by local distance.
SwitchVote client_switch_vote(u16 id);
// Vote for one object (CSE_ALifeDynamicObject::try_switch_online/offline; game thread): a client asks the host
// (client_switch_vote); anywhere else a player's character (coop_player_avatar) is offline unless its player is
// in the world right now (W9): on the host only after he finished loading, without a session never (the host
// playing alone, or a client after the session ended — the host's avatar included). Default otherwise.
SwitchVote switch_vote(const CSE_ALifeDynamicObject* e);
} // namespace coop
