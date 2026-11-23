#include "stdafx.h"
#include "coop_mapserver.h"
#include "../coop_join.h"
#include "../coop_levels.h"
#include "../coop_manager.h"
#include "xrCoop/coop_mode.h"
#include "xrCoop/srv/coop_session_srv.h"
#include "xrCoop/srv/coop_srv_departure_rules.h" // VEH-DISC

// F3a split levels: the trunk between the host's process and a map server (plan §1.3, T9). A client talks to the host's process alone,
// on the one port, and his network code does not know a map server exists:
//   client -> the host's process   a message of scope Map from a player on a level of a map server goes on in a Msg::Tunnel, with his
//                                  slot, from the session's in-filter, before any handler here runs (host_incoming);
//   the host's process -> Mk       the Tunnel comes out on the session of its level as a message of that virtual peer;
//   Mk -> the host's process       what the level's host code sends a virtual peer goes up in a Tunnel (srv::SessionSrv) and the
//                                  host's process sends it to that player on the lane the Tunnel names.
// The virtual peers are the world server's LevelMap: every player on the level has one. The players' ids are the world server's.
// VEH-DISC (review rv_F5-VEH-7, blocker 2): a virtual peer leaves for a quit and for a move to another map alike; when it leaves, the
// map server keeps what the LevelMap says of him (xrCoop/srv/coop_srv_departure_rules.h) and answers it through its mode
// (MapServerMode::departure): the Core releases his seat in a car as Disconnect or LeftLevel.

namespace coop
{
namespace mapserver
{
namespace wire = levels::wire;

bool host_incoming(PeerId from, Msg type, Lane lane, const u8* packet, size_t size)
{
	// R32 W5 (design R32-final.md §1 W5.1): a map server is a client of the world for every object it does not write. What it asks
	// (StateDumpRequest, EntityResync: mapserver::map_server_sends) the world server answers itself; it is never tunnelled back to a
	// map server as a player's map message would be. (The session gives a trunk's messages no in-filter today: this keeps it so.)
	if (is_bridge(from))
		return true;
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
	// Build 57, S3 (protocol 33; TZ 6.3, the psy phantoms of a bloodsucker were unseen): this goes only to players of that level, so
	// a spawn its map server has online is online at the authority of the level they stand on. Their machine knows it by the byte
	// (xrCoop/coop_levels_wire.h mark_spawn_of_your_level) and brings a free entity with no game vertex online at once, as a co-op
	// client does with its host's spawn. The world server sends them no copy of its own of such a spawn (host_spawn_from_leader).
	static u32 s_marked = 0;
	if (wire::mark_spawn_of_your_level(m.packet) && (++s_marked <= 3 || s_marked % 500 == 0))
		::Msg("[coop] trunk: a spawn of the map server of level %u goes to its players as online at their level (%u so far)", u32(level),
		    s_marked);
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
	srv::SessionSrv(mgr().session()).virtual_receive(m.peer, tunnel_lane(m.flags), m.packet.data(), m.packet.size());
}

namespace
{
// VEH-DISC: how the players of this level left it, as the LevelMap said when their virtual peers left (a map server's own)
srv::departure::Book& departures()
{
	static srv::departure::Book book;
	return book;
}
} // namespace

Departure departure_of(PeerId peer, u16 character)
{
	return departures().of(peer, character);
}

void server_on_map(const wire::LevelMapMsg& m)
{
	if (!is_map_server())
		return;
	const std::map<PeerId, Visitor> before = trunk_peers().on(); // VEH-DISC: the characters of those who leave now
	const VirtualPeers::Change c = trunk_peers().update(key().level, m.players);
	Session& world = mgr().world_session();
	const srv::SessionSrv level(mgr().session()); // the host of its level (its peers are virtual: srv::SessionSrv)
	for (const PeerId peer : c.left)
	{
		// VEH-DISC: why he leaves, by this table, before his virtual peer goes (the Core asks in its next frame): no row of his - he
		// quit the world; a row on another level - he went there; a row on this one - another character of his took his place
		const Departure why = srv::departure::classify(key().level, peer, m.players);
		const auto was = before.find(peer);
		departures().left(peer, was == before.end() ? wire::kNoObject : was->second.proxy, why);
		level.virtual_leave(peer, std::string("he is not on this level any more: ") + srv::departure::text(why));
	}
	for (const Visitor& v : c.came)
	{
		departures().came(v.peer); // VEH-DISC: he is a player of this level again, what his last departure said is over
		const PeerInfo* info = world.peer(v.peer);
		level.virtual_join(v.peer, info ? info->nick : std::string("?"));
		server_visitor_came(v);
	}
	for (const Visitor& v : c.in_world)
		server_visitor_in_world(v.peer);
	LevelSide* side = mode().level();
	if (!side)
		return;
	for (const wire::LevelMapMsg::Player& p : m.players)
		if (p.level == key().level)
			side->level_balance_heard(p.peer, p.money);
}
} // namespace mapserver
} // namespace coop
