#include "stdafx.h"
#include "coop_mapserver.h"
#include "../coop_join.h"
#include "../coop_levels.h"
#include "../coop_manager.h"
#include "../coop_requests.h" // the world takes a weapon's stamp from its map server's spawn
#include "xrCoop/coop_mode.h"
#include "xrCoop/srv/coop_session_srv.h"
#include "xrCoop/srv/coop_srv_departure_rules.h"

// The trunk between the host's process and a map server. A client talks to the host's process alone,
// on the one port, and his network code does not know a map server exists:
//   client -> the host's process   a message of scope Map from a player on a level of a map server goes on in a Msg::Tunnel, with his
//                                  slot, from the session's in-filter, before any handler here runs (host_incoming);
//   the host's process -> Mk       the Tunnel comes out on the session of its level as a message of that virtual peer;
//   Mk -> the host's process       what the level's host code sends a virtual peer goes up in a Tunnel (srv::SessionSrv) and the
//                                  host's process sends it to that player on the lane the Tunnel names.
// The virtual peers are the world server's LevelMap: every player on the level has one. The players' ids are the world server's.
// A virtual peer leaves for a quit and for a move to another map alike; when it leaves, the
// map server keeps what the LevelMap says of him (xrCoop/srv/coop_srv_departure_rules.h) and answers it through its mode
// (MapServerMode::departure): the Core releases his seat in a car as Disconnect or LeftLevel.

