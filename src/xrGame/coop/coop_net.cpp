#include "stdafx.h"
#include "coop_net.h"
#include "coop_sync.h"

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
} // namespace

void host_broadcast(Msg type, const Writer& payload, Lane lane, PeerId except)
{
	if (!mgr().is_host())
		return;
	Session& s = mgr().session();
	for (const PeerInfo& p : s.peers())
	{
		if (p.id == except || p.state != PeerState::Joined)
			continue;
		if (sync::host_peer_joining(p.id))
			queue(p.id, type, payload, lane);
		else
			s.send(p.id, lane, type, payload);
	}
}

void host_send(PeerId to, Msg type, const Writer& payload, Lane lane)
{
	if (!mgr().is_host())
		return;
	if (sync::host_peer_joining(to))
		queue(to, type, payload, lane);
	else
		mgr().session().send(to, lane, type, payload);
}

void client_send(Msg type, const Writer& payload, Lane lane)
{
	if (!mgr().is_client())
		return;
	mgr().session().send(kHostPeer, lane, type, payload);
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

void reset()
{
	g_queues.clear();
	g_overflow.clear();
	g_queued_total = 0;
}
} // namespace net
} // namespace coop
