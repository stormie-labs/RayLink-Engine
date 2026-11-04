#include "stdafx.h"
#include "coop_net.h"
#include "coop_sync.h"
#include "coop_levels.h"
#include "coop_mapserver.h"

namespace coop
{
namespace net
{
namespace
{
struct Queued
{
	Msg type;
	Lane lane;
	std::vector<u8> payload;
};
xr_map<PeerId, xr_vector<Queued>> g_queues;
u64 g_queued_total = 0;
constexpr size_t kMaxQueued = 50000; // a peer that never finishes loading must not eat the host's memory
xr_set<PeerId> g_overflow;

void queue(PeerId to, Msg type, const Writer& payload, Lane lane)
{
	if (lane == Lane::Unreliable)
		return; // snapshots are worthless later; the next one will do
	Queued q;
	q.type = type;
	q.lane = lane;
	q.payload.assign(payload.data().begin(), payload.data().end());
	xr_vector<Queued>& queue_of_peer = g_queues[to];
	if (queue_of_peer.size() >= kMaxQueued)
	{
		g_overflow.insert(to);
		return;
	}
	queue_of_peer.push_back(std::move(q));
	++g_queued_total;
}

// F3a: the map servers are clients of the world: what reaches every level reaches them, a bridge that loads its world gets it after.
void to_bridges(Msg type, const Writer& payload, Lane lane, PeerId except)
{
	if (!mapserver::bridge_takes(type))
		return;
	for (const PeerId bridge : mapserver::host_bridges())
	{
		if (bridge == except)
			continue;
		if (sync::host_peer_joining(bridge))
			queue(bridge, type, payload, lane);
		else
			mgr().session().send(bridge, lane, type, payload);
	}
}
} // namespace

void host_broadcast(Msg type, const Writer& payload, Lane lane, PeerId except)
{
	if (!is_map_authority())
		return;
	Session& s = mgr().session();
	for (const PeerInfo& p : s.peers())
	{
		if (p.id == except || p.state != PeerState::Joined)
			continue;
		if (!levels::passes_to(type, p.id))
			continue; // F3a multilevel: the stream of the host's level to a player on another one
		if (sync::host_peer_joining(p.id))
			queue(p.id, type, payload, lane);
		else
			s.send(p.id, lane, type, payload);
	}
	// F3a T4: the host player too, over the LocalLink (its gate decides; R1 5: no players() vector per message)
	if (except != kHostPeer && s.local())
		s.send(kHostPeer, lane, type, payload);
	if (is_world_server())
		to_bridges(type, payload, lane, except);
}

void host_stream(Msg type, const Writer& payload, PeerId except)
{
	if (!is_map_authority())
		return;
	Session& s = mgr().session();
	if (!levels::split())
	{
		s.broadcast(Lane::Unreliable, type, payload, except);
		return;
	}
	for (const PeerInfo& p : s.peers())
		if (p.id != except && p.state == PeerState::Joined && levels::passes_to(type, p.id))
			s.send(p.id, Lane::Unreliable, type, payload);
}

void host_send(PeerId to, Msg type, const Writer& payload, Lane lane)
{
	if (!is_map_authority())
		return;
	if (sync::host_peer_joining(to))
		queue(to, type, payload, lane);
	else
		mgr().session().send(to, lane, type, payload);
}

void client_send(Msg type, const Writer& payload, Lane lane)
{
	if (is_map_server() && !mapserver::map_server_sends(type))
		return; // its actor is a ghost: nobody's body, nobody's hands
	if (mgr().is_client())
	{
		mgr().session().send(kHostPeer, lane, type, payload);
		return;
	}
	// F3a T4: the host player -> his own world server, over the LocalLink (only the messages moved onto it pass)
	if (mgr().is_host() && mgr().session().local())
		mgr().local_link().push_to_server(type, lane, payload);
}

void host_flush(PeerId to)
{
	auto it = g_queues.find(to);
	if (it == g_queues.end())
		return;
	Session& s = mgr().session();
	for (const Queued& q : it->second)
	{
		Writer w(q.payload.size());
		if (!q.payload.empty())
			w.bytes(q.payload.data(), q.payload.size());
		s.send(to, q.lane, q.type, w);
	}
	::Msg("[coop] flushed %u queued world change(s) to #%u", (u32)it->second.size(), to);
	g_queues.erase(it);
}

void host_drop(PeerId to)
{
	g_queues.erase(to);
	g_overflow.erase(to);
}

bool host_queue_overflow(PeerId to) { return g_overflow.count(to) != 0; }

void print_status()
{
	::Msg("[coop]   join queues: %llu changes queued in all, at most %u per peer", g_queued_total, u32(kMaxQueued));
	for (const auto& it : g_queues)
		::Msg("[coop]     #%u loads: %u queued%s", it.first, u32(it.second.size()), g_overflow.count(it.first) ? ", overflowed" : "");
}

void reset()
{
	g_queues.clear();
	g_overflow.clear();
	g_queued_total = 0;
}
} // namespace net
} // namespace coop