namespace coop
{
namespace mapserver
{
namespace wire = levels::wire;

bool host_incoming(PeerId from, Msg type, Lane lane, const u8* packet, size_t size)
{
	// A map server is a client of the world for every object it does not write. What it asks
	// (StateDumpRequest, EntityResync: mapserver::map_server_sends) the world server answers itself; it is never tunnelled back to a
	// map server as a player's map message would be. (The session gives a trunk's messages no in-filter today: this keeps it so.)
	if (is_bridge(from))
		return true;
	const bool split = levels::split();
	if (!split)
		return true;
	const u16 level = levels::level_of_player(from);
	const bool home = levels::on_host_level(from);
	const PeerId bridge = home ? kInvalidPeer : bridge_of(level);
	const Incoming go = route_incoming(type, split, home, bridge != kInvalidPeer);
	// One answer to a player's repair, his map server's while it is connected, else this world's (route_incoming).
	// Each of the two ways counts and logs on its own: its first three and every 500th. A player on no level (between two, or not
	// seated yet) is no player of a map server: no line for him ("level 65535").
	static u32 s_by_map = 0, s_by_world = 0;
	if (answered_by_his_map_server(type) && !home && level != levels::wire::kNoLevel)
	{
		u32& n = go.tunnel ? s_by_map : s_by_world;
		if (++n <= 3 || n % 500 == 0)
			::Msg("[coop] trunk: a repair asked by player %u on level %u is answered by %s alone (P99, %u so far)", u32(from), u32(level),
			    go.tunnel ? "the map server of his level" : "the world server (his map server is not connected)", n);
	}
	if (go.tunnel)
	{
		Writer w(size + 8);
		if (wire::TunnelMsg::write_raw(w, from, tunnel_lane_flags(lane), packet, size))
			mgr().session().send(bridge, lane, Msg::Tunnel, w);
	}
	return go.here;
}

namespace
{
// The world's copy of a weapon takes the stamp its map server's spawn carries. write_clone_initial
// stamps a magazined weapon with the instance its process gave it first (clone_export keeps it), and the players of the level take
// that stamp from the map server's spawn (client_on_spawn -> stage_clone_initial). A weapon this world never stamped (loaded from the
// save, never in a spawn of this process) had no stamp here, so its item riders (jam, camo) a player sent under the map server's stamp
// were refused here: the rider lane keeps a strict instance (coop_mods_item_state.script matches_instance, "weapon instance changed";
// coop_economy_mdata.script on_imd), and an unstamped copy's instance is the weapon's name (item_instance). Its
// copy takes the stamp alone (requests::adopt_spawn_stamp -> coop_economy_mdata.script clone_take_stamp; not clone_adopt,
// whose purge would drop the item's held changes here): while it has no stamp or the same one. A weapon this world stamped otherwise
// keeps the world's (refused, counted and logged).
void world_takes_spawn_stamp(u16 level, const std::vector<u8>& packet)
{
	u16 id = 0;
	const requests::SpawnStamp got = requests::adopt_spawn_stamp(packet.data(), packet.size(), id);
	if (got == requests::SpawnStamp::NoWeapon)
		return;
	// each outcome counts and logs on its own: its first three and every 500th
	static u32 s_taken = 0, s_refused = 0, s_absent = 0, s_malformed = 0;
	switch (got)
	{
	case requests::SpawnStamp::Taken:
		if (++s_taken <= 3 || s_taken % 500 == 0)
			::Msg("[coop] trunk: weapon %u holds the stamp the map server of level %u gave it, here too (P99 D1, %u so far)", u32(id),
			    u32(level), s_taken);
		break;
	case requests::SpawnStamp::Refused:
		if (++s_refused <= 3 || s_refused % 500 == 0)
			::Msg("! [coop] trunk: weapon %u keeps this world's stamp, the map server of level %u gave it another, or not taken (another "
			      "item stands under its id here, its clone is in flight, its state is not read): its riders under that one are refused "
			      "here (P99 D1, %u so far)",
			    u32(id), u32(level), s_refused);
		break;
	case requests::SpawnStamp::NoEntity:
		if (++s_absent <= 3 || s_absent % 500 == 0)
			::Msg("[coop] trunk: weapon %u of the map server of level %u is not in this world yet (or its clone is in flight here): its "
			      "stamp is not taken (P99 D1, %u so far)",
			    u32(id), u32(level), s_absent);
		break;
	default:
		if (++s_malformed <= 3 || s_malformed % 500 == 0)
			::Msg("! [coop] trunk: a spawn of the map server of level %u is not read as write_spawn writes it: its weapon stamp is not "
			      "taken (P99 D1, %u so far)",
			    u32(level), s_malformed);
		break;
	}
}
} // namespace

void trunk_host_on_tunnel(PeerId from, Reader& r)
{
	wire::TunnelMsg m;
	if (!is_bridge(from) || !mgr().is_host() || !m.read(r))
		return;
	const u16 level = level_of_bridge(from);
	if (level == wire::kNoLevel)
		return;
	// Before the spawn goes on, so that no rider under its stamp can reach this world before the stamp does. Whether or not a
	// player of the level gets it now: the stamp is the level's, the world's own later spawns of the weapon carry it (clone_export)
	if (m.type() == Msg::EntitySpawn)
		world_takes_spawn_stamp(level, m.packet);
	// This goes only to players of that level, so
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
// How the players of this level left it, as the LevelMap said when their virtual peers left (a map server's own)
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
	const std::map<PeerId, Visitor> before = trunk_peers().on(); // the characters of those who leave now
	const VirtualPeers::Change c = trunk_peers().update(key().level, m.players);
	Session& world = mgr().world_session();
	const srv::SessionSrv level(mgr().session()); // the host of its level (its peers are virtual: srv::SessionSrv)
	for (const PeerId peer : c.left)
	{
		// Why he leaves, by this table, before his virtual peer goes (the Core asks in its next frame): no row of his - he
		// quit the world; a row on another level - he went there; a row on this one - another character of his took his place
		const Departure why = srv::departure::classify(key().level, peer, m.players);
		const auto was = before.find(peer);
		departures().left(peer, was == before.end() ? wire::kNoObject : was->second.proxy, why);
		level.virtual_leave(peer, std::string("he is not on this level any more: ") + srv::departure::text(why));
	}
	for (const Visitor& v : c.came)
	{
		departures().came(v.peer); // he is a player of this level again, what his last departure said is over
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
