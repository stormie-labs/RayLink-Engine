// Anomaly Coop — ALife hooks: who keeps the world online, and who decides switching.
// Called from ALife code, which in the MT build runs on the game thread between frames
// (never concurrently with the network pump in FrameMove).
#pragma once

class CALifeSimulator;

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
} // namespace coop
