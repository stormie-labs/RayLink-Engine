#include "stdafx.h"
#include "coop_mapserver.h"
#include "coop_join.h"
#include "coop_levels.h"
#include "coop_manager.h"
#include "coop_money.h"

// F3a split levels: the trunk between the host's process and a map server (plan §1.3, T9). A client talks to the host's process alone,
// on the one port, and his network code does not know a map server exists:
//   client -> the host's process   a message of scope Map from a player on a level of a map server goes on in a Msg::Tunnel, with his
//                                  slot, from the session's in-filter, before any handler here runs (host_incoming);
//   the host's process -> Mk       the Tunnel comes out on the session of its level as a message of that virtual peer;
//   Mk -> the host's process       what the level's host code sends a virtual peer goes up in a Tunnel (Session::host_virtual) and the
//                                  host's process sends it to that player on the lane the Tunnel names.
// The virtual peers are the world server's LevelMap: every player on the level has one. The players' ids are the world server's.

namespace coop
{
namespace mapserver
{
namespace wire = levels::wire;

bool host_incoming(PeerId from, Msg type, Lane lane, const u8* packet, size_t size)
{
	const bool split = levels::split();
	if (!split)
		return true;
	const u16 level = levels::level_of_player(from);
	const PeerId bridge = levels::on_host_level(from) ? kInvalidPeer : bridge_of(level);
	const Incoming go = route_incoming(type, split, levels::on_host_level(from), bridge != kInvalidPeer);
	if (go.tunnel)
	{
		wire::TunnelMsg m;
		m.peer = from;
		m.flags = tunnel_lane_flags(lane);
		m.packet.assign(packet, packet + size);
		Writer w(size + 8);
		if (m.write(w))
			mgr().session().send(bridge, lane, Msg::Tunnel, w);
	}
	return go.here;
}

void trunk_host_on_tunnel(PeerId from, Reader& r)
{
	wire::TunnelMsg m;
	if (!is_bridge(from) || !mgr().is_host() || !m.read(r))
		return;
	const u16 level = level_of_bridge(from);
	if (level == wire::kNoLevel)
		return;
	Writer w(m.packet.size());
	if (m.packet.size() > 2)
		w.bytes(m.packet.data() + 2, m.packet.size() - 2);
	const Lane lane = tunnel_lane(m.flags);
	// a player who has left its level meanwhile hears nothing of it any more
	if (m.flags & wire::TunnelMsg::AllOnMap)
	{
		for (const u32 peer : levels::host_players_on(level))
			mgr().session().send(peer, lane, m.type(), w);
		return;
	}
	if (levels::level_of_player(m.peer) == level)
		mgr().session().send(m.peer, lane, m.type(), w);
}

void trunk_server_on_tunnel(PeerId from, Reader& r)
{
	wire::TunnelMsg m;
	if (from != kHostPeer || !is_map_server() || !m.read(r))
		return;
	mgr().session().virtual_receive(m.peer, tunnel_lane(m.flags), m.packet.data(), m.packet.size());
}

void server_on_map(const wire::LevelMapMsg& m)
{
	if (!is_map_server())
		return;
	const VirtualPeers::Change c = trunk_peers().update(key().level, m.players);
	Session& world = mgr().world_session();
	for (const PeerId peer : c.left)
		mgr().session().virtual_leave(peer, "he is not on this level any more");
	for (const Visitor& v : c.came)
	{
		const PeerInfo* info = world.peer(v.peer);
		mgr().session().virtual_join(v.peer, info ? info->nick : std::string("?"));
		server_visitor_came(v);
	}
	for (const Visitor& v : c.in_world)
		server_visitor_in_world(v.peer);
	for (const wire::LevelMapMsg::Player& p : m.players)
		if (p.level == key().level)
			money::server_balance(p.peer, p.money);
}
} // namespace mapserver
} // namespace coop
