// Anomaly Coop — who is listed as a player and who carries the host's badge (docs/plans/2026-11-12-split-levels-native.md §1.3).
// The one place that decides it: the Lua lists (coop.players, coop.session_info), the HUD star, "/players" and every count of
// players go through these. A world server's own seat plays no character (PeerInfo::plays false, Welcome flag HostSeatPlays
// off): it is in no list and counts nowhere. The badge is the operator's (PeerInfo::host_rights, PeerList flag Operator), and
// the host's seat's when that seat plays (an ordinary host).
// Pure C++17, header-only, no engine dependencies: used by xrGame (coop/coop_script.cpp) and by the unit-test exe.
#pragma once

#include <cstdint>
#include <vector>

#include "coop_session.h"

namespace coop
{
namespace roster
{
// A seat that plays is listed; the world server's own is not.
inline bool listed(const PeerInfo& p) { return p.plays; }

// The host's badge: the operator, or seat 0 of a host whose seat plays. `host_plays`: a client reads it from peer 0's
// PeerInfo::plays (Welcome flags), a host from its SessionConfig::host_plays.
inline bool host_badge(const PeerInfo& p, bool host_plays)
{
	if (!listed(p))
		return false;
	return p.host_rights || (host_plays && p.id == kHostPeer);
}

// The players of a list (Session::players() or peers()): the seats that play.
inline uint32_t player_count(const std::vector<PeerInfo>& seats)
{
	uint32_t n = 0;
	for (const PeerInfo& p : seats)
		if (listed(p))
			++n;
	return n;
}

// The seats of a list that are listed, in its order.
inline std::vector<PeerInfo> listed_only(const std::vector<PeerInfo>& seats)
{
	std::vector<PeerInfo> out;
	out.reserve(seats.size());
	for (const PeerInfo& p : seats)
		if (listed(p))
			out.push_back(p);
	return out;
}

// Whether the host's seat plays, as a client or a host knows it: the PeerInfo of seat 0 in `seats` (a client's peers() has it
// from Welcome), else `fallback` (a host's peers() has no seat 0: it passes SessionConfig::host_plays).
inline bool host_plays_in(const std::vector<PeerInfo>& seats, bool fallback)
{
	for (const PeerInfo& p : seats)
		if (p.id == kHostPeer)
			return p.plays;
	return fallback;
}
} // namespace roster
} // namespace coop
